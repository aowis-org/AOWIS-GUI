#include "projects/personal_draft_edit_session.h"

#include "projects/dvmdb_identity.h"
#include "projects/hydraulic_history_delta.h"

#include <QDateTime>
#include <QJsonArray>

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
QString normalized(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces);
}
}

PersonalDraftEditSession::PersonalDraftEditSession(QObject *parent)
    : QObject(parent), client_(this)
{
    this->history_retry_timer_.setSingleShot(true);
    connect(&this->history_retry_timer_, &QTimer::timeout, this,
            &PersonalDraftEditSession::recoverHistoryNavigation);
    this->renew_timer_.setInterval(40000);
    connect(&this->renew_timer_, &QTimer::timeout, this, [this]
    {
        if (this->state_ == State::Ready)
            renew();
        else if (this->state_ == State::Submitting)
            this->renewal_due_ = true;
    });
}

PersonalDraftEditSession::~PersonalDraftEditSession()
{
    // QObject destroys the HTTP client before its pending callbacks can run.
    // An outstanding remote lease expires naturally if no release completes.
    this->renew_timer_.stop();
    this->history_retry_timer_.stop();
    this->stage_.close();
}

void PersonalDraftEditSession::start(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &network_id, const QString &queue_root)
{
    if (this->state_ != State::Idle)
    {
        emit blocked(QStringLiteral("Close the previous private draft session before opening another."));
        return;
    }
    if ((target.kind != PersonalRepositoryTarget::Kind::Http &&
         target.kind != PersonalRepositoryTarget::Kind::Embedded) ||
        !DvmdbIdentity::isUuidV7(target.repository_id) ||
        !DvmdbIdentity::isUuidV7(draft_id) ||
        !DvmdbIdentity::isUuidV7(network_id) || queue_root.trimmed().isEmpty())
    {
        emit blocked(QStringLiteral("A verified private repository, draft, network and queue root are required."));
        return;
    }
    this->saved_history_events_.clear();
    this->history_edit_position_verified_ = false;
    this->target_ = target;
    this->draft_id_ = draft_id;
    this->network_id_ = network_id;
    this->queue_root_ = queue_root;
    this->state_ = State::Starting;
    this->stop_requested_ = false;

    this->client_.reopen(this->target_, this->draft_id_,
        [this](const PersonalWorkingCopyInfo &draft, const QString &error)
    {
        if (this->stop_requested_) { finishStop(); return; }
        if (!error.isEmpty() || draft.id != this->draft_id_ ||
            !DvmdbIdentity::isUuidV7(draft.session_id))
        {
            fail(QStringLiteral("Cannot verify the private draft: %1").arg(error));
            return;
        }
        const QByteArray journal_tail = draft.events.isEmpty()
            ? QByteArray() : draft.events.last().encoded;
        this->saved_history_events_.clear();
        for (const PersonalWorkingCopyEvent &event : draft.events)
            this->saved_history_events_.append(event.encoded);
        this->project_id_ = draft.project_id;
        this->session_id_ = draft.session_id;
        this->generation_ = draft.generation;
        this->base_revision_ = draft.base_revision;
        this->selected_draft_position_ = draft.events.size();
        this->client_.loadDraftNetwork(this->target_, this->draft_id_, this->network_id_,
            [this, journal_tail](const NetworkHydraulic &network,
                                 const QString &generation, const QString &load_error)
        {
            if (this->stop_requested_) { finishStop(); return; }
            if (!load_error.isEmpty() || generation != this->generation_ ||
                normalized(network.uuid) != this->network_id_)
            {
                fail(QStringLiteral("Draft changed during network hydration: %1").arg(load_error));
                return;
            }
            // Take the local queue lock BEFORE the lease. All writers of this
            // draft lease with its session UUID, and every acquisition rotates
            // the fencing token: a second window that acquired first would
            // fence out the running session and only then fail on the lock.
            // The queue lock is per machine, so it refuses here instead.
            QString stage_error;
            if (!this->stage_.open(this->queue_root_, QUuid(this->target_.repository_id),
                                   QUuid(this->draft_id_), network, journal_tail,
                                   &stage_error))
            {
                fail(QStringLiteral("Unable to recover local draft event queue: %1").arg(stage_error));
                return;
            }
            // The acquisition checks the generation once more.
            this->client_.acquireLease(this->target_, this->draft_id_, this->session_id_, 90,
                [this](const PersonalWorkingCopyLease &lease, const QString &lease_error)
            {
                if (!lease_error.isEmpty())
                {
                    if (this->stop_requested_) { finishStop(); return; }
                    fail(QStringLiteral("Cannot acquire private draft writer lease: %1").arg(lease_error));
                    return;
                }
                this->fencing_token_ = lease.token;
                this->lease_expiry_ = lease.expires_at_ms;
                if (this->stop_requested_)
                {
                    releaseAndClose();
                    return;
                }
                if (lease.generation != this->generation_ || leaseIsExpiring())
                {
                    fail(QStringLiteral("Draft generation changed or lease expired during checkout."));
                    return;
                }
                this->state_ = State::Ready;
                this->renew_timer_.start();
                emit ready();
                emit pendingCountChanged(this->stage_.pendingCount());
                refreshHistoryCursor();
                scheduleFlush();
            });
        });
    });
}

