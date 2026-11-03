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
                 notice->text().contains(QStringLiteral("not implemented")) &&
                 !undo->isEnabled() && !redo->isEnabled(),
                 "dock omitted real checkpoints or made placeholders interactive")) return 1;
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
