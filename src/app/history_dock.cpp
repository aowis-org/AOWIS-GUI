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
    this->undo_button_->setEnabled(false);
    this->redo_button_->setEnabled(false);
    this->undo_button_->setToolTip(tr("Undo one verified action in the private working copy."));
    this->redo_button_->setToolTip(tr("Redo one verified action in the private working copy."));
    connect(this->undo_button_, &QPushButton::clicked, this, [this]
    {
        if (this->navigation_available_ && this->undo_button_->isEnabled())
            emit undoRequested();
    });
    connect(this->redo_button_, &QPushButton::clicked, this, [this]
    {
        if (this->navigation_available_ && this->redo_button_->isEnabled())
            emit redoRequested();
    });
    actions->addWidget(this->undo_button_);
    actions->addWidget(this->redo_button_);
    layout->addLayout(actions);

    this->timeline_ = new QListWidget(content);
    this->timeline_->setObjectName(QStringLiteral("historyDockTimeline"));
    this->timeline_->setSelectionMode(QAbstractItemView::NoSelection);
    this->timeline_->setFocusPolicy(Qt::NoFocus);
    this->timeline_->setToolTip(tr("Commit markers and draft events. Undo/Redo follows the authoritative DVMDB cursor."));
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
    this->navigation_cursor_ = {};
    this->navigation_available_ = false;
    this->navigation_unavailability_reason_.clear();
    rebuild();
}

void HistoryDock::setNavigationState(const QJsonObject &cursor, bool available,
                                     const QString &unavailable_reason)
{
    this->navigation_cursor_ = cursor;
    this->navigation_available_ = available;
    this->navigation_unavailability_reason_ = unavailable_reason;
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
        const QString kind = this->navigation_cursor_.value(QStringLiteral("selectionKind")).toString();
        const bool live = kind == QStringLiteral("draft");
        bool valid_position = false;
        const int position = this->navigation_cursor_.value(
            QStringLiteral("actionPosition")).toString().toInt(&valid_position);
        const QString revision = this->navigation_cursor_.value(
            QStringLiteral("archivedRevisionId")).toString();
        QString marker = this->blocked_ ? tr("▶ Private working state — editing blocked") :
            this->editing_ ? tr("▶ Current private working state") :
                this->context_.commits.isEmpty() && !this->context_.incomplete_ancestry
                    ? tr("▶ Current unborn HEAD") : tr("▶ Current branch HEAD");
        int insert_position = this->timeline_->count();
        if (live && valid_position && position >= 0 && position <= this->actions_.size())
        {
            marker = tr("▶ Selected draft position %1").arg(position);
            insert_position = this->context_.commits.size() + position;
        }
        else if (kind == QStringLiteral("archived") && !revision.isEmpty())
        {
            marker = tr("▶ Archived action %1 in %2")
                .arg(this->navigation_cursor_.value(QStringLiteral("archivedActionPosition")).toString(),
                     revision.left(8));
            for (int index = 0; index < this->context_.commits.size(); ++index)
                if (this->context_.commits.at(index).identity == revision)
                {
                    insert_position = index + 1;
                    break;
                }
        }
        QListWidgetItem *current = new QListWidgetItem(marker);
        this->timeline_->insertItem(insert_position, current);
        QFont font = current->font();
        font.setBold(true);
        current->setFont(font);
        current->setToolTip(this->navigation_cursor_.isEmpty()
            ? tr("Open a verified private working copy to navigate History.")
            : tr("Position reported by the private DVMDB server."));
    }
    else if (this->timeline_->count() == 0)
        new QListWidgetItem(tr("No branch history selected"), this->timeline_);

    if (follow_latest) this->timeline_->scrollToBottom();
    else this->timeline_->verticalScrollBar()->setValue(previous_position);

    const bool undo = this->navigation_available_ &&
        this->navigation_cursor_.value(QStringLiteral("previousAction")).isObject();
    const bool redo = this->navigation_available_ &&
        this->navigation_cursor_.value(QStringLiteral("nextAction")).isObject();
    this->undo_button_->setEnabled(undo);
    this->redo_button_->setEnabled(redo);
    const QString unavailable = this->navigation_unavailability_reason_.isEmpty()
        ? tr("Open a ready private draft with a verified History cursor.")
        : this->navigation_unavailability_reason_;
    this->undo_button_->setToolTip(undo ? tr("Undo one DVMDB-verified action.") : unavailable);
    this->redo_button_->setToolTip(redo ? tr("Redo one DVMDB-verified action.") : unavailable);
    QString notice = tr("◆ Commit · ● Saved edit · ◌ Pending Save. ");
    notice += this->navigation_available_
        ? tr("Undo/Redo uses verified server History. Actions are selected atomically.")
        : tr("Undo/Redo unavailable: %1").arg(unavailable);
    if (this->blocked_)
        notice += tr(" Private editing is blocked; check the network status before making changes.");
    if (this->context_.incomplete_ancestry)
        notice += tr(" Some committed ancestors are unavailable in the loaded graph.");
    if (this->context_.commits.isEmpty() && has_project && !this->context_.branch.isEmpty())
        notice += tr(" No commits yet on this branch.");
    if (!this->context_.commits.isEmpty())
        notice += tr(" Committed actions are navigable when verified, but this list currently shows Commit markers rather than expanding archived frames.");
    if (this->editing_)
        notice += tr(" Draft actions appear after commit checkpoints; pending actions are marked ◌.");
    this->notice_label_->setText(notice);
}
