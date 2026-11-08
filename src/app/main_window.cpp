#include "app/main_window.h"
#include "app/history_dock.h"
#include "tabs/tab_map_workspace.h"
#include "tabs/tab_projects_widget.h"
#include "config/gui_configuration.h"
#include "map/core/gui_map_provider_catalog.h"
#include "services/map_server_transport_factory.h"
#include "config/unit_profile_manager.h"
#include "config/shortcut_registry.h"
#include "config/map_server_client_configuration.h"
#include "services/http_health_monitor.h"
#include "map/render/map_render_diagnostics.h"
#include "projects/dvmdb_identity.h"

#include <aowis/model/serialization/model_codec.h>
#include <aowis/model/edit/model_edit_transaction.h>

#include <QDir>
#include <QStandardPaths>
#include <QStatusBar>
#include <QDebug>
#include <QPushButton>
#include <QShortcut>
#include <qapplication.h>

#include <limits>
#include <functional>
#include <optional>
#include <utility>

MainWindow::~MainWindow()
{
    this->hydraulic_data->setLogicalEditCaptureEnabled(false);
    if (this->personal_edit_guard != nullptr)
        this->personal_edit_guard->stop();
    if (this->personal_edit_session != nullptr)
        stopPersonalDraftEditing();
    // The tile and terrain repositories, GPS provider and hydraulic model
    // were created before the navigation page. QObject's default child
    // destruction order would dispose of those dependencies first, while
    // the shared bgfx viewport is still alive inside the navigation page.
    // Destroy the workspace first, while every service it uses still exists.
    if (MapRenderDiagnostics::enabled())
        qInfo() << "AOWIS map shutdown: workspace teardown starting";
    delete this->map_workspace;
    this->map_workspace = nullptr;
    if (MapRenderDiagnostics::enabled())
        qInfo() << "AOWIS map shutdown: workspace teardown completed";
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
    hydraulic_data(new HydraulicData(this)),
    gps( new GpsProvider(this) ),
    dock_entity_inspector( new EntityInspectorDock(hydraulic_data, this) ),
    dock_entity_map_legend( new EntityMapLegendDock(hydraulic_data, this)),
    dock_map_editor_guide( new MapEditorGuideDock(this) ),
    top_control_bar( new TopControlBar(this) ),
    map_tile_repository( new MapTileRepository(guiMapServerTransport(), this) ),
    map_terrain_repository( new MapTerrainRepository(guiMapServerTransport(), this) ),
    map_model_monitor( new MapModel(&mapProviderCatalog(), this) ),
    settings( new SettingsWidget(this) ),
    map_monitor( new MapMonitorContainer(this->map_model_monitor, this->map_tile_repository,
                                         this->map_terrain_repository, this->hydraulic_data,
                                         this->gps, this) ),
    map_editor( new MapEditorContainer(this->map_model_monitor, this->map_tile_repository,
                                       this->map_terrain_repository, this->hydraulic_data,
                                       this->gps, this->dock_entity_inspector, this->map_monitor, this) ),
    energy( new EnergyWidget(this) ),
    hydraulic_libraries( new HydraulicLibrariesWidget(this->hydraulic_data, this) ),
    hydraulic_controls( new HydraulicControlsWidget(this->hydraulic_data, this) ),
    reservoirs( new ReservoirsWidget(this->hydraulic_data, this) ),
    tanks( new TanksWidget(this->hydraulic_data, this) ),
    pumps( new PumpsWidget(this->hydraulic_data, this) ),
    valves( new ValvesWidget(this->hydraulic_data, this) ),
    junctions( new JunctionsWidget(this->hydraulic_data, this) ),
    pipes( new PipesWidget(this->hydraulic_data, this) ),
    demand_points( new DemandPointsWidget(this->hydraulic_data, this) ),
    customers( new CustomersWidget(this) ),
    logs( new LogsWidget(this) ),
    alarms( new AlarmsWidget(this) ),
    simulation_manager( new SimulationManager(hydraulic_data, this) )
{
    this->main_navigation = new MainNavigationWidget(this);
    this->projects_workspace = new ProjectsWidget(this->main_navigation);
    this->map_workspace = new MapWorkspaceWidget(this->map_editor, this->map_monitor,
                                                this->main_navigation);

    #ifdef AOWIS_STANDALONE
    setWindowTitle(tr("AOWIS Controller [Standalone]"));
    #else
    setWindowTitle(tr("AOWIS Controller"));
    #endif
    
    #ifdef Q_OS_LINUX
    showMaximized();
    #endif
    
    QLocale layout = qApp->inputMethod()->locale();
    qDebug() << layout.name();   // e.g. "en_US", "de_DE"
    
    
    //this->gps->start();
    #ifdef Q_OS_LINUX
    this->gps->startGpsd("127.0.0.1");
    #endif
    
    addDockWidget(Qt::RightDockWidgetArea, this->dock_entity_inspector);
    addDockWidget(Qt::RightDockWidgetArea, this->dock_entity_map_legend);
    addDockWidget(Qt::RightDockWidgetArea, this->dock_map_editor_guide);
    this->history_dock = new HistoryDock(this);
    addDockWidget(Qt::LeftDockWidgetArea, this->history_dock);
    this->history_dock->hide();
    connect(this->history_dock, &HistoryDock::undoRequested,
            this, [this] { this->personal_edit_session->navigateHistory(true); });
    connect(this->history_dock, &HistoryDock::redoRequested,
            this, [this] { this->personal_edit_session->navigateHistory(false); });
    connect(this->history_dock, &HistoryDock::jumpRequested,
            this, [this](const QJsonObject &coordinate)
    {
        if (this->personal_edit_active && !this->personal_edit_blocked &&
            this->personal_edit_session != nullptr)
            this->personal_edit_session->navigateHistoryTo(coordinate);
    });
    connect(this->history_dock, &HistoryDock::branchHistoryPageRequested,
            this, [this](const QString &after_revision)
    {
        if (!this->personal_edit_active || this->personal_edit_session == nullptr ||
            this->opened_branch_name.isEmpty())
        {
            this->history_dock->failBranchHistoryPage(
                after_revision, tr("Open a private branch to read committed History."));
            return;
        }
        const QString branch = this->opened_branch_name;
        const QString scope = this->personal_edit_session->historyScopeKey() +
            QStringLiteral(":") + branch;
        this->personal_edit_session->branchHistoryPage(branch, after_revision,
            [this, scope, after_revision](const QList<QJsonObject> &data,
                                          const QString &next_cursor, const QString &error)
        {
            if (!this->personal_edit_active || this->personal_edit_session == nullptr ||
                this->personal_edit_session->historyScopeKey() +
                    QStringLiteral(":") + this->opened_branch_name != scope)
                return;
            if (!error.isEmpty())
            {
                this->history_dock->failBranchHistoryPage(after_revision, error);
                return;
            }
            QList<HistoryDockEntry> entries;
            for (const QJsonObject &commit : data)
            {
                HistoryDockEntry entry;
                entry.kind = HistoryDockEntry::Kind::Commit;
                entry.identity = commit.value(QStringLiteral("id")).toString();
                entry.first_parent = commit.value(QStringLiteral("firstParent")).toString();
                entry.title = commit.value(QStringLiteral("message")).toString().simplified();
                if (entry.title.isEmpty())
                    entry.localized_title = [id = entry.identity] { return HistoryDock::tr("Commit %1").arg(id.left(8)); };
                entry.localized_details = [id = entry.identity, snapshot = commit.value(QStringLiteral("checkpointOnly")).toBool()] {
                    QString text = HistoryDock::tr("Immutable revision: %1").arg(id);
                    if (snapshot)
                        text += HistoryDock::tr("\nSnapshot-only checkpoint: no recorded MODEL actions.");
                    return text;
                };
                entries.append(entry);
            }
            this->history_dock->appendBranchHistoryPage(
                after_revision, entries, next_cursor);
        });
    });
    connect(this->history_dock, &HistoryDock::archivedActionPageRequested,
            this, [this](const QString &revision, int offset)
    {
        if (!this->personal_edit_active || this->personal_edit_session == nullptr ||
            this->opened_branch_name.isEmpty())
        {
            this->history_dock->failArchivedActionPage(
                revision, tr("Open a private branch to read archived actions."));
            return;
        }
        const QString branch = this->opened_branch_name;
        const QString scope = this->personal_edit_session->historyScopeKey() +
            QStringLiteral(":") + branch;
        this->personal_edit_session->archivedActionPage(branch, revision, offset,
            [this, scope, revision, offset](const QList<QJsonObject> &data,
                                             int next_offset, int total_count,
                                             const QString &error)
        {
            // A delayed response may belong to the previous private draft or
            // branch. Never render it in the currently opened History dock.
            if (!this->personal_edit_active || this->personal_edit_session == nullptr ||
                this->personal_edit_session->historyScopeKey() +
                    QStringLiteral(":") + this->opened_branch_name != scope)
                return;
            if (!error.isEmpty())
            {
                this->history_dock->failArchivedActionPage(revision, error);
                return;
            }
            QList<HistoryDockEntry> entries;
            for (const QJsonObject &action : data)
            {
                HistoryDockEntry entry;
                entry.kind = HistoryDockEntry::Kind::ArchivedAction;
                entry.identity = action.value(QStringLiteral("id")).toString();
                entry.title = action.value(QStringLiteral("label")).toString();
                const QString operation = action.value(QStringLiteral("operation")).toString();
                const QJsonValue edits = action.value(QStringLiteral("editCount"));
                entry.localized_details = [id = entry.identity, operation, edits] {
                    QString text = HistoryDock::tr("Original archived MODEL action: %1").arg(id);
                    if (!operation.isEmpty()) text += HistoryDock::tr("\nOperation: %1").arg(operation);
                    if (edits.isDouble()) text += HistoryDock::tr("\n%1 entity edit(s)").arg(edits.toInt());
                    return text;
                };
                entries.append(entry);
            }
            this->history_dock->appendArchivedActionPage(
                revision, offset, entries, next_offset, total_count);
        });
    });
    addToolBar(Qt::TopToolBarArea, this->top_control_bar);

    const QList<QDockWidget *> right_docks = findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QDockWidget *dock : right_docks)
    {
        connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool visible)
        {
            onRightDockVisibilityChanged(dock, visible);
            scheduleRightDockResize();
        });
        connect(dock, &QDockWidget::topLevelChanged, this, [this](bool)
        {
            scheduleRightDockResize();
        });
        connect(dock, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea)
        {
            scheduleRightDockResize();
        });
    }

    connect(this->dock_entity_map_legend, &EntityMapLegendDock::signalDockHeightPreferredChanged, this, [this](int)
    {
        scheduleRightDockResize();
    });

    this->shortcut_toggle_right_docks = new QShortcut(
        guiShortcutRegistry().keySequence(GuiShortcutId::SidebarToggle), this);
    this->shortcut_toggle_right_docks->setContext(Qt::WindowShortcut);
    this->shortcut_toggle_right_docks->setAutoRepeat(false);
    connect(this->shortcut_toggle_right_docks, &QShortcut::activated,
            this, &MainWindow::toggleRightDockArea);
    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged,
            this, [this](GuiShortcutId id)
    {
        if (id == GuiShortcutId::SidebarToggle)
            this->shortcut_toggle_right_docks->setKey(
                guiShortcutRegistry().keySequence(GuiShortcutId::SidebarToggle));
    });
    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutCaptureActiveChanged,
            this, [this](bool active)
    {
        this->shortcut_toggle_right_docks->setEnabled(!active);
    });
    
    this->dock_entity_map_legend->setVisible(false);
    this->dock_map_editor_guide->setVisible(false);
    scheduleRightDockResize();
    connect(this->map_monitor, &MapMonitorContainer::signalShowMapLegendLink, this, [this](VisualLink visual_link)
    {
        if (visual_link != VisualLink::None && this->right_dock_area_hidden
            && this->main_navigation->currentWidget() == this->map_workspace
            && this->map_workspace->isDistributionMonitorActive())
            toggleRightDockArea();

        this->dock_entity_map_legend->showMapLegendLink(visual_link);
    });
    connect(this->map_monitor, &MapMonitorContainer::signalShowMapLegendNode, this, [this](VisualNode visual_node)
    {
        if (visual_node != VisualNode::None && this->right_dock_area_hidden
            && this->main_navigation->currentWidget() == this->map_workspace
            && this->map_workspace->isDistributionMonitorActive())
            toggleRightDockArea();

        this->dock_entity_map_legend->showMapLegendNode(visual_node);
    });
    connect(this->map_monitor, &MapMonitorContainer::signalShowMapLegendHeatmap, this, [this](VisualHeatmap visual_heatmap)
    {
        if (visual_heatmap != VisualHeatmap::None && this->right_dock_area_hidden
            && this->main_navigation->currentWidget() == this->map_workspace
            && this->map_workspace->isDistributionMonitorActive())
            toggleRightDockArea();

        this->dock_entity_map_legend->showMapLegendHeatmap(visual_heatmap);
    });
    connect(this->dock_entity_map_legend, &EntityMapLegendDock::signalNodePaletteSelected,
            this->map_monitor, &MapMonitorContainer::setNodePalette);
    connect(this->dock_entity_map_legend, &EntityMapLegendDock::signalLinkPaletteSelected,
            this->map_monitor, &MapMonitorContainer::setLinkPalette);
    connect(this->dock_entity_map_legend, &EntityMapLegendDock::signalHeatmapPaletteSelected,
            this->map_monitor, &MapMonitorContainer::setHeatmapPalette);
    connect(this->map_monitor, &MapMonitorContainer::signalNodePaletteChanged,
            this->dock_entity_map_legend, &EntityMapLegendDock::setNodePalette);
    connect(this->map_monitor, &MapMonitorContainer::signalLinkPaletteChanged,
            this->dock_entity_map_legend, &EntityMapLegendDock::setLinkPalette);
    connect(this->map_monitor, &MapMonitorContainer::signalHeatmapPaletteChanged,
            this->dock_entity_map_legend, &EntityMapLegendDock::setHeatmapPalette);
    connect(this->map_editor, &MapEditorContainer::signalMapEditorGuideVisibilityChanged, this, [this](bool visible)
    {
        if (visible && this->right_dock_area_hidden)
        {
            // Restore first so the guide is not immediately suppressed as a shortcut-hidden dock.
            toggleRightDockArea();
        }

        this->dock_map_editor_guide->setRequestedVisible(visible);
    });
    connect(this->map_editor, &MapEditorContainer::signalEditNetworkSectionActive, this->dock_map_editor_guide, &MapEditorGuideDock::setEditNetworkSectionActive);
    connect(this->dock_map_editor_guide, &MapEditorGuideDock::requestedVisibilityChanged, this->map_editor, &MapEditorContainer::setMapEditorGuideChecked);
    this->dock_map_editor_guide->setEditNetworkSectionActive(this->map_editor->isEditNetworkSectionActive());
    
    /*
    setStyleSheet(
        "QMainWindow::separator"
        "{"
        "    width: 0px;"
        "    height: 0px;"
        "}"
    );
    */
    
    this->map_mon = this->map_monitor->getMap();
    
    this->main_navigation->setContentsMargins(0, 0, 0, 0);
    
    setCentralWidget(this->main_navigation);
    
    this->footer = new FooterStatusBar(this);
    setStatusBar(this->footer->statusBar());
    
    // A private draft can be edited only through the fenced session. The status
    // bar offers an explicit way to end editing without discarding queued edits.
    this->personal_edit_session = new PersonalDraftEditSession(this);
    // The mode tabs remain interactive. An archived History selection is
    // navigable but not editable: reject attempts to re-enter editor tools
    // before an authoritative draft cursor has restored capture permission.
    connect(this->map_workspace, &MapWorkspaceWidget::currentModeChanged,
            this, [this](int mode)
    {
        if (mode != MapWorkspaceWidget::DistributionEditor ||
            !this->personal_edit_active || this->personal_edit_blocked ||
            this->personal_edit_session->canCaptureHistoryEdits())
            return;
        QTimer::singleShot(0, this, [this]
        {
            if (this->personal_edit_active && !this->personal_edit_blocked &&
                !this->personal_edit_session->canCaptureHistoryEdits() &&
                this->map_workspace->isDistributionEditorActive())
                this->map_workspace->setCurrentMode(MapWorkspaceWidget::DistributionMonitor);
        });
    });
    this->personal_edit_guard = new QTimer(this);
    this->personal_edit_guard->setInterval(250);
    this->personal_edit_stop_button = new QPushButton(tr("Stop private editing"), this);
    this->personal_edit_stop_button->setObjectName(QStringLiteral("personalDraftStopEditing"));
    this->personal_edit_stop_button->hide();
    statusBar()->addPermanentWidget(this->personal_edit_stop_button);
    connect(this->personal_edit_stop_button, &QPushButton::clicked,
            this, &MainWindow::stopPersonalDraftEditing);
    connect(this->personal_edit_guard, &QTimer::timeout,
            this, &MainWindow::checkPersonalDraftModel);
    connect(this->hydraulic_data, &HydraulicData::signalLogicalEditRecorded,
            this, [this](const QByteArray &transaction)
    {
        if (!this->personal_edit_active || !this->hydraulic_data->logicalEditCaptureEnabled())
            return;
        QString error;
        if (!this->personal_edit_session->appendTransaction(transaction, &error))
        {
            blockPersonalDraftEditing(tr("The edit could not be durably queued: %1").arg(error));
            return;
        }
        // Check immediately after staging; the model and staged candidate must
        // describe exactly the same canonical network.
        checkPersonalDraftModel();
    });
    connect(this->hydraulic_data, &HydraulicData::signalLogicalEditCaptureFailed,
            this, [this](const QString &reason)
    {
        if (this->personal_edit_active)
            blockPersonalDraftEditing(tr("Hydraulic transaction capture failed: %1").arg(reason));
    });
    connect(this->personal_edit_session, &PersonalDraftEditSession::ready, this, [this]
    {
        // Replay may include locally queued transactions not yet on the server.
        // Never load the raw HTTP snapshot into an editable workspace.
        this->hydraulic_data->replaceNetworkHydraulic(
            this->personal_edit_session->visibleNetwork(), {});
        this->immutable_network_read_only = false;
        // Keep the Distribution Editor active while browsing History.
        // Logical edit capture is enabled only after cursor verification.
        this->map_workspace->setCurrentMode(MapWorkspaceWidget::DistributionEditor);
        this->main_navigation->setCurrentIndex(this->map_workspace_page_index);
        this->hydraulic_data->setLogicalEditCaptureEnabled(false);
        this->personal_edit_last_checked_revision = std::numeric_limits<quint64>::max();
        this->personal_edit_guard->start();
        this->personal_edit_stop_button->setEnabled(true);
        checkPersonalDraftModel();
        if (this->network_open_pending)
        {
            this->opened_project_id = this->pending_open_project_id;
            this->opened_branch_id = this->pending_open_branch_id;
            this->opened_project_name = this->pending_open_project_name;
            this->opened_branch_name = this->pending_open_branch_name;
            this->top_control_bar->setOpenedContext(this->pending_open_project_name,
                                                    this->pending_open_branch_name);
            restoreOpenedNetworkSelection();
        }
        this->network_open_pending = false;
        refreshHistoryDock();
        this->top_control_bar->setNetworkStatus(this->personal_edit_session->pendingCount() > 0
            ? QT_TRANSLATE_NOOP("LiveUiText", "Saving · %1 pending") : QT_TRANSLATE_NOOP("LiveUiText", "Autosaved"), QString::number(this->personal_edit_session->pendingCount()));
        statusBar()->showMessage(tr("Private draft editing active; changes are queued and saved to your DVMDB."), 10000);
    });
    connect(this->personal_edit_session, &PersonalDraftEditSession::pendingCountChanged,
            this, [this](int count)
    {
        refreshHistoryDock();
        if (this->personal_edit_active && !this->personal_edit_blocked)
        {
            this->personal_edit_stop_button->setText(tr("Stop private editing (%1 pending)").arg(count));
            this->top_control_bar->setNetworkStatus(count > 0
            ? QT_TRANSLATE_NOOP("LiveUiText", "Saving · %1 pending") : QT_TRANSLATE_NOOP("LiveUiText", "Autosaved"), QString::number(count));
        }
    });
    connect(this->personal_edit_session, &PersonalDraftEditSession::saveConfirmed,
            this, [this](const QString &, bool)
    {
        refreshHistoryDock();
        if (this->personal_edit_active && !this->personal_edit_blocked)
        {
            this->top_control_bar->setNetworkStatus(this->personal_edit_session->pendingCount() > 0
            ? QT_TRANSLATE_NOOP("LiveUiText", "Saving · %1 pending") : QT_TRANSLATE_NOOP("LiveUiText", "Autosaved"), QString::number(this->personal_edit_session->pendingCount()));
            statusBar()->showMessage(tr("Private draft saved to DVMDB."), 5000);
        }
    });
    connect(this->personal_edit_session, &PersonalDraftEditSession::historyCursorChanged,
            this, [this]
    {
        synchronizePersonalHistoryEditMode();
        refreshHistoryDock();
    });
    connect(this->personal_edit_session, &PersonalDraftEditSession::historyNavigationStarted,
            this, [this]
    {
        this->hydraulic_data->setLogicalEditCaptureEnabled(false);
        this->personal_edit_guard->stop();
        this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Navigating History…"));
        refreshHistoryDock();
    });
    connect(this->personal_edit_session, &PersonalDraftEditSession::historyNavigationRecovering,
            this, [this]
    {
        this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Reconnecting History…"));
        statusBar()->showMessage(tr("The History reply is uncertain. Editing is paused while "
                                    "AOWIS retries the same navigation."));
        refreshHistoryDock();
    });
    connect(this->personal_edit_session, &PersonalDraftEditSession::historyNavigationCompleted,
            this, [this](const NetworkHydraulic &network)
    {
        if (!this->personal_edit_active || this->personal_edit_blocked) return;
        this->hydraulic_data->replaceNetworkHydraulic(network, {});
        // Rehydration is not edit permission. Wait for the authoritative cursor
        // before restoring capture or the Distribution Editor.
        this->hydraulic_data->setLogicalEditCaptureEnabled(false);
        this->personal_edit_last_checked_revision = std::numeric_limits<quint64>::max();
        this->personal_edit_guard->start();
        checkPersonalDraftModel();
        if (!this->personal_edit_blocked)
        {
            this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "History position saved"));
            statusBar()->showMessage(tr("Editor updated from verified incremental DVMDB History state."), 5000);
        }
        refreshHistoryDock();
    });
    connect(this->personal_edit_session, &PersonalDraftEditSession::blocked,
            this, &MainWindow::blockPersonalDraftEditing);
    connect(this->personal_edit_session, &PersonalDraftEditSession::stopped,
            this, [this](const QString &error)
    {
        this->personal_edit_active = false;
        refreshHistoryDock();
        this->personal_edit_blocked = false;
        this->personal_edit_closing = false;
        this->personal_edit_stop_button->hide();
        this->personal_edit_stop_button->setText(tr("Stop private editing"));
        if (this->network_open_pending)
        {
            if (!error.isEmpty())
            {
                this->network_open_pending = false;
                this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Open failed · Read-only"));
                statusBar()->showMessage(tr("Cannot switch: %1").arg(error), 15000);
                restoreOpenedNetworkSelection();
            }
            else
                this->projects_workspace->prepareNetworkOpen(this->pending_open_project_id,
                                                               this->pending_open_branch_id);
        }
        else
        {
            this->top_control_bar->setNetworkStatus(error.isEmpty()
                ? QT_TRANSLATE_NOOP("LiveUiText", "Read-only · Not editing") : QT_TRANSLATE_NOOP("LiveUiText", "Save failed"));
            if (!error.isEmpty())
                QMessageBox::warning(this, tr("Private writer lease release"), error);
        }
    });

    // don't make it smaller, because that could lead to positioning issues on the canvas
    this->setMinimumHeight(700);
    this->setMinimumWidth(1222);
    
    this->main_navigation->addPage(this->projects_workspace,
                                  QIcon(":/icon/projects.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Servers and projects"));
    this->main_navigation->addPage(new QWidget(this->main_navigation),
                                  QIcon(":/icon/dashboard_global.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Dashboard"));

    //this->main_navigation->addPage(new QWidget(this->main_navigation),
    //                              QIcon(":/icon/site_switcher.png"),
    //                              "Site Switcher");

    //this->main_navigation->addPage(new QWidget(this->main_navigation),
    //                              QIcon(":/icon/dashboard.png"),
    //                              "Site Dashboard");

    this->map_workspace_page_index = this->main_navigation->addPage(this->map_workspace,
                                  QIcon(":/icon/map.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Maps"));
    this->main_navigation->addPage(this->energy,
                                  QIcon(":/icon/energy.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Energy"));
    this->main_navigation->addPage(this->hydraulic_libraries,
                                  QIcon(":/icon/hydraulic_library.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Hydraulic Libraries"));
    this->main_navigation->addPage(this->hydraulic_controls,
                                  QIcon(":/icon/inspector_quality.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Controls & Map Labels"));
    this->main_navigation->addPage(this->reservoirs,
                                  QIcon(":/icon/reservoir.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Reservoirs"));
    this->main_navigation->addPage(this->tanks,
                                  QIcon(":/icon/tower.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Tanks"));
    this->main_navigation->addPage(this->pumps,
                                  QIcon(":/icon/pump.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Pumps"));
    this->main_navigation->addPage(this->valves,
                                  QIcon(":/icon/valve.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Valves"));
    this->main_navigation->addPage(this->junctions,
                                  QIcon(":/icon/junction.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Junctions"));
    this->main_navigation->addPage(this->pipes,
                                  QIcon(":/icon/pipe.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Pipes"));
    this->main_navigation->addPage(this->demand_points,
                                  QIcon(":/icon/demand_point.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Demand Points"));
    this->main_navigation->addPage(this->customers,
                                  QIcon(":/icon/users.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Customers"));

    this->main_navigation->addPage(this->alarms,
                                  QIcon(":/icon/alarm.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Alarms"),
                                  MainNavigationWidget::Placement::Bottom);
    this->main_navigation->addPage(this->logs,
                                  QIcon(":/icon/log.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Logs"),
                                  MainNavigationWidget::Placement::Bottom);
    this->settings_page_index = this->main_navigation->addPage(
                                  this->settings,
                                  QIcon(":/icon/settings.png"),
                                  QT_TRANSLATE_NOOP("MainNavigationWidget", "Settings"),
                                  MainNavigationWidget::Placement::Bottom);

    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::editShortcutRequested,
            this, [this](GuiShortcutId id)
    {
        if (this->settings_page_index < 0)
            return;

        this->main_navigation->setCurrentIndex(this->settings_page_index);
        this->settings->focusShortcut(id);
    });

    connect(this->top_control_bar, &TopControlBar::signalManageUnitsRequested, this, [this]
    {
        if (this->settings_page_index < 0)
            return;
        this->main_navigation->setCurrentIndex(this->settings_page_index);
        this->settings->focusUnits();
    });

    connect(this->map_workspace, &MapWorkspaceWidget::currentModeChanged,
            this, [this](int mode)
    {
        // Once a fenced session is blocked, do not let the shared UI switch
        // straight back into its editor while the working copy is unverified.
        if ((this->personal_edit_active && this->personal_edit_blocked
             && mode == MapWorkspaceWidget::DistributionEditor)
            || (this->immutable_network_read_only
                && mode == MapWorkspaceWidget::DistributionEditor))
            this->map_workspace->setCurrentMode(MapWorkspaceWidget::DistributionMonitor);
    });
    connect(this->projects_workspace, &ProjectsWidget::draftNetworkPreviewRequested,
            this, [this](NetworkHydraulic network, const QString &draft_id,
                         const QString &generation)
    {
        if (this->personal_edit_active)
        {
            QMessageBox::warning(this, tr("Private draft editing active"),
                tr("Stop the current editing session before replacing its hydraulic model."));
            return;
        }
        const QMessageBox::StandardButton decision = QMessageBox::question(this,
            tr("View private draft network"),
            tr("Replace the current in-memory hydraulic model with the verified private draft "
               "%1 (generation %2)?\n\nAny unsaved changes in the current model will be lost. "
               "The Distribution Monitor can display this network, but editing and autosave "
               "are NOT bound to the draft yet.")
                .arg(draft_id, generation), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (decision != QMessageBox::Yes) return;
        this->hydraulic_data->replaceNetworkHydraulic(std::move(network), {});
        this->immutable_network_read_only = true;
        this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Read-only"));
        this->map_workspace->setCurrentMode(MapWorkspaceWidget::DistributionMonitor);
        this->main_navigation->setCurrentIndex(this->map_workspace_page_index);
    });
    connect(this->projects_workspace, &ProjectsWidget::immutableNetworkPreviewRequested,
            this, [this](NetworkHydraulic network, const QString &revision) {
        if (this->personal_edit_active) {
            QMessageBox::warning(this, tr("Private draft editing active"),
                tr("Stop your current editing session before opening a revision snapshot."));
            return;
        }
        // The immutable network is shown only in Distribution Monitor; no
        // draft lease, editing capture or repository write is created.
        QMessageBox *question = new QMessageBox(QMessageBox::Question,
            tr("Open immutable revision"),
            tr("Load revision %1 into Distribution Monitor in read-only mode? "
               "This replaces the current in-memory network.").arg(revision),
            QMessageBox::Yes | QMessageBox::Cancel, this);
        question->setAttribute(Qt::WA_DeleteOnClose);
        question->setDefaultButton(QMessageBox::Cancel);
        connect(question, &QMessageBox::finished, this,
                [this, question, network, revision](int result) mutable {
            if (result != QMessageBox::Yes || this->personal_edit_active) return;
            this->hydraulic_data->replaceNetworkHydraulic(std::move(network), {});
            this->immutable_network_read_only = true;
            this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Read-only · Revision %1"), revision);
            this->map_workspace->setCurrentMode(MapWorkspaceWidget::DistributionMonitor);
            this->main_navigation->setCurrentIndex(this->map_workspace_page_index);
        });
        question->open();
    });
    connect(this->projects_workspace, &ProjectsWidget::selectedNetworkExportRequested,
            this->simulation_manager, &SimulationManager::exportSelectedEpanetNetwork);
    connect(this->projects_workspace, &ProjectsWidget::selectedHeadImportFileRequested,
            this->simulation_manager, &SimulationManager::importNetworkProjectForHead);
    connect(this->simulation_manager, &SimulationManager::signalPrivateHeadNetworkImported,
            this->projects_workspace, &ProjectsWidget::importNetworkIntoSelectedHead);
    connect(this->projects_workspace, &ProjectsWidget::draftNetworkEditRequested,
            this, [this](const QString &draft_id, const QString &network_id)
    {
        if (this->personal_edit_active)
        {
            this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Save failed · Previous editing session still active"));
            this->network_open_pending = false;
            restoreOpenedNetworkSelection();
            return;
        }
        const PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
        if ((target.kind != PersonalRepositoryTarget::Kind::Http &&
             target.kind != PersonalRepositoryTarget::Kind::Embedded) ||
            !DvmdbIdentity::isUuidV7(draft_id) || !DvmdbIdentity::isUuidV7(network_id))
        {
            this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Cannot open · Personal database or UUIDv7 missing"));
            this->network_open_pending = false;
            restoreOpenedNetworkSelection();
            return;
        }
        const QString directory = PersonalEditQueue::defaultRoot();
        if (directory.isEmpty())
        {
            this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Cannot open · Durable edit queue unavailable"));
            this->network_open_pending = false;
            restoreOpenedNetworkSelection();
            return;
        }
        this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Opening network…"));
        this->hydraulic_data->setLogicalEditCaptureEnabled(false);
        this->personal_edit_active = true;
        this->personal_edit_blocked = false;
        this->personal_edit_closing = false;
        this->personal_edit_stop_button->setEnabled(false);
        this->personal_edit_stop_button->setText(tr("Opening private draft…"));
        this->personal_edit_stop_button->show();
        this->personal_edit_session->start(target, draft_id, network_id, directory);
    });
    connect(this->projects_workspace, &ProjectsWidget::networkOpenProgress,
            this, [this](const QString &message, bool failed)
    {
        this->top_control_bar->setNetworkStatus(failed ? QT_TRANSLATE_NOOP("LiveUiText", "Open failed") : QT_TRANSLATE_NOOP("LiveUiText", "Opening…"));
        statusBar()->showMessage(message, failed ? 15000 : 5000);
        if (failed)
        {
            this->network_open_pending = false;
            restoreOpenedNetworkSelection();
        }
    });
    connect(this->projects_workspace, &ProjectsWidget::networkOpenRequested,
            this, [this](const QString &project_id, const QString &branch_id,
                         const QString &project_name, const QString &branch_name,
                         const QString &working_name, bool remote_checkout)
    {
        if (this->network_open_pending || this->network_confirmation_visible) return;
        if (this->personal_edit_active && !this->personal_edit_blocked
            && project_id == this->opened_project_id
            && branch_id == this->opened_branch_id)
            return; // Already editing this branch; never reload it.
        this->network_confirmation_visible = true;
        const int queued_edits = this->personal_edit_active
            ? this->personal_edit_session->pendingCount() : 0;
        const QString pending_warning = queued_edits > 0
            ? tr("\n\nThe current draft has %1 locally queued edit(s). They remain "
                 "durable on disk, but may not yet be saved to DVMDB. Reopening that "
                 "draft recovers its queue.").arg(queued_edits)
            : QString();
        QMessageBox *confirmation = new QMessageBox(QMessageBox::Question,
            tr("Open hydraulic network"),
            tr("Project: %1\nBranch: %2\nWorking database: %3\n\n"
               "Opening this network replaces the network currently displayed in the editor. "
               "Changes will be automatically saved to your personal working database.%4%5")
                .arg(project_name, branch_name, working_name,
                     remote_checkout
                         ? tr("\n\nThe remote branch will first be checked out into %1.")
                               .arg(working_name)
                         : QString(), pending_warning),
            QMessageBox::NoButton, this);
        confirmation->setAttribute(Qt::WA_DeleteOnClose);
        QPushButton *open_button = confirmation->addButton(tr("Open network"), QMessageBox::AcceptRole);
        QPushButton *cancel_button = confirmation->addButton(tr("Cancel"), QMessageBox::RejectRole);
        confirmation->setDefaultButton(cancel_button);
        confirmation->setEscapeButton(cancel_button);
        connect(confirmation, &QMessageBox::finished, this,
            [this, confirmation, open_button, project_id, branch_id,
             project_name, branch_name](int)
        {
            this->network_confirmation_visible = false;
            if (confirmation->clickedButton() != open_button)
            {
                restoreOpenedNetworkSelection();
                return;
            }
            this->pending_open_project_id = project_id;
            this->pending_open_branch_id = branch_id;
            this->pending_open_project_name = project_name;
            this->pending_open_branch_name = branch_name;
            this->network_open_pending = true;
            this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Opening…"));
            if (this->personal_edit_active)
                stopPersonalDraftEditing(); // Wait for stopped() before the next checkout.
            else
                this->projects_workspace->prepareNetworkOpen(project_id, branch_id);
        });
        confirmation->open();
    });
    connect(this->projects_workspace, &ProjectsWidget::projectContextChanged,
            this->top_control_bar, &TopControlBar::setProjectContext);
    connect(this->top_control_bar, &TopControlBar::signalProjectSelected,
            this->projects_workspace, &ProjectsWidget::selectToolbarProject);
    connect(this->top_control_bar, &TopControlBar::signalBranchSelected,
            this->projects_workspace, &ProjectsWidget::selectToolbarBranch);
    connect(this->top_control_bar, &TopControlBar::signalProjectHistoryRequested,
            this, [this]
    {
        // History is a dock in the *active* workspace, not a command to
        // leave the hydraulic editor or to select a different revision.
        refreshHistoryDock();
        this->history_dock->show();
        this->history_dock->raise();
    });
    connect(this->projects_workspace, &ProjectsWidget::projectContextChanged,
            this, [this](const QStringList &, const QString &, const QStringList &, const QString &)
    {
        refreshHistoryDock();
    });
    // Restore an optional bundled-preview selection; real sources load automatically.
    this->projects_workspace->selectProjectByName(QStringLiteral("Sample North Network"));
    this->main_navigation->setCurrentIndex(0);

    // Map fullscreen: requested by the map pages' HUD button / shortcut;
    // another page (shortcut) ends it.
    connect(this->map_monitor, &MapMonitorContainer::signalMapFullscreenRequested,
            this, &MainWindow::setMapFullscreen);
    connect(this->map_editor, &MapEditorContainer::signalMapFullscreenRequested,
            this, &MainWindow::setMapFullscreen);
    connect(this->main_navigation, &MainNavigationWidget::currentChanged, this, [this](int)
    {
        if (this->map_fullscreen_active)
            setMapFullscreen(false);
        updateMapEdgePanning();
        updateActiveMapPanels();
    });
    connect(this->map_workspace, &MapWorkspaceWidget::currentModeChanged,
            this, [this](int)
    {
        // All five modes retain the same map, including in fullscreen.
        if (this->map_fullscreen_active)
            applyMapFullscreenToMode();
        updateActiveMapPanels();
        updateMapEdgePanning();
    });
    updateActiveMapPanels();
    
    connect(this->map_mon, &MapWidget::signalZoomLevelChanged, this->footer, &FooterStatusBar::setMapZoom);
    connect(this->map_mon, &MapWidget::signalCoordsChangedWgs84, this->footer, &FooterStatusBar::setMapCoordinatesWGS84);
    connect(this->map_mon, &MapWidget::signalCoordsChangedUTM, this->footer, &FooterStatusBar::setMapCoordinatesUTM);
    connect(this->map_mon, &MapWidget::signalCoordsUnavailable, this->footer, &FooterStatusBar::clearMapCoordinates);
    
    // Editor and monitor use this same MapWidget and its footer connections.

    // Writes footer zoom edits to the shared camera in any Maps mode.
    connect(this->footer, &FooterStatusBar::zoomEdited, this, [this](double zoom)
    {
        if (this->main_navigation->currentWidget() != this->map_workspace)
            return;
        MapWidget *active_widget = this->map_workspace->activeMap();
        MapModel *active_model = this->map_workspace->activeModel();
        if (active_widget == nullptr || active_model == nullptr)
            return;
        if (active_model->viewMode() == MapViewMode::Globe)
            active_model->setViewGlobeZoomLevel(zoom, active_widget->size());
        else
            active_model->setView2dContinuousZoom(zoom, active_widget->size());
    });
    
    // The model (Render) sees the providers only through its directory
    // interface: forward the catalog's changes, and read the orbit setting
    // live so a changed value applies to the next drag.
    connect(&mapProviderCatalog(), &MapProviderCatalog::catalogChanged,
            this->map_model_monitor, &MapModel::providerCatalogChanged);
    // Provider URLs may change under stable ids: cached tiles of a changed
    // provider are dropped (the repository is Render's, without the catalog).
    connect(&mapProviderCatalog(), &MapProviderCatalog::catalogChanged,
            this->map_tile_repository, &MapTileRepository::invalidateProviderCatalog);
    connect(&mapProviderCatalog(), &MapProviderCatalog::fallbackChanged,
            this->map_model_monitor, &MapModel::providerFallbackChanged);
    const std::function<double()> orbit_sensitivity = []()
    {
        return guiConfiguration().map_navigation.orbit_3d_sensitivity;
    };
    this->map_model_monitor->setOrbitSensitivitySource(orbit_sensitivity);

    // Both distribution toolbars select providers on the same MapModel.
    // The model itself owns the 2D/3D camera-scale transfer in setViewMode().
    // No editor/monitor synchronization or per-view camera snapshots exist.
    connect(this->map_model_monitor, &MapModel::providerChanged,
            this->map_editor->mapNavigationWidget(), &MapNavigationWidget::mapProviderChange);
    connect(this->map_model_monitor, &MapModel::providerChanged,
            this->map_monitor->mapNavigationWidget(), &MapNavigationWidget::mapProviderChange);

    connect(this->top_control_bar, &TopControlBar::signalHeadlossFormulaChanged, this->dock_entity_inspector, &EntityInspectorDock::onHeadlossFormulaChanged);
    connect(this->top_control_bar, &TopControlBar::signalHeadlossFormulaChanged, this, [this](HeadlossFormulas)
    {
        const HydraulicHeadlossFormula selected =
            this->top_control_bar->selectedSimulationHeadlossFormula();
        // The private draft must be fully ready to capture before an editor
        // setting may change. A blocked or still-opening draft must never
        // acquire an uncaptured in-memory mutation.
        if (this->personal_edit_active &&
            (this->personal_edit_blocked ||
             !this->hydraulic_data->logicalEditCaptureEnabled()))
        {
            QTimer::singleShot(0, this, [this]
            {
                this->top_control_bar->setSelectedSimulationHeadlossFormula(
                    this->hydraulic_data->networkHydraulic().options_hydraulic.headloss_formula);
            });
            statusBar()->showMessage(tr("The private draft is not ready for editing."), 8000);
            return;
        }
        this->hydraulic_data->setSimulationHeadlossFormula(selected);
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]
    {
        UnitProfileManager::instance().presentation().setNetworkSpecificGravity(
            this->hydraulic_data->networkHydraulic().options_hydraulic.specific_gravity);
        this->top_control_bar->setSelectedSimulationHeadlossFormula(
            this->hydraulic_data->networkHydraulic().options_hydraulic.headloss_formula);
    });
    connect(this->top_control_bar, &TopControlBar::signalSimulationStart, this, [this]
    {
        this->simulation_manager->runOrStop(
            this->top_control_bar->selectedSimulationQualityAnalyses());
    });
    connect(this->simulation_manager, &SimulationManager::signalSimulationStarted, this->top_control_bar, &TopControlBar::setSimulationRunRunningIcon);
    connect(this->simulation_manager, &SimulationManager::signalSimulationStopRequested, this->top_control_bar, &TopControlBar::setSimulationRunStoppingIcon);
    connect(this->simulation_manager, &SimulationManager::signalSimulationFinished, this, [this](bool cancelled)
    {
        if (cancelled)
            this->top_control_bar->setSimulationRunCancelledIcon();
    });
    connect(this->top_control_bar, &TopControlBar::signalShowSimulationStatistics, this->simulation_manager, &SimulationManager::showSimulationStatistics);
    connect(this->top_control_bar, &TopControlBar::signalShowEpanetLog, this->simulation_manager, &SimulationManager::showEpanetLog);
#ifdef Q_OS_WASM
    connect(
        this->projects_workspace,
        &ProjectsWidget::importProjectRequested,
        this->simulation_manager,
        &SimulationManager::importNetworkProject,
        Qt::DirectConnection);
#else
    connect(this->projects_workspace, &ProjectsWidget::importProjectRequested, this->simulation_manager, &SimulationManager::importNetworkProject);
#endif
    connect(
        this->projects_workspace,
        &ProjectsWidget::builtinExampleRequested,
        this->simulation_manager,
        &SimulationManager::importNetworkProjectResource);
    connect(this->top_control_bar, &TopControlBar::signalExportEpanetNetwork, this->simulation_manager, &SimulationManager::exportEpanetNetwork);
    connect(this->simulation_manager, &SimulationManager::signalEpanetNetworkImported, this, [this]
    {
        this->immutable_network_read_only = false;
        QList<WaterQualityAnalysisType> analyses;
        for (const WaterQualitySolverOptions &quality_options : this->hydraulic_data->simulationQualityRunOptions())
        {
            if (quality_options.analysis != WaterQualityAnalysisType::None
                && !analyses.contains(quality_options.analysis))
            {
                analyses.append(quality_options.analysis);
            }
        }
        this->top_control_bar->setSelectedSimulationQualityAnalyses(analyses);
    });
    connect(this->simulation_manager, &SimulationManager::signalEpanetNetworkImported,
            this->top_control_bar, &TopControlBar::signalShowNetworkOnMap);
    connect(this->top_control_bar, &TopControlBar::signalShowNetworkOnMap, this, [this]
    {
        if (!this->hydraulic_data->boundingBoxWgs84Valid())
            return;

        const CoordinateWGS84 &minimum = this->hydraulic_data->boundingBoxWgs84Minimum();
        const CoordinateWGS84 &maximum = this->hydraulic_data->boundingBoxWgs84Maximum();
        const double elevation_minimum_m = this->hydraulic_data->nodeElevationMMinimum();
        const double elevation_maximum_m = this->hydraulic_data->nodeElevationMMaximum();
        // One shared navigation model and viewport: fit only once.
        if (this->map_mon != nullptr)
            this->map_mon->fitViewToBounds(
                minimum, maximum, elevation_minimum_m, elevation_maximum_m);
    });
    connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged, this, [this](bool available)
    {
        this->top_control_bar->setSimulationResultsAvailable(available);

        const std::optional<HydraulicSimulationResultTimeline> &result_timeline =
            this->hydraulic_data->simulationResultTimeline();

        if (!result_timeline.has_value() || this->hydraulic_data->simulationDiagnosticsStale())
            this->top_control_bar->resetSimulationRunIcon();
        else
            this->top_control_bar->setSimulationRunResultIcon(result_timeline.value());

        if (!available || !result_timeline.has_value())
        {
            this->top_control_bar->clearSimulationResultTimeline();
            return;
        }

        this->top_control_bar->setSimulationResultTimeline(result_timeline.value());
    });
    connect(this->hydraulic_data, &HydraulicData::signalCurrentSimulationResultChanged,
            this->top_control_bar, &TopControlBar::setCurrentSimulationResultIndex);
    connect(this->top_control_bar, &TopControlBar::signalSimulationResultIndexSelected, this, [this](int result_index)
    {
        this->hydraulic_data->setCurrentSimulationResultIndex(result_index);
    });
    connect(this->simulation_manager, &SimulationManager::signalEpanetLogAvailabilityChanged,
            this->top_control_bar, &TopControlBar::setEpanetLogAvailable);
    connect(this->top_control_bar, &TopControlBar::signalFullScreenToggle, this, &MainWindow::fullScreenToggle);

    connect(this->hydraulic_data, &HydraulicData::signalSelectedTank, this, &MainWindow::showEntityInspectorForSelection);
    connect(this->hydraulic_data, &HydraulicData::signalSelectedReservoir, this, &MainWindow::showEntityInspectorForSelection);
    connect(this->hydraulic_data, &HydraulicData::signalSelectedJunction, this, &MainWindow::showEntityInspectorForSelection);
    connect(this->hydraulic_data, &HydraulicData::signalSelectedPipe, this, &MainWindow::showEntityInspectorForSelection);
    connect(this->hydraulic_data, &HydraulicData::signalSelectedPump, this, &MainWindow::showEntityInspectorForSelection);
    connect(this->hydraulic_data, &HydraulicData::signalSelectedValve, this, &MainWindow::showEntityInspectorForSelection);
    connect(this->hydraulic_data, &HydraulicData::signalSelectedDemandPoint, this, &MainWindow::showEntityInspectorForSelection);

