#include "map/bgfx/map_bgfx_surface.h"

#include "config/gui_configuration.h"
#include "config/shortcut_registry.h"
#include "config/unit_aware_view.h"
#include "config/unit_profile_manager.h"
#include "map/bgfx/map_bgfx_globe_hud_controller.h"
#include "map/bgfx/map_bgfx_globe_model_images.h"
#include "map/core/map_scale_distance.h"
#include "map/core/map_scale_renderer.h"

#include <QEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QHideEvent>
#include <QImage>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>

#include <cmath>
#include <utility>

MapBgfxSurface::MapBgfxSurface(MapModel *model, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aowis-bgfx-monitor-surface"));
    MapBgfxMapViewHost *host = this;
    this->view = std::make_unique<MapBgfxMapView>(model, host);
    connect(this->view.get(), &MapBgfxMapView::signalRendererReady,
            this, &MapBgfxSurface::signalRendererReady);
    connect(this->view.get(), &MapBgfxMapView::signalRendererFailed,
            this, &MapBgfxSurface::signalRendererFailed);
    connect(this->view.get(), &MapBgfxMapView::localizedPresentationRefreshRequested,
            this, &MapBgfxSurface::localizedPresentationRefreshRequested);

    // The user's terrain detail setting, read live.
    this->view->setTerrainDetailReductionSource([]()
    {
        return guiConfiguration().map_performance.terrain_detail_reduction;
    });
    // The user's antialiasing setting, read live (a change resets the back
    // buffer on the next frame).
    this->view->setAntialiasingSamplesSource([]()
    {
        return guiConfiguration().map_performance.antialiasing_samples;
    });
    // Tank and reservoir model textures are GUI resources.
    this->view->setGlobeModelImages(mapGlobeTankAlbedoImage(), mapGlobeReservoirAlbedoImage());
    // The QWidget HUD's images.
    this->view->setHudImages(QImage(QStringLiteral(":/icon/crosshair.png")),
                             QImage(QStringLiteral(":/icon/fullscreen.png")),
                             QImage(QStringLiteral(":/icon/fullscreen_undo.png")),
                             QImage(QStringLiteral(":/icon/zoom_in.png")),
                             QImage(QStringLiteral(":/icon/zoom_out.png")));
    // A scale bar in the user's distance unit from the continuous camera.
    this->view->setHudScaleProvider([](double lon0, double lat0, double lon1, double lat1,
                                       double bar_px)
    {
        MapBgfxHud::ScaleBar result;
        CoordinateWGS84 start;
        start.longitude_deg = lon0;
        start.latitude_deg = lat0;
        CoordinateWGS84 end;
        end.longitude_deg = lon1;
        end.latitude_deg = lat1;
        const double maximum_distance_m = MapScaleRenderer::Detail::distanceMeters(start, end);
        const MapScaleDistance::Scale scale = MapScaleDistance::forMaximumDistance(
            maximum_distance_m, UnitProfileManager::instance().presentation());
        if (!(maximum_distance_m > 0.0) || !(scale.distance_m > 0.0)
            || !std::isfinite(maximum_distance_m))
            return result;
        result.valid = true;
        result.width_px = bar_px * scale.distance_m / maximum_distance_m;
        result.label = scale.label;
        return result;
    });
    // Globe sliders and compass in the user's units.
    this->view->setGlobeHudStateProvider([](const MapModel &map_model, const QSize &viewport,
                                            MapBgfxHud::Globe *globe)
    {
        MapBgfxGlobeHudController::fillHudState(map_model, viewport, globe);
    });
    // A unit profile switch changes the scale bar label (HUD).
    new UnitAwareView(this, UnitProfileManager::instance().presentation(),
                      [this]() { this->view->requestRedraw(); });
    // The navigation keys in the joystick's / zoom buttons' tooltips follow
    // the shortcut settings.
    updateHudNavigationKeys();
    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged, this,
            [this](GuiShortcutId) { updateHudNavigationKeys(); });

#ifndef Q_OS_WASM
    setFocusPolicy(Qt::StrongFocus);
    // createWindowContainer embeds the view's native QWindow (and owns it
    // from now on). Its key events are not delivered to MapWidget once
    // keyboard focus moves to bgfx; the view routes them.
    QWidget *container = QWidget::createWindowContainer(this->view->nativeInputWindow(), this);
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(container);
#endif
}

MapBgfxSurface::~MapBgfxSurface()
{
    // bgfx lets go of the native window before the container destroys it.
    this->view.reset();
}

