#ifndef TAB_MAP_MONITOR_CONTAINER_H
#define TAB_MAP_MONITOR_CONTAINER_H

#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QWidget>
#include <QUuid>

#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QStackedLayout>

#include <QScrollArea>
#include <QScrollBar>
#include <QGroupBox>
#include <QButtonGroup>
#include <QPushButton>
#include <QRadioButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QLabel>

#include <QIcon>

#include <memory>

#include "common/_enums_structs.h"
#include "network/network_symbology.h"
#include "map/render/map_network_render_symbology.h"
#include "map/core/map_model.h"
#include "map/data/map_tile_repository.h"
#include "map/core/map_widget.h"
#include "map/core/map_navigation_widget.h"
#include "widgets/group_box_collapsible.h"

class HydraulicData;
class MapTerrainRepository;
class MapBgfxSurface;
class MapBgfxEditorOverlay;
class MapBgfxGlobeHudController;
class QTimer;
class QShortcut;

#ifdef Q_OS_WASM
#include "gps/gps_provider_dummy.h"
#else
#include "gps/gps_provider.h"
#endif

#include "common/_sizes.h"

class MapModeToolPanel;

class MapMonitorMenuWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MapMonitorMenuWidget(MapWidget *map, QWidget *parent = nullptr);
    
    MapNavigationWidget *mapNavigationWidget();
    void setNodeVisualSelection(VisualNode visual_node);
    void setLinkVisualSelection(VisualLink visual_link);
    void setHeatmapVisualSelection(VisualHeatmap visual_heatmap);

private:
    QVBoxLayout *layout;
    
    MapWidget *map;
    
    MapNavigationWidget *map_nav;
    QButtonGroup *node_visual_group = nullptr;
    QButtonGroup *link_visual_group = nullptr;
    QButtonGroup *heatmap_visual_group = nullptr;
    NetworkSymbologySizeUnit node_size_unit = NetworkSymbologySizeUnit::Pixels;
    double node_size_m = NetworkSymbologyDefaultNodeSizeM;
    int node_size_px = NetworkSymbologyDefaultNodeSizePx;
    NetworkSymbologySizeUnit link_thickness_unit = NetworkSymbologySizeUnit::Pixels;
    double link_thickness_m = NetworkSymbologyDefaultLinkThicknessM;
    int link_thickness_px = NetworkSymbologyDefaultLinkThicknessPx;
    HeatmapRadiusUnit heatmap_radius_unit = HeatmapRadiusUnit::Meters;
    int heatmap_radius_m = 400;
    int heatmap_radius_px = 50;
    void addGroupVisualSettings();
    void addGroupNodeVisuals();
    void addGroupLinkVisuals();
    void addGroupHeatmapVisuals();
    
signals:
    void mapZoomIn();
    void mapZoomOut();
    
    void signalNodeVisualClicked(VisualNode visual_node);
    void signalNodeSizeUnitChanged(NetworkSymbologySizeUnit unit);
    void signalNodeSizeChanged(NetworkSymbologySizeUnit unit, double size);
    void signalNodeDeclutteringChanged(bool enabled);
    void signalLinkVisualClicked(VisualLink visual_link);
    void signalLinkThicknessUnitChanged(NetworkSymbologySizeUnit unit);
    void signalLinkThicknessChanged(NetworkSymbologySizeUnit unit, double thickness);
    void signalFlowDirectionSizeChanged(int size_px);
    void signalHeatmapVisualClicked(VisualHeatmap visual_heatmap);
    void signalHeatmapOpacityChanged(int opacity);
    void signalHeatmapRadiusUnitChanged(HeatmapRadiusUnit unit);
    void signalHeatmapRadiusChanged(HeatmapRadiusUnit unit, int radius);
    void signalHeatmapSolidCenterChanged(int percent);
};





