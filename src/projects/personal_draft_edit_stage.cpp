#include "projects/personal_draft_edit_stage.h"
#include "projects/hydraulic_draft_entity_registry.h"
#include "network/map_annotation_layer_id.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_distribution_root_edit.h>
#include <aowis/model/serialization/model_codec.h>
#include <aowis/model/serialization/model_validation.h>

#include <optional>

#include <utility>

namespace
{
bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

template<typename T>
bool currentImage(const QList<T> &collection, const QUuid &uuid,
                  std::optional<QByteArray> &image)
{
    int matches = 0;
    for (const T &entity : collection)
    {
        if (entity.uuid != uuid)
            continue;
        ++matches;
        image = aowis::model::ModelCodec<T>::encode(entity);
    }
    return matches <= 1;
}

// Annotation membership is materialized by DVMDB, never by the hydraulic
// registry. Apply ALL annotation subedits to a scratch candidate: a mixed
// operation must not advance either candidate if one before-image is stale.
bool prepareAnnotationEdits(const QList<aowis::model::ModelEntityEdit> &edits,
                            const QUuid &layer_id, QList<MapAnnotation> &annotations,
                            QString *error)
{
    if (edits.isEmpty() || layer_id.isNull())
        return fail(error, QStringLiteral("Invalid annotation transaction or owner layer"));
    QList<MapAnnotation> next = annotations;
    QSet<QUuid> touched;
    for (const aowis::model::ModelEntityEdit &edit : edits)
    {
        if (edit.entityType != QString::fromLatin1(aowis::model::mapAnnotationEditType) ||
            !edit.ownerLayerId || *edit.ownerLayerId != layer_id || edit.entityId.isNull() ||
            touched.contains(edit.entityId))
            return fail(error, QStringLiteral("Annotation owner or identity mismatch"));
        touched.insert(edit.entityId);
        qsizetype index = -1;
        for (qsizetype at = 0; at < next.size(); ++at)
            if (next.at(at).uuid == edit.entityId)
            {
                if (index >= 0)
                    return fail(error, QStringLiteral("Duplicate annotation UUID"));
                index = at;
            }
        const bool exists = index >= 0;
        if (edit.before.has_value() != exists ||
            (exists && aowis::model::ModelCodec<MapAnnotation>::encode(next.at(index)) !=
                           *edit.before))
            return fail(error, QStringLiteral("Stale annotation before-image"));
        if (!edit.after)
        {
            if (!exists)
                return fail(error, QStringLiteral("Cannot delete missing annotation"));
            next.removeAt(index);
            continue;
        }
        QString codec_error;
        const std::optional<MapAnnotation> decoded =
            aowis::model::ModelCodec<MapAnnotation>::decode(*edit.after, &codec_error);
        if (!decoded || decoded->uuid != edit.entityId ||
            aowis::model::ModelCodec<MapAnnotation>::encode(*decoded) != *edit.after ||
            !aowis::model::validateMapAnnotation(*decoded).valid())
            return fail(error, QStringLiteral("Invalid annotation after-image: ") + codec_error);
        if (exists)
            next[index] = *decoded;
        else
            next.append(*decoded);
    }
    annotations = std::move(next);
    return true;
}

// The layer and hydraulic network are one final candidate. A deletion without
// its detach subedit is invalid, including on crash recovery / replay.
bool annotationAnchorsValid(const NetworkHydraulicDistribution &network,
                            const QList<MapAnnotation> &annotations)
{
    for (const MapAnnotation &annotation : annotations)
    {
        if (!annotation.anchor)
            continue;
        const MapAnnotationAnchor &anchor = *annotation.anchor;
        int matches = 0;
        bool type_match = false;
#define MATCH_ANCHOR(collection, type_name) \
        for (const type_name &node : network.collection) \
            if (node.uuid == anchor.entity_uuid) \
            { \
                ++matches; \
                if (anchor.entity_type == QStringLiteral(#type_name)) type_match = true; \
            }
        MATCH_ANCHOR(nodes_junctions, HydraulicNodeJunction)
        MATCH_ANCHOR(nodes_reservoirs, HydraulicNodeReservoir)
        MATCH_ANCHOR(nodes_tanks, HydraulicNodeTank)
        MATCH_ANCHOR(demand_points, HydraulicDemandPoint)
#undef MATCH_ANCHOR
        if (matches != 1 || !type_match)
            return false;
    }
    return true;
}

bool prepareAnyEvent(const QByteArray &encoded_event,
                     NetworkHydraulicDistribution &network,
                     QList<MapAnnotation> &annotations, const QUuid &layer_id,
                     QList<PersonalWorkingCopyChange> &records,
                     QList<HydraulicDraftMembershipEdit> &membership,
                     QString *error)
{
    const std::optional<aowis::model::ModelEditTransaction> decoded =
        aowis::model::ModelEditTransactionCodec::decode(encoded_event, error);
    if (!decoded || decoded->edits.isEmpty())
        return fail(error, QStringLiteral("Invalid staged event"));

    aowis::model::ModelEditTransaction hydraulic = *decoded;
    hydraulic.edits.clear();
    QList<aowis::model::ModelEntityEdit> annotation_edits;
    QSet<QUuid> identities;
    for (const aowis::model::ModelEntityEdit &edit : decoded->edits)
    {
        if (edit.entityId.isNull() || identities.contains(edit.entityId))
            return fail(error, QStringLiteral("Duplicate or null mixed event identity"));
        identities.insert(edit.entityId);
        if (edit.entityType == QString::fromLatin1(aowis::model::mapAnnotationEditType))
            annotation_edits.append(edit);
        else
            hydraulic.edits.append(edit);
    }

    NetworkHydraulicDistribution next_network = network;
    QList<MapAnnotation> next_annotations = annotations;
    QList<PersonalWorkingCopyChange> next_records;
    QList<HydraulicDraftMembershipEdit> next_membership;
    if (!hydraulic.edits.isEmpty())
    {
        const QByteArray hydraulic_event =
            aowis::model::ModelEditTransactionCodec::encode(hydraulic, error);
        if (hydraulic_event.isEmpty() || !HydraulicDraftEventAdapter::prepareWithMembership(
                hydraulic_event, next_network, next_records, next_membership, error))
            return false;
    }
    if (!annotation_edits.isEmpty() &&
        !prepareAnnotationEdits(annotation_edits, layer_id, next_annotations, error))
        return false;
    if (!annotationAnchorsValid(next_network, next_annotations))
        return fail(error, QStringLiteral("Annotation anchor is missing from the final network candidate"));

    network = std::move(next_network);
    annotations = std::move(next_annotations);
    records = std::move(next_records);
    membership = std::move(next_membership);
    return true;
}

// True when the candidate already holds every after-image of the event (and
// lacks every entity it deletes). That is the state the event produces, so it
// reached DVMDB before its local acknowledgement was lost, also when a later
// Commit has since cleared the journal that would otherwise prove it.
bool eventAlreadyApplied(const QByteArray &encoded_event, const NetworkHydraulicDistribution &candidate,
                         const QList<MapAnnotation> &annotations)
{
    const std::optional<aowis::model::ModelEditTransaction> event =
        aowis::model::ModelEditTransactionCodec::decode(encoded_event);
    if (!event.has_value() || event->edits.isEmpty())
        return false;
    for (const aowis::model::ModelEntityEdit &edit : event->edits)
    {
        if (edit.entityType == QString::fromLatin1(aowis::model::mapAnnotationEditType))
        {
            std::optional<QByteArray> image;
            if (!currentImage(annotations, edit.entityId, image) || image != edit.after)
                return false;
            continue;
        }
        if (edit.entityType == QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType))
        {
            if (edit.entityId != candidate.uuid ||
                edit.after != aowis::model::hydraulicDistributionRootImage(candidate))
                return false;
            continue;
        }
        std::optional<QByteArray> image;
        bool known = false;
#define AOWIS_CURRENT_IMAGE(name, type, storage_kind, ordinal) \
        if (!known && edit.entityType == QStringLiteral(#type)) \
        { \
            known = true; \
            if (!currentImage(candidate.name, edit.entityId, image)) \
                return false; \
        }
        AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(AOWIS_CURRENT_IMAGE)
#undef AOWIS_CURRENT_IMAGE
        if (!known || image != edit.after)
            return false;
    }
    return true;
}

// DVMDB's /save-events bounds (dvmdb_http_routes.cpp): 1..128 typed records,
// at most 128 membership intents and an 8 MiB JSON body, in which records
// and the event travel as base64. One queued event is always sent alone, so
// an event beyond these bounds can never be saved; queueing it would block
// the draft on every reopen.
constexpr qsizetype max_save_records = 128;
constexpr qsizetype max_save_membership = 128;
constexpr qint64 max_save_body_bytes = 7 * 1024 * 1024; // 1 MiB margin for JSON framing

QString unsaveableEvent(const QByteArray &encoded_event,
                        const QList<PersonalWorkingCopyChange> &records,
                        const QList<HydraulicDraftMembershipEdit> &membership)
{
    qint64 body_bytes = (static_cast<qint64>(encoded_event.size()) + 2) / 3 * 4;
    for (const PersonalWorkingCopyChange &record : records)
        body_bytes += (static_cast<qint64>(record.payload.size()) + 2) / 3 * 4 +
            static_cast<qint64>(record.references.size()) * 40 + 160;
    if (records.size() <= max_save_records && membership.size() <= max_save_membership &&
        body_bytes <= max_save_body_bytes)
        return {};
    return QStringLiteral("This operation changes %1 entities (%2 created or removed, about "
                          "%3 KiB). A private draft accepts at most %4 entities, %5 created or "
                          "removed, per operation. It was not queued or saved; reopen the draft "
                          "and apply it in smaller steps.")
        .arg(records.size()).arg(membership.size()).arg(body_bytes / 1024)
        .arg(max_save_records).arg(max_save_membership);
}
}

bool PersonalDraftEditStage::open(
    const QString &queue_root, const QUuid &repository_id, const QUuid &draft_id,
    const NetworkHydraulicDistribution &verified_candidate,
    const QByteArray &last_journal_event, QString *error,
    const QList<MapAnnotation> &verified_annotations, const QUuid &annotation_layer_id)
{
    close();
    if (error)
        error->clear();
    if (verified_candidate.uuid.isNull())
        return fail(error, QStringLiteral("A verified hydraulic network UUID is required"));
    const QUuid resolved_layer = annotation_layer_id.isNull()
        ? distributionMapAnnotationLayerId(verified_candidate.uuid) : annotation_layer_id;
    if (resolved_layer != distributionMapAnnotationLayerId(verified_candidate.uuid))
        return fail(error, QStringLiteral("The verified annotation layer does not match this network"));
    QSet<QUuid> identities;
    for (const MapAnnotation &annotation : verified_annotations)
    {
        if (identities.contains(annotation.uuid) ||
            !aowis::model::validateMapAnnotation(annotation).valid())
            return fail(error, QStringLiteral("The verified annotation list is invalid"));
        identities.insert(annotation.uuid);
    }
    if (!this->queue_.open(queue_root, repository_id, draft_id, error))
        return false;

    this->persisted_ = verified_candidate;
    this->visible_ = verified_candidate;
    this->annotation_layer_id_ = resolved_layer;
    this->persisted_annotations_ = verified_annotations;
    this->visible_annotations_ = verified_annotations;

    // Exactly the FIRST local queued event may have reached the authoritative
    // journal before the process crashed. The caller supplies the journal tail
    // from the same validated draft/network, not from a client-side guess.
    if (!last_journal_event.isEmpty() && !this->queue_.pending().isEmpty() &&
        this->queue_.pending().first().encoded_transaction == last_journal_event)
    {
        const QUuid id = this->queue_.pending().first().transaction_id;
        if (!this->queue_.acknowledgePersisted(id, error))
        {
            close();
            return false;
        }
    }

    const QList<PersonalEditQueue::Entry> pending = this->queue_.pending();
    for (qsizetype index = 0; index < pending.size(); ++index)
    {
        const PersonalEditQueue::Entry &entry = pending.at(index);
        QList<PersonalWorkingCopyChange> records;
        QList<HydraulicDraftMembershipEdit> membership;
        QString replay_error;
        if (!prepareAnyEvent(entry.encoded_transaction, this->visible_,
                             this->visible_annotations_, this->annotation_layer_id_,
                             records, membership, &replay_error))
        {
            // Saves are serial, so only the oldest queued event can have
            // reached DVMDB unacknowledged. If the candidate already is its
            // result, retire it locally instead of blocking the draft forever.
            if (index == 0 && eventAlreadyApplied(entry.encoded_transaction, this->visible_,
                                                   this->visible_annotations_))
            {
                if (!this->queue_.acknowledgePersisted(entry.transaction_id, error))
                {
                    close();
                    return false;
                }
                continue;
            }
            // Never discard an unmatched pending event. Keep the queue files
            // on disk and reject the session until it is reconciled.
            close();
            return fail(error, QStringLiteral("Cannot replay queued transaction %1: %2")
                .arg(entry.transaction_id.toString(QUuid::WithoutBraces), replay_error));
        }
    }
    return true;
}

bool PersonalDraftEditStage::resetVerifiedNetwork(
    const NetworkHydraulicDistribution &network, QString *error,
    const QList<MapAnnotation> &annotations)
{
    if (error) error->clear();
    if (!isOpen() || this->blocked_ || !this->queue_.pending().isEmpty() ||
        network.uuid.isNull() || network.uuid != this->persisted_.uuid)
        return fail(error, QStringLiteral("History cannot replace a busy or mismatched private draft."));
    this->persisted_ = network;
    this->visible_ = network;
    this->persisted_annotations_ = annotations;
    this->visible_annotations_ = annotations;
    return true;
}

void PersonalDraftEditStage::close()
{
    this->queue_.close();
    this->persisted_ = NetworkHydraulicDistribution{};
    this->visible_ = NetworkHydraulicDistribution{};
    this->persisted_annotations_.clear();
    this->visible_annotations_.clear();
    this->annotation_layer_id_ = QUuid();
    this->blocked_ = false;
}

bool PersonalDraftEditStage::append(const QByteArray &encoded_event, QString *error)
{
    if (error)
        error->clear();
    if (!isOpen() || this->blocked_)
        return fail(error, QStringLiteral("Personal draft edit stage is not writable"));
    NetworkHydraulicDistribution next = this->visible_;
    QList<MapAnnotation> annotations = this->visible_annotations_;
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    if (!prepareAnyEvent(encoded_event, next, annotations,
                         this->annotation_layer_id_, records, membership, error))
        return false;
    // Refuse before the queue is touched: nothing durable refers to it.
    const QString unsaveable = unsaveableEvent(encoded_event, records, membership);
    if (!unsaveable.isEmpty())
        return fail(error, unsaveable);
    if (!this->queue_.append(encoded_event, error))
    {
        // An I/O failure may have happened after the transaction was written
        // to disk. We cannot safely accept further edits in that situation.
        this->blocked_ = true;
        return false;
    }
    this->visible_ = std::move(next);
    this->visible_annotations_ = std::move(annotations);
    return true;
}

bool PersonalDraftEditStage::prepareOldest(
    NetworkHydraulicDistribution &next_candidate,
    QList<PersonalWorkingCopyChange> &records,
    QList<HydraulicDraftMembershipEdit> &membership,
    QString *error, QUuid *annotation_layer_id) const
{
    if (error)
        error->clear();
    if (!isOpen() || this->blocked_ || this->queue_.pending().isEmpty())
        return fail(error, QStringLiteral("No writable personal draft with a queued transaction"));
    NetworkHydraulicDistribution next = this->persisted_;
    QList<MapAnnotation> annotations = this->persisted_annotations_;
    QList<PersonalWorkingCopyChange> proposed;
    QList<HydraulicDraftMembershipEdit> intents;
    const QByteArray encoded = this->queue_.pending().first().encoded_transaction;
    if (!prepareAnyEvent(encoded, next, annotations, this->annotation_layer_id_,
                         proposed, intents, error))
        return false;
    next_candidate = std::move(next);
    records = std::move(proposed);
    membership = std::move(intents);
    if (annotation_layer_id)
    {
        const std::optional<aowis::model::ModelEditTransaction> decoded =
            aowis::model::ModelEditTransactionCodec::decode(encoded);
        *annotation_layer_id = QUuid();
        if (decoded)
            for (const aowis::model::ModelEntityEdit &edit : decoded->edits)
                if (edit.entityType == QString::fromLatin1(aowis::model::mapAnnotationEditType))
                {
                    *annotation_layer_id = this->annotation_layer_id_;
                    break;
                }
    }
    return true;
}

bool PersonalDraftEditStage::acknowledgeOldest(const QByteArray &confirmed_event, QString *error)
{
    if (error)
        error->clear();
    if (!isOpen() || this->blocked_ || this->queue_.pending().isEmpty())
        return fail(error, QStringLiteral("No queued event to acknowledge"));
    const PersonalEditQueue::Entry first = this->queue_.pending().first();
    if (first.encoded_transaction != confirmed_event)
        return fail(error, QStringLiteral("Server confirmation does not match oldest local event"));
    NetworkHydraulicDistribution next = this->persisted_;
    QList<MapAnnotation> next_annotations = this->persisted_annotations_;
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    if (!prepareAnyEvent(confirmed_event, next, next_annotations,
                         this->annotation_layer_id_, records, membership, error))
    {
        this->blocked_ = true;
        return false;
    }
    if (!this->queue_.acknowledgePersisted(first.transaction_id, error))
    {
        this->blocked_ = true;
        return false;
    }
    this->persisted_ = std::move(next);
    this->persisted_annotations_ = std::move(next_annotations);
    return true;
}

bool PersonalDraftEditStage::isOpen() const
{
    return this->queue_.isOpen();
}

bool PersonalDraftEditStage::isBlocked() const
{
    return this->blocked_;
}

int PersonalDraftEditStage::pendingCount() const
{
    return this->queue_.pending().size();
}

const NetworkHydraulicDistribution &PersonalDraftEditStage::visibleNetwork() const
{
    return this->visible_;
}

const NetworkHydraulicDistribution &PersonalDraftEditStage::persistedNetwork() const
{
    return this->persisted_;
}

const PersonalEditQueue &PersonalDraftEditStage::queue() const
{
    return this->queue_;
}

const QList<MapAnnotation> &PersonalDraftEditStage::visibleAnnotations() const
{
    return this->visible_annotations_;
}

const QList<MapAnnotation> &PersonalDraftEditStage::persistedAnnotations() const
{
    return this->persisted_annotations_;
}