void MapBgfxSurface::updateHudNavigationKeys()
{
    const GuiShortcutRegistry &registry = guiShortcutRegistry();
    MapBgfxHud::NavigationKeys keys;
    keys.pan_up = registry.shortcut(GuiShortcutId::MapPanUp);
    keys.pan_down = registry.shortcut(GuiShortcutId::MapPanDown);
    keys.pan_left = registry.shortcut(GuiShortcutId::MapPanLeft);
    keys.pan_right = registry.shortcut(GuiShortcutId::MapPanRight);
    keys.zoom_in = registry.shortcut(GuiShortcutId::MapZoomIn);
    keys.zoom_out = registry.shortcut(GuiShortcutId::MapZoomOut);
    this->view->setHudNavigationKeys(keys);
}

void MapBgfxSurface::focusNavigationInput(Qt::FocusReason reason)
{
    // The map's window first (another window, e.g. a dialog or the
    // fullscreen map, may be active), then the key receiver in it.
    if (this->keyboard_focus_widget)
    {
        // Editor: keys go to its canvas widget (ordinary QWidget focus).
        this->keyboard_focus_widget->window()->activateWindow();
        this->keyboard_focus_widget->setFocus(reason);
        return;
    }
#ifndef Q_OS_WASM
    // The native window already holds keyboard focus: nothing to route.
    // Re-activating the top-level here would pull Win32 focus off the
    // embedded child HWND on every click (Windows), only to hand it back.
    QWindow *focused_native_window = this->view->nativeInputWindow();
    if (focused_native_window != nullptr
        && QGuiApplication::focusWindow() == focused_native_window)
        return;
    // Monitor: QWidget focus on this surface delivers the keys (keyPressEvent)
    // wherever an embedded window cannot be activated by itself (Wayland: a
    // subsurface never gets keyboard focus of its own, so requestActivate()
    // alone left the keys with whatever widget had them); where it can
    // (X11, Windows, macOS) the native window then takes them.
    window()->activateWindow();
    setFocus(reason);
    QWindow *native_window = this->view->nativeInputWindow();
    if (native_window != nullptr)
        native_window->requestActivate();
#else
    Q_UNUSED(reason)
#endif
}

void MapBgfxSurface::setKeyboardFocusWidget(QWidget *widget)
{
    this->keyboard_focus_widget = widget;
    // Clicks must not activate the embedded window, or the top-level loses
    // its focus widget (and the editor's window shortcuts with it).
    this->view->setNativeWindowAcceptsFocus(widget == nullptr);
}

// --- QWidget events -------------------------------------------------------------

void MapBgfxSurface::keyPressEvent(QKeyEvent *event)
{
    if (this->view->handleKeyPress(event))
        return;
    QWidget::keyPressEvent(event);
}

void MapBgfxSurface::keyReleaseEvent(QKeyEvent *event)
{
    if (this->view->handleKeyRelease(event))
        return;
    QWidget::keyReleaseEvent(event);
}

void MapBgfxSurface::focusOutEvent(QFocusEvent *event)
{
    this->view->handleFocusOut();
    QWidget::focusOutEvent(event);
}

void MapBgfxSurface::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    this->view->handleHostShown();
}

void MapBgfxSurface::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (this->view)
        this->view->handleHostHidden();
}

void MapBgfxSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (this->view)
        this->view->handleHostResized();
}

void MapBgfxSurface::changeEvent(QEvent *event)
{
    if (this->view)
    {
        this->view->handleHostChange(event->type());
        // Language changes need an actual HUD frame even when the camera is
        // stationary. Layout direction changes can also affect text placement.
        // Neither operation rebuilds the view or modifies map state.
        if (event->type() == QEvent::LanguageChange
            || event->type() == QEvent::LayoutDirectionChange)
            this->view->requestRedraw();
    }
    QWidget::changeEvent(event);
}

// --- MapBgfxMapViewHost -----------------------------------------------------------

bool MapBgfxSurface::mapViewVisible() const
{
    return isVisible();
}

QFont MapBgfxSurface::mapViewFont() const
{
    return font();
}

QPalette MapBgfxSurface::mapViewPalette() const
{
    return palette();
}

void MapBgfxSurface::mapViewFocusNavigationInput(Qt::FocusReason reason)
{
    focusNavigationInput(reason);
}

QSize MapBgfxSurface::mapViewLogicalSize() const
{
    return size();
}

qreal MapBgfxSurface::mapViewDevicePixelRatio() const
{
    return devicePixelRatioF();
}

QScreen *MapBgfxSurface::mapViewScreen() const
{
    return screen();
}

QPoint MapBgfxSurface::mapViewGlobalPosition() const
{
    return mapToGlobal(QPoint(0, 0));
}