class MapMonitorContainer : public QWidget
{
    Q_OBJECT
public:
    explicit MapMonitorContainer(MapModel *map_model, MapTileRepository *tile_repository,
                                 MapTerrainRepository *terrain_repository,
                                 HydraulicData *hydraulic_data, GpsProvider *gps,
                                 QWidget *parent = nullptr);
    ~MapMonitorContainer() override;
    
    MapWidget *getMap();
    MapBgfxSurface *sharedBgfxSurface() const;
    bool surfaceReady() const;
    void setEditorInputCanvas(QWidget *canvas);
    void activateMonitorSurface();
    void setEditorMode(bool enabled);
    void setBasemapOnly(bool enabled);
    MapModeToolPanel *toolPanel() const;
    QWidget *mapViewport() const;
    
    MapNavigationWidget *mapNavigationWidget();

public slots:
    // Map fullscreen ended (or started) by the main window: the HUD button
    // state and the left menu follow. Never emits signalMapFullscreenRequested().
    void applyMapFullscreen(bool fullscreen);
    void setNodePalette(NetworkSymbologyPalette palette, bool flipped);
    void setLinkPalette(NetworkSymbologyPalette palette, bool flipped);
    void setHeatmapPalette(NetworkSymbologyPalette palette, bool flipped);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QHBoxLayout *layout = nullptr;
    GpsProvider *gps = nullptr;
    MapModel *map_model = nullptr;
    MapTileRepository *tile_repository = nullptr;
    MapTerrainRepository *terrain_repository = nullptr;
    HydraulicData *hydraulic_data = nullptr;
    QWidget *map_stack = nullptr;
    QStackedLayout *map_stack_layout = nullptr;
    MapWidget *map = nullptr;
    // Shown while no GPU map surface is active (startup or failure). There
    // is deliberately no CPU or JavaScript map renderer behind it.
    QLabel *renderer_status = nullptr;
    MapMonitorMenuWidget *map_menu = nullptr;
    // The left menu's scroll area (hidden in map fullscreen).
    MapModeToolPanel *tool_panel = nullptr;
    int network_background_opacity = 0;
    NetworkSymbologySettings symbology_settings;
    bool visual_control_symbology_apply_pending = false;
    // The settings of the last full symbology resolve (applySymbology()):
    // slider changes against them take the surface's cheap path.
    NetworkSymbologySettings resolved_symbology_settings;
    bool symbology_resolved = false;
    // Dense per-entity values per time step (order-cached render ids).
    MapNetworkSymbologyResolver symbology_resolver;
    // Network or simulation data changed: the next coalesced apply must
    // resolve again whatever the settings say.
    bool symbology_resolve_requested = false;
    // The bgfx map surface (created in the constructor). It receives the
    // resolved symbology through applySymbology().
    MapBgfxSurface *bgfx_surface = nullptr;
    QWidget *editor_input_canvas = nullptr;
    bool editor_mode = false;
    bool basemap_only = false;
    // The logical selection belongs to monitoring even when its GPU
    // highlight is temporarily hidden in the editor profile.
    InfrastructureEntity monitor_selected_type = InfrastructureEntity::Unknown;
    QUuid monitor_selected_uuid;

