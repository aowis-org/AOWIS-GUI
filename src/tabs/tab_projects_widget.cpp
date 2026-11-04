#include "projects/model_codec_compatibility.h"
#include "tabs/tab_projects_widget.h"
#include "projects/dvmdb_identity.h"
#include "config/personal_repository_storage.h"
#include "projects/embedded_dvmdb_host.h"
#include "projects/personal_edit_queue.h"
#include "tabs/project_graph_placement.h"
#include "tabs/personal_working_copy_dialog.h"
#include "tabs/projects_inspector_panels.h"
#include "tabs/projects_inspector_details.h"
#include "tabs/projects_inspector_creation.h"
#include "tabs/projects_inspector_presentation.h"

#include "app/builtin_examples.h"
#include "config/gui_configuration.h"
#include "common/_sizes.h"
#include "widgets/group_box_collapsible.h"
#include "services/http_health_monitor.h"
#include <aowis/model/serialization/model_codec.h>

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QComboBox>
#include <QCheckBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QMainWindow>
#include <QFormLayout>
#include <QFrame>
#include <QHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRadioButton>
#include <QPointer>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSet>
#include <QStandardItemModel>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QScrollArea>
#include <QStyle>
#include <QStackedWidget>
#include <QTabWidget>
#include <QSpacerItem>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>

#include <functional>
#include <memory>
#include <optional>
#include <algorithm>

namespace
{
constexpr int role_key = Qt::UserRole;
QPushButton *compactButton(const QString &label, const QString &tip, QWidget *parent)
{
    QPushButton *button = new QPushButton(label, parent);
    button->setToolTip(tip);
    button->setMinimumHeight(28);
    return button;
}
// QLabel::setWordWrap() alone does not remove a long text size hint from
// a QScrollArea's layout. Permit every explanatory label to shrink to the
// available inspector width so Qt can actually compute its wrapped height.
void wrapInspectorLabel(QLabel *label)
{
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
}
QString keyFor(const QString &type, const QString &id)
{
    return type + QLatin1Char(':') + id;
}
QString projectContextLabel(const ProjectPreviewData &project,
                            const QList<ProjectServerProfile> &servers)
{
    if (project.server_id == QStringLiteral("preview-local")
        || project.server_id == QStringLiteral("preview-remote")) return project.name;
    for (const ProjectServerProfile &server : servers)
        if (server.id == project.server_id)
            return server.name + QStringLiteral(" / ") + project.name;
    return project.name;
}
QTreeWidgetItem *newTreeItem(QTreeWidget *tree, const QString &label, const QString &key)
{
    QTreeWidgetItem *item = new QTreeWidgetItem(tree, QStringList{label});
    item->setData(0, role_key, key);
    item->setToolTip(0, label);
    return item;
}
QTreeWidgetItem *newTreeChild(QTreeWidgetItem *parent, const QString &label, const QString &key)
{
    QTreeWidgetItem *item = new QTreeWidgetItem(parent, QStringList{label});
    item->setData(0, role_key, key);
    item->setToolTip(0, label);
    return item;
}

// Select an origin branch which contains the historical revision in the full
// (both-parent) DAG. Prefer the user's current branch when it qualifies.
QString sourceBranchForRevision(const ProjectPreviewData &project,
                                const QString &revision_id, const QString &preferred)
{
    QHash<QString, QStringList> parents;
    for (const ProjectPreviewRevision &revision : project.revisions)
        parents.insert(revision.id, revision.parents);
    const auto reachable = [&parents, &revision_id](const QString &head)
    {
        QStringList stack{head};
        QSet<QString> visited;
        while (!stack.isEmpty())
        {
            const QString id = stack.takeLast();
            if (id == revision_id) return true;
            if (visited.contains(id)) continue;
            visited.insert(id);
            for (const QString &parent : parents.value(id)) stack.append(parent);
        }
        return false;
    };
    for (const ProjectPreviewBranch &branch : project.branches)
        if (branch.name == preferred && reachable(branch.head)) return branch.name;
    for (const ProjectPreviewBranch &branch : project.branches)
        if (reachable(branch.head)) return branch.name;
    return {};
}
}

struct ProjectsWidget::SourceSession
{
    ProjectSourceReader *reader = nullptr;
    HttpHealthMonitor *health = nullptr; // only remote sources have HTTP health monitoring
    QTimer *refresh = nullptr;
    SourceState state = SourceState::Checking;
    QString repository_id;
    QString error;
    QString codec_error; // explicit version error from the health response
    bool loading = false;
};

ProjectsWidget::~ProjectsWidget()
{
    for (SourceSession *session : this->source_sessions)
    {
        if (session->health != nullptr) session->health->stop();
        session->reader->cancel();
        delete session->refresh;
        delete session->health;
        delete session->reader;
        delete session;
    }
}

ProjectsWidget::ProjectsWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("projectsWorkspace"));
    this->source_writer = new ProjectSourceWriter(this);
    seedPreview();
    this->servers = loadProjectServerProfiles();
    ensureDefaultLocalDvmdbProfile(this->servers);
    this->server_id.clear();
    this->project_id.clear();
    this->branch_id.clear();
    if (!this->servers.isEmpty())
    {
        this->server_id = this->servers.first().id;
        for (const ProjectServerProfile &profile : this->servers)
            if (profile.personal)
            {
                this->server_id = profile.id;
                break;
            }
        for (const ProjectPreviewData &project : this->projects)
            if (project.server_id == this->server_id)
            {
                this->project_id = project.id;
                if (!project.branches.isEmpty())
                    this->branch_id = project.branches.first().id;
                break;
            }
    }

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Give Projects its own dock host: closing/floating this inspector must
    // never hide or rearrange the independent map inspector docks.
    QMainWindow *dock_host = new QMainWindow(this);
    // QMainWindow normally creates a top-level window, even with a parent.
    // The Projects page needs an *embedded* dock host: otherwise the host
    // escapes this page layout and the project tree and graph disappear.
    dock_host->setWindowFlags(Qt::Widget);
    dock_host->setObjectName(QStringLiteral("projectsDockHost"));
    dock_host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    dock_host->setDockNestingEnabled(false);
    outer->addWidget(dock_host, 1);

    QSplitter *splitter = new QSplitter(Qt::Horizontal, dock_host);
    splitter->setObjectName(QStringLiteral("projectsWorkspaceSplitter"));
    splitter->setChildrenCollapsible(false);
    dock_host->setCentralWidget(splitter);

    QScrollArea *left_scroll = new QScrollArea(splitter);
    left_scroll->setWidgetResizable(true);
    // Preserve the normal scroll behavior. The action layouts must fit
    // naturally without disabling horizontal scrolling to hide overflow.
    left_scroll->setObjectName(QStringLiteral("projectsSidebarScrollArea"));
    left_scroll->setFrameShape(QFrame::NoFrame);
    left_scroll->setMinimumWidth(240);
    QWidget *left = new QWidget(left_scroll);
    QVBoxLayout *left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(8, 8, 8, 8);
    left_layout->setSpacing(10);

    // Profiles are persisted locally; each source is opened asynchronously at startup.
    GroupBoxCollapsible *servers_group = new GroupBoxCollapsible(tr("Databases"), left);
    servers_group->setObjectName(QStringLiteral("serversCollapsibleGroup"));
    QVBoxLayout *servers_layout = new QVBoxLayout(servers_group);
    QToolButton *add_server = new QToolButton(servers_group);
    add_server->setText(QStringLiteral("+"));
    add_server->setToolTip(tr("Add a database (local or REST)"));
    add_server->setAccessibleName(tr("Add database"));
    add_server->setObjectName(QStringLiteral("serversCreateButton"));
    servers_group->setHeaderAction(add_server);
    this->server_tree = new QTreeWidget(servers_group);
    this->server_tree->setObjectName(QStringLiteral("serversTree"));
    this->server_tree->setHeaderHidden(true);
    this->server_tree->setMinimumWidth(0);
    this->server_tree->setMinimumHeight(86);
    // Keep the server selector compact. Without an upper bound its QTreeWidget
    // size hint reserves hundreds of pixels before the two revision/project
    // trees receive their stretch, even when only two servers are listed.
    // The tree remains scrollable, and its section can still be collapsed.
    this->server_tree->setMaximumHeight(112);
    this->server_tree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    servers_layout->addWidget(this->server_tree, 1);
    QLabel *no_sources = new QLabel(tr("No databases configured"), servers_group);
    no_sources->setObjectName(QStringLiteral("projectsNoSourcesSidebar"));
    no_sources->setAlignment(Qt::AlignCenter);
    servers_layout->addWidget(no_sources);
    left_layout->addWidget(servers_group, 0);

    // QGroupBox interprets a single & as a mnemonic marker. Use && for a visible ampersand.
    GroupBoxCollapsible *projects_group = new GroupBoxCollapsible(tr("Projects && Branches"), left);
    projects_group->setObjectName(QStringLiteral("projectsCollapsibleGroup"));
    QVBoxLayout *projects_layout = new QVBoxLayout(projects_group);
    // The navigator locates objects; object-specific actions belong to the inspector.
    this->project_tree = new QTreeWidget(projects_group);
    this->project_tree->setObjectName(QStringLiteral("projectsTree"));
    this->project_tree->setHeaderHidden(true);
    this->project_tree->setIndentation(18);
    this->project_tree->setMinimumHeight(190);
    this->project_tree->setMinimumWidth(0);
    this->project_tree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    projects_layout->addWidget(this->project_tree, 1);
    left_layout->addWidget(projects_group, 1);

    // Bundled examples are templates, not objects in the selected DB server.
    // The library stays accessible while any server profile is selected.
    QPushButton *browse_examples_button = nullptr;
    if (guiConfiguration().examples_builtin_enable)
    {
        browse_examples_button = compactButton(tr("Browse examples…"),
            tr("Open a bundled network or prepare a project from it"), left);
        browse_examples_button->setObjectName(QStringLiteral("projectsBrowseExamplesButton"));
        // A long translated label must not widen the sidebar's scroll content.
        browse_examples_button->setMinimumWidth(0);
        browse_examples_button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        left_layout->addWidget(browse_examples_button, 0);
    }

    GroupBoxCollapsible *revisions_group = new GroupBoxCollapsible(tr("Revisions"), left);
    revisions_group->setObjectName(QStringLiteral("revisionsCollapsibleGroup"));
    QVBoxLayout *revisions_layout = new QVBoxLayout(revisions_group);
    // Two exclusive views over the same immutable revisions. The "editable"
    // heads are branch pointers, not mutable historical snapshots.
    QHBoxLayout *revision_views = new QHBoxLayout();
    revision_views->setSpacing(4);
    QPushButton *heads_button = compactButton(tr("Editable heads"),
        tr("Show only the latest revision referenced by each branch"), revisions_group);
    heads_button->setObjectName(QStringLiteral("revisionsHeadsOnlyButton"));
    this->full_tree_button = compactButton(tr("Full tree"),
        tr("Show all revisions nested by their parent relationships"), revisions_group);
    this->full_tree_button->setObjectName(QStringLiteral("revisionsFullTreeButton"));
    QButtonGroup *revision_views_group = new QButtonGroup(revisions_group);
    revision_views_group->setExclusive(true);
    for (QPushButton *button : {heads_button, this->full_tree_button})
    {
        button->setCheckable(true);
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        revision_views_group->addButton(button);
        revision_views->addWidget(button, 1);
    }
    this->full_tree_button->setChecked(true);
    revisions_layout->addLayout(revision_views);
    this->revision_tree = new QTreeWidget(revisions_group);
    this->revision_tree->setObjectName(QStringLiteral("revisionsTree"));
    this->revision_tree->setHeaderHidden(true);
    this->revision_tree->setMinimumHeight(220);
    this->revision_tree->setMinimumWidth(0);
    this->revision_tree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    revisions_layout->addWidget(this->revision_tree, 1);
    left_layout->addWidget(revisions_group, 1);
    // Keep the footer inert while either tree can grow. An always-expanding
    // spacer competes for height with the tree sections inside QScrollArea.
    QSpacerItem *footer_spacer = new QSpacerItem(0, 0, QSizePolicy::Minimum,
                                                QSizePolicy::Fixed);
    left_layout->addItem(footer_spacer);

    // Give the trees any spare sidebar height without reducing their existing
    // minimum heights. When a section is collapsed, its header should remain
    // compact and the other expanded tree should use the freed space.
    const auto update_section_stretches = [left_layout, footer_spacer, servers_group,
                                           projects_group, revisions_group]()
    {
        const bool projects_expanded = !projects_group->isCollapsed();
        const bool revisions_expanded = !revisions_group->isCollapsed();
        // The server selector must occupy only its own content height. A
        // vertically Preferred QGroupBox may absorb the extra viewport height
        // without growing its capped tree, leaving blank space while the two
        // actual project/revision trees are starved.
        servers_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        projects_group->setSizePolicy(QSizePolicy::Preferred,
            projects_expanded ? QSizePolicy::Expanding : QSizePolicy::Maximum);
        revisions_group->setSizePolicy(QSizePolicy::Preferred,
            revisions_expanded ? QSizePolicy::Expanding : QSizePolicy::Maximum);
        left_layout->setStretch(left_layout->indexOf(servers_group), 0);
        left_layout->setStretch(left_layout->indexOf(projects_group), projects_expanded ? 1 : 0);
        left_layout->setStretch(left_layout->indexOf(revisions_group), revisions_expanded ? 1 : 0);
        // Only use the footer as a filler when neither tree can expand.
        const bool expand_footer = !projects_expanded && !revisions_expanded;
        footer_spacer->changeSize(0, 0, QSizePolicy::Minimum,
            expand_footer ? QSizePolicy::Expanding : QSizePolicy::Fixed);
        left_layout->setStretch(left_layout->count() - 1, expand_footer ? 1 : 0);
        left_layout->invalidate();
    };
    connect(servers_group, &GroupBoxCollapsible::signalExpanded, left,
            [update_section_stretches](GroupBoxCollapsible *) { update_section_stretches(); });
    connect(servers_group, &GroupBoxCollapsible::signalCollapsed, left,
            [update_section_stretches](GroupBoxCollapsible *) { update_section_stretches(); });
    connect(servers_group, &GroupBoxCollapsible::signalExpanded, this,
            [this](GroupBoxCollapsible *) { updateEmptyState(); });
    connect(servers_group, &GroupBoxCollapsible::signalCollapsed, this,
            [this](GroupBoxCollapsible *) { updateEmptyState(); });
    connect(projects_group, &GroupBoxCollapsible::signalExpanded, left,
            [update_section_stretches](GroupBoxCollapsible *) { update_section_stretches(); });
    connect(projects_group, &GroupBoxCollapsible::signalCollapsed, left,
            [update_section_stretches](GroupBoxCollapsible *) { update_section_stretches(); });
    connect(revisions_group, &GroupBoxCollapsible::signalExpanded, left,
            [update_section_stretches](GroupBoxCollapsible *) { update_section_stretches(); });
    connect(revisions_group, &GroupBoxCollapsible::signalCollapsed, left,
            [update_section_stretches](GroupBoxCollapsible *) { update_section_stretches(); });
    update_section_stretches();
    left_scroll->setWidget(left);
    splitter->addWidget(left_scroll);

    QWidget *center = new QWidget(splitter);
    QVBoxLayout *center_layout = new QVBoxLayout(center);
    center_layout->setContentsMargins(0, 8, 0, 8);
    center_layout->setSpacing(5);
    QWidget *graph_toolbar = new QWidget(center);
    graph_toolbar->setObjectName(QStringLiteral("projectsGraphToolbar"));
    QHBoxLayout *graph_header = new QHBoxLayout(graph_toolbar);
    graph_header->setContentsMargins(0, 0, 0, 0);
    QLabel *graph_title = new QLabel(tr("Branch & revision graph"), center);
    QFont graph_font = graph_title->font();
    graph_font.setBold(true);
    graph_title->setFont(graph_font);
    graph_header->addWidget(graph_title);
    graph_header->addStretch();
    // One visibly grouped, mutually exclusive control. The selected half
    // uses the Qt highlight color, so it is unmistakably a view-mode switch.
    // This remains independent of the left revision-tree filter.
    QFrame *graph_view_switch = new QFrame(center);
    graph_view_switch->setObjectName(QStringLiteral("projectsGraphViewSegmentedControl"));
    graph_view_switch->setStyleSheet(QStringLiteral(
        "QFrame#projectsGraphViewSegmentedControl { border: 1px solid palette(mid); "
        "border-radius: 5px; background: palette(button); } "
        "QFrame#projectsGraphViewSegmentedControl QPushButton { border: none; "
        "border-radius: 0px; padding: 3px 9px; background: transparent; } "
        "QFrame#projectsGraphViewSegmentedControl QPushButton:checked { "
        "background: palette(highlight); color: palette(highlighted-text); "
        "font-weight: bold; } "
        "QFrame#projectsGraphViewSegmentedControl QPushButton:hover:!checked { "
        "background: palette(midlight); }"));
    QHBoxLayout *graph_switch_layout = new QHBoxLayout(graph_view_switch);
    graph_switch_layout->setContentsMargins(2, 2, 2, 2);
    graph_switch_layout->setSpacing(0);
    QPushButton *graph_heads_button = compactButton(tr("Editable heads"),
        tr("Show branch heads, shared ancestors, and expandable hidden history"), graph_view_switch);
    graph_heads_button->setObjectName(QStringLiteral("projectsGraphHeadsOnlyButton"));
    QPushButton *graph_full_button = compactButton(tr("Full tree"),
        tr("Show every revision in the graph"), graph_view_switch);
    graph_full_button->setObjectName(QStringLiteral("projectsGraphFullTreeButton"));
    QButtonGroup *graph_view_group = new QButtonGroup(graph_view_switch);
    graph_view_group->setExclusive(true);
    for (QPushButton *button : {graph_heads_button, graph_full_button})
    {
        button->setCheckable(true);
        graph_view_group->addButton(button);
        graph_switch_layout->addWidget(button);
    }
    graph_full_button->setChecked(true);
    graph_header->addWidget(graph_view_switch);
    QPushButton *direction_button = compactButton(tr("Top down ↓"),
        tr("Switch the order of revisions between top-down and bottom-up"), center);
    direction_button->setObjectName(QStringLiteral("projectsGraphOrientationToggle"));
    direction_button->setCheckable(true);
    graph_header->addWidget(direction_button);
    QPushButton *fit_button = compactButton(tr("Fit graph"), tr("Fit all revisions to the canvas"), center);
    fit_button->setObjectName(QStringLiteral("projectsGraphFitButton"));
    graph_header->addWidget(fit_button);
    center_layout->addWidget(graph_toolbar);
    this->graph_stack = new QStackedWidget(center);
    this->graph_stack->setObjectName(QStringLiteral("projectsGraphStack"));
    this->graph = new ProjectRevisionGraph(this->graph_stack);
    this->graph_stack->addWidget(this->graph);
    QWidget *empty_page = new QWidget(this->graph_stack);
    empty_page->setObjectName(QStringLiteral("projectsNoSourcesPage"));
    QVBoxLayout *empty_layout = new QVBoxLayout(empty_page);
    empty_layout->setAlignment(Qt::AlignCenter);
    QLabel *empty_title = new QLabel(tr("No database configured"), empty_page);
    QFont empty_font = empty_title->font();
    empty_font.setPointSizeF(empty_font.pointSizeF() + 3);
    empty_font.setBold(true);
    empty_title->setFont(empty_font);
    empty_title->setAlignment(Qt::AlignCenter);
    QLabel *empty_description = new QLabel(tr("Add a database to explore its projects, branches and revisions."), empty_page);
    empty_description->setAlignment(Qt::AlignCenter);
    empty_description->setWordWrap(true);
    QPushButton *empty_add = compactButton(tr("Add database"), tr("Configure a local or remote database"), empty_page);
    empty_add->setObjectName(QStringLiteral("projectsEmptyAddDatabaseButton"));
    empty_layout->addWidget(empty_title, 0, Qt::AlignHCenter);
    empty_layout->addWidget(empty_description, 0, Qt::AlignHCenter);
    empty_layout->addWidget(empty_add, 0, Qt::AlignHCenter);
    this->graph_stack->addWidget(empty_page);
    QWidget *no_projects_page = new QWidget(this->graph_stack);
    no_projects_page->setObjectName(QStringLiteral("projectsNoProjectsPage"));
    QVBoxLayout *no_projects_layout = new QVBoxLayout(no_projects_page);
    no_projects_layout->setAlignment(Qt::AlignCenter);
    this->no_projects_title = new QLabel(no_projects_page);
    this->no_projects_title->setFont(empty_font);
    this->no_projects_title->setAlignment(Qt::AlignCenter);
    this->no_projects_description = new QLabel(no_projects_page);
    this->no_projects_description->setAlignment(Qt::AlignCenter);
    this->no_projects_description->setWordWrap(true);
    no_projects_layout->addWidget(this->no_projects_title, 0, Qt::AlignHCenter);
    no_projects_layout->addWidget(this->no_projects_description, 0, Qt::AlignHCenter);
    this->graph_stack->addWidget(no_projects_page);
    center_layout->addWidget(this->graph_stack, 1);
    splitter->addWidget(center);

    QScrollArea *right_scroll = new QScrollArea();
    right_scroll->setObjectName(QStringLiteral("projectsInspectorScrollArea"));
    right_scroll->setWidgetResizable(true);
    right_scroll->setFrameShape(QFrame::NoFrame);
    // The dock already has a standardized width; explanatory text must reflow
    // vertically rather than make its scroll area horizontally scrollable.
    right_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    right_scroll->setMinimumWidth(0);
    QWidget *right = new QWidget(right_scroll);
    right->setMinimumWidth(0);
    right->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QVBoxLayout *right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(12, 12, 12, 12);
    right_layout->setSpacing(12);
    this->inspector_heading = new QLabel(tr("Inspector"), right);
    QFont inspector_font = this->inspector_heading->font();
    inspector_font.setBold(true);
    inspector_font.setPointSizeF(inspector_font.pointSizeF() + 2);
    this->inspector_heading->setFont(inspector_font);
    wrapInspectorLabel(this->inspector_heading);
    right_layout->addWidget(this->inspector_heading);
    this->inspector_subtitle = new QLabel(right);
    wrapInspectorLabel(this->inspector_subtitle);
    right_layout->addWidget(this->inspector_subtitle);
    // One live database-status line for every Database Inspector tab.
    // It uses the same source sessions and health monitor as the sidebar.
    this->inspector_health_row = new QWidget(right);
    this->inspector_health_row->setObjectName(QStringLiteral("projectsDatabaseHealthRow"));
    QHBoxLayout *health_layout = new QHBoxLayout(this->inspector_health_row);
    health_layout->setContentsMargins(0, 0, 0, 0);
    health_layout->setSpacing(7);
    this->inspector_health_icon = new QLabel(this->inspector_health_row);
    this->inspector_health_icon->setObjectName(QStringLiteral("projectsDatabaseHealthIndicator"));
    this->inspector_health_icon->setFixedSize(16, 16);
    health_layout->addWidget(this->inspector_health_icon);
    this->inspector_health_text = new QLabel(this->inspector_health_row);
    this->inspector_health_text->setObjectName(QStringLiteral("projectsDatabaseHealthStatus"));
    wrapInspectorLabel(this->inspector_health_text);
    health_layout->addWidget(this->inspector_health_text, 1);
    right_layout->addWidget(this->inspector_health_row);
    this->inspector_health_row->hide();
    this->inspector_content_stack = new QStackedWidget(right);
    this->inspector_content_stack->setObjectName(QStringLiteral("projectsInspectorContentStack"));
    this->inspector_content_stack->setMinimumWidth(0);
    this->inspector_content_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    this->inspector_tabs = new QTabWidget(this->inspector_content_stack);
    this->inspector_tabs->setMinimumWidth(0);
    this->inspector_tabs->setObjectName(QStringLiteral("projectsInspectorTabs"));
    this->inspector_tabs->setDocumentMode(true);
    this->inspector_content_stack->addWidget(this->inspector_tabs);
    this->inspector_overview_page = new QWidget(this->inspector_tabs);
    this->inspector_overview_page->setObjectName(QStringLiteral("projectsInspectorOverview"));
    QVBoxLayout *overview_layout = new QVBoxLayout(this->inspector_overview_page);
    overview_layout->setContentsMargins(0, 12, 0, 4);
    this->inspector_overview_stack = new QStackedWidget(this->inspector_overview_page);
    this->inspector_overview_stack->setObjectName(QStringLiteral("projectsInspectorOverviewStack"));
    this->inspector_overview_stack->setMinimumWidth(0);
    this->inspector_overview_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    this->database_overview = new DatabaseInspectorOverview(this->inspector_overview_stack);
    this->project_overview = new ProjectInspectorOverview(this->inspector_overview_stack);
    this->branch_overview = new BranchInspectorOverview(this->inspector_overview_stack);
    this->revision_network_overview = new RevisionNetworkOverview(this->inspector_overview_stack);
    for (ProjectsInspectorOverview *overview : {
             static_cast<ProjectsInspectorOverview *>(this->database_overview),
             static_cast<ProjectsInspectorOverview *>(this->project_overview),
             static_cast<ProjectsInspectorOverview *>(this->branch_overview),
             static_cast<ProjectsInspectorOverview *>(this->revision_network_overview)})
        this->inspector_overview_stack->addWidget(overview);
    overview_layout->addWidget(this->inspector_overview_stack);
    this->revision_details_page = new QWidget(this->inspector_tabs);
    this->revision_details_page->setObjectName(QStringLiteral("projectsRevisionDetailsTab"));
    QVBoxLayout *revision_details_layout = new QVBoxLayout(this->revision_details_page);
    revision_details_layout->setContentsMargins(0, 12, 0, 4);
    this->revision_overview = new RevisionInspectorOverview(this->revision_details_page);
    revision_details_layout->addWidget(this->revision_overview);
    this->revision_details_page->hide();
    this->network_read_manager = new QNetworkAccessManager(this);
    this->revision_branch_page = new RevisionInspectorBranch(
        this->revision_overview->name, this->inspector_tabs);
    // A page parented to QTabWidget but not yet registered through addTab()
    // would otherwise paint as a free-floating child over unrelated inspectors.
    // Keep it hidden until the HEAD/Revision Branch tab owns its visibility.
    this->revision_branch_page->hide();
    this->database_connection = new DatabaseInspectorConnection(
        defaultLocalDvmdbPath(), this->inspector_tabs);
    this->database_connection->hide();
    this->database_relations = new DatabaseInspectorRelations(this->inspector_tabs);
    this->project_relations = new ProjectInspectorRelations(this->inspector_tabs);
    this->branch_relations = new BranchInspectorRelations(this->inspector_tabs);
    this->revision_relations = new RevisionInspectorRelations(this->inspector_tabs);
    for (ProjectsInspectorRelations *panel : {
             static_cast<ProjectsInspectorRelations *>(this->database_relations),
             static_cast<ProjectsInspectorRelations *>(this->project_relations),
             static_cast<ProjectsInspectorRelations *>(this->branch_relations),
             static_cast<ProjectsInspectorRelations *>(this->revision_relations)})
        panel->hide();
    QWidget *creation_page = new QWidget(this->inspector_content_stack);
    creation_page->setObjectName(QStringLiteral("projectsInspectorCreationPage"));
    QVBoxLayout *creation_layout = new QVBoxLayout(creation_page);
    creation_layout->setContentsMargins(0, 0, 0, 0);
    this->creation_forms = new QStackedWidget(creation_page);
    this->creation_forms->setObjectName(QStringLiteral("projectsInspectorCreationForms"));
    this->creation_forms->setMinimumWidth(0);
    this->creation_forms->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    this->project_creation = new ProjectInspectorCreationForm(this->creation_forms);
    this->branch_creation = new BranchInspectorCreationForm(this->creation_forms);
    this->creation_forms->addWidget(this->project_creation);
    this->creation_forms->addWidget(this->branch_creation);
    creation_layout->addWidget(this->creation_forms);
    this->inspector_content_stack->addWidget(creation_page);
    right_layout->addWidget(this->inspector_content_stack, 1);
    // Independent action panels belong to their respective domain inspectors.
    // No shared footer contains actions for unrelated selection kinds.
    this->inspector_actions_stack = new ProjectsInspectorActionStack(right);
    this->inspector_actions_stack->setObjectName(QStringLiteral("projectsInspectorActionsStack"));
    this->database_inspector = new DatabaseInspector(this->inspector_actions_stack);
    this->project_inspector = new ProjectInspector(this->inspector_actions_stack);
    this->branch_inspector = new BranchInspector(this->inspector_actions_stack);
    this->revision_inspector = new RevisionInspector(this->inspector_actions_stack);
    this->revision_inspector->placePrimaryActions(this->revision_overview);
    this->inspector_actions_stack->addWidget(this->database_inspector);
    this->inspector_actions_stack->addWidget(this->project_inspector);
    this->inspector_actions_stack->addWidget(this->branch_inspector);
    this->inspector_actions_stack->addWidget(this->revision_inspector);
    this->inspector_actions_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
    right_layout->addWidget(this->inspector_actions_stack);
    right_scroll->setWidget(right);
    this->inspector_dock = new QDockWidget(tr("Project Inspector"), dock_host);
    this->inspector_dock->setObjectName(QStringLiteral("projectsInspectorDock"));
    // Use the same fixed right-sidebar width as the map's EntityInspectorDock.
    // Without an explicit dock width QMainWindow allocates roughly half of the
    // entire Projects workspace to this inspector.
    this->inspector_dock->setMinimumWidth(Sizes::SidebarRightWidth);
    this->inspector_dock->setMaximumWidth(Sizes::SidebarRightWidth);
    this->inspector_dock->resize(Sizes::SidebarRightWidth, this->inspector_dock->height());
    this->inspector_dock->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    this->inspector_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    this->inspector_dock->setFeatures(QDockWidget::DockWidgetClosable |
                                      QDockWidget::DockWidgetMovable |
                                      QDockWidget::DockWidgetFloatable);
    this->inspector_dock->setWidget(right_scroll);
    dock_host->addDockWidget(Qt::RightDockWidgetArea, this->inspector_dock);
    this->inspector_dock->installEventFilter(this);
    this->inspector_dock->hide();

    splitter->setSizes(QList<int>{295, 720});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    connect(add_server, &QToolButton::clicked, this, &ProjectsWidget::createServer);
    connect(this->database_connection->source_refresh_button, &QPushButton::clicked, this, &ProjectsWidget::connectSource);
    connect(empty_add, &QPushButton::clicked, this, &ProjectsWidget::createServer);
    connect(this->database_inspector->save_source, &QPushButton::clicked, this, &ProjectsWidget::saveNewSource);
    connect(this->database_connection->source_name_edit, &QLineEdit::textChanged, this,
            [this](const QString &name)
    {
        if (this->selection_kind == SelectionKind::NewSource)
            this->database_overview->name->setText(name);
    });
    connect(this->database_overview->name, &QLineEdit::textEdited, this,
            [this](const QString &name)
    {
        if (this->selection_kind == SelectionKind::NewSource)
            this->database_connection->source_name_edit->setText(name);
    });
    connect(this->database_inspector->cancel_source, &QPushButton::clicked, this, &ProjectsWidget::cancelNewSource);
    connect(this->database_connection->source_kind_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateSourceFields(); });
    connect(this->database_connection->browse_path, &QPushButton::clicked, this, [this]
    {
        const QString path = QFileDialog::getExistingDirectory(this, tr("Select DVMDB data directory"),
            this->database_connection->local_path_edit->text());
        if (!path.isEmpty())
        {
            this->database_connection->local_path_edit->setText(path);
            applyInspectorEdits();
        }
    });
    connect(this->server_tree, &QTreeWidget::itemSelectionChanged, this, [this]
    {
        const QList<QTreeWidgetItem *> items = this->server_tree->selectedItems();
        if (items.isEmpty() || this->updating_ui) return;
        const QString key = items.first()->data(0, role_key).toString();
        if (key.startsWith(QStringLiteral("s:"))) select(SelectionKind::Server, key.mid(2));
    });
    connect(this->server_tree, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *item, int column)
    {
        if (this->updating_ui || column != 0 || item == nullptr) return;
        const QString key = item->data(0, role_key).toString();
        if (!key.startsWith(QStringLiteral("s:"))) return;
        const QString id = key.mid(2);
        for (ProjectServerProfile &profile : this->servers)
        {
            if (profile.id != id) continue;
            const bool old = profile.visible;
            profile.visible = item->checkState(0) == Qt::Checked;
            if (!saveProjectServerProfiles(this->servers))
            {
                profile.visible = old;
                const QSignalBlocker blocked(this->server_tree);
                item->setCheckState(0, old ? Qt::Checked : Qt::Unchecked);
                reportServerSaveFailure();
                return;
            }
            if (!profile.visible) restoreVisibleSelection();
            populateProjectTree();
            populateRevisionTree();
            rebuildGraph();
            updateEmptyState();
            updateButtons();
            broadcastContext();
            return;
        }
    });
    for (QPushButton *button : {this->database_overview->create_project,
                                this->database_relations->create_project})
        connect(button, &QPushButton::clicked, this, [this] { beginCreation(false); });
    for (QPushButton *button : {this->project_overview->create_branch,
                                this->project_relations->create_branch,
                                this->branch_inspector->create_branch,
                                this->revision_inspector->create_branch})
        connect(button, &QPushButton::clicked, this, [this] { beginCreation(true); });
    connect(this->branch_inspector->working_copies, &QPushButton::clicked,
            this, &ProjectsWidget::showWorkingCopies);
    connect(this->revision_inspector->head_working_copies, &QPushButton::clicked,
            this, &ProjectsWidget::showWorkingCopies);
    connect(this->revision_network_overview->open_network, &QPushButton::clicked,
            this, &ProjectsWidget::openInspectedNetwork);
    connect(this->revision_network_overview->export_network, &QPushButton::clicked,
            this, &ProjectsWidget::exportInspectedNetwork);
    connect(this->revision_network_overview->import_network, &QPushButton::clicked,
            this, &ProjectsWidget::beginInspectedNetworkImport);
    connect(this->project_overview->import_network, &QPushButton::clicked,
            this, &ProjectsWidget::importProjectRequested);
    for (ProjectsInspectorPanel *panel : {static_cast<ProjectsInspectorPanel *>(this->database_inspector),
                                          static_cast<ProjectsInspectorPanel *>(this->project_inspector),
                                          static_cast<ProjectsInspectorPanel *>(this->branch_inspector),
                                          static_cast<ProjectsInspectorPanel *>(this->revision_inspector)})
    {
        connect(panel->creation_cancel, &QPushButton::clicked, this, &ProjectsWidget::cancelCreation);
        connect(panel->creation_confirm, &QPushButton::clicked, this, &ProjectsWidget::submitCreation);
    }
    for (ProjectsInspectorCreationForm *form : {
             static_cast<ProjectsInspectorCreationForm *>(this->project_creation),
             static_cast<ProjectsInspectorCreationForm *>(this->branch_creation)})
        connect(form->name, &QLineEdit::textChanged,
                this, [this](const QString &) { validateCreation(); });
    connect(this->branch_creation->tags, &QLineEdit::textChanged,
            this, [this](const QString &) { validateCreation(); });
    connect(this->branch_creation->description, &QTextEdit::textChanged,
            this, [this] { validateCreation(); });
    connect(this->branch_creation->source, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { validateCreation(); });
    for (ProjectsInspectorRelations *panel : {
             static_cast<ProjectsInspectorRelations *>(this->database_relations),
             static_cast<ProjectsInspectorRelations *>(this->project_relations),
             static_cast<ProjectsInspectorRelations *>(this->branch_relations),
             static_cast<ProjectsInspectorRelations *>(this->revision_relations)})
    {
        connect(panel->list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item)
        {
            if (item != nullptr) activateInspectorRelation(item->data(Qt::UserRole).toString());
        });
        connect(panel->list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item)
        {
            if (item != nullptr) activateInspectorRelation(item->data(Qt::UserRole).toString());
        });
    }
    if (browse_examples_button != nullptr)
        connect(browse_examples_button, &QPushButton::clicked,
                this, &ProjectsWidget::browseExamples);
    connect(graph_heads_button, &QPushButton::clicked, this, [this]
    {
        // A historical selection must not silently remain offscreen when the
        // graph displays only branch heads.
        if (this->selection_kind == SelectionKind::Revision && !isBranchHead(this->selected_id))
        {
            if (ProjectPreviewBranch *branch = currentBranch())
                if (isBranchHead(branch->head))
                    select(SelectionKind::Revision, branch->head);
        }
        this->graph->setShowFullTree(false);
    });
    connect(graph_full_button, &QPushButton::clicked, this->graph,
            [this]() { this->graph->setShowFullTree(true); });
    connect(fit_button, &QPushButton::clicked, this->graph, &ProjectRevisionGraph::fitGraph);
    connect(direction_button, &QPushButton::toggled, this,
            [this, direction_button](bool bottom_up)
    {
        direction_button->setText(bottom_up ? tr("Bottom up ↑") : tr("Top down ↓"));
        this->graph->setTopDown(!bottom_up);
    });
    connect(this->project_tree, &QTreeWidget::itemSelectionChanged, this, [this]
    {
        const QList<QTreeWidgetItem *> items = this->project_tree->selectedItems();
        if (items.isEmpty() || this->updating_ui) return;
        const QString key = items.first()->data(0, role_key).toString();
        if (key.startsWith(QStringLiteral("s:"))) select(SelectionKind::Server, key.mid(2));
        else if (key.startsWith(QStringLiteral("p:"))) select(SelectionKind::Project, key.mid(2));
        else if (key.startsWith(QStringLiteral("b:"))) select(SelectionKind::Branch, key.mid(2));
    });
    connect(this->project_tree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int)
    {
        // A HEAD card also highlights its owning branch in the sidebar.
        // Clicking that already-selected branch must still switch to the
        // branch-management Inspector (selectionChanged would not fire).
        if (this->updating_ui || this->selection_kind != SelectionKind::Head
            || item == nullptr) return;
        const QString key = item->data(0, role_key).toString();
        if (key.startsWith(QStringLiteral("b:")))
            select(SelectionKind::Branch, key.mid(2));
    });
    connect(heads_button, &QPushButton::clicked, this, [this]
    {
        this->show_full_revision_tree = false;
        populateRevisionTree();
        // A historical selection cannot remain highlighted in Heads-only mode.
        // Follow its branch to the editable head and synchronize all panes.
        if (this->selection_kind == SelectionKind::Revision && !isBranchHead(this->selected_id))
        {
            if (ProjectPreviewBranch *branch = currentBranch())
                if (isBranchHead(branch->head))
                {
                    select(SelectionKind::Revision, branch->head);
                    return;
                }
        }
        selectTreeItem(this->revision_tree,
            this->selection_kind == SelectionKind::Revision
                ? keyFor(QStringLiteral("r"), this->selected_id) : QString());
    });
    connect(this->full_tree_button, &QPushButton::clicked, this, [this]
    {
        this->show_full_revision_tree = true;
        populateRevisionTree();
        selectTreeItem(this->revision_tree,
            this->selection_kind == SelectionKind::Revision
                ? keyFor(QStringLiteral("r"), this->selected_id) : QString());
    });
    connect(this->revision_tree, &QTreeWidget::itemSelectionChanged, this, [this]
    {
        const QList<QTreeWidgetItem *> items = this->revision_tree->selectedItems();
        if (items.isEmpty() || this->updating_ui) return;
        const QString key = items.first()->data(0, role_key).toString();
        if (key.startsWith(QStringLiteral("r:"))) select(SelectionKind::Revision, key.mid(2));
    });
    connect(this->graph, &ProjectRevisionGraph::revisionActivated, this, [this](const QString &id)
    {
        const QPair<QString, QString> target = this->graph_revision_lookup.value(id);
        if (target.first.isEmpty()) return;
        select(SelectionKind::Project, target.first);
        select(SelectionKind::Revision, target.second);
    });
    connect(this->graph, &ProjectRevisionGraph::headActivated, this, [this](const QString &id)
    {
        // A graph HEAD is a writable state: show its HEAD/Revision Inspector,
        // not the branch-management Inspector used by the sidebar tree.
        select(SelectionKind::Head, id);
    });
    connect(this->graph, &ProjectRevisionGraph::serverActivated, this, [this](const QString &id)
    {
        select(SelectionKind::Server, id);
    });
    connect(this->graph, &ProjectRevisionGraph::projectActivated, this, [this](const QString &id)
    {
        select(SelectionKind::Project, id);
    });
    connect(this->database_connection->personal_server_checkbox, &QCheckBox::toggled,
            this, &ProjectsWidget::setPersonalServer);
    connect(this->database_connection->server_endpoint_edit, &QLineEdit::editingFinished, this, &ProjectsWidget::applyInspectorEdits);
    connect(this->database_connection->local_path_edit, &QLineEdit::editingFinished, this, &ProjectsWidget::applyInspectorEdits);
    connect(this->database_connection->server_auth_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { applyInspectorEdits(); });
    connect(this->database_connection->delete_connection, &QPushButton::clicked,
            this, &ProjectsWidget::deleteConnection);
    connect(this->project_inspector->delete_project, &QPushButton::clicked,
            this, &ProjectsWidget::deleteSelection);
    connect(this->branch_inspector->delete_branch, &QPushButton::clicked,
            this, &ProjectsWidget::deleteInspectedBranch);
    connect(this->revision_branch_page->delete_branch, &QPushButton::clicked,
            this, &ProjectsWidget::deleteInspectedBranch);
    connect(this->revision_network_overview->delete_revision, &QPushButton::clicked,
            this, &ProjectsWidget::deleteInspectedRevision);
    for (ProjectsInspectorOverview *overview : {
             static_cast<ProjectsInspectorOverview *>(this->database_overview),
             static_cast<ProjectsInspectorOverview *>(this->project_overview),
             static_cast<ProjectsInspectorOverview *>(this->branch_overview)})
    {
        connect(overview->name, &QLineEdit::editingFinished,
                this, &ProjectsWidget::applyInspectorEdits);
        connect(overview->tags, &QLineEdit::editingFinished,
                this, &ProjectsWidget::applyInspectorEdits);
        connect(overview->description, &QTextEdit::textChanged,
                this, &ProjectsWidget::applyInspectorEdits);
    }

    // AOWIS persists edits automatically; no explicit Save changes button.
    // Branch renames are committed on editingFinished so confirmation never
    // interrupts typing; multiline metadata is saved after a quiet interval.
    this->revision_autosave_timer = new QTimer(this);
    this->revision_autosave_timer->setSingleShot(true);
    this->revision_autosave_timer->setInterval(800);
    connect(this->revision_autosave_timer, &QTimer::timeout, this, [this]() {
        if (this->selection_kind != SelectionKind::Head
            && this->selection_kind != SelectionKind::Revision) return;
        if (this->revision_rename_confirmation_pending) return;
        if (this->updating_ui || this->source_writer->isBusy()
            || this->revision_overview->name->hasFocus()) {
            this->revision_autosave_timer->start();
            return;
        }
        saveRevisionInspectorChanges();
    });
    // A pending rename is reflected in Overview while typing, but is not
    // persisted until the asynchronous confirmation is accepted.
    connect(this->revision_overview->name, &QLineEdit::textChanged,
            this, [this](const QString &name) {
        this->revision_overview->branch_label->setText(name);
        this->revision_network_overview->branch->setText(tr("Branch: %1").arg(name));
    });
    connect(this->revision_overview->name, &QLineEdit::editingFinished,
            this, [this]() {
        if (this->updating_ui) return;
        this->revision_autosave_timer->stop();
        if (this->source_writer->isBusy()) this->revision_autosave_timer->start();
        else saveRevisionInspectorChanges();
    });
    // Overview and Branch edit the same branch metadata. Keep both editors in
    // sync, while the Overview remains the single source read by autosave and
    // HEAD commit validation. Block reflected signals to avoid feedback loops.
    connect(this->revision_overview->tags, &QLineEdit::textChanged,
            this, [this](const QString &text) {
        const QSignalBlocker blocked(this->revision_branch_page->tags);
        this->revision_branch_page->tags->setText(text);
    });
    connect(this->revision_branch_page->tags, &QLineEdit::textChanged,
            this, [this](const QString &text) {
        const QSignalBlocker blocked(this->revision_overview->tags);
        this->revision_overview->tags->setText(text);
    });
    connect(this->revision_overview->tags, &QLineEdit::editingFinished,
            this, [this]() {
        if (!this->updating_ui) this->revision_autosave_timer->start();
    });
    connect(this->revision_branch_page->tags, &QLineEdit::editingFinished,
            this, [this]() {
        if (!this->updating_ui) this->revision_autosave_timer->start();
    });
    connect(this->revision_overview->description, &QTextEdit::textChanged,
            this, [this]() {
        const QString text = this->revision_overview->description->toPlainText();
        if (this->revision_branch_page->description->toPlainText() != text) {
            const QSignalBlocker blocked(this->revision_branch_page->description);
            this->revision_branch_page->description->setPlainText(text);
        }
        if (!this->updating_ui) this->revision_autosave_timer->start();
    });
    connect(this->revision_branch_page->description, &QTextEdit::textChanged,
            this, [this]() {
        const QString text = this->revision_branch_page->description->toPlainText();
        if (this->revision_overview->description->toPlainText() != text) {
            const QSignalBlocker blocked(this->revision_overview->description);
            this->revision_overview->description->setPlainText(text);
        }
        if (!this->updating_ui) this->revision_autosave_timer->start();
    });
    connect(this->revision_inspector->commit_head, &QPushButton::clicked,
            this, &ProjectsWidget::commitSelectedHead);
    connect(this->revision_overview->commit_message, &QPlainTextEdit::textChanged,
            this, [this] {
        if (this->updating_ui || (this->selection_kind != SelectionKind::Head
                                  && this->selection_kind != SelectionKind::Revision)) return;
        if (this->selection_kind == SelectionKind::Head) {
            if (const ProjectPreviewBranch *branch = currentBranch())
                this->head_commit_drafts.insert(branch->id,
                    this->revision_overview->commit_message->toPlainText());
        } else {
            this->revision_autosave_timer->start();
        }
        updateButtons();
    });
    populateServerTree();
    updateEmptyState();
    populateProjectTree();
    populateRevisionTree();
    rebuildGraph();
    bool personal_selected = false;
    for (const ProjectServerProfile &profile : this->servers)
        if (profile.id == this->server_id && profile.personal)
        {
            personal_selected = true;
            break;
        }
    // A personal working database takes precedence at workspace startup.
    // Subsequent user selection is never redirected to the personal database.
    if (personal_selected || this->branch_id.isEmpty())
    {
        if (!this->server_id.isEmpty()) select(SelectionKind::Server, this->server_id);
        else
        {
            updateInspector();
            updateButtons();
            broadcastContext();
        }
    }
    else
        select(SelectionKind::Branch, this->branch_id);
    // Sources are always active. The checkboxes affect visibility only.
    QTimer::singleShot(0, this, [this]
    {
        for (const ProjectServerProfile &profile : this->servers) startSource(profile);
    });
}

