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
                 list->item(1)->text().contains(QStringLiteral("Archived action")),
                 "archived cursor marker or eligibility is wrong")) return 1;
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
