// Writer-lease handover from the open editor to a Commit and back
// (DraftLeaseHandover): queue drained before the lease is released, editing
// reopened afterwards, nothing handed over while blocked, closing or opening,
// failures leave editing running, and every done() runs exactly once.
#include "projects/draft_lease_handover.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>

#include <cstdio>
#include <functional>
#include <memory>

namespace
{
int failures = 0;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "draft_lease_handover: %s\n", message);
        ++failures;
    }
}

const QString draft = QStringLiteral("018f84c0-0000-7000-8000-0000000000d1");
const QString network = QStringLiteral("018f84c0-0000-7000-8000-0000000000e1");

// A scripted editor: what the handover may observe and what it asked for.
struct FakeEditor
{
    bool active = true;
    bool ready = true;
    bool blocked = false;
    bool closing = false;
    int pending = 0;
    bool paused = false;
    QStringList calls; // pause, unpause, stop, reopen

    DraftLeaseHandover::Editor bind()
    {
        DraftLeaseHandover::Editor editor;
        editor.active = [this] { return this->active; };
        editor.ready = [this] { return this->ready; };
        editor.blocked = [this] { return this->blocked; };
        editor.closing = [this] { return this->closing; };
        editor.draft_id = [this] { return this->active ? draft : QString(); };
        editor.network_id = [this] { return this->active ? network : QString(); };
        editor.pending = [this] { return this->pending; };
        editor.pause = [this] { this->paused = true; this->calls.append(QStringLiteral("pause")); };
        editor.unpause = [this] { this->paused = false; this->calls.append(QStringLiteral("unpause")); };
        editor.stop = [this] { this->closing = true; this->calls.append(QStringLiteral("stop")); };
        editor.reopen = [this](const QString &draft_id, const QString &network_id)
        {
            this->calls.append(QStringLiteral("reopen %1 %2").arg(draft_id, network_id));
        };
        return editor;
    }
    // The session finished stopping (MainWindow's stopped() handler).
    void finishStop() { this->active = false; this->closing = false; this->paused = false; }
};

struct Result
{
    int calls = 0;
    QString error;
};

std::function<void(const QString &)> recorder(const std::shared_ptr<Result> &result)
{
    return [result](const QString &error)
    {
        ++result->calls;
        result->error = error;
    };
}

void wait(int ms)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void testNotEditingThatDraft()
{
    FakeEditor editor;
    DraftLeaseHandover handover(editor.bind());
    std::shared_ptr<Result> other = std::make_shared<Result>();
    handover.release(QStringLiteral("018f84c0-0000-7000-8000-0000000000d9"), recorder(other));
    require(other->calls == 1 && other->error.isEmpty() && editor.calls.isEmpty(),
            "another draft's Commit touched the editor");
    editor.active = false;
    std::shared_ptr<Result> idle = std::make_shared<Result>();
    handover.release(draft, recorder(idle));
    require(idle->calls == 1 && idle->error.isEmpty() && editor.calls.isEmpty() && !handover.isHandingOver(),
            "a Commit without an open editor did not go ahead at once");
    handover.resume(draft);
    require(editor.calls.isEmpty(), "editing was reopened although nothing was handed over");
}

void testHandoverWithEmptyQueue()
{
    FakeEditor editor;
    DraftLeaseHandover handover(editor.bind());
    std::shared_ptr<Result> result = std::make_shared<Result>();
    handover.release(draft, recorder(result));
    require(editor.calls == QStringList({QStringLiteral("pause"), QStringLiteral("stop")}),
            "an empty queue was not paused and stopped at once");
    require(result->calls == 0 && handover.isHandingOver(),
            "the Commit went ahead before the editor released the lease");
    editor.finishStop();
    require(handover.stopped({}), "the handover's own stop was not recognized");
    require(result->calls == 1 && result->error.isEmpty() && !handover.isHandingOver(),
            "the released lease was not handed to the Commit");
    handover.resume(draft);
    require(editor.calls.last() == QStringLiteral("reopen %1 %2").arg(draft, network)
                && handover.isResuming(),
            "editing was not reopened on the same draft and network after the Commit");
    handover.clearResuming();
    handover.resume(draft);
    require(editor.calls.count() == 3, "editing was reopened twice");
}

void testQueueDrainsFirst()
{
    FakeEditor editor;
    editor.pending = 3;
    DraftLeaseHandover handover(editor.bind());
    std::shared_ptr<Result> result = std::make_shared<Result>();
    handover.release(draft, recorder(result));
    require(editor.calls == QStringList({QStringLiteral("pause")}) && editor.paused,
            "capture was not paused while queued edits remain");
    handover.pendingCountChanged(1);
    require(!editor.calls.contains(QStringLiteral("stop")), "the editor stopped before its queue was saved");
    handover.pendingCountChanged(0);
    require(editor.calls.last() == QStringLiteral("stop"), "the editor was not stopped once its queue was saved");
    editor.finishStop();
    require(handover.stopped({}) && result->calls == 1 && result->error.isEmpty(),
            "the drained handover did not complete");
}