bool PersonalDraftEditSession::appendTransaction(const QByteArray &encoded_event, QString *error)
{
    if (error) error->clear();
    if (this->state_ != State::Ready && this->state_ != State::Submitting &&
        this->state_ != State::Renewing)
    {
        if (error) *error = QStringLiteral("Personal draft writer is not ready.");
        return false;
    }
    if (!this->history_edit_position_verified_)
    {
        if (error) *error = QStringLiteral(
            "Editing requires a verified DVMDB History position.");
        return false;
    }
    if (!this->stage_.append(encoded_event, error))
    {
        const QString detail = error ? *error : QStringLiteral("Queue append failed");
        fail(QStringLiteral("Cannot durably stage hydraulic edit: %1").arg(detail));
        return false;
    }
    this->history_cursor_ = {};
    emit historyCursorChanged();
    emit pendingCountChanged(this->stage_.pendingCount());
    scheduleFlush();
    return true;
}

void PersonalDraftEditSession::scheduleFlush()
{
    if (this->flush_scheduled_ || this->state_ == State::Blocked || this->stop_requested_)
        return;
    this->flush_scheduled_ = true;
    QTimer::singleShot(0, this, [this]
    {
        this->flush_scheduled_ = false;
        flush();
    });
}

bool PersonalDraftEditSession::leaseIsExpiring() const
{
    bool valid = false;
    const qulonglong expires = this->lease_expiry_.toULongLong(&valid);
    if (!valid || expires > static_cast<qulonglong>(std::numeric_limits<qint64>::max()))
        return true;
    return static_cast<qint64>(expires) <= QDateTime::currentMSecsSinceEpoch() + 25000;
}

void PersonalDraftEditSession::flush()
{
    if (this->state_ != State::Ready || this->stop_requested_)
        return;
    if (this->renewal_due_ || leaseIsExpiring())
    {
        renew();
        return;
    }
    if (this->stage_.pendingCount() == 0)
        return;
    NetworkHydraulic next;
    QList<PersonalWorkingCopyChange> changes;
    QList<HydraulicDraftMembershipEdit> membership;
    QString error;
    if (!this->stage_.prepareOldest(next, changes, membership, &error))
    {
        fail(QStringLiteral("Cannot stage next durable Save: %1").arg(error));
        return;
    }
    const QList<PersonalEditQueue::Entry> pending = this->stage_.queue().pending();
    const QByteArray oldest = pending.first().encoded_transaction;
    this->state_ = State::Submitting;
    this->request_in_flight_ = true;
    this->client_.saveEventsRecovering(this->target_, this->draft_id_, this->session_id_,
        this->fencing_token_, this->generation_, changes, {oldest}, membership,
        [this, oldest](const PersonalWorkingCopyEventSaveResult &result,
                       bool recovered, const QString &save_error)
    {
        this->request_in_flight_ = false;
        if (!save_error.isEmpty() || result.draft_id != this->draft_id_)
        {
            fail(QStringLiteral("Draft Save not authoritatively confirmed: %1").arg(save_error));
            return;
        }
        QString ack_error;
        if (!this->stage_.acknowledgeOldest(oldest, &ack_error))
        {
            fail(QStringLiteral("DVMDB saved an event, but local queue acknowledgement failed: %1")
                 .arg(ack_error));
            return;
        }
        this->generation_ = result.generation;
        // A new Save after Undo keeps only the selected journal prefix.
        if (this->selected_draft_position_ < this->saved_history_events_.size())
            this->saved_history_events_ = this->saved_history_events_.mid(
                0, this->selected_draft_position_);
        this->saved_history_events_.append(oldest);
        this->selected_draft_position_ = this->saved_history_events_.size();
        this->history_cursor_ = {};
        emit historyCursorChanged();
        emit saveConfirmed(this->generation_, recovered);
        emit pendingCountChanged(this->stage_.pendingCount());
        if (this->state_ == State::Blocked && !this->stop_requested_)
            return;
        if (this->stop_requested_)
        {
            releaseAndClose();
            return;
        }
        this->state_ = State::Ready;
        if (this->renewal_due_ || leaseIsExpiring())
            renew();
        else
            scheduleFlush();
        refreshHistoryCursor();
    });
}