#ifdef Q_OS_WASM
    emscripten_set_fullscreenchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, this, EM_TRUE, &MainWindow::fullScreenChangeCallback);

    EmscriptenFullscreenChangeEvent fullscreen_status = {};
    if (emscripten_get_fullscreen_status(&fullscreen_status) == EMSCRIPTEN_RESULT_SUCCESS)
    {
        this->browser_fullscreen_active = fullscreen_status.isFullscreen != 0;
        this->top_control_bar->setFullScreenState(this->browser_fullscreen_active);
        this->updateMapEdgePanning();
    }
#endif
    
    #ifdef AOWIS_STANDALONE
    #else    
    checkServerMapInit();
    #endif
}


void MainWindow::checkServerMapInit()
{
    this->footer->statusUpdateServerMap(StatusColorCode::Yellow);
    const MapServerClientConfiguration &configuration = mapServerClientConfiguration();
    QString base = configuration.base_url.trimmed();
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    this->map_health_monitor = new HttpHealthMonitor(this);
    this->map_health_monitor->start(QUrl(base + QStringLiteral("/status")),
        [](const QByteArray &body)
        {
            return body.contains("AOWIS map server");
        },
        [this](HttpHealthMonitor::State state, const QString &error)
        {
            this->footer->statusUpdateServerMap(state == HttpHealthMonitor::State::Healthy
                ? StatusColorCode::Green
                : state == HttpHealthMonitor::State::Offline
                    ? StatusColorCode::Red : StatusColorCode::Yellow);
            if (state == HttpHealthMonitor::State::Offline)
                qWarning() << "Map server health check failed:" << error;
        }, configuration.api_key, 4500);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    scheduleRightDockResize();
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
#ifndef Q_OS_WASM
    if (!guiShortcutRegistry().shortcutCaptureActive()
        && guiShortcutMatches(event, guiShortcutRegistry().shortcut(GuiShortcutId::Fullscreen)))
    {
        fullScreenToggle();
        event->accept();
        return;
    }
#endif

#ifdef Q_OS_WASM
    switch (event->key())
    {
    case Qt::Key_F5:
        QMessageBox *box = new QMessageBox(this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        
        box->setIcon(QMessageBox::Question);
        box->setWindowTitle("Reload page");
        box->setText("Do you really want to reload this page?<br>You might loose unsaved inputs.");
        box->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box->setDefaultButton(QMessageBox::No);
        
        connect(box, &QMessageBox::buttonClicked, this,
                [this, box, event](QAbstractButton *button)
                {
                    if (box->standardButton(button) == QMessageBox::Yes)
                    {
                        emscripten_run_script("window.location.reload();");
                    }
                });
        
        box->open();
        
        event->accept();
        return;
    }
#endif

    QWidget::keyPressEvent(event);
}

void MainWindow::refreshPersonalEditButtonText()
{
    if (this->personal_edit_stop_button == nullptr)
        return;
    if (!this->personal_edit_active)
        this->personal_edit_stop_button->setText(tr("Stop private editing"));
    else if (this->personal_edit_closing)
        this->personal_edit_stop_button->setText(tr("Closing private draft…"));
    else if (this->personal_edit_blocked)
        this->personal_edit_stop_button->setText(tr("Close blocked private draft"));
    else if (this->personal_edit_session == nullptr || !this->personal_edit_session->isReady())
        this->personal_edit_stop_button->setText(tr("Opening private draft…"));
    else
        this->personal_edit_stop_button->setText(tr("Stop private editing (%1 pending)")
            .arg(this->personal_edit_session->pendingCount()));
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
    {
#ifdef AOWIS_STANDALONE
        setWindowTitle(tr("AOWIS Controller [Standalone]"));
#else
        setWindowTitle(tr("AOWIS Controller"));
#endif
        refreshPersonalEditButtonText();
    }

#ifndef Q_OS_WASM
    if (event->type() == QEvent::WindowStateChange)
    {
        QTimer::singleShot(0, this, [this]
        {
            this->top_control_bar->setFullScreenState(this->isFullScreen());
            // Leaving full screen (window manager, the fullscreen shortcut)
            // ends map fullscreen too.
            if (this->map_fullscreen_active && !isFullScreen())
                setMapFullscreen(false);
            this->updateMapEdgePanning();
        });
    }
#endif
}

void MainWindow::setMapFullscreen(bool fullscreen)
{
    if (fullscreen == this->map_fullscreen_active)
        return;
    this->map_fullscreen_active = fullscreen;

    if (fullscreen)
    {
        this->map_fullscreen_toolbar_visible = this->top_control_bar->isVisible();
        this->map_fullscreen_status_bar_visible = this->footer->statusBar()->isVisible();
        this->top_control_bar->hide();
        this->footer->statusBar()->hide();
        this->main_navigation->setNavigationBarVisible(false);
        this->map_fullscreen_history_visible = this->history_dock->isVisible();
        this->history_dock->hide();
        // The right dock area's own hide (it keeps docks that want to show
        // up, e.g. the inspector on a selection, hidden meanwhile).
        this->map_fullscreen_hid_right_docks = !this->right_dock_area_hidden;
        if (this->map_fullscreen_hid_right_docks)
            toggleRightDockArea();
#ifdef Q_OS_WASM
        // Browsers allow this only inside a user gesture; when it is refused
        // the page still shows nothing but the map.
        this->map_fullscreen_entered_window_fullscreen = !this->browser_fullscreen_active;
        if (this->map_fullscreen_entered_window_fullscreen)
        {
            emscripten_run_script(
                "if (!document.fullscreenElement) "
                "document.documentElement.requestFullscreen().catch(() => {});");
        }
#else
        this->map_fullscreen_entered_window_fullscreen = !isFullScreen();
        if (this->map_fullscreen_entered_window_fullscreen)
        {
            this->window_state_saved = windowState();
            this->window_geometry_saved = geometry();
            showFullScreen();
        }
#endif
    }
    else
    {
        this->top_control_bar->setVisible(this->map_fullscreen_toolbar_visible);
        this->footer->statusBar()->setVisible(this->map_fullscreen_status_bar_visible);
        this->main_navigation->setNavigationBarVisible(true);
        if (this->map_fullscreen_history_visible)
            this->history_dock->show();
        this->map_fullscreen_history_visible = false;
        if (this->map_fullscreen_hid_right_docks && this->right_dock_area_hidden)
            toggleRightDockArea();
        this->map_fullscreen_hid_right_docks = false;
        if (this->map_fullscreen_entered_window_fullscreen)
        {
#ifdef Q_OS_WASM
            emscripten_run_script(
                "if (document.fullscreenElement) document.exitFullscreen().catch(() => {});");
#else
            if (isFullScreen())
            {
                showNormal();
                setGeometry(this->window_geometry_saved);
                setWindowState(this->window_state_saved);
            }
#endif
        }
        this->map_fullscreen_entered_window_fullscreen = false;
    }

    applyMapFullscreenToMode();
#ifndef Q_OS_WASM
    this->top_control_bar->setFullScreenState(isFullScreen());
#endif
    updateMapEdgePanning();
}

void MainWindow::applyMapFullscreenToMode()
{
    // Management uses the editor's input controller for cache rectangle
    // selections and therefore shares its fullscreen HUD/shortcut state.
    const bool editor_input_mode = this->map_workspace->isDistributionEditorActive()
        || this->map_workspace->currentMode() == MapWorkspaceWidget::MapManagement;
    const bool monitor_active = this->map_fullscreen_active && !editor_input_mode;
    const bool editor_active = this->map_fullscreen_active && editor_input_mode;

    // Deactivate the old mode first and activate the new one last: both
    // controllers address the same bgfx HUD and navigation input focus.
    if (monitor_active)
    {
        this->map_editor->applyMapFullscreen(false);
        this->map_monitor->applyMapFullscreen(true);
    }
    else
    {
        this->map_monitor->applyMapFullscreen(false);
        this->map_editor->applyMapFullscreen(editor_active);
    }
    // The shared viewport page, not its currently selected tool panel,
    // must release the sidebar's layout width in map fullscreen.
    this->map_workspace->setMapFullscreen(this->map_fullscreen_active);
}

void MainWindow::refreshHistoryDock()
{
    if (this->history_dock == nullptr || this->projects_workspace == nullptr)
        return;
    // Editing follows the exact opened project/branch, even if the user is
    // browsing a different revision in Projects. Without a private editor,
    // follow the Projects selection instead.
    const bool opened = this->personal_edit_active && !this->opened_project_id.isEmpty();
    const HistoryDockContext context = this->projects_workspace->historyContext(
        opened ? this->opened_project_id : QString(),
        opened ? this->opened_branch_id : QString());
    QList<HistoryDockEntry> actions;
    if (this->personal_edit_active && this->personal_edit_session != nullptr)
    {
        const std::function<void(const QList<QByteArray> &, HistoryDockEntry::Kind)> appendEvents =
            [&actions, this](const QList<QByteArray> &frames,
                                                    HistoryDockEntry::Kind kind)
        {
            for (const QByteArray &frame : frames)
            {
                const std::optional<aowis::model::ModelEditTransaction> transaction =
                    aowis::model::ModelEditTransactionCodec::decode(frame);
                HistoryDockEntry entry;
                entry.kind = kind;
                if (transaction.has_value())
                {
                    entry.title = transaction->description.trimmed();
                    if (entry.title.isEmpty())
                    {
                        entry.title = transaction->operation;
                        entry.title.replace(QLatin1Char('.'), QLatin1Char(' '));
                    }
                    entry.identity = transaction->transactionId.toString(QUuid::WithoutBraces);
                    entry.localized_details = [operation = transaction->operation, count = transaction->edits.size(), id = entry.identity] {
                        return HistoryDock::tr("%1\n%2 entity edit(s)\nEvent %3").arg(operation, QString::number(count), id);
                    };
                }
                else
                    entry.localized_details = [] { return HistoryDock::tr("This event could not be decoded for display."); };
                entry.localized_title = [title = entry.title, valid = transaction.has_value(), kind] {
                    QString text = title;
                    if (text.isEmpty())
                        text = valid ? HistoryDock::tr("Hydraulic edit") : HistoryDock::tr("Unrecognized stored edit");
                    if (kind == HistoryDockEntry::Kind::PendingAction)
                        text += HistoryDock::tr(" (pending Save)");
                    return text;
                };
                actions.append(entry);
            }
        };
        appendEvents(this->personal_edit_session->savedHistoryEvents(),
                     HistoryDockEntry::Kind::SavedAction);
        appendEvents(this->personal_edit_session->pendingHistoryEvents(),
                     HistoryDockEntry::Kind::PendingAction);
    }
    QString historyScope = opened && this->personal_edit_session != nullptr &&
        !this->opened_branch_name.isEmpty()
        ? this->personal_edit_session->historyScopeKey() + QStringLiteral(":") +
            this->opened_branch_name : QString();
    if (!historyScope.isEmpty())
    {
        const QJsonObject authoritative = this->personal_edit_session->historyCursor();
        const QString fork = authoritative.value(QStringLiteral("forkRevisionId")).toString();
        if (!fork.isEmpty())
            historyScope += QStringLiteral(":fork:") + fork + QStringLiteral(":") +
                authoritative.value(QStringLiteral("forkActionPosition")).toString();
    }
    this->history_dock->setArchiveScope(historyScope);
    this->history_dock->setTimeline(context, actions, this->personal_edit_active,
                                    this->personal_edit_blocked);
    this->history_dock->setNavigationState(
        opened && this->personal_edit_session != nullptr
            ? this->personal_edit_session->historyCursor() : QJsonObject{},
        opened && !this->personal_edit_blocked &&
            this->personal_edit_session != nullptr &&
            this->personal_edit_session->canNavigateHistory(),
        opened && this->personal_edit_session != nullptr
            ? this->personal_edit_session->historyNavigationStatus()
            : tr("Open a private draft to enable History navigation."));
}

void MainWindow::toggleRightDockArea()
{
    const QList<QDockWidget *> docks = findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);

    if (!this->right_dock_area_hidden)
    {
        this->right_dock_visibility.clear();
        this->right_dock_area_hidden = true;

        for (QDockWidget *dock : docks)
        {
            if (dock->isFloating() || dockWidgetArea(dock) != Qt::RightDockWidgetArea)
                continue;

            this->right_dock_visibility.insert(dock, dock->isVisible());
            hideRightDock(dock);
        }

        return;
    }

    this->right_dock_area_hidden = false;
    this->right_docks_being_hidden.clear();

    for (QDockWidget *dock : docks)
    {
        if (!this->right_dock_visibility.contains(dock) || dock->isFloating() || dockWidgetArea(dock) != Qt::RightDockWidgetArea)
            continue;

        bool visible = this->right_dock_visibility.value(dock);
        if (dock == this->dock_map_editor_guide)
            visible = visible && this->dock_map_editor_guide->shouldBeVisible();

        dock->setVisible(visible);
    }

    this->right_dock_visibility.clear();
    scheduleRightDockResize();
}

