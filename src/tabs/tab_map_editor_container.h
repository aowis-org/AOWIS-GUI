#ifndef TAB_MAP_EDITOR_CONTAINER_H
#define TAB_MAP_EDITOR_CONTAINER_H

#include <QObject>
#include <QPointF>
#include <QWidget>

#include <QEvent>
#include <QTimer>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedLayout>
#include <QScrollArea>
#include <QToolBox>
#include <QToolButton>
#include <QRadioButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QKeySequence>

#include "map/core/map_model.h"
#include "map/data/map_tile_repository.h"
#include "map/data/map_terrain_repository.h"
#include "map/core/map_widget.h"
#include "map/core/map_navigation_widget.h"
#include "map/editor/map_canvas_widget.h"
#include "map/bgfx/map_bgfx_surface.h"
#include "map/editor/map_editor_controller.h"
#include "entity_inspector/entity_inspector_dock.h"
#include "network/hydraulic_data.h"

#ifdef Q_OS_WASM
#include "gps/gps_provider_dummy.h"
#else
#include "gps/gps_provider.h"
#endif

#include "common/_sizes.h"
#include "common/_enums_structs.h"

#include <QDebug>

// The map editor draws with bgfx only (roadmap Phase 2; the QPainter editor
// and the QRhi editor path are gone): imagery, the network, every editor
// overlay, map labels and the HUD. Without bgfx in the build, or when it
// cannot start, a status label takes the map's place.
//
// 3D: the HUD's 2D/3D switch shows the globe (terrain, the network in the
// editor look, the globe HUD) once the bgfx globe initialized. New nodes
// commit through the shared 2D editor path with cached terrain elevations;
// link completion and movement remain preview-only until elevation-safe.
// Cache tile selection works in 3D.
class MapModeToolPanel;
class MapBgfxSurface;
class MapMonitorContainer;
class MapBgfxGlobeHudController;
class QKeyEvent;
class QShortcut;

class MapEditorMenuWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MapEditorMenuWidget(MapWidget *map, MapCanvasWidget *map_canvas,
                                 MapEditorController *editor_controller,
                                 MapTerrainRepository *terrain_repository, CanvasMode mode,
                                 QWidget *parent = nullptr);
    
    MapNavigationWidget *mapNavigationWidget();
    bool isEditNetworkSectionActive() const;
    // Map fullscreen: the left menu is not visible, so its buttons' window
    // shortcuts (tools, map providers) cannot fire. The map forwards key
    // presses here instead; true when a button's shortcut matched (the
    // button was clicked).
    bool triggerButtonShortcut(const QKeyEvent *event);
    // The cache and terrain toolbox is hosted by the Map Management panel,
    // not by the Distribution Editor's tools.
    QWidget *managementControls() const;
    void setManagementActive(bool active);
    void clearCacheSelections();
    
private:
    CanvasMode mode;
    QVBoxLayout *layout;
    
    MapWidget *map;
    MapNavigationWidget *map_nav;
    MapCanvasWidget *map_canvas;
    MapEditorController *editor_controller;
    MapTerrainRepository *terrain_repository;
    
    QSpinBox *spin_zoom_from;
    QSpinBox *spin_zoom_to;
    
    QToolBox *toolbox;
    bool management_active = false;
    int toolbox_cache_index = -1;
    int toolbox_terrain_index = -1;
    void createToolboxCache(QToolBox *tbx);
    void createToolboxTerrain(QToolBox *tbx);
    void createEditControls();
    void setToolboxMode(int index);
    void updateToolboxHeight(int index);
    // Network editing is 2D only; cache management works in both views.
    void applyViewMode(MapViewMode view_mode);
    // The terrain dataset (zoom range for the spin boxes and the selection,
    // availability) from the terrain service's catalog.
    void applyTerrainDataset();
    
    QButtonGroup *button_group_tools = nullptr;
    QRadioButton *button_radio_select = nullptr;
    QToolButton *button_tiles_delete = nullptr;
    QCheckBox *checkbox_map_editor_guide = nullptr;
    QWidget *edit_tools = nullptr;
    QToolButton *button_edit_delete = nullptr;
    bool editor_delete_selection_available = false;

    // Terrain Cache section.
    QLabel *label_terrain_dataset = nullptr;
    QSpinBox *spin_terrain_zoom_from = nullptr;
    QSpinBox *spin_terrain_zoom_to = nullptr;
    QCheckBox *checkbox_terrain_source = nullptr;
    QToolButton *button_terrain_delete = nullptr;
    QLabel *label_terrain_result = nullptr;
    // A region is selected in the Terrain Cache section.
    bool terrain_selection_valid = false;
    // A deleteCache() batch is running (no second one meanwhile), and
    // whether it includes the source data (its result text).
    bool terrain_deletion_running = false;
    bool terrain_deletion_with_source = false;
    bool terrain_deletion_result_available = false;
    int terrain_deleted_tile_count = -1;
    int terrain_deleted_source_count = -1;
    QString terrain_deletion_error;
    void refreshTerrainDeletionText();
    // Delete Terrain is enabled with a selection, a known dataset and no
    // deletion running.
    void refreshTerrainDeleteButton();

public slots:
    void setMapEditorGuideChecked(bool checked);
    