void PersonalDraftEditSession::renew()
{
    if (this->state_ == State::Submitting)
    {
        this->renewal_due_ = true;
        return;
    }
    if (this->state_ != State::Ready || this->stop_requested_)
        return;
    this->state_ = State::Renewing;
    this->request_in_flight_ = true;
    emit historyCursorChanged();
    // DVMDB rotates fencing tokens even when renewing the same session.
    // Never issue this concurrently with a Save using the previous token.
    this->client_.acquireLease(this->target_, this->draft_id_, this->session_id_, 90,
        [this](const PersonalWorkingCopyLease &lease, const QString &error)
    {
        this->request_in_flight_ = false;
        if (!error.isEmpty())
        {
            fail(QStringLiteral("Private draft writer lease renewal failed: %1").arg(error));
            return;
        }
        this->fencing_token_ = lease.token;
        this->lease_expiry_ = lease.expires_at_ms;
        this->renewal_due_ = false;
        if (this->state_ == State::Blocked && !this->stop_requested_)
            return;
        if (this->stop_requested_)
        {
            releaseAndClose();
            return;
        }
        if (lease.generation != this->generation_ || leaseIsExpiring())
        {
            fail(QStringLiteral("Draft generation changed during lease renewal."));
            return;
        }
        this->state_ = State::Ready;
        scheduleFlush();
        refreshHistoryCursor();
    });
}

QString PersonalDraftEditSession::historyScopeKey() const
{
    return this->target_.repository_id + QStringLiteral(":") +
        this->project_id_ + QStringLiteral(":") + this->draft_id_;
}

void PersonalDraftEditSession::branchHistoryPage(
    const QString &branch_name, const QString &after_revision,
    BranchHistoryPageCompletion completion)
{
    if (this->state_ == State::Idle || this->state_ == State::Stopping ||
        this->project_id_.isEmpty() || this->stop_requested_)
    {
        completion({}, {}, QStringLiteral("No open private History repository."));
        return;
    }
    this->client_.branchHistoryPage(this->target_, this->project_id_,
        branch_name, after_revision, std::move(completion), this->draft_id_);
}

void PersonalDraftEditSession::archivedActionPage(
    const QString &branch_name, const QString &revision_id, int offset,
    ArchivedPageCompletion completion)
{
    if (this->state_ == State::Idle || this->state_ == State::Stopping ||
        this->project_id_.isEmpty() || this->stop_requested_)
    {
        completion({}, -1, -1, QStringLiteral("No open private History repository."));
        return;
    }
    this->client_.archivedActionPage(this->target_, this->project_id_,
        branch_name, revision_id, offset, std::move(completion), this->draft_id_);
}

const QJsonObject &PersonalDraftEditSession::historyCursor() const
{
    return this->history_cursor_;
}

bool PersonalDraftEditSession::canCaptureHistoryEdits() const
{
    return isReady() && !this->stop_requested_ &&
        this->history_edit_position_verified_;
}

bool PersonalDraftEditSession::canNavigateHistory() const
{
    return this->state_ == State::Ready && !this->stop_requested_ &&
        this->history_network_qualified_ &&
        !this->request_in_flight_ && !this->history_cursor_request_in_flight_ &&
        this->stage_.pendingCount() == 0 && !leaseIsExpiring() &&
        !this->history_cursor_.isEmpty() &&
        this->history_cursor_.value(QStringLiteral("generation")).toString() == this->generation_ &&
        this->history_cursor_.value(QStringLiteral("baseRevision")).toString() == this->base_revision_;
}

