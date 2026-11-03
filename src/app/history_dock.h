#pragma once

#include <QDockWidget>
#include <QList>
#include <QString>

class QLabel;
class QListWidget;
class QPushButton;

// A display-only view of history that is actually available today. Prior
// committed *actions* are not recoverable from DVMDB's bounded draft journal.
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

// HIST-UI: no navigation, inversion, undo or redo is implemented here.
// Never advertise actions as functional until the authoritative history
// controller is available. The main window owns and docks this widget.
class HistoryDock final : public QDockWidget
{
public:
    explicit HistoryDock(QWidget *parent = nullptr);

    void setContext(const HistoryDockContext &context);
    void setDraftActions(const QList<HistoryDockEntry> &actions, bool editing);
    void setTimeline(const HistoryDockContext &context,
                     const QList<HistoryDockEntry> &actions, bool editing, bool blocked);
    void clearDraftActions();

private:
    void rebuild();

    HistoryDockContext context_;
    QList<HistoryDockEntry> actions_;
    bool editing_ = false;
    bool blocked_ = false;
    QLabel *context_label_ = nullptr;
    QLabel *notice_label_ = nullptr;
    QListWidget *timeline_ = nullptr;
    QPushButton *undo_button_ = nullptr;
    QPushButton *redo_button_ = nullptr;
};