signals:
    void signalSlideOpacityChanged(int opacity);
    void signalMapEditorGuideVisibilityChanged(bool visible);
    void signalEditNetworkSectionActive(bool active);
};



class MapEditorContainer : public QWidget
{
    Q_OBJECT
public:
    explicit MapEditorContainer(MapModel *map_model, MapTileRepository *tile_repository,
                                MapTerrainRepository *terrain_repository,
                                HydraulicData *hydraulic_data, GpsProvider *gps,
                                EntityInspectorDock *map_inspector,
                                MapMonitorContainer *monitor, QWidget *parent = nullptr);
    ~MapEditorContainer() override;
    
    MapWidget *getMap();
    void activateSharedSurface(bool active);
    QWidget *inputCanvas() const;
    // Independent left tools and map viewport, still hosted side by side by
    // this legacy container until the shared viewport migration.
    MapModeToolPanel *toolPanel() const;
    QWidget *mapViewport() const;
    QWidget *managementControls() const;
    void setManagementActive(bool active);
    
    MapNavigationWidget *mapNavigationWidget();
    bool isEditNetworkSectionActive() const;

public slots:
    void setMapEditorGuideChecked(bool checked);
    // Map fullscreen ended (or started) by the main window: the HUD button
    // state and the left menu follow. Never emits signalMapFullscreenRequested().
    void applyMapFullscreen(bool fullscreen);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Leaving the editor page clears the cache sections' selections.
    void hideEvent(QHideEvent *event) override;

private:
    HydraulicData *hydraulic_data = nullptr;
    GpsProvider *gps;
    
    EntityInspectorDock *map_inspector;
    
    MapMonitorContainer *monitor = nullptr;
    bool shared_editor_mode = false;
    std::function<void(const MapBgfxHud::Command &)> editor_hud_action_handler;
    MapBgfxSurface::OverlayBuilder editor_overlay_builder;
    MapBgfxSurface::GlobeOverlayBuilder editor_globe_overlay_builder;
    MapBgfxLabels::SymbolRect editor_node_label_rect;
    MapBgfxLabels::SymbolRect editor_link_label_rect;
    MapModel *map_model;
    MapTileRepository *tile_repository;
    MapTerrainRepository *terrain_repository;
    QWidget *map_stack;
    QStackedLayout *map_stack_layout;
    MapWidget *map;
    MapCanvasWidget *map_canvas;
    // Shown instead of the map until bgfx presents its first frame, and
    // when it cannot (no bgfx in the build, startup failure).
    QLabel *renderer_status;
    void showRendererStatus(const QString &text);
    MapBgfxSurface *bgfx_surface = nullptr;
    bool bgfx_editor_active = false;
    // The globe HUD's sliders and compass (the monitor's controller).
    MapBgfxGlobeHudController *bgfx_globe_hud = nullptr;
    // The editor's visual state for the bgfx overlay, refreshed when the
    // canvas reports a change (MapCanvasWidget::signalRenderUpdateRequested)
    // instead of being rebuilt every frame.
    MapEditorVisualState bgfx_visual_state;
    bool bgfx_visual_state_dirty = true;
    // Only the camera moved: refresh the pointer's map coordinate.
    bool bgfx_visual_camera_dirty = false;
    // Pointer shape of the HUD element under the pointer (buttons); wins
    // over the editor tool's cursor.
    Qt::CursorShape bgfx_hud_cursor = Qt::ArrowCursor;
    // Map fullscreen (desktop and WASM): the HUD button and the map
    // fullscreen shortcut, in place like the monitor's (the main window
    // hides everything else; bgfx keeps running).
    QShortcut *bgfx_fullscreen_shortcut = nullptr;
    bool bgfx_fullscreen_active = false;
    void setupBgfxEditor();
    // HUD clicks (queued from the surface): 2D/3D switch, fullscreen, zoom,
    // the globe toggles and underground mode (value).
    void handleBgfxHudAction(int action, int value);
    void setBgfxFullscreen(bool fullscreen);
    void applyEditorCursor();
    // HUD first for pointer, wheel and Escape on the canvas (desktop: the
    // events forwarded from the native window; WASM: the canvas' own).
    bool routeBgfxHudEvent(QEvent *event);

    // Network snapshot, editor symbology and move preview (hidden entities,
    // full-network translation) of the bgfx map.
    void connectRenderSurfaceSync();
    void syncRenderSurfaceEditorState();
    void syncRenderSurfaceNetworkSnapshot();
    void applyRenderSurfaceEditorSymbology();
    quint64 render_move_session_id = 0;
    QPointF render_move_start_mouse_position;
    bool render_full_network_move = false;
    QPointF render_move_translation;
    MapEditorController *editor_controller;
    MapEditorMenuWidget *map_menu;
    // The left menu's scroll area (hidden in map fullscreen).
    MapModeToolPanel *tool_panel = nullptr;
    
    QHBoxLayout *layout;
    
signals:
    // The HUD button / shortcut: the main window enters or leaves map
    // fullscreen and answers with applyMapFullscreen().
    void signalMapFullscreenRequested(bool fullscreen);
    void signalMapEditorGuideVisibilityChanged(bool visible);
    void signalEditNetworkSectionActive(bool active);
};

#endif // TAB_MAP_EDITOR_CONTAINER_H