QString PersonalDraftEditSession::historyNavigationStatus() const
{
    if (this->state_ == State::Navigating)
        return QStringLiteral("Navigating and verifying History state…");
    if (this->state_ == State::RecoveringHistory)
        return QStringLiteral("History acknowledgement uncertain; reconnecting with the same operation UUID. "
                              "Editing is paused; no full network is being downloaded.");
    if (this->state_ == State::Blocked)
        return QStringLiteral("Private editing is blocked; reopen the draft to recover History.");
    if (this->stage_.pendingCount() > 0)
        return QStringLiteral("Wait for all pending Saves before navigating History.");
    if (this->state_ != State::Ready || this->stop_requested_ || leaseIsExpiring())
        return QStringLiteral("Private writer lease is not ready for History navigation.");
    if (!this->history_network_qualified_)
        return QStringLiteral("History requires one verified existing hydraulic network.");
    if (this->history_cursor_request_in_flight_ ||
        this->history_qualification_in_flight_ || this->history_cursor_.isEmpty())
        return QStringLiteral("Waiting for the authoritative DVMDB History cursor.");
    return QStringLiteral("Only DVMDB-verified adjacent History actions can be selected.");
}

void PersonalDraftEditSession::refreshHistoryCursor()
{
    if (this->state_ != State::Ready || this->stop_requested_ ||
        this->request_in_flight_ ||
        this->history_cursor_request_in_flight_ ||
        this->stage_.pendingCount() != 0)
        return;
    if (!this->history_network_qualified_)
    {
        if (this->history_qualification_in_flight_) return;
        this->history_qualification_in_flight_ = true;
        const QString generation = this->generation_;
        const QString draft_id = this->draft_id_;
        this->client_.summarizeDraftNetworks(this->target_, this->draft_id_,
            [this, generation, draft_id](const QJsonObject &summary, const QString &error)
        {
            if (this->draft_id_ != draft_id) return;
            this->history_qualification_in_flight_ = false;
            if (this->state_ != State::Ready || this->stop_requested_ ||
                this->stage_.pendingCount() != 0 || this->generation_ != generation)
                return;
            const QJsonArray networks = summary.value(QStringLiteral("networks")).toArray();
            this->history_network_qualified_ = error.isEmpty() &&
                summary.value(QStringLiteral("generation")).toString() == generation &&
                summary.value(QStringLiteral("networkCount")).toInt(-1) == 1 &&
                networks.size() == 1 &&
                networks.first().toObject().value(QStringLiteral("id")).toString() ==
                    this->network_id_;
            if (this->history_network_qualified_) refreshHistoryCursor();
            else emit historyCursorChanged();
        });
        return;
    }
    this->history_cursor_request_in_flight_ = true;
    emit historyCursorChanged();
    const QString generation = this->generation_;
    const QString draft_id = this->draft_id_;
    this->client_.historyCursor(this->target_, this->draft_id_,
        [this, generation, draft_id](const QJsonObject &cursor, const QString &error)
    {
        if (this->draft_id_ != draft_id) return;
        this->history_cursor_request_in_flight_ = false;
        if (this->state_ != State::Ready || this->stop_requested_ ||
            this->stage_.pendingCount() != 0 || this->generation_ != generation)
            return;
        this->history_cursor_ = {};
        // A failed or inconsistent authoritative read must not grant edit capture.
        this->history_edit_position_verified_ = false;
        if (error.isEmpty() && cursor.value(QStringLiteral("generation")).toString() == generation &&
            cursor.value(QStringLiteral("baseRevision")).toString() == this->base_revision_)
        {
            this->history_cursor_ = cursor;
            const QString kind = cursor.value(QStringLiteral("selectionKind")).toString();
            if (kind == QStringLiteral("draft"))
            {
                bool valid = false;
                const qsizetype position = cursor.value(
                    QStringLiteral("actionPosition")).toString().toLongLong(&valid);
                if (valid && position >= 0 && position <= this->saved_history_events_.size())
                {
                    this->selected_draft_position_ = position;
                    this->history_edit_position_verified_ = true;
                }
            }
            else if (kind == QStringLiteral("archived") &&
                     cursor.value(QStringLiteral("canBranchFromHistory")).toBool(false))
            {
                this->selected_draft_position_ = 0;
                this->history_edit_position_verified_ = true;
            }
        }
        emit historyCursorChanged();
    });
}

void PersonalDraftEditSession::navigateHistory(bool backward)
{
    if (!canNavigateHistory()) return;
    const QJsonObject coordinate = this->history_cursor_.value(
        backward ? QStringLiteral("previousAction") : QStringLiteral("nextAction")).toObject();
    navigateHistoryTo(coordinate);
}

