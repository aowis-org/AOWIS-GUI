#pragma once

#include "config/project_server_profile_storage.h"
#include "tabs/project_revision_graph.h"
#include "projects/project_source_reader.h"
#include "projects/project_source_writer.h"

#include <QHash>
#include <QPair>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <aowis/model/hydraulic/network_hydraulic.h>

class GroupBoxCollapsible;
struct BuiltinExampleRevision;
class QDockWidget;
class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QListWidget;
class QTextEdit;
class ProjectsInspectorActionStack;
class ProjectsInspectorCreationForm;
class ProjectInspectorCreationForm;
class BranchInspectorCreationForm;
class DatabaseInspectorConnection;
class ProjectsInspectorRelations;
class DatabaseInspectorRelations;
class ProjectInspectorRelations;
class BranchInspectorRelations;
class RevisionInspectorRelations;
class DatabaseInspector;
class ProjectsInspectorOverview;
class DatabaseInspectorOverview;
class ProjectInspectorOverview;
class BranchInspectorOverview;
class RevisionInspectorOverview;
class ProjectInspector;
class BranchInspector;
class RevisionInspector;
class QTreeWidget;
class QTreeWidgetItem;

struct ProjectPreviewRevision
{
    QString id;
    QString title;
    QString description;
    QString branch;
    QStringList parents;
    QStringList tags;
    QString author;
    QString created_at;
    QString record_sha256;
};

struct ProjectPreviewBranch
{
    QString id;
    QString name;
    QString description;
    QStringList tags;
    QString head;
    QString origin_branch_id; // Source branch UUIDv7 (or preview branch ID), if recorded.
    QString fork_revision_id; // Exact original fork revision; never an ancestry edge.
};

struct ProjectPreviewData
{
    QString id;
    QString backend_id; // raw DVMDB ID, distinct from globally unique GUI ID
    QString repository_id; // DVMDB root UUID; immutable source identity, not GUI profile ID.
    QString server_id;
    QString name;
    QString description;
    QString source_example_resource; // template origin, preview only: network not persisted
    bool database_backed = false;
    QStringList tags;
    QList<ProjectPreviewBranch> branches;
    QList<ProjectPreviewRevision> revisions;
};

// Data-source profiles are locally persisted; real DVMDB listings are read-only.
// The legacy example fixture is explicitly isolated from connected data.
class ProjectsWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit ProjectsWidget(QWidget *parent = nullptr);
    ~ProjectsWidget() override;

public slots:
    void selectProjectByName(const QString &name);
    void selectBranchByName(const QString &name);
    void showRevisionHistory();