void MainWindow::showEntityInspectorForSelection()
{
    const bool area_was_hidden = this->right_dock_area_hidden;
    if (area_was_hidden)
        toggleRightDockArea();

    // The inspector dock is also independently closable via its own title
    // bar button (DockWidgetClosable), which never touches
    // right_dock_area_hidden - that flag only tracks the "toggle sidebar"
    // shortcut/action closing the whole right-hand area at once. So closing
    // just the inspector this way leaves the flag false and
    // toggleRightDockArea() above never runs for it. Force it visible (and
    // the resize-blocking flag clear) unconditionally, regardless of which
    // way it was closed.
    this->right_dock_area_hidden = false;
    this->dock_entity_inspector->setVisible(true);
    this->dock_entity_inspector->show();
    this->dock_entity_inspector->raise();
    scheduleRightDockResize();

    // The dock lives inside this window, so raising it only reorders
    // widgets within our own stacking - it does nothing if another
    // top-level window (e.g. the non-modal "Simulation Results" dialog) is
    // currently in front of us. Bring the whole window forward too, so a
    // click that selects an entity is actually visible to the person who
    // clicked it.
    raise();
    activateWindow();
}

void MainWindow::hideRightDock(QDockWidget *dock)
{
    if (dock == nullptr || !dock->isVisible())
        return;

    this->right_docks_being_hidden.insert(dock);
    dock->hide();
}

