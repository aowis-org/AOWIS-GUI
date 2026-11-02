#pragma once

#include <QApplication>
#include <QDockWidget>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QWidget>
#include <QGridLayout>
#include <QResizeEvent>
#include <QProcessEnvironment>

#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QIcon>

#include <QKeyEvent>
#include <QEvent>
#include <QMessageBox>
#include <QContextMenuEvent>

#include <QByteArray>

#include <QTimer>
#include <QTime>
#include <QDateTime>

#include "app/footer_statusbar.h"
#include "services/rest_client.h"
#include "app/menubar.h"

#include "tabs/tab_settings_widget.h"
#include "config/shortcut_registry.h"

#include "map/core/map_model.h"
#include "map/data/map_tile_repository.h"
#include "map/data/map_terrain_repository.h"
#include "map/core/map_widget.h"
#include "tabs/tab_map_monitor_container.h"
#include "tabs/tab_map_editor_container.h"

#include "tabs/tab_energy_widget.h"
#include "tabs/tab_hydraulic_libraries_widget.h"
#include "tabs/tab_hydraulic_controls_widget.h"
#include "tabs/tab_reservoirs_widget.h"
#include "tabs/tab_tanks_widget.h"
#include "tabs/tab_pumps_widget.h"
#include "tabs/tab_valves_widget.h"
#include "tabs/tab_junctions_widget.h"
#include "tabs/tab_pipes_widget.h"
#include "tabs/tab_demand_points_widget.h"
#include "tabs/tab_customers_widget.h"
#include "tabs/tab_logs_widget.h"
#include "tabs/tab_alarms_widget.h"
#include "app/top_control_bar.h"
#include "entity_inspector/entity_inspector_dock.h"
#include "entity_inspector/entity_map_legend_dock.h"
#include "map/editor/map_editor_guide_dock.h"
#include "simulation/simulation_manager.h"
#include "widgets/main_navigation_widget.h"

#include "network/hydraulic_data.h"
#include "projects/personal_draft_edit_session.h"

#ifdef Q_OS_WASM
#include <emscripten.h>
#include <emscripten/html5.h>
#include "gps/gps_provider_dummy.h"
#else
#include "gps/gps_provider.h"
#endif

class QShortcut;
class MapWorkspaceWidget;
class ProjectsWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    
protected:
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    
private:
    Qt::WindowStates window_state_saved;
    QRect window_geometry_saved;
    void fullScreenToggle();
    // Map fullscreen (the map HUD's fullscreen button and shortcut, monitor
    // and editor): the window goes full screen (desktop: the window; WASM:
    // the browser page, when the browser allows it) and shows only the
    // current map page's map - toolbar, status bar, navigation bar, right
    // docks and the page's left menu are hidden. In place: the map's native
    // window / browser canvas is never moved, so bgfx keeps running.
    void setMapFullscreen(bool fullscreen);
    bool map_fullscreen_active = false;
    // This mode made the window (browser page) full screen: leaving it
    // restores the previous state; the window leaving full screen ends it.
    bool map_fullscreen_entered_window_fullscreen = false;
    bool map_fullscreen_hid_right_docks = false;
    bool map_fullscreen_toolbar_visible = true;
    bool map_fullscreen_status_bar_visible = true;
    void toggleRightDockArea();
    void showEntityInspectorForSelection();
    void hideRightDock(QDockWidget *dock);
    void onRightDockVisibilityChanged(QDockWidget *dock, bool visible);
    void scheduleRightDockResize();
    void resizeRightDocks();
    void updateMapEdgePanning();
    void applyMapFullscreenToMode();

#ifdef Q_OS_WASM
    static EM_BOOL fullScreenChangeCallback(int event_type, const EmscriptenFullscreenChangeEvent *event, void *user_data);
    bool browser_fullscreen_active = false;
#endif
    
    void restoreOpenedNetworkSelection();
    void stopPersonalDraftEditing();
    void blockPersonalDraftEditing(const QString &reason);
    void checkPersonalDraftModel();

    HydraulicData *hydraulic_data = nullptr;
    PersonalDraftEditSession *personal_edit_session = nullptr;
    QPushButton *personal_edit_stop_button = nullptr;
    QTimer *personal_edit_guard = nullptr;
    bool personal_edit_active = false;
    bool personal_edit_blocked = false;
    bool network_open_pending = false;
    bool network_confirmation_visible = false;
    QString opened_project_name;
    QString opened_branch_name;
    QString pending_open_project_id;
    QString pending_open_branch_id;
    QString pending_open_project_name;
    QString pending_open_branch_name;
    QString opened_project_id;
    QString opened_branch_id;
    bool immutable_network_read_only = false;
    quint64 personal_edit_last_checked_revision = 0;
    
    GpsProvider *gps = nullptr;
    
    MenuBar *menu;
    FooterStatusBar *footer;
    
    EntityInspectorDock *dock_entity_inspector;
    EntityMapLegendDock *dock_entity_map_legend;
    MapEditorGuideDock *dock_map_editor_guide;
    TopControlBar *top_control_bar;
    bool right_dock_area_hidden = false;
    QHash<QDockWidget *, bool> right_dock_visibility;
    QSet<QDockWidget *> right_docks_being_hidden;
    bool right_dock_resize_pending = false;
    
    MapTileRepository *map_tile_repository;
    MapTerrainRepository *map_terrain_repository;
    MapModel *map_model_monitor;
    MapWidget *map_mon = nullptr;

    MainNavigationWidget *main_navigation = nullptr;
    SettingsWidget *settings;
    int settings_page_index = -1;
    QShortcut *shortcut_toggle_right_docks = nullptr;
    MapWorkspaceWidget *map_workspace = nullptr;
    int map_workspace_page_index = -1;
    ProjectsWidget *projects_workspace = nullptr;
    void updateActiveMapPanels();
    MapMonitorContainer *map_monitor;
    MapEditorContainer *map_editor;
    EnergyWidget *energy;
    HydraulicLibrariesWidget *hydraulic_libraries;
    HydraulicControlsWidget *hydraulic_controls;
    ReservoirsWidget *reservoirs;
    TanksWidget *tanks;
    PumpsWidget *pumps;
    ValvesWidget *valves;
    JunctionsWidget *junctions;
    PipesWidget* pipes;
    DemandPointsWidget *demand_points;
    CustomersWidget *customers;
    LogsWidget *logs;
    AlarmsWidget *alarms;
    SimulationManager *simulation_manager;
    
    
    //QGridLayout *layout = new QGridLayout;
    
    QLabel *label_image_a;
    QLabel *label_image_b;
    
    class HttpHealthMonitor *map_health_monitor = nullptr;
    void checkServerMapInit();
    
protected:
    void resizeEvent(QResizeEvent *event) override;
};
