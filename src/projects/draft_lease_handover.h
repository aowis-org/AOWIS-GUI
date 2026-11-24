#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

// Hands the writer lease of a private draft from the open editor to a Commit
// and back. While a network is open for editing, its PersonalDraftEditSession
// holds the draft's lease and keeps renewing it; a Commit from the Projects
// HEAD inspector starts its own writer session, which DVMDB refuses with
// "draft held by another writer session" for as long as the editor holds it.
//
// release(): pause edit capture, wait until every queued edit is saved, stop
// the session (which releases its lease), then call done(). resume(): reopen
// editing of the same draft once the Commit has released its own lease.
// Nothing is handed over while editing is blocked, closing or still opening;
// a failure before the stop leaves editing running and unpaused.
class DraftLeaseHandover final : public QObject
{
    Q_DECLARE_TR_FUNCTIONS(DraftLeaseHandover)

public:
    // The editor as the handover sees it (MainWindow and its session).
    struct Editor
    {
        std::function<bool()> active;   // a draft is open for editing
        std::function<bool()> ready;    // its writer holds the lease
        std::function<bool()> blocked;
        std::function<bool()> closing;
        std::function<QString()> draft_id;
        std::function<QString()> network_id;
        std::function<int()> pending;   // queued edits not yet saved
        std::function<void()> pause;    // stop capture, show "Committing"
        std::function<void()> unpause;  // undo pause() when nothing was handed over
        std::function<void()> stop;     // stop the session (releases the lease)
        std::function<void(const QString &draft_id, const QString &network_id)> reopen;
    };

    explicit DraftLeaseHandover(Editor editor, QObject *parent = nullptr, int timeout_ms = 30000);

    // ProjectsWidget::DraftWriterHandoff. done(error) runs exactly once.
    void release(const QString &draft_id, std::function<void(const QString &error)> done);
    void resume(const QString &draft_id);

    // Forwarded editor events.
    void pendingCountChanged(int count);
    void blocked(const QString &reason);
    // True when this stop was the handover's own; the caller then skips its
    // usual "stopped editing" UI.
    bool stopped(const QString &error);

    // Edit capture must stay off (and its guard quiet) while this is true.
    bool isHandingOver() const;
    // The next ready() belongs to a reopen after a Commit.
    bool isResuming() const;
    void clearResuming();

private:
    enum class Phase { Idle, Draining, Stopping };
    void beginStop();
    void fail(const QString &message, bool unpause);
    void settle(const QString &error);

    Editor editor;
    QTimer timeout;
    Phase phase = Phase::Idle;
    std::function<void(const QString &)> done;
    QString resume_draft_id;
    QString resume_network_id;
    bool resuming = false;
};
