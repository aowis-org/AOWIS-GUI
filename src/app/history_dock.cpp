#include "app/history_dock.h"

#include <QAbstractItemView>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QScrollBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

HistoryDock::HistoryDock(QWidget *parent) : QDockWidget(tr("History"), parent)
{
    setObjectName(QStringLiteral("aowisHistoryDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    setMinimumWidth(220);

    QWidget *content = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(9);
    this->context_label_ = new QLabel(content);
    this->context_label_->setObjectName(QStringLiteral("historyDockContext"));
    this->context_label_->setWordWrap(true);
    layout->addWidget(this->context_label_);

    QHBoxLayout *actions = new QHBoxLayout();
    this->undo_button_ = new QPushButton(tr("Undo"), content);
    this->undo_button_->setObjectName(QStringLiteral("historyDockUndo"));
    this->redo_button_ = new QPushButton(tr("Redo"), content);
    this->redo_button_->setObjectName(QStringLiteral("historyDockRedo"));
    // These are placeholders, not an in-memory imitation of the missing
    // journal/cursor and not controls that silently move immutable HEADs.
    this->undo_button_->setEnabled(false);
    this->redo_button_->setEnabled(false);
    this->undo_button_->setToolTip(tr("Undo requires the persistent history controller (HIST-01–03)."));
    this->redo_button_->setToolTip(tr("Redo requires the persistent history controller (HIST-01–03)."));
    actions->addWidget(this->undo_button_);
    actions->addWidget(this->redo_button_);
    layout->addLayout(actions);

    this->timeline_ = new QListWidget(content);
    this->timeline_->setObjectName(QStringLiteral("historyDockTimeline"));
    this->timeline_->setSelectionMode(QAbstractItemView::NoSelection);
    this->timeline_->setFocusPolicy(Qt::NoFocus);
    this->timeline_->setToolTip(tr("Read-only timeline. Navigation becomes available with the history subsystem."));
    layout->addWidget(this->timeline_, 1);

    this->notice_label_ = new QLabel(content);
    this->notice_label_->setObjectName(QStringLiteral("historyDockNotice"));
    this->notice_label_->setWordWrap(true);
    layout->addWidget(this->notice_label_);
    setWidget(content);
    rebuild();
}

void HistoryDock::setContext(const HistoryDockContext &context)
{
    this->context_ = context;
    rebuild();
}

void HistoryDock::setDraftActions(const QList<HistoryDockEntry> &actions, bool editing)
{
    this->actions_ = actions;
    this->editing_ = editing;
    rebuild();
}

void HistoryDock::setTimeline(const HistoryDockContext &context,
                              const QList<HistoryDockEntry> &actions,
                              bool editing, bool blocked)
{
    this->context_ = context;
    this->actions_ = actions;
    this->editing_ = editing;
    this->blocked_ = blocked;
    rebuild();
}

void HistoryDock::clearDraftActions()
{
    this->actions_.clear();
    this->editing_ = false;
    this->blocked_ = false;
    rebuild();
}

void HistoryDock::rebuild()
{
    if (this->timeline_ == nullptr) return;
    const bool has_project = !this->context_.project.isEmpty();
    this->context_label_->setText(!has_project ? tr("Select a project and branch") :
        this->context_.branch.isEmpty() ? this->context_.project + tr(" — select a branch") :
        this->context_.project + QStringLiteral(" / ") + this->context_.branch);

    const int previous_position = this->timeline_->verticalScrollBar()->value();
    const bool follow_latest = previous_position >=
        this->timeline_->verticalScrollBar()->maximum() - 1;
    this->timeline_->clear();
    const QList<HistoryDockEntry> timeline = this->context_.commits + this->actions_;
    for (const HistoryDockEntry &entry : timeline)
    {
        QString prefix;
        switch (entry.kind)
        {
        case HistoryDockEntry::Kind::Commit: prefix = QStringLiteral("◆ "); break;
        case HistoryDockEntry::Kind::SavedAction: prefix = QStringLiteral("● "); break;
        case HistoryDockEntry::Kind::PendingAction: prefix = QStringLiteral("◌ "); break;
        }
        QListWidgetItem *item = new QListWidgetItem(prefix + entry.title, this->timeline_);
        item->setData(Qt::UserRole, entry.identity);
        item->setToolTip(entry.details.isEmpty() ? entry.identity : entry.details);
        if (entry.kind == HistoryDockEntry::Kind::Commit)
        {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
    }
    if (has_project && !this->context_.branch.isEmpty())
    {
        QListWidgetItem *current = new QListWidgetItem(
            this->blocked_ ? tr("▶ Private working state — editing blocked") :
                this->editing_ ? tr("▶ Current private working state") :
                    this->context_.commits.isEmpty() && !this->context_.incomplete_ancestry
                        ? tr("▶ Current unborn HEAD") : tr("▶ Current branch HEAD"),
            this->timeline_);
        QFont font = current->font();
        font.setBold(true);
        current->setFont(font);
        current->setToolTip(tr("This marker is informational until history navigation is implemented."));
    }
    else if (this->timeline_->count() == 0)
        new QListWidgetItem(tr("No branch history selected"), this->timeline_);

    if (follow_latest) this->timeline_->scrollToBottom();
    else this->timeline_->verticalScrollBar()->setValue(previous_position);

    QString notice = tr("◆ Commit · ● Saved edit · ◌ Pending Save. "
                        "Undo/Redo and history navigation are not implemented yet.");
    if (this->blocked_)
        notice += tr(" Private editing is blocked; check the network status before making changes.");
    if (this->context_.incomplete_ancestry)
        notice += tr(" Some committed ancestors are unavailable in the loaded graph.");
    if (this->context_.commits.isEmpty() && has_project && !this->context_.branch.isEmpty())
        notice += tr(" No commits yet on this branch.");
    if (!this->context_.commits.isEmpty())
        notice += tr(" Individual edits preceding Commit are not retained in the current history API.");
    if (this->editing_)
        notice += tr(" Draft actions appear after commit checkpoints; pending actions are marked ◌.");
    this->notice_label_->setText(notice);
}