void MainWindow::onRightDockVisibilityChanged(QDockWidget *dock, bool visible)
{
    if (!this->right_dock_area_hidden || dock == nullptr || dock->isFloating() || dockWidgetArea(dock) != Qt::RightDockWidgetArea)
        return;

    if (!visible)
    {
        if (this->right_docks_being_hidden.remove(dock) == 0)
            this->right_dock_visibility.insert(dock, false);

        return;
    }

    this->right_dock_visibility.insert(dock, true);
    hideRightDock(dock);
}

void MainWindow::scheduleRightDockResize()
{
    if (this->right_dock_resize_pending)
        return;

    this->right_dock_resize_pending = true;
    QTimer::singleShot(0, this, [this]
    {
        this->right_dock_resize_pending = false;
        resizeRightDocks();
    });
}

void MainWindow::resizeRightDocks()
{
    if (this->right_dock_area_hidden)
        return;

    const int available_height = this->centralWidget() != nullptr ? this->centralWidget()->height() : this->height();
    if (available_height <= 0)
        return;

    const bool inspector_visible = this->dock_entity_inspector->isVisible() && !this->dock_entity_inspector->isFloating() &&
                                   dockWidgetArea(this->dock_entity_inspector) == Qt::RightDockWidgetArea;
    const bool legend_visible = this->dock_entity_map_legend->isVisible() && !this->dock_entity_map_legend->isFloating() &&
                                dockWidgetArea(this->dock_entity_map_legend) == Qt::RightDockWidgetArea;
    const bool guide_visible = this->dock_map_editor_guide->isVisible() && !this->dock_map_editor_guide->isFloating() &&
                               dockWidgetArea(this->dock_map_editor_guide) == Qt::RightDockWidgetArea;

    const int visible_count = static_cast<int>(inspector_visible) + static_cast<int>(legend_visible) + static_cast<int>(guide_visible);
    if (visible_count == 0)
        return;

    int height_remaining = available_height;
    int height_legend = 0;
    int height_guide = 0;
    int height_inspector = 0;

    if (legend_visible)
    {
        const int height_reserved_for_other_docks = qMax(0, visible_count - 1);
        height_legend = qBound(1, this->dock_entity_map_legend->dockHeightPreferred(), qMax(1, available_height - height_reserved_for_other_docks));
        height_remaining = qMax(0, height_remaining - height_legend);
    }

    if (guide_visible)
    {
        if (inspector_visible)
            height_guide = qMax(1, height_remaining / 4);
        else
            height_guide = qMax(1, height_remaining);

        height_remaining = qMax(0, height_remaining - height_guide);
    }

    if (inspector_visible)
        height_inspector = qMax(1, height_remaining);

    QList<QDockWidget *> docks_to_resize;
    QList<int> dock_heights;
    if (inspector_visible)
    {
        docks_to_resize.append(this->dock_entity_inspector);
        dock_heights.append(height_inspector);
    }

    if (legend_visible)
    {
        docks_to_resize.append(this->dock_entity_map_legend);
        dock_heights.append(height_legend);
    }

    if (guide_visible)
    {
        docks_to_resize.append(this->dock_map_editor_guide);
        dock_heights.append(height_guide);
    }

    if (docks_to_resize.size() < 2)
        return;

    resizeDocks(docks_to_resize, dock_heights, Qt::Vertical);
}

