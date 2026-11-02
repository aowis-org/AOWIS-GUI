#include "tabs/tab_projects_widget.h"
#include "tabs/projects_inspector_panels.h"
#include "tabs/projects_inspector_details.h"
#include "tabs/projects_inspector_presentation.h"
#include "tabs/personal_working_copy_dialog.h"
#include "widgets/group_box_collapsible.h"
#include "config/gui_configuration.h"
#include "config/project_server_profile_storage.h"
#include "projects/dvmdb_identity.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDockWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QLabel>
#include <QFrame>
#include <QEventLoop>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QScrollBar>
#include <QSettings>
#include <QMainWindow>
#include <QMessageBox>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <cstdio>
#include <cmath>

namespace
{
bool require(bool condition, const char *reason)
{
    if (!condition)
        std::fprintf(stderr, "projects_inspector_dock: %s\n", reason);
    return condition;
}

QPushButton *activeInspectorAction(ProjectsWidget &projects, const QString &object_name)
{
    QStackedWidget *stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorActionsStack"));
    if (stack != nullptr && stack->currentWidget() != nullptr) {
        QPushButton *action = stack->currentWidget()->findChild<QPushButton *>(object_name);
        if (action != nullptr) return action;
    }
    // HEAD/Revision primary actions live immediately below Description in
    // the active Overview, not in the footer action stack.
    QStackedWidget *overview_stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorOverviewStack"));
    if (overview_stack != nullptr && overview_stack->currentWidget() != nullptr) {
        QPushButton *action = overview_stack->currentWidget()
            ->findChild<QPushButton *>(object_name);
        if (action != nullptr) return action;
    }
    if (stack == nullptr || stack->currentWidget() == nullptr
        || stack->currentWidget()->objectName() != QStringLiteral("projectsRevisionInspector"))
        return nullptr;
    QWidget *details = projects.findChild<QWidget *>(QStringLiteral("projectsRevisionDetailsTab"));
    return details != nullptr ? details->findChild<QPushButton *>(object_name) : nullptr;
}

QWidget *activeInspectorOverview(ProjectsWidget &projects)
{
    QStackedWidget *stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorOverviewStack"));
    return stack != nullptr ? stack->currentWidget() : nullptr;
}

QLineEdit *activeInspectorName(ProjectsWidget &projects)
{
    QWidget *overview = activeInspectorOverview(projects);
    if (overview == nullptr) return nullptr;
    QLineEdit *name = overview->findChild<QLineEdit *>(QStringLiteral("projectInspectorName"));
    if (name != nullptr) return name;
    // HEAD and revision edit branch metadata in their dedicated Branch tab.
    QWidget *branch_tab = projects.findChild<QWidget *>(QStringLiteral("projectsRevisionBranchTab"));
    return branch_tab != nullptr
        ? branch_tab->findChild<QLineEdit *>(QStringLiteral("projectInspectorName")) : nullptr;
}

QLabel *activeInspectorMetadata(ProjectsWidget &projects)
{
    QWidget *overview = activeInspectorOverview(projects);
    return overview != nullptr
        ? overview->findChild<QLabel *>(QStringLiteral("projectInspectorMetadata")) : nullptr;
}

void waitFor(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

bool near(double a, double b)
{
    return std::abs(a - b) < 0.001;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir configuration_dir;
    if (!require(configuration_dir.isValid(), "could not create a temporary configuration"))
        return 1;
    qputenv("XDG_DATA_HOME", configuration_dir.path().toUtf8());
    QApplication app(argc, argv);

    // Domain presentation must preserve identity, branch provenance and
    // immutable revision metadata without borrowing fields from other forms.
    {
        ProjectServerProfile database;
        database.id = QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6371");
        database.name = QStringLiteral("Working DB");
        database.connection_type = QStringLiteral("remote");
        database.personal = true;
        const ProjectsInspectorPresentation database_view = ProjectsInspectorPresentations::database(
            &database, QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6371"));
        if (!require(database_view.name == database.name
                         && !database_view.metadata.contains(QStringLiteral("Personal working database"))
                         && database_view.metadata.contains(QStringLiteral("DVMDB repository UUIDv7")),
                     "database Inspector presentation lost repository identity or retained the old crown text"))
            return 1;

        ProjectPreviewData project;
        project.id = QStringLiteral("project-ui-id");
        project.backend_id = QStringLiteral("project-uuid");
        project.repository_id = QStringLiteral("repository-uuid");
        project.name = QStringLiteral("Network project");
        project.database_backed = true;
        ProjectPreviewBranch main_branch;
        main_branch.id = QStringLiteral("project-ui-id:branch-main");
        main_branch.name = QStringLiteral("main");
        main_branch.head = QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6371");
        ProjectPreviewBranch feature_branch;
        feature_branch.id = QStringLiteral("project-ui-id:branch-feature");
        feature_branch.name = QStringLiteral("feature");
        feature_branch.origin_branch_id = QStringLiteral("branch-main");
        feature_branch.fork_revision_id = main_branch.head;
        project.branches.append(main_branch);
        project.branches.append(feature_branch);
        const ProjectsInspectorPresentation project_view = ProjectsInspectorPresentations::project(&project);
        const ProjectsInspectorPresentation branch_view = ProjectsInspectorPresentations::branch(
            QList<ProjectPreviewData>{project}, feature_branch.id);
        const ProjectsInspectorPresentation head_view = ProjectsInspectorPresentations::head(
            &main_branch, &project);
        const ProjectsInspectorPresentation unborn_view = ProjectsInspectorPresentations::head(
            &feature_branch, &project);
        if (!require(head_view.heading == QStringLiteral("HEAD")
                         && head_view.name == main_branch.name
                         && head_view.metadata.contains(main_branch.head)
                         && !head_view.editable
                         && unborn_view.metadata.contains(QStringLiteral("No commits yet"))
                         && unborn_view.metadata.indexOf(QStringLiteral("Project UUIDv7"))
                            < unborn_view.metadata.indexOf(QStringLiteral("No commits yet")),
                     "HEAD Inspector lost its branch identity or unborn state")) return 1;
        if (!require(project_view.name == project.name
                         && project_view.metadata.contains(project.repository_id)
                         && !project_view.metadata.contains(QStringLiteral("(read-only)"))
                         && branch_view.name == feature_branch.name
                         && branch_view.metadata.contains(QStringLiteral("Created from branch: main"))
                         && branch_view.metadata.contains(feature_branch.fork_revision_id),
                     "project or branch Inspector presentation lost source and fork metadata"))
            return 1;
        ProjectPreviewRevision revision;
        revision.id = main_branch.head;
        revision.title = QStringLiteral("Initial revision");
        revision.parents.clear();
        revision.record_sha256 = QStringLiteral("canonical-sha256");
        revision.message = QStringLiteral("Implemented initial network");
        const ProjectsInspectorPresentation revision_view = ProjectsInspectorPresentations::revision(
            &revision, &project, &main_branch);
        if (!require(revision_view.name == main_branch.name
                         && revision_view.commit_message == revision.message
                         && revision_view.subtitle.contains(QStringLiteral("Immutable"))
                         && revision_view.metadata.contains(QStringLiteral("HEAD of: main"))
                         && revision_view.metadata.contains(revision.record_sha256),
                     "revision Inspector presentation lost immutable ancestry metadata"))
            return 1;
    }

    // Network initialization and Monitor preview must not be available until
    // a real private draft has been reopened and canonically hydrated.
    {
        PersonalWorkingCopyDialog dialog(QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6371"),
            QStringLiteral("019a47b1-2042-7af9-8d1c-2670badb6371"),
            QStringLiteral("main"), {}, {}, {}, QStringLiteral("Test"));
        QPushButton *initialize = dialog.findChild<QPushButton *>(
            QStringLiteral("projectsWorkingCopiesInitializeNetwork"));
        QPushButton *view = dialog.findChild<QPushButton *>(
            QStringLiteral("projectsWorkingCopiesViewNetwork"));
        QPushButton *edit = dialog.findChild<QPushButton *>(
            QStringLiteral("projectsWorkingCopiesEditNetwork"));
        if (!require(initialize != nullptr && view != nullptr && edit != nullptr
                         && !initialize->isEnabled() && !view->isEnabled()
                         && !edit->isEnabled() && !dialog.selectedEditRequested()
                         && dialog.selectedEditNetworkId().isEmpty()
                         && !dialog.selectedPreviewNetwork().has_value(),
                     "private draft network actions enabled without a verified draft"))
            return 1;
    }

    // The direct project-to-editor action must never open an unverified
    // repository or expose manual wizard operations as a replacement.
    {
        PersonalWorkingCopyDialog direct(QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6371"),
            QStringLiteral("019a47b1-2042-7af9-8d1c-2670badb6371"),
            QStringLiteral("main"), {}, {}, {}, QStringLiteral("Test"), nullptr, true);
        QPushButton *checkout = direct.findChild<QPushButton *>(
            QStringLiteral("projectsWorkingCopiesCheckout"));
        QPushButton *create = direct.findChild<QPushButton *>(
            QStringLiteral("projectsWorkingCopiesCreate"));
        if (!require(checkout != nullptr && create != nullptr
                         && checkout->isHidden() && create->isHidden()
                         && !direct.selectedEditRequested(),
                     "direct project edit exposed a manual checkout or opened an unverified writer"))
            return 1;
    }

    // A fresh GUI must not present fictional entries as configured servers.
    {
        ProjectsWidget empty_profiles;
        QTreeWidget *empty_tree = empty_profiles.findChild<QTreeWidget *>(
            QStringLiteral("serversTree"));
        QDockWidget *empty_dock = empty_profiles.findChild<QDockWidget *>(
            QStringLiteral("projectsInspectorDock"));
        QStackedWidget *empty_stack = empty_profiles.findChild<QStackedWidget *>(
            QStringLiteral("projectsGraphStack"));
        QPushButton *empty_add = empty_profiles.findChild<QPushButton *>(
            QStringLiteral("projectsEmptyAddDatabaseButton"));
        QPushButton *draft_cancel = empty_profiles.findChild<QPushButton *>(
            QStringLiteral("projectSourceCancelButton"));
        if (!require(empty_tree != nullptr && empty_tree->topLevelItemCount() == 0,
                     "a fresh configuration contains fictitious server profiles"))
            return 1;
        if (!require(empty_dock != nullptr && empty_dock->isHidden()
                         && empty_stack != nullptr && empty_stack->currentIndex() == 1
                         && empty_add != nullptr && draft_cancel != nullptr,
                     "empty workspace must hide the inspector and display add-database prompt"))
            return 1;
        empty_profiles.resize(1200, 740);
        empty_profiles.show();
        app.processEvents();
        empty_add->click();
        app.processEvents();
        if (!require(empty_dock->isVisible() && draft_cancel->isVisible()
                         && loadProjectServerProfiles().isEmpty(),
                     "starting a database must only open an unsaved inspector draft"))
            return 1;
        draft_cancel->click();
        app.processEvents();
        if (!require(empty_dock->isHidden() && loadProjectServerProfiles().isEmpty(),
                     "cancelled database draft was persisted or left the inspector open"))
            return 1;
    }

    // The storage format supports embedded sources, while existing endpoint-only
    // records remain REST sources. Merely displaying a local source opens no DB.
    ProjectServerProfile embedded_fixture;
    embedded_fixture.id = QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6371");
    embedded_fixture.name = QStringLiteral("Embedded fixture");
    embedded_fixture.connection_type = QStringLiteral("local");
    embedded_fixture.local_path = configuration_dir.path() + QStringLiteral("/embedded");
    embedded_fixture.auth_mode = QStringLiteral("None");
    if (!require(saveProjectServerProfiles({embedded_fixture}),
                 "could not persist an embedded data-source fixture"))
        return 1;
    {
        ProjectsWidget embedded_profiles;
        embedded_profiles.resize(1200, 740);
        embedded_profiles.show();
        app.processEvents();
        // Even before any project is created, a checked database belongs on
        // the graph canvas as a real server node, not behind an empty page.
        QStackedWidget *server_graph_stack = embedded_profiles.findChild<QStackedWidget *>(
            QStringLiteral("projectsGraphStack"));
        ProjectRevisionGraph *server_graph = embedded_profiles.findChild<ProjectRevisionGraph *>(
            QStringLiteral("projectRevisionGraph"));
        QTreeWidget *embedded_servers = embedded_profiles.findChild<QTreeWidget *>(
            QStringLiteral("serversTree"));
        if (!require(server_graph_stack != nullptr && server_graph != nullptr
                         && embedded_servers != nullptr && embedded_servers->topLevelItemCount() > 0
                         && server_graph_stack->currentWidget() == server_graph
                         && !server_graph->graphBounds().isEmpty(),
                     "checked database without projects is missing its graph server node")) return 1;
        // A Standalone build may also inject its default local profile. Test
        // all-unchecked independently of how many profiles are configured.
        for (int i = 0; i < embedded_servers->topLevelItemCount(); ++i)
            embedded_servers->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        app.processEvents();
        if (!require(server_graph_stack->currentIndex() == 2,
                     "unchecked databases did not show the no-databases-selected page")) return 1;
        embedded_servers->topLevelItem(0)->setCheckState(0, Qt::Checked);
        app.processEvents();
        if (!require(server_graph_stack->currentWidget() == server_graph
                         && !server_graph->graphBounds().isEmpty(),
                     "rechecking a database did not restore its server node")) return 1;
        QComboBox *kind = embedded_profiles.findChild<QComboBox *>(
            QStringLiteral("projectSourceKind"));
        QLineEdit *path = embedded_profiles.findChild<QLineEdit *>(
            QStringLiteral("projectLocalDvmdbPath"));
        QWidget *local_options = path != nullptr ? path->parentWidget() : nullptr;
        QTabWidget *embedded_tabs = embedded_profiles.findChild<QTabWidget *>(
            QStringLiteral("projectsInspectorTabs"));
        if (!require(embedded_tabs != nullptr && embedded_tabs->count() >= 2
                         && embedded_tabs->tabText(1) == QStringLiteral("Connection"),
                     "database inspector is missing its Connection tab")) return 1;
        embedded_tabs->setCurrentIndex(1);
        app.processEvents();
        QLineEdit *endpoint = embedded_profiles.findChild<QLineEdit *>(
            QStringLiteral("projectServerEndpoint"));
        if (!require(kind != nullptr && kind->currentData().toString() == QStringLiteral("local")
                         && !kind->isEnabled() && path != nullptr
                         && path->text() == embedded_fixture.local_path
                         && local_options != nullptr && local_options->isVisible()
                         && endpoint != nullptr && !endpoint->isVisible(),
                     "embedded profile did not use the local inspector fields"))
            return 1;
        QCheckBox *personal = embedded_profiles.findChild<QCheckBox *>(
            QStringLiteral("projectsPersonalServerCheckbox"));
        if (!require(personal != nullptr && personal->isVisible() && !personal->isChecked(),
                     "server inspector missing personal database designation")) return 1;
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        // Only Standalone has a native DVMDB service behind a local profile.
        if (!require(personal->isEnabled(),
                     "Standalone disabled embedded personal designation")) return 1;
        personal->setChecked(true);
        app.processEvents();
        QList<ProjectServerProfile> designated = loadProjectServerProfiles();
        if (!require(!designated.isEmpty() && designated.first().personal,
                     "personal server designation was not persisted")) return 1;
        bool crown_found = false;
        for (int i = 0; i < embedded_servers->topLevelItemCount(); ++i)
            if (embedded_servers->topLevelItem(i)->text(0).contains(QStringLiteral("♛")))
                crown_found = true;
        if (!require(crown_found, "personal database crown is missing from server list")) return 1;
        personal->setChecked(false);
        app.processEvents();
        if (!require(!loadProjectServerProfiles().first().personal,
                     "clearing personal designation was not persisted")) return 1;
#else
        // Local profiles are inspectable in the ordinary GUI, but cannot
        // acquire a native database role. Calling setChecked(true) here used
        // to open a blocking QMessageBox and hang the offscreen CTest run.
        if (!require(!personal->isEnabled()
                         && !loadProjectServerProfiles().first().personal,
                     "GUI-only local source incorrectly allows personal designation")) return 1;
#endif
        path->setText(configuration_dir.path() + QStringLiteral("/updated-embedded"));
        QMetaObject::invokeMethod(path, "editingFinished", Qt::DirectConnection);
        if (!require(loadProjectServerProfiles().first().local_path == path->text(),
                     "local directory changes were not persisted"))
            return 1;
    }

    std::fprintf(stderr, "projects_inspector_dock: embedded profile checks complete\n");
    // Explicit demo fixtures retain the existing version-explorer regression
    // coverage without forcing fake servers on users of a new installation.
    ProjectServerProfile preview_local;
    preview_local.id = QStringLiteral("preview-local");
    preview_local.name = QStringLiteral("Local preview");
    preview_local.endpoint = QStringLiteral("http://localhost:8080");
    preview_local.description = QStringLiteral("Local test profile");
    preview_local.auth_mode = QStringLiteral("None");
    ProjectServerProfile preview_remote;
    preview_remote.id = QStringLiteral("preview-remote");
    preview_remote.name = QStringLiteral("Remote preview");
    preview_remote.endpoint = QStringLiteral("https://database.example.invalid");
    preview_remote.description = QStringLiteral("Remote test profile");
    preview_remote.auth_mode = QStringLiteral("API token");
    const QList<ProjectServerProfile> sample_profiles{preview_local, preview_remote};
    if (!require(saveProjectServerProfiles(sample_profiles), "could not seed profile fixtures"))
        return 1;
    if (!require(loadProjectServerProfiles().at(0).connection_type == QStringLiteral("remote"),
                 "legacy server profile without connection type must remain REST"))
        return 1;

    ProjectsWidget projects;
    projects.resize(1250, 740);
    projects.show();
    app.processEvents();
    waitFor(120); // first fit follows the embedded dock/splitter layout

    QMainWindow *host = projects.findChild<QMainWindow *>(QStringLiteral("projectsDockHost"));
    QDockWidget *dock = projects.findChild<QDockWidget *>(QStringLiteral("projectsInspectorDock"));
    QTreeWidget *revisions = projects.findChild<QTreeWidget *>(QStringLiteral("revisionsTree"));
    QLineEdit *name = activeInspectorName(projects);
    ProjectRevisionGraph *graph = projects.findChild<ProjectRevisionGraph *>(
        QStringLiteral("projectRevisionGraph"));
    if (!require(host != nullptr && dock != nullptr
                     && revisions != nullptr && name != nullptr && graph != nullptr,
                 "workspace did not create its dock and inspector controls"))
        return 1;
    QPushButton *delete_in_inspector = activeInspectorAction(projects,
        QStringLiteral("projectInspectorDeleteButton"));
    QToolButton *add_server = projects.findChild<QToolButton *>(
        QStringLiteral("serversCreateButton"));
    QPushButton *add_project = projects.findChild<QPushButton *>(
        QStringLiteral("projectsInspectorCreateProject"));
    QPushButton *add_branch = activeInspectorAction(projects,
        QStringLiteral("projectsInspectorCreateBranch"));
    QTabWidget *inspector_tabs = projects.findChild<QTabWidget *>(
        QStringLiteral("projectsInspectorTabs"));
    if (!require(delete_in_inspector != nullptr && add_server != nullptr
                     && add_project != nullptr && add_branch != nullptr
                     && inspector_tabs != nullptr && inspector_tabs->count() >= 1
                     && activeInspectorAction(projects, QStringLiteral("projectInspectorDeleteButton"))->isVisible()
                     && activeInspectorAction(projects, QStringLiteral("projectInspectorDeleteButton"))->text() == QStringLiteral("Delete branch…"),
                 "context-sensitive inspector tabs or actions are missing"))
        return 1;
    // All four domains own separate Overview widgets, not just separate
    // buttons; choosing a different object swaps the complete form.
    QStackedWidget *overview_stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorOverviewStack"));
    if (!require(overview_stack != nullptr && overview_stack->count() == 4,
                 "Projects Inspector has not extracted four domain overview forms")) return 1;
    QSet<QLineEdit *> domain_names;
    for (int index = 0; index < overview_stack->count(); ++index)
    {
        QWidget *page = overview_stack->widget(index);
        QLineEdit *field = page->findChild<QLineEdit *>(QStringLiteral("projectInspectorName"));
        if (field == nullptr && page->objectName() == QStringLiteral("projectsRevisionOverviewForm"))
        {
            QWidget *branch_tab = projects.findChild<QWidget *>(QStringLiteral("projectsRevisionBranchTab"));
            if (branch_tab != nullptr)
                field = branch_tab->findChild<QLineEdit *>(QStringLiteral("projectInspectorName"));
        }
        if (!require(field != nullptr && page->findChild<QLabel *>(
                         QStringLiteral("projectInspectorMetadata")) != nullptr,
                     "domain form lacks its own fields and metadata")) return 1;
        domain_names.insert(field);
    }
    if (!require(domain_names.size() == 4
                     && overview_stack->currentWidget()->objectName()
                         == QStringLiteral("projectsBranchOverviewForm"),
                 "Overview forms share input widgets or do not track selected branch")) return 1;
    // Presentation is domain-owned and must neither change another form
    // nor emit editing callbacks while restoring a selection's values.
    ProjectInspectorOverview isolated_project;
    BranchInspectorOverview isolated_branch;
    int name_signals = 0;
    int description_signals = 0;
    QObject::connect(isolated_project.name, &QLineEdit::textChanged,
                     &isolated_project, [&name_signals](const QString &) { ++name_signals; });
    QObject::connect(isolated_project.description, &QTextEdit::textChanged,
                     &isolated_project, [&description_signals]() { ++description_signals; });
    isolated_project.present(QStringLiteral("Independent project"),
        QStringList{QStringLiteral("hydraulics")}, QStringLiteral("Project details"),
        QStringLiteral("Project metadata"), false, false);
    isolated_branch.present(QStringLiteral("Independent branch"),
        QStringList{}, QStringLiteral("Branch details"),
        QStringLiteral("Branch metadata"), true, true);
    if (!require(isolated_project.name->text() == QStringLiteral("Independent project")
                     && isolated_project.metadata->text() == QStringLiteral("Project metadata")
                     && isolated_branch.name->text() == QStringLiteral("Independent branch")
                     && isolated_project.description->toPlainText() == QStringLiteral("Project details")
                     && isolated_branch.description->toPlainText() == QStringLiteral("Branch details")
                     && !isolated_project.name->isEnabled()
                     && isolated_branch.name->isEnabled()
                     && isolated_project.fields->isHidden()
                     && !isolated_branch.fields->isHidden()
                     && name_signals == 0 && description_signals == 0,
                 "domain overview presentation leaked shared state or emitted edits")) return 1;
    // Connection is database-owned, and Related lists must be distinct pages
    // with independent row data. No one shared list is reconfigured by kind.
    QWidget *database_connection = projects.findChild<QWidget *>(
        QStringLiteral("projectsInspectorConnection"));
    QSet<QListWidget *> relation_lists;
    for (const QString &kind : {QStringLiteral("Database"), QStringLiteral("Project"),
                               QStringLiteral("Branch"), QStringLiteral("Revision")})
    {
        QWidget *page = projects.findChild<QWidget *>(
            QStringLiteral("projects") + kind + QStringLiteral("Relations"));
        QListWidget *list = page != nullptr ? page->findChild<QListWidget *>() : nullptr;
        if (!require(page != nullptr && list != nullptr,
                     "domain-owned Related page or list was not constructed")) return 1;
        relation_lists.insert(list);
    }
    if (!require(database_connection != nullptr && relation_lists.size() == 4
                     && database_connection->findChild<QLineEdit *>(
                         QStringLiteral("projectServerEndpoint")) != nullptr,
                 "Database connection or independent Related lists are missing")) return 1;
    if (!require(inspector_tabs->tabText(inspector_tabs->count() - 1) == QStringLiteral("Revisions")
                     && inspector_tabs->widget(inspector_tabs->count() - 1)->objectName()
                         == QStringLiteral("projectsBranchRelations"),
                 "branch selection did not mount its own revisions page")) return 1;
    QStackedWidget *action_stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorActionsStack"));
    if (!require(action_stack != nullptr && action_stack->count() == 4
                     && action_stack->currentWidget()->objectName()
                         == QStringLiteral("projectsBranchInspector"),
                 "Projects Inspector does not route to four independent domain action panels")) return 1;
    QPushButton *working_copies = projects.findChild<QPushButton *>(
        QStringLiteral("projectsInspectorWorkingCopies"));
    if (!require(working_copies != nullptr && !working_copies->isVisible()
                     && projects.findChild<QPushButton *>(
                         QStringLiteral("projectsInspectorEditNetwork")) == nullptr,
                 "preview-only branch must not expose legacy network editing")) return 1;
    if (!require(projects.findChild<QPushButton *>(QStringLiteral("serversRenameButton")) == nullptr
                     && projects.findChild<QPushButton *>(QStringLiteral("serversDeleteButton")) == nullptr,
                 "redundant server rename/delete controls remain in the sidebar"))
        return 1;
    for (QPushButton *button : projects.findChildren<QPushButton *>())
        if (!require(button == delete_in_inspector
                         || (button->text() != QStringLiteral("Rename")
                             && button->text() != QStringLiteral("Delete")),
                     "redundant project/branch rename or delete button remains"))
            return 1;
    for (QLabel *label : projects.findChildren<QLabel *>())
        if (!require(!label->text().contains(QStringLiteral("Select a revision to inspect it")),
                     "obsolete revision help text remains in the sidebar"))
            return 1;

    if (!require(!host->isWindow(),
                 "projects dock host must be embedded, not a separate top-level window"))
        return 1;
    if (!require(host->isVisible() && host->width() > 500 && host->height() > 300,
                 "projects dock host does not fill the visible workspace"))
        return 1;
    QSplitter *splitter = projects.findChild<QSplitter *>(
        QStringLiteral("projectsWorkspaceSplitter"));
    QTreeWidget *project_tree = projects.findChild<QTreeWidget *>(
        QStringLiteral("projectsTree"));
    if (!require(splitter != nullptr && splitter->isVisible()
                     && project_tree != nullptr && project_tree->isVisible()
                     && graph->isVisible() && graph->width() > 200 && graph->height() > 200,
                 "project tree or revision graph is missing from the visible layout"))
        return 1;
    // The map-fit action belongs to the application's top toolbar, not this sidebar.
    for (QPushButton *button : projects.findChildren<QPushButton *>())
    {
        if (!require(button->text() != QStringLiteral("Show network on map"),
                     "obsolete Show network on map button remains in Projects sidebar"))
            return 1;
    }
    if (!require(host->dockWidgetArea(dock) == Qt::RightDockWidgetArea,
                 "inspector is not docked on the right of the projects workspace"))
        return 1;
    if (!require(dock->features().testFlag(QDockWidget::DockWidgetClosable)
                     && dock->features().testFlag(QDockWidget::DockWidgetMovable)
                     && dock->features().testFlag(QDockWidget::DockWidgetFloatable),
                 "inspector does not support close, move and float"))
        return 1;
    if (!require(dock->isVisible(), "inspector should start open"))
        return 1;

    // Inspector notes (including UUIDs, long source names and errors) must
    // wrap within the standardized dock, never widen its scrollable content.
    QScrollArea *inspector_scroll = qobject_cast<QScrollArea *>(dock->widget());
    QLabel *inspector_metadata = activeInspectorMetadata(projects);
    if (!require(inspector_scroll != nullptr && inspector_metadata != nullptr
                     && inspector_scroll->widgetResizable()
                     && inspector_scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
                 "project inspector is not configured for vertical-only scrolling"))
        return 1;
    const QString saved_metadata = inspector_metadata->text();
    const QString long_metadata = QStringLiteral(
        "Canonical network metadata and repository identifiers need to remain "
        "readable within the narrow Projects inspector. ").repeated(12);
    inspector_metadata->setText(long_metadata);
    app.processEvents();
    if (!require(inspector_metadata->wordWrap()
                     && inspector_metadata->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored
                     && inspector_metadata->sizePolicy().hasHeightForWidth()
                     && inspector_metadata->text() == long_metadata
                     && inspector_metadata->heightForWidth(inspector_metadata->width())
                         > 2 * inspector_metadata->fontMetrics().lineSpacing()
                     && inspector_scroll->widget()->width() <= inspector_scroll->viewport()->width()
                     && inspector_scroll->horizontalScrollBar()->maximum() == 0,
                 "long inspector metadata did not wrap without horizontal overflow"))
        return 1;
    inspector_metadata->setText(saved_metadata);
    app.processEvents();

    std::fprintf(stderr, "projects_inspector_dock: inspector overview and graph checks complete\n");
    // Servers are managed as GUI-only profiles. Selecting one scopes the
    // preview projects and reopens its inspector, without creating a session.
    GroupBoxCollapsible *server_section = projects.findChild<GroupBoxCollapsible *>(
        QStringLiteral("serversCollapsibleGroup"));
    QTreeWidget *servers = projects.findChild<QTreeWidget *>(QStringLiteral("serversTree"));
    QLineEdit *endpoint = projects.findChild<QLineEdit *>(QStringLiteral("projectServerEndpoint"));
    QComboBox *auth_mode = projects.findChild<QComboBox *>(QStringLiteral("projectServerAuthMode"));
    QWidget *server_options = projects.findChild<QWidget *>(QStringLiteral("projectServerOptions"));
    if (!require(server_section != nullptr && servers != nullptr
                     && servers->topLevelItemCount() == 2
                     && endpoint != nullptr
                     && auth_mode != nullptr && server_options != nullptr,
                 "Servers panel or inspector configuration fields are missing"))
        return 1;
    if (!require(servers->parentWidget() != nullptr && server_section->isVisible(),
                 "Servers section is not visible above the project tree"))
        return 1;
    if (!require(servers->minimumHeight() == 86 && servers->maximumHeight() == 112,
                 "Servers tree must remain compact so Projects and Revisions can expand"))
        return 1;
    for (QLabel *label : projects.findChildren<QLabel *>())
        if (!require(label->text() != QStringLiteral("View:"),
                     "obsolete graph View: label is still present"))
            return 1;
    dock->close();
    servers->setCurrentItem(servers->topLevelItem(1));
    app.processEvents();
    if (!require(inspector_tabs->tabText(1) == QStringLiteral("Connection"),
                 "server inspector is missing the Connection tab")) return 1;
    QWidget *head_branch_page = projects.findChild<QWidget *>(
        QStringLiteral("projectsRevisionBranchTab"));
    if (!require(head_branch_page != nullptr && !head_branch_page->isVisible()
                     && inspector_tabs->indexOf(head_branch_page) < 0,
                 "unregistered HEAD Branch widget overlaps the Database Inspector")) return 1;
    QWidget *database_projects_tab = inspector_tabs->count() > 2
        ? inspector_tabs->widget(2) : nullptr;
    QPushButton *database_projects_create = database_projects_tab == nullptr
        ? nullptr : database_projects_tab->findChild<QPushButton *>(
              QStringLiteral("projectsDatabaseProjectsCreateProject"));
    if (!require(inspector_tabs->widget(1) == database_connection
                     && inspector_tabs->count() == 3
                     && inspector_tabs->tabText(2) == QStringLiteral("Projects")
                     && database_projects_tab->objectName()
                         == QStringLiteral("projectsDatabaseRelations")
                     && database_projects_create != nullptr,
                 "empty remote server must keep its Projects tab and Create project action"))
        return 1;
    inspector_tabs->setCurrentIndex(1);
    app.processEvents();
    QLabel *inspector_status = projects.findChild<QLabel *>(
        QStringLiteral("serversPreviewStatus"));
    if (!require(inspector_status != nullptr && inspector_status->wordWrap()
                     && inspector_status->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored
                     && inspector_scroll->widget()->width() <= inspector_scroll->viewport()->width()
                     && inspector_scroll->horizontalScrollBar()->maximum() == 0,
                 "connection inspector labels force horizontal scrolling")) return 1;
    if (!require(overview_stack->currentWidget()->objectName()
                     == QStringLiteral("projectsDatabaseOverviewForm"),
                 "server selection did not activate the database-owned form")) return 1;
    // Each domain has its own Tags and Description widgets. Resolve them only
    // after selecting the database: looking them up globally while a branch
    // is active can select the branch's fields and never persist server edits.
    QWidget *server_overview = activeInspectorOverview(projects);
    QLineEdit *tags_edit = server_overview != nullptr
        ? server_overview->findChild<QLineEdit *>(QStringLiteral("projectInspectorTags"))
        : nullptr;
    QTextEdit *description_edit = server_overview != nullptr
        ? server_overview->findChild<QTextEdit *>(QStringLiteral("projectInspectorDescription"))
        : nullptr;
    if (!require(tags_edit != nullptr && description_edit != nullptr
                     && tags_edit->isEnabled() && description_edit->isEnabled(),
                 "database Inspector does not expose its editable metadata fields")) return 1;
    if (!require(dock->isVisible() && activeInspectorName(projects)->text() == QStringLiteral("Remote preview")
                     && server_options->isVisible() && endpoint->text().startsWith(QStringLiteral("https://")),
                 "remote server selection did not show its preview inspector"))
        return 1;
    QPushButton *delete_connection = database_connection->findChild<QPushButton *>(
        QStringLiteral("projectsDeleteConnectionButton"));
    if (!require(delete_connection != nullptr && delete_connection->isVisible()
                     && activeInspectorAction(projects, QStringLiteral("projectInspectorDeleteButton")) == nullptr,
                 "connection removal must be available only on the database Connection page"))
        return 1;
    if (!require(action_stack->currentWidget()->objectName()
                     == QStringLiteral("projectsDatabaseInspector")
                     && activeInspectorAction(projects,
                         QStringLiteral("projectsInspectorWorkingCopies")) == nullptr,
                 "database inspector exposes branch-only controls")) return 1;
    inspector_tabs->setCurrentIndex(0);
    app.processEvents();
    QWidget *database_overview_page = activeInspectorOverview(projects);
    QLabel *personal_banner = database_overview_page != nullptr
        ? database_overview_page->findChild<QLabel *>(
              QStringLiteral("projectsPersonalDatabaseBanner")) : nullptr;
    QLabel *database_metadata = database_overview_page != nullptr
        ? database_overview_page->findChild<QLabel *>(
              QStringLiteral("projectInspectorMetadata")) : nullptr;
    if (!require(add_project->isVisible() && add_project->isEnabled()
                     && add_project->parentWidget() == database_overview_page
                     && personal_banner != nullptr && database_metadata != nullptr
                     && activeInspectorAction(projects, QStringLiteral("projectsInspectorCreateBranch")) == nullptr,
                 "database Overview did not expose its project creation action"))
        return 1;
    std::fprintf(stderr, "projects_inspector_dock: opening project creation\n");
    add_project->click();
    app.processEvents();
    QLabel *inspector_creation_summary = projects.findChild<QLabel *>(
        QStringLiteral("projectsProjectCreationSummary"));
    if (!require(inspector_creation_summary != nullptr
                     && inspector_creation_summary->wordWrap()
                     && inspector_creation_summary->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored
                     && inspector_scroll->widget()->width() <= inspector_scroll->viewport()->width()
                     && inspector_scroll->horizontalScrollBar()->maximum() == 0,
                 "creation inspector labels force horizontal scrolling")) return 1;
    QStackedWidget *project_draft_stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorContentStack"));
    QLineEdit *project_draft_name = projects.findChild<QLineEdit *>(
        QStringLiteral("projectsProjectCreationName"));
    // Every domain owns its own Create/Cancel buttons. Inspect the active
    // action page so tests cannot operate on a hidden sibling's controls.
    QPushButton *project_draft_confirm = activeInspectorAction(projects,
        QStringLiteral("projectsCreationConfirm"));
    QPushButton *project_draft_cancel = activeInspectorAction(projects,
        QStringLiteral("projectsCreationCancel"));
    QStackedWidget *creation_forms = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorCreationForms"));
    QLineEdit *branch_creation_name = projects.findChild<QLineEdit *>(
        QStringLiteral("projectsBranchCreationName"));
    if (!require(creation_forms != nullptr && creation_forms->count() == 2
                     && creation_forms->currentWidget()->objectName()
                        == QStringLiteral("projectsProjectCreationForm")
                     && branch_creation_name != nullptr && branch_creation_name != project_draft_name
                     && branch_creation_name->text().isEmpty(),
                 "project creation did not mount an independent form")) return 1;
    if (!require(project_draft_stack != nullptr && project_draft_stack->currentIndex() == 1
                     && project_draft_name != nullptr && project_draft_confirm != nullptr
                     && project_draft_cancel != nullptr && !project_draft_confirm->isEnabled(),
                 "project creation did not open a validated inline inspector form")) return 1;
    project_draft_name->setText(QStringLiteral("unused-draft"));
    app.processEvents();
    if (!require(project_draft_confirm->isEnabled(),
                 "valid project name did not enable the inline Create action")) return 1;
    project_draft_cancel->click();
    app.processEvents();
    if (!require(project_draft_stack->currentIndex() == 0 && add_project->isVisible(),
                 "cancelled project draft did not restore the database inspector")) return 1;
    // Inspector renaming replaces the redundant Rename buttons in the trees.
    activeInspectorName(projects)->setText(QStringLiteral("Renamed remote preview"));
    QMetaObject::invokeMethod(activeInspectorName(projects), "editingFinished", Qt::DirectConnection);
    app.processEvents();
    if (!require(servers->topLevelItem(1)->text(0) == QStringLiteral("Renamed remote preview"),
                 "server rename through inspector was not reflected in the tree"))
        return 1;
    activeInspectorName(projects)->setText(QStringLiteral("Remote preview"));
    QMetaObject::invokeMethod(activeInspectorName(projects), "editingFinished", Qt::DirectConnection);
    app.processEvents();
    if (!require(project_tree->topLevelItemCount() == 1
                     || project_tree->topLevelItemCount() == 0,
                 "projects from the local server leaked into the remote server view"))
        return 1;
    endpoint->setText(QStringLiteral("https://new-preview.example.invalid"));
    QMetaObject::invokeMethod(endpoint, "editingFinished", Qt::DirectConnection);
    auth_mode->setCurrentIndex(2);
    tags_edit->setText(QStringLiteral("remote, study"));
    QMetaObject::invokeMethod(tags_edit, "editingFinished", Qt::DirectConnection);
    description_edit->setPlainText(QStringLiteral("Persisted remote server description"));
    app.processEvents();
    if (!require(tags_edit->parentWidget() == description_edit->parentWidget()
                     && tags_edit->text() == QStringLiteral("remote, study")
                     && description_edit->toPlainText()
                         == QStringLiteral("Persisted remote server description"),
                 "metadata edits did not target the same active database form")) return 1;
    servers->setCurrentItem(servers->topLevelItem(0));
    app.processEvents();
    if (!require(servers->currentItem() != nullptr
                     && servers->currentItem()->data(0, Qt::UserRole).toString()
                         == QStringLiteral("s:preview-local")
                     && project_tree->topLevelItemCount() >= 2,
                 "switching to the local preview server did not load its projects"))
        return 1;
    if (!require(inspector_tabs->count() == 3
                     && inspector_tabs->widget(2)->objectName()
                         == QStringLiteral("projectsDatabaseRelations"),
                 "database related Projects tab is not owned by its domain")) return 1;
    // Switching databases preserves the Overview tab. Connection controls
    // belong to a separate tab and must only be tested once it is open.
    inspector_tabs->setCurrentIndex(1);
    app.processEvents();
    if (!require(inspector_tabs->currentWidget() == database_connection
                     && server_options->isVisible(),
                 "local preview database Connection tab did not show its controls")) return 1;
    servers->setCurrentItem(servers->topLevelItem(1));
    app.processEvents();
    if (!require(endpoint->text() == QStringLiteral("https://new-preview.example.invalid")
                     && auth_mode->currentText() == QStringLiteral("User login"),
                 "server configuration preview was lost on profile switch"))
        return 1;
    const QList<ProjectServerProfile> saved_profiles = loadProjectServerProfiles();
    QSettings saved_ini(guiConfigurationFilePath(), QSettings::IniFormat);
    if (!require(saved_profiles.size() == 2
                     && saved_profiles.at(1).name == QStringLiteral("Remote preview")
                     && saved_profiles.at(1).endpoint == QStringLiteral("https://new-preview.example.invalid")
                     && saved_profiles.at(1).auth_mode == QStringLiteral("User login")
                     && saved_profiles.at(1).description == QStringLiteral("Persisted remote server description")
                     && saved_profiles.at(1).tags == QStringList({QStringLiteral("remote"), QStringLiteral("study")})
                     && saved_ini.contains(QStringLiteral("servers/profiles"))
                     && !saved_ini.contains(QStringLiteral("projects/servers")),
                 "inspector modifications were not saved in the GUI INI file"))
        return 1;
    servers->setCurrentItem(servers->topLevelItem(0));
    projects.selectBranchByName(QStringLiteral("main"));
    app.processEvents();
    if (!require(!server_options->isVisible(),
                 "server-specific inspector fields remain visible for a branch"))
        return 1;

    QPushButton *heads_view = projects.findChild<QPushButton *>(
        QStringLiteral("revisionsHeadsOnlyButton"));
    QPushButton *full_tree_view = projects.findChild<QPushButton *>(
        QStringLiteral("revisionsFullTreeButton"));
    if (!require(heads_view != nullptr && full_tree_view != nullptr
                     && heads_view->isCheckable() && full_tree_view->isCheckable()
                     && full_tree_view->isChecked() && !heads_view->isChecked(),
                 "Revisions must default to the Full tree view"))
        return 1;
    const auto find_revision = [revisions](const QString &revision_id) -> QTreeWidgetItem *
    {
        for (QTreeWidgetItemIterator it(revisions); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toString()
                == QStringLiteral("r:") + revision_id)
                return *it;
        return nullptr;
    };
    const QString baseline_id = QStringLiteral("93f0de41");
    const QString main_head_id = QStringLiteral("c8d102ab");
    const QString scenario_start_id = QStringLiteral("a24f918d");
    const QString scenario_head_id = QStringLiteral("ec9045df");
    QTreeWidgetItem *baseline = find_revision(baseline_id);
    QTreeWidgetItem *main_head = find_revision(main_head_id);
    QTreeWidgetItem *scenario_start = find_revision(scenario_start_id);
    QTreeWidgetItem *scenario_head = find_revision(scenario_head_id);
    if (!require(revisions->topLevelItemCount() == 1 && baseline != nullptr
                     && main_head != nullptr && scenario_start != nullptr
                     && scenario_head != nullptr && baseline->parent() == nullptr
                     && main_head->parent() == baseline
                     && scenario_start->parent() == baseline
                     && scenario_head->parent() == scenario_start,
                 "Full tree should nest revisions under their parents"))
        return 1;
    if (!require(baseline->text(0) == QStringLiteral("Network baseline")
                     && main_head->text(0) == QStringLiteral("Validated network")
                     && scenario_start->text(0) == QStringLiteral("Scenario start")
                     && scenario_head->text(0) == QStringLiteral("Pressure tuning"),
                 "Revision labels must contain names only, never hashes"))
        return 1;

    QPushButton *graph_heads_view = projects.findChild<QPushButton *>(
        QStringLiteral("projectsGraphHeadsOnlyButton"));
    QPushButton *graph_full_view = projects.findChild<QPushButton *>(
        QStringLiteral("projectsGraphFullTreeButton"));
    QFrame *graph_view_switch = projects.findChild<QFrame *>(
        QStringLiteral("projectsGraphViewSegmentedControl"));
    if (!require(graph_heads_view != nullptr && graph_full_view != nullptr
                     && graph_view_switch != nullptr
                     && graph_heads_view->parentWidget() == graph_view_switch
                     && graph_full_view->parentWidget() == graph_view_switch
                     && graph_full_view->isChecked() && graph->showsFullTree(),
                 "graph modes must be visibly grouped with Full tree selected by default"))
        return 1;
    graph_heads_view->click();
    app.processEvents();
    if (!require(graph_heads_view->isChecked() && !graph->showsFullTree()
                     && graph->omissionCount() == 1
                     && !graph->revisionViewportPosition(baseline_id).isNull()
                     && revisions->topLevelItemCount() == 1,
                 "graph heads mode must retain the common ancestor and fold history"))
        return 1;
    graph->expandOmission(0);
    app.processEvents();
    if (!require(!graph->showsFullTree() && graph->omissionCount() == 0
                     && revisions->topLevelItemCount() == 1,
                 "graph omission expansion changed sidebar filter or did not unfold"))
        return 1;
    graph_full_view->click();
    app.processEvents();
    if (!require(graph_full_view->isChecked() && !graph_heads_view->isChecked()
                     && graph->showsFullTree() && graph->omissionCount() == 0,
                 "graph Full tree did not restore hidden history"))
        return 1;
    graph_heads_view->click();
    app.processEvents();
    if (!require(graph_heads_view->isChecked() && !graph_full_view->isChecked()
                     && !graph->showsFullTree() && graph->omissionCount() == 1,
                 "switching back to Editable heads must re-fold the graph"))
        return 1;
    graph->expandOmission(0);
    graph_heads_view->click();
    app.processEvents();
    if (!require(graph_heads_view->isChecked() && graph->omissionCount() == 1,
                 "pressing the active Editable heads control must re-fold expanded history"))
        return 1;

    heads_view->click();
    app.processEvents();
    if (!require(heads_view->isChecked() && !full_tree_view->isChecked()
                     && revisions->topLevelItemCount() == 2
                     && find_revision(main_head_id) != nullptr
                     && find_revision(scenario_head_id) != nullptr
                     && find_revision(baseline_id) == nullptr
                     && find_revision(scenario_start_id) == nullptr,
                 "Editable heads mode must show only distinct branch heads"))
        return 1;
    // The filter must also follow the active project instead of keeping
    // stale branch heads from the previously opened project.
    projects.selectProjectByName(QStringLiteral("Sample Pump Station"));
    app.processEvents();
    if (!require(revisions->topLevelItemCount() == 1
                     && revisions->topLevelItem(0)->text(0) == QStringLiteral("Initial layout")
                     && find_revision(main_head_id) == nullptr,
                 "Heads-only view retained revisions from the previous project"))
        return 1;
    projects.selectProjectByName(QStringLiteral("Sample North Network"));
    app.processEvents();
    if (!require(heads_view->isChecked() && revisions->topLevelItemCount() == 2
                     && find_revision(main_head_id) != nullptr,
                 "Heads-only view did not follow a project switch"))
        return 1;
    revisions->setCurrentItem(find_revision(scenario_head_id));
    app.processEvents();
    if (!require(activeInspectorName(projects)->text() == QStringLiteral("pressure-study")
                     && find_revision(scenario_head_id)->isSelected(),
                 "Selecting a head did not synchronize the inspector and tree"))
        return 1;
    full_tree_view->click();
    app.processEvents();
    if (!require(full_tree_view->isChecked() && revisions->topLevelItemCount() == 1
                     && find_revision(scenario_head_id) != nullptr
                     && find_revision(scenario_head_id)->isSelected(),
                 "Returning to Full tree did not keep the selected head"))
        return 1;
    revisions->setCurrentItem(find_revision(baseline_id));
    app.processEvents();
    heads_view->click();
    app.processEvents();
    if (!require(heads_view->isChecked() && find_revision(baseline_id) == nullptr
                     && find_revision(main_head_id) != nullptr
                     && find_revision(main_head_id)->isSelected()
                     && activeInspectorName(projects)->text() == QStringLiteral("Validated network"),
                 "Filtering a historical selection must select its branch head"))
        return 1;
    // A historical revision picked in the central DAG should be made visible
    // in the sidebar rather than silently losing tree synchronization.
    graph->revisionActivated(baseline_id);
    app.processEvents();
    if (!require(full_tree_view->isChecked()
                     && find_revision(baseline_id) != nullptr
                     && find_revision(baseline_id)->isSelected()
                     && activeInspectorName(projects)->text() == QStringLiteral("Network baseline"),
                 "Graph selection of history should reveal the Full tree"))
        return 1;
    // A historical revision initiates an inline branch draft at the exact
    // selected revision, without creating an extra revision or opening a dialog.
    QPushButton *revision_branch_action = activeInspectorAction(projects,
        QStringLiteral("projectsInspectorCreateBranch"));
    QStackedWidget *inspector_content = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorContentStack"));
    QComboBox *creation_source = projects.findChild<QComboBox *>(
        QStringLiteral("projectsCreationSource"));
    QLineEdit *creation_name = projects.findChild<QLineEdit *>(
        QStringLiteral("projectsBranchCreationName"));
    QLabel *creation_summary = projects.findChild<QLabel *>(
        QStringLiteral("projectsBranchCreationSummary"));
    QPushButton *creation_confirm = activeInspectorAction(projects,
        QStringLiteral("projectsCreationConfirm"));
    QPushButton *creation_cancel = activeInspectorAction(projects,
        QStringLiteral("projectsCreationCancel"));
    if (!require(revision_branch_action != nullptr && revision_branch_action->isVisible()
                     && creation_source != nullptr && creation_name != nullptr
                     && creation_summary != nullptr && creation_confirm != nullptr
                     && creation_cancel != nullptr && inspector_content != nullptr,
                 "revision branch workflow is missing from inspector")) return 1;
    std::fprintf(stderr, "projects_inspector_dock: opening revision branch creation\n");
    revision_branch_action->click();
    app.processEvents();
    if (!require(creation_forms->currentWidget()->objectName()
                     == QStringLiteral("projectsBranchCreationForm")
                     && creation_name != project_draft_name
                     && project_draft_name->text().isEmpty(),
                 "historical branch creation did not activate its independent form")) return 1;
    if (!require(inspector_content->currentIndex() == 1
                     && creation_source->currentData().toString() == QStringLiteral("revision:") + baseline_id
                     && creation_summary->text().contains(
                         QStringLiteral("Fork revision UUID: ") + baseline_id)
                     && creation_summary->text().contains(QStringLiteral("Source branch: main"))
                     && creation_summary->text().contains(QStringLiteral("Source branch UUID: "))
                     && !creation_confirm->isEnabled(),
                 "historical fork did not retain its exact revision and source provenance")) return 1;
    // Branch source choices are keyed by the stable branch identity, not
    // their mutable human-readable name. The display label remains readable.
    bool found_main_source = false;
    for (int index = 0; index < creation_source->count(); ++index)
    {
        const QString key = creation_source->itemData(index).toString();
        if (!key.startsWith(QStringLiteral("branch:"))) continue;
        if (creation_source->itemText(index).startsWith(QStringLiteral("main ·")))
        {
            found_main_source = true;
            if (!require(key != QStringLiteral("branch:main") && key.size() > 7,
                         "branch creation source is keyed by a mutable name")) return 1;
        }
    }
    if (!require(found_main_source, "main branch is absent from creation sources")) return 1;
    creation_name->setText(QStringLiteral("from-inspector"));
    app.processEvents();
    if (!require(creation_confirm->isEnabled(),
                 "valid unique branch draft cannot be submitted")) return 1;
    creation_cancel->click();
    app.processEvents();
    if (!require(inspector_content->currentIndex() == 0
                     && revision_branch_action->isVisible(),
                 "cancel did not restore the selected revision inspector")) return 1;
    QPushButton *orientation_toggle = projects.findChild<QPushButton *>(
        QStringLiteral("projectsGraphOrientationToggle"));
    if (!require(orientation_toggle != nullptr && orientation_toggle->isCheckable()
                     && !orientation_toggle->isChecked() && graph->isTopDown(),
                 "graph orientation toggle did not default to Top down"))
        return 1;
    const QString root_id = QStringLiteral("93f0de41");
    const QString head_id = QStringLiteral("c8d102ab");
    if (!require(graph->revisionViewportPosition(root_id).y()
                     < graph->revisionViewportPosition(head_id).y(),
                 "default Top down graph must place root above HEAD"))
        return 1;
    // Automatic startup fit must match an explicit press of Fit graph, not
    // merely use the temporary size from widget construction.
    QPushButton *fit_button = projects.findChild<QPushButton *>(
        QStringLiteral("projectsGraphFitButton"));
    if (!require(fit_button != nullptr, "Fit graph control is missing"))
        return 1;
    // Graph layout changes can schedule a deferred initial fit. Compare only
    // after that fit has completed, not during the settling interval.
    waitFor(110);
    const double startup_zoom = graph->zoomFactor();
    const QPointF startup_head = graph->revisionViewportPosition(head_id);
    fit_button->click();
    if (!require(near(startup_zoom, graph->zoomFactor())
                     && near(startup_head.x(), graph->revisionViewportPosition(head_id).x())
                     && near(startup_head.y(), graph->revisionViewportPosition(head_id).y()),
                 "first graph display was not already fitted as if Fit graph was clicked"))
        return 1;
    // Both sidebar trees keep their existing minimum heights and consume
    // additional vertical space as the Projects page becomes taller.
    GroupBoxCollapsible *project_section = projects.findChild<GroupBoxCollapsible *>(
        QStringLiteral("projectsCollapsibleGroup"));
    GroupBoxCollapsible *revision_section = projects.findChild<GroupBoxCollapsible *>(
        QStringLiteral("revisionsCollapsibleGroup"));
    // QGroupBox uses && for a visible ampersand rather than a mnemonic.
    if (!require(project_section != nullptr && revision_section != nullptr
                     && project_section->title() == QStringLiteral("Projects && Branches"),
                 "project/branch section is missing or incorrectly titled"))
        return 1;
    if (!require(server_section->isVisible() && server_section->y() < project_section->y(),
                 "Servers section must appear above Projects & Branches"))
        return 1;
    // The navigator has no create/import button row. Its only job is navigation.
    QScrollArea *sidebar = projects.findChild<QScrollArea *>(
        QStringLiteral("projectsSidebarScrollArea"));
    QPushButton *browse_action = projects.findChild<QPushButton *>(
        QStringLiteral("projectsBrowseExamplesButton"));
    if (!require(sidebar != nullptr && browse_action != nullptr
                     && projects.findChild<QPushButton *>(QStringLiteral("projectsCreateProjectButton")) == nullptr
                     && projects.findChild<QPushButton *>(QStringLiteral("projectsCreateBranchButton")) == nullptr
                     && projects.findChild<QPushButton *>(QStringLiteral("projectsImportButton")) == nullptr,
                 "obsolete navigator creation toolbar remains")) return 1;
    const auto sidebar_fits = [&]()
    {
        return sidebar->horizontalScrollBarPolicy() == Qt::ScrollBarAsNeeded
            && sidebar->horizontalScrollBar()->maximum() == 0
            && !sidebar->horizontalScrollBar()->isVisible();
    };
    if (!require(sidebar_fits(), "navigator overflows horizontally")) return 1;
    splitter->setSizes(QList<int>{240, 900});
    app.processEvents();
    if (!require(sidebar_fits(), "narrow navigator overflows horizontally")) return 1;
    splitter->setSizes(QList<int>{295, 720});
    app.processEvents();

    const int default_project_height = project_tree->height();
    const int default_revision_height = revisions->height();
    const int default_project_section_height = project_section->height();
    const int default_revision_section_height = revision_section->height();
    const int default_servers_section_height = server_section->height();
    const int default_content_height = sidebar->widget()->height();
    const int default_viewport_height = sidebar->viewport()->height();
    if (!require(default_project_height >= 190 && default_revision_height >= 220,
                 "sidebar trees fell below their existing minimum heights"))
        return 1;
    projects.resize(1250, 1040);
    app.processEvents();
    const int project_growth = project_tree->height() - default_project_height;
    const int revision_growth = revisions->height() - default_revision_height;
    const int project_section_growth = project_section->height() - default_project_section_height;
    const int revision_section_growth = revision_section->height() - default_revision_section_height;
    // The initial sidebar can scroll vertically because its three sections
    // already exceed the short viewport. A fixed "+50 px per tree" threshold
    // mistakes viewport growth for *available* growth; much of the resize
    // first removes that initial overflow. Verify instead that both trees
    // actually grow and that the height received by each section is passed
    // through to its tree (rather than becoming empty space in its group).
    // A caption may rewrap when the sidebar's vertical scrollbar disappears,
    // changing the non-tree portion of a group by one line of text.
    const int caption_reflow_tolerance = 24;
    const bool trees_absorb_section_growth =
        project_growth >= 20 && revision_growth >= 20
        && std::abs(project_growth - project_section_growth) <= caption_reflow_tolerance
        && std::abs(revision_growth - revision_section_growth) <= caption_reflow_tolerance;
    const bool server_remains_compact =
        std::abs(server_section->height() - default_servers_section_height) <= 2;
    const bool content_fills_viewport =
        std::abs(sidebar->widget()->height() - sidebar->viewport()->height()) <= 2;
    if (!(trees_absorb_section_growth && server_remains_compact && content_fills_viewport))
    {
        std::fprintf(stderr,
            "projects_inspector_dock: sidebar tree growth (initial -> tall): "
            "projects %d -> %d (section %+d), revisions %d -> %d (section %+d), "
            "servers tree %d, servers section %d -> %d (hint %d); "
            "viewport height %d -> %d, content height %d -> %d\n",
            default_project_height, project_tree->height(), project_section_growth,
            default_revision_height, revisions->height(), revision_section_growth,
            servers->height(), default_servers_section_height, server_section->height(),
            server_section->sizeHint().height(),
            default_viewport_height, sidebar->viewport()->height(),
            default_content_height, sidebar->widget()->height());
        if (!require(false, "sidebar extra height was not allocated to the two trees"))
            return 1;
    }
    const int expanded_revision_height = revisions->height();
    project_section->setCollapsed(true);
    app.processEvents();
    if (!require(!project_tree->isVisible() && revisions->isVisible()
                     && revisions->height() > expanded_revision_height + 50,
                 "collapsing Projects & Branches did not expand the Revisions tree"))
        return 1;
    project_section->setCollapsed(false);
    app.processEvents();
    const int expanded_project_height = project_tree->height();
    revision_section->setCollapsed(true);
    app.processEvents();
    if (!require(!revisions->isVisible() && project_tree->isVisible()
                     && project_tree->height() > expanded_project_height + 50,
                 "collapsing Revisions did not expand the Projects & Branches tree"))
        return 1;
    revision_section->setCollapsed(false);
    projects.resize(1250, 740);
    app.processEvents();
    if (!require(project_tree->height() >= 190 && revisions->height() >= 220,
                 "restoring sidebar sections lost their minimum tree heights"))
        return 1;

    orientation_toggle->click();
    if (!require(orientation_toggle->isChecked() && !graph->isTopDown()
                     && graph->revisionViewportPosition(head_id).y()
                        < graph->revisionViewportPosition(root_id).y(),
                 "Bottom up toggle did not put HEAD above root"))
        return 1;
    orientation_toggle->click();
    if (!require(!orientation_toggle->isChecked() && graph->isTopDown(),
                 "Top down toggle did not restore the default orientation"))
        return 1;
    if (!require(projects.findChild<QToolButton *>(QStringLiteral("projectsInspectorToggle")) == nullptr,
                 "obsolete inspector toggle must not exist above the graph"))
        return 1;

    const double zoom_before_close = graph->zoomFactor();
    dock->close();
    app.processEvents();
    if (!require(graph->zoomFactor() == zoom_before_close,
                 "closing the inspector unexpectedly reset graph zoom"))
        return 1;
    if (!require(!dock->isVisible(), "closing inspector did not hide the dock"))
        return 1;

    // Closing is sticky while the workspace is idle. Graph camera actions
    // must not unexpectedly restore the inspector.
    graph->fitGraph();
    app.processEvents();
    if (!require(!dock->isVisible(), "graph navigation unexpectedly reopened the inspector"))
        return 1;

    // A revision selected in the left tree changes inspector contents and
    // therefore automatically restores the dock.
    if (!require(revisions->topLevelItemCount() > 0,
                 "preview revision tree is unexpectedly empty"))
        return 1;
    revisions->setCurrentItem(find_revision(main_head_id));
    app.processEvents();
    if (!require(dock->isVisible() && activeInspectorName(projects)->text() == QStringLiteral("Validated network"),
                 "selecting a revision in the tree did not reopen its inspector"))
        return 1;

    dock->close();
    app.processEvents();
    if (!require(project_tree->topLevelItemCount() >= 2, "preview project tree is empty"))
        return 1;
    const double zoom_before_project_selection = graph->zoomFactor();
    project_tree->setCurrentItem(project_tree->topLevelItem(1));
    app.processEvents();
    if (!require(near(zoom_before_project_selection, graph->zoomFactor()),
                 "sidebar project selection unexpectedly fitted the graph")) return 1;
    if (!require(dock->isVisible() && activeInspectorName(projects)->text() == QStringLiteral("Sample Pump Station"),
                 "selecting a project in the tree did not reopen its inspector"))
        return 1;
    const QString selected_project_key = project_tree->currentItem()->data(0, Qt::UserRole).toString();
    if (!require(graph->selectionId() == QStringLiteral("group:project:")
                     + selected_project_key.mid(2),
                 "sidebar project selection did not highlight its graph card")) return 1;
    // The same branch-creation workflow is available at the top of Overview
    // and below the branch list on Branches (even if empty).
    QWidget *project_overview_page = activeInspectorOverview(projects);
    QWidget *project_branches_page = inspector_tabs->count() > 1
        ? inspector_tabs->widget(1) : nullptr;
    QPushButton *project_import = project_overview_page == nullptr
        ? nullptr : project_overview_page->findChild<QPushButton *>(
              QStringLiteral("projectsInspectorImport"));
    QPushButton *project_overview_create_branch = project_overview_page == nullptr
        ? nullptr : project_overview_page->findChild<QPushButton *>(
              QStringLiteral("projectsInspectorOverviewCreateBranch"));
    QPushButton *project_create_branch = project_branches_page == nullptr
        ? nullptr : project_branches_page->findChild<QPushButton *>(
              QStringLiteral("projectsInspectorCreateBranch"));
    QListWidget *project_branch_list = project_branches_page == nullptr
        ? nullptr : project_branches_page->findChild<QListWidget *>();
    QVBoxLayout *branch_column = project_branches_page == nullptr
        ? nullptr : qobject_cast<QVBoxLayout *>(project_branches_page->layout());
    QVBoxLayout *project_column = project_overview_page == nullptr
        ? nullptr : qobject_cast<QVBoxLayout *>(project_overview_page->layout());
    QLabel *project_identity = project_overview_page == nullptr
        ? nullptr : project_overview_page->findChild<QLabel *>(
              QStringLiteral("projectInspectorMetadata"));
    QLineEdit *project_name = project_overview_page == nullptr
        ? nullptr : project_overview_page->findChild<QLineEdit *>(
              QStringLiteral("projectInspectorName"));
    QLineEdit *project_tags = project_overview_page == nullptr
        ? nullptr : project_overview_page->findChild<QLineEdit *>(
              QStringLiteral("projectInspectorTags"));
    QTextEdit *project_description = project_overview_page == nullptr
        ? nullptr : project_overview_page->findChild<QTextEdit *>(
              QStringLiteral("projectInspectorDescription"));
    if (!require(inspector_tabs->count() == 2
                     && inspector_tabs->tabText(0) == QStringLiteral("Overview")
                     && inspector_tabs->tabText(1) == QStringLiteral("Branches")
                     && project_import != nullptr
                     && project_overview_create_branch != nullptr
                     && project_create_branch != nullptr && project_branch_list != nullptr
                     && branch_column != nullptr
                     && project_overview_create_branch->parentWidget() == project_overview_page
                     && project_column != nullptr && project_identity != nullptr
                     && project_name != nullptr && project_tags != nullptr
                     && project_description != nullptr
                     && project_name->parentWidget() == project_tags->parentWidget()
                     && project_tags->parentWidget() == project_description->parentWidget()
                     && project_column->indexOf(project_name->parentWidget()) == 0
                     && project_column->indexOf(project_overview_create_branch) == 1
                     && project_column->indexOf(project_identity) == project_column->count() - 1
                     && project_column->itemAt(project_column->count() - 2)->widget() == project_import
                     && branch_column->indexOf(project_create_branch)
                         > branch_column->indexOf(project_branch_list)
                     && project_branches_page->findChild<QPushButton *>(
                         QStringLiteral("projectsInspectorOverviewCreateBranch")) == nullptr
                     && project_branches_page->findChild<QPushButton *>(
                         QStringLiteral("projectsInspectorImport")) == nullptr
                     && project_branches_page->findChild<QPushButton *>(
                         QStringLiteral("projectsInspectorEditNetwork")) == nullptr
                     && activeInspectorAction(projects, QStringLiteral("projectsInspectorImport")) == nullptr
                     && activeInspectorAction(projects, QStringLiteral("projectsInspectorEditNetwork")) == nullptr,
                 "Project Inspector actions do not belong to the requested tabs")) return 1;
    inspector_tabs->setCurrentIndex(1);
    app.processEvents();
    if (!require(project_create_branch->isVisible() && project_create_branch->isEnabled()
                     && !project_import->isVisible(),
                 "Branches tab exposes network actions or hides Create branch")) return 1;
    inspector_tabs->setCurrentIndex(0);
    app.processEvents();
    if (!require(project_import->isVisible() && project_overview_create_branch->isVisible()
                     && project_overview_create_branch->isEnabled()
                     && !project_create_branch->isVisible(),
                 "Overview tab does not expose its Create branch and network actions")) return 1;
    ProjectInspectorRelations empty_branches;
    ProjectPreviewData project_without_branches;
    empty_branches.populate(&project_without_branches);
    if (!require(empty_branches.list->count() == 0
                     && empty_branches.create_branch != nullptr,
                 "an empty branch list removed its Create branch action")) return 1;
    if (!require(activeInspectorAction(projects, QStringLiteral("projectInspectorDeleteButton"))->isVisible()
                     && activeInspectorAction(projects, QStringLiteral("projectInspectorDeleteButton"))->text() == QStringLiteral("Delete project…"),
                 "selecting a project did not offer deletion in the inspector"))
        return 1;
    // A selected card may intentionally be off-screen after manual zoom/pan.
    // Selection must not move the camera to reveal it or implicitly Fit graph.

    // Toolbar-driven project and branch changes use the same selection path.
    dock->close();
    projects.selectProjectByName(QStringLiteral("Sample North Network"));
    app.processEvents();
    if (!require(dock->isVisible() && activeInspectorName(projects)->text() == QStringLiteral("Sample North Network"),
                 "toolbar project change did not reopen its inspector"))
        return 1;

    dock->close();
    projects.selectBranchByName(QStringLiteral("pressure-study"));
    app.processEvents();
    if (!require(dock->isVisible() && activeInspectorName(projects)->text() == QStringLiteral("pressure-study"),
                 "toolbar branch change did not reopen its inspector"))
        return 1;
    if (!require(graph->selectionId().startsWith(QStringLiteral("group:branch:")),
                 "sidebar branch selection did not highlight its graph card")) return 1;
    if (!require(activeInspectorAction(projects, QStringLiteral("projectInspectorDeleteButton"))->isVisible()
                     && activeInspectorAction(projects, QStringLiteral("projectInspectorDeleteButton"))->text() == QStringLiteral("Delete branch…"),
                 "selecting a branch did not offer deletion in the inspector"))
        return 1;

    // The graph and the left tree must behave identically.
    dock->close();
    graph->revisionActivated(QStringLiteral("ec9045df"));
    app.processEvents();
    if (!require(dock->isVisible() && activeInspectorName(projects)->text() == QStringLiteral("pressure-study"),
                 "selecting a revision on the graph did not reopen its inspector"))
        return 1;
    if (!require(activeInspectorAction(projects, QStringLiteral("projectInspectorDeleteButton")) == nullptr,
                 "immutable revision must not expose a delete action"))
        return 1;
    if (!require(action_stack->currentWidget()->objectName()
                     == QStringLiteral("projectsRevisionInspector")
                     && activeInspectorAction(projects,
                         QStringLiteral("projectsInspectorEditNetwork")) == nullptr,
                 "immutable revision inspector exposes editing actions")) return 1;
    // The revision-opening dialog must lay out its buttons below the form,
    // not as unpositioned children obscuring the revision ID at the top.
    inspector_tabs->setCurrentIndex(0);
    app.processEvents();
    QWidget *revision_network_page = activeInspectorOverview(projects);
    QPushButton *revision_open_network = revision_network_page == nullptr
        ? nullptr : revision_network_page->findChild<QPushButton *>(
            QStringLiteral("projectsNetworkOverviewOpen"));
    if (!require(revision_open_network != nullptr,
                 "revision Overview lacks its Open network action")) return 1;
    // Preview fixtures have no committed hydraulic snapshot; enable the UI
    // action for this layout-only check without touching a repository.
    revision_open_network->setEnabled(true);
    revision_open_network->click();
    app.processEvents();
    QDialog *open_revision_dialog = projects.findChild<QDialog *>(
        QStringLiteral("projectsRevisionOpenDialog"));
    QLabel *open_revision_context = open_revision_dialog == nullptr
        ? nullptr : open_revision_dialog->findChild<QLabel *>(
            QStringLiteral("projectsRevisionOpenContext"));
    QLineEdit *open_revision_branch = open_revision_dialog == nullptr
        ? nullptr : open_revision_dialog->findChild<QLineEdit *>(
            QStringLiteral("projectsRevisionOpenBranchName"));
    QDialogButtonBox *open_revision_buttons = open_revision_dialog == nullptr
        ? nullptr : open_revision_dialog->findChild<QDialogButtonBox *>(
            QStringLiteral("projectsRevisionOpenButtons"));
    QVBoxLayout *open_revision_layout = open_revision_dialog == nullptr
        ? nullptr : qobject_cast<QVBoxLayout *>(open_revision_dialog->layout());
    if (!require(open_revision_dialog != nullptr && open_revision_dialog->isVisible()
                     && open_revision_context != nullptr && open_revision_branch != nullptr
                     && open_revision_buttons != nullptr && open_revision_layout != nullptr
                     && open_revision_layout->indexOf(open_revision_buttons)
                         > open_revision_layout->indexOf(open_revision_branch)
                     && open_revision_buttons->geometry().top()
                         > open_revision_branch->geometry().bottom()
                     && open_revision_branch->geometry().top()
                         > open_revision_context->geometry().bottom()
                     && open_revision_buttons->geometry().bottom()
                         <= open_revision_dialog->contentsRect().bottom(),
                 "Open immutable revision dialog overlaps its content or loses action buttons"))
        return 1;
    open_revision_dialog->reject();
    app.processEvents();
    QPushButton *revision_branch = activeInspectorAction(projects,
        QStringLiteral("projectsInspectorCreateBranch"));
    QPushButton *revision_commit = activeInspectorAction(projects,
        QStringLiteral("projectsHeadInspectorCommit"));
    QWidget *revision_form = projects.findChild<QWidget *>(
        QStringLiteral("projectsRevisionOverviewForm"));
    QPlainTextEdit *revision_message = revision_form != nullptr
        ? revision_form->findChild<QPlainTextEdit *>(QStringLiteral("projectsRevisionCommitMessage")) : nullptr;
    QLabel *revision_identity = action_stack->currentWidget()->findChild<QLabel *>(
        QStringLiteral("projectsRevisionIdentityFooter"));
    if (!require(revision_branch != nullptr && revision_branch->isEnabled()
                     && revision_commit != nullptr && !revision_commit->isVisible()
                     && activeInspectorAction(projects,
                         QStringLiteral("projectsRevisionSaveMetadata")) == nullptr
                     && revision_message != nullptr && revision_message->isEnabled()
                     && revision_identity != nullptr,
                 "Revision Inspector did not offer editable message and branch creation")) return 1;
    QLabel *overview_branch = revision_form != nullptr
        ? revision_form->findChild<QLabel *>(QStringLiteral("projectsRevisionBranchLabel")) : nullptr;
    QWidget *revision_branch_tab = projects.findChild<QWidget *>(
        QStringLiteral("projectsRevisionBranchTab"));
    QLineEdit *revision_branch_edit = revision_branch_tab != nullptr
        ? revision_branch_tab->findChild<QLineEdit *>(QStringLiteral("projectInspectorName")) : nullptr;
    QLineEdit *revision_branch_tags = revision_branch_tab != nullptr
        ? revision_branch_tab->findChild<QLineEdit *>(QStringLiteral("projectsRevisionBranchTags")) : nullptr;
    QTextEdit *revision_branch_description = revision_branch_tab != nullptr
        ? revision_branch_tab->findChild<QTextEdit *>(
              QStringLiteral("projectsRevisionBranchDescription")) : nullptr;
    QLineEdit *overview_tags = revision_form != nullptr
        ? revision_form->findChild<QLineEdit *>(QStringLiteral("projectInspectorTags")) : nullptr;
    QTextEdit *overview_description = revision_form != nullptr
        ? revision_form->findChild<QTextEdit *>(QStringLiteral("projectInspectorDescription")) : nullptr;
    QLabel *branch_name_label = nullptr;
    if (revision_branch_tab != nullptr)
        for (QFormLayout *form : revision_branch_tab->findChildren<QFormLayout *>())
            branch_name_label = qobject_cast<QLabel *>(form->labelForField(revision_branch_edit));
    if (!require(head_branch_page == revision_branch_tab
                     && inspector_tabs->indexOf(head_branch_page) == 2
                     && !head_branch_page->isVisible(),
                 "HEAD/Revision Branch page is visible outside its selected tab")) return 1;
    inspector_tabs->setCurrentIndex(2);
    app.processEvents();
    if (!require(head_branch_page->isVisible() && revision_branch_edit->isVisible()
                     && revision_branch_tags != nullptr && revision_branch_tags->isVisible()
                     && revision_branch_description != nullptr
                     && revision_branch_description->isVisible()
                     && revision_branch_tags->parentWidget() == revision_branch_tab
                     && revision_branch_description->parentWidget() == revision_branch_tab,
                 "HEAD/Revision Branch tab lacks visible Tags and Description editors")) return 1;
    inspector_tabs->setCurrentIndex(1);
    app.processEvents();
    if (!require(!head_branch_page->isVisible(),
                 "HEAD/Revision Branch editor escaped its tab on return to Details")) return 1;
    if (!require(revision_form != nullptr && revision_form->isVisible()
                     && revision_message != nullptr && overview_branch != nullptr
                     && overview_branch->text() == QStringLiteral("pressure-study")
                     && revision_branch_edit != nullptr && branch_name_label != nullptr
                     && branch_name_label->text() == QStringLiteral("Branch name")
                     && inspector_tabs->tabText(1) == QStringLiteral("Revision")
                     && inspector_tabs->tabText(2) == QStringLiteral("Branch")
                     && revision_branch->parentWidget() == revision_form
                     && revision_commit->parentWidget() == revision_form,
                 "Revision inspector did not separate branch editing from Overview")) return 1;
    if (!require(revision_message->minimumHeight() == revision_message->maximumHeight()
                     && revision_message->height() == revision_message->minimumHeight()
                     && revision_message->height() >= 2 * revision_message->fontMetrics().lineSpacing()
                     && revision_message->height() < 4 * revision_message->fontMetrics().lineSpacing()
                         + 2 * revision_message->frameWidth() + 16,
                 "commit message editor is not constrained to two visible text lines")) return 1;
    // Typing in Branch previews the name on Overview without modifying revisions.
    revision_branch_edit->setText(QStringLiteral("pressure-study-next"));
    if (!require(overview_branch->text() == QStringLiteral("pressure-study-next"),
                 "Overview branch label did not track Branch tab edits")) return 1;
    revision_branch_edit->setText(QStringLiteral("pressure-study"));
    if (!require(overview_tags != nullptr && overview_description != nullptr
                     && revision_branch_tags->text() == overview_tags->text()
                     && revision_branch_description->toPlainText()
                         == overview_description->toPlainText(),
                 "Branch metadata did not load from the selected revision")) return 1;
    const QString original_tags = overview_tags->text();
    const QString original_description = overview_description->toPlainText();
    revision_branch_tags->setText(QStringLiteral("hydraulic, reviewed"));
    revision_branch_description->setPlainText(QStringLiteral("Edited in Branch tab"));
    if (!require(overview_tags->text() == QStringLiteral("hydraulic, reviewed")
                     && overview_description->toPlainText()
                         == QStringLiteral("Edited in Branch tab"),
                 "Branch tab edits did not synchronize to Overview/autosave fields")) return 1;
    overview_tags->setText(QStringLiteral("overview, updated"));
    overview_description->setPlainText(QStringLiteral("Edited in Overview"));
    if (!require(revision_branch_tags->text() == QStringLiteral("overview, updated")
                     && revision_branch_description->toPlainText()
                         == QStringLiteral("Edited in Overview"),
                 "Overview metadata edits did not synchronize to the Branch tab")) return 1;
    overview_tags->setText(original_tags);
    overview_description->setPlainText(original_description);

    dock->close();
    const double zoom_before_graph_selection = graph->zoomFactor();
    graph->headActivated(QStringLiteral("north-main"));
    app.processEvents();
    if (!require(near(zoom_before_graph_selection, graph->zoomFactor()),
                 "graph HEAD selection unexpectedly fitted the graph")) return 1;
    if (!require(dock->isVisible() && activeInspectorName(projects)->text() == QStringLiteral("main")
                     && revision_branch_tags->text() == overview_tags->text()
                     && revision_branch_description->toPlainText()
                         == overview_description->toPlainText()
                     && action_stack->currentWidget()->objectName()
                         == QStringLiteral("projectsRevisionInspector")
                     && inspector_tabs->count() == 3
                     && inspector_tabs->tabText(1) == QStringLiteral("HEAD")
                     && inspector_tabs->tabText(2) == QStringLiteral("Branch")
                     && activeInspectorAction(projects,
                         QStringLiteral("projectsHeadInspectorEditNetwork")) == nullptr,
                 "selecting a HEAD card did not open its HEAD/Revision Inspector"))
        return 1;
    QPushButton *head_branch = activeInspectorAction(projects,
        QStringLiteral("projectsInspectorCreateBranch"));
    QPushButton *head_commit = activeInspectorAction(projects,
        QStringLiteral("projectsHeadInspectorCommit"));
    QWidget *head_form = projects.findChild<QWidget *>(
        QStringLiteral("projectsRevisionOverviewForm"));
    QPlainTextEdit *head_message = head_form != nullptr
        ? head_form->findChild<QPlainTextEdit *>(QStringLiteral("projectsRevisionCommitMessage")) : nullptr;
    if (!require(head_branch != nullptr && !head_branch->isEnabled()
                     && head_branch->toolTip().contains(QStringLiteral("Commit HEAD first"))
                     && head_commit != nullptr && head_commit->isVisible()
                     && !head_commit->isEnabled() && head_message != nullptr
                     && head_message->isEnabled(),
                 "HEAD Inspector must require a commit message and disable branching")) return 1;
    QLabel *head_identity = action_stack->currentWidget()->findChild<QLabel *>(
        QStringLiteral("projectsRevisionIdentityFooter"));
    if (!require(head_form != nullptr
                     && head_identity != nullptr
                     && head_identity->text().contains(QStringLiteral("Preview project ID"))
                     && head_identity->text().contains(QStringLiteral("Base revision")),
                 "HEAD Overview lost its project identity or base revision")) return 1;
    head_message->setPlainText(QStringLiteral("Commit the hydraulic changes"));
    app.processEvents();
    if (!require(!head_commit->isEnabled(),
                 "preview HEAD offered a commit without a persistent private working copy")) return 1;
    QWidget *network_overview = activeInspectorOverview(projects);
    QLabel *network_branch = network_overview != nullptr
        ? network_overview->findChild<QLabel *>(
            QStringLiteral("projectsNetworkOverviewBranch")) : nullptr;
    QPushButton *network_open = network_overview != nullptr
        ? network_overview->findChild<QPushButton *>(
            QStringLiteral("projectsNetworkOverviewOpen")) : nullptr;
    QPushButton *network_import = network_overview != nullptr
        ? network_overview->findChild<QPushButton *>(
            QStringLiteral("projectsNetworkOverviewImport")) : nullptr;
    QPushButton *network_export = network_overview != nullptr
        ? network_overview->findChild<QPushButton *>(
            QStringLiteral("projectsNetworkOverviewExport")) : nullptr;
    if (!require(network_branch != nullptr
                     && network_branch->text().contains(QStringLiteral("main"))
                     && network_open != nullptr
                     && network_open->text() == QStringLiteral("Open network")
                     && network_import != nullptr
                     && network_export != nullptr && !network_import->isEnabled()
                     && !network_export->isEnabled(),
                 "HEAD network Overview must not expose unscoped import/export")) return 1;
    if (!require(graph->selectionId() == QStringLiteral("group:branch:north-main"),
                 "graph HEAD activation did not synchronize the selected HEAD")) return 1;
    // The highlighted branch row must remain clickable after HEAD selection,
    // even though the row's selection has not changed.
    const QList<QTreeWidgetItem *> branch_rows = project_tree->selectedItems();
    if (!require(branch_rows.size() == 1
                     && branch_rows.first()->data(0, Qt::UserRole).toString().endsWith(
                         QStringLiteral("north-main")),
                 "HEAD selection lost synchronization with the branch tree")) return 1;
    project_tree->itemClicked(branch_rows.first(), 0);
    app.processEvents();
    if (!require(action_stack->currentWidget()->objectName()
                     == QStringLiteral("projectsBranchInspector"),
                 "clicking the selected branch row did not open its Branch Inspector")) return 1;

    // Return to a revision; then close it and ask for History again.
    graph->revisionActivated(QStringLiteral("ec9045df"));
    app.processEvents();
    // Explicitly requesting History should reopen even when the same revision
    // is already selected, because its details are the requested action.
    dock->close();
    projects.showRevisionHistory();
    app.processEvents();
    if (!require(dock->isVisible() && activeInspectorName(projects)->text() == QStringLiteral("pressure-study"),
                 "History did not reopen the already selected revision"))
        return 1;

    // The bundled examples must NOT be injected into the selected server's
    // project tree. Their library works independently of that server.
    for (QTreeWidgetItemIterator it(project_tree); *it; ++it)
        if (!require(!(*it)->data(0, Qt::UserRole).toString().startsWith(QStringLiteral("e:"))
                         && (*it)->data(0, Qt::UserRole).toString() != QStringLiteral("examples-root"),
                     "built-in examples leaked into the server project hierarchy"))
            return 1;

    QString opened_resource;
    QString opened_file;
    QObject::connect(&projects, &ProjectsWidget::builtinExampleRequested, &projects,
        [&](const QString &resource, const QString &file)
    {
        opened_resource = resource;
        opened_file = file;
    });
    bool library_opened = false;
    QTimer::singleShot(0, &projects, [&]()
    {
        QDialog *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) return;
        QListWidget *list = dialog->findChild<QListWidget *>(QStringLiteral("exampleLibraryList"));
        QPushButton *open = dialog->findChild<QPushButton *>(QStringLiteral("exampleLibraryOpenButton"));
        QPushButton *create = dialog->findChild<QPushButton *>(QStringLiteral("exampleLibraryCreateButton"));
        QLineEdit *filter = dialog->findChild<QLineEdit *>(QStringLiteral("exampleLibraryFilter"));
        library_opened = list != nullptr && list->count() == 6 && open != nullptr
                         && create != nullptr && filter != nullptr;
        if (!library_opened) { dialog->reject(); return; }
        filter->setText(QStringLiteral("leakage"));
        library_opened = list->currentItem() != nullptr
            && list->currentItem()->text().contains(QStringLiteral("Leakage"))
            && open->isEnabled();
        if (library_opened) open->click();
        else dialog->reject();
    });
    browse_action->click();
    if (!require(library_opened
                     && opened_resource == QStringLiteral(":/examples/epanet/tutorial-leakage.inp")
                     && opened_file == QStringLiteral("tutorial-leakage.inp"),
                 "Example Library did not open the selected bundled resource through the existing importer"))
        return 1;

    // A template can be converted to an in-memory project on ANY configured
    // server, with an initial branch/revision placeholder. No import or DB
    // write occurs in this path.
    bool wizard_ok = false;
    opened_resource.clear();
    QTimer::singleShot(0, &projects, [&]()
    {
        QDialog *browser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (browser == nullptr) return;
        QListWidget *list = browser->findChild<QListWidget *>(QStringLiteral("exampleLibraryList"));
        QPushButton *create = browser->findChild<QPushButton *>(QStringLiteral("exampleLibraryCreateButton"));
        if (list == nullptr || create == nullptr) { browser->reject(); return; }
        list->setCurrentRow(0); // NET3
        QTimer::singleShot(0, &projects, [&]()
        {
            QDialog *wizard = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (wizard == nullptr) return;
            QComboBox *server = wizard->findChild<QComboBox *>(QStringLiteral("exampleCreateServer"));
            QLineEdit *project_name = wizard->findChild<QLineEdit *>(QStringLiteral("exampleCreateProjectName"));
            QLineEdit *branch_name = wizard->findChild<QLineEdit *>(QStringLiteral("exampleCreateBranchName"));
            QPushButton *confirm = wizard->findChild<QPushButton *>(QStringLiteral("exampleCreateConfirmButton"));
            QLabel *notice = wizard->findChild<QLabel *>(QStringLiteral("exampleCreatePreviewNotice"));
            wizard_ok = server != nullptr && server->count() == 2 && project_name != nullptr
                && branch_name != nullptr && confirm != nullptr && notice != nullptr
                && notice->text().contains(QStringLiteral("not copied"));
            if (!wizard_ok) { wizard->reject(); return; }
            server->setCurrentIndex(1); // Remote preview
            project_name->setText(QStringLiteral("NET3 Remote Experiment"));
            branch_name->setText(QStringLiteral("main"));
            wizard_ok = confirm->isEnabled();
            if (wizard_ok) confirm->click();
            else wizard->reject();
        });
        create->click();
    });
    browse_action->click();
    app.processEvents();
    if (!require(wizard_ok && opened_resource.isEmpty()
                     && activeInspectorName(projects)->text() == QStringLiteral("NET3 Remote Experiment")
                     && project_tree->topLevelItemCount() == 1
                     && revisions->topLevelItemCount() == 1
                     && revisions->topLevelItem(0)->text(0) == QStringLiteral("Initial import (preview)"),
                 "creating a project from an example did not create a remote-scoped preview"))
        return 1;
    QLabel *metadata = activeInspectorMetadata(projects);
    if (!require(metadata != nullptr
                     && metadata->text().contains(QStringLiteral("NOT stored")),
                 "example-origin projects must disclose that no network was saved"))
        return 1;
    // Restore the original preview context for the rest of the regression.
    servers->setCurrentItem(servers->topLevelItem(0));
    projects.selectBranchByName(QStringLiteral("main"));
    app.processEvents();

    // Reproduce the real application hierarchy: Projects is a page in a
    // stacked navigator, not a top-level window. The previous regression
    // left only the Projects header visible inside this layout.
    QStackedWidget navigator;
    navigator.resize(1320, 760);
    ProjectsWidget *embedded_projects = new ProjectsWidget(&navigator);
    navigator.addWidget(embedded_projects);
    navigator.addWidget(new QWidget(&navigator));
    navigator.setCurrentIndex(1);
    navigator.show();
    app.processEvents();
    navigator.setCurrentIndex(0);
    app.processEvents();
    waitFor(120); // stack activation is a separate first-visible layout path

    QMainWindow *embedded_host = embedded_projects->findChild<QMainWindow *>(
        QStringLiteral("projectsDockHost"));
    QTreeWidget *embedded_tree = embedded_projects->findChild<QTreeWidget *>(
        QStringLiteral("projectsTree"));
    ProjectRevisionGraph *embedded_graph = embedded_projects->findChild<ProjectRevisionGraph *>(
        QStringLiteral("projectRevisionGraph"));
    QDockWidget *embedded_dock = embedded_projects->findChild<QDockWidget *>(
        QStringLiteral("projectsInspectorDock"));
    if (!require(embedded_host != nullptr && !embedded_host->isWindow()
                     && embedded_host->isVisible()
                     && embedded_host->width() > 500 && embedded_host->height() > 300,
                 "embedded projects dock host disappeared after page activation"))
        return 1;
    if (!require(embedded_tree != nullptr && embedded_tree->isVisible()
                     && embedded_graph != nullptr && embedded_graph->isVisible()
                     && embedded_graph->width() > 200 && embedded_graph->height() > 200
                     && embedded_dock != nullptr && embedded_dock->isVisible(),
                 "embedded page must show project tree, graph and inspector together"))
        return 1;
    const double embedded_startup_zoom = embedded_graph->zoomFactor();
    const QPointF embedded_head_before = embedded_graph->revisionViewportPosition(head_id);
    embedded_graph->fitGraph();
    const QPointF embedded_head_after = embedded_graph->revisionViewportPosition(head_id);
    if (!require(near(embedded_startup_zoom, embedded_graph->zoomFactor())
                     && near(embedded_head_before.x(), embedded_head_after.x())
                     && near(embedded_head_before.y(), embedded_head_after.y()),
                 "the initially hidden Projects tab was not automatically fitted on activation"))
        return 1;
    embedded_dock->close();
    app.processEvents();
    if (!require(!embedded_dock->isVisible() && embedded_tree->isVisible()
                     && embedded_graph->isVisible() && embedded_graph->width() > 200,
                 "closing the embedded inspector also hid the project tree or graph"))
        return 1;
    QTreeWidget *embedded_revisions = embedded_projects->findChild<QTreeWidget *>(
        QStringLiteral("revisionsTree"));
    if (!require(embedded_revisions != nullptr && embedded_revisions->topLevelItemCount() > 0,
                 "embedded revision list is missing"))
        return 1;
    embedded_revisions->setCurrentItem(embedded_revisions->topLevelItem(0));
    app.processEvents();
    if (!require(embedded_dock->isVisible() && embedded_tree->isVisible()
                     && embedded_graph->isVisible(),
                 "selection did not restore the inspector in an embedded Projects page"))
        return 1;

    std::fprintf(stderr, "projects_inspector_dock: inspector navigation checks complete\n");
    // Deleting from the inspector still performs the original preview-only
    // action, including its confirmation, without sidebar Delete buttons.
    ProjectsWidget delete_preview;
    delete_preview.resize(1200, 740);
    delete_preview.show();
    app.processEvents();
    QPushButton *delete_action = delete_preview.findChild<QPushButton *>(
        QStringLiteral("projectInspectorDeleteButton"));
    QTreeWidget *delete_projects = delete_preview.findChild<QTreeWidget *>(
        QStringLiteral("projectsTree"));
    if (!require(delete_action != nullptr && delete_projects != nullptr,
                 "inspector deletion regression setup failed"))
        return 1;
    delete_preview.selectProjectByName(QStringLiteral("Sample Pump Station"));
    app.processEvents();
    delete_action = activeInspectorAction(delete_preview,
        QStringLiteral("projectInspectorDeleteButton"));
    if (!require(delete_action != nullptr && delete_action->text() == QStringLiteral("Delete project…"),
                 "inspector project deletion is not available"))
        return 1;
    bool confirmed_deletion = false;
    QTimer confirmation_timer;
    confirmation_timer.setInterval(1);
    QObject::connect(&confirmation_timer, &QTimer::timeout, [&]
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            QMessageBox *confirmation = qobject_cast<QMessageBox *>(widget);
            if (confirmation != nullptr && confirmation->isVisible()
                && confirmation->windowTitle() == QStringLiteral("Delete from preview"))
            {
                confirmed_deletion = true;
                confirmation_timer.stop();
                if (QAbstractButton *yes = confirmation->button(QMessageBox::Yes))
                    yes->click();
                else
                    confirmation->done(QMessageBox::Yes);
                return;
            }
        }
    });
    confirmation_timer.start();
    std::fprintf(stderr, "projects_inspector_dock: confirming preview project deletion\n");
    delete_action->click();
    confirmation_timer.stop();
    if (!require(confirmed_deletion, "inspector Delete project confirmation was not displayed"))
        return 1;
    app.processEvents();
    bool deleted_project_present = false;
    bool retained_project_present = false;
    QStringList project_keys_after_delete;
    for (QTreeWidgetItemIterator it(delete_projects); *it; ++it)
    {
        const QString key = (*it)->data(0, Qt::UserRole).toString();
        if (key.startsWith(QStringLiteral("p:"))) project_keys_after_delete.append(key);
        if (key == QStringLiteral("p:preview-south")) deleted_project_present = true;
        if (key == QStringLiteral("p:preview-north")) retained_project_present = true;
    }
    if (deleted_project_present || !retained_project_present)
    {
        std::fprintf(stderr, "projects_inspector_dock: project IDs after deletion: %s\n",
                     project_keys_after_delete.join(QStringLiteral(", ")).toUtf8().constData());
        if (!require(false, "inspector Delete project action did not remove the preview project"))
            return 1;
    }