bool ProjectsWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->inspector_dock && event->type() == QEvent::Close)
    {
        if (this->creation_kind != CreationKind::None)
        {
            if (this->source_writer->isBusy()) { event->ignore(); return true; }
            if (currentCreationForm() != nullptr && currentCreationForm()->isDirty()
                && QMessageBox::question(this, tr("Discard creation draft?"),
                    tr("Closing the inspector will discard the unfinished creation form."),
                    QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel)
                    != QMessageBox::Discard)
            { event->ignore(); return true; }
            cancelCreation();
        }
        if (this->selection_kind == SelectionKind::NewSource) cancelNewSource();
    }
    return QWidget::eventFilter(watched, event);
}

QString ProjectsWidget::makeId(const QString &prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
}

void ProjectsWidget::seedPreview()
{
    // Deliberately fictional examples, prominently marked as NOT DB content.
    ProjectPreviewData north;
    north.id = QStringLiteral("preview-north");
    north.name = tr("Sample North Network");
    north.description = tr("Illustrative project for the version explorer layout.");
    north.tags = {QStringLiteral("preview"), QStringLiteral("water")};
    north.branches = {
        {QStringLiteral("north-main"), QStringLiteral("main"), tr("Stable hydraulic model"), {QStringLiteral("stable")}, QStringLiteral("c8d102ab")},
        {QStringLiteral("north-study"), QStringLiteral("pressure-study"), tr("Pressure optimization experiment"), {QStringLiteral("scenario")}, QStringLiteral("ec9045df")}
    };
    // Preview revisions have commit messages, not separate names. Initialize
    // fields explicitly: positional aggregate initialization previously put
    // the intended description into the wrong field and left message empty.
    north.revisions = {
        {.id = QStringLiteral("c8d102ab"),
         .description = tr("Update base network parameters."),
         .branch = QStringLiteral("main"),
         .parents = {QStringLiteral("93f0de41")},
         .tags = {QStringLiteral("validated")},
         .author = tr("Preview author"),
         .created_at = QStringLiteral("2026-10-25"),
         .message = tr("Validated network")},
        {.id = QStringLiteral("ec9045df"),
         .description = tr("Adjusted pressure scenario inputs."),
         .branch = QStringLiteral("pressure-study"),
         .parents = {QStringLiteral("a24f918d")},
         .tags = {QStringLiteral("experiment")},
         .author = tr("Preview author"),
         .created_at = QStringLiteral("2026-10-24"),
         .message = tr("Pressure tuning")},
        {.id = QStringLiteral("a24f918d"),
         .description = tr("Branch off the network baseline."),
         .branch = QStringLiteral("pressure-study"),
         .parents = {QStringLiteral("93f0de41")},
         .author = tr("Preview author"),
         .created_at = QStringLiteral("2026-10-23"),
         .message = tr("Scenario start")},
        {.id = QStringLiteral("93f0de41"),
         .description = tr("Initial imported hydraulic model."),
         .branch = QStringLiteral("main"),
         .tags = {QStringLiteral("baseline")},
         .author = tr("Preview author"),
         .created_at = QStringLiteral("2026-10-22"),
         .message = tr("Network baseline")}
    };
    // Historical example data is retained for the version-explorer prototype,
    // but is only shown by explicitly configured profiles with this demo ID.
    // Real newly added servers never acquire fictional projects automatically.
    north.server_id = QStringLiteral("preview-local");
    this->projects.append(north);
    ProjectPreviewData south;
    south.id = QStringLiteral("preview-south");
    south.name = tr("Sample Pump Station");
    south.description = tr("Another fictional project in this GUI preview.");
    south.tags = {QStringLiteral("preview")};
    south.branches = {{QStringLiteral("south-main"), QStringLiteral("main"), QString(), {}, QStringLiteral("742b0a90")}};
    south.revisions = {{.id = QStringLiteral("742b0a90"),
                        .description = tr("Demo revision."),
                        .branch = QStringLiteral("main"),
                        .author = tr("Preview author"),
                        .created_at = QStringLiteral("2026-10-23"),
                        .message = tr("Initial layout")}};
    south.server_id = QStringLiteral("preview-local");
    this->projects.append(south);
}

ProjectServerProfile *ProjectsWidget::currentServer()
{
    for (ProjectServerProfile &server : this->servers)
        if (server.id == this->server_id) return &server;
    return nullptr;
}

void ProjectsWidget::databaseAvailability(const ProjectServerProfile &profile,
                                          QColor &color, QString &status,
                                          QString &details) const
{
    const SourceSession *session = this->source_sessions.value(profile.id, nullptr);
    color = QColor(QStringLiteral("#c9a227"));
    status = tr("Checking");
    details = tr("Waiting for a database response");

    if (profile.pending)
    {
        color = QColor(QStringLiteral("#777777"));
        status = tr("Unverified");
        details = session != nullptr && !session->error.isEmpty()
            ? tr("Repository identity not yet verified: %1").arg(session->error)
            : tr("Repository UUIDv7 not yet verified");
        return;
    }
    if (profile.id == QStringLiteral("preview-local")
        || profile.id == QStringLiteral("preview-remote"))
    {
        color = QColor(QStringLiteral("#777777"));
        status = tr("Preview");
        details = tr("In-memory demonstration; no database connection");
        return;
    }
    if (session == nullptr) return;

    if (session->health != nullptr
        && session->health->currentState() == HttpHealthMonitor::State::Degraded)
    {
        color = QColor(QStringLiteral("#dd9537"));
        status = tr("Unstable");
        details = session->error.isEmpty()
            ? tr("A recent health check failed; retrying") : session->error;
        return;
    }
    // The health probe establishes service reachability independently of
    // parsing the project catalogue. A malformed or temporarily unreadable
    // revision graph must not make a responding DVMDB appear offline.
    if (session->health != nullptr
        && session->health->currentState() == HttpHealthMonitor::State::Healthy
        && session->state == SourceState::Error)
    {
        color = QColor(QStringLiteral("#dd9537"));
        status = tr("Available · catalogue error");
        details = session->error.isEmpty()
            ? tr("DVMDB is responding, but its project catalogue could not be loaded")
            : tr("DVMDB is responding, but its project catalogue could not be loaded: %1")
                .arg(session->error);
        return;
    }
    if (session->health != nullptr
        && session->health->currentState() == HttpHealthMonitor::State::Checking)
    {
        details = session->error.isEmpty()
            ? tr("Checking DVMDB server identity")
            : tr("Checking DVMDB server identity; catalogue error: %1").arg(session->error);
        return;
    }
    if (session->state == SourceState::Error
        || (session->health != nullptr
            && session->health->currentState() == HttpHealthMonitor::State::Offline))
    {
        color = QColor(QStringLiteral("#c24848"));
        status = session->codec_error.isEmpty()
            ? tr("Unavailable") : tr("Incompatible MODEL");
        details = !session->codec_error.isEmpty() ? session->codec_error
            : session->error.isEmpty() ? tr("Database connection unavailable") : session->error;
        return;
    }
    if (session->state == SourceState::Connected
        && (session->health == nullptr
            || session->health->currentState() == HttpHealthMonitor::State::Healthy))
    {
        color = QColor(QStringLiteral("#339950"));
        status = tr("Available");
        details = profile.connection_type == QStringLiteral("local")
            ? tr("Embedded database is open and its catalogue is loaded")
            : tr("Database is responding and its catalogue is loaded");
        return;
    }
    if (session->state == SourceState::Loading)
    {
        status = tr("Loading");
        details = tr("Loading the database catalogue");
    }
    else if (!session->error.isEmpty())
        details = session->error;
}

