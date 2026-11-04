#include "app/history_dock.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QDockWidget>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>

#include <cstdio>

namespace
{
bool require(bool expression, const char *message)
{
    if (!expression) std::fprintf(stderr, "history_dock: %s\n", message);
    return expression;
}
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication application(argc, argv);
    QMainWindow window;
    HistoryDock dock(&window);
    window.addDockWidget(Qt::LeftDockWidgetArea, &dock);
    dock.hide();
    QPushButton *undo = dock.findChild<QPushButton *>(QStringLiteral("historyDockUndo"));
    QPushButton *redo = dock.findChild<QPushButton *>(QStringLiteral("historyDockRedo"));
    QListWidget *list = dock.findChild<QListWidget *>(QStringLiteral("historyDockTimeline"));
    QLabel *context_label = dock.findChild<QLabel *>(QStringLiteral("historyDockContext"));
    QLabel *notice = dock.findChild<QLabel *>(QStringLiteral("historyDockNotice"));
    if (!require(dock.objectName() == QStringLiteral("aowisHistoryDock") &&
                 window.dockWidgetArea(&dock) == Qt::LeftDockWidgetArea &&
                 undo != nullptr && redo != nullptr && list != nullptr &&
                 context_label != nullptr && notice != nullptr,
                 "dock missing expected left-side controls")) return 1;
    if (!require(!undo->isEnabled() && !redo->isEnabled() &&
                 list->selectionMode() == QAbstractItemView::NoSelection,
                 "Undo/Redo or history jumping accidentally enabled")) return 1;
    HistoryDockContext context;
    context.project = QStringLiteral("Distribution");
    context.branch = QStringLiteral("main");
    HistoryDockEntry commit;
    commit.kind = HistoryDockEntry::Kind::Commit;
    commit.title = QStringLiteral("Initial layout");
    commit.identity = QStringLiteral("018f84c0-0000-7000-8000-000000000001");
    context.commits.append(commit);
    dock.setContext(context);
    HistoryDockEntry saved;
    saved.kind = HistoryDockEntry::Kind::SavedAction;
    saved.title = QStringLiteral("Add junction");
    HistoryDockEntry queued;
    queued.kind = HistoryDockEntry::Kind::PendingAction;
    queued.title = QStringLiteral("Change diameter");
    dock.setDraftActions({saved, queued}, true);
    if (!require(list->count() == 4 &&
                 list->item(0)->text().contains(QStringLiteral("Initial layout")) &&
                 list->item(1)->text().contains(QStringLiteral("Add junction")) &&
                 list->item(2)->text().contains(QStringLiteral("Change diameter")) &&
                 list->item(3)->text().contains(QStringLiteral("Current private working state")) &&
                 context_label->text().contains(QStringLiteral("main")) &&
                 notice->text().contains(QStringLiteral("unavailable")) &&
                 !undo->isEnabled() && !redo->isEnabled(),
                 "dock enabled navigation without an authoritative cursor")) return 1;
    // Only DVMDB's explicit adjacent coordinates enable these controls.
    int undo_requested = 0;
    int redo_requested = 0;
    QObject::connect(&dock, &HistoryDock::undoRequested, &dock,
                     [&undo_requested] { ++undo_requested; });
    QObject::connect(&dock, &HistoryDock::redoRequested, &dock,
                     [&redo_requested] { ++redo_requested; });
    const QJsonObject cursor{
        {QStringLiteral("generation"), QStringLiteral("7")},
        {QStringLiteral("selectionKind"), QStringLiteral("draft")},
        {QStringLiteral("actionPosition"), QStringLiteral("1")},
        {QStringLiteral("journalCount"), QStringLiteral("2")},
        {QStringLiteral("previousAction"), QJsonObject{
            {QStringLiteral("selectionKind"), QStringLiteral("draft")},
            {QStringLiteral("actionPosition"), QStringLiteral("0")}}},
        {QStringLiteral("nextAction"), QJsonObject{
            {QStringLiteral("selectionKind"), QStringLiteral("draft")},
            {QStringLiteral("actionPosition"), QStringLiteral("2")}}}};
    dock.setNavigationState(cursor, true);
    if (!require(undo->isEnabled() && redo->isEnabled() && list->count() == 4 &&
                 list->item(2)->text().contains(QStringLiteral("Selected draft position 1")),
                 "dock ignored server cursor or misplaced the selected action")) return 1;
    undo->click();
    redo->click();
    if (!require(undo_requested == 1 && redo_requested == 1,
                 "enabled History controls did not request the adjacent actions")) return 1;
    dock.setNavigationState(cursor, false);
    undo->click();
    redo->click();
    if (!require(!undo->isEnabled() && !redo->isEnabled() &&
                 undo_requested == 1 && redo_requested == 1,
                 "unavailable History navigation emitted an action")) return 1;
    const QJsonObject archived{
        {QStringLiteral("selectionKind"), QStringLiteral("archived")},
        {QStringLiteral("archivedRevisionId"), commit.identity},
        {QStringLiteral("archivedActionPosition"), QStringLiteral("1")},
        {QStringLiteral("previousAction"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("nextAction"), QJsonValue(QJsonValue::Null)}};
    dock.setNavigationState(archived, true);
    if (!require(!undo->isEnabled() && !redo->isEnabled() &&
                 list->item(0)->text().contains(QStringLiteral("Archived action")),
                 "archived cursor marker or eligibility is wrong")) return 1;
    // Commit rows now select verified checkpoints, and their disclosure rows
    // load the archived action descriptions without changing the draft.
    int archived_page_requests = 0;
    int archived_last_offset = -1;
    int jumps = 0;
    QJsonObject last_jump;
    QObject::connect(&dock, &HistoryDock::jumpRequested, &dock,
        [&jumps, &last_jump](const QJsonObject &coordinate)
    {
        ++jumps;
        last_jump = coordinate;
    });
    QObject::connect(&dock, &HistoryDock::archivedActionPageRequested, &dock,
        [&archived_page_requests, &archived_last_offset](const QString &, int offset)
    {
        ++archived_page_requests;
        archived_last_offset = offset;
    });
    dock.setArchiveScope(QStringLiteral("personal:project:draft:main"));
    dock.setTimeline(context, {}, false, false);
    dock.setNavigationState(archived, true);
    // Initially: selected position, disclosure, Commit checkpoint. One
    // click on the Commit fetches only the first bounded metadata page.
    if (!require(list->count() == 3 &&
                 list->item(2)->text().contains(QStringLiteral("Initial layout")),
                 "Commit disclosure/checkpoint rows were not separated")) return 1;
    emit list->itemClicked(list->item(2));
    if (!require(archived_page_requests == 1 && archived_last_offset == 0 &&
                 jumps == 0 && list->item(1)->text().contains(QStringLiteral("Loading")),
                 "Commit checkpoint did not request verified archive metadata")) return 1;
    HistoryDockEntry a;
    a.kind = HistoryDockEntry::Kind::ArchivedAction;
    a.identity = QStringLiteral("018f84c0-0000-7000-8000-000000000011");
    a.title = QStringLiteral("Lay pipe");
    HistoryDockEntry b = a;
    b.identity = QStringLiteral("018f84c0-0000-7000-8000-000000000012");
    b.title = QStringLiteral("Change demand");
    dock.appendArchivedActionPage(commit.identity, 0, {a, b}, 2, 3);
    if (!require(jumps == 1 &&
                 last_jump.value(QStringLiteral("selectionKind")).toString() ==
                     QStringLiteral("archived") &&
                 last_jump.value(QStringLiteral("revisionId")).toString() == commit.identity &&
                 last_jump.value(QStringLiteral("actionPosition")).toString() == QStringLiteral("3"),
                 "Commit jump did not use the authenticated total action count")) return 1;
    if (!require(list->count() == 6 &&
                 list->item(0)->text().contains(QStringLiteral("Lay pipe")) &&
                 list->item(1)->text().contains(QStringLiteral("Archived action 1")) &&
                 list->item(2)->text().contains(QStringLiteral("Change demand")) &&
                 list->item(3)->text().contains(QStringLiteral("Load more")),
                 "archived actions are not ordered around the exact cursor")) return 1;
    // A stale or duplicated page must not change the loaded archive.
    dock.appendArchivedActionPage(commit.identity, 0, {a}, -1, 1);
    if (!require(list->count() == 6 && jumps == 1 && archived_page_requests == 1,
                 "unexpected archived page changed the verified timeline")) return 1;
    emit list->itemClicked(list->item(3));
    if (!require(archived_page_requests == 2 && archived_last_offset == 2,
                 "Load more did not use the next authoritative offset")) return 1;
    HistoryDockEntry c = a;
    c.identity = QStringLiteral("018f84c0-0000-7000-8000-000000000013");
    c.title = QStringLiteral("Add pump");
    dock.appendArchivedActionPage(commit.identity, 2, {c}, -1, 3);
    if (!require(list->count() == 6 &&
                 list->item(3)->text().contains(QStringLiteral("Add pump")) &&
                 list->item(5)->text().contains(QStringLiteral("Initial layout")),
                 "final archive page or Commit checkpoint ordering is incorrect")) return 1;
    // A click on an original archived action chooses the action state; a
    // click on the disclosure row still only collapses the display.
    emit list->itemClicked(list->item(2));
    if (!require(jumps == 2 &&
                 last_jump.value(QStringLiteral("actionPosition")).toString() == QStringLiteral("2"),
                 "archived action row did not select its exact position")) return 1;
    emit list->itemClicked(list->item(4));
    if (!require(jumps == 2 && list->count() == 3,
                 "disclosure click navigated the model or failed to collapse")) return 1;
    emit list->itemClicked(list->item(2));
    if (!require(jumps == 3 &&
                 last_jump.value(QStringLiteral("actionPosition")).toString() == QStringLiteral("3"),
                 "checkpoint click did not select the archive tip")) return 1;
    dock.setNavigationState(archived, false);
    emit list->itemClicked(list->item(2));
    if (!require(jumps == 3, "blocked History still emitted a direct jump")) return 1;
    // A real saved draft action can be selected, but pending events cannot.
    dock.setNavigationState(cursor, true);
    dock.setDraftActions({saved, queued}, true);
    QJsonObject base_cursor = cursor;
    base_cursor.insert(QStringLiteral("actionPosition"), QStringLiteral("0"));
    dock.setNavigationState(base_cursor, true);
    // Collapsed Commit + disclosure precede saved and pending rows.
    emit list->itemClicked(list->item(3));
    if (!require(jumps == 4 &&
                 last_jump.value(QStringLiteral("selectionKind")).toString() ==
                     QStringLiteral("draft") &&
                 last_jump.value(QStringLiteral("actionPosition")).toString() == QStringLiteral("1"),
                 "saved draft action did not select its exact journal position")) return 1;
    emit list->itemClicked(list->item(4));
    if (!require(jumps == 4, "pending Save was exposed as navigable history")) return 1;
    dock.setDraftActions({}, false);
    // The next opened draft must not inherit another private archive's frames.
    dock.setArchiveScope(QStringLiteral("another:repository:draft:main"));
    if (!require(list->count() == 3 && jumps == 4,
                 "changing private repository scope leaked archived actions")) return 1;
    // An in-flight checkpoint lookup must not jump after a generation change.
    emit list->itemClicked(list->item(1));
    if (!require(archived_page_requests == 3 && archived_last_offset == 0,
                 "fresh scope did not request checkpoint metadata")) return 1;
    QJsonObject newer_cursor = base_cursor;
    newer_cursor.insert(QStringLiteral("generation"), QStringLiteral("8"));
    dock.setNavigationState(newer_cursor, true);
    dock.appendArchivedActionPage(commit.identity, 0, {a}, -1, 1);
    if (!require(jumps == 4,
                 "a checkpoint lookup jumped after its generation became stale")) return 1;
    // No archived MODEL actions means a snapshot-only checkpoint is not a
    // fabricated executable coordinate.
    dock.setArchiveScope(QStringLiteral("checkpoint-only:repository:draft:main"));
    emit list->itemClicked(list->item(1));
    dock.appendArchivedActionPage(commit.identity, 0, {}, -1, 0);
    emit list->itemClicked(list->item(list->count() - 2));
    if (!require(jumps == 4 && notice->text().contains(QStringLiteral("checkpoint")),
                 "snapshot-only checkpoint was offered as a jump")) return 1;
    // An old response must not populate the newly opened private scope.
    dock.setArchiveScope(QStringLiteral("third:repository:draft:main"));
    dock.appendArchivedActionPage(commit.identity, 0, {a}, -1, 1);
    if (!require(jumps == 4 && list->count() == 3,
                 "old archived response leaked into a newly opened private draft")) return 1;
    window.show();
    dock.show();
    application.processEvents();
    if (!require(!dock.isHidden(), "dock cannot reopen after being hidden")) return 1;
    dock.clearDraftActions();
    if (!require(list->count() == 2 && !undo->isEnabled() && !redo->isEnabled(),
                 "closing draft left stale actions in History")) return 1;
    HistoryDockContext unborn;
    unborn.project = QStringLiteral("Distribution");
    unborn.branch = QStringLiteral("new-branch");
    dock.setContext(unborn);
    if (!require(notice->text().contains(QStringLiteral("No commits yet")) &&
                 list->count() == 1, "unborn branch incorrectly displays commits")) return 1;
    return 0;
}