    bool monitor_node_decluttering = true;
    bool monitor_globe_3d_icons = false;
    int monitor_globe_icon_size_percent = 100;
    std::shared_ptr<MapBgfxEditorOverlay> monitor_globe_icon_overlay;
    // The surface presents frames: picking and hover only run against a
    // live surface.
    bool bgfx_render_surface_active = false;
    // What the globe HUD's camera sliders and compass do to the map model
    // (Phase 3 slice 5); created with the bgfx surface.
    MapBgfxGlobeHudController *bgfx_globe_hud = nullptr;
    // Download activity of the shared repositories, mirrored into the bgfx
    // HUD (signals plus the QWidget HUD's 400 ms poll).
    QTimer *bgfx_download_poll_timer = nullptr;
    int bgfx_tiles_active = 0;
    int bgfx_tiles_queued = 0;
    int bgfx_terrain_active = 0;
    int bgfx_terrain_queued = 0;
    // Map fullscreen for the bgfx surface (desktop and WASM): the HUD button
    // and the MapMonitorFullscreen shortcut. In place: the main window hides
    // everything but this map (signalMapFullscreenRequested()); the map's
    // native window is never reparented, so bgfx keeps running (no device
    // restart, no re-upload) and the browser canvas keeps its stacking.
    QShortcut *bgfx_fullscreen_shortcut = nullptr;
    bool bgfx_fullscreen_active = false;
    // Pointer shape the bgfx HUD wants under the pointer (pointing hand over
    // buttons and combos, cross over legend ramps, arrow otherwise).
    Qt::CursorShape bgfx_hud_cursor = Qt::ArrowCursor;
    // Click selection on the bgfx surface (desktop and WASM); true when an
    // entity was hit and selected.
    bool selectBgfxNetworkEntityAt(const QPointF &position);
    // Sends a map mouse event to the bgfx HUD first; true when the HUD
    // consumed it. *over_hud_panel: the pointer is on a HUD panel (no map
    // hover picking there).
    bool routeBgfxHudMouseEvent(QMouseEvent *event, bool *over_hud_panel);
    // A clicked HUD button or legend entry (MapBgfxHud::Command, passed as
    // plain values to keep bgfx out of this header). group: legend node (0),
    // link (1), heatmap (2); value: metric or palette, or the underground
    // mode; flipped: palette flip. The globe sliders and the compass go to
    // bgfx_globe_hud instead.
    void handleBgfxHudAction(int action, int group, int value, bool flipped);
    // Map legend on the bgfx HUD (slice 6b): rebuilt from the symbology
    // settings and their canonical ranges whenever the symbology is
    // resolved, the unit profile changes or the surface becomes active.
    void rememberMonitorSelection(InfrastructureEntity entity_type, const QUuid &uuid);
    void applyMonitorDiagnostics();
    void pushBgfxLegend(const NetworkSymbologyRanges &ranges);
    void refreshBgfxLegend();
    void refreshBgfxDownloadActivity();
    void pushBgfxDownloadActivity();
    void setBgfxFullscreen(bool fullscreen);
    void setBgfxHudCursor(Qt::CursorShape shape);
    // One cursor decision for network hover and the HUD; on desktop the
    // bgfx native window needs its own cursor.
    void applyMapHoverCursor();

    bool selectNetworkEntity(quint32 render_id, InfrastructureEntity entity_type, const QUuid &uuid = QUuid());
    void applySymbology();
    // A palette or flip change: GPU state and the legend only, no resolve.
    void applyPaletteSymbology();
    void applyVisualControlSymbology();
    void setNetworkBackgroundOpacity(int opacity);

    bool network_hovered = false;

    void updateNetworkHover(
        const QPointF &position, Qt::MouseButtons buttons,
        Qt::KeyboardModifiers modifiers);
    void setNetworkHovered(bool hovered);
    void showRendererStatus(const QString &text);
    void showMapLegendNode(VisualNode visual_node);
    void showMapLegendLink(VisualLink visual_link);
    void showMapLegendHeatmap(VisualHeatmap visual_heatmap);


signals:
    // The HUD button / shortcut: the main window enters or leaves map
    // fullscreen and answers with applyMapFullscreen().
    void signalMapFullscreenRequested(bool fullscreen);
    void signalShowMapLegendNode(VisualNode visual_node);
    void signalShowMapLegendLink(VisualLink visual_link);
    void signalShowMapLegendHeatmap(VisualHeatmap visual_heatmap);
    void signalNodePaletteChanged(NetworkSymbologyPalette palette, bool flipped);
    void signalLinkPaletteChanged(NetworkSymbologyPalette palette, bool flipped);
    void signalHeatmapPaletteChanged(NetworkSymbologyPalette palette, bool flipped);
};



#endif // TAB_MAP_MONITOR_CONTAINER_H