void ProjectsWidget::populateServerTree()
{
    const QSignalBlocker blocked(this->server_tree);
    this->server_tree->clear();
    for (const ProjectServerProfile &server : this->servers)
    {
        const bool local = server.connection_type == QStringLiteral("local");
        QTreeWidgetItem *item = newTreeItem(this->server_tree,
            server.personal ? tr("♛ %1").arg(server.name)
                : server.pending ? tr("◷ %1").arg(server.name) : server.name,
            keyFor(QStringLiteral("s"), server.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, server.visible ? Qt::Checked : Qt::Unchecked);
        const SourceSession *session = this->source_sessions.value(server.id, nullptr);
        QColor color;
        QString status;
        QString status_details;
        databaseAvailability(server, color, status, status_details);
        if (local)
            item->setIcon(0, QIcon::fromTheme(QStringLiteral("drive-harddisk"),
                style()->standardIcon(QStyle::SP_DriveHDIcon)));
        else
        {
            QPixmap indicator(16, 16);
            indicator.fill(Qt::transparent);
            QPainter painter(&indicator);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(QRectF(3, 3, 10, 10));
            item->setIcon(0, QIcon(indicator));
        }
        item->setToolTip(0, tr("%1%2\n%3\n%4: %5\nCheckbox: show in explorer and graph")
            .arg(server.name, server.personal ? tr(" (personal working database)") : QString(),
                 local ? server.local_path : server.endpoint, status, status_details));
        if (session != nullptr && session->state == SourceState::Error)
            item->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
    }
    selectTreeItem(this->server_tree, keyFor(QStringLiteral("s"), this->server_id));
}

void ProjectsWidget::updateEmptyState()
{
    const bool empty = this->servers.isEmpty();
    bool has_visible_database = false;
    for (const ProjectServerProfile &profile : this->servers)
        if (profile.visible) { has_visible_database = true; break; }
    // The graph contains database nodes even when their catalogues are empty.
    // Only the *absence of checked databases* warrants a placeholder page.
    if (this->graph_stack != nullptr)
        this->graph_stack->setCurrentIndex(empty ? 1 : has_visible_database ? 0 : 2);
    if (QWidget *toolbar = findChild<QWidget *>(QStringLiteral("projectsGraphToolbar")))
        toolbar->setVisible(has_visible_database);
    bool expanded = true;
    if (GroupBoxCollapsible *section = findChild<GroupBoxCollapsible *>(QStringLiteral("serversCollapsibleGroup")))
        expanded = !section->isCollapsed();
    if (QLabel *notice = findChild<QLabel *>(QStringLiteral("projectsNoSourcesSidebar")))
        notice->setVisible(empty && expanded);
    if (this->server_tree != nullptr)
        this->server_tree->setVisible(!empty && expanded);
    updateConnectionStatus();
}

bool ProjectsWidget::isPreviewSource() const
{
    return this->server_id == QStringLiteral("preview-local")
        || this->server_id == QStringLiteral("preview-remote");
}

bool ProjectsWidget::isSourceReady(const QString &id) const
{
    const SourceSession *session = this->source_sessions.value(id, nullptr);
    return session != nullptr && session->state == SourceState::Connected
        && !session->loading;
}

void ProjectsWidget::updateConnectionStatus()
{
    if (this->database_connection->source_status == nullptr) return;
    const ProjectServerProfile *server = nullptr;
    for (const ProjectServerProfile &candidate : this->servers)
        if (candidate.id == this->server_id) { server = &candidate; break; }
    const bool has_server = server != nullptr && this->selection_kind == SelectionKind::Server;
    // This row lives above the tabs, so it follows the selected database
    // across Overview, Connection and Projects without a second health poll.
    this->inspector_health_row->setVisible(has_server);
    if (has_server)
    {
        QColor color;
        QString status;
        QString details;
        databaseAvailability(*server, color, status, details);
        QPixmap indicator(16, 16);
        indicator.fill(Qt::transparent);
        QPainter painter(&indicator);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(3, 3, 10, 10));
        painter.end();
        this->inspector_health_icon->setPixmap(indicator);
        this->inspector_health_text->setText(status);
        this->inspector_health_row->setToolTip(details);
        this->inspector_health_icon->setToolTip(details);
        this->inspector_health_text->setToolTip(details);
    }
    const SourceSession *session = has_server ? this->source_sessions.value(server->id, nullptr) : nullptr;
    const QString status = isPreviewSource() ? tr("In-memory demonstration")
        : session == nullptr ? tr("Starting automatically…")
        : session->health != nullptr
          && session->health->currentState() == HttpHealthMonitor::State::Degraded
            ? tr("Connection unstable · checking again…")
        : session->health != nullptr
          && session->health->currentState() == HttpHealthMonitor::State::Offline
            ? tr("Unavailable · server identity probe failed: %1").arg(
                  session->error.isEmpty() ? tr("No valid DVMDB response") : session->error)
        : this->source_writer->isBusy() ? tr("Saving to DVMDB…")
        : session->health != nullptr
          && session->health->currentState() == HttpHealthMonitor::State::Healthy
          && session->state == SourceState::Error
            ? tr("Available · catalogue error: %1").arg(session->error)
        : session->health != nullptr
          && session->health->currentState() == HttpHealthMonitor::State::Checking
            ? tr("Checking server identity…")
        : session->state == SourceState::Connected ? tr("Available · catalogue loaded")
        : session->state == SourceState::Loading ? tr("Loading catalogue…")
        : session->state == SourceState::Error ? tr("Unavailable: %1").arg(session->error)
        : tr("Checking connection…");
    this->database_connection->source_status->setText(has_server && server->pending
        ? tr("Unverified repository · %1").arg(status) : status);
    if (has_server && server->pending)
        this->database_connection->personal_server_checkbox->setEnabled(false);
    this->database_connection->source_status->setVisible(has_server);
    this->database_connection->source_refresh_button->setVisible(has_server && !isPreviewSource());
    this->database_connection->source_refresh_button->setEnabled(has_server && !this->source_writer->isBusy()
        && session != nullptr && !session->loading);
    if (this->database_connection->server_options != nullptr)
        this->database_connection->server_options->setEnabled(!this->source_writer->isBusy());
    if (this->no_projects_title != nullptr)
        this->no_projects_title->setText(tr("No databases selected"));
    if (this->no_projects_description != nullptr)
        this->no_projects_description->setText(tr("Check a database in the sidebar to show it on the canvas."));
    activeOverview()->setPreviewExplanation(isPreviewSource()
        ? tr("Only this demonstration uses in-memory project and revision edits.")
        : QString());
}

void ProjectsWidget::restoreVisibleSelection()
{
    const ProjectPreviewData *current = currentProject();
    if (current != nullptr)
        for (const ProjectServerProfile &profile : this->servers)
            if (profile.id == current->server_id && profile.visible) return;
    this->project_id.clear();
    this->branch_id.clear();
    if (this->selection_kind == SelectionKind::Project
        || this->selection_kind == SelectionKind::Branch
        || this->selection_kind == SelectionKind::Head
        || this->selection_kind == SelectionKind::Revision)
    {
        this->selection_kind = SelectionKind::None;
        this->selected_id.clear();
        if (this->inspector_dock != nullptr) this->inspector_dock->hide();
    }
}

void ProjectsWidget::resetSource(const QString &id)
{
    SourceSession *session = this->source_sessions.take(id);
    if (session != nullptr)
    {
        if (session->health != nullptr) session->health->stop();
        session->reader->cancel();
        delete session->refresh;
        delete session->health;
        delete session->reader;
        delete session;
    }
    for (int i = this->projects.size() - 1; i >= 0; --i)
        if (this->projects.at(i).database_backed && this->projects.at(i).server_id == id)
            this->projects.removeAt(i);
    restoreVisibleSelection();
    populateProjectTree();
    populateRevisionTree();
    rebuildGraph();
    populateServerTree();
    updateEmptyState();
    updateButtons();
    broadcastContext();
}

void ProjectsWidget::startSource(const ProjectServerProfile &profile)
{
    if (this->source_sessions.contains(profile.id) || this->source_writer->isBusy()) return;
    // Legacy in-memory example sources deliberately do not initiate network I/O.
    if (profile.id == QStringLiteral("preview-local") || profile.id == QStringLiteral("preview-remote")) return;
    SourceSession *session = new SourceSession;
    session->reader = new ProjectSourceReader(this);
    session->refresh = new QTimer(this);
    session->refresh->setInterval(60000);
    this->source_sessions.insert(profile.id, session);
    const QString id = profile.id;
    connect(session->refresh, &QTimer::timeout, this, [this, id]
    {
        SourceSession *current = this->source_sessions.value(id, nullptr);
        if (current == nullptr || current->loading || this->source_writer->isBusy()) return;
        if (current->health == nullptr
            || current->health->currentState() == HttpHealthMonitor::State::Healthy)
            loadSource(id);
    });
    session->refresh->start();
    if (profile.connection_type == QStringLiteral("local"))
    {
        loadSource(id);
    }
    else if (profile.auth_mode != QStringLiteral("None") && !profile.auth_mode.isEmpty())
    {
        session->state = SourceState::Error;
        session->error = tr("REST authentication is not implemented");
    }
    else
    {
        QString endpoint = profile.endpoint.trimmed();
        while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
        if (!endpoint.endsWith(QStringLiteral("/dvmdb/v1"))) endpoint += QStringLiteral("/dvmdb/v1");
        session->health = new HttpHealthMonitor(this);
        // Liveness is not catalogue availability. /projects can fail while
        // DVMDB is still responding (for example, a revision-graph error).
        // Probe the lightweight identity route and separately load the
        // project catalogue. A verified source must keep the same UUIDv7.
        const QString expected_repository = profile.pending ? QString() : profile.id;
        session->health->start(QUrl(endpoint + QStringLiteral("/repository")),
            [this, id, expected_repository](const QByteArray &body)
            {
                const QJsonDocument document = QJsonDocument::fromJson(body);
                SourceSession *current = this->source_sessions.value(id, nullptr);
                if (current != nullptr)
                    current->codec_error = document.isObject()
                        ? ModelCodecCompatibility::error(document.object()) : QString();
                if (!document.isObject()) return false;
                const QString identity = document.object().value(
                    QStringLiteral("repositoryId")).toString();
                return DvmdbIdentity::isUuidV7(identity)
                    && (current == nullptr || current->codec_error.isEmpty())
                    && (expected_repository.isEmpty() || identity == expected_repository);
            }, [this, id](HttpHealthMonitor::State state, const QString &error)
            {
                SourceSession *current = this->source_sessions.value(id, nullptr);
                if (current == nullptr) return;
                if (state == HttpHealthMonitor::State::Healthy)
                {
                    if (current->state == SourceState::Checking
                        || current->state == SourceState::Error) loadSource(id);
                }
                else if (state == HttpHealthMonitor::State::Offline)
                {
                    if (!current->loading)
                    {
                        current->state = SourceState::Error;
                        current->error = current->codec_error.isEmpty()
                            ? error : current->codec_error;
                        // Keep an existing catalogue as an explicitly offline cache;
                        // never claim it is currently writable.
                    }
                }
                else if (state == HttpHealthMonitor::State::Degraded
                         && current->state == SourceState::Checking)
                    current->error = error;
                populateServerTree();
                if (state == HttpHealthMonitor::State::Healthy
                    || state == HttpHealthMonitor::State::Offline)
                {
                    populateProjectTree();
                    rebuildGraph();
                }
                updateConnectionStatus();
                updateButtons();
            }, {}, 5000);
    }
    populateServerTree();
    updateConnectionStatus();
}

void ProjectsWidget::loadSource(const QString &id)
{
    SourceSession *session = this->source_sessions.value(id, nullptr);
    if (session == nullptr || session->loading || this->source_writer->isBusy()) return;
    ProjectServerProfile profile;
    bool found = false;
    for (const ProjectServerProfile &entry : this->servers)
        if (entry.id == id) { profile = entry; found = true; break; }
    if (!found) return;
    session->loading = true;
    session->state = SourceState::Loading;
    session->error.clear();
    populateServerTree();
    updateConnectionStatus();
    session->reader->load(profile, [this, id, profile](const SourceSnapshot &snapshot, const QString &error)
    {
        SourceSession *current = this->source_sessions.value(id, nullptr);
        if (current == nullptr) return;
        current->loading = false;
        if (!error.isEmpty())
        {
            current->state = SourceState::Error;
            current->error = error;
        }
        else if ((!current->repository_id.isEmpty()
                   && current->repository_id != snapshot.repository_id)
                 || (!profile.pending && profile.id != snapshot.repository_id))
        {
            current->state = SourceState::Error;
            current->error = tr("The database repository UUID changed; refusing to mix unrelated histories.");
        }
        else
        {
            if (profile.pending)
            {
                const QString verified_id = snapshot.repository_id;
                QTimer::singleShot(0, this, [this, id, verified_id]
                { promoteVerifiedSource(id, verified_id); });
                return;
            }
            current->repository_id = snapshot.repository_id;
            for (int index = this->projects.size() - 1; index >= 0; --index)
                if (this->projects.at(index).database_backed
                    && this->projects.at(index).server_id == id)
                    this->projects.removeAt(index);
            applySourceSnapshot(id, snapshot);
            const bool healthy = current->health == nullptr
                || current->health->currentState() == HttpHealthMonitor::State::Healthy;
            current->state = healthy ? SourceState::Connected : SourceState::Error;
            current->error = healthy ? QString() : tr("Network health check is failing");
            if (this->server_id == id && !this->select_after_write_project.isEmpty())
            {
                const QString project_to_select = this->select_after_write_project;
                const QString branch_to_select = this->select_after_write_branch;
                this->select_after_write_project.clear();
                this->select_after_write_branch.clear();
                for (const ProjectPreviewData &project : this->projects)
                    if (project.id == project_to_select)
                    {
                        select(SelectionKind::Project, project.id);
                        if (!branch_to_select.isEmpty())
                            for (const ProjectPreviewBranch &branch : project.branches)
                                if (branch.id == project.id + QStringLiteral(":") + branch_to_select
                                    || branch.name == branch_to_select)
                                {
                                    const bool edit_new = this->edit_after_branch_creation;
                                    this->edit_after_branch_creation = false;
                                    select(edit_new ? SelectionKind::Head
                                                    : SelectionKind::Branch, branch.id);
                                    if (edit_new)
                                        QTimer::singleShot(0, this,
                                            [this] { editProjectNetwork(); });
                                    break;
                                }
                        break;
                    }
            }
        }
        populateServerTree();
        updateEmptyState();
        updateConnectionStatus();
        if (this->server_id == id) updateInspector();
        updateButtons();
    });
}

void ProjectsWidget::connectSource()
{
    loadSource(this->server_id);
}

void ProjectsWidget::applySourceSnapshot(const QString &source_id, const SourceSnapshot &snapshot)
{
    for (const SourceProject &entry : snapshot.projects)
    {
        ProjectPreviewData project;
        project.id = source_id + QStringLiteral("|") + entry.id;
        project.backend_id = entry.id;
        project.repository_id = snapshot.repository_id;
        project.server_id = source_id;
        project.name = entry.name;
        project.database_backed = true;
        // P0 returns one immutable DAG per project, *independent* of branches.
        // A revision may be reachable from several branch HEADs or from none
        // except through a merge parent. Never assign it a fictitious owner.
        for (const SourceBranch &source_branch : entry.branches)
        {
            ProjectPreviewBranch branch;
            // Use a durable branch UUID when the backend supplies one.
            // Legacy sources without branch identities retain a source-scoped
            // name key. Never manufacture UUIDs in the GUI.
            branch.id = project.id + QStringLiteral(":")
                + (source_branch.id.isEmpty() ? source_branch.name : source_branch.id);
            branch.name = source_branch.name;
            branch.description = source_branch.description;
            branch.tags = source_branch.tags;
            branch.head = source_branch.head;
            branch.origin_branch_id = source_branch.origin_branch_id;
            branch.fork_revision_id = source_branch.fork_revision_id;
            project.branches.append(branch);
        }
        for (const SourceRevision &source_revision : entry.revisions)
        {
            ProjectPreviewRevision revision;
            revision.id = source_revision.id;
            revision.parents = source_revision.parents;
            revision.record_sha256 = source_revision.record_sha256;
            revision.message = source_revision.message;
            revision.title = tr("Revision %1").arg(DvmdbIdentity::shortLabel(revision.id));
            project.revisions.append(revision);
        }
        this->projects.append(project);
    }
    restoreVisibleSelection();
    populateProjectTree();
    populateRevisionTree();
    rebuildGraph();
    broadcastContext();
}

void ProjectsWidget::updateSourceFields()
{
    const bool source = this->selection_kind == SelectionKind::Server
        || this->selection_kind == SelectionKind::NewSource;
    this->database_connection->updateMode(source,
        this->selection_kind == SelectionKind::NewSource, this->source_writer->isBusy());
}

ProjectPreviewData *ProjectsWidget::currentProject()
{
    for (ProjectPreviewData &project : this->projects)
        if (project.id == this->project_id) return &project;
    return nullptr;
}

const ProjectPreviewData *ProjectsWidget::currentProject() const
{
    for (const ProjectPreviewData &project : this->projects)
        if (project.id == this->project_id) return &project;
    return nullptr;
}

ProjectPreviewBranch *ProjectsWidget::currentBranch()
{
    ProjectPreviewData *project = currentProject();
    if (project == nullptr) return nullptr;
    for (ProjectPreviewBranch &branch : project->branches)
        if (branch.id == this->branch_id) return &branch;
    return nullptr;
}

ProjectPreviewRevision *ProjectsWidget::selectedRevision()
{
    ProjectPreviewData *project = currentProject();
    if (project == nullptr) return nullptr;
    for (ProjectPreviewRevision &revision : project->revisions)
        if (revision.id == this->selected_id) return &revision;
    return nullptr;
}

QStringList ProjectsWidget::parseTags(const QString &text)
{
    QStringList output;
    for (const QString &tag : text.split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QString cleaned = tag.trimmed();
        if (!cleaned.isEmpty() && !output.contains(cleaned)) output.append(cleaned);
    }
    return output;
}

void ProjectsWidget::populateProjectTree()
{
    const QSignalBlocker blocked(this->project_tree);
    this->project_tree->clear();
    for (const ProjectServerProfile &server : this->servers)
    {
        if (!server.visible) continue;
        const bool preview = server.id == QStringLiteral("preview-local")
            || server.id == QStringLiteral("preview-remote");
        // Preserve the isolated demonstration explorer's original flat tree.
        if (preview && server.id != this->server_id) continue;
        QTreeWidgetItem *group = preview ? nullptr : newTreeItem(this->project_tree,
            server.name, keyFor(QStringLiteral("s"), server.id));
        if (group != nullptr)
        {
            QFont heading = group->font(0);
            heading.setBold(true);
            group->setFont(0, heading);
            group->setExpanded(true);
        }
        const SourceSession *session = this->source_sessions.value(server.id, nullptr);
        if (group != nullptr && session != nullptr && session->state == SourceState::Error)
        {
            group->setToolTip(0, tr("Unavailable · cached projects (read-only): %1").arg(session->error));
            group->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
        }
        for (const ProjectPreviewData &project : this->projects)
        {
            if (project.server_id != server.id) continue;
            QTreeWidgetItem *parent = group != nullptr
                ? newTreeChild(group, QStringLiteral("▣  ") + project.name,
                    keyFor(QStringLiteral("p"), project.id))
                : newTreeItem(this->project_tree, QStringLiteral("▣  ") + project.name,
                    keyFor(QStringLiteral("p"), project.id));
            parent->setExpanded(project.id == this->project_id);
            for (const ProjectPreviewBranch &branch : project.branches)
                newTreeChild(parent, QStringLiteral("⑂  ") + branch.name,
                             keyFor(QStringLiteral("b"), branch.id));
        }
    }
}

bool ProjectsWidget::isBranchHead(const QString &revision_id) const
{
    if (revision_id.isEmpty()) return false;
    const ProjectPreviewData *project = currentProject();
    if (project == nullptr) return false;
    for (const ProjectPreviewBranch &branch : project->branches)
        if (branch.head == revision_id) return true;
    return false;
}

void ProjectsWidget::populateRevisionTree()
{
    const QSignalBlocker blocked(this->revision_tree);
    this->revision_tree->clear();
    const ProjectPreviewData *project = currentProject();
    if (project == nullptr) return;

    QHash<QString, const ProjectPreviewRevision *> revisions_by_id;
    for (const ProjectPreviewRevision &revision : project->revisions)
        revisions_by_id.insert(revision.id, &revision);

    const auto displayTitle = [this](const ProjectPreviewRevision &revision)
    {
        const QString title = revision.message.trimmed().isEmpty()
            ? revision.title.trimmed() : revision.message.simplified();
        return title.isEmpty() ? tr("Revision without commit message") : title;
    };
    const auto labelRevision = [project](QTreeWidgetItem *item,
                                         const ProjectPreviewRevision &revision)
    {
        // The tree displays titles only: identities remain in Qt::UserRole so
        // duplicate names still select their own revision in the graph.
        QStringList head_branches;
        for (const ProjectPreviewBranch &branch : project->branches)
            if (branch.head == revision.id) head_branches.append(branch.name);
        QStringList details;
        if (!head_branches.isEmpty())
        {
            QFont label_font = item->font(0);
            label_font.setBold(true);
            item->setFont(0, label_font);
            details.append(QObject::tr("Editable head: %1").arg(head_branches.join(QStringLiteral(", "))));
        }
        if (!revision.description.isEmpty()) details.append(revision.description);
        item->setToolTip(0, details.join(QLatin1Char('\n')));
    };

    if (!this->show_full_revision_tree)
    {
        // One entry per *revision*, even when several branches share a head.
        QSet<QString> shown;
        for (const ProjectPreviewBranch &branch : project->branches)
        {
            const ProjectPreviewRevision *revision = revisions_by_id.value(branch.head, nullptr);
            if (revision == nullptr || shown.contains(revision->id)) continue;
            shown.insert(revision->id);
            QTreeWidgetItem *item = newTreeItem(this->revision_tree, displayTitle(*revision),
                keyFor(QStringLiteral("r"), revision->id));
            labelRevision(item, *revision);
        }
        return;
    }

    // QTreeWidget represents a tree rather than a DAG. Nest every revision
    // under its first available parent; additional merge edges stay visible in
    // the center graph, while each revision appears only once in the sidebar.
    QHash<QString, QTreeWidgetItem *> items_by_id;
    QSet<QString> visiting;
    std::function<QTreeWidgetItem *(const ProjectPreviewRevision &)> append_revision;
    append_revision = [&](const ProjectPreviewRevision &revision) -> QTreeWidgetItem *
    {
        if (QTreeWidgetItem *item = items_by_id.value(revision.id, nullptr)) return item;
        if (visiting.contains(revision.id)) return nullptr; // protect against bad preview data
        visiting.insert(revision.id);
        QTreeWidgetItem *parent_item = nullptr;
        for (const QString &parent_id : revision.parents)
        {
            if (const ProjectPreviewRevision *parent_revision = revisions_by_id.value(parent_id, nullptr))
            {
                parent_item = append_revision(*parent_revision);
                if (parent_item != nullptr) break;
            }
        }
        QTreeWidgetItem *item = parent_item != nullptr
            ? newTreeChild(parent_item, displayTitle(revision), keyFor(QStringLiteral("r"), revision.id))
            : newTreeItem(this->revision_tree, displayTitle(revision), keyFor(QStringLiteral("r"), revision.id));
        items_by_id.insert(revision.id, item);
        labelRevision(item, revision);
        visiting.remove(revision.id);
        return item;
    };
    for (const ProjectPreviewRevision &revision : project->revisions)
        append_revision(revision);
    this->revision_tree->expandAll();
}

void ProjectsWidget::rebuildGraph()
{
    QVector<ProjectGraphNode> nodes;
    this->graph_revision_lookup.clear();
    // Build the immutable graph topology first. Placement is a separate,
    // deterministic compound-layered pass over database, project and revision
    // nodes; fetching order and branch labels must not dictate geometry.
    for (const ProjectServerProfile &source : this->servers)
    {
        if (!source.visible) continue;
        const bool preview = source.id == QStringLiteral("preview-local")
            || source.id == QStringLiteral("preview-remote");
        if (preview && source.id != this->server_id) continue;
        const QString server_node_id = QStringLiteral("group:server:") + source.id;
        for (const ProjectPreviewData &project : this->projects)
        {
            if (project.server_id != source.id
                || (preview && project.id != this->project_id)) continue;
            const QString project_node_id = QStringLiteral("group:project:") + project.id;
            ProjectGraphNode project_node;
            project_node.kind = ProjectGraphNode::Kind::Project;
            project_node.id = project_node_id;
            project_node.title = project.name;
            project_node.branch = tr("PROJECT · %n branch(es)", nullptr, project.branches.size());
            // A partial catalogue can omit a branch HEAD revision. Keep the
            // source visible without inventing a revision card for that ref.
            QSet<QString> loaded_revisions;
            for (const ProjectPreviewRevision &revision : project.revisions)
                loaded_revisions.insert(revision.id);
            int missing_heads = 0;
            for (const ProjectPreviewBranch &branch : project.branches)
                if (!branch.head.isEmpty() && !loaded_revisions.contains(branch.head))
                    ++missing_heads;
            if (missing_heads > 0)
                project_node.branch += tr(" · %n HEAD(s) not loaded", nullptr, missing_heads);
            project_node.container_id = server_node_id;
            nodes.append(project_node);
            // Immutable revision records have no owning branch: several HEADs
            // can legitimately reach the same revision. Label each revision
            // with its actual reachable branches instead of "Branch: Unknown".
            QHash<QString, QStringList> revision_parents;
            for (const ProjectPreviewRevision &revision : project.revisions)
                revision_parents.insert(revision.id, revision.parents);
            QHash<QString, QStringList> reachable_branch_names;
            for (const ProjectPreviewBranch &branch : project.branches)
            {
                QStringList pending{branch.head};
                QSet<QString> visited;
                while (!pending.isEmpty())
                {
                    const QString revision_id = pending.takeLast();
                    if (revision_id.isEmpty() || visited.contains(revision_id)
                        || !revision_parents.contains(revision_id)) continue;
                    visited.insert(revision_id);
                    reachable_branch_names[revision_id].append(branch.name);
                    pending.append(revision_parents.value(revision_id));
                }
            }
            for (const ProjectPreviewRevision &revision : project.revisions)
            {
                ProjectGraphNode node;
                node.id = preview ? revision.id : project.id + QStringLiteral("|") + revision.id;
                node.title = revision.message.trimmed().isEmpty()
                    ? revision.title : revision.message.simplified();
                if (!revision.branch.isEmpty())
                    node.branch = tr("Branch: %1").arg(revision.branch);
                else
                {
                    QStringList names = reachable_branch_names.value(revision.id);
                    names.sort(Qt::CaseInsensitive);
                    if (names.size() == 1)
                        node.branch = tr("Branch: %1").arg(names.first());
                    else if (!names.isEmpty())
                        node.branch = tr("Branches: %1").arg(names.join(QStringLiteral(", ")));
                    else
                        node.branch = tr("No branch currently references this revision");
                }
                node.display_id = revision.id;
                for (const QString &parent : revision.parents)
                    node.parents.append(preview ? parent : project.id + QStringLiteral("|") + parent);
                node.tags = revision.tags;
                node.project_group_id = project_node_id;
                for (const ProjectPreviewBranch &branch : project.branches)
                    if (!branch.head.isEmpty() && branch.head == revision.id)
                        node.heads.append({branch.id, branch.name});
                std::sort(node.heads.begin(), node.heads.end(),
                    [](const ProjectGraphBranchHead &lhs, const ProjectGraphBranchHead &rhs)
                    { return lhs.id < rhs.id; });
                this->graph_revision_lookup.insert(node.id, qMakePair(project.id, revision.id));
                nodes.append(node);
            }
            // A branch has one writable HEAD even before its first commit.
            // HEAD is a working state, not an immutable revision node.
            for (const ProjectPreviewBranch &branch : project.branches)
            {
                ProjectGraphNode head;
                head.kind = ProjectGraphNode::Kind::Branch; // Graph HEAD node.
                head.id = QStringLiteral("group:branch:") + branch.id;
                head.title = tr("HEAD");
                head.branch = tr("Branch: %1").arg(branch.name);
                head.project_group_id = project_node_id;
                if (!branch.head.isEmpty() && loaded_revisions.contains(branch.head))
                    head.head_revision_id = preview ? branch.head
                        : project.id + QStringLiteral("|") + branch.head;
                else if (!branch.fork_revision_id.isEmpty()
                         && loaded_revisions.contains(branch.fork_revision_id))
                    head.head_revision_id = preview ? branch.fork_revision_id
                        : project.id + QStringLiteral("|") + branch.fork_revision_id;
                nodes.append(head);
            }
        }
        // Also show checked databases with an empty catalogue when the graph
        // contains projects from other sources. A server does not cease to
        // exist just because it currently contains no projects.
        ProjectGraphNode server_node;
        server_node.kind = ProjectGraphNode::Kind::Server;
        server_node.personal_server = source.personal;
        server_node.pending_server = source.pending;
        server_node.id = server_node_id;
        server_node.title = source.name;
        server_node.branch = source.connection_type == QStringLiteral("local")
            ? tr("LOCAL DATABASE") : source.pending ? tr("PENDING VERIFICATION") : tr("REST SERVER");
        nodes.append(server_node);
    }
    const double aspect = double(qMax(1, this->graph->width()))
        / double(qMax(1, this->graph->height()));
    ProjectGraphPlacement::place(nodes, aspect);
    this->graph->setNodes(nodes);
    QString graph_selection;
    if (this->selection_kind == SelectionKind::Server)
        graph_selection = QStringLiteral("group:server:") + this->selected_id;
    else if (this->selection_kind == SelectionKind::Project)
        graph_selection = QStringLiteral("group:project:") + this->selected_id;
    else if (this->selection_kind == SelectionKind::Branch
             || this->selection_kind == SelectionKind::Head)
        graph_selection = QStringLiteral("group:branch:") + this->selected_id;
    else if (this->selection_kind == SelectionKind::Revision && currentProject() != nullptr)
        graph_selection = isPreviewSource() ? this->selected_id
            : this->project_id + QStringLiteral("|") + this->selected_id;
    this->graph->setSelection(graph_selection);
}

