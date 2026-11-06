#ifndef MAP_BGFX_SURFACE_H
#define MAP_BGFX_SURFACE_H

#include "map/bgfx/map_bgfx_map_view.h"

#include <QPointer>
#include <QWidget>

#include <memory>

class MapModel;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QWindow;

// The Qt Widgets host of the shared map view (AOWIS-GUI-Render
// MapBgfxMapView): embeds its native window, forwards the widget's events,
// keeps keyboard focus, and supplies the GUI's settings and assets (terrain
// detail, model textures, HUD images, navigation shortcuts, units). Every
// map method forwards to the view.
class MapBgfxSurface final : public QWidget, private MapBgfxMapViewHost
{
    Q_OBJECT
public:
    explicit MapBgfxSurface(MapModel *model, QWidget *parent = nullptr);
    ~MapBgfxSurface() override;

    QString graphicsApiName() const;
    // Picking with the QRhi rules at the model's current camera: 2D
    // (map_bgfx_2d_picking.h) and the globe (MapBgfxGlobeNetwork::hitTest()).
    MapRenderHit hitTest(const QPointF &screen_position) const;
    // Terrain-aware navigation (Phase 3, slice 2; MapGlobeTerrainNavigation):
    // pointer coordinates with DEM altitude (2D: cached DEM only), rays
    // against the drawn terrain meshes, terrain-anchored globe panning. The
    // camera-height follow runs inside this surface.
    bool terrainCoordinateAtScreen(const QPointF &screen_position, CoordinateWGS84 *coordinate,
                                   bool request_missing_tile = true);
    // The same drawn-terrain projection used for the last globe overlay
    // frame. No independent 3D editing camera or terrain calculation.
    bool globeCoordinateToScreen(const CoordinateWGS84 &coordinate,
                                 QPointF *screen_position) const;
    bool cachedTerrainElevationAtCoordinate(const CoordinateWGS84 &coordinate,
                                            double *elevation_m) const;
    bool globeSurfaceRayHitAtScreen(const QPointF &screen_position,
                                    MapGlobeSurfaceHit *hit) const;
    // The drawn globe surface (terrain mesh, else ellipsoid) under a screen
    // position, with the camera as the model has it now: the crosshair's
    // sets the sphere a pointer pan turns the globe on.
    bool globeSurfacePointAtScreen(const QPointF &screen_position,
                                   GeoWgs84Ellipsoid::EcefPositionD *point);

    // Keyboard events originate from the embedded QWindow on native desktop.
    // Forward them to the shared map-navigation owner; bgfx must not implement
    // a second camera controller or independently quantize zoom input.
    using NavigationKeyHandler = MapBgfxMapView::NavigationKeyHandler;
    using NavigationWheelHandler = MapBgfxMapView::NavigationWheelHandler;
    using NavigationFrameHandler = MapBgfxMapView::NavigationFrameHandler;
    void setNavigationHandlers(NavigationKeyHandler press,
                               NavigationKeyHandler release,
                               std::function<void()> cancel);
    // Wheel events on the embedded native window (desktop) go to the same
    // shared navigation owner as keys; they share one zoom motion.
    void setNavigationWheelHandler(NavigationWheelHandler wheel);
    // Called once per presented frame with the paced elapsed time, *before*
    // the camera is sampled. This surface is then the only navigation clock.
    void setNavigationFrameHandler(NavigationFrameHandler advance);
    // Mouse press/release/move on the embedded native window (desktop).
    // Returns true when consumed, like MapWidget::handleMouse*Event().
    using NavigationMouseHandler = MapBgfxMapView::NavigationMouseHandler;
    void setNavigationMouseHandlers(NavigationMouseHandler press,
                                    NavigationMouseHandler release,
                                    NavigationMouseHandler move);
    // The window that actually has pointer focus over the map (desktop), so
    // a Wayland pointer lock can target its own wl_surface. nullptr on WASM.
    QWindow *nativeInputWindow() const;
    // One line on where startup stands (initialization, frame loop, frames),
    // for the startup watchdog and failure messages.
    QString startupDiagnostics() const;
    void focusNavigationInput(Qt::FocusReason reason = Qt::OtherFocusReason);