void MainWindow::fullScreenToggle()
{
#ifdef Q_OS_WASM
    emscripten_run_script(R"JS(
        if (typeof toggleAowisFullscreen === 'function') {
            toggleAowisFullscreen();
        } else if (document.fullscreenElement) {
            document.exitFullscreen();
        } else {
            document.documentElement.requestFullscreen();
        }
    )JS");
#else
    const bool enter_fullscreen = !this->isFullScreen();

    if (enter_fullscreen)
    {
        this->window_state_saved = this->windowState();
        this->window_geometry_saved = this->geometry();
        this->showFullScreen();
    }
    else
    {
        this->showNormal();
        this->setGeometry(this->window_geometry_saved);
        this->setWindowState(this->window_state_saved);
    }

    this->top_control_bar->setFullScreenState(enter_fullscreen);
    this->updateMapEdgePanning();
#endif
}

#ifdef Q_OS_WASM
EM_BOOL MainWindow::fullScreenChangeCallback(int event_type, const EmscriptenFullscreenChangeEvent *event, void *user_data)
{
    Q_UNUSED(event_type);

    MainWindow *window = static_cast<MainWindow *>(user_data);

    if (window == nullptr || event == nullptr)
        return EM_FALSE;

    window->browser_fullscreen_active = event->isFullscreen != 0;
    window->top_control_bar->setFullScreenState(window->browser_fullscreen_active);
    // The browser left the full screen map fullscreen entered (Escape): map
    // fullscreen ends with it.
    if (!window->browser_fullscreen_active && window->map_fullscreen_active
        && window->map_fullscreen_entered_window_fullscreen)
    {
        window->map_fullscreen_entered_window_fullscreen = false;
        window->setMapFullscreen(false);
    }
    window->updateMapEdgePanning();
    return EM_TRUE;
}
#endif