void ProjectsWidget::selectTreeItem(QTreeWidget *tree, const QString &key)
{
    const QSignalBlocker blocked(tree);
    tree->clearSelection();
    if (key.isEmpty()) return;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
    {
        if ((*it)->data(0, role_key).toString() == key)
        {
            QTreeWidgetItem *ancestor = (*it)->parent();
            while (ancestor != nullptr)
            {
                ancestor->setExpanded(true);
                ancestor = ancestor->parent();
            }
            tree->setCurrentItem(*it);
            tree->scrollToItem(*it);
            return;
        }
    }
}

void ProjectsWidget::select(SelectionKind kind, const QString &id)
{
    if (this->creation_kind != CreationKind::None
        && (kind != this->selection_kind || id != this->selected_id))
    {
        if (this->source_writer->isBusy()) return;
        if (currentCreationForm() != nullptr && currentCreationForm()->isDirty()
            && QMessageBox::question(this, tr("Discard creation draft?"),
                tr("Changing selection will discard the unfinished creation form."),
                QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel)
                != QMessageBox::Discard)
        {
            this->updating_ui = true;
            selectTreeItem(this->server_tree, keyFor(QStringLiteral("s"), this->server_id));
            selectTreeItem(this->project_tree,
                this->selection_kind == SelectionKind::Project
                    ? keyFor(QStringLiteral("p"), this->selected_id)
                    : this->selection_kind == SelectionKind::Branch
                        || this->selection_kind == SelectionKind::Head
                        ? keyFor(QStringLiteral("b"), this->selected_id) : QString());
            selectTreeItem(this->revision_tree,
                this->selection_kind == SelectionKind::Revision
                    ? keyFor(QStringLiteral("r"), this->selected_id) : QString());
            this->updating_ui = false;
            return;
        }
        cancelCreation();
    }
    if (this->source_writer->isBusy() && kind == SelectionKind::Server && id != this->server_id)
    {
        selectTreeItem(this->server_tree, keyFor(QStringLiteral("s"), this->server_id));
        return;
    }
    if (kind == SelectionKind::Server)
    {
        bool found = false;
        for (const ProjectServerProfile &server : this->servers)
            if (server.id == id) { found = true; break; }
        if (!found) return;
        if (this->server_id != id)
        {
            this->server_id = id;
            this->project_id.clear();
            this->branch_id.clear();
            for (const ProjectPreviewData &project : this->projects)
                if (project.server_id == id)
                {
                    this->project_id = project.id;
                    if (!project.branches.isEmpty()) this->branch_id = project.branches.first().id;
                    break;
                }
            populateProjectTree();
            populateRevisionTree();
            rebuildGraph();
        }
    }
    else if (kind == SelectionKind::Project)
    {
        bool found = false;
        for (const ProjectPreviewData &project : this->projects)
            if (project.id == id) { found = true; break; }
        if (!found) return;
        if (id != this->project_id)
        {
            for (const ProjectPreviewData &project : this->projects)
                if (project.id == id && project.server_id != this->server_id)
                {
                    if (this->source_writer->isBusy()) return;
                    this->server_id = project.server_id;
                    populateServerTree();
                    populateProjectTree();
                    break;
                }
            this->project_id = id;
            const ProjectPreviewData *project = currentProject();
            this->branch_id = project != nullptr && !project->branches.isEmpty()
                ? project->branches.first().id : QString();
            populateRevisionTree();
            rebuildGraph();
        }
    }
    else if (kind == SelectionKind::Branch || kind == SelectionKind::Head)
    {
        bool found = false;
        for (const ProjectPreviewData &project : this->projects)
            for (const ProjectPreviewBranch &branch : project.branches)
                if (branch.id == id)
                {
                    if (this->project_id != project.id)
                    {
                        if (this->server_id != project.server_id)
                        {
                            if (this->source_writer->isBusy()) return;
                            this->server_id = project.server_id;
                            populateServerTree();
                            populateProjectTree();
                        }
                        this->project_id = project.id;
                        populateRevisionTree();
                        rebuildGraph();
                    }
                    this->branch_id = branch.id;
                    found = true;
                    break;
                }
        if (!found) return;
    }
    else if (kind == SelectionKind::Revision)
    {
        ProjectPreviewData *project = currentProject();
        if (project == nullptr) return;
        bool found = false;
        for (const ProjectPreviewRevision &revision : project->revisions)
            if (revision.id == id)
            {
                // Preview revisions retain their synthetic branch ownership.
                // Real database revisions do not: selection must not invent
                // a source branch just because a revision is shared.
                if (!project->database_backed)
                    for (const ProjectPreviewBranch &branch : project->branches)
                        if (branch.name == revision.branch) this->branch_id = branch.id;
                found = true;
                break;
            }
        if (!found) return;
        // The graph can select historical revisions even when the sidebar is
        // filtered to heads. Reveal the full tree to keep that selection visible.
        if (!this->show_full_revision_tree && !isBranchHead(id))
        {
            this->show_full_revision_tree = true;
            this->full_tree_button->setChecked(true);
            populateRevisionTree();
        }
    }

    this->selection_kind = kind;
    this->selected_id = id;
    this->updating_ui = true;
    selectTreeItem(this->server_tree, keyFor(QStringLiteral("s"), this->server_id));
    selectTreeItem(this->project_tree,
                   kind == SelectionKind::Project ? keyFor(QStringLiteral("p"), id)
                   : kind == SelectionKind::Branch || kind == SelectionKind::Head
                       ? keyFor(QStringLiteral("b"), id) : QString());
    selectTreeItem(this->revision_tree,
                   kind == SelectionKind::Revision ? keyFor(QStringLiteral("r"), id) : QString());
    this->updating_ui = false;
    QString graph_selection;
    if (kind == SelectionKind::Server)
        graph_selection = QStringLiteral("group:server:") + id;
    else if (kind == SelectionKind::Project)
        graph_selection = QStringLiteral("group:project:") + id;
    else if (kind == SelectionKind::Branch || kind == SelectionKind::Head)
        graph_selection = QStringLiteral("group:branch:") + id;
    else if (kind == SelectionKind::Revision)
        graph_selection = isPreviewSource() ? id : this->project_id + QStringLiteral("|") + id;
    this->graph->setSelection(graph_selection);
    updateInspector();
    updateEmptyState();
    if (this->inspector_dock->isHidden())
        this->inspector_dock->show();
    updateButtons();
    broadcastContext();
}

ProjectsInspectorOverview *ProjectsWidget::activeOverview() const
{
    switch (this->selection_kind)
    {
    case SelectionKind::Project: return this->project_overview;
    case SelectionKind::Branch: return this->branch_overview;
    case SelectionKind::Head:
    case SelectionKind::Revision: return this->revision_network_overview;
    case SelectionKind::Server:
    case SelectionKind::NewSource:
    case SelectionKind::None: return this->database_overview;
    }
    return this->database_overview;
}

void ProjectsWidget::updateInspectorTabs()
{
    const int old_tab = this->inspector_tabs->currentIndex();
    const bool same_kind = this->inspector_tab_kind == this->selection_kind;
    this->inspector_tab_kind = this->selection_kind;
    this->inspector_tabs->clear();
    // clear() removes pages from the tab bar. Detached pages must not be
    // visible as direct QTabWidget children on Database/Project/Branch views.
    this->revision_branch_page->hide();
    this->revision_details_page->hide();
    this->database_connection->hide();
    for (ProjectsInspectorRelations *panel : {
             static_cast<ProjectsInspectorRelations *>(this->database_relations),
             static_cast<ProjectsInspectorRelations *>(this->project_relations),
             static_cast<ProjectsInspectorRelations *>(this->branch_relations),
             static_cast<ProjectsInspectorRelations *>(this->revision_relations)})
        panel->hide();
    this->inspector_tabs->addTab(this->inspector_overview_page, tr("Overview"));
    // A database has no Overview until its connection has been created.
    // Keep the reason discoverable on the disabled tab.
    const bool creating_database = this->selection_kind == SelectionKind::NewSource;
    this->inspector_tabs->setTabEnabled(0, !creating_database);
    this->inspector_tabs->setTabToolTip(0, creating_database
        ? tr("Create the database connection in the Connection tab first.") : QString());
    if (this->selection_kind == SelectionKind::Server
        || this->selection_kind == SelectionKind::NewSource)
        this->inspector_tabs->addTab(this->database_connection, tr("Connection"));

    ProjectsInspectorRelations *related = nullptr;
    QString title;
    if (this->selection_kind == SelectionKind::Server)
    {
        this->database_relations->populate(this->projects, this->server_id);
        related = this->database_relations;
        title = tr("Projects");
    }
    else if (this->selection_kind == SelectionKind::Project)
    {
        this->project_relations->populate(currentProject());
        related = this->project_relations;
        title = tr("Branches");
    }
    else if (this->selection_kind == SelectionKind::Branch)
    {
        this->branch_relations->populate(currentProject(), currentBranch());
        related = this->branch_relations;
        title = tr("Revisions");
    }
    else if (this->selection_kind == SelectionKind::Revision)
    {
        this->revision_relations->populate(currentProject(), selectedRevision());
        related = this->revision_relations;
        title = tr("Ancestry");
    }
    // The network's Overview is the first tab; the existing commit form has
    // a mode-specific HEAD/Revision tab, followed by independent branch data.
    if (this->selection_kind == SelectionKind::Head
        || this->selection_kind == SelectionKind::Revision)
    {
        this->inspector_tabs->addTab(this->revision_details_page,
            this->selection_kind == SelectionKind::Head ? tr("HEAD") : tr("Revision"));
        this->inspector_tabs->addTab(this->revision_branch_page, tr("Branch"));
    }
    // An empty database still needs its Projects tab to offer Create project.
    if (related != nullptr && (this->selection_kind == SelectionKind::Server
                               || this->selection_kind == SelectionKind::Project
                               || related->list->count() > 0))
        this->inspector_tabs->addTab(related, title);
    this->inspector_tabs->setCurrentIndex(this->selection_kind == SelectionKind::NewSource
        ? 1 : same_kind
            ? std::min(std::max(0, old_tab), this->inspector_tabs->count() - 1) : 0);
}

void ProjectsWidget::activateInspectorRelation(const QString &key)
{
    if (key.startsWith(QStringLiteral("p:"))) select(SelectionKind::Project, key.mid(2));
    else if (key.startsWith(QStringLiteral("b:"))) select(SelectionKind::Branch, key.mid(2));
    else if (key.startsWith(QStringLiteral("r:"))) select(SelectionKind::Revision, key.mid(2));
}

ProjectsInspectorCreationForm *ProjectsWidget::currentCreationForm() const
{
    if (this->creation_kind == CreationKind::Project) return this->project_creation;
    if (this->creation_kind == CreationKind::Branch) return this->branch_creation;
    return nullptr;
}

void ProjectsWidget::beginCreation(bool branch)
{
    if (this->creation_kind != CreationKind::None || this->source_writer->isBusy()) return;
    if (branch && currentProject() == nullptr) return;
    if (!branch && (this->selection_kind != SelectionKind::Server || currentServer() == nullptr)) return;
    if (!isPreviewSource() && !isSourceReady(this->server_id)) return;
    this->creation_kind = branch ? CreationKind::Branch : CreationKind::Project;
    this->creation_server_id = this->server_id;
    this->creation_project_id = branch ? this->project_id : QString();
    if (branch)
    {
        QString chosen;
        if (this->selection_kind == SelectionKind::Revision)
            chosen = QStringLiteral("revision:") + this->selected_id;
        else if (this->selection_kind == SelectionKind::Branch
                 || this->selection_kind == SelectionKind::Head)
            if (const ProjectPreviewBranch *selected = currentBranch())
                chosen = QStringLiteral("branch:") + selected->id;
        this->branch_creation->begin(*currentProject(), chosen);
        this->creation_forms->setCurrentWidget(this->branch_creation);
    }
    else
    {
        this->project_creation->reset();
        this->creation_forms->setCurrentWidget(this->project_creation);
    }
    this->inspector_heading->setText(branch ? tr("Create branch") : tr("Create project"));
    this->inspector_subtitle->setText(branch
        ? tr("Project: %1").arg(currentProject()->name)
        : tr("Database: %1").arg(currentServer()->name));
    this->inspector_subtitle->show();
    this->inspector_content_stack->setCurrentIndex(1);
    validateCreation();
    updateButtons();
    if (ProjectsInspectorCreationForm *form = currentCreationForm()) form->name->setFocus();
}

void ProjectsWidget::validateCreation()
{
    if (this->creation_kind == CreationKind::Project)
        this->project_creation->validate(this->projects, this->creation_server_id);
    else if (this->creation_kind == CreationKind::Branch)
        this->branch_creation->validate(currentProject(), this->creation_project_id,
                                        sourceBranchForRevision);
    else return;
    updateButtons();
}

void ProjectsWidget::cancelCreation()
{
    if (this->creation_kind == CreationKind::None || this->source_writer->isBusy()) return;
    this->creation_kind = CreationKind::None;
    this->creation_project_id.clear();
    this->creation_server_id.clear();
    this->project_creation->reset();
    this->branch_creation->reset();
    updateInspector();
    updateButtons();
}

void ProjectsWidget::submitCreation()
{
    if (this->creation_kind == CreationKind::None || this->source_writer->isBusy()) return;
    validateCreation();
    if (currentCreationForm() == nullptr || !currentCreationForm()->isValid()) return;
    if (this->creation_server_id != this->server_id
        || (this->creation_kind == CreationKind::Branch
            && this->creation_project_id != this->project_id)) return;
    if (this->creation_kind == CreationKind::Project) createProject();
    else createBranch();
}

void ProjectsWidget::updateInspector()
{
    this->updating_ui = true;
    ProjectPreviewData *project = currentProject();
    ProjectsInspectorPresentation view;
    if (this->selection_kind == SelectionKind::NewSource)
        view = ProjectsInspectorPresentations::newDatabase();
    else if (this->selection_kind == SelectionKind::Server)
    {
        const ProjectServerProfile *server = currentServer();
        const SourceSession *session = server == nullptr ? nullptr
            : this->source_sessions.value(server->id, nullptr);
        view = ProjectsInspectorPresentations::database(server,
            session == nullptr ? QString() : session->repository_id);
    }
    else if (this->selection_kind == SelectionKind::Project && project != nullptr)
        view = ProjectsInspectorPresentations::project(project);
    else if (this->selection_kind == SelectionKind::Branch)
        view = ProjectsInspectorPresentations::branch(this->projects, this->selected_id);
    else if (this->selection_kind == SelectionKind::Head)
        view = ProjectsInspectorPresentations::head(currentBranch(), project);
    else if (this->selection_kind == SelectionKind::Revision)
        view = ProjectsInspectorPresentations::revision(selectedRevision(), project,
            inspectedRevisionBranch());
    else
        view = ProjectsInspectorPresentations::empty();
    const bool new_source = this->selection_kind == SelectionKind::NewSource;
    if (this->selection_kind == SelectionKind::Server && currentServer() != nullptr)
    {
        const ProjectServerProfile *server = currentServer();
        const int kind_index = this->database_connection->source_kind_combo->findData(server->connection_type);
        this->database_connection->source_kind_combo->setCurrentIndex(kind_index < 0 ? 0 : kind_index);
        {
            const QSignalBlocker personal_blocker(this->database_connection->personal_server_checkbox);
            this->database_connection->personal_server_checkbox->setChecked(server->personal);
        }
        this->database_connection->server_endpoint_edit->setText(server->endpoint);
        this->database_connection->local_path_edit->setText(server->local_path);
        const int index = this->database_connection->server_auth_combo->findData(server->auth_mode);
        this->database_connection->server_auth_combo->setCurrentIndex(index < 0 ? 0 : index);
    }
    updateSourceFields();
    if (this->creation_kind == CreationKind::Project)
    {
        view.heading = tr("Create project");
        view.subtitle = tr("Database: %1").arg(currentServer() == nullptr ? QString() : currentServer()->name);
    }
    else if (this->creation_kind == CreationKind::Branch)
    {
        view.heading = tr("Create branch");
        view.subtitle = tr("Project: %1").arg(project == nullptr ? QString() : project->name);
    }
    // A domain overview now owns its fields and presentation state. No
    // ProjectsWidget member aliases point at whichever domain was last shown.
    ProjectsInspectorOverview *overview = activeOverview();
    this->inspector_overview_stack->setCurrentWidget(overview);
    this->inspector_heading->setText(view.heading);
    this->inspector_subtitle->setText(view.subtitle);
    this->inspector_subtitle->setVisible(!view.subtitle.isEmpty());
    this->database_connection->source_name_edit->setText(view.name);
    this->database_connection->source_name_edit->setVisible(new_source);
    const bool real_data = project != nullptr && project->database_backed
        && (this->selection_kind == SelectionKind::Project
            || this->selection_kind == SelectionKind::Branch
            || this->selection_kind == SelectionKind::Head
            || this->selection_kind == SelectionKind::Revision);
    overview->present(view.name, view.tags, view.description, view.metadata,
                      view.editable && !real_data,
                      this->selection_kind != SelectionKind::None
                          && this->selection_kind != SelectionKind::Head
                          && this->selection_kind != SelectionKind::Revision);
    // Project UUIDs are immutable, but the display name is stored by DVMDB.
    // Keep Tags/Description selectable until their persistent edit API exists.
    if (real_data && this->selection_kind == SelectionKind::Project)
    {
        overview->name->setEnabled(!this->source_writer->isBusy());
        overview->tags->setEnabled(true);
        overview->description->setEnabled(true);
        overview->name->setReadOnly(false);
        overview->tags->setReadOnly(true);
        overview->description->setReadOnly(true);
    }
    else if (this->selection_kind == SelectionKind::Head
             || this->selection_kind == SelectionKind::Revision)
    {
        // The network Overview and commit-details page are independent. The
        // latter retains its own tags, message and description editors.
        this->revision_overview->present(view.name, view.tags, view.description,
                                         view.metadata, true, true);
        const bool has_branch = inspectedRevisionBranch() != nullptr;
        overview = this->revision_overview;
        overview->name->setEnabled(has_branch && !this->source_writer->isBusy());
        overview->tags->setEnabled(has_branch && !this->source_writer->isBusy());
        overview->description->setEnabled(has_branch && !this->source_writer->isBusy());
        overview->name->setReadOnly(false);
        overview->tags->setReadOnly(false);
        overview->description->setReadOnly(false);
        {
            // present() blocks signals while replacing Overview's values;
            // explicitly refresh the Branch tab too on every selection.
            const QSignalBlocker tags_blocker(this->revision_branch_page->tags);
            const QSignalBlocker description_blocker(this->revision_branch_page->description);
            this->revision_branch_page->tags->setText(this->revision_overview->tags->text());
            this->revision_branch_page->description->setPlainText(
                this->revision_overview->description->toPlainText());
        }
        this->revision_branch_page->tags->setEnabled(has_branch && !this->source_writer->isBusy());
        this->revision_branch_page->description->setEnabled(has_branch && !this->source_writer->isBusy());
        this->revision_branch_page->tags->setReadOnly(false);
        this->revision_branch_page->description->setReadOnly(false);
        this->revision_overview->branch_label->setText(view.name);
        this->revision_inspector->identity_footer->setText(view.metadata);
        const QSignalBlocker message_blocker(this->revision_overview->commit_message);
        this->revision_overview->commit_message->setPlainText(
            this->selection_kind == SelectionKind::Head
                ? this->head_commit_drafts.value(this->branch_id)
                : view.commit_message);
        this->revision_overview->commit_message->setEnabled(
            !this->source_writer->isBusy());
        this->revision_overview->status->setText(this->selection_kind == SelectionKind::Head
            ? tr("Status: Writable HEAD")
            : tr("Status: Immutable revision"));
    }
    else
    {
        overview->name->setReadOnly(false);
        overview->tags->setReadOnly(false);
        overview->description->setReadOnly(false);
    }
    const ProjectServerProfile *overview_database = currentServer();
    this->database_overview->personal_banner->setVisible(
        this->selection_kind == SelectionKind::Server && overview_database != nullptr
        && overview_database->personal);
    updateInspectorTabs();
    refreshNetworkSummary();
    // Route to the inspector associated with the selected model object.
    QWidget *active_inspector = this->database_inspector;
    if (this->selection_kind == SelectionKind::Project)
        active_inspector = this->project_inspector;
    else if (this->selection_kind == SelectionKind::Branch)
        active_inspector = this->branch_inspector;
    else if (this->selection_kind == SelectionKind::Head
             || this->selection_kind == SelectionKind::Revision)
        active_inspector = this->revision_inspector;
    this->inspector_actions_stack->showPanel(active_inspector);
    this->inspector_content_stack->setCurrentIndex(
        this->creation_kind == CreationKind::None ? 0 : 1);
    this->updating_ui = false;
    updateConnectionStatus();
    updateButtons();
}