    // The compact + opens the inspector; only Create persists a full profile.
    add_server->click();
    app.processEvents();
    if (!require(inspector_tabs->tabText(inspector_tabs->currentIndex())
                     == QStringLiteral("Connection")
                     && !inspector_tabs->isTabEnabled(0)
                     && !inspector_tabs->tabToolTip(0).isEmpty(),
                 "new database must open Connection with Overview disabled and explained")) return 1;
    QPushButton *create_source = projects.findChild<QPushButton *>(
        QStringLiteral("projectSourceCreateButton"));
    QPushButton *cancel_source = projects.findChild<QPushButton *>(
        QStringLiteral("projectSourceCancelButton"));
    QComboBox *source_kind = projects.findChild<QComboBox *>(
        QStringLiteral("projectSourceKind"));
    if (!require(create_source != nullptr && cancel_source != nullptr
                     && create_source->isVisible() && source_kind != nullptr
                     && loadProjectServerProfiles().size() == 2,
                 "opening the creation inspector should not save a name-only profile"))
        return 1;
    QLineEdit *source_name = projects.findChild<QLineEdit *>(
        QStringLiteral("projectsNewSourceName"));
    if (!require(source_name != nullptr && source_name->isVisible(),
                 "new database name is not editable on the Connection tab")) return 1;
    source_name->setText(QStringLiteral("Added server"));
    endpoint->setText(QStringLiteral("http://127.0.0.1:1"));
    bool accepted_pending = false;
    QTimer pending_confirmation_timer;
    pending_confirmation_timer.setInterval(1);
    QObject::connect(&pending_confirmation_timer, &QTimer::timeout, [&]
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            QMessageBox *confirmation = qobject_cast<QMessageBox *>(widget);
            if (confirmation != nullptr && confirmation->isVisible()
                && confirmation->windowTitle() == QStringLiteral("Database unreachable"))
            {
                // Click the actual standard button: forcing QDialog::done()
                // bypasses QMessageBox's clicked-button handling and can
                // cause question() to return NoButton instead of Yes.
                if (QAbstractButton *yes = confirmation->button(QMessageBox::Yes))
                {
                    accepted_pending = true;
                    pending_confirmation_timer.stop();
                    yes->click();
                }
                break;
            }
        }
    });
    pending_confirmation_timer.start();
    create_source->click();
    for (int attempt = 0; attempt < 300 && loadProjectServerProfiles().size() != 3; ++attempt)
        waitFor(10);
    pending_confirmation_timer.stop();
    if (!require(accepted_pending, "unreachable database did not ask before saving pending"))
        return 1;
    const QList<ProjectServerProfile> created_profiles = loadProjectServerProfiles();
    if (servers->topLevelItemCount() != 3 || created_profiles.size() != 3)
    {
        std::fprintf(stderr,
            "projects_inspector_dock: after adding remote database: tree=%d profiles=%lld\n",
            servers->topLevelItemCount(), static_cast<long long>(created_profiles.size()));
        if (!require(false, "inspector Create did not persist a configured remote database"))
            return 1;
    }
    if (!require(created_profiles.at(2).connection_type == QStringLiteral("remote")
                     && created_profiles.at(2).pending
                     && DvmdbIdentity::isUuidV7(created_profiles.at(2).id),
                 "new remote database did not retain pending UUIDv7 identity"))
        return 1;
    ProjectsWidget reloaded;
    QTreeWidget *reloaded_servers = reloaded.findChild<QTreeWidget *>(
        QStringLiteral("serversTree"));
    if (!require(reloaded_servers != nullptr && reloaded_servers->topLevelItemCount() == 3
                     && reloaded_servers->topLevelItem(2)->text(0).endsWith(QStringLiteral("Added server")),
                 "created server profile was not restored at startup"))
        return 1;

    // Deletion must remove the profile from disk, without deleting any DVMDB data.
    servers->setCurrentItem(servers->topLevelItem(2));
    app.processEvents();
    if (!require(!head_branch_page->isVisible()
                     && inspector_tabs->indexOf(head_branch_page) < 0,
                 "HEAD/Revision Branch form leaked into a different Database Inspector")) return 1;
    inspector_tabs->setCurrentIndex(1);
    app.processEvents();
    if (!require(delete_connection->isVisible() && delete_connection->isEnabled(),
                 "new database connection cannot be removed from its Connection tab"))
        return 1;
    bool confirmed_server_deletion = false;
    QTimer server_confirmation_timer;
    server_confirmation_timer.setInterval(1);
    QObject::connect(&server_confirmation_timer, &QTimer::timeout, [&]
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            QMessageBox *confirmation = qobject_cast<QMessageBox *>(widget);
            if (confirmation == nullptr || !confirmation->isVisible()
                || confirmation->windowTitle() != QStringLiteral("Remove connection"))
                continue;
            if (QAbstractButton *yes = confirmation->button(QMessageBox::Yes))
            {
                confirmed_server_deletion = true;
                server_confirmation_timer.stop();
                yes->click();
            }
            return;
        }
    });
    server_confirmation_timer.start();
    delete_connection->click();
    server_confirmation_timer.stop();
    if (!require(confirmed_server_deletion,
                 "database profile deletion confirmation was not accepted"))
        return 1;
    app.processEvents();
    const QList<ProjectServerProfile> saved_servers = loadProjectServerProfiles();
    bool added_server_remains = false;
    for (const ProjectServerProfile &profile : saved_servers)
        if (profile.name == QStringLiteral("Added server")) added_server_remains = true;
    if (!require(servers->topLevelItemCount() == 2 && saved_servers.size() == 2
                     && !added_server_remains,
                 "server deletion was not persisted"))
        return 1;

    std::fprintf(stderr, "projects_inspector_dock: preview/database CRUD checks complete\n");
    // An unconfigured HTTP DVMDB must become the personal database directly
    // from Projects Inspector. It must not require an out-of-band UUID input.
    {
        const QString repositoryId = QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6371");
        QTcpServer roleServer;
        if (!require(roleServer.listen(QHostAddress::LocalHost, 0),
                     "could not start local role-configuration test server")) return 1;
        bool isPrivate = false;
        int rolePosts = 0;
        QObject::connect(&roleServer, &QTcpServer::newConnection, &app, [&]()
        {
            while (QTcpSocket *socket = roleServer.nextPendingConnection())
            {
                QByteArray *buffer = new QByteArray();
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [&, socket, buffer, repositoryId]()
                {
                    buffer->append(socket->readAll());
                    const int separator = buffer->indexOf("\r\n\r\n");
                    if (separator < 0) return;
                    const QByteArray headers = buffer->left(separator + 4);
                    int expectedBodyLength = 0;
                    for (const QByteArray &line : headers.split('\n'))
                    {
                        if (line.trimmed().toLower().startsWith("content-length:"))
                            expectedBodyLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (buffer->size() < separator + 4 + expectedBodyLength) return;
                    const QByteArray payload = buffer->mid(separator + 4, expectedBodyLength);
                    const bool tracking = headers.startsWith("GET /dvmdb/v1/repository/tracking ");
                    const bool configure = headers.startsWith("POST /dvmdb/v1/repository/role ");
                    int status = 200;
                    QJsonObject answer;
                    if (tracking)
                    {
                        answer = QJsonObject{{"repositoryId", repositoryId},
                            {"role", isPrivate ? QStringLiteral("private") : QStringLiteral("unconfigured")},
                            {"remotes", QJsonArray()}, {"tracking", QJsonArray()}};
                    }
                    else if (configure)
                    {
                        ++rolePosts;
                        const QJsonObject body = QJsonDocument::fromJson(payload).object();
                        if (body.value("expectedRepositoryId").toString() != repositoryId ||
                            body.value("role").toString() != QStringLiteral("private"))
                            status = 400;
                        else
                            isPrivate = true;
                        answer = QJsonObject{{"repositoryId", repositoryId},
                                             {"role", QStringLiteral("private")}};
                    }
                    else
                    {
                        status = 404;
                        answer = QJsonObject{{"error", QStringLiteral("not found")}};
                    }
                    const QByteArray reply = QJsonDocument(answer).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                        (status == 200 ? " OK\r\n" : " Not Found\r\n") +
                        "Content-Type: application/json\r\nContent-Length: " +
                        QByteArray::number(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply);
                    socket->flush();
                    socket->disconnectFromHost();
                    delete buffer;
                });
            }
        });
        ProjectServerProfile profile;
        profile.id = repositoryId;
        profile.name = QStringLiteral("Personal HTTP DVMDB");
        profile.endpoint = QStringLiteral("http://127.0.0.1:%1").arg(roleServer.serverPort());
        profile.connection_type = QStringLiteral("remote");
        if (!require(saveProjectServerProfiles({profile}),
                     "could not save HTTP personal-role fixture")) return 1;
        ProjectsWidget roleProjects;
        QTreeWidget *tree = roleProjects.findChild<QTreeWidget *>(QStringLiteral("serversTree"));
        QCheckBox *personal = roleProjects.findChild<QCheckBox *>(
            QStringLiteral("projectsPersonalServerCheckbox"));
        if (!require(tree != nullptr && tree->topLevelItemCount() > 0 && personal != nullptr,
                     "could not find HTTP role fixture in Projects")) return 1;
        roleProjects.show();
        QTreeWidgetItem *roleItem = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() ==
                QStringLiteral("s:") + repositoryId) roleItem = tree->topLevelItem(i);
        if (!require(roleItem != nullptr, "role test server not visible")) return 1;
        tree->setCurrentItem(roleItem);
        app.processEvents();
        personal->setChecked(true);
        bool designatedRole = false;
        for (int i = 0; i < 120 && !designatedRole; ++i)
        {
            waitFor(10);
            for (const ProjectServerProfile &candidate : loadProjectServerProfiles())
                if (candidate.id == profile.id && candidate.personal) designatedRole = true;
        }
        const QList<ProjectServerProfile> configured = loadProjectServerProfiles();
        bool pinnedIdentity = false;
        int designatedCount = 0;
        for (const ProjectServerProfile &candidate : configured)
        {
            if (candidate.personal) ++designatedCount;
            if (candidate.id == profile.id && candidate.personal &&
                !candidate.pending && candidate.id == repositoryId) pinnedIdentity = true;
        }
        bool hasCrown = false;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() ==
                QStringLiteral("s:") + repositoryId &&
                tree->topLevelItem(i)->text(0).contains(QStringLiteral("♛"))) hasCrown = true;
        if (!require(isPrivate && rolePosts == 1 && designatedCount == 1 &&
                     pinnedIdentity && hasCrown,
                     "personal HTTP checkbox did not configure role and pin discovered UUID")) return 1;

        // The personal database takes priority over the first configured
        // connection on a fresh workspace, but users may select others later.
        ProjectServerProfile other;
        other.id = QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6372");
        other.name = QStringLiteral("Other database");
        other.endpoint = QStringLiteral("http://127.0.0.1:1");
        other.connection_type = QStringLiteral("remote");
        if (!require(saveProjectServerProfiles({other, configured.first()}),
                     "could not prepare a personal database startup fixture")) return 1;
        ProjectsWidget initialSelection;
        QTreeWidget *initialTree = initialSelection.findChild<QTreeWidget *>(
            QStringLiteral("serversTree"));
        QStackedWidget *initialOverview = initialSelection.findChild<QStackedWidget *>(
            QStringLiteral("projectsInspectorOverviewStack"));
        if (!require(initialTree != nullptr && initialTree->currentItem() != nullptr
                         && initialTree->currentItem()->data(0, Qt::UserRole).toString()
                            == QStringLiteral("s:") + repositoryId
                         && initialOverview != nullptr
                         && initialOverview->currentWidget()->objectName()
                            == QStringLiteral("projectsDatabaseOverviewForm"),
                     "Projects did not initially select the crowned database")) return 1;
        initialTree->setCurrentItem(initialTree->topLevelItem(0));
        app.processEvents();
        if (!require(initialTree->currentItem()->data(0, Qt::UserRole).toString()
                         == QStringLiteral("s:") + other.id,
                     "Projects prevented manual selection of another database")) return 1;
    }

    std::puts("Projects inspector dock tests passed");
    return 0;
}