    void setNetworkSnapshot(const NetworkRenderSnapshot &snapshot);
    // Hide distribution network/heatmaps/labels/picking in drainage modes.
    // Retains CPU/GPU geometry, imagery, terrain and navigation unchanged.
    void setDistributionNetworkVisible(bool visible);
    bool distributionNetworkVisible() const { return this->view->distributionNetworkVisible(); }
    // Editor move preview: these entities are drawn transparent (colour
    // stream only, no geometry upload) while the overlay draws them moving.
    void setHiddenEntityUuids(const QSet<QUuid> &uuids);
    void setNodeDeclutteringEnabled(bool enabled);
    // Editor full-network move: the network (not the imagery) is drawn
    // shifted by this many logical pixels (camera offset, no upload).
    void setNetworkScreenTranslation(const QPointF &translation);
    void setSymbology(const MapNetworkRenderSymbology &symbology);
    void setVisualControlSettings(const NetworkSymbologySettings &settings);
    void setTileRepository(MapTileRepository *tile_repository);
    void setTerrainRepository(MapTerrainRepository *terrain_repository);
    void setBackgroundOpacity(int opacity);
    void setSelectedEntity(InfrastructureEntity entity_type, const QUuid &uuid);
    void setSimulationErrorEntities(const QHash<QUuid, InfrastructureEntity> &error_entities,
                                    const QSet<QUuid> &stale_entity_uuids);
    // Globe network (slice 3): tank/reservoir as 3D models instead of icons;
    // underground pipes Solid (through the terrain), X-Ray (buried runs
    // dashed through the terrain) or Hide.
    void setGlobe3dIconsEnabled(bool enabled);
    // Full-colour overlay icons on the globe replace only its generic GPU
    // atlas symbols; the 2D monitor symbology is unchanged.
    void setGlobeFlatEntityIconsEnabled(bool enabled);
    void setUndergroundMode(MapUndergroundMode mode);
    MapUndergroundMode undergroundMode() const { return this->view->undergroundMode(); }
    void setTerrainWireframeVisible(bool visible);
    void setMapTilesVisible(bool visible);
    // Current state of the globe HUD's "wireframe" / "map" toggles.
    bool terrainWireframeVisible() const;
    // The globe HUD's "hillshade" toggle.
    void setTerrainHillshadeVisible(bool visible);
    bool terrainHillshadeVisible() const;
    bool mapTilesVisible() const;

    // --- Globe on bgfx (roadmap Phase 3) ------------------------------------------
    // Terrain surface (imagery, DEM relief, caps, wireframe),
    // terrain-aware navigation, the network and its heatmap, picking and
    // the globe HUD (compass, camera sliders, underground mode, toggles,
    // terrain-mesh progress, scale).
    // The globe surface initialized on this device (the HUD offers 3D).
    bool globeAvailable() const { return this->view->globeAvailable(); }
    // Surfaces that never show the globe (the editor, 2D only) skip its GPU
    // setup and offer no 3D. Call before the surface is first shown.
    void setGlobeOffered(bool offered) { this->view->setGlobeOffered(offered); }

    // Planned native-window changes (reparenting, desktop; map fullscreen
    // no longer reparents, MainWindow::setMapFullscreen()): shut
    // bgfx down while the old native surface still exists, restart once the
    // window is exposed again. Qt Wayland destroys the wl_surface on reparent
    // without a QPlatformSurfaceEvent, so this must not rely on events.
    void suspendForNativeWindowChange();
    void resumeAfterNativeWindowChange();

    // --- Native HUD (slices 6a, 6b) ---------------------------------------------
    // Called (queued, outside the native window's event dispatch) when a HUD
    // button or legend entry is clicked.
    void setHudActionHandler(std::function<void(const MapBgfxHud::Command &)> handler);
    void setHudGpsPosition(bool valid, double longitude_deg, double latitude_deg);
    void setHudDownloadActivity(int tiles_active, int tiles_queued,
                                int terrain_active, int terrain_queued);
    void setHudFullscreen(bool active);
    // The pan joystick's knob (MapWidget::joystickChanged()).
    void setHudJoystick(const QPointF &deflection, bool active);
    // Held keyboard pan keys (MapWidget::keyboardPanChanged()): the
    // joystick's arrows glow.
    void setHudKeyboardPan(const QPointF &direction);
    // Map legend content (map_bgfx_legend_content.h); visible=false hides it.
    void setHudLegend(const MapBgfxHud::Legend &legend);
    // Closes an open HUD popup (combo list, palette menu).
    void closeHudPopup();
    // The 2D/3D switch (shown by default; 3D is offered once the bgfx globe
    // exists).
    void setHudViewSwitchVisible(bool visible);

    // --- Map labels (map_label_settings.h) ----------------------------------------
    // Which labels are drawn; all off (default) costs nothing.
    void setLabelSettings(const MapLabelSettings &settings);
    // Canonical -> displayed value of the node / link colouring metric
    // (mapBgfxLabelValueFormat()); the values come with setSymbology().
    void setLabelValueFormats(const MapBgfxLabelValueFormat &node_format,
                              const MapBgfxLabelValueFormat &link_format);
    // Symbol boxes the labels are placed around and keep clear of, relative
    // to the anchor (logical px). Unset: the GPU network's own symbols (dots,
    // icons at the symbology's sizes). The editor sets its icon boxes (its
    // icons are drawn by the overlay, the GPU network only has dots).
    void setLabelSymbolRects(MapBgfxLabels::SymbolRect node_symbol,
                             MapBgfxLabels::SymbolRect link_symbol);

