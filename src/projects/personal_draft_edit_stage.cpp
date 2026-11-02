#include "projects/personal_draft_edit_stage.h"
#include "projects/hydraulic_draft_entity_registry.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/serialization/model_codec.h>

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

// True when the candidate already holds every after-image of the event (and
// lacks every entity it deletes). That is the state the event produces, so it
// reached DVMDB before its local acknowledgement was lost, also when a later
// Commit has since cleared the journal that would otherwise prove it.
bool eventAlreadyApplied(const QByteArray &encoded_event, const NetworkHydraulic &candidate)
{
    const std::optional<aowis::model::ModelEditTransaction> event =
        aowis::model::ModelEditTransactionCodec::decode(encoded_event);
    if (!event.has_value() || event->edits.isEmpty())
        return false;
    for (const aowis::model::ModelEntityEdit &edit : event->edits)
    {
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
    const NetworkHydraulic &verified_candidate,
    const QByteArray &last_journal_event, QString *error)
{
    close();
    if (error)
        error->clear();
    if (verified_candidate.uuid.isNull())
        return fail(error, QStringLiteral("A verified hydraulic network UUID is required"));
    if (!this->queue_.open(queue_root, repository_id, draft_id, error))
        return false;

    this->persisted_ = verified_candidate;
    this->visible_ = verified_candidate;

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
        if (!HydraulicDraftEventAdapter::prepareWithMembership(
                entry.encoded_transaction, this->visible_, records,
                membership, &replay_error))
        {
            // Saves are serial, so only the oldest queued event can have
            // reached DVMDB unacknowledged. If the candidate already is its
            // result, retire it locally instead of blocking the draft forever.
            if (index == 0 && eventAlreadyApplied(entry.encoded_transaction, this->visible_))
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

void PersonalDraftEditStage::close()
{
    this->queue_.close();
    this->persisted_ = NetworkHydraulic{};
    this->visible_ = NetworkHydraulic{};
    this->blocked_ = false;
}

bool PersonalDraftEditStage::append(const QByteArray &encoded_event, QString *error)
{
    if (error)
        error->clear();
    if (!isOpen() || this->blocked_)
        return fail(error, QStringLiteral("Personal draft edit stage is not writable"));
    NetworkHydraulic next = this->visible_;
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    if (!HydraulicDraftEventAdapter::prepareWithMembership(
            encoded_event, next, records, membership, error))
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
    return true;
}

bool PersonalDraftEditStage::prepareOldest(
    NetworkHydraulic &next_candidate,
    QList<PersonalWorkingCopyChange> &records,
    QList<HydraulicDraftMembershipEdit> &membership,
    QString *error) const
{
    if (error)
        error->clear();
    if (!isOpen() || this->blocked_ || this->queue_.pending().isEmpty())
        return fail(error, QStringLiteral("No writable personal draft with a queued transaction"));
    NetworkHydraulic next = this->persisted_;
    QList<PersonalWorkingCopyChange> proposed;
    QList<HydraulicDraftMembershipEdit> intents;
    if (!HydraulicDraftEventAdapter::prepareWithMembership(
            this->queue_.pending().first().encoded_transaction,
            next, proposed, intents, error))
        return false;
    next_candidate = std::move(next);
    records = std::move(proposed);
    membership = std::move(intents);
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
    NetworkHydraulic next;
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    if (!prepareOldest(next, records, membership, error))
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

const NetworkHydraulic &PersonalDraftEditStage::visibleNetwork() const
{
    return this->visible_;
}

const NetworkHydraulic &PersonalDraftEditStage::persistedNetwork() const
{
    return this->persisted_;
}

const PersonalEditQueue &PersonalDraftEditStage::queue() const
{
    return this->queue_;
}
