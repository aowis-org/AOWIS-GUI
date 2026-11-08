#pragma once

#include <QDockWidget>
#include <QHash>
#include <QList>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <functional>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// The visible revision markers come from Projects, while navigability and
// the active position come only from the authoritative private DVMDB cursor.
struct HistoryDockEntry
{
    enum class Kind { Commit, ArchivedAction, SavedAction, PendingAction };
    Kind kind = Kind::Commit;
    QString title;
    QString identity;
    QString details;
    // For server-paged first-parent checkpoints; empty for action rows.
    QString first_parent;
    std::function<QString()> localized_title;
    std::function<QString()> localized_details;

    QString displayTitle() const { return this->localized_title ? this->localized_title() : this->title; }
    QString displayDetails() const { return this->localized_details ? this->localized_details() : this->details; }
};

struct HistoryDockContext
{
    QString project;
    QString branch;
    QList<HistoryDockEntry> commits;
    bool incomplete_ancestry = false;
};

// Adjacent buttons use server-resolved coordinates; direct jumps use numeric
// positions proven by the private journal or authenticated archive metadata.
// Every navigation remains an atomic fenced operation on the private draft.
class HistoryDock final : public QDockWidget
{
    Q_OBJECT
public:
    explicit HistoryDock(QWidget *parent = nullptr);

    void setContext(const HistoryDockContext &context);
    void setDraftActions(const QList<HistoryDockEntry> &actions, bool editing);
    void setTimeline(const HistoryDockContext &context,
                     const QList<HistoryDockEntry> &actions, bool editing, bool blocked);
    void clearDraftActions();
    // Archived metadata is scoped to the exact private draft. The server's
    // validated action total authenticates checkpoint coordinates even when
    // only one 50-action page is loaded for display.
    void setArchiveScope(const QString &scope);
    void appendArchivedActionPage(const QString &revision, int offset,
                                  const QList<HistoryDockEntry> &entries,
                                  int next_offset, int total_count);
    void failArchivedActionPage(const QString &revision, const QString &error);
    // Independent first-parent Commit pagination from the private repository.
    // Entries arrive newest-first and are rendered oldest-first; previously
    // loaded pages remain visible when an older-page request fails.
    void appendBranchHistoryPage(const QString &after_revision,
                                 const QList<HistoryDockEntry> &commits,
                                 const QString &next_cursor);
    void failBranchHistoryPage(const QString &after_revision, const QString &error);
    void setNavigationState(const QJsonObject &cursor, bool available,
                            const QString &unavailable_reason = QString());

signals:
    void undoRequested();
    void redoRequested();
    void jumpRequested(const QJsonObject &coordinate);
    void archivedActionPageRequested(const QString &revision, int offset);
    void branchHistoryPageRequested(const QString &after_revision);

private:
    void rebuild();
    void handleTimelineClick(QListWidgetItem *item);
    void requestBranchHistoryPage(const QString &after_revision);

    struct ArchivedPage
    {
        QList<HistoryDockEntry> actions;
        int next_offset = 0;
        int total_count = -1;
        bool complete = false;
        bool expanded = false;
        bool loading = false;
        bool pending_checkpoint_jump = false;
        QString pending_generation;
        QString error;
    };

    QString archive_scope_;
    QList<HistoryDockEntry> branch_commits_; // newest-first from DVMDB
    QString branch_next_cursor_;
    QString branch_request_cursor_;
    QString branch_history_error_;
    bool branch_history_loaded_ = false;
    bool branch_history_loading_ = false;
    QHash<QString, ArchivedPage> archived_pages_;
    HistoryDockContext context_;
    QList<HistoryDockEntry> actions_;
    bool editing_ = false;
    bool blocked_ = false;
    bool navigation_available_ = false;
    QJsonObject navigation_cursor_;
    QString navigation_unavailability_reason_;
    QLabel *context_label_ = nullptr;
    QLabel *notice_label_ = nullptr;
    QListWidget *timeline_ = nullptr;
    QPushButton *undo_button_ = nullptr;
    QPushButton *redo_button_ = nullptr;
};