    // --- On-demand rendering ------------------------------------------------------
    // The surface presents a frame only when its camera or size changed, one
    // of its setters was called, or something animates (HUD busy bars or
    // tooltip delay, budgeted imagery uploads, deferred labels). Anything
    // else that changes what it draws - the editor overlay's content - calls
    // this.
    void requestRedraw();

    // --- Map editor (Phase 2) ---------------------------------------------------
    // Called every 2D frame after the network and before the HUD with the
    // frame's continuous camera; draws into its own canvas and atlas, which
    // the surface uploads and submits with the HUD program. Returns the
    // translation (logical px) to draw the canvas with: a builder may keep
    // the canvas of an earlier frame and only move it (pure pans,
    // map_bgfx_pan_reuse.h); QPointF() for a canvas built for this camera.
    using OverlayBuilder = MapBgfxMapView::OverlayBuilder;
    void setOverlayBuilder(OverlayBuilder builder);
    // Globe mode: where a WGS84 coordinate on the drawn surface (terrain
    // relief where it is drawn, else the ellipsoid) lies on screen this
    // frame, in logical px. False behind the camera or beyond the horizon
    // (relief does not occlude: the overlay is drawn on top of the globe).
    // Optional distance_m returns eye-to-draped-anchor distance for correct
    // painter ordering of transparent icons in the 3D editor overlay.
    using GlobeProjector = MapBgfxMapView::GlobeProjector;
    // Network elevations are hydraulic metres, transformed into the same
    // globe-space height used by the retained 3D network (including its lift).
    // The surface projector above remains terrain-draped for map selections,
    // demand points, reservoirs and intermediate pipe bends.
    using GlobeElevatedProjector = MapBgfxMapView::GlobeElevatedProjector;
    // Called every globe frame after the terrain and the network and before
    // the HUD; draws into the overlay's canvas and atlas (the 2D builder's,
    // the two never run in the same frame), submitted untranslated.
    using GlobeOverlayBuilder = MapBgfxMapView::GlobeOverlayBuilder;
    void setGlobeOverlayBuilder(GlobeOverlayBuilder builder);
    // Desktop: keyboard focus stays on this widget instead of moving to the
    // embedded native window on a click (the editor canvas handles keys and
    // owns the editor's window shortcuts). nullptr: the native window takes
    // focus (Map Monitor).
    void setKeyboardFocusWidget(QWidget *widget);
    // Routes a mouse event (logical px of this widget) to the HUD first.
    // consumed: the event belongs to the HUD and must not reach navigation or
    // picking. cursor: the HUD element's pointer shape (Qt::ArrowCursor when
    // the HUD has no opinion). over_panel: hovering a blocking HUD panel or
    // popup (no map hover there).
    using HudPointer = MapBgfxMapView::HudPointer;
    HudPointer handleHudMouseEvent(QMouseEvent *event);
    // Wheel over a HUD panel scrolls an open combo list and never zooms the
    // map underneath; true when consumed. On desktop the native window's
    // wheel events are routed here internally; WASM routes MapWidget's.
    bool handleHudWheelEvent(QWheelEvent *event);
    // Escape closes an open HUD popup; true when consumed (same routing).
    bool handleHudKeyPress(QKeyEvent *event);
    // Map pointer shape: pointing hand over pickable entities and HUD
    // buttons, cross over legend ramps, Qt::ArrowCursor resets it. On
    // desktop the cursor must be set on the embedded native window; the
    // widget's own cursor is never visible there.
    void setMapCursor(Qt::CursorShape shape);

    // The shared map view this widget hosts.
    MapBgfxMapView *mapView() const { return this->view.get(); }

signals:
    void signalRendererReady();
    void signalRendererFailed(const QString &reason);
    // Host-produced HUD strings (legend, units, tooltips) need regeneration
    // after Qt installs/removes a translator. No device or camera reset.
    void localizedPresentationRefreshRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    // MapBgfxMapViewHost
    bool mapViewVisible() const override;
    QFont mapViewFont() const override;
    QPalette mapViewPalette() const override;
    void mapViewFocusNavigationInput(Qt::FocusReason reason) override;
    QSize mapViewLogicalSize() const override;
    qreal mapViewDevicePixelRatio() const override;
    QScreen *mapViewScreen() const override;
    QPoint mapViewGlobalPosition() const override;
    void mapViewSetCursor(Qt::CursorShape shape) override;

    // The GUI's navigation shortcuts in the HUD tooltips.
    void updateHudNavigationKeys();

    std::unique_ptr<MapBgfxMapView> view;
    QPointer<QWidget> keyboard_focus_widget;
};

#endif // MAP_BGFX_SURFACE_H