void PersonalDraftEditSession::navigateHistoryTo(const QJsonObject &coordinate)
{
    if (!canNavigateHistory() || coordinate.isEmpty()) return;
    const QString kind = coordinate.value(QStringLiteral("selectionKind")).toString();
    const QString position_string = coordinate.value(QStringLiteral("actionPosition")).toString();
    bool valid = false;
    const qulonglong position = position_string.toULongLong(&valid);
    if (!valid || position_string.isEmpty() || position > 4096) return;
    if (kind == QStringLiteral("draft"))
    {
        if (!coordinate.value(QStringLiteral("revisionId")).isNull() ||
            position > static_cast<qulonglong>(this->saved_history_events_.size())) return;
        if (this->history_cursor_.value(QStringLiteral("selectionKind")).toString() == kind &&
            this->history_cursor_.value(QStringLiteral("actionPosition")).toString() ==
                position_string) return;
    }
    else if (kind == QStringLiteral("archived"))
    {
        const QString revision = coordinate.value(QStringLiteral("revisionId")).toString();
        // Position zero is a legitimate archived boundary used by adjacent
        // Undo to cross the first action of a Commit into its parent state.
        if (!DvmdbIdentity::isUuidV7(revision)) return;
        if (this->history_cursor_.value(QStringLiteral("selectionKind")).toString() == kind &&
            this->history_cursor_.value(QStringLiteral("archivedRevisionId")).toString() == revision &&
            this->history_cursor_.value(QStringLiteral("archivedActionPosition")).toString() ==
                position_string) return;
    }
    else return;
    // Retain the source coordinate before clearing the GUI's navigable
    // cursor. A matching SHA alone is insufficient to prove History position.
    this->history_source_cursor_ = this->history_cursor_;
    this->state_ = State::Navigating;
    this->history_edit_position_verified_ = false;
    this->request_in_flight_ = true;
    this->history_cursor_ = {};
    emit historyCursorChanged();
    emit historyNavigationStarted();
    const QString operation_id = DvmdbIdentity::newUuidV7();
    const QString old_generation = this->generation_;
    bool sourceGenerationValid = false;
    const qulonglong sourceGeneration = old_generation.toULongLong(&sourceGenerationValid);
    if (!sourceGenerationValid || sourceGeneration == std::numeric_limits<qulonglong>::max())
    {
        this->request_in_flight_ = false;
        fail(QStringLiteral("Invalid or exhausted private History generation."));
        return;
    }
    this->history_operation_id_ = operation_id;
    this->history_source_generation_ = old_generation;
    this->history_target_coordinate_ = coordinate;
    this->history_recovery_attempts_ = 0;
    this->client_.navigateHistoryRecovering(this->target_, this->draft_id_, operation_id,
        this->session_id_, this->fencing_token_, old_generation,
        this->base_revision_, coordinate,
        [this](const PersonalWorkingCopyInfo &result, bool uncertain, const QString &error)
    {
        this->request_in_flight_ = false;
        if (this->stop_requested_) { releaseAndClose(); return; }
        if (uncertain && !error.isEmpty())
        {
            scheduleHistoryRecovery();
            return;
        }
        finishHistoryNavigation(result, error);
    }, this->network_id_);
}

void PersonalDraftEditSession::finishHistoryNavigation(
    const PersonalWorkingCopyInfo &result, const QString &error)
{
    bool sourceGenerationValid = false;
    const qulonglong sourceGeneration = this->history_source_generation_.toULongLong(
        &sourceGenerationValid);
    if (!sourceGenerationValid || sourceGeneration == std::numeric_limits<qulonglong>::max())
    {
        fail(QStringLiteral("Invalid pending History source generation."));
        return;
    }
    if (!error.isEmpty() || result.id != this->draft_id_ ||
        result.generation != QString::number(sourceGeneration + 1) ||
        result.base_revision != this->base_revision_ ||
        result.session_id != this->session_id_ ||
        result.event_count != QString::number(this->saved_history_events_.size()))
    {
        fail(QStringLiteral("History navigation was not authoritatively confirmed: %1").arg(error));
        return;
    }
    // The server reconstructs the full network, but sends only changed
    // typed MODEL images and sparse membership moves. Nothing is applied
    // to the local stage until all source images and the final canonical
    // SHA-256 have been verified against our saved snapshot.
    NetworkHydraulic network;
    QString deltaError;
    if (!HydraulicHistoryDelta::apply(result.history_delta,
            this->stage_.persistedNetwork(), network, &deltaError) ||
        normalized(network.uuid) != this->network_id_)
    {
        fail(QStringLiteral("Cannot verify incremental History state: %1").arg(deltaError));
        return;
    }
    if (!this->history_expected_destination_digest_.isEmpty() &&
        HydraulicHistoryDelta::digest(network) != this->history_expected_destination_digest_)
    {
        fail(QStringLiteral("Sparse History delta disagrees with reconciled destination SHA-256."));
        return;
    }
    completeHistoryNavigation(network, result.generation, true);
}

