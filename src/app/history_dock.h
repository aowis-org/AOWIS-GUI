#pragma once

#include <QDockWidget>
#include <QList>
#include <QJsonObject>
#include <QString>

class QLabel;
class QListWidget;
class QPushButton;

// The visible revision markers come from Projects, while navigability and
// the active position come only from the authoritative private DVMDB cursor.
struct HistoryDockEntry
{
    enum class Kind { Commit, SavedAction, PendingAction };
    Kind kind = Kind::Commit;
    QString title;
    QString identity;
    QString details;
};

struct HistoryDockContext
{
    QString project;
    QString branch;
    QList<HistoryDockEntry> commits;
    bool incomplete_ancestry = false;
};

// The buttons request only adjacent backend-approved positions. They never
// invent history from displayed labels or change immutable revisions.
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
    void setNavigationState(const QJsonObject &cursor, bool available,
                            const QString &unavailable_reason = QString());

signals:
    void undoRequested();
    void redoRequested();

private:
    void rebuild();

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
