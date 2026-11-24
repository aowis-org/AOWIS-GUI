#include "projects/draft_lease_handover.h"

#include <utility>

DraftLeaseHandover::DraftLeaseHandover(Editor editor, QObject *parent, int timeout_ms)
    : QObject(parent), editor(std::move(editor)), timeout(this)
{
    this->timeout.setSingleShot(true);
    this->timeout.setInterval(timeout_ms);
    connect(&this->timeout, &QTimer::timeout, this, [this]
    {
        if (this->phase == Phase::Draining)
            fail(tr("The queued edits were not saved in time, so the Commit was not started."), true);
        else if (this->phase == Phase::Stopping)
        {
            this->resume_draft_id.clear();
            this->resume_network_id.clear();
            settle(tr("The editor did not release the draft in time."));
        }
    });
}

void DraftLeaseHandover::release(const QString &draft_id, std::function<void(const QString &)> done)
{
    if (!this->editor.active() || this->editor.draft_id() != draft_id)
    {
        done({}); // This GUI is not editing that draft.
        return;
    }
    if (this->phase != Phase::Idle || this->editor.closing())
    {
        done(tr("The editor is closing this draft. Try again in a moment."));
        return;
    }
    if (this->editor.blocked())
    {
        done(tr("Editing of this draft is blocked. Close the blocked private draft, then commit."));
        return;
    }
    if (!this->editor.ready())
    {
        done(tr("The draft is still being opened for editing. Try again in a moment."));
        return;
    }
    this->done = std::move(done);
    this->resume_draft_id = draft_id;
    this->resume_network_id = this->editor.network_id();
    this->phase = Phase::Draining;
    this->editor.pause();
    this->timeout.start();
    if (this->editor.pending() == 0)
        beginStop();
}

void DraftLeaseHandover::resume(const QString &draft_id)
{
    if (draft_id.isEmpty() || draft_id != this->resume_draft_id) return;
    const QString network_id = this->resume_network_id;
    this->resume_draft_id.clear();
    this->resume_network_id.clear();
    if (this->editor.active()) return; // Something else was opened meanwhile.
    this->resuming = true;
    this->editor.reopen(draft_id, network_id);
}

void DraftLeaseHandover::pendingCountChanged(int count)
{
    if (this->phase == Phase::Draining && count == 0)
        beginStop();
}

void DraftLeaseHandover::blocked(const QString &reason)
{
    // While stopping, a failure still releases the lease and ends in stopped().
    if (this->phase == Phase::Draining)
        fail(tr("Saving the queued edits failed, so the Commit was not started: %1").arg(reason), false);
}

bool DraftLeaseHandover::stopped(const QString &error)
{
    this->resuming = false;
    if (this->phase == Phase::Draining)
    {
        // Editing ended some other way before the queue was saved.
        fail(error.isEmpty() ? tr("Editing stopped before the queued edits were saved.")
                             : tr("Editing stopped before the queued edits were saved: %1").arg(error),
             false);
        return false;
    }
    if (this->phase != Phase::Stopping) return false;
    if (!error.isEmpty())
    {
        this->resume_draft_id.clear();
        this->resume_network_id.clear();
    }
    settle(error.isEmpty() ? QString() : tr("The editor could not release the draft: %1").arg(error));
    return true;
}

bool DraftLeaseHandover::isHandingOver() const
{
    return this->phase != Phase::Idle;
}

bool DraftLeaseHandover::isResuming() const
{
    return this->resuming;
}

void DraftLeaseHandover::clearResuming()
{
    this->resuming = false;
}

void DraftLeaseHandover::beginStop()
{
    this->phase = Phase::Stopping;
    this->editor.stop();
}

void DraftLeaseHandover::fail(const QString &message, bool unpause)
{
    this->resume_draft_id.clear();
    this->resume_network_id.clear();
    this->phase = Phase::Idle;
    if (unpause && this->editor.active() && !this->editor.blocked())
        this->editor.unpause();
    settle(message);
}

void DraftLeaseHandover::settle(const QString &error)
{
    this->timeout.stop();
    this->phase = Phase::Idle;
    const std::function<void(const QString &)> finished = std::move(this->done);
    this->done = {};
    if (finished) finished(error);
}