void ProjectsWidget::updateButtons()
{
    const bool busy = this->source_writer->isBusy();
    const bool ready = (isPreviewSource() || isSourceReady(this->server_id)) && !busy;
    const bool editing_project = this->creation_kind == CreationKind::Project;
    const bool editing_branch = this->creation_kind == CreationKind::Branch;
    const bool creating = editing_project || editing_branch;
    const bool new_source = this->selection_kind == SelectionKind::NewSource;
    const bool valid = currentCreationForm() != nullptr && currentCreationForm()->isValid();
    const ProjectPreviewData *project = currentProject();
    const bool backed = project != nullptr && project->database_backed;
    const bool preview = isPreviewSource();
    const ProjectServerProfile *database = currentServer();
    const bool removable_connection = this->selection_kind == SelectionKind::Server
        && !creating && database != nullptr
        && (database->connection_type != QStringLiteral("local")
            || QDir::cleanPath(database->local_path)
                != QDir::cleanPath(defaultLocalDvmdbPath()));
    this->database_inspector->update(new_source,
        creating && this->selection_kind == SelectionKind::Server,
        busy, valid);
    this->database_overview->create_project->setVisible(
        this->selection_kind == SelectionKind::Server && !creating);
    this->database_overview->create_project->setEnabled(ready);
    this->database_relations->create_project->setEnabled(ready);
    this->project_overview->import_network->setVisible(
        this->selection_kind == SelectionKind::Project && !creating);
    this->project_overview->import_network->setEnabled(ready && project != nullptr);
    this->project_overview->create_branch->setVisible(
        this->selection_kind == SelectionKind::Project && !creating);
    this->project_overview->create_branch->setEnabled(ready && project != nullptr);
    this->project_relations->create_branch->setVisible(
        this->selection_kind == SelectionKind::Project && !creating);
    this->project_relations->create_branch->setEnabled(ready && project != nullptr);
    this->database_connection->delete_connection->setVisible(removable_connection);
    this->database_connection->delete_connection->setEnabled(removable_connection && !busy);
    this->project_inspector->update(creating && this->selection_kind == SelectionKind::Project,
        busy, preview && project != nullptr, valid);
    this->branch_inspector->update(creating && this->selection_kind == SelectionKind::Branch,
        busy, ready && project != nullptr, backed, preview && currentBranch() != nullptr, valid);
    QString saved_commit_state;
    if (this->selection_kind == SelectionKind::Head && backed && project != nullptr) {
        const ProjectPreviewBranch *current_branch = currentBranch();
        const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
        if (current_branch != nullptr && target.repository_id == project->repository_id) {
            QString state_error;
            saved_commit_state = PersonalWorkingCopyClient::recordedCommitState(
                target, project->backend_id,
                current_branch->id.mid(project->id.size() + 1), &state_error);
            if (!state_error.isEmpty()) saved_commit_state = QStringLiteral("error");
        }
    }
    this->revision_inspector->update(creating && (this->selection_kind == SelectionKind::Revision
                                                  || this->selection_kind == SelectionKind::Head),
        busy, ready && project != nullptr, valid,
        this->selection_kind == SelectionKind::Head, backed,
        inspectedRevisionBranch() != nullptr,
        !this->revision_overview->commit_message->toPlainText().trimmed().isEmpty()
            || !saved_commit_state.isEmpty());
    const ProjectPreviewBranch *deletion_branch = inspectedRevisionBranch();
    const bool deletion_unborn_head = this->selection_kind == SelectionKind::Head
        && deletion_branch != nullptr && deletion_branch->head.isEmpty();
    const bool may_delete = backed && ready && !creating
        && inspectedRevisionBranch() != nullptr;
    this->revision_branch_page->delete_branch->setEnabled(may_delete);
    this->revision_network_overview->delete_revision->setVisible(
        this->selection_kind == SelectionKind::Revision
        || this->selection_kind == SelectionKind::Head);
    this->revision_network_overview->delete_revision->setEnabled(
        may_delete && (this->selection_kind == SelectionKind::Revision
            || this->selection_kind == SelectionKind::Head));
    this->revision_network_overview->delete_revision->setText(
        this->selection_kind == SelectionKind::Head
            ? deletion_unborn_head ? tr("Delete unborn HEAD and branch…")
                                   : tr("Delete HEAD working changes…")
            : tr("Delete revision and later history…"));
    this->revision_network_overview->delete_revision->setToolTip(
        this->selection_kind == SelectionKind::Head && !deletion_unborn_head
            ? tr("Discard this HEAD's saved private working copy; immutable revisions remain unchanged.")
            : QString());
    this->revision_inspector->commit_head->setText(saved_commit_state.isEmpty()
        ? tr("Commit") : tr("Reconcile Commit"));
    this->revision_inspector->commit_head->setToolTip(saved_commit_state.isEmpty()
        ? tr("Commit the saved private draft as an immutable revision")
        : tr("Recover the previous Commit operation before another Commit is allowed"));
    if (!saved_commit_state.isEmpty() && this->selection_kind == SelectionKind::Head) {
        this->revision_overview->status->setText(saved_commit_state == QStringLiteral("pending")
            ? tr("Status: Previous Commit pending — reconcile before continuing")
            : saved_commit_state == QStringLiteral("succeeded")
                ? tr("Status: Commit confirmed — acknowledge result")
                : saved_commit_state == QStringLiteral("rejected")
                    ? tr("Status: Commit rejected — acknowledge result")
                    : tr("Status: Local Commit record unreadable — new Commit blocked"));
    }
    if (saved_commit_state == QStringLiteral("pending") && project != nullptr
        && this->selection_kind == SelectionKind::Head) {
        const ProjectPreviewBranch *selected_branch = currentBranch();
        const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
        if (selected_branch != nullptr && target.repository_id == project->repository_id) {
            const QString project_id = project->backend_id;
            const QString branch_id = selected_branch->id.mid(project->id.size() + 1);
            const QString key = target.repository_id + QLatin1Char(':') + project_id
                + QLatin1Char(':') + branch_id;
            if (!this->probed_commit_intents.contains(key)) {
                this->probed_commit_intents.insert(key);
                QTimer::singleShot(0, this, [this, target, project_id, branch_id] {
                    PersonalWorkingCopyClient *recovery = new PersonalWorkingCopyClient(this);
                    recovery->reconcileRecordedCommit(target, project_id, branch_id,
                        [this, recovery](const QString &, const QString &error) {
                        if (!error.isEmpty())
                            qDebug() << "Background Commit status probe:" << error;
                        recovery->deleteLater();
                        updateButtons();
                    });
                });
            }
        }
    }
    this->revision_overview->setCommitExplanationVisible(
        this->selection_kind == SelectionKind::Head && !creating);
    const bool selected_network = this->selection_kind == SelectionKind::Head
        || this->selection_kind == SelectionKind::Revision;
    this->revision_network_overview->open_network->setEnabled(
        selected_network && ready && backed);
    this->revision_network_overview->open_network->setText(tr("Open network"));
    const ProjectPreviewBranch *selected_branch = inspectedRevisionBranch();
    const bool unborn_head = selected_network
        && this->selection_kind == SelectionKind::Head
        && selected_branch != nullptr && selected_branch->head.isEmpty();
    const PersonalRepositoryTarget personal = resolvePersonalRepositoryTarget();
    const bool import_ready = ready && backed && unborn_head
        && this->network_summary_count == 0
        && (!this->network_summary_is_working_copy
            || this->network_summary_generation == QStringLiteral("0"))
        && personal.kind != PersonalRepositoryTarget::Kind::Unavailable
        && !this->head_network_import_active;
    this->revision_network_overview->import_network->setEnabled(import_ready);
    this->revision_network_overview->import_network->setToolTip(import_ready
        ? tr("Import an EPANET project into this empty unborn private HEAD")
        : tr("Import requires an empty unborn HEAD and an available personal DVMDB. "
             "Existing network contents are never overwritten."));
    const bool revision_export = this->selection_kind == SelectionKind::Revision
        && this->network_summary_count == 1
        && DvmdbIdentity::isUuidV7(this->network_summary_id);
    const bool draft_export = this->selection_kind == SelectionKind::Head
        && this->network_summary_is_working_copy
        && this->network_summary_count == 1
        && DvmdbIdentity::isUuidV7(this->network_summary_id)
        && DvmdbIdentity::isUuidV7(this->network_summary_draft_id)
        && !this->network_summary_generation.isEmpty();
    this->revision_network_overview->export_network->setEnabled(
        ready && backed && (revision_export || draft_export));
    this->revision_network_overview->export_network->setToolTip(
        this->selection_kind == SelectionKind::Head
        ? tr("Export the selected saved private working-copy generation, not the editor's current model")
        : tr("Export only the selected immutable revision's hydraulic network"));
    if (this->inspector_actions_stack != nullptr)
        this->inspector_actions_stack->setVisible(
            this->selection_kind != SelectionKind::None);
}

void ProjectsWidget::broadcastContext()
{
    QStringList project_names;
    for (const ProjectPreviewData &project : this->projects)
        for (const ProjectServerProfile &server : this->servers)
            if (server.visible && server.id == project.server_id)
                project_names.append(projectContextLabel(project, this->servers));
    QString current_name;
    QStringList branch_names;
    QString branch_name;
    if (const ProjectPreviewData *project = currentProject())
    {
        current_name = projectContextLabel(*project, this->servers);
        for (const ProjectPreviewBranch &branch : project->branches)
        {
            branch_names.append(branch.name);
            if (branch.id == this->branch_id
                && (this->selection_kind == SelectionKind::Branch
                    || this->selection_kind == SelectionKind::Head
                    || this->selection_kind == SelectionKind::Revision
                    || project->branches.size() == 1))
                branch_name = branch.name;
        }
    }
    emit projectContextChanged(project_names, current_name, branch_names, branch_name);
}

void ProjectsWidget::selectProjectByName(const QString &name)
{
    for (const ProjectPreviewData &project : this->projects)
        if (projectContextLabel(project, this->servers) == name)
        {
            for (const ProjectServerProfile &server : this->servers)
                if (server.id == project.server_id && server.visible)
                { select(SelectionKind::Project, project.id); return; }
        }
}

void ProjectsWidget::selectBranchByName(const QString &name)
{
    if (const ProjectPreviewData *project = currentProject())
        for (const ProjectPreviewBranch &branch : project->branches)
            if (branch.name == name) { select(SelectionKind::Branch, branch.id); return; }
}

void ProjectsWidget::selectToolbarProject(const QString &name)
{
    selectProjectByName(name);
    const ProjectPreviewData *project = currentProject();
    if (project == nullptr || projectContextLabel(*project, this->servers) != name)
        return;
    if (project->database_backed && project->branches.size() == 1)
        requestNetworkOpen(*project, project->branches.first());
    // For multiple branches the toolbar displays [none] until the user
    // selects a branch; do not silently choose main or raise a dialog.
}

void ProjectsWidget::selectToolbarBranch(const QString &name)
{
    if (name.isEmpty()) return;
    selectBranchByName(name);
    const ProjectPreviewData *project = currentProject();
    const ProjectPreviewBranch *branch = currentBranch();
    if (project != nullptr && branch != nullptr && project->database_backed
        && branch->name == name)
        requestNetworkOpen(*project, *branch);
}

void ProjectsWidget::requestNetworkOpen(const ProjectPreviewData &project,
                                        const ProjectPreviewBranch &branch)
{
    const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    if (target.kind == PersonalRepositoryTarget::Kind::Unavailable)
    {
        emit networkOpenProgress(tr("Select a personal DVMDB in the Server Inspector."), true);
        return;
    }
    QString working_name = tr("♛ Personal database");
    for (const ProjectServerProfile &server : this->servers)
        if (server.personal)
        {
            working_name = tr("♛ %1").arg(server.name);
            break;
        }
    emit networkOpenRequested(project.id, branch.id,
        projectContextLabel(project, this->servers), branch.name,
        working_name, project.repository_id != target.repository_id);
}

HistoryDockContext ProjectsWidget::historyContext(const QString &project_id,
                                                  const QString &branch_id) const
{
    HistoryDockContext context;
    const QString selected_project = project_id.isEmpty() ? this->project_id : project_id;
    const QString selected_branch = branch_id.isEmpty() ? this->branch_id : branch_id;
    const ProjectPreviewData *project = nullptr;
    for (const ProjectPreviewData &candidate : this->projects)
        if (candidate.id == selected_project) { project = &candidate; break; }
    if (project == nullptr) return context;
    context.project = projectContextLabel(*project, this->servers);

    const ProjectPreviewBranch *branch = nullptr;
    for (const ProjectPreviewBranch &candidate : project->branches)
        if (candidate.id == selected_branch) { branch = &candidate; break; }
    if (branch == nullptr && selected_branch.isEmpty() && project->branches.size() == 1)
        branch = &project->branches.first();
    if (branch == nullptr) return context;
    context.branch = branch->name;

    QHash<QString, const ProjectPreviewRevision *> revisions;
    for (const ProjectPreviewRevision &revision : project->revisions)
        revisions.insert(revision.id, &revision);
    QSet<QString> seen;
    QString revision_id = branch->head;
    while (!revision_id.isEmpty())
    {
        if (seen.contains(revision_id)) { context.incomplete_ancestry = true; break; }
        seen.insert(revision_id);
        const ProjectPreviewRevision *revision = revisions.value(revision_id, nullptr);
        if (revision == nullptr) { context.incomplete_ancestry = true; break; }
        HistoryDockEntry entry;
        entry.kind = HistoryDockEntry::Kind::Commit;
        entry.title = revision->message.trimmed().isEmpty()
            ? tr("Commit %1").arg(DvmdbIdentity::shortLabel(revision->id))
            : revision->message.simplified();
        entry.identity = revision->id;
        entry.details = tr("Immutable revision: %1").arg(revision->id);
        if (revision->parents.size() > 1)
            entry.details += tr("\nMerge commit: additional parent history is visible in Projects.");
        context.commits.prepend(entry);
        revision_id = revision->parents.isEmpty() ? QString() : revision->parents.first();
    }
    return context;
}

void ProjectsWidget::showRevisionHistory()
{
    if (this->selection_kind != SelectionKind::Revision)
    {
        if (ProjectPreviewBranch *branch = currentBranch())
        {
            if (!branch->head.isEmpty()) select(SelectionKind::Revision, branch->head);
        }
    }
    this->revision_tree->setFocus();
    // History is an explicit request to inspect a revision. Reopening works
    // even when the selected revision was already the current one.
    if (this->selection_kind == SelectionKind::Revision
        && this->inspector_dock != nullptr && this->inspector_dock->isHidden())
        this->inspector_dock->show();
}

void ProjectsWidget::createServer()
{
    if (this->source_writer->isBusy()) return;
    ++this->source_creation_request; // Retire responses from any previous add form.
    this->selection_kind = SelectionKind::NewSource;
    this->selected_id.clear();
    updateConnectionStatus();
    this->updating_ui = true;
    selectTreeItem(this->server_tree, QString());
    selectTreeItem(this->project_tree, QString());
    selectTreeItem(this->revision_tree, QString());
    this->database_connection->source_kind_combo->setCurrentIndex(0);
    this->database_connection->server_endpoint_edit->clear();
    this->database_connection->server_auth_combo->setCurrentIndex(0);
    this->updating_ui = false;
    updateInspector();
    this->database_connection->local_path_edit->setText(defaultLocalDvmdbPath());
    updateSourceFields();
    this->inspector_dock->show();
    this->database_overview->name->setFocus();
}

void ProjectsWidget::cancelNewSource()
{
    if (this->selection_kind != SelectionKind::NewSource) return;
    ++this->source_creation_request;
    this->selection_kind = SelectionKind::None;
    this->selected_id.clear();
    this->inspector_dock->hide();
    updateInspector();
    updateButtons();
}

void ProjectsWidget::saveNewSource()
{
    if (this->selection_kind != SelectionKind::NewSource) return;
    const QString name = this->database_overview->name->text().trimmed();
    if (name.isEmpty())
    {
        QMessageBox::warning(this, tr("Invalid name"), tr("Enter a database name."));
        this->database_overview->name->setFocus();
        return;
    }
    for (const ProjectServerProfile &server : this->servers)
        if (server.name.compare(name, Qt::CaseInsensitive) == 0)
        {
            QMessageBox::warning(this, tr("Already exists"), tr("Choose a different database name."));
            return;
        }
    ProjectServerProfile server;
    server.id = DvmdbIdentity::newUuidV7();
    server.pending = true;
    server.name = name;
    server.connection_type = this->database_connection->source_kind_combo->currentData().toString();
    server.description = this->database_overview->description->toPlainText();
    server.tags = parseTags(this->database_overview->tags->text());
    server.auth_mode = this->database_connection->server_auth_combo->currentData().toString();
    if (server.connection_type == QStringLiteral("local"))
    {
#ifndef AOWIS_STANDALONE_DVMDB_ENABLED
        QMessageBox::warning(this, tr("Embedded database unavailable"),
            tr("This application was built without embedded DVMDB support."));
        return;
#endif
        server.auth_mode = QStringLiteral("None");
        server.local_path = QDir::cleanPath(this->database_connection->local_path_edit->text().trimmed());
        if (!QDir::isAbsolutePath(server.local_path))
        {
            QMessageBox::warning(this, tr("Invalid directory"),
                tr("Enter an absolute path for the local DVMDB data directory."));
            this->database_connection->local_path_edit->setFocus();
            return;
        }
        for (const ProjectServerProfile &existing : this->servers)
            if (existing.connection_type == QStringLiteral("local")
                && QDir::cleanPath(existing.local_path) == server.local_path)
            {
                QMessageBox::warning(this, tr("Database already configured"),
                    tr("This local data directory is already configured."));
                return;
            }
    }
    else
    {
        server.endpoint = this->database_connection->server_endpoint_edit->text().trimmed();
        const QUrl url(server.endpoint);
        if (server.endpoint.isEmpty() || !url.isValid()
            || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
            || url.host().isEmpty() || !url.userInfo().isEmpty()
            || url.hasQuery() || url.hasFragment())
        {
            QMessageBox::warning(this, tr("Invalid database URL"),
                tr("Enter an http:// or https:// URL with a host, without credentials, query or fragment."));
            this->database_connection->server_endpoint_edit->setFocus();
            return;
        }
    }
    if (server.connection_type == QStringLiteral("local"))
    {
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        QString repository_id;
        QString error;
        const QString endpoint = embeddedDvmdbEndpoint(server.local_path, false,
                                                         &repository_id, &error);
        if (!endpoint.isEmpty() && DvmdbIdentity::isUuidV7(repository_id))
        {
            server.id = repository_id;
            server.pending = false;
        }
        else if (QMessageBox::question(this, tr("Embedded database unavailable"),
            tr("The local database could not be opened: %1\n\nSave this connection as pending?")
                .arg(error)) != QMessageBox::Yes) return;
#endif
        persistNewSource(server);
        return;
    }
    // The new profile is not persisted until the DVMDB repository identity is
    // checked. A network failure offers an explicit pending choice instead.
    const int request_id = ++this->source_creation_request;
    this->database_inspector->save_source->setEnabled(false);
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QString endpoint = server.endpoint;
    while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
    if (!endpoint.endsWith(QStringLiteral("/dvmdb/v1")))
        endpoint += QStringLiteral("/dvmdb/v1");
    QNetworkRequest request(QUrl(endpoint + QStringLiteral("/repository")));
    request.setTransferTimeout(4000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, server, request_id, manager, reply]() mutable
    {
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString error = reply->errorString();
        const bool transport_ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        manager->deleteLater();
        if (request_id != this->source_creation_request
            || this->selection_kind != SelectionKind::NewSource) return;
        this->database_inspector->save_source->setEnabled(true);
        if (transport_ok && status == 200)
        {
            const QJsonDocument document = QJsonDocument::fromJson(body);
            const QString id = document.isObject()
                ? document.object().value(QStringLiteral("repositoryId")).toString()
                : QString();
            const QString codec_error = document.isObject()
                ? ModelCodecCompatibility::error(document.object())
                : tr("Missing DVMDB identity response.");
            if (!codec_error.isEmpty())
            {
                QMessageBox::warning(this, tr("Incompatible MODEL codec"), codec_error);
                return;
            }
            if (!DvmdbIdentity::isUuidV7(id))
            {
                QMessageBox::warning(this, tr("Invalid DVMDB identity"),
                    tr("The endpoint did not return a valid repository UUIDv7."));
                return;
            }
            server.id = id;
            server.pending = false;
        }
        else if (status == 0)
        {
            if (QMessageBox::question(this, tr("Database unreachable"),
                tr("Could not reach the database at %1 (%2).\n\n"
                   "Save it as an unverified pending connection anyway?")
                    .arg(server.endpoint, error)) != QMessageBox::Yes) return;
        }
        else
        {
            QMessageBox::warning(this, tr("DVMDB verification failed"),
                tr("The endpoint responded with HTTP %1 (%2), but its repository identity "
                   "could not be verified. Check the URL or server configuration.")
                    .arg(status).arg(error));
            return;
        }
        persistNewSource(server);
    });
}

void ProjectsWidget::persistNewSource(ProjectServerProfile server)
{
    for (ProjectServerProfile &existing : this->servers)
    {
        if (existing.id == server.id && !server.pending)
        {
            if (existing.endpoint != server.endpoint && server.connection_type == QStringLiteral("remote"))
            {
                if (QMessageBox::question(this, tr("Database already configured"),
                    tr("This repository UUIDv7 is already configured as '%1'. "
                       "Update its endpoint to %2?").arg(existing.name, server.endpoint))
                    != QMessageBox::Yes) return;
                const QString previous_endpoint = existing.endpoint;
                existing.endpoint = server.endpoint;
                if (!saveProjectServerProfiles(this->servers))
                {
                    existing.endpoint = previous_endpoint;
                    reportServerSaveFailure();
                    return;
                }
                resetSource(existing.id);
                startSource(existing);
            }
            select(SelectionKind::Server, existing.id);
            return;
        }
        if (server.pending && existing.connection_type == server.connection_type
            && existing.endpoint == server.endpoint && existing.local_path == server.local_path)
        {
            QMessageBox::information(this, tr("Database already configured"),
                tr("This database endpoint is already configured."));
            select(SelectionKind::Server, existing.id);
            return;
        }
    }
    this->servers.append(server);
    if (!saveProjectServerProfiles(this->servers))
    {
        this->servers.removeLast();
        reportServerSaveFailure();
        return;
    }
    populateServerTree();
    updateEmptyState();
    select(SelectionKind::Server, server.id);
    startSource(server);
}

void ProjectsWidget::promoteVerifiedSource(const QString &old_id, const QString &repository_id)
{
    if (!DvmdbIdentity::isUuidV7(repository_id)) return;
    int index = -1;
    int duplicate = -1;
    for (int i = 0; i < this->servers.size(); ++i)
    {
        if (this->servers.at(i).id == old_id) index = i;
        if (this->servers.at(i).id == repository_id && repository_id != old_id) duplicate = i;
    }
    if (index < 0 || !this->servers.at(index).pending) return;
    const QList<ProjectServerProfile> previous = this->servers;
    const bool selected = this->server_id == old_id;
    if (duplicate >= 0)
        this->servers.removeAt(index);
    else
    {
        this->servers[index].id = repository_id;
        this->servers[index].pending = false;
    }
    if (!saveProjectServerProfiles(this->servers))
    {
        this->servers = previous;
        reportServerSaveFailure();
        return;
    }
    // Stop the old session (whose timer and reply callbacks capture old_id),
    // remove its transient catalogue, then start a clean session keyed by the
    // authoritative repository UUIDv7. This avoids stale hash keys and UI IDs.
    resetSource(old_id);
    if (selected)
    {
        this->server_id = repository_id;
        this->selection_kind = SelectionKind::Server;
        this->selected_id = repository_id;
    }
    if (duplicate < 0)
    {
        for (const ProjectServerProfile &profile : this->servers)
            if (profile.id == repository_id) { startSource(profile); break; }
    }
    populateServerTree();
    populateProjectTree();
    populateRevisionTree();
    rebuildGraph();
    updateInspector();
    updateButtons();
}

void ProjectsWidget::deleteConnection()
{
    if (this->source_writer->isBusy()) return;
    if (currentServer() == nullptr
        || (currentServer()->connection_type == QStringLiteral("local")
            && QDir::cleanPath(currentServer()->local_path)
                == QDir::cleanPath(defaultLocalDvmdbPath()))) return;
    if (QMessageBox::question(this, tr("Remove connection"),
        tr("Remove this database connection from the GUI configuration? "
           "The DVMDB repository and its stored projects and networks will not be deleted."))
        != QMessageBox::Yes) return;
    const QString removed = this->server_id;
    resetSource(removed);
    const QList<ProjectServerProfile> previous_servers = this->servers;
    for (int i = this->servers.size() - 1; i >= 0; --i)
        if (this->servers.at(i).id == removed) this->servers.removeAt(i);
    if (!saveProjectServerProfiles(this->servers))
    {
        this->servers = previous_servers;
        reportServerSaveFailure();
        return;
    }
    for (int i = this->projects.size() - 1; i >= 0; --i)
        if (this->projects.at(i).server_id == removed) this->projects.removeAt(i);
    this->server_id.clear();
    this->project_id.clear();
    this->branch_id.clear();
    populateServerTree();
    this->selection_kind = SelectionKind::None;
    this->selected_id.clear();
    updateEmptyState();
    if (!this->servers.isEmpty())
    {
        QString preferred = this->servers.first().id;
        for (const ProjectServerProfile &profile : this->servers)
            if (profile.personal) { preferred = profile.id; break; }
        select(SelectionKind::Server, preferred);
    }
    else
    {
        populateProjectTree();
        populateRevisionTree();
        rebuildGraph();
        updateInspector();
        this->inspector_dock->hide();
        updateButtons();
        broadcastContext();
    }
}

void ProjectsWidget::reportServerSaveFailure()
{
    QMessageBox::warning(this, tr("Cannot save database profiles"),
        tr("The database profile change could not be written to the GUI configuration:\n%1")
            .arg(guiConfigurationFilePath()));
}

void ProjectsWidget::finishSourceWrite(const QString &created_id, const QString &error,
                                       const QString &project_id, const QString &branch_name)
{
    updateConnectionStatus();
    updateButtons();
    if (!error.isEmpty())
    {
        QMessageBox::warning(this, tr("DVMDB write failed"), error);
        // Even on a timed-out request, the server may have committed the write.
        // Reload the authoritative catalogue rather than inventing local rows.
        connectSource();
        return;
    }
    cancelCreation();
    this->select_after_write_project = this->server_id + QStringLiteral("|")
        + (project_id.isEmpty() ? created_id : project_id);
    this->select_after_write_branch = branch_name;
    connectSource();
}

void ProjectsWidget::createProject()
{
    if (this->creation_kind != CreationKind::Project
        || this->source_writer->isBusy() || this->selection_kind == SelectionKind::NewSource) return;
    const bool preview = isPreviewSource();
    if (!preview && !isSourceReady(this->server_id)) return;
    const QString name = this->project_creation->name->text().trimmed();
    if (name.isEmpty()) return;
    for (const ProjectPreviewData &project : this->projects)
        if (project.server_id == this->server_id && project.name.compare(name, Qt::CaseInsensitive) == 0)
        {
            QMessageBox::information(this, tr("Already exists"), tr("Choose a different project name."));
            return;
        }
    if (!preview)
    {
        const ProjectServerProfile *server = currentServer();
        if (server == nullptr) return;
        this->source_writer->createProject(*server, name,
            [this](const QString &id, const QString &error)
            {
                finishSourceWrite(id, error, {}, {});
            });
        updateConnectionStatus();
        updateButtons();
        return;
    }
    ProjectPreviewData project;
    project.id = makeId(QStringLiteral("p-"));
    project.server_id = this->server_id;
    project.name = name;
    const QString id = project.id;
    this->projects.append(project);
    populateProjectTree();
    cancelCreation();
    select(SelectionKind::Project, id);
}

// HEAD is NOT the committed branch tip. It is the saved private working-copy
// candidate, which may be unborn or contain edits not yet committed. Revisions
// always resolve to their exact immutable snapshot. Never show committed-base
// counts as though they were the user's saved writable network.
void ProjectsWidget::refreshNetworkSummary()
{
    const int epoch = ++this->network_read_epoch;
    this->network_summary_count = -1;
    this->network_summary_id.clear();
    this->network_summary_draft_id.clear();
    this->network_summary_generation.clear();
    this->network_summary_is_working_copy = false;
    this->network_summary_revision.clear();
    this->network_summary_project.clear();
    this->network_summary_branch.clear();
    if (this->selection_kind != SelectionKind::Head
        && this->selection_kind != SelectionKind::Revision) return;
    const ProjectPreviewData *project = currentProject();
    const ProjectPreviewBranch *branch = inspectedRevisionBranch();
    this->revision_network_overview->branch->setText(
        tr("Branch: %1").arg(branch == nullptr ? tr("Unknown") : branch->name));
    this->revision_network_overview->state->setText(
        this->selection_kind == SelectionKind::Head ? tr("Writable HEAD")
                                                  : tr("Immutable revision"));
    this->revision_network_overview->clearCounts();
    this->revision_network_overview->network_status->setText(tr("Checking network…"));
    if (project == nullptr || branch == nullptr || !project->database_backed) {
        this->revision_network_overview->network_status->setText(
            tr("Network contents are not persisted for this preview."));
        return;
    }
    const QString project_id = project->backend_id;
    const QString branch_name = branch->name;
    if (this->selection_kind == SelectionKind::Revision) {
        refreshCommittedNetworkSummary(epoch, project_id, branch_name, this->selected_id);
        return;
    }
    const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    if (target.kind == PersonalRepositoryTarget::Kind::Unavailable) {
        this->revision_network_overview->network_status->setText(
            tr("No personal database selected. Only the committed base can be summarized."));
        refreshCommittedNetworkSummary(epoch, project_id, branch_name, {});
        return;
    }
    const QString branch_uuid = branch->id.mid(project->id.size() + 1);
    if (!DvmdbIdentity::isUuidV7(project_id)
        || !DvmdbIdentity::isUuidV7(branch_uuid)) {
        this->revision_network_overview->network_status->setText(
            tr("Cannot identify the selected HEAD in the personal database."));
        return;
    }
    PersonalWorkingCopyClient *client = new PersonalWorkingCopyClient(this);
    client->list(target, project_id, [this, client, target, epoch, project_id, branch_name, branch_uuid]
        (const QList<PersonalWorkingCopyInfo> &drafts, const QString &error) {
        if (this->network_read_epoch != epoch) { client->deleteLater(); return; }
        if (!error.isEmpty()) {
            this->revision_network_overview->network_status->setText(
                tr("Unable to read saved personal working copies: %1").arg(error));
            client->deleteLater(); return;
        }
        QString draft_id;
        int matches = 0;
        for (const PersonalWorkingCopyInfo &draft : drafts) {
            if (draft.project_id != project_id || draft.branch_id != branch_uuid) continue;
            ++matches;
            draft_id = draft.id;
        }
        if (matches > 1) {
            this->revision_network_overview->network_status->setText(
                tr("Multiple private working copies exist for this HEAD. "
                   "Open Personal working copies and choose one; their contents may differ."));
            client->deleteLater(); return;
        }
        if (matches == 0) {
            client->deleteLater();
            const ProjectPreviewBranch *selected = inspectedRevisionBranch();
            if (selected != nullptr && selected->head.isEmpty()) {
                // Unborn HEAD, no private draft and therefore no candidate
                // records: a new draft can safely be provisioned for import.
                this->network_summary_count = 0;
                this->revision_network_overview->showCounts(QJsonObject{}, 0);
                this->revision_network_overview->network_status->setText(
                    tr("No hydraulic network in this unborn HEAD."));
                updateButtons();
                return;
            }
            // A committed branch without a private draft displays only its
            // pinned base; its counts are not permission to overwrite it.
            refreshCommittedNetworkSummary(epoch, project_id, branch_name, {});
            return;
        }
        client->summarizeDraftNetworks(target, draft_id,
            [this, client, epoch](const QJsonObject &summary, const QString &summary_error) {
            client->deleteLater();
            if (this->network_read_epoch != epoch) return;
            if (!summary_error.isEmpty()) {
                this->revision_network_overview->network_status->setText(
                    tr("Saved working-copy summary unavailable: %1").arg(summary_error));
                updateButtons();
                return;
            }
            displayNetworkSummary(summary, true);
        });
    });
}