void PersonalDraftEditSession::completeHistoryNavigation(
    const NetworkHydraulic &network, const QString &generation, bool replace_network)
{
    // Both a successful sparse delta and a matching SHA reconciliation must
    // pass this final publication path. The latter never replaces the model.
    if (replace_network)
    {
        QString stageError;
        if (!this->stage_.resetVerifiedNetwork(network, &stageError))
        {
            fail(QStringLiteral("Cannot publish navigated editor state: %1").arg(stageError));
            return;
        }
    }
    this->generation_ = generation;
    this->history_network_qualified_ = true;
    const QJsonObject coordinate = this->history_target_coordinate_;
    if (coordinate.value(QStringLiteral("selectionKind")).toString() ==
        QStringLiteral("draft"))
    {
        bool valid = false;
        const qsizetype position = coordinate.value(
            QStringLiteral("actionPosition")).toString().toLongLong(&valid);
        if (!valid || position < 0 || position > this->saved_history_events_.size())
        {
            fail(QStringLiteral("Invalid navigated draft History action position."));
            return;
        }
        this->selected_draft_position_ = position;
    }
    else
        this->selected_draft_position_ = 0;
    clearHistoryRecovery();
    if (this->stop_requested_) { releaseAndClose(); return; }
    this->state_ = State::Ready;
    emit historyNavigationCompleted(network);
    refreshHistoryCursor();
    if (leaseIsExpiring()) renew();
}

void PersonalDraftEditSession::scheduleHistoryRecovery()
{
    if (this->stop_requested_)
    {
        releaseAndClose();
        return;
    }
    if (this->history_operation_id_.isEmpty())
    {
        fail(QStringLiteral("Lost the pending History navigation identity."));
        return;
    }
    const bool firstRecovery = this->state_ != State::RecoveringHistory;
    this->state_ = State::RecoveringHistory;
    this->history_edit_position_verified_ = false;
    this->history_cursor_ = {};
    // Back off during outages instead of continuously flooding a remote
    // personal REST server. Do not impose a short fixed retry deadline:
    // an offline session can remain safely paused until the link returns.
    const unsigned int shift = std::min(this->history_recovery_attempts_, 3u);
    const int delayMs = 1000 * static_cast<int>(1u << shift);
    if (this->history_recovery_attempts_ < 100) ++this->history_recovery_attempts_;
    this->history_retry_timer_.start(delayMs);
    emit historyCursorChanged();
    if (firstRecovery) emit historyNavigationRecovering();
}