void MainWindow::updateActiveMapPanels()
{
    const bool map_visible = this->main_navigation->currentWidget() == this->map_workspace;
    this->dock_entity_map_legend->setMapMonitorActive(
        map_visible && this->map_workspace->isDistributionMonitorActive());
    this->dock_map_editor_guide->setMapEditorActive(
        map_visible && this->map_workspace->isDistributionEditorActive());
}

void MainWindow::updateMapEdgePanning()
{
    if (this->map_mon == nullptr || this->main_navigation == nullptr)
        return;

#ifdef Q_OS_WASM
    const bool fullscreen = this->browser_fullscreen_active || this->map_fullscreen_active;
#else
    const bool fullscreen = isFullScreen() || this->map_fullscreen_active;
#endif

    const bool map_visible = this->main_navigation->currentWidget() == this->map_workspace
        && this->map_workspace->activeMap() == this->map_mon;
    // The same MapWidget handles edge panning in all four modes.
    this->map_mon->setEdgePanningEnabled(fullscreen && map_visible);
}

void MainWindow::restoreOpenedNetworkSelection()
{
    if (!this->opened_project_name.isEmpty())
    {
        this->projects_workspace->selectProjectByName(this->opened_project_name);
        if (!this->opened_branch_name.isEmpty())
            this->projects_workspace->selectBranchByName(this->opened_branch_name);
    }
    this->top_control_bar->restoreOpenedContext();
}

