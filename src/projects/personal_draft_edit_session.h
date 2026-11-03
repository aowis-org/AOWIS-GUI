#pragma once

#include "projects/personal_draft_edit_stage.h"
#include "projects/personal_working_copy_client.h"

#include <QObject>
#include <QTimer>

// Owns the fenced HTTP writer for ONE verified private hydraulic draft.
// The session opens the authoritative draft and network, then obtains a
// rotating fencing lease before touching the crash-recoverable queue.
// All HTTP operations are serialized: renewal must never invalidate a token
// while a Save is in flight. A failed/uncertain Save blocks this session and
// preserves every unacknowledged event for a fresh authoritative reopen.
//
// This controller does not attach itself to HydraulicData. Callers may enable
// live editor capture only after ready(), and MUST disable it immediately on
// blocked(). The caller must compare visibleNetwork() to the live model before
// enabling capture. This component alone makes no claim of editor autosave.
class PersonalDraftEditSession final : public QObject
{
    Q_OBJECT
public:
    explicit PersonalDraftEditSession(QObject *parent = nullptr);
    ~PersonalDraftEditSession() override;

    void start(const PersonalRepositoryTarget &target, const QString &draft_id,
               const QString &network_id, const QString &queue_root);
    // This is called AFTER an operation has changed the live editor model.
    // A false return means capture must be disabled immediately.
    bool appendTransaction(const QByteArray &encoded_event, QString *error = nullptr);
    void stop();
    // Fail closed when the GUI detects an unrecorded or inconsistent mutation.
    void blockEditing(const QString &message);

    bool isReady() const;
    bool isBlocked() const;
    int pendingCount() const;
    QString currentGeneration() const;
    const NetworkHydraulic &visibleNetwork() const;
    QList<QByteArray> savedHistoryEvents() const;
    QList<QByteArray> pendingHistoryEvents() const;

signals:
    void ready();
    void pendingCountChanged(int count);
    void saveConfirmed(const QString &generation, bool recovered);
    void blocked(const QString &message);
    void stopped(const QString &error);

private:
    enum class State { Idle, Starting, Ready, Submitting, Renewing, Blocked, Stopping };
    void scheduleFlush();
    void flush();
    void renew();
    void fail(const QString &message);
    void finishStop();
    void releaseAndClose();
    void resetLocal();
    bool leaseIsExpiring() const;

    PersonalWorkingCopyClient client_;
    QList<QByteArray> saved_history_events_;
    PersonalDraftEditStage stage_;
    QTimer renew_timer_;
    PersonalRepositoryTarget target_;
    QString draft_id_;
    QString network_id_;
    QString queue_root_;
    QString session_id_;
    QString generation_;
    QString fencing_token_;
    QString lease_expiry_;
    State state_ = State::Idle;
    bool stop_requested_ = false;
    bool flush_scheduled_ = false;
    bool renewal_due_ = false;
    bool request_in_flight_ = false;
};
