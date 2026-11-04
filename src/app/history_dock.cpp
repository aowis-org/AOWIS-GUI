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

namespace
{
bool isCurrentHistoryPosition(const QJsonObject &cursor, const QJsonObject &target)
{
    const QString kind = target.value(QStringLiteral("selectionKind")).toString();
    if (cursor.value(QStringLiteral("selectionKind")).toString() != kind)
        return false;
    if (kind == QStringLiteral("draft"))
        return cursor.value(QStringLiteral("actionPosition")) ==
            target.value(QStringLiteral("actionPosition"));
    return kind == QStringLiteral("archived") &&
        cursor.value(QStringLiteral("archivedRevisionId")).toString() ==
            target.value(QStringLiteral("revisionId")).toString() &&
        cursor.value(QStringLiteral("archivedActionPosition")) ==
            target.value(QStringLiteral("actionPosition"));
}
}

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
    this->timeline_->setToolTip(tr("Click a saved action to jump to it, or a Commit to select its checkpoint. Use the disclosure row to expand archived actions."));
    connect(this->timeline_, &QListWidget::itemClicked,
            this, &HistoryDock::handleTimelineClick);
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
    if (this->context_.project != context.project ||
        this->context_.branch != context.branch)
        this->archived_pages_.clear();
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
    if (this->context_.project != context.project ||
        this->context_.branch != context.branch)
        this->archived_pages_.clear();
    this->context_ = context;
    this->actions_ = actions;
    this->editing_ = editing;
    this->blocked_ = blocked;
    rebuild();
}

void HistoryDock::clearDraftActions()
{
    this->actions_.clear();
    this->archived_pages_.clear();
    this->archive_scope_.clear();
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
    if (!available || cursor.value(QStringLiteral("generation")) !=
                      this->navigation_cursor_.value(QStringLiteral("generation")))
    {
        for (ArchivedPage &page : this->archived_pages_)
            page.pending_checkpoint_jump = false;
    }
    this->navigation_cursor_ = cursor;
    this->navigation_available_ = available;
    this->navigation_unavailability_reason_ = unavailable_reason;
    rebuild();
}

void HistoryDock::setArchiveScope(const QString &scope)
{
    if (this->archive_scope_ == scope) return;
    this->archive_scope_ = scope;
    this->archived_pages_.clear();
    rebuild();
}

void HistoryDock::appendArchivedActionPage(
    const QString &revision, int offset, const QList<HistoryDockEntry> &entries,
    int next_offset, int total_count)
{
    if (!this->archived_pages_.contains(revision)) return;
    ArchivedPage &page = this->archived_pages_[revision];
    // An old completion after a collapsed/retried load must not invalidate
    // already authenticated pages or start a second navigation.
    if (!page.loading) return;
    if (offset != page.actions.size() || total_count < 0 ||
        (page.total_count >= 0 && page.total_count != total_count) ||
        offset + entries.size() > total_count ||
        (next_offset >= 0 && (next_offset <= offset ||
                              next_offset != offset + entries.size() ||
                              next_offset > total_count)) ||
        (next_offset < 0 && offset + entries.size() != total_count))
    {
        page.loading = false;
        page.pending_checkpoint_jump = false;
        page.total_count = -1;
        page.error = tr("Archived History page did not match the requested cursor.");
        rebuild();
        return;
    }
    QSet<QString> seen;
    for (const HistoryDockEntry &prior : page.actions) seen.insert(prior.identity);
    for (const HistoryDockEntry &entry : entries)
    {
        if (entry.identity.isEmpty() || seen.contains(entry.identity) ||
            entry.kind != HistoryDockEntry::Kind::ArchivedAction)
        {
            page.loading = false;
            page.pending_checkpoint_jump = false;
            page.total_count = -1;
            page.error = tr("Archived History returned duplicate or invalid actions.");
            rebuild();
            return;
        }
        seen.insert(entry.identity);
    }
    page.actions.append(entries);
    page.next_offset = next_offset;
    page.total_count = total_count;
    page.complete = next_offset < 0;
    page.loading = false;
    page.error.clear();
    const bool jump = page.pending_checkpoint_jump && this->navigation_available_ &&
        this->navigation_cursor_.value(QStringLiteral("generation")).toString() ==
            page.pending_generation && total_count > 0 &&
        !this->archive_scope_.isEmpty();
    page.pending_checkpoint_jump = false;
    rebuild();
    if (jump)
    {
        const QJsonObject coordinate{{QStringLiteral("selectionKind"), QStringLiteral("archived")},
            {QStringLiteral("revisionId"), revision},
            {QStringLiteral("actionPosition"), QString::number(total_count)}};
        if (!isCurrentHistoryPosition(this->navigation_cursor_, coordinate))
            emit jumpRequested(coordinate);
    }
}

