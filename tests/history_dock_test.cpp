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
    // Expand a Commit without selecting a historical state. Archived MODEL
    // actions belong before the Commit marker, are loaded in explicit pages,
    // and never become locally invented Undo/Redo targets.
    int archived_page_requests = 0;
    int archived_last_offset = -1;
    QObject::connect(&dock, &HistoryDock::archivedActionPageRequested, &dock,
        [&archived_page_requests, &archived_last_offset](const QString &, int offset)
    {
        ++archived_page_requests;
        archived_last_offset = offset;
    });
    dock.setArchiveScope(QStringLiteral("personal:project:draft:main"));
    dock.setTimeline(context, {}, false, false);
    dock.setNavigationState(archived, true);
    // The marker is first for a collapsed Commit, followed by that Commit.
    emit list->itemClicked(list->item(1));
    if (!require(archived_page_requests == 1 && archived_last_offset == 0 &&
                 list->item(1)->text().contains(QStringLiteral("Loading")),
                 "opening a Commit did not request the first archived action page")) return 1;
    HistoryDockEntry a;
    a.kind = HistoryDockEntry::Kind::ArchivedAction;
    a.identity = QStringLiteral("018f84c0-0000-7000-8000-000000000011");
    a.title = QStringLiteral("Lay pipe");
    HistoryDockEntry b = a;
    b.identity = QStringLiteral("018f84c0-0000-7000-8000-000000000012");
    b.title = QStringLiteral("Change demand");
    dock.appendArchivedActionPage(commit.identity, 0, {a, b}, 2);
    if (!require(list->count() == 5 &&
                 list->item(0)->text().contains(QStringLiteral("Lay pipe")) &&
                 list->item(1)->text().contains(QStringLiteral("Archived action 1")) &&
                 list->item(2)->text().contains(QStringLiteral("Change demand")) &&
                 list->item(3)->text().contains(QStringLiteral("Load more")),
                 "archived actions are not ordered around the exact cursor")) return 1;
    // A stale or duplicated page must never silently overwrite the timeline.
    dock.appendArchivedActionPage(commit.identity, 0, {a}, -1);
    if (!require(list->count() == 5 && archived_page_requests == 1,
                 "unexpected archived page altered the verified timeline")) return 1;
    emit list->itemClicked(list->item(3));
    if (!require(archived_page_requests == 2 && archived_last_offset == 2,
                 "Load more did not use the next authoritative page offset")) return 1;
    HistoryDockEntry c = a;
    c.identity = QStringLiteral("018f84c0-0000-7000-8000-000000000013");
    c.title = QStringLiteral("Add pump");
    dock.appendArchivedActionPage(commit.identity, 2, {c}, -1);
    if (!require(list->count() == 5 &&
                 list->item(3)->text().contains(QStringLiteral("Add pump")) &&
                 list->item(4)->text().contains(QStringLiteral("Initial layout")),
                 "last archived page or Commit checkpoint ordering is incorrect")) return 1;
    // The next opened draft must not inherit another private archive's frames.
    dock.setArchiveScope(QStringLiteral("another:repository:draft:main"));
    if (!require(list->count() == 2 && !undo->isEnabled() && !redo->isEnabled(),
                 "changing private repository scope leaked archived actions")) return 1;
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