void MapBgfxSurface::mapViewSetCursor(Qt::CursorShape shape)
{
    if (shape != Qt::ArrowCursor)
        setCursor(shape);
    else
        unsetCursor();
}

// --- Forwarded to the shared map view ----------------------------------------

QString MapBgfxSurface::graphicsApiName() const
{
    return this->view->graphicsApiName();
}

MapRenderHit MapBgfxSurface::hitTest(const QPointF &screen_position) const
{
    return this->view->hitTest(screen_position);
}

bool MapBgfxSurface::terrainCoordinateAtScreen(const QPointF &screen_position, CoordinateWGS84 *coordinate, bool request_missing_tile)
{
    return this->view->terrainCoordinateAtScreen(screen_position, coordinate, request_missing_tile);
}

bool MapBgfxSurface::globeCoordinateToScreen(const CoordinateWGS84 &coordinate, QPointF *screen_position) const
{
    return this->view->globeCoordinateToScreen(coordinate, screen_position);
}

bool MapBgfxSurface::cachedTerrainElevationAtCoordinate(const CoordinateWGS84 &coordinate, double *elevation_m) const
{
    return this->view->cachedTerrainElevationAtCoordinate(coordinate, elevation_m);
}

bool MapBgfxSurface::globeSurfaceRayHitAtScreen(const QPointF &screen_position, MapGlobeSurfaceHit *hit) const
{
    return this->view->globeSurfaceRayHitAtScreen(screen_position, hit);
}

bool MapBgfxSurface::globeSurfacePointAtScreen(const QPointF &screen_position, GeoWgs84Ellipsoid::EcefPositionD *point)
{
    return this->view->globeSurfacePointAtScreen(screen_position, point);
}

void MapBgfxSurface::setNavigationHandlers(NavigationKeyHandler press, NavigationKeyHandler release, std::function<void()> cancel)
{
    this->view->setNavigationHandlers(std::move(press), std::move(release), std::move(cancel));
}

void MapBgfxSurface::setNavigationWheelHandler(NavigationWheelHandler wheel)
{
    this->view->setNavigationWheelHandler(std::move(wheel));
}

void MapBgfxSurface::setNavigationFrameHandler(NavigationFrameHandler advance)
{
    this->view->setNavigationFrameHandler(std::move(advance));
}

void MapBgfxSurface::setNavigationMouseHandlers(NavigationMouseHandler press, NavigationMouseHandler release, NavigationMouseHandler move)
{
    this->view->setNavigationMouseHandlers(std::move(press), std::move(release), std::move(move));
}

QWindow *MapBgfxSurface::nativeInputWindow() const
{
    return this->view->nativeInputWindow();
}

QString MapBgfxSurface::startupDiagnostics() const
{
    return this->view->startupDiagnostics();
}

void MapBgfxSurface::setNetworkSnapshot(const NetworkRenderSnapshot &snapshot)
{
    this->view->setNetworkSnapshot(snapshot);
}

void MapBgfxSurface::setDistributionNetworkVisible(bool visible)
{
    this->view->setDistributionNetworkVisible(visible);
}

void MapBgfxSurface::setHiddenEntityUuids(const QSet<QUuid> &uuids)
{
    this->view->setHiddenEntityUuids(uuids);
}

void MapBgfxSurface::setNodeDeclutteringEnabled(bool enabled)
{
    this->view->setNodeDeclutteringEnabled(enabled);
}

void MapBgfxSurface::setNetworkScreenTranslation(const QPointF &translation)
{
    this->view->setNetworkScreenTranslation(translation);
}

void MapBgfxSurface::setSymbology(const MapNetworkRenderSymbology &symbology)
{
    this->view->setSymbology(symbology);
}

void MapBgfxSurface::setVisualControlSettings(const NetworkSymbologySettings &settings)
{
    this->view->setVisualControlSettings(settings);
}

void MapBgfxSurface::setTileRepository(MapTileRepository *tile_repository)
{
    this->view->setTileRepository(tile_repository);
}

void MapBgfxSurface::setTerrainRepository(MapTerrainRepository *terrain_repository)
{
    this->view->setTerrainRepository(terrain_repository);
}

void MapBgfxSurface::setBackgroundOpacity(int opacity)
{
    this->view->setBackgroundOpacity(opacity);
}

void MapBgfxSurface::setSelectedEntity(InfrastructureEntity entity_type, const QUuid &uuid)
{
    this->view->setSelectedEntity(entity_type, uuid);
}

void MapBgfxSurface::setSimulationErrorEntities(const QHash<QUuid, InfrastructureEntity> &error_entities, const QSet<QUuid> &stale_entity_uuids)
{
    this->view->setSimulationErrorEntities(error_entities, stale_entity_uuids);
}