void ProjectsWidget::refreshCommittedNetworkSummary(int epoch, const QString &project_id,
                                                     const QString &branch_name,
                                                     const QString &revision_id)
{
    if (this->network_read_epoch != epoch) return;
    const ProjectServerProfile *server = currentServer();
    QString endpoint = server != nullptr && server->connection_type == QStringLiteral("remote")
        ? server->endpoint.trimmed() : QString();
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    if (server != nullptr && server->connection_type == QStringLiteral("local"))
        endpoint = embeddedDvmdbEndpoint(server->local_path, server->personal,
                                         nullptr, nullptr);
#endif
    if (endpoint.isEmpty()) {
        this->revision_network_overview->network_status->setText(
            tr("A connected DVMDB HTTP endpoint is needed for network statistics."));
        return;
    }
    while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
    if (!endpoint.endsWith(QStringLiteral("/dvmdb/v1")))
        endpoint += QStringLiteral("/dvmdb/v1");
    QUrl url(endpoint + QStringLiteral("/projects/") + project_id
        + QStringLiteral("/branches/")
        + QString::fromLatin1(QUrl::toPercentEncoding(branch_name))
        + QStringLiteral("/networks/summary"));
    if (!revision_id.isEmpty()) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("revision"), revision_id);
        url.setQuery(query);
    }
    QNetworkReply *reply = this->network_read_manager->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, epoch]() {
        const QByteArray bytes = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError
            ? QString() : reply->errorString();
        reply->deleteLater();
        if (this->network_read_epoch != epoch) return;
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        if (!error.isEmpty() || !document.isObject()) {
            this->revision_network_overview->network_status->setText(
                tr("Committed network summary unavailable: %1").arg(
                    error.isEmpty() ? tr("Invalid response") : error));
            updateButtons();
            return;
        }
        displayNetworkSummary(document.object(), false);
    });
}

void ProjectsWidget::displayNetworkSummary(const QJsonObject &result, bool working_copy)
{
    const QString scope = working_copy ? QStringLiteral("working-copy")
                                       : QStringLiteral("committed");
    if (result.value(QStringLiteral("scope")).toString() != scope
        || !result.value(QStringLiteral("networks")).isArray()) {
        this->revision_network_overview->network_status->setText(
            tr("Unexpected network summary response."));
        updateButtons();
        return;
    }
    const QJsonArray networks = result.value(QStringLiteral("networks")).toArray();
    if (result.value(QStringLiteral("networkCount")).toInt(-1) != networks.size()) {
        this->revision_network_overview->network_status->setText(
            tr("Network count does not match the saved summary."));
        updateButtons();
        return;
    }
    this->network_summary_count = networks.size();
    this->network_summary_is_working_copy = working_copy;
    if (working_copy) {
        this->network_summary_draft_id = result.value(QStringLiteral("draftId")).toString();
        this->network_summary_generation = result.value(QStringLiteral("generation")).toString();
        if (!DvmdbIdentity::isUuidV7(this->network_summary_draft_id)
            || this->network_summary_generation.isEmpty()) {
            this->network_summary_count = -1;
            this->revision_network_overview->network_status->setText(
                tr("Invalid saved draft identity or generation."));
            updateButtons();
            return;
        }
    }
    this->revision_network_overview->clearCounts();
    const QStringList keys{QStringLiteral("junctions"), QStringLiteral("pipes"),
        QStringLiteral("reservoirs"), QStringLiteral("tanks"),
        QStringLiteral("pumps"), QStringLiteral("valves")};
    QHash<QString, int> totals;
    int all_entities = 0;
    for (const QJsonValue &entry : networks) {
        const QJsonObject network = entry.toObject();
        if (!entry.isObject() || !network.value(QStringLiteral("counts")).isObject()
            || !DvmdbIdentity::isUuidV7(network.value(QStringLiteral("id")).toString())) {
            this->network_summary_count = -1;
            this->revision_network_overview->network_status->setText(
                tr("Invalid hydraulic network in summary response."));
            updateButtons();
            return;
        }
        const QJsonObject counts = network.value(QStringLiteral("counts")).toObject();
        all_entities += network.value(QStringLiteral("entityCount")).toInt();
        for (const QString &key : keys)
            totals.insert(key, totals.value(key) + counts.value(key).toInt());
    }
    QJsonObject sum;
    for (const QString &key : keys) sum.insert(key, totals.value(key));
    this->revision_network_overview->showCounts(sum, all_entities);
    if (networks.isEmpty()) {
        this->revision_network_overview->network_status->setText(working_copy
            ? tr("No hydraulic network in this saved working copy.")
            : (this->selection_kind == SelectionKind::Head
                ? tr("No committed hydraulic network. Private draft contents may differ.")
                : tr("No hydraulic network in this committed revision.")));
    } else {
        if (networks.size() == 1)
            this->network_summary_id = networks.first().toObject()
                .value(QStringLiteral("id")).toString();
        const QString heading = networks.size() == 1
            ? tr("Network: %1").arg(networks.first().toObject()
                .value(QStringLiteral("title")).toString().trimmed())
            : tr("%1 hydraulic networks").arg(networks.size());
        this->revision_network_overview->network_status->setText(heading + (working_copy
            ? tr("\nSaved private working copy · generation %1")
                  .arg(result.value(QStringLiteral("generation")).toString())
            : (this->selection_kind == SelectionKind::Head
                ? tr("\nCommitted base only; no private draft was found.") : QString())));
    }
    if (!working_copy) {
        const ProjectPreviewData *project = currentProject();
        const ProjectPreviewBranch *branch = inspectedRevisionBranch();
        if (project != nullptr && branch != nullptr) {
            this->network_summary_project = project->backend_id;
            this->network_summary_branch = branch->name;
            this->network_summary_revision = result.value(QStringLiteral("revision")).toString();
        }
    }
    updateButtons();
}

// A read of an immutable revision always pins its exact UUID. This avoids
// exporting or previewing a different revision after a concurrent HEAD move.
void ProjectsWidget::fetchInspectedNetwork(bool export_requested)
{
    if (this->selection_kind != SelectionKind::Revision
        || this->network_summary_count != 1
        || !DvmdbIdentity::isUuidV7(this->network_summary_id)
        || !DvmdbIdentity::isUuidV7(this->network_summary_revision)) return;
    const ProjectServerProfile *server = currentServer();
    QString endpoint = server != nullptr && server->connection_type == QStringLiteral("remote")
        ? server->endpoint.trimmed() : QString();
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    if (server != nullptr && server->connection_type == QStringLiteral("local"))
        endpoint = embeddedDvmdbEndpoint(server->local_path, server->personal,
                                         nullptr, nullptr);
#endif
    if (endpoint.isEmpty()) return;
    while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
    if (!endpoint.endsWith(QStringLiteral("/dvmdb/v1")))
        endpoint += QStringLiteral("/dvmdb/v1");
    QUrl url(endpoint + QStringLiteral("/projects/") + this->network_summary_project
        + QStringLiteral("/branches/")
        + QString::fromLatin1(QUrl::toPercentEncoding(this->network_summary_branch))
        + QStringLiteral("/networks/") + this->network_summary_id);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("revision"), this->network_summary_revision);
    url.setQuery(query);
    const QString expected_id = this->network_summary_id;
    const QString revision_id = this->network_summary_revision;
    const int epoch = this->network_read_epoch;
    QNetworkReply *reply = this->network_read_manager->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, epoch, expected_id, revision_id, export_requested]() {
        const QByteArray bytes = reply->readAll();
        const QString transport_error = reply->error() == QNetworkReply::NoError
            ? QString() : reply->errorString();
        reply->deleteLater();
        if (this->network_read_epoch != epoch
            || this->selection_kind != SelectionKind::Revision) return;
        const QJsonDocument result = QJsonDocument::fromJson(bytes);
        if (!transport_error.isEmpty() || !result.isObject()) {
            QMessageBox::warning(this, tr("Network read failed"),
                transport_error.isEmpty() ? tr("Invalid DVMDB response") : transport_error);
            return;
        }
        const QJsonObject object = result.object();
        if (object.value(QStringLiteral("uuid")).toString() != expected_id
            || object.value(QStringLiteral("revision")).toString() != revision_id) {
            QMessageBox::warning(this, tr("Network identity mismatch"),
                tr("The DVMDB reply did not match the selected revision and network."));
            return;
        }
        const QByteArray encoded = object.value(QStringLiteral("payloadBase64"))
            .toString().toLatin1();
        const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
            encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.toBase64() != encoded) {
            QMessageBox::warning(this, tr("Network validation failed"),
                tr("The DVMDB response contains invalid canonical base64."));
            return;
        }
        const QByteArray data = decoded.decoded;
        QString codec_error;
        const std::optional<NetworkHydraulic> network =
            aowis::model::ModelCodec<NetworkHydraulic>::decode(data, &codec_error);
        if (!network || network->uuid.toString(QUuid::WithoutBraces) != expected_id
            || aowis::model::ModelCodec<NetworkHydraulic>::encode(*network) != data) {
            QMessageBox::warning(this, tr("Network validation failed"),
                tr("The selected revision network could not be decoded: %1")
                    .arg(codec_error));
            return;
        }
        if (export_requested)
            emit selectedNetworkExportRequested(*network);
        else
            emit immutableNetworkPreviewRequested(*network, revision_id);
    });
}

void ProjectsWidget::beginInspectedNetworkImport()
{
    if (this->head_network_import_active || this->selection_kind != SelectionKind::Head
        || !this->revision_network_overview->import_network->isEnabled()) return;
    this->pending_head_import_epoch = this->network_read_epoch;
    emit selectedHeadImportFileRequested();
}

void ProjectsWidget::importNetworkIntoSelectedHead(NetworkHydraulic network)
{
    const int epoch = this->pending_head_import_epoch;
    this->pending_head_import_epoch = -1;
    if (epoch < 0 || epoch != this->network_read_epoch
        || this->head_network_import_active || this->selection_kind != SelectionKind::Head
        || this->network_summary_count != 0) return;
    const ProjectPreviewData *project = currentProject();
    const ProjectPreviewBranch *branch = inspectedRevisionBranch();
    const ProjectServerProfile *source = currentServer();
    if (project == nullptr || branch == nullptr || source == nullptr
        || !project->database_backed || !branch->head.isEmpty()) return;
    const QString branch_uuid = branch->id.mid(project->id.size() + 1);
    const QString project_uuid = project->backend_id;
    if (!DvmdbIdentity::isUuidV7(project_uuid)
        || !DvmdbIdentity::isUuidV7(branch_uuid)) return;
    const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    if (target.kind == PersonalRepositoryTarget::Kind::Unavailable) return;
    if (!DvmdbIdentity::isUuidV7(network.uuid.toString(QUuid::WithoutBraces)))
        network.uuid = QUuid(DvmdbIdentity::newUuidV7());
    if (network.id.trimmed().isEmpty()) network.id = project->name;
    const QString source_repository = project->repository_id;
    const QString project_name = project->name;
    QString source_endpoint = source->connection_type == QStringLiteral("remote")
        ? source->endpoint : QString();
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    if (source->connection_type == QStringLiteral("local"))
        source_endpoint = embeddedDvmdbEndpoint(source->local_path, source->personal,
                                                nullptr, nullptr);
#endif
    this->head_network_import_active = true;
    updateButtons();
    this->revision_network_overview->network_status->setText(
        tr("Importing hydraulic network into saved private HEAD…"));
    PersonalWorkingCopyClient *client = new PersonalWorkingCopyClient(this);
    const std::shared_ptr<NetworkHydraulic> imported =
        std::make_shared<NetworkHydraulic>(std::move(network));
    const std::function<void(const QString &)> failed =
        [this, client, epoch](const QString &error) {
            client->deleteLater();
            this->head_network_import_active = false;
            if (this->network_read_epoch == epoch
                && this->selection_kind == SelectionKind::Head)
                this->revision_network_overview->network_status->setText(
                    tr("Network import was not confirmed: %1").arg(error));
            updateButtons();
        };
    const std::function<void(const PersonalWorkingCopyInfo &, const QString &)> use_draft =
        [this, client, target, epoch, project_uuid, branch_uuid, imported, failed]
        (const PersonalWorkingCopyInfo &draft, const QString &error) {
            if (!error.isEmpty()) { failed(error); return; }
            if (this->network_read_epoch != epoch || this->selection_kind != SelectionKind::Head) {
                failed(tr("HEAD selection changed during import.")); return;
            }
            if (draft.project_id != project_uuid || draft.branch_id != branch_uuid
                || !draft.base_revision.isEmpty() || !draft.changes.isEmpty()
                || (!draft.event_count.isEmpty() && draft.event_count != QStringLiteral("0"))) {
                failed(tr("Private draft is not an untouched unborn HEAD. Existing content was preserved."));
                return;
            }
            client->acquireLease(target, draft.id, draft.session_id, 90,
                [this, client, target, epoch, draft, imported, failed]
                (const PersonalWorkingCopyLease &lease, const QString &lease_error) {
                    if (!lease_error.isEmpty()) { failed(lease_error); return; }
                    if (lease.generation != draft.generation
                        || this->network_read_epoch != epoch
                        || this->selection_kind != SelectionKind::Head) {
                        client->releaseLease(target, draft.id, draft.session_id,
                            lease.token, [failed](const QString &) {
                                failed(QObject::tr("Draft changed before import; no data was overwritten."));
                            });
                        return;
                    }
                    client->importNetworkIntoEmptyDraft(target, draft.id, draft.session_id,
                        lease.token, lease.generation, *imported,
                        [this, client, target, epoch, draft, lease, failed]
                        (const PersonalWorkingCopyInfo &saved, const QString &import_error) {
                            client->releaseLease(target, draft.id, draft.session_id,
                                lease.token,
                                [this, client, epoch, saved, import_error, failed]
                                (const QString &release_error) {
                                    if (!import_error.isEmpty()) { failed(import_error); return; }
                                    client->deleteLater();
                                    this->head_network_import_active = false;
                                    if (this->network_read_epoch == epoch
                                        && this->selection_kind == SelectionKind::Head) {
                                        refreshNetworkSummary();
                                        if (!release_error.isEmpty())
                                            this->revision_network_overview->network_status->setText(
                                                tr("Network saved to draft %1, but lease release is unconfirmed: %2")
                                                    .arg(saved.id, release_error));
                                    }
                                    updateButtons();
                                });
                        });
                });
        };
    client->list(target, project_uuid,
        [this, client, target, epoch, project_uuid, branch_uuid, source_repository,
         source_endpoint, project_name, use_draft, failed]
        (const QList<PersonalWorkingCopyInfo> &drafts, const QString &list_error) {
            if (!list_error.isEmpty()) { failed(list_error); return; }
            if (this->network_read_epoch != epoch || this->selection_kind != SelectionKind::Head) {
                failed(tr("HEAD selection changed during import.")); return;
            }
            QString matched;
            for (const PersonalWorkingCopyInfo &draft : drafts) {
                if (draft.project_id != project_uuid || draft.branch_id != branch_uuid) continue;
                if (!matched.isEmpty()) {
                    failed(tr("Multiple private drafts exist for this HEAD. Choose one in Personal working copies."));
                    return;
                }
                matched = draft.id;
            }
            if (!matched.isEmpty()) {
                client->reopen(target, matched, use_draft);
                return;
            }
            const std::function<void(const QString &)> create_draft =
                [client, target, project_uuid, branch_uuid, use_draft, failed]
                (const QString &error) {
                    if (!error.isEmpty()) { failed(error); return; }
                    client->create(target, project_uuid, branch_uuid, {}, use_draft);
                };
            if (source_repository == target.repository_id) {
                create_draft({});
            } else if (!source_endpoint.isEmpty()) {
                client->checkoutBranchPrivately(target, source_endpoint, source_repository,
                    project_uuid, project_name, branch_uuid, {}, create_draft);
            } else {
                failed(tr("Source branch must be checked out into the personal DVMDB first."));
            }
        });
}

void ProjectsWidget::exportInspectedNetwork()
{
    if (this->selection_kind == SelectionKind::Revision) {
        fetchInspectedNetwork(true);
        return;
    }
    if (this->selection_kind != SelectionKind::Head
        || !this->network_summary_is_working_copy
        || this->network_summary_count != 1
        || !DvmdbIdentity::isUuidV7(this->network_summary_draft_id)
        || !DvmdbIdentity::isUuidV7(this->network_summary_id)) return;
    const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    if (target.kind == PersonalRepositoryTarget::Kind::Unavailable) return;
    const QString draft_id = this->network_summary_draft_id;
    const QString network_id = this->network_summary_id;
    const QString generation = this->network_summary_generation;
    const int epoch = this->network_read_epoch;
    PersonalWorkingCopyClient *client = new PersonalWorkingCopyClient(this);
    client->loadDraftNetwork(target, draft_id, network_id,
        [this, client, epoch, generation](const NetworkHydraulic &network,
                                          const QString &saved_generation,
                                          const QString &error) {
        client->deleteLater();
        if (epoch != this->network_read_epoch || this->selection_kind != SelectionKind::Head)
            return;
        if (!error.isEmpty() || saved_generation != generation) {
            this->revision_network_overview->network_status->setText(error.isEmpty()
                ? tr("HEAD changed since its network summary. Refresh before exporting.")
                : tr("Saved HEAD export failed: %1").arg(error));
            this->network_summary_count = -1;
            updateButtons();
            return;
        }
        emit selectedNetworkExportRequested(network);
    });
}

void ProjectsWidget::createEditingBranchFromRevision(const QString &name)
{
    if (this->selection_kind != SelectionKind::Revision || this->source_writer->isBusy()) return;
    const ProjectPreviewData *project = currentProject();
    const ProjectPreviewBranch *branch = inspectedRevisionBranch();
    const ProjectServerProfile *server = currentServer();
    const QString requested_name = name.trimmed();
    if (project == nullptr || branch == nullptr || server == nullptr
        || !project->database_backed || requested_name.isEmpty()) return;
    for (const ProjectPreviewBranch &candidate : project->branches)
        if (candidate.name.compare(requested_name, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, tr("Branch name already exists"),
                tr("Choose a different name for the new branch."));
            return;
        }
    const QString source_uuid = branch->id.mid(project->id.size() + 1);
    const QString project_id = project->backend_id;
    const QString revision_id = this->selected_id;
    if (!DvmdbIdentity::isUuidV7(source_uuid)
        || !DvmdbIdentity::isUuidV7(revision_id)) return;
    this->source_writer->createBranch(*server, project_id, requested_name, source_uuid,
        revision_id, [this, project_id, requested_name](const QString &id, const QString &error) {
            this->edit_after_branch_creation = error.isEmpty();
            finishSourceWrite(id, error, project_id,
                id.isEmpty() ? requested_name : id);
        });
    updateButtons();
}

void ProjectsWidget::openInspectedNetwork()
{
    if (this->selection_kind == SelectionKind::Head) {
        editProjectNetwork();
        return;
    }
    if (this->selection_kind != SelectionKind::Revision) return;
    // Nonblocking modal dialog: QDialog::exec() is not supported reliably by
    // the WebAssembly event loop.
    QDialog *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Open immutable revision"));
    dialog->setObjectName(QStringLiteral("projectsRevisionOpenDialog"));
    dialog->setMinimumWidth(420);
    QVBoxLayout *column = new QVBoxLayout(dialog);
    column->setContentsMargins(16, 16, 16, 16);
    column->setSpacing(12);
    column->setSizeConstraint(QLayout::SetMinimumSize);
    QLabel *context = new QLabel(tr("Revision %1\nBranch: %2")
        .arg(this->selected_id, this->network_summary_branch), dialog);
    context->setObjectName(QStringLiteral("projectsRevisionOpenContext"));
    context->setWordWrap(true);
    context->setTextInteractionFlags(Qt::TextSelectableByMouse);
    column->addWidget(context);
    QRadioButton *read_only = new QRadioButton(tr("Open read-only in Distribution Monitor"), dialog);
    QRadioButton *new_branch = new QRadioButton(tr("Create branch and open for full editing"), dialog);
    column->addWidget(read_only);
    column->addWidget(new_branch);
    read_only->setEnabled(this->network_summary_count == 1);
    if (read_only->isEnabled()) read_only->setChecked(true);
    else new_branch->setChecked(true);
    QLineEdit *branch_name = new QLineEdit(dialog);
    branch_name->setObjectName(QStringLiteral("projectsRevisionOpenBranchName"));
    branch_name->setPlaceholderText(tr("New branch name"));
    branch_name->setEnabled(new_branch->isChecked());
    column->addWidget(branch_name);
    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->setObjectName(QStringLiteral("projectsRevisionOpenButtons"));
    column->addWidget(buttons);
    QPushButton *confirm = buttons->button(QDialogButtonBox::Ok);
    confirm->setText(tr("Open"));
    connect(new_branch, &QRadioButton::toggled, branch_name, &QLineEdit::setEnabled);
    const std::function<void()> validate = [new_branch, branch_name, confirm] {
        confirm->setEnabled(!new_branch->isChecked() || !branch_name->text().trimmed().isEmpty());
    };
    connect(new_branch, &QRadioButton::toggled, dialog,
            [validate](bool) { validate(); });
    connect(branch_name, &QLineEdit::textChanged, dialog,
            [validate](const QString &) { validate(); });
    validate();
    const QString selected_project = this->project_id;
    const QString selected_revision = this->selected_id;
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, read_only, branch_name, selected_project, selected_revision] {
        if (this->project_id != selected_project
            || this->selected_id != selected_revision
            || this->selection_kind != SelectionKind::Revision) {
            dialog->reject();
            return;
        }
        if (read_only->isChecked())
            fetchInspectedNetwork(false);
        else
            createEditingBranchFromRevision(branch_name->text());
        dialog->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->open();
}

void ProjectsWidget::showWorkingCopies()
{
    openWorkingCopy(false);
}

void ProjectsWidget::editProjectNetwork()
{
    openWorkingCopy(true);
}

void ProjectsWidget::openWorkingCopy(bool direct_edit)
{
    const ProjectPreviewData *project = currentProject();
    if (project == nullptr || !project->database_backed) return;
    const ProjectPreviewBranch *branch = currentBranch();
    if (direct_edit)
    {
        // Inspector and toolbar share exactly the same confirmed open path.
        // A project with several branches never implicitly selects main.
        if (this->selection_kind == SelectionKind::Project)
            branch = project->branches.size() == 1 ? &project->branches.first() : nullptr;
        if (branch != nullptr)
            requestNetworkOpen(*project, *branch);
        else
            emit networkOpenProgress(tr("Select a branch in the toolbar to open its network."), false);
        return;
    }
    if ((this->selection_kind != SelectionKind::Branch
        && this->selection_kind != SelectionKind::Head) || branch == nullptr)
        return;
    const ProjectServerProfile *source = nullptr;
    for (const ProjectServerProfile &candidate : this->servers)
        if (candidate.id == project->server_id) { source = &candidate; break; }
    QString source_endpoint = source != nullptr &&
        source->connection_type == QStringLiteral("remote") ? source->endpoint : QString();
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    if (source != nullptr && source->connection_type == QStringLiteral("local"))
        source_endpoint = embeddedDvmdbEndpoint(source->local_path, source->personal,
                                                nullptr, nullptr);
#endif
    PersonalWorkingCopyDialog dialog(project->backend_id,
        branch->id.mid(project->id.size() + 1), branch->name, branch->head,
        project->repository_id, source_endpoint, project->name, this, false);
    if (dialog.exec() == QDialog::Accepted && dialog.selectedPreviewNetwork().has_value())
    {
        if (dialog.selectedEditRequested())
            emit draftNetworkEditRequested(dialog.selectedPreviewDraftId(),
                                           dialog.selectedEditNetworkId());
        else
            emit draftNetworkPreviewRequested(*dialog.selectedPreviewNetwork(),
                dialog.selectedPreviewDraftId(), dialog.selectedPreviewGeneration());
    }
}

void ProjectsWidget::prepareNetworkOpen(const QString &project_id,
                                        const QString &branch_id)
{
    const ProjectPreviewData *project = nullptr;
    const ProjectPreviewBranch *branch = nullptr;
    for (const ProjectPreviewData &candidate : this->projects)
        if (candidate.id == project_id && candidate.database_backed)
        {
            project = &candidate;
            for (const ProjectPreviewBranch &candidate_branch : candidate.branches)
                if (candidate_branch.id == branch_id)
                {
                    branch = &candidate_branch;
                    break;
                }
            break;
        }
    if (project == nullptr || branch == nullptr)
    {
        emit networkOpenProgress(tr("Project or branch changed. Select it again."), true);
        return;
    }
    const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    const QString branch_uuid = branch->id.mid(project->id.size() + 1);
    if (target.kind == PersonalRepositoryTarget::Kind::Unavailable
        || !DvmdbIdentity::isUuidV7(project->backend_id)
        || !DvmdbIdentity::isUuidV7(branch_uuid)
        || (!branch->head.isEmpty() && !DvmdbIdentity::isUuidV7(branch->head)))
    {
        emit networkOpenProgress(tr("Cannot open: personal DVMDB or canonical UUIDv7 identity is unavailable."), true);
        return;
    }
    const ProjectServerProfile *source = nullptr;
    for (const ProjectServerProfile &candidate : this->servers)
        if (candidate.id == project->server_id) { source = &candidate; break; }
    QString source_endpoint = source != nullptr &&
        source->connection_type == QStringLiteral("remote") ? source->endpoint : QString();
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    if (source != nullptr && source->connection_type == QStringLiteral("local"))
        source_endpoint = embeddedDvmdbEndpoint(source->local_path, source->personal,
                                                nullptr, nullptr);
#endif
    // The previous intermediary dialog performed real work. Keep its verified
    // checkout, draft, and network-loading logic, but run it invisibly and
    // asynchronously AFTER the visible Open Network confirmation.
    PersonalWorkingCopyDialog *operation = new PersonalWorkingCopyDialog(
        project->backend_id, branch_uuid, branch->name, branch->head,
        project->repository_id, source_endpoint, project->name, this, true,
        [this](const QString &status, bool failed)
        {
            emit networkOpenProgress(status, failed);
        });
    connect(operation, &QDialog::finished, this, [this, operation](int result)
    {
        if (result == QDialog::Accepted && operation->selectedEditRequested()
            && operation->selectedPreviewNetwork().has_value())
        {
            // A personal-server designation may have changed during the
            // asynchronous checkout. Never open a draft against another DB.
            if (operation->targetRepositoryId()
                != resolvePersonalRepositoryTarget().repository_id)
                emit networkOpenProgress(tr("Personal database changed during opening. Try again."), true);
            else
                emit draftNetworkEditRequested(operation->selectedPreviewDraftId(),
                                               operation->selectedEditNetworkId());
        }
        else if (result == QDialog::Accepted)
            emit networkOpenProgress(tr("Prepared network was not approved for editing."), true);
        operation->deleteLater();
    });
}