void HistoryDock::failArchivedActionPage(const QString &revision, const QString &error)
{
    if (!this->archived_pages_.contains(revision)) return;
    ArchivedPage &page = this->archived_pages_[revision];
    page.loading = false;
    page.pending_checkpoint_jump = false;
    page.error = error.isEmpty() ? tr("Archived actions are unavailable.") : error;
    rebuild();
}

void HistoryDock::handleTimelineClick(QListWidgetItem *item)
{
    if (item == nullptr) return;
    const QString revision = item->data(Qt::UserRole).toString();
    const QString role = item->data(Qt::UserRole + 1).toString();
    if (role == QStringLiteral("draft-action"))
    {
        if (!this->navigation_available_) return;
        const int position = item->data(Qt::UserRole + 2).toInt();
        bool valid = false;
        const int count = this->navigation_cursor_.value(
            QStringLiteral("journalCount")).toString().toInt(&valid);
        if (!valid || position < 1 || position > count) return;
        const QJsonObject coordinate{{QStringLiteral("selectionKind"), QStringLiteral("draft")},
            {QStringLiteral("revisionId"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("actionPosition"), QString::number(position)}};
        if (!isCurrentHistoryPosition(this->navigation_cursor_, coordinate))
            emit jumpRequested(coordinate);
        return;
    }
    if (this->archive_scope_.isEmpty()) return;
    if (role == QStringLiteral("archived-action"))
    {
        if (!this->navigation_available_ || !this->archived_pages_.contains(revision)) return;
        const ArchivedPage &page = this->archived_pages_.value(revision);
        if (!page.error.isEmpty() || page.total_count < 0) return;
        const int position = item->data(Qt::UserRole + 2).toInt();
        if (position < 1 || position > page.actions.size() ||
            page.actions.at(position - 1).identity !=
                item->data(Qt::UserRole + 3).toString()) return;
        const QJsonObject coordinate{{QStringLiteral("selectionKind"), QStringLiteral("archived")},
            {QStringLiteral("revisionId"), revision},
            {QStringLiteral("actionPosition"), QString::number(position)}};
        if (!isCurrentHistoryPosition(this->navigation_cursor_, coordinate))
            emit jumpRequested(coordinate);
        return;
    }
    if (role != QStringLiteral("commit") && role != QStringLiteral("expand") &&
        role != QStringLiteral("more")) return;
    ArchivedPage &page = this->archived_pages_[revision];
    if (role == QStringLiteral("commit"))
    {
        if (!this->navigation_available_) return;
        if (page.total_count == 0) return; // Snapshot-only: no executable MODEL archive.
        if (page.total_count > 0)
        {
            const QJsonObject coordinate{{QStringLiteral("selectionKind"), QStringLiteral("archived")},
                {QStringLiteral("revisionId"), revision},
                {QStringLiteral("actionPosition"), QString::number(page.total_count)}};
            if (!isCurrentHistoryPosition(this->navigation_cursor_, coordinate))
                emit jumpRequested(coordinate);
            return;
        }
        page.pending_checkpoint_jump = true;
        page.pending_generation = this->navigation_cursor_.value(
            QStringLiteral("generation")).toString();
        page.expanded = true;
    }
    if (role == QStringLiteral("expand"))
    {
        page.expanded = !page.expanded;
        page.pending_checkpoint_jump = false;
    }
    if (role == QStringLiteral("more") && !page.expanded) return;
    if (page.expanded && !page.complete && !page.loading &&
        (page.actions.isEmpty() || role == QStringLiteral("more")))
    {
        page.loading = true;
        page.error.clear();
        const int offset = page.next_offset;
        rebuild();
        emit archivedActionPageRequested(revision, offset);
        return;
    }
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
    const int previous_maximum = this->timeline_->verticalScrollBar()->maximum();
    const bool follow_latest = previous_maximum > 0 &&
        previous_position >= previous_maximum - 1;
    this->timeline_->clear();
    // Actions belong *before* their Commit checkpoint, oldest to newest.
    // The disclosure row expands metadata; the Commit row is a state target.
    QHash<QString, int> archive_boundaries;
    for (const HistoryDockEntry &commit : this->context_.commits)
    {
        const ArchivedPage page = this->archived_pages_.value(commit.identity);
        const int first_action_index = this->timeline_->count();
        if (page.expanded)
        {
            for (int index = 0; index < page.actions.size(); ++index)
            {
                const HistoryDockEntry &entry = page.actions.at(index);
                QListWidgetItem *item = new QListWidgetItem(
                    QStringLiteral("    ● ") + entry.title, this->timeline_);
                item->setData(Qt::UserRole, commit.identity);
                item->setData(Qt::UserRole + 1, QStringLiteral("archived-action"));
                item->setData(Qt::UserRole + 2, index + 1);
                item->setData(Qt::UserRole + 3, entry.identity);
                item->setToolTip(entry.details + tr("\nClick to select this verified action state."));
            }
            if (page.complete && page.actions.isEmpty())
                new QListWidgetItem(tr("    Snapshot-only checkpoint (no archived actions)"),
                                    this->timeline_);
            if (page.loading || !page.error.isEmpty() || !page.complete)
            {
                QString text = page.loading ? tr("    Loading archived actions…") :
                    !page.error.isEmpty() ? tr("    Archived actions unavailable — click to retry") :
                    tr("    Load more archived actions…");
                QListWidgetItem *item = new QListWidgetItem(text, this->timeline_);
                item->setData(Qt::UserRole, commit.identity);
                item->setData(Qt::UserRole + 1, QStringLiteral("more"));
                if (!page.error.isEmpty()) item->setToolTip(page.error);
            }
        }
        archive_boundaries.insert(commit.identity, first_action_index);
        if (!this->archive_scope_.isEmpty())
        {
            QListWidgetItem *expand = new QListWidgetItem(page.expanded
                ? tr("    ▾ Hide archived actions")
                : tr("    ▸ Show archived actions"), this->timeline_);
            expand->setData(Qt::UserRole, commit.identity);
            expand->setData(Qt::UserRole + 1, QStringLiteral("expand"));
            expand->setToolTip(tr("Expand or collapse the original archived MODEL actions."));
        }
        QListWidgetItem *item = new QListWidgetItem(
            QStringLiteral("◆ ") + commit.title, this->timeline_);
        item->setData(Qt::UserRole, commit.identity);
        item->setData(Qt::UserRole + 1, QStringLiteral("commit"));
        item->setToolTip(commit.details + (this->archive_scope_.isEmpty() ? QString() :
            page.total_count == 0 ? tr("\nSnapshot-only checkpoint: no executable action archive.") :
            tr("\nClick to navigate to this checkpoint (verified by DVMDB).")));
        QFont font = item->font();
        font.setBold(true);
        item->setFont(font);
    }
    const int draft_start = this->timeline_->count();
    int draft_action_position = 0;
    for (const HistoryDockEntry &entry : this->actions_)
    {
        const QString prefix = entry.kind == HistoryDockEntry::Kind::PendingAction ?
            QStringLiteral("◌ ") : QStringLiteral("● ");
        QListWidgetItem *item = new QListWidgetItem(prefix + entry.title, this->timeline_);
        item->setData(Qt::UserRole, entry.identity);
        if (entry.kind == HistoryDockEntry::Kind::SavedAction)
        {
            ++draft_action_position;
            item->setData(Qt::UserRole + 1, QStringLiteral("draft-action"));
            item->setData(Qt::UserRole + 2, draft_action_position);
        }
        item->setToolTip((entry.details.isEmpty() ? entry.identity : entry.details) +
            (entry.kind == HistoryDockEntry::Kind::SavedAction
                ? tr("\nClick to select this saved action state.") : QString()));
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
            insert_position = draft_start + position;
        }
        else if (kind == QStringLiteral("archived") && !revision.isEmpty())
        {
            marker = tr("▶ Archived action %1 in %2")
                .arg(this->navigation_cursor_.value(QStringLiteral("archivedActionPosition")).toString(),
                     revision.left(8));
            if (archive_boundaries.contains(revision))
            {
                const ArchivedPage page = this->archived_pages_.value(revision);
                insert_position = archive_boundaries.value(revision);
                bool exact = false;
                const int archived_position = this->navigation_cursor_.value(
                    QStringLiteral("archivedActionPosition")).toString().toInt(&exact);
                if (page.expanded && exact && archived_position >= 0 &&
                    archived_position <= page.actions.size())
                    insert_position += archived_position;
                else if (page.expanded)
                    marker += tr(" (expand/load more to locate)");
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
        notice += this->archive_scope_.isEmpty()
            ? tr(" Open a private draft to inspect archived MODEL actions.")
            : tr(" Click a Commit to jump to its verified checkpoint; use the disclosure row to expand actions in pages.");
    if (this->editing_)
        notice += tr(" Draft actions appear after commit checkpoints; pending actions are marked ◌.");
    this->notice_label_->setText(notice);
}