void MainWindow::stopPersonalDraftEditing()
{
    if (!this->personal_edit_active) return;
    this->hydraulic_data->setLogicalEditCaptureEnabled(false);
    this->personal_edit_guard->stop();
    this->personal_edit_closing = true;
    this->personal_edit_stop_button->setEnabled(false);
    this->personal_edit_stop_button->setText(tr("Closing private draft…"));
    // Other editing views must not remain active with an unbound writer.
    if (this->map_workspace != nullptr && this->map_workspace->isDistributionEditorActive())
        this->map_workspace->setCurrentMode(MapWorkspaceWidget::DistributionMonitor);
    this->personal_edit_session->stop();
}

void MainWindow::blockPersonalDraftEditing(const QString &reason)
{
    if (!this->personal_edit_active || this->personal_edit_blocked)
        return;
    this->personal_edit_blocked = true;
    refreshHistoryDock();
    this->hydraulic_data->setLogicalEditCaptureEnabled(false);
    this->personal_edit_guard->stop();
    this->personal_edit_stop_button->setEnabled(true);
    this->personal_edit_stop_button->setText(tr("Close blocked private draft"));
    if (this->map_workspace->isDistributionEditorActive())
        this->map_workspace->setCurrentMode(MapWorkspaceWidget::DistributionMonitor);
    this->personal_edit_session->blockEditing(reason);
    this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Save failed"));
    if (this->network_open_pending) restoreOpenedNetworkSelection();
    this->network_open_pending = false;
    statusBar()->showMessage(tr("Private draft editing blocked: %1").arg(reason));
    // Defer the modal warning until the hydraulic edit call has unwound.
    QTimer::singleShot(0, this, [this, reason]
    {
        QMessageBox::warning(this, tr("Private draft editing blocked"),
            tr("Editing has been stopped. Queued events remain on disk. "
               "An uncaptured mutation may still be present in memory; do not treat it as saved. "
               "Close and reopen the private draft to recover a verified model.\n\n%1").arg(reason));
    });
}