void ProjectsWidget::createBranch()
{
    if (this->creation_kind != CreationKind::Branch) return;
    ProjectPreviewData *project = currentProject();
    if (this->source_writer->isBusy() || project == nullptr) return;
    const bool preview = isPreviewSource();
    if (!preview && (!project->database_backed || !isSourceReady(this->server_id))) return;
    const QString name = this->branch_creation->name->text().trimmed();
    if (name.isEmpty()) return;
    for (const ProjectPreviewBranch &branch : project->branches)
        if (branch.name.compare(name, Qt::CaseInsensitive) == 0)
        {
            QMessageBox::information(this, tr("Already exists"), tr("Choose a different branch name."));
            return;
        }
    if (!preview && this->selection_kind == SelectionKind::Project)
    {
        const ProjectServerProfile *server = currentServer();
        if (server == nullptr) return;
        QStringList tags;
        for (const QString &part : this->branch_creation->tags->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString tag = part.trimmed();
            if (!tag.isEmpty() && !tags.contains(tag)) tags.append(tag);
        }
        const QString project_id = project->backend_id;
        this->source_writer->createUnbornBranch(*server, project_id, name, tags,
            this->branch_creation->description->toPlainText().trimmed(),
            [this, project_id, name](const QString &id, const QString &error)
            {
                finishSourceWrite(id, error, project_id, id.isEmpty() ? name : id);
            });
        updateConnectionStatus();
        updateButtons();
        return;
    }
    if (!preview)
    {
        const ProjectServerProfile *server = currentServer();
        if (server == nullptr) return;
        // The source is identified by its branch UUIDv7 (DG-01): a name may
        // be renamed or reused while this request is on its way.
        const ProjectPreviewBranch *source_branch = nullptr;
        QString from_revision;
        const QString origin = this->branch_creation->source->currentData().toString();
        if (origin.startsWith(QStringLiteral("branch:")))
        {
            for (const ProjectPreviewBranch &candidate : project->branches)
                if (candidate.id == origin.mid(7))
                {
                    source_branch = &candidate;
                    // Pin the HEAD shown in the inspector. Sending only the
                    // branch would allow a concurrent HEAD advance to
                    // silently change the selected immutable fork point.
                    from_revision = candidate.head;
                    break;
                }
        }
        else if (origin.startsWith(QStringLiteral("revision:")))
        {
            from_revision = origin.mid(9);
            const QString source_name = sourceBranchForRevision(*project, from_revision, QString());
            for (const ProjectPreviewBranch &candidate : project->branches)
                if (!source_name.isEmpty() && candidate.name == source_name)
                {
                    source_branch = &candidate;
                    break;
                }
            if (source_branch == nullptr)
            {
                this->branch_creation->error->setText(tr("The selected revision is not reachable from any branch."));
                updateButtons();
                return;
            }
        }
        if (source_branch == nullptr) return;
        if (source_branch->head.isEmpty())
        {
            this->branch_creation->error->setText(
                tr("Commit the source branch before creating another database branch."));
            updateButtons();
            return;
        }
        // Database branch IDs are "<GUI project id>:<branch UUIDv7>".
        const QString from_branch_id = source_branch->id.mid(project->id.size() + 1);
        if (!DvmdbIdentity::isUuidV7(from_branch_id))
        {
            this->branch_creation->error->setText(tr("The source branch has no UUIDv7 identity."));
            updateButtons();
            return;
        }
        const QString project_id = project->backend_id;
        this->source_writer->createBranch(*server, project_id, name, from_branch_id, from_revision,
            [this, project_id, name](const QString &id, const QString &error)
            {
                finishSourceWrite(id, error, project_id, id.isEmpty() ? name : id);
            });
        updateConnectionStatus();
        updateButtons();
        return;
    }
    ProjectPreviewBranch branch;
    branch.id = makeId(QStringLiteral("b-"));
    branch.name = name;
    branch.description = this->branch_creation->description->toPlainText().trimmed();
    for (const QString &tag : this->branch_creation->tags->text().split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QString cleaned = tag.trimmed();
        if (!cleaned.isEmpty() && !branch.tags.contains(cleaned)) branch.tags.append(cleaned);
    }
    const QString origin = this->selection_kind == SelectionKind::Project
        ? QString() : this->branch_creation->source->currentData().toString();
    if (origin.startsWith(QStringLiteral("branch:")))
    {
        for (const ProjectPreviewBranch &candidate : project->branches)
            if (candidate.id == origin.mid(7)) branch.head = candidate.head;
    }
    else if (origin.startsWith(QStringLiteral("revision:")))
        branch.head = origin.mid(9);
    if (origin.startsWith(QStringLiteral("branch:")))
        for (const ProjectPreviewBranch &candidate : project->branches)
            if (candidate.id == origin.mid(7))
                branch.origin_branch_id = candidate.id;
    const QString id = branch.id;
    project->branches.append(branch);
    populateProjectTree();
    populateRevisionTree();
    rebuildGraph();
    cancelCreation();
    select(SelectionKind::Branch, id);
}

void ProjectsWidget::deleteInspectedBranch() {
    if (this->source_writer->isBusy()) return;
    const ProjectPreviewData *project = currentProject();
    const ProjectPreviewBranch *branch = inspectedRevisionBranch();
    const ProjectServerProfile *server = currentServer();
    if (project == nullptr || branch == nullptr || server == nullptr) return;
    if (!project->database_backed) { deleteSelection(); return; }
    const QString project_id = project->backend_id;
    const QString branch_id = branch->id.mid(project->id.size() + 1);
    const QString expected_head = branch->head;
    if (!DvmdbIdentity::isUuidV7(branch_id)) return;
    if (QMessageBox::warning(this, tr("Delete entire branch"),
        tr("Permanently remove the branch '%1' and its HEAD from this project? "
           "Its unique history will disappear from the branch graph. "
           "Immutable revisions still needed by other branches or merges remain stored. "
           "Deletion cannot be undone. Active working copies must be discarded first.")
            .arg(branch->name), QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel) != QMessageBox::Yes) return;
    this->source_writer->deleteBranch(*server, project_id, branch_id, expected_head,
        [this](const QString &, const QString &error) {
            if (!error.isEmpty()) QMessageBox::warning(this, tr("Branch deletion failed"), error);
            // Refresh even after ambiguous HTTP failure; never fake a deletion.
            connectSource();
        });
    updateButtons();
}