void PersonalDraftEditSession::recoverHistoryNavigation()
{
    if (this->state_ != State::RecoveringHistory || this->request_in_flight_)
        return;
    if (this->stop_requested_) { releaseAndClose(); return; }
    // Read a small authoritative SHA-256 receipt first. This is read-only:
    // a bad connection does not rotate our lease or accidentally Undo again.
    this->request_in_flight_ = true;
    this->client_.reconcileHistory(this->target_, this->draft_id_, this->network_id_,
        [this](const QJsonObject &receipt, bool temporary, const QString &error)
    {
        this->request_in_flight_ = false;
        if (this->stop_requested_) { releaseAndClose(); return; }
        if (!error.isEmpty())
        {
            if (temporary) scheduleHistoryRecovery();
            else fail(QStringLiteral("Cannot verify History reconciliation: %1").arg(error));
            return;
        }
        bool sourceValid = false;
        const qulonglong source = this->history_source_generation_.toULongLong(&sourceValid);
        if (!sourceValid || source == std::numeric_limits<qulonglong>::max())
        {
            fail(QStringLiteral("Invalid pending History source generation."));
            return;
        }
        const QString targetGeneration = QString::number(source + 1);
        const QString generation = receipt.value(QStringLiteral("generation")).toString();
        const QString operation = receipt.value(
            QStringLiteral("lastNavigationOperationId")).toString();
        const QString operationSource = receipt.value(
            QStringLiteral("lastNavigationSourceGeneration")).toString();
        const QString kind = receipt.value(QStringLiteral("selectionKind")).toString();
        const QString revision = receipt.value(QStringLiteral("revisionId")).toString();
        const QString position = receipt.value(QStringLiteral("actionPosition")).toString();
        const QString localDigest = HydraulicHistoryDelta::digest(this->stage_.persistedNetwork());
        const QString remoteDigest = receipt.value(QStringLiteral("networkSha256")).toString();
        if (receipt.value(QStringLiteral("sessionId")).toString() != this->session_id_ ||
            receipt.value(QStringLiteral("baseRevision")).toString() != this->base_revision_ ||
            receipt.value(QStringLiteral("journalCount")).toString() !=
                QString::number(this->saved_history_events_.size()) ||
            localDigest.isEmpty())
        {
            fail(QStringLiteral("The personal working copy changed during History recovery."));
            return;
        }
        const QJsonObject &target = this->history_target_coordinate_;
        const bool targetMatches = kind == target.value(QStringLiteral("selectionKind")).toString() &&
            position == target.value(QStringLiteral("actionPosition")).toString() &&
            (kind != QStringLiteral("archived") ||
             revision == target.value(QStringLiteral("revisionId")).toString());
        const QJsonObject &sourceCursor = this->history_source_cursor_;
        const QString sourceKind = sourceCursor.value(QStringLiteral("selectionKind")).toString();
        const QString sourcePosition = sourceKind == QStringLiteral("archived")
            ? sourceCursor.value(QStringLiteral("archivedActionPosition")).toString()
            : sourceCursor.value(QStringLiteral("actionPosition")).toString();
        const bool sourceMatches = kind == sourceKind && position == sourcePosition &&
            (kind != QStringLiteral("archived") ||
             revision == sourceCursor.value(QStringLiteral("archivedRevisionId")).toString());
        const bool alreadyPublished = generation == targetGeneration &&
            operation == this->history_operation_id_ &&
            operationSource == this->history_source_generation_ && targetMatches;
        const bool notPublished = generation == this->history_source_generation_ &&
            sourceMatches && remoteDigest == localDigest;
        if (!alreadyPublished && !notPublished)
        {
            fail(QStringLiteral("History generation, operation or cursor diverged during outage."));
            return;
        }
        // A successful Undo can leave identical network bytes (e.g. a MODEL
        // value changed and changed back). Its UUID, generation and cursor
        // must still agree before skipping sparse replay.
        this->history_expected_destination_digest_ = alreadyPublished ? remoteDigest : QString();
        resumeHistoryAfterReconcile(alreadyPublished, remoteDigest == localDigest, generation);
    });
}

void PersonalDraftEditSession::resumeHistoryAfterReconcile(
    bool already_published, bool model_matches, const QString &server_generation)
{
    // The writer lease may expire during a long outage. Reacquire it with
    // the same session UUID only AFTER verifying the server's state.
    this->request_in_flight_ = true;
    this->client_.acquireLeaseRecovering(this->target_, this->draft_id_,
        this->session_id_, 90,
        [this, already_published, model_matches, server_generation](
            const PersonalWorkingCopyLease &lease, bool temporary, const QString &error)
    {
        this->request_in_flight_ = false;
        if (this->stop_requested_) { releaseAndClose(); return; }
        if (!error.isEmpty())
        {
            if (temporary) scheduleHistoryRecovery();
            else fail(QStringLiteral("Cannot reacquire History writer lease: %1").arg(error));
            return;
        }
        if (lease.generation != server_generation)
        {
            fail(QStringLiteral("History generation changed between reconciliation and lease."));
            return;
        }
        this->fencing_token_ = lease.token;
        this->lease_expiry_ = lease.expires_at_ms;
        if (leaseIsExpiring())
        {
            fail(QStringLiteral("Recovered writer lease is already expiring."));
            return;
        }
        if (already_published && model_matches)
        {
            // The server has the exact requested UUID/cursor and the same
            // canonical network. No MODEL delta, no network GET, no replay.
            completeHistoryNavigation(this->stage_.persistedNetwork(),
                                      server_generation, false);
            return;
        }
        // Not published: execute the original intent. Published but different
        // hash: replay the same UUID to obtain the retained sparse delta.
        this->request_in_flight_ = true;
        this->client_.navigateHistoryRecovering(this->target_, this->draft_id_,
            this->history_operation_id_, this->session_id_, this->fencing_token_,
            this->history_source_generation_, this->base_revision_,
            this->history_target_coordinate_,
            [this](const PersonalWorkingCopyInfo &result, bool uncertain,
                   const QString &navigationError)
        {
            this->request_in_flight_ = false;
            if (this->stop_requested_) { releaseAndClose(); return; }
            if (uncertain && !navigationError.isEmpty())
            {
                scheduleHistoryRecovery();
                return;
            }
            finishHistoryNavigation(result, navigationError);
        }, this->network_id_);
    });
}