void MainWindow::synchronizePersonalHistoryEditMode()
{
    if (!this->personal_edit_active || this->personal_edit_blocked ||
        this->personal_edit_session == nullptr || this->map_workspace == nullptr)
        return;
    if (!this->personal_edit_session->canCaptureHistoryEdits())
    {
        this->hydraulic_data->setLogicalEditCaptureEnabled(false);
        if (this->personal_edit_session->isReady())
        {
            this->personal_edit_guard->start();
            this->top_control_bar->setNetworkStatus(QT_TRANSLATE_NOOP("LiveUiText", "Verifying writable History position…"));
        }
        return;
    }
    if (!this->hydraulic_data->logicalEditCaptureEnabled())
    {
        this->hydraulic_data->setLogicalEditCaptureEnabled(true);
        this->personal_edit_last_checked_revision = std::numeric_limits<quint64>::max();
        this->personal_edit_guard->start();
        checkPersonalDraftModel();
    }
    if (!this->personal_edit_blocked)
        this->top_control_bar->setNetworkStatus(this->personal_edit_session->pendingCount() > 0
            ? QT_TRANSLATE_NOOP("LiveUiText", "Saving · %1 pending") : QT_TRANSLATE_NOOP("LiveUiText", "Autosaved"), QString::number(this->personal_edit_session->pendingCount()));
}

void MainWindow::checkPersonalDraftModel()
{
    if (!this->personal_edit_active || this->personal_edit_blocked)
        return;
    // Capture is guarded by the verified History cursor. It must
    // equal the session's authoritative permission; compare canonical states
    // even while capture is disabled to detect accidental widget mutations.
    if (this->hydraulic_data->logicalEditCaptureEnabled() !=
        this->personal_edit_session->canCaptureHistoryEdits())
    {
        blockPersonalDraftEditing(tr("Edit capture does not match the verified History position."));
        return;
    }
    if (!this->personal_edit_session->isReady())
    {
        blockPersonalDraftEditing(tr("Private draft writer is no longer ready."));
        return;
    }
    const quint64 revision = this->hydraulic_data->visualRevision();
    if (revision == this->personal_edit_last_checked_revision)
        return;
    const QByteArray live = aowis::model::ModelCodec<NetworkHydraulic>::encode(
        this->hydraulic_data->networkHydraulic());
    const QByteArray staged = aowis::model::ModelCodec<NetworkHydraulic>::encode(
        this->personal_edit_session->visibleNetwork());
    if (live.isEmpty() || staged.isEmpty() || live != staged)
        blockPersonalDraftEditing(tr("Live model differs from durably staged MODEL transactions."));
    else
        this->personal_edit_last_checked_revision = revision;
}