void ProjectsWidget::deleteInspectedRevision() {
    if (this->selection_kind == SelectionKind::Head) {
        const ProjectPreviewBranch *branch = inspectedRevisionBranch();
        if (branch != nullptr && branch->head.isEmpty()) deleteInspectedBranch();
        else discardInspectedHead();
        return;
    }
    if (this->selection_kind != SelectionKind::Revision || this->source_writer->isBusy()) return;
    const ProjectPreviewData *project = currentProject();
    const ProjectPreviewBranch *branch = inspectedRevisionBranch();
    const ProjectServerProfile *server = currentServer();
    if (project == nullptr || branch == nullptr || server == nullptr || !project->database_backed)
        return;
    const QString project_id = project->backend_id;
    const QString branch_id = branch->id.mid(project->id.size() + 1);
    const QString first_removed = this->selected_id;
    const QString head = branch->head;
    if (!DvmdbIdentity::isUuidV7(branch_id) || !DvmdbIdentity::isUuidV7(first_removed)
        || !DvmdbIdentity::isUuidV7(head)) return;
    if (QMessageBox::warning(this, tr("Delete revision history"),
        tr("Remove the selected revision and all subsequent first-parent revisions "
           "on branch '%1'? The branch may be deleted if this is its first revision. "
           "Shared merge ancestry is preserved. This cannot be undone. "
           "Active working copies must be discarded first.").arg(branch->name),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    this->source_writer->truncateBranch(*server, project_id, branch_id, head, first_removed,
        [this](const QString &, const QString &error) {
            if (!error.isEmpty()) QMessageBox::warning(this, tr("Revision deletion failed"), error);
            connectSource();
        });
    updateButtons();
}

void ProjectsWidget::discardInspectedHead() {
    if (this->selection_kind != SelectionKind::Head || this->source_writer->isBusy()) return;
    const ProjectPreviewData *project = currentProject();
    const ProjectPreviewBranch *branch = inspectedRevisionBranch();
    if (project == nullptr || branch == nullptr || !project->database_backed) return;
    const QString project_id = project->backend_id;
    const QString branch_id = branch->id.mid(project->id.size() + 1);
    const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    if (target.kind == PersonalRepositoryTarget::Kind::Unavailable) {
        QMessageBox::warning(this, tr("Cannot discard HEAD"),
            tr("A personal working database must be available to discard HEAD changes."));
        return;
    }
    if (QMessageBox::warning(this, tr("Delete HEAD working changes"),
        tr("Discard the saved private working copy of branch '%1', including its Undo/Redo "
           "journal? Committed revisions and other branches remain unchanged. "
           "This operation cannot be undone.").arg(branch->name),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    PersonalWorkingCopyClient *client = new PersonalWorkingCopyClient(this);
    client->list(target, project_id,
        [this, client, target, project_id, branch_id](
            const QList<PersonalWorkingCopyInfo> &drafts, const QString &error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Cannot discard HEAD"), error);
            client->deleteLater();
            return;
        }
        QList<PersonalWorkingCopyInfo> matches;
        for (const PersonalWorkingCopyInfo &draft : drafts)
            if (draft.project_id == project_id && draft.branch_id == branch_id)
                matches.append(draft);
        if (matches.isEmpty()) {
            QMessageBox::information(this, tr("HEAD is clean"),
                tr("No saved working copy exists for this HEAD. Committed history is unchanged."));
            client->deleteLater();
            return;
        }
        if (matches.size() != 1) {
            QMessageBox::warning(this, tr("Multiple working copies"),
                tr("This HEAD has multiple private working copies. Resolve them in "
                   "Personal working copies before deleting any HEAD state."));
            client->deleteLater();
            return;
        }
        const PersonalWorkingCopyInfo draft = matches.first();
        const QString session_id = DvmdbIdentity::newUuidV7();
        client->acquireLease(target, draft.id, session_id, 30,
            [this, client, target, draft, session_id](
                const PersonalWorkingCopyLease &lease, const QString &lease_error) {
            if (!lease_error.isEmpty()) {
                QMessageBox::warning(this, tr("HEAD is in use"), lease_error);
                client->deleteLater();
                return;
            }
            if (lease.generation != draft.generation) {
                client->releaseLease(target, draft.id, session_id, lease.token,
                    [this, client](const QString &) {
                    QMessageBox::warning(this, tr("HEAD changed"),
                        tr("The working copy changed. Review it before discarding."));
                    client->deleteLater();
                });
                return;
            }
            client->discardDraft(target, draft.id, session_id, lease.token,
                lease.generation, [this, client](const QString &discard_error) {
                if (!discard_error.isEmpty())
                    QMessageBox::warning(this, tr("HEAD discard failed"), discard_error);
                client->deleteLater();
                connectSource();
            });
        });
    });
}

void ProjectsWidget::deleteSelection()
{
    const SelectionKind kind = this->selection_kind;
    const QString id = this->selected_id;
    if (kind != SelectionKind::Project && kind != SelectionKind::Branch) return;
    if (!isPreviewSource() || (currentProject() != nullptr && currentProject()->database_backed)) return;
    if (QMessageBox::question(this, tr("Delete from preview"),
        tr("Remove this %1 from the in-memory UI preview? No database data is touched.")
            .arg(kind == SelectionKind::Project ? tr("project") : tr("branch"))) != QMessageBox::Yes)
        return;
    if (kind == SelectionKind::Project)
    {
        for (int i = 0; i < this->projects.size(); ++i)
            if (this->projects.at(i).id == id) { this->projects.removeAt(i); break; }
        this->project_id.clear();
        for (const ProjectPreviewData &candidate : this->projects)
            if (candidate.server_id == this->server_id)
            { this->project_id = candidate.id; break; }
        const ProjectPreviewData *project = currentProject();
        this->branch_id = project != nullptr && !project->branches.isEmpty()
            ? project->branches.first().id : QString();
    }
    else if (ProjectPreviewData *project = currentProject())
    {
        for (int i = 0; i < project->branches.size(); ++i)
            if (project->branches.at(i).id == id) { project->branches.removeAt(i); break; }
        this->branch_id = project->branches.isEmpty() ? QString() : project->branches.first().id;
    }
    this->selection_kind = SelectionKind::None;
    this->selected_id.clear();
    populateProjectTree();
    populateRevisionTree();
    rebuildGraph();
    if (kind == SelectionKind::Project && !this->project_id.isEmpty())
        select(SelectionKind::Project, this->project_id);
    else if (!this->branch_id.isEmpty())
        select(SelectionKind::Branch, this->branch_id);
    else if (currentProject() != nullptr)
        select(SelectionKind::Project, this->project_id);
    else if (currentServer() != nullptr)
        select(SelectionKind::Server, this->server_id);
    else
    {
        updateInspector();
        updateEmptyState();
        this->inspector_dock->hide();
        updateButtons();
        broadcastContext();
    }
}

void ProjectsWidget::browseExamples()
{
    if (!guiConfiguration().examples_builtin_enable) return;

    QDialog browser(this);
    browser.setObjectName(QStringLiteral("projectsExampleLibraryDialog"));
    browser.setWindowTitle(tr("Example Library"));
    browser.resize(720, 435);
    QVBoxLayout *layout = new QVBoxLayout(&browser);
    QLabel *heading = new QLabel(tr("Built-in network templates"), &browser);
    QFont heading_font = heading->font();
    heading_font.setBold(true);
    heading_font.setPointSizeF(heading_font.pointSizeF() + 2);
    heading->setFont(heading_font);
    layout->addWidget(heading);
    QLabel *intro = new QLabel(tr("Open an example network in the editor, or create an isolated in-memory demonstration."), &browser);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QHBoxLayout *content = new QHBoxLayout();
    QVBoxLayout *browser_left = new QVBoxLayout();
    QLineEdit *filter = new QLineEdit(&browser);
    filter->setObjectName(QStringLiteral("exampleLibraryFilter"));
    filter->setPlaceholderText(tr("Find a network…"));
    browser_left->addWidget(filter);
    QListWidget *list = new QListWidget(&browser);
    list->setObjectName(QStringLiteral("exampleLibraryList"));
    for (const BuiltinExampleRevision &example : builtinExampleRevisions())
    {
        QListWidgetItem *item = new QListWidgetItem(example.display_name, list);
        item->setData(Qt::UserRole, example.resource_path);
        item->setToolTip(example.description);
    }
    browser_left->addWidget(list, 1);
    content->addLayout(browser_left, 2);
    QVBoxLayout *browser_right = new QVBoxLayout();
    QLabel *example_name = new QLabel(&browser);
    example_name->setObjectName(QStringLiteral("exampleLibraryName"));
    QFont example_font = example_name->font();
    example_font.setBold(true);
    example_name->setFont(example_font);
    browser_right->addWidget(example_name);
    QLabel *description = new QLabel(&browser);
    description->setObjectName(QStringLiteral("exampleLibraryDescription"));
    description->setWordWrap(true);
    description->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    browser_right->addWidget(description);
    QLabel *origin = new QLabel(tr("Source: Bundled EPANET model (read-only template)"), &browser);
    origin->setWordWrap(true);
    browser_right->addWidget(origin);
    browser_right->addStretch();
    content->addLayout(browser_right, 3);
    layout->addLayout(content, 1);

    QLabel *disclaimer = new QLabel(tr("Creating a project is a GUI preview only: "
                                     "no database is contacted and no network is stored."), &browser);
    disclaimer->setObjectName(QStringLiteral("exampleLibraryPreviewNotice"));
    disclaimer->setWordWrap(true);
    layout->addWidget(disclaimer);

    QHBoxLayout *buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton *close_button = compactButton(tr("Close"), QString(), &browser);
    QPushButton *open_button = compactButton(tr("Open in editor"),
        tr("Load this EPANET example into the current working model"), &browser);
    open_button->setObjectName(QStringLiteral("exampleLibraryOpenButton"));
    QPushButton *create_button = compactButton(tr("Create preview project…"),
        tr("Choose a target server and prepare a project preview"), &browser);
    create_button->setObjectName(QStringLiteral("exampleLibraryCreateButton"));
    bool has_preview_profile = false;
    for (const ProjectServerProfile &source : this->servers)
        if (source.id == QStringLiteral("preview-local") || source.id == QStringLiteral("preview-remote"))
            has_preview_profile = true;
    create_button->setVisible(has_preview_profile);
    buttons->addWidget(close_button);
    buttons->addWidget(open_button);
    buttons->addWidget(create_button);
    layout->addLayout(buttons);

    const auto selectedExample = [list]() -> const BuiltinExampleRevision *
    {
        const QListWidgetItem *item = list->currentItem();
        if (item == nullptr) return nullptr;
        const QString path = item->data(Qt::UserRole).toString();
        for (const BuiltinExampleRevision &example : builtinExampleRevisions())
            if (example.resource_path == path) return &example;
        return nullptr;
    };
    const auto updateDetail = [selectedExample, example_name, description, open_button, create_button]()
    {
        const BuiltinExampleRevision *example = selectedExample();
        example_name->setText(example == nullptr ? QString() : example->display_name);
        description->setText(example == nullptr ? QString() : example->description);
        open_button->setEnabled(example != nullptr);
        create_button->setEnabled(example != nullptr);
    };
    connect(list, &QListWidget::currentItemChanged, &browser,
            [updateDetail](QListWidgetItem *, QListWidgetItem *) { updateDetail(); });
    connect(filter, &QLineEdit::textChanged, &browser, [list](const QString &text)
    {
        for (int row = 0; row < list->count(); ++row)
            list->item(row)->setHidden(!list->item(row)->text().contains(text, Qt::CaseInsensitive));
        if (list->currentItem() == nullptr || list->currentItem()->isHidden())
        {
            for (int row = 0; row < list->count(); ++row)
                if (!list->item(row)->isHidden()) { list->setCurrentRow(row); return; }
            list->setCurrentRow(-1);
        }
    });
    connect(close_button, &QPushButton::clicked, &browser, &QDialog::reject);
    connect(open_button, &QPushButton::clicked, &browser, [this, &browser, selectedExample]()
    {
        const BuiltinExampleRevision *example = selectedExample();
        if (example == nullptr) return;
        const QString path = example->resource_path;
        const QString file_name = example->file_name;
        browser.accept();
        emit builtinExampleRequested(path, file_name);
    });
    connect(list, &QListWidget::itemDoubleClicked, open_button,
            [open_button](QListWidgetItem *) { open_button->click(); });
    connect(create_button, &QPushButton::clicked, &browser, [this, &browser, selectedExample]()
    {
        const BuiltinExampleRevision *example = selectedExample();
        if (example == nullptr) return;
        const QString path = example->resource_path;
        browser.accept();
        for (const BuiltinExampleRevision &entry : builtinExampleRevisions())
            if (entry.resource_path == path) { createProjectFromExample(entry); break; }
    });
    list->setCurrentRow(0);
    updateDetail();
    browser.exec();
}

void ProjectsWidget::createProjectFromExample(const BuiltinExampleRevision &example)
{
    if (this->servers.isEmpty() || !std::any_of(this->servers.cbegin(), this->servers.cend(),
        [](const ProjectServerProfile &source)
        { return source.id == QStringLiteral("preview-local") || source.id == QStringLiteral("preview-remote"); }))
    {
        QMessageBox::information(this, tr("No preview database"),
            tr("Creating an in-memory example project is only available in the demonstration profile."));
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("exampleCreateProjectDialog"));
    dialog.setWindowTitle(tr("Create project from %1 (preview)").arg(example.display_name));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QFormLayout *fields = new QFormLayout();
    fields->setRowWrapPolicy(QFormLayout::WrapAllRows);
    QComboBox *server = new QComboBox(&dialog);
    server->setObjectName(QStringLiteral("exampleCreateServer"));
    for (const ProjectServerProfile &entry : this->servers)
        if (entry.id == QStringLiteral("preview-local") || entry.id == QStringLiteral("preview-remote"))
            server->addItem(entry.name, entry.id);
    const int selected_server = server->findData(this->server_id);
    if (selected_server >= 0) server->setCurrentIndex(selected_server);
    QLineEdit *project_name = new QLineEdit(example.display_name, &dialog);
    project_name->setObjectName(QStringLiteral("exampleCreateProjectName"));
    QLineEdit *branch_name = new QLineEdit(QStringLiteral("main"), &dialog);
    branch_name->setObjectName(QStringLiteral("exampleCreateBranchName"));
    QLineEdit *tags = new QLineEdit(QStringLiteral("example"), &dialog);
    tags->setObjectName(QStringLiteral("exampleCreateTags"));
    QLineEdit *description = new QLineEdit(tr("Based on %1").arg(example.display_name), &dialog);
    description->setObjectName(QStringLiteral("exampleCreateDescription"));
    fields->addRow(tr("Destination database"), server);
    fields->addRow(tr("Project name"), project_name);
    fields->addRow(tr("Initial branch"), branch_name);
    fields->addRow(tr("Tags"), tags);
    fields->addRow(tr("Description"), description);
    layout->addLayout(fields);
    QLabel *notice = new QLabel(tr("GUI PREVIEW ONLY — the server is not contacted. "
                                  "This creates a project/branch/revision placeholder; "
                                  "the hydraulic network is not copied or saved."), &dialog);
    notice->setObjectName(QStringLiteral("exampleCreatePreviewNotice"));
    notice->setWordWrap(true);
    layout->addWidget(notice);
    QDialogButtonBox *actions = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton *create = actions->addButton(tr("Create preview project"), QDialogButtonBox::AcceptRole);
    create->setObjectName(QStringLiteral("exampleCreateConfirmButton"));
    layout->addWidget(actions);
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto validate = [this, project_name, branch_name, server, create]()
    {
        const QString name = project_name->text().trimmed();
        const QString branch = branch_name->text().trimmed();
        bool duplicate = false;
        for (const ProjectPreviewData &entry : this->projects)
            if (entry.server_id == server->currentData().toString()
                && entry.name.compare(name, Qt::CaseInsensitive) == 0) duplicate = true;
        create->setEnabled(!name.isEmpty() && !branch.isEmpty() && !duplicate);
        create->setToolTip(duplicate ? tr("A project with this name already exists on the selected server") : QString());
    };
    connect(project_name, &QLineEdit::textChanged, &dialog, [validate](const QString &) { validate(); });
    connect(branch_name, &QLineEdit::textChanged, &dialog, [validate](const QString &) { validate(); });
    connect(server, QOverload<int>::of(&QComboBox::currentIndexChanged),
            &dialog, [validate](int) { validate(); });
    connect(create, &QPushButton::clicked, &dialog, &QDialog::accept);
    validate();
    if (dialog.exec() != QDialog::Accepted || !create->isEnabled()) return;

    ProjectPreviewData project;
    project.id = makeId(QStringLiteral("p-"));
    project.server_id = server->currentData().toString();
    project.name = project_name->text().trimmed();
    project.description = description->text().trimmed();
    project.tags = parseTags(tags->text());
    project.source_example_resource = example.resource_path;
    ProjectPreviewBranch branch;
    branch.id = makeId(QStringLiteral("b-"));
    branch.name = branch_name->text().trimmed();
    branch.description = tr("Preview branch for %1").arg(example.display_name);
    ProjectPreviewRevision revision;
    revision.id = makeId(QStringLiteral("r-"));
    revision.title = tr("Initial import (preview)");
    revision.description = tr("Planned import from %1. No network data is stored.")
        .arg(example.display_name);
    revision.branch = branch.name;
    revision.tags = {QStringLiteral("preview"), QStringLiteral("example")};
    branch.head = revision.id;
    project.branches.append(branch);
    project.revisions.append(revision);
    const QString id = project.id;
    this->projects.append(project);
    populateProjectTree();
    select(SelectionKind::Project, id);
}

void ProjectsWidget::setPersonalServer(bool enabled)
{
    if (this->updating_ui || this->selection_kind != SelectionKind::Server) return;
    ProjectServerProfile *server = currentServer();
    if (server == nullptr) return;
    if (enabled && server->pending)
    {
        const QSignalBlocker blocker(this->database_connection->personal_server_checkbox);
        this->database_connection->personal_server_checkbox->setChecked(false);
        return;
    }
    const QString selected_id = server->id;
    const QString selected_endpoint = server->endpoint;
    const int request_id = ++this->personal_designation_request;
    const std::function<void(const QString &, bool)> persistSelection = [this, selected_id](const QString &repository_id, bool select)
    {
        if (select && repository_id != selected_id) return;
        const QList<ProjectServerProfile> previous = this->servers;
        for (ProjectServerProfile &candidate : this->servers)
        {
            if (select || candidate.id == selected_id)
            {
                candidate.personal = select && candidate.id == selected_id;
            }
        }
        if (!saveProjectServerProfiles(this->servers))
        {
            this->servers = previous;
            reportServerSaveFailure();
        }
        populateServerTree();
        rebuildGraph(); // Update the golden server card immediately when designation changes.
        if (this->selection_kind == SelectionKind::Server) updateInspector();
    };
    if (!enabled)
    {
        persistSelection(QString(), false);
        return;
    }
    if (server->connection_type == QStringLiteral("local"))
    {
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        QString repository_id;
        QString error;
        const QString endpoint = embeddedDvmdbEndpoint(server->local_path, true,
                                                       &repository_id, &error);
        if (endpoint.isEmpty() || !DvmdbIdentity::isUuidV7(repository_id))
        {
            QMessageBox::warning(this, tr("Cannot select personal database"),
                error.isEmpty() ? tr("Unable to verify local DVMDB identity.") : error);
            const QSignalBlocker blocker(this->database_connection->personal_server_checkbox);
            this->database_connection->personal_server_checkbox->setChecked(false);
            return;
        }
        if (repository_id != selected_id)
        {
            QMessageBox::warning(this, tr("Repository identity mismatch"),
                tr("The embedded database identity does not match the configured repository UUID."));
            return;
        }
        persistSelection(repository_id, true);
#else
        QMessageBox::warning(this, tr("Embedded database unavailable"),
            tr("This build does not include native DVMDB support."));
#endif
        return;
    }
    // The checkbox is an explicit deployment-role choice. Discover identity first,
    // configure an unconfigured repository once, then pin its UUID locally.
    // Never silently change a server that is already marked as shared.
    QString endpoint = selected_endpoint;
    while (endpoint.endsWith(QLatin1Char('/'))) endpoint.chop(1);
    if (!endpoint.endsWith(QStringLiteral("/dvmdb/v1")))
        endpoint += QStringLiteral("/dvmdb/v1");
    this->database_connection->personal_server_checkbox->setEnabled(false);
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    const std::function<bool()> stillSelected = [this, selected_id, selected_endpoint, request_id]()
    {
        if (request_id != this->personal_designation_request) return false;
        for (const ProjectServerProfile &candidate : this->servers)
            if (candidate.id == selected_id && candidate.endpoint == selected_endpoint)
                return true;
        return false;
    };
    const std::function<void(const QString &)> rejectSelection = [this, stillSelected](const QString &reason)
    {
        if (!stillSelected()) return;
        QMessageBox::warning(this, tr("Cannot select personal database"), reason);
        if (this->selection_kind == SelectionKind::Server) updateInspector();
    };
    const std::function<void(const QString &, const QString &)> acceptRole =
        [this, selected_id, persistSelection, stillSelected, rejectSelection](const QString &id, const QString &role)
    {
        if (!stillSelected()) return;
        if (!DvmdbIdentity::isUuidV7(id) || id != selected_id
            || role != QStringLiteral("private"))
        {
            rejectSelection(tr("The server did not confirm its private role and UUIDv7 identity."));
            return;
        }
        persistSelection(id, true);
    };
    QNetworkRequest request(QUrl(endpoint + QStringLiteral("/repository/tracking")));
    request.setTransferTimeout(8000);
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, manager, reply, endpoint, stillSelected, rejectSelection, acceptRole]()
    {
        const QByteArray body = reply->readAll();
        const QString networkError = reply->errorString();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool success = reply->error() == QNetworkReply::NoError && status == 200;
        reply->deleteLater();
        if (!stillSelected()) { manager->deleteLater(); return; }
        if (!success)
        {
            manager->deleteLater();
            rejectSelection(tr("Could not read DVMDB repository identity (%1, HTTP %2). "
                               "Check the selected server URL and whether DVMDB is running.")
                            .arg(networkError).arg(status));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QJsonObject data = document.isObject() ? document.object() : QJsonObject();
        const QString id = data.value(QStringLiteral("repositoryId")).toString();
        const QString role = data.value(QStringLiteral("role")).toString();
        const QString codec_error = ModelCodecCompatibility::error(data);
        if (!codec_error.isEmpty())
        {
            manager->deleteLater();
            rejectSelection(codec_error);
            return;
        }
        if (!DvmdbIdentity::isUuidV7(id))
        {
            manager->deleteLater();
            rejectSelection(tr("DVMDB returned an invalid repository UUIDv7."));
            return;
        }
        if (role == QStringLiteral("private"))
        {
            manager->deleteLater();
            acceptRole(id, role);
            return;
        }
        if (role == QStringLiteral("shared"))
        {
            manager->deleteLater();
            rejectSelection(tr("This DVMDB is already configured as a shared repository. "
                               "Its role cannot be changed to private. Select a different server."));
            return;
        }
        if (role != QStringLiteral("unconfigured"))
        {
            manager->deleteLater();
            rejectSelection(tr("DVMDB returned an unknown repository role: %1.").arg(role));
            return;
        }
        // Configure the role on the server itself, bound to the UUID we just
        // discovered. This is idempotent, including lost response retries.
        QNetworkRequest configure(QUrl(endpoint + QStringLiteral("/repository/role")));
        configure.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        configure.setTransferTimeout(8000);
        const QByteArray payload = QJsonDocument(QJsonObject{
            {QStringLiteral("expectedRepositoryId"), id},
            {QStringLiteral("role"), QStringLiteral("private")}
        }).toJson(QJsonDocument::Compact);
        QNetworkReply *configured = manager->post(configure, payload);
        connect(configured, &QNetworkReply::finished, this,
                [this, manager, configured, id, stillSelected, rejectSelection, acceptRole]()
        {
            const QByteArray response = configured->readAll();
            const QString networkError = configured->errorString();
            const int status = configured->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool success = configured->error() == QNetworkReply::NoError && status == 200;
            configured->deleteLater();
            manager->deleteLater();
            if (!stillSelected()) return;
            if (!success)
            {
                rejectSelection(tr("Could not configure this DVMDB as private (HTTP %1: %2). "
                                   "The server may need the updated DVMDB build or may already "
                                   "have a conflicting repository role.")
                                .arg(status).arg(networkError));
                return;
            }
            const QJsonDocument document = QJsonDocument::fromJson(response);
            const QJsonObject result = document.isObject() ? document.object() : QJsonObject();
            if (result.value(QStringLiteral("repositoryId")).toString() != id)
            {
                rejectSelection(tr("DVMDB repository identity changed while configuring its role."));
                return;
            }
            acceptRole(id, result.value(QStringLiteral("role")).toString());
        });
    });
}

const ProjectPreviewBranch *ProjectsWidget::inspectedRevisionBranch() const
{
    const ProjectPreviewData *project = currentProject();
    if (project == nullptr) return nullptr;
    if (this->selection_kind == SelectionKind::Head || this->selection_kind == SelectionKind::Branch)
        for (const ProjectPreviewBranch &branch : project->branches)
            if (branch.id == this->branch_id) return &branch;
    if (this->selection_kind != SelectionKind::Revision) return nullptr;
    const QString revision_id = this->selected_id;
    QHash<QString, QStringList> parents;
    for (const ProjectPreviewRevision &revision : project->revisions)
        parents.insert(revision.id, revision.parents);
    const std::function<bool(const ProjectPreviewBranch &)> contains =
        [&parents, revision_id](const ProjectPreviewBranch &branch) -> bool {
        QStringList pending{branch.head};
        QSet<QString> visited;
        while (!pending.isEmpty()) {
            const QString current = pending.takeLast();
            if (current == revision_id) return true;
            if (current.isEmpty() || visited.contains(current)) continue;
            visited.insert(current);
            pending.append(parents.value(current));
        }
        return false;
    };
    for (const ProjectPreviewBranch &branch : project->branches)
        if (branch.id == this->branch_id && contains(branch)) return &branch;
    const ProjectPreviewBranch *unique = nullptr;
    for (const ProjectPreviewBranch &branch : project->branches) {
        if (!contains(branch)) continue;
        if (unique != nullptr) return nullptr; // No invented ownership for shared history.
        unique = &branch;
    }
    return unique;
}

void ProjectsWidget::saveRevisionInspectorChanges()
{
    if (this->source_writer->isBusy() || this->revision_rename_confirmation_pending) return;
    const SelectionKind mode = this->selection_kind;
    if (mode != SelectionKind::Head && mode != SelectionKind::Revision) return;
    ProjectPreviewData *project = currentProject();
    const ProjectPreviewBranch *branch = inspectedRevisionBranch();
    if (project == nullptr || branch == nullptr) {
        QMessageBox::warning(this, tr("Branch context required"),
            tr("This revision belongs to more than one branch. Select a branch in the left tree first."));
        return;
    }
    const QString new_name = this->revision_overview->name->text().trimmed();
    const QString new_description = this->revision_overview->description->toPlainText().trimmed();
    const QString new_message = this->revision_overview->commit_message->toPlainText();
    QStringList new_tags = parseTags(this->revision_overview->tags->text());
    if (new_name.isEmpty() || new_name.toUtf8().size() > 96
        || new_description.toUtf8().size() > 512 || new_tags.size() > 12) {
        QMessageBox::warning(this, tr("Invalid branch metadata"),
            tr("The name must be 1–96 bytes, description at most 512 bytes, and tags at most 12."));
        return;
    }
    for (const QString &tag : new_tags)
        if (tag.toUtf8().size() > 32) {
            QMessageBox::warning(this, tr("Invalid branch tag"),
                tr("Each tag may contain at most 32 UTF-8 bytes."));
            return;
        }
    for (const ProjectPreviewBranch &candidate : project->branches)
        if (candidate.id != branch->id && candidate.name.compare(new_name, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, tr("Duplicate branch"),
                tr("Another branch in this project already has that name."));
            return;
        }
    if (mode == SelectionKind::Revision && (new_message.trimmed().isEmpty()
        || new_message.toUtf8().size() > 65536)) {
        QMessageBox::warning(this, tr("Invalid commit message"),
            tr("A revision commit message must be nonempty and at most 65536 UTF-8 bytes."));
        return;
    }
    const QString old_name = branch->name;
    const QString old_description = branch->description;
    const QStringList old_tags = branch->tags;
    const QString branch_id = branch->id;
    const QString project_id = project->id;
    const QString selected_id = this->selected_id;
    const QString server_id = this->server_id;
    const QString backend_project_id = project->backend_id;
    const QString backend_branch_id = branch_id.mid(project_id.size() + 1);
    const QString old_message = mode == SelectionKind::Revision && selectedRevision() != nullptr
        ? (selectedRevision()->message.isEmpty() ? selectedRevision()->title : selectedRevision()->message)
        : QString();
    const bool branch_changed = old_name != new_name || old_description != new_description
        || old_tags != new_tags;
    const bool message_changed = mode == SelectionKind::Revision && old_message != new_message;
    if (!branch_changed && !message_changed) return;
    const ProjectServerProfile *server = currentServer();
    if (project->database_backed && server == nullptr) return;
    const ProjectServerProfile profile = server != nullptr ? *server : ProjectServerProfile{};
    const std::function<void()> finish_success = [this, mode, project_id, selected_id, server_id]() {
        if (server_id != this->server_id) return;
        if (!isPreviewSource()) {
            loadSource(server_id);
        } else {
            populateProjectTree(); populateRevisionTree(); rebuildGraph();
            select(mode, selected_id);
        }
    };
    const std::function<void()> perform = [this, mode, project_id, branch_id,
            selected_id, profile, backend_project_id, backend_branch_id, old_name,
            new_name, old_description, new_description, old_tags, new_tags,
            old_message, new_message, branch_changed, message_changed, finish_success]() {
        if (this->project_id != project_id || this->selected_id != selected_id
            || this->selection_kind != mode) return;
        if (isPreviewSource()) {
            ProjectPreviewData *mutable_project = currentProject();
            if (mutable_project == nullptr) return;
            for (ProjectPreviewBranch &mutable_branch : mutable_project->branches)
                if (mutable_branch.id == branch_id) {
                    mutable_branch.name = new_name;
                    mutable_branch.description = new_description;
                    mutable_branch.tags = new_tags;
                }
            if (mode == SelectionKind::Revision) {
                if (ProjectPreviewRevision *revision = selectedRevision())
                    revision->message = new_message;
            } else {
                this->head_commit_drafts.insert(branch_id, new_message);
            }
            finish_success();
            return;
        }
        const std::function<void()> update_message = [this, profile, backend_project_id,
                selected_id, old_message, new_message, message_changed, finish_success]() {
            if (!message_changed) { finish_success(); return; }
            this->source_writer->updateRevisionMessage(profile, backend_project_id,
                selected_id, old_message, new_message,
                [this, finish_success](const QString &, const QString &error) {
                    if (!error.isEmpty())
                        QMessageBox::warning(this, tr("Commit message update failed"), error);
                    finish_success();
                });
        };
        if (branch_changed) {
            this->source_writer->updateBranchProperties(profile, backend_project_id,
                backend_branch_id, old_name, new_name, new_tags, new_description,
                [this, update_message, finish_success](const QString &, const QString &error) {
                    if (!error.isEmpty()) {
                        QMessageBox::warning(this, tr("Branch update failed"), error);
                        finish_success();
                    } else update_message();
                });
        } else update_message();
    };
    if (old_name == new_name) { perform(); return; }
    this->revision_rename_confirmation_pending = true;
    this->revision_autosave_timer->stop();
    QMessageBox *confirmation = new QMessageBox(QMessageBox::Question, tr("Rename branch?"),
        tr("Rename branch '%1' to '%2' everywhere in this project?\n\n"
           "Revision UUIDs, contents and record hashes remain unchanged.")
            .arg(old_name, new_name), QMessageBox::Yes | QMessageBox::Cancel, this);
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    connect(confirmation, &QMessageBox::finished, this,
            [this, perform, old_name, mode, selected_id](int button) {
        this->revision_rename_confirmation_pending = false;
        if (button == QMessageBox::Yes) {
            perform();
        } else if (this->selection_kind == mode && this->selected_id == selected_id) {
            const QSignalBlocker blocked(this->revision_overview->name);
            this->revision_overview->name->setText(old_name);
            this->revision_overview->branch_label->setText(old_name);
            this->revision_autosave_timer->start();
        }
    });
    confirmation->open(); // Non-blocking: required for Qt WebAssembly.
}

void ProjectsWidget::commitSelectedHead()
{
    if (this->selection_kind != SelectionKind::Head || this->source_writer->isBusy()) return;
    const ProjectPreviewBranch *branch = currentBranch();
    const ProjectPreviewData *project = currentProject();
    if (branch == nullptr || project == nullptr || !project->database_backed) {
        QMessageBox::information(this, tr("Commit HEAD"),
            tr("A real, database-backed private HEAD and saved working copy are required."));
        return;
    }
    const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    if (target.kind == PersonalRepositoryTarget::Kind::Unavailable
        || target.repository_id != project->repository_id) {
        QMessageBox::warning(this, tr("Private working copy required"),
            tr("Checkout the branch into your personal database and select its HEAD before committing."));
        return;
    }
    const QString backend_branch_id = branch->id.mid(project->id.size() + 1);
    const QString backend_project_id = project->backend_id;
    QString disk_error;
    const QString saved_state = PersonalWorkingCopyClient::recordedCommitState(
        target, backend_project_id, backend_branch_id, &disk_error);
    if (!disk_error.isEmpty()) {
        QMessageBox::warning(this, tr("Commit recovery blocked"), disk_error);
        return;
    }
    if (!saved_state.isEmpty()) {
        // This can run after a GUI restart without a saved message, writer
        // lease or currently dirty draft. Never generate a second operation.
        PersonalWorkingCopyClient *recovery = new PersonalWorkingCopyClient(this);
        const QString source_id = this->server_id;
        recovery->reconcileRecordedCommit(target, backend_project_id, backend_branch_id,
            [this, recovery, target, backend_project_id, backend_branch_id, source_id]
            (const QString &revision, const QString &error) {
            QString state_error;
            const QString state = PersonalWorkingCopyClient::recordedCommitState(
                target, backend_project_id, backend_branch_id, &state_error);
            if (state == QStringLiteral("succeeded") && !revision.isEmpty()) {
                if (this->server_id == source_id) loadSource(source_id);
                QMessageBox::information(this, tr("Commit recovered"),
                    tr("Confirmed immutable revision %1.").arg(revision));
            } else {
                QMessageBox::warning(this, tr("Commit recovery"),
                    !state_error.isEmpty() ? state_error : error);
            }
            if (state == QStringLiteral("succeeded") || state == QStringLiteral("rejected")) {
                QString acknowledgement_error;
                if (!PersonalWorkingCopyClient::acknowledgeRecordedCommit(
                        target, backend_project_id, backend_branch_id, &acknowledgement_error))
                    QMessageBox::warning(this, tr("Commit recovery not cleared"), acknowledgement_error);
            }
            recovery->deleteLater();
            updateButtons();
        });
        return;
    }
    const QString message = this->revision_overview->commit_message->toPlainText();
    if (message.trimmed().isEmpty() || message.toUtf8().size() > 65536) {
        QMessageBox::warning(this, tr("Commit message required"),
            tr("Enter a nonempty commit message (up to 65536 UTF-8 bytes)."));
        return;
    }
    if (this->revision_overview->name->text().trimmed() != branch->name
        || parseTags(this->revision_overview->tags->text()) != branch->tags
        || this->revision_overview->description->toPlainText().trimmed() != branch->description) {
        QMessageBox::information(this, tr("Branch metadata still pending"),
            tr("Finish editing the branch metadata and allow the automatic save to complete before committing HEAD."));
        return;
    }
    const QString branch_id = branch->id.mid(project->id.size() + 1);
    const QString project_id = project->backend_id;
    const QString source_id = this->server_id;
    const QString selected_branch = branch->id;
    PersonalWorkingCopyClient *client = new PersonalWorkingCopyClient(this);
    client->list(target, project_id, [this, client, target, project_id, branch_id, source_id,
                          selected_branch, message](const QList<PersonalWorkingCopyInfo> &drafts,
                                                     const QString &error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Cannot find saved draft"), error);
            client->deleteLater(); return;
        }
        QString draft_id;
        int matches = 0;
        for (const PersonalWorkingCopyInfo &draft : drafts)
            if (draft.project_id == project_id && draft.branch_id == branch_id && draft.dirty) {
                ++matches;
                draft_id = draft.id;
            }
        if (matches != 1) {
            QMessageBox::warning(this, tr("Select a saved working copy"), matches == 0
                ? tr("There is no saved modified working copy for this HEAD.")
                : tr("Multiple modified drafts exist. Resolve the competing drafts before committing."));
            client->deleteLater(); return;
        }
        client->reopen(target, draft_id,
            [this, client, target, project_id, branch_id, source_id, selected_branch, message]
            (const PersonalWorkingCopyInfo &draft, const QString &reopen_error) {
            if (!reopen_error.isEmpty() || draft.project_id != project_id
                || draft.branch_id != branch_id || !draft.dirty || draft.changes.isEmpty()) {
                QMessageBox::warning(this, tr("Cannot commit draft"), reopen_error.isEmpty()
                    ? tr("The saved working copy is no longer valid or has no changes.") : reopen_error);
                client->deleteLater(); return;
            }
            // Edits still in this device's queue are not part of the saved
            // draft yet; committing now would leave them out of the revision.
            const int queued = PersonalEditQueue::pendingTransactionFileCount(
                PersonalEditQueue::defaultRoot(), QUuid(target.repository_id), QUuid(draft.id));
            if (queued != 0) {
                QMessageBox::warning(this, tr("Edits not yet saved"), queued < 0
                    ? tr("The local edit queue of this draft cannot be read. Commit was not started.")
                    : tr("%n edit(s) of this draft are queued on this device but not yet saved "
                         "to DVMDB. Open the network for editing so they are saved, stop "
                         "editing, then commit.", nullptr, queued));
                client->deleteLater(); return;
            }
            // A fresh writer session must acquire the lease, fencing out edits
            // from another GUI session. Never commit a stale editor session.
            const QString session_id = DvmdbIdentity::newUuidV7();
            client->acquireLease(target, draft.id, session_id, 120,
                [this, client, target, draft, session_id, source_id, selected_branch, message]
                (const PersonalWorkingCopyLease &lease, const QString &lease_error) {
                if (!lease_error.isEmpty()) {
                    QMessageBox::warning(this, tr("Draft is being edited"), lease_error);
                    client->deleteLater(); return;
                }
                client->commitSavedDraft(target, draft, session_id, lease, message,
                    [this, client, target, draft, session_id, lease, source_id, selected_branch]
                    (const QString &revision, const QString &commit_error) {
                    if (!commit_error.isEmpty()) {
                        // An unconfirmed Commit may still finish on the server;
                        // its lease then expires on its own (120 s).
                        client->deleteLater();
                        QMessageBox::warning(this, tr("Commit not confirmed"), commit_error);
                        updateButtons();
                        return;
                    }
                    // Give the draft back at once, before the modal message:
                    // otherwise reopening it for editing fails for up to two
                    // minutes ("draft held by another writer session").
                    client->releaseLease(target, draft.id, session_id, lease.token,
                        [client](const QString &release_error) {
                            if (!release_error.isEmpty())
                                qWarning() << "Commit lease release not confirmed:" << release_error;
                            client->deleteLater();
                        });
                    this->head_commit_drafts.remove(selected_branch);
                    if (this->server_id == source_id) loadSource(source_id);
                    QMessageBox::information(this, tr("Committed"),
                        tr("Created immutable revision %1.").arg(revision));
                    QString acknowledgement_error;
                    if (!PersonalWorkingCopyClient::acknowledgeRecordedCommit(
                            target, draft.project_id, draft.branch_id, &acknowledgement_error))
                        QMessageBox::warning(this, tr("Commit recovery not cleared"), acknowledgement_error);
                });
            });
        });
    });
}

void ProjectsWidget::applyInspectorEdits()
{
    if (this->updating_ui || this->source_writer->isBusy()) return;
    ProjectsInspectorOverview *overview = activeOverview();
    const QString name = overview->name->text().trimmed();
    const QString description = overview->description->toPlainText();
    const QStringList tags = parseTags(overview->tags->text());
    bool tree_changed = false;
    if (this->selection_kind == SelectionKind::Server)
    {
        ProjectServerProfile *server = currentServer();
        if (server == nullptr) return;
        const ProjectServerProfile previous = *server;
        if (name.isEmpty())
        {
            if (sender() == overview->name)
            {
                QMessageBox::warning(this, tr("Invalid database name"), tr("A database name cannot be empty."));
                overview->name->setText(server->name);
            }
        }
        else if (name != server->name)
        {
            bool exists = false;
            for (const ProjectServerProfile &other : this->servers)
                if (other.id != server->id && other.name.compare(name, Qt::CaseInsensitive) == 0)
                    exists = true;
            if (exists)
            {
                if (sender() == overview->name)
                {
                    QMessageBox::warning(this, tr("Already exists"), tr("Choose a different database name."));
                    overview->name->setText(server->name);
                }
            }
            else server->name = name;
        }
        if (server->connection_type == QStringLiteral("local"))
        {
            const QString path = QDir::cleanPath(this->database_connection->local_path_edit->text().trimmed());
            if (QDir::isAbsolutePath(path))
                server->local_path = path;
            else if (sender() == this->database_connection->local_path_edit)
            {
                QMessageBox::warning(this, tr("Invalid directory"),
                    tr("Enter an absolute path for the local DVMDB data directory."));
                this->database_connection->local_path_edit->setText(server->local_path);
            }
        }
        else
        {
            const QString endpoint = this->database_connection->server_endpoint_edit->text().trimmed();
            const QUrl url(endpoint);
            if (!endpoint.isEmpty() && (!url.isValid()
                || (url.scheme() != QStringLiteral("http")
                    && url.scheme() != QStringLiteral("https")) || url.host().isEmpty()
                || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()))
            {
                if (sender() == this->database_connection->server_endpoint_edit)
                {
                    QMessageBox::warning(this, tr("Invalid server URL"),
                        tr("Use an http:// or https:// URL with a host, without embedded credentials, query or fragment."));
                    {
            const QSignalBlocker personal_blocker(this->database_connection->personal_server_checkbox);
            this->database_connection->personal_server_checkbox->setChecked(server->personal);
        }
        this->database_connection->server_endpoint_edit->setText(server->endpoint);
                }
            }
            else server->endpoint = endpoint;
            server->auth_mode = this->database_connection->server_auth_combo->currentData().toString();
        }
        if (server->personal && (server->endpoint != previous.endpoint
                                 || server->local_path != previous.local_path))
        {
            *server = previous;
            this->database_connection->server_endpoint_edit->setText(previous.endpoint);
            this->database_connection->local_path_edit->setText(previous.local_path);
            QMessageBox::information(this, tr("Personal database"),
                tr("Uncheck 'Use as my personal working database' before changing its endpoint or data directory."));
            return;
        }
        server->description = description;
        server->tags = tags;
        if (server->name != previous.name || server->endpoint != previous.endpoint
            || server->local_path != previous.local_path
            || server->auth_mode != previous.auth_mode
            || server->description != previous.description || server->tags != previous.tags)
        {
            if (!saveProjectServerProfiles(this->servers))
            {
                *server = previous;
                updateInspector();
                reportServerSaveFailure();
                return;
            }
            if (server->endpoint != previous.endpoint || server->local_path != previous.local_path
                || server->auth_mode != previous.auth_mode)
            {
                resetSource(server->id);
                startSource(*server);
            }
            // A database rename also appears in the Projects tree, server graph
            // cards and contextual project selectors. Refresh them immediately
            // after the profile is saved, without restarting the DVMDB session.
            populateServerTree();
            if (server->name != previous.name)
            {
                // Database type and name share the bold heading; update it
                // immediately after a successful profile rename.
                this->inspector_heading->setText(
                    ProjectsInspectorPresentations::database(server, QString()).heading);
                populateProjectTree();
                rebuildGraph();
                broadcastContext();
            }
            selectTreeItem(this->server_tree, keyFor(QStringLiteral("s"), this->server_id));
        }
    }
    else if (this->selection_kind == SelectionKind::Project)
    {
        ProjectPreviewData *project = currentProject();
        if (project == nullptr) return;
        if (project->database_backed)
        {
            // The project UUID stays fixed; only its persisted display name changes.
            if (sender() != overview->name || name == project->name) return;
            if (name.isEmpty() || name.toUtf8().size() > 200)
            {
                QMessageBox::warning(this, tr("Invalid project name"),
                    tr("A project name must contain between 1 and 200 UTF-8 bytes."));
                overview->name->setText(project->name);
                return;
            }
            for (const ProjectPreviewData &other : this->projects)
                if (other.id != project->id && other.server_id == project->server_id
                    && other.name.compare(name, Qt::CaseInsensitive) == 0)
                {
                    QMessageBox::warning(this, tr("Already exists"),
                        tr("Choose a different project name."));
                    overview->name->setText(project->name);
                    return;
                }
            const ProjectServerProfile *server = currentServer();
            if (server == nullptr || !isSourceReady(this->server_id))
            {
                overview->name->setText(project->name);
                return;
            }
            const QString selected_project = project->id;
            const QString selected_server = this->server_id;
            this->source_writer->renameProject(*server, project->backend_id, project->name, name,
                [this, selected_project, selected_server](const QString &, const QString &error)
                {
                    if (!error.isEmpty())
                        QMessageBox::warning(this, tr("Project rename failed"), error);
                    // Even a timed-out request may have committed. Always reload.
                    this->select_after_write_project = selected_project;
                    this->select_after_write_branch.clear();
                    if (this->server_id == selected_server) connectSource();
                    else updateInspector();
                });
            updateButtons();
            return;
        }
        if (!name.isEmpty() && name != project->name)
        {
            bool exists = false;
            for (const ProjectPreviewData &other : this->projects)
                if (other.id != project->id && other.server_id == this->server_id
                    && other.name.compare(name, Qt::CaseInsensitive) == 0) exists = true;
            if (!exists) { project->name = name; tree_changed = true; }
        }
        project->description = description;
        project->tags = tags;
    }
    else if (this->selection_kind == SelectionKind::Branch)
    {
        ProjectPreviewData *project = currentProject();
        if (project != nullptr && project->database_backed) return;
        if (project == nullptr) return;
        for (ProjectPreviewBranch &branch : project->branches)
            if (branch.id == this->selected_id)
            {
                if (!name.isEmpty() && branch.name != name)
                {
                    bool exists = false;
                    for (const ProjectPreviewBranch &other : project->branches)
                        if (other.id != branch.id && other.name.compare(name, Qt::CaseInsensitive) == 0) exists = true;
                    if (!exists)
                    {
                        const QString previous = branch.name;
                        branch.name = name;
                        for (ProjectPreviewRevision &revision : project->revisions)
                            if (revision.branch == previous) revision.branch = name;
                        tree_changed = true;
                    }
                }
                branch.description = description;
                branch.tags = tags;
                break;
            }
    }
    else if (this->selection_kind == SelectionKind::Head)
        return; // HEAD fields display branch identity; edit branch metadata in its own inspector.
    else if (this->selection_kind == SelectionKind::Revision)
    {
        if (currentProject() != nullptr && currentProject()->database_backed) return;
        if (ProjectPreviewRevision *revision = selectedRevision())
        {
            if (!name.isEmpty() && revision->title != name) { revision->title = name; tree_changed = true; }
            revision->description = description;
            revision->tags = tags;
        }
    }
    else return;
    if (tree_changed)
    {
        populateProjectTree();
        populateRevisionTree();
        if (this->selection_kind != SelectionKind::Server) rebuildGraph();
        select(this->selection_kind, this->selected_id);
    }
}