signals:
    void projectContextChanged(const QStringList &projects,
                               const QString &project,
                               const QStringList &branches,
                               const QString &branch);
    void builtinExampleRequested(const QString &resource_path, const QString &file_name);
    void importProjectRequested();
    // Canonical generation-verified preview; autosave is not attached.
    void draftNetworkPreviewRequested(NetworkHydraulic network, const QString &draft_id,
                                      const QString &generation);
    void draftNetworkEditRequested(const QString &draft_id, const QString &network_id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class SelectionKind { None, Server, Project, Branch, Revision, NewSource };
    enum class CreationKind { None, Project, Branch };
    void seedPreview();
    void populateServerTree();
    void populateProjectTree();
    void populateRevisionTree();
    bool isBranchHead(const QString &revision_id) const;
    void rebuildGraph();
    void updateInspector();
    void updateEmptyState();
    void updateSourceFields();
    void updateButtons();
    struct SourceSession;
    void startSource(const ProjectServerProfile &profile);
    void resetSource(const QString &id);
    void loadSource(const QString &id);
    void connectSource(); // explicit refresh of selected source (inspector only)
    bool isSourceReady(const QString &id) const;
    void restoreVisibleSelection();
    void finishSourceWrite(const QString &created_id, const QString &error,
                           const QString &project_id, const QString &branch_name);

    void applySourceSnapshot(const QString &source_id, const SourceSnapshot &snapshot);
    void updateConnectionStatus();
    bool isPreviewSource() const;
    void broadcastContext();
    void select(SelectionKind kind, const QString &id);
    void selectTreeItem(QTreeWidget *tree, const QString &key);
    void createServer();
    void saveNewSource();
    void cancelNewSource();
    void deleteServer();
    void createProject();
    void createBranch();
    void showWorkingCopies();
    void editProjectNetwork();
    void openWorkingCopy(bool direct_edit);
    void beginCreation(bool branch);
    ProjectsInspectorCreationForm *currentCreationForm() const;
    void cancelCreation();
    void submitCreation();
    void validateCreation();
    void updateInspectorTabs();
    void activateInspectorRelation(const QString &key);
    void deleteSelection();
    void browseExamples();
    void createProjectFromExample(const BuiltinExampleRevision &example);
    void applyInspectorEdits();
    void reportServerSaveFailure();
    void setPersonalServer(bool enabled);
    ProjectServerProfile *currentServer();
    ProjectPreviewData *currentProject();
    const ProjectPreviewData *currentProject() const;
    ProjectPreviewBranch *currentBranch();
    ProjectPreviewRevision *selectedRevision();
    static QString makeId(const QString &prefix);
    static QStringList parseTags(const QString &text);

    QList<ProjectServerProfile> servers;
    QString server_id;
    QList<ProjectPreviewData> projects;
    QString project_id;
    QString branch_id;
    SelectionKind selection_kind = SelectionKind::None;
    QString selected_id;
    QTreeWidget *server_tree = nullptr;
    QTreeWidget *project_tree = nullptr;
    QTreeWidget *revision_tree = nullptr;
    QPushButton *full_tree_button = nullptr;
    bool show_full_revision_tree = true;
    ProjectRevisionGraph *graph = nullptr;
    QStackedWidget *graph_stack = nullptr;
    QDockWidget *inspector_dock = nullptr;
    QLabel *inspector_heading = nullptr;
    QLabel *inspector_subtitle = nullptr;
    QStackedWidget *inspector_content_stack = nullptr;
    QTabWidget *inspector_tabs = nullptr;
    QWidget *inspector_overview_page = nullptr;
    QStackedWidget *inspector_overview_stack = nullptr;
    DatabaseInspectorOverview *database_overview = nullptr;
    ProjectInspectorOverview *project_overview = nullptr;
    BranchInspectorOverview *branch_overview = nullptr;
    RevisionInspectorOverview *revision_overview = nullptr;
    DatabaseInspectorConnection *database_connection = nullptr;
    DatabaseInspectorRelations *database_relations = nullptr;
    ProjectInspectorRelations *project_relations = nullptr;
    BranchInspectorRelations *branch_relations = nullptr;
    RevisionInspectorRelations *revision_relations = nullptr;
    QWidget *inspector_fields = nullptr;
    QStackedWidget *creation_forms = nullptr;
    ProjectInspectorCreationForm *project_creation = nullptr;
    BranchInspectorCreationForm *branch_creation = nullptr;
    CreationKind creation_kind = CreationKind::None;
    QString creation_server_id;
    QString creation_project_id;
    SelectionKind inspector_tab_kind = SelectionKind::None;
    QLabel *metadata_info = nullptr;
    QLineEdit *name_edit = nullptr;
    QLineEdit *tags_edit = nullptr;
    QTextEdit *description_edit = nullptr;
    ProjectsInspectorActionStack *inspector_actions_stack = nullptr;
    DatabaseInspector *database_inspector = nullptr;
    ProjectInspector *project_inspector = nullptr;
    BranchInspector *branch_inspector = nullptr;
    RevisionInspector *revision_inspector = nullptr;
    QHash<QString, SourceSession *> source_sessions;
    QHash<QString, QPair<QString, QString>> graph_revision_lookup;
    ProjectSourceWriter *source_writer = nullptr;
    QString select_after_write_project;
    QString select_after_write_branch;
    QLabel *no_projects_description = nullptr;
    QLabel *no_projects_title = nullptr;
    QLabel *preview_banner = nullptr;
    QLabel *inspector_preview_note = nullptr;
    enum class SourceState { Checking, Loading, Connected, Error };
    int personal_designation_request = 0;
    bool updating_ui = false;
};