void PersonalDraftEditSession::clearHistoryRecovery()
{
    this->history_retry_timer_.stop();
    this->history_operation_id_.clear();
    this->history_source_generation_.clear();
    this->history_expected_destination_digest_.clear();
    this->history_target_coordinate_ = {};
    this->history_source_cursor_ = {};
    this->history_recovery_attempts_ = 0;
}

void PersonalDraftEditSession::blockEditing(const QString &message)
{
    if (this->state_ == State::Ready || this->state_ == State::Submitting ||
        this->state_ == State::Renewing)
        fail(message);
}

void PersonalDraftEditSession::fail(const QString &message)
{
    this->renew_timer_.stop();
    clearHistoryRecovery();
    this->state_ = State::Blocked;
    this->history_edit_position_verified_ = false;
    this->history_cursor_ = {};
    emit historyCursorChanged();
    emit blocked(message);
    if (this->stop_requested_ && !this->request_in_flight_)
        releaseAndClose();
}

void PersonalDraftEditSession::stop()
{
    if (this->state_ == State::Idle) return;
    this->stop_requested_ = true;
    this->renew_timer_.stop();
    this->history_retry_timer_.stop();
    if (this->request_in_flight_ || this->state_ == State::Starting)
        return;
    releaseAndClose();
}

void PersonalDraftEditSession::finishStop()
{
    this->stage_.close();
    resetLocal();
    emit stopped({});
}

void PersonalDraftEditSession::releaseAndClose()
{
    if (this->state_ == State::Stopping) return;
    this->state_ = State::Stopping;
    this->renew_timer_.stop();
    this->stage_.close();
    if (this->fencing_token_.isEmpty())
    {
        finishStop();
        return;
    }
    this->client_.releaseLease(this->target_, this->draft_id_, this->session_id_,
        this->fencing_token_, [this](const QString &error)
    {
        resetLocal();
        emit stopped(error);
    });
}

void PersonalDraftEditSession::resetLocal()
{
    clearHistoryRecovery();
    this->stage_.close();
    this->saved_history_events_.clear();
    this->history_cursor_ = {};
    this->history_edit_position_verified_ = false;
    this->history_cursor_request_in_flight_ = false;
    this->history_network_qualified_ = false;
    this->history_qualification_in_flight_ = false;
    this->base_revision_.clear();
    this->selected_draft_position_ = 0;
    emit historyCursorChanged();
    this->target_ = PersonalRepositoryTarget{};
    this->draft_id_.clear();
    this->project_id_.clear();
    this->network_id_.clear();
    this->queue_root_.clear();
    this->session_id_.clear();
    this->generation_.clear();
    this->fencing_token_.clear();
    this->lease_expiry_.clear();
    this->state_ = State::Idle;
    this->stop_requested_ = false;
    this->renewal_due_ = false;
    this->flush_scheduled_ = false;
    this->request_in_flight_ = false;
}

bool PersonalDraftEditSession::isReady() const
{
    return this->state_ == State::Ready || this->state_ == State::Submitting ||
           this->state_ == State::Renewing;
}

bool PersonalDraftEditSession::isBlocked() const
{
    return this->state_ == State::Blocked;
}

int PersonalDraftEditSession::pendingCount() const
{
    return this->stage_.pendingCount();
}

QString PersonalDraftEditSession::currentGeneration() const
{
    return this->generation_;
}

const NetworkHydraulic &PersonalDraftEditSession::visibleNetwork() const
{
    return this->stage_.visibleNetwork();
}

QList<QByteArray> PersonalDraftEditSession::savedHistoryEvents() const
{
    return this->saved_history_events_;
}

QList<QByteArray> PersonalDraftEditSession::pendingHistoryEvents() const
{
    QList<QByteArray> events;
    for (const PersonalEditQueue::Entry &entry : this->stage_.queue().pending())
        events.append(entry.encoded_transaction);
    return events;
}