void testRefusals()
{
    struct RefusalCase
    {
        const char *message;
        std::function<void(FakeEditor &)> set;
        const char *expected;
    };
    const RefusalCase cases[] = {
        {"blocked editor", [](FakeEditor &e) { e.blocked = true; }, "blocked"},
        {"closing editor", [](FakeEditor &e) { e.closing = true; }, "closing"},
        {"opening editor", [](FakeEditor &e) { e.ready = false; }, "being opened"},
    };
    for (const RefusalCase &entry : cases)
    {
        FakeEditor editor;
        entry.set(editor);
        DraftLeaseHandover handover(editor.bind());
        std::shared_ptr<Result> result = std::make_shared<Result>();
        handover.release(draft, recorder(result));
        require(result->calls == 1 && result->error.contains(QLatin1String(entry.expected))
                    && editor.calls.isEmpty() && !handover.isHandingOver(),
                entry.message);
    }
    // One handover at a time.
    FakeEditor editor;
    editor.pending = 1;
    DraftLeaseHandover handover(editor.bind());
    std::shared_ptr<Result> first = std::make_shared<Result>();
    std::shared_ptr<Result> second = std::make_shared<Result>();
    handover.release(draft, recorder(first));
    handover.release(draft, recorder(second));
    require(first->calls == 0 && second->calls == 1 && !second->error.isEmpty(),
            "a second Commit was accepted during a handover");
}

void testFailureWhileDraining()
{
    // Saving the queue fails: no lease changes hands; the editor's own
    // block handling applies (no unpause), and nothing is reopened.
    FakeEditor editor;
    editor.pending = 2;
    DraftLeaseHandover handover(editor.bind());
    std::shared_ptr<Result> result = std::make_shared<Result>();
    handover.release(draft, recorder(result));
    editor.blocked = true;
    handover.blocked(QStringLiteral("HTTP 500"));
    require(result->calls == 1 && result->error.contains(QStringLiteral("HTTP 500"))
                && !editor.calls.contains(QStringLiteral("stop")) && !editor.calls.contains(QStringLiteral("unpause"))
                && !handover.isHandingOver(),
            "a failed save did not cancel the handover");
    handover.resume(draft);
    require(!editor.calls.last().startsWith(QStringLiteral("reopen")), "a failed handover reopened editing");

    // Editing ended some other way before the queue was saved.
    FakeEditor other;
    other.pending = 1;
    DraftLeaseHandover closing(other.bind());
    std::shared_ptr<Result> closed = std::make_shared<Result>();
    closing.release(draft, recorder(closed));
    other.finishStop();
    require(!closing.stopped({}) && closed->calls == 1 && !closed->error.isEmpty(),
            "a stop that was not the handover's was claimed by it");
}

void testDrainTimeoutUnpauses()
{
    FakeEditor editor;
    editor.pending = 1;
    DraftLeaseHandover handover(editor.bind(), nullptr, 30);
    std::shared_ptr<Result> result = std::make_shared<Result>();
    handover.release(draft, recorder(result));
    wait(120);
    require(result->calls == 1 && !result->error.isEmpty()
                && editor.calls == QStringList({QStringLiteral("pause"), QStringLiteral("unpause")})
                && !editor.paused && !handover.isHandingOver(),
            "a queue that did not drain did not resume editing and report the Commit as not started");
    handover.pendingCountChanged(0);
    require(!editor.calls.contains(QStringLiteral("stop")) && result->calls == 1,
            "a late save stopped the editor after the handover gave up");
}

void testStopFailures()
{
    // The release reported an error: the Commit is told, editing is not
    // reopened (the lease state is unknown).
    FakeEditor editor;
    DraftLeaseHandover handover(editor.bind());
    std::shared_ptr<Result> result = std::make_shared<Result>();
    handover.release(draft, recorder(result));
    editor.finishStop();
    require(handover.stopped(QStringLiteral("release refused")) && result->calls == 1
                && result->error.contains(QStringLiteral("release refused")),
            "a failed lease release was not reported to the Commit");
    handover.resume(draft);
    require(!editor.calls.last().startsWith(QStringLiteral("reopen")), "editing was reopened after a failed release");

    // A failure while stopping still ends in stopped(); it must not settle early.
    FakeEditor renewing;
    DraftLeaseHandover stopping(renewing.bind());
    std::shared_ptr<Result> later = std::make_shared<Result>();
    stopping.release(draft, recorder(later));
    stopping.blocked(QStringLiteral("renewal failed"));
    require(later->calls == 0 && stopping.isHandingOver(), "a failure while stopping settled the handover early");
    renewing.finishStop();
    require(stopping.stopped({}) && later->calls == 1 && later->error.isEmpty(),
            "the lease released after a failure was not handed over");

    // The session never reports back: the Commit is not left waiting.
    FakeEditor silent;
    DraftLeaseHandover waiting(silent.bind(), nullptr, 30);
    std::shared_ptr<Result> timed_out = std::make_shared<Result>();
    waiting.release(draft, recorder(timed_out));
    wait(120);
    require(timed_out->calls == 1 && !timed_out->error.isEmpty() && !waiting.isHandingOver(),
            "a stop that never finished left the Commit waiting");
    silent.finishStop();
    require(!waiting.stopped({}) && timed_out->calls == 1, "a late stop called done() twice");
}

void testReopenedMeanwhile()
{
    FakeEditor editor;
    DraftLeaseHandover handover(editor.bind());
    std::shared_ptr<Result> result = std::make_shared<Result>();
    handover.release(draft, recorder(result));
    editor.finishStop();
    handover.stopped({});
    editor.active = true; // the user opened another network during the Commit
    const int before = editor.calls.size();
    handover.resume(draft);
    require(editor.calls.size() == before && !handover.isResuming(),
            "resume replaced a network opened during the Commit");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testNotEditingThatDraft();
    testHandoverWithEmptyQueue();
    testQueueDrainsFirst();
    testRefusals();
    testFailureWhileDraining();
    testDrainTimeoutUnpauses();
    testStopFailures();
    testReopenedMeanwhile();
    if (failures != 0)
    {
        std::fprintf(stderr, "%d draft lease handover check(s) failed\n", failures);
        return 1;
    }
    std::puts("Draft lease handover tests passed");
    return 0;
}
