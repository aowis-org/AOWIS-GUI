#include "projects/hydraulic_draft_event_adapter.h"
#include "projects/hydraulic_draft_entity_registry.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/serialization/model_codec.h>
#include <aowis/model/serialization/model_validation.h>

#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <utility>

namespace
{
bool fail(QString *error, const QString &description)
{
    if (error)
        *error = description;
    return false;
}

template<typename T>
bool applyToCollection(QList<T> &collection,
                       const aowis::model::ModelEntityEdit &edit,
                       bool &membership_changed,
                       QString *error)
{
    qsizetype found = -1;
    for (qsizetype index = 0; index < collection.size(); ++index)
    {
        if (collection.at(index).uuid != edit.entityId)
            continue;
        if (found >= 0)
            return fail(error, QStringLiteral("Duplicate model entity UUID"));
        found = index;
    }
    if (edit.before.has_value() != (found >= 0))
        return fail(error, QStringLiteral("Event's before-image does not match the staged model"));
    if (found >= 0 &&
        aowis::model::ModelCodec<T>::encode(collection.at(found)) != edit.before.value())
        return fail(error, QStringLiteral("Stale or out-of-order event before-image"));

    membership_changed = false;
    if (!edit.after.has_value())
    {
        if (found < 0)
            return fail(error, QStringLiteral("Cannot delete a missing hydraulic entity"));
        collection.removeAt(found);
        membership_changed = true;
        return true;
    }
    QString codec_error;
    const std::optional<T> after = aowis::model::ModelCodec<T>::decode(
        edit.after.value(), &codec_error);
    if (!after.has_value() || after->uuid != edit.entityId ||
        aowis::model::ModelCodec<T>::encode(after.value()) != edit.after.value())
        return fail(error, QStringLiteral("Invalid or noncanonical hydraulic after-image: ") + codec_error);
    if (found < 0)
    {
        collection.append(after.value());
        membership_changed = true;
    }
    else
        collection[found] = after.value();
    return true;
}

bool applyEdit(NetworkHydraulic &network, const aowis::model::ModelEntityEdit &edit,
               quint16 &kind, quint16 &collection_index,
               bool &membership_changed, QString *error)
{
#define APPLY(name, type, storage_kind, ordinal) \
    if (edit.entityType == QStringLiteral(#type)) \
    { \
        kind = static_cast<quint16>(storage_kind); \
        collection_index = static_cast<quint16>(ordinal); \
        return applyToCollection(network.name, edit, membership_changed, error); \
    }
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(APPLY)
#undef APPLY
    return fail(error, QStringLiteral("Unsupported hydraulic entity type: ") + edit.entityType);
}

bool collectMemberIds(const NetworkHydraulic &network, QSet<QUuid> &members,
                      QString *error)
{
#define COLLECT(name, type, storage_kind, ordinal) \
    for (const type &entity : network.name) \
    { \
        if (entity.uuid.isNull() || members.contains(entity.uuid)) \
            return fail(error, QStringLiteral("Null or duplicate hydraulic member UUID")); \
        members.insert(entity.uuid); \
    }
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(COLLECT)
#undef COLLECT
    return true;
}

} // namespace

bool HydraulicDraftEventAdapter::prepareWithMembership(
                                         const QByteArray &encoded_event,
                                         NetworkHydraulic &staged,
                                         QList<PersonalWorkingCopyChange> &records,
                                         QList<HydraulicDraftMembershipEdit> &membership,
                                         QString *error)
{
    if (error)
        error->clear();
    QString codec_error;
    const std::optional<aowis::model::ModelEditTransaction> event =
        aowis::model::ModelEditTransactionCodec::decode(encoded_event, &codec_error);
    if (!event.has_value())
        return fail(error, QStringLiteral("Invalid MODEL editing event: ") + codec_error);

    // Changes are staged independently and become visible only after every
    // before-image, membership delta and canonical reference has been checked.
    NetworkHydraulic next = staged;
    QList<PersonalWorkingCopyChange> proposed;
    QList<HydraulicDraftMembershipEdit> proposed_membership;
    QSet<QUuid> seen;
    QSet<QUuid> old_members;
    if (!collectMemberIds(next, old_members, error))
        return false;
    for (const aowis::model::ModelEntityEdit &edit : event->edits)
    {
        if (edit.entityId.isNull() || seen.contains(edit.entityId))
            return fail(error, QStringLiteral("Duplicate or null event entity UUID"));
        seen.insert(edit.entityId);
        quint16 kind = 0;
        quint16 collection_index = 0;
        bool membership_changed = false;
        if (!applyEdit(next, edit, kind, collection_index, membership_changed, error))
            return false;
        PersonalWorkingCopyChange record;
        record.uuid = edit.entityId.toString(QUuid::WithoutBraces);
        record.deleted = !edit.after.has_value();
        record.kind = kind;
        if (edit.after.has_value())
            record.payload = *edit.after;
        proposed.append(std::move(record));
        if (membership_changed)
        {
            HydraulicDraftMembershipEdit intent;
            intent.network_id = next.uuid;
            intent.entity_id = edit.entityId;
            intent.kind = kind;
            intent.collection_index = collection_index;
            intent.inserted = edit.after.has_value();
            proposed_membership.append(intent);
        }
    }
    QSet<QUuid> new_members;
    if (!collectMemberIds(next, new_members, error))
        return false;
    for (const HydraulicDraftMembershipEdit &intent : proposed_membership)
    {
        if (intent.network_id.isNull() || intent.entity_id == intent.network_id ||
            (intent.inserted ? old_members.contains(intent.entity_id) ||
                               !new_members.contains(intent.entity_id)
                             : !old_members.contains(intent.entity_id) ||
                               new_members.contains(intent.entity_id)))
            return fail(error, QStringLiteral("Inconsistent hydraulic membership intent"));
    }
    // A membership change must be fully described; no implicit owner edits.
    qsizetype expected_delta = 0;
    for (const QUuid &id : new_members)
        if (!old_members.contains(id)) ++expected_delta;
    for (const QUuid &id : old_members)
        if (!new_members.contains(id)) ++expected_delta;
    if (expected_delta != proposed_membership.size())
        return fail(error, QStringLiteral("Unrepresented hydraulic membership mutation"));

    const aowis::model::HydraulicDependencyValidation checked =
        aowis::model::validateHydraulicNetworkWithDependencies(next);
    if (!checked.report.valid())
        return fail(error, QStringLiteral("Invalid staged hydraulic model: ") + checked.report.summary());

    QHash<QUuid, QStringList> references;
    for (const aowis::model::HydraulicDependency &dependency : checked.dependencies)
    {
        if (new_members.contains(dependency.source_uuid) &&
            new_members.contains(dependency.target_uuid))
            references[dependency.source_uuid].append(
                dependency.target_uuid.toString(QUuid::WithoutBraces));
    }
    for (PersonalWorkingCopyChange &record : proposed)
    {
        if (record.deleted)
            continue;
        QStringList refs = references.value(QUuid(record.uuid));
        refs.sort(Qt::CaseSensitive);
        refs.removeDuplicates();
        record.references = refs;
    }
    staged = std::move(next);
    records = std::move(proposed);
    membership = std::move(proposed_membership);
    return true;
}

bool HydraulicDraftEventAdapter::prepare(const QByteArray &encoded_event,
                                         NetworkHydraulic &staged,
                                         QList<PersonalWorkingCopyChange> &records,
                                         QString *error)
{
    NetworkHydraulic next = staged;
    QList<PersonalWorkingCopyChange> proposed;
    QList<HydraulicDraftMembershipEdit> membership;
    if (!prepareWithMembership(encoded_event, next, proposed, membership, error))
        return false;
    if (!membership.isEmpty())
        return fail(error, QStringLiteral(
            "Hydraulic membership changes require server-side atomic membership materialization"));
    staged = std::move(next);
    records = std::move(proposed);
    return true;
}
