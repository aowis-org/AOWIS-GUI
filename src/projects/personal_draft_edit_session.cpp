#include "projects/personal_draft_edit_session.h"

#include "projects/dvmdb_identity.h"

#include <QDateTime>

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
        this->session_id_ = draft.session_id;
        this->generation_ = draft.generation;
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
    if (!this->stage_.append(encoded_event, error))
    {
        const QString detail = error ? *error : QStringLiteral("Queue append failed");
        fail(QStringLiteral("Cannot durably stage hydraulic edit: %1").arg(detail));
        return false;
    }
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
        this->saved_history_events_.append(oldest);
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
    });
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
    this->state_ = State::Blocked;
    emit blocked(message);
    if (this->stop_requested_ && !this->request_in_flight_)
        releaseAndClose();
}

void PersonalDraftEditSession::stop()
{
    if (this->state_ == State::Idle) return;
    this->stop_requested_ = true;
    this->renew_timer_.stop();
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
    this->stage_.close();
    this->saved_history_events_.clear();
    this->target_ = PersonalRepositoryTarget{};
    this->draft_id_.clear();
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