void MapBgfxSurface::setGlobe3dIconsEnabled(bool enabled)
{
    this->view->setGlobe3dIconsEnabled(enabled);
}

void MapBgfxSurface::setGlobeFlatEntityIconsEnabled(bool enabled)
{
    this->view->setGlobeFlatEntityIconsEnabled(enabled);
}

void MapBgfxSurface::setUndergroundMode(MapUndergroundMode mode)
{
    this->view->setUndergroundMode(mode);
}

void MapBgfxSurface::setTerrainWireframeVisible(bool visible)
{
    this->view->setTerrainWireframeVisible(visible);
}

void MapBgfxSurface::setMapTilesVisible(bool visible)
{
    this->view->setMapTilesVisible(visible);
}

bool MapBgfxSurface::terrainWireframeVisible() const
{
    return this->view->terrainWireframeVisible();
}

void MapBgfxSurface::setTerrainHillshadeVisible(bool visible)
{
    this->view->setTerrainHillshadeVisible(visible);
}

bool MapBgfxSurface::terrainHillshadeVisible() const
{
    return this->view->terrainHillshadeVisible();
}

bool MapBgfxSurface::mapTilesVisible() const
{
    return this->view->mapTilesVisible();
}

void MapBgfxSurface::suspendForNativeWindowChange()
{
    this->view->suspendForNativeWindowChange();
}

void MapBgfxSurface::resumeAfterNativeWindowChange()
{
    this->view->resumeAfterNativeWindowChange();
}

void MapBgfxSurface::setHudActionHandler(std::function<void(const MapBgfxHud::Command &)> handler)
{
    this->view->setHudActionHandler(std::move(handler));
}

void MapBgfxSurface::setHudGpsPosition(bool valid, double longitude_deg, double latitude_deg)
{
    this->view->setHudGpsPosition(valid, longitude_deg, latitude_deg);
}

void MapBgfxSurface::setHudDownloadActivity(int tiles_active, int tiles_queued, int terrain_active, int terrain_queued)
{
    this->view->setHudDownloadActivity(tiles_active, tiles_queued, terrain_active, terrain_queued);
}

void MapBgfxSurface::setHudFullscreen(bool active)
{
    this->view->setHudFullscreen(active);
}

void MapBgfxSurface::setHudJoystick(const QPointF &deflection, bool active)
{
    this->view->setHudJoystick(deflection, active);
}

void MapBgfxSurface::setHudKeyboardPan(const QPointF &direction)
{
    this->view->setHudKeyboardPan(direction);
}

void MapBgfxSurface::setHudLegend(const MapBgfxHud::Legend &legend)
{
    this->view->setHudLegend(legend);
}

void MapBgfxSurface::closeHudPopup()
{
    this->view->closeHudPopup();
}

void MapBgfxSurface::setHudViewSwitchVisible(bool visible)
{
    this->view->setHudViewSwitchVisible(visible);
}

void MapBgfxSurface::setLabelSettings(const MapLabelSettings &settings)
{
    this->view->setLabelSettings(settings);
}

void MapBgfxSurface::setLabelValueFormats(const MapBgfxLabelValueFormat &node_format, const MapBgfxLabelValueFormat &link_format)
{
    this->view->setLabelValueFormats(node_format, link_format);
}

void MapBgfxSurface::setLabelSymbolRects(MapBgfxLabels::SymbolRect node_symbol, MapBgfxLabels::SymbolRect link_symbol)
{
    this->view->setLabelSymbolRects(node_symbol, link_symbol);
}

void MapBgfxSurface::requestRedraw()
{
    this->view->requestRedraw();
}

void MapBgfxSurface::setOverlayBuilder(OverlayBuilder builder)
{
    this->view->setOverlayBuilder(std::move(builder));
}

void MapBgfxSurface::setGlobeOverlayBuilder(GlobeOverlayBuilder builder)
{
    this->view->setGlobeOverlayBuilder(std::move(builder));
}

MapBgfxSurface::HudPointer MapBgfxSurface::handleHudMouseEvent(QMouseEvent *event)
{
    return this->view->handleHudMouseEvent(event);
}

bool MapBgfxSurface::handleHudWheelEvent(QWheelEvent *event)
{
    return this->view->handleHudWheelEvent(event);
}

bool MapBgfxSurface::handleHudKeyPress(QKeyEvent *event)
{
    return this->view->handleHudKeyPress(event);
}

void MapBgfxSurface::setMapCursor(Qt::CursorShape shape)
{
    this->view->setMapCursor(shape);
}
