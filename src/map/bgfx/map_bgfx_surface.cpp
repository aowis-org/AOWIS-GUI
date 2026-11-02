#include "map/bgfx/map_bgfx_surface.h"

#include "map/bgfx/map_bgfx_backend.h"
#include "map/bgfx/map_bgfx_globe_hud_controller.h"
#include "map/core/map_model.h"
#include "map/data/map_terrain_repository.h"
#include "map/data/map_tile_repository.h"
#include "map/bgfx/map_bgfx_2d_picking.h"
#include "map/bgfx/map_bgfx_layer_visibility.h"
#include "map/core/map_scale_distance.h"
#include "map/render/map_render_diagnostics.h"
#include "map/core/map_scale_renderer.h"
#include "config/shortcut_registry.h"
#include "config/unit_aware_view.h"
#include "config/unit_profile_manager.h"
#include "map/bgfx/map_bgfx_2d_symbology.h"
#include "map/render/map_icon_atlas.h"
#include "network/infrastructure_entity_traits.h"
#include "network/network_symbology_rendering.h"

#include <QByteArray>
#include <QDebug>
#include <QGuiApplication>
#include <QFocusEvent>
#include <QFont>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPalette>
#include <QPlatformSurfaceEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QVector4D>
#include <QWindow>
#include <QtMath>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdint>
#include <utility>

#ifdef Q_OS_WASM
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

// Resolve the independent canvas within Qt's shadow-root screen, as tested
// by the existing bgfx probe. Emscripten also needs this alias for reset().
EM_JS(int, aowisBgfxMapPrepare, (), {
    const bridge = window.aowisBgfxMap;
    if (!bridge || !bridge.prepare() || !bridge.attach()
        || typeof specialHTMLTargets === 'undefined')
        return 0;
    specialHTMLTargets['!aowis-bgfx-map'] = bridge.canvas();
    return 1;
});
EM_JS(void, aowisBgfxMapGeometry, (int x, int y, int w, int h, int visible), {
    window.aowisBgfxMap?.geometry(x, y, w, h, Boolean(visible));
});
// Latest uncaught browser error recorded by index.html, written as UTF-8
// into buffer (NUL-terminated, truncated to capacity). Uses only HEAPU8 so
// it does not depend on optional Emscripten string helpers being linked.
EM_JS(int, aowisLastBrowserError, (char *buffer, int capacity), {
    const errors = window.aowisRecentErrors;
    const text = Array.isArray(errors) && errors.length > 0 ? errors[errors.length - 1] : "";
    const bytes = new TextEncoder().encode(String(text));
    const length = Math.min(bytes.length, capacity - 1);
    for (let index = 0; index < length; ++index)
        HEAPU8[buffer + index] = bytes[index];
    HEAPU8[buffer + length] = 0;
    return length;
});
EM_JS(void, aowisBgfxMapRemove, (), {
    if (typeof specialHTMLTargets !== 'undefined')
        delete specialHTMLTargets['!aowis-bgfx-map'];
    window.aowisBgfxMap?.remove();
});
#else
#if defined(Q_OS_LINUX) && defined(AOWIS_BGFX_NATIVE_WAYLAND_PRIVATE)
#include <qpa/qplatformnativeinterface.h>
#endif
#endif

namespace {
constexpr uint16_t kMapBgfxView = 0;
// bgfx::init/bgfx::shutdown own a process-global renderer. Normally the
// workspace has one shared surface: it keeps ownership even while a different
// main-navigation page is shown. A second surface can take ownership only
// when needed (initializeBackend() explicitly retires an inactive owner).
MapBgfxSurface *g_bgfx_device_owner = nullptr;
// Room for the editor overlay (one icon quad per visible node) on top of the
// HUD and imagery: bgfx's defaults are 6 MiB vertices / 2 MiB indices.
constexpr uint32_t kTransientVertexBytes = 16u << 20;
constexpr uint32_t kTransientIndexBytes = 8u << 20;
// Tile uploads per frame (~256 KiB each, atlas cell updates). Enough to fill
// a screen within a few frames, small enough to never cost a frame while
// streaming.
constexpr int kImageryUploadsPerFrame = 8;
// The globe replaces far more tiles per frame while zooming or turning
// (300-400 leaves at a tilted view): at 8 the uploads fell behind and the
// backlog showed as blank tiles; 12 keeps up (measured; 16 gained nothing).
constexpr int kGlobeImageryUploadsPerFrame = 12;
// VSYNC: presentation pacing (see MapFramePacer). MSAA x4: antialiased thick
// line and marker edges; bgfx clamps it to what the renderer supports.
// FLIP_AFTER_RENDER: with BGFX_CONFIG_MULTITHREADED=0 bgfx by default
// presents (swaps) the frame rendered by the previous bgfx::frame() call at
// the start of the next one. The map renders on demand, so the last frame
// before it goes idle was never shown: a released selection rectangle, a
// cleared tile selection or a just arrived tile stayed on screen as the
// frame before, until anything else asked for a redraw. Presenting right
// after rendering shows every frame (and one frame earlier).
#ifdef Q_OS_WASM
// NOT on WebGL: bgfx's OpenGL ES MSAA back buffer calls
// glFramebufferTexture2DMultisampleEXT (EXT_multisampled_render_to_texture),
// which WebGL does not provide. The function pointer stays NULL and the first
// frame traps with "RuntimeError: null function" inside
// RendererContextGL::submit(). The browser multisamples the canvas instead:
// the WebGL context is created with antialias=true (createBrowserContext()).
constexpr uint32_t kMapBgfxResetFlags = BGFX_RESET_VSYNC | BGFX_RESET_FLIP_AFTER_RENDER;
#else
constexpr uint32_t kMapBgfxResetFlags =
    BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4 | BGFX_RESET_FLIP_AFTER_RENDER;
#endif
// Fallback when the platform reports no usable refresh rate.
constexpr double kFallbackRefreshHz = 60.0;
#ifdef Q_OS_WASM
// The WebGL 2 context for the map canvas, created here instead of by bgfx
// (bgfx hard-codes antialias=false for the contexts it creates). bgfx adopts
// it through Init::platformData.context and destroys it on bgfx::shutdown().
// Opaque canvas (alpha=false), depth + stencil for the default framebuffer,
// browser multisampling for antialiased line and marker edges below a device
// pixel ratio of 1.75.
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE createBrowserContext()
{
    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    attributes.alpha = false;
    attributes.depth = true;
    attributes.stencil = true;
    // Multisampling quadruples fill and adds a resolve per frame; at a
    // device pixel ratio of 2 and more the physical pixels are small enough
    // that edges look smooth without it (and the map has 4x the pixels).
    attributes.antialias = emscripten_get_device_pixel_ratio() < 1.75;
    // Laptops with two GPUs: the map wants the fast one.
    attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    attributes.premultipliedAlpha = false;
    attributes.enableExtensionsByDefault = true;
    return emscripten_webgl_create_context("!aowis-bgfx-map", &attributes);
}

// requestAnimationFrame loops cannot be cancelled from C++. A loop is only
// valid while its generation matches; stop/restart/destruction bump it, so a
// stale callback returns EM_FALSE instead of touching a dead surface or
// running a second loop in parallel.
MapBgfxSurface *g_bgfx_frame_loop_surface = nullptr;
quintptr g_bgfx_frame_loop_generation = 0;
#endif
}

MapBgfxSurface::MapBgfxSurface(MapModel *model, QWidget *parent)
    : QWidget(parent), map_model(model), terrain_navigation(model)
{
    Q_ASSERT(this->map_model != nullptr);
    setObjectName(QStringLiteral("aowis-bgfx-monitor-surface"));
    this->camera_state = this->map_model->cameraViewState();
    this->globe_surface = std::make_unique<MapBgfxGlobeSurface>(this->map_model);
    // Rays and the visible terrain LOD come from the meshes the globe draws.
    this->terrain_navigation.setSurfacePreparation(&this->globe_surface->preparation());
    // --- Globe network (slice 3) --------------------------------------------
    // X-Ray classifies the network against the DEM the globe shows; the
    // network follows the shared vertical transform (exaggeration, ground
    // offset) of the terrain.
    this->globe_network.setTerrainElevationResolver(
        [this](const CoordinateWGS84 &coordinate, double *elevation_m, double *cell_size_m)
    {
        return this->terrain_navigation.terrainElevationAtCoordinate(coordinate, elevation_m, cell_size_m);
    });
    // Demand points (no elevation in the model) lie on the terrain as drawn.
    this->globe_network.setSurfaceHeightResolver(
        [this](const CoordinateWGS84 &coordinate, double *height_m)
    {
        return this->terrain_navigation.drawnSurfaceHeightAtCoordinate(coordinate, height_m);
    });
    this->globe_network.setGroundOffsetM(this->map_model->view3dNetworkGroundOffsetM());
    this->globe_network.setVerticalExaggeration(this->map_model->view3dVerticalExaggeration());
    this->globe_network.setUndergroundMode(this->underground_mode);
    connect(this->map_model, &MapModel::view3dNetworkGroundOffsetChanged, this, [this](double offset_m)
    {
        this->globe_network.setGroundOffsetM(offset_m);
        requestRedraw();
    });
    // --- Terrain-aware globe camera (slice 2), as the QRhi globe ----------
    // The orbit rig follows the DEM under the eye. Camera changes take a
    // filter step at once (and request the DEM tile under the eye); the
    // frame loop continues while it settles (renderFrameAt()). The height
    // offsets change the camera state, so they present frames by themselves.
    connect(this->map_model, &MapModel::viewModeChanged, this, [this](MapViewMode view_mode)
    {
        if (!this->globe_ready)
            return;
        this->terrain_navigation.stopFollowing();
        // Heatmap markers exist only on the globe.
        syncGlobeHeatmap();
        if (view_mode == MapViewMode::Globe)
        {
            // Window selection from scratch for the camera entering now.
            this->globe_surface->invalidateTerrainView();
            syncGlobeCameraHeight(true);
        }
        else
        {
            this->map_model->setViewGlobeCameraCollisionLiftM(0.0);
            // No X-Ray classification outside the globe.
            this->globe_underground_refresh_requested = false;
            ++this->globe_underground_refresh_generation;
        }
    });
    connect(this->map_model, &MapModel::centerChangedWGS84, this, [this]
    {
        // A network fit can move the centre before it sets the final
        // distance: while the one-shot prime is pending, request the DEM of
        // this centre at once; viewGlobeCameraChanged consumes the prime.
        const bool prime_terrain = this->globe_terrain_prime_pending
            && this->terrain_repository != nullptr && this->globe_ready
            && this->map_model->viewMode() == MapViewMode::Globe;
        syncGlobeCameraHeight(true);
        if (prime_terrain)
            this->globe_surface->requestTerrainForCurrentView(globeViewportSize());
    });
    connect(this->map_model, &MapModel::viewGlobeCameraChanged, this, [this]
    {
        const bool prime_terrain = this->globe_terrain_prime_pending
            && this->terrain_repository != nullptr && this->globe_ready
            && this->map_model->viewMode() == MapViewMode::Globe;
        if (prime_terrain)
            this->globe_terrain_prime_pending = false;
        syncGlobeCameraHeight(true);
        if (prime_terrain)
            this->globe_surface->requestTerrainForCurrentView(globeViewportSize());
        // Other terrain under the network: classify again once it settled.
        if (this->underground_mode == MapUndergroundMode::XRay)
        {
            this->globe_underground_refresh_requested = true;
            scheduleGlobeUndergroundRefresh();
        }
    });
    connect(this->map_model, &MapModel::view3dNavigationStateChanged, this,
            [this](MapView3dNavigationState state)
    {
        if (!this->globe_ready || this->map_model->viewMode() != MapViewMode::Globe)
            return;
        this->terrain_navigation.stopFollowing();
        // Entering an orbit is a pure state change (re-anchoring here would
        // jump the map before any orbit delta); the orbit's camera changes
        // drive the collision lift. Its end resumes ordinary following.
        if (state == MapView3dNavigationState::Rotate)
            return;
        syncGlobeCameraHeight(true);
    });
    connect(this->map_model, &MapModel::view3dVerticalExaggerationChanged, this, [this](double)
    {
        // Every terrain mesh is rebuilt with the new exaggeration (vertex
        // patches), the network is placed again; the rig follows the
        // exaggerated height under the eye.
        this->globe_surface->invalidateTerrain();
        this->globe_network.setVerticalExaggeration(this->map_model->view3dVerticalExaggeration());
        syncGlobeCameraHeight(false);
        requestRedraw();
    });
    // A provider switch reuses the same tile ids for different pixels. The
    // new imagery must be drawn at once (on-demand rendering): tiles already
    // in the repository cache arrive without any signalTileAvailable.
    connect(this->map_model, &MapModel::providerChanged, this, [this](MapProvider)
    {
        if (this->initialized)
            this->imagery_gpu.clearTextures();
        this->imagery_layout_key = 0;
        this->imagery_requests_dirty = true;
        // The new provider's tiles go out at once: the old provider's queued
        // requests are dropped and its transfers in flight no longer hold
        // the request slots (both map tabs share the repository; the
        // sources are the same for both, as their providers are synced).
        if (this->tile_repository != nullptr)
        {
            QSet<QString> wanted_sources;
            for (int zoom = MapModel::MinZoom; zoom <= MapModel::MaxZoom; ++zoom)
                wanted_sources.insert(this->map_model->tileSourcePath(zoom));
            this->tile_repository->supersedeTileSources(wanted_sources);
        }
        // The globe's window tiles carry provider-specific imagery keys:
        // without this it kept drawing the old provider's cached tiles.
        this->globe_surface->providerChanged();
        requestRedraw();
    });
    // A unit profile switch changes the scale bar label (HUD).
    new UnitAwareView(this, UnitProfileManager::instance().presentation(),
                      [this]() { requestRedraw(); });
    // Native HUD (slice 6a): app font, the QWidget HUD's images, a scale bar in
    // the user's distance unit from the continuous camera.
    this->hud.setFont(font());
    // The navigation keys in the joystick's / zoom buttons' tooltips follow
    // the shortcut settings.
    const std::function<void()> update_navigation_keys = [this]()
    {
        const GuiShortcutRegistry &registry = guiShortcutRegistry();
        MapBgfxHud::NavigationKeys keys;
        keys.pan_up = registry.shortcut(GuiShortcutId::MapPanUp);
        keys.pan_down = registry.shortcut(GuiShortcutId::MapPanDown);
        keys.pan_left = registry.shortcut(GuiShortcutId::MapPanLeft);
        keys.pan_right = registry.shortcut(GuiShortcutId::MapPanRight);
        keys.zoom_in = registry.shortcut(GuiShortcutId::MapZoomIn);
        keys.zoom_out = registry.shortcut(GuiShortcutId::MapZoomOut);
        this->hud.setNavigationKeys(keys);
        requestRedraw();
    };
    update_navigation_keys();
    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged, this,
            [update_navigation_keys](GuiShortcutId) { update_navigation_keys(); });
    this->hud.setImages(QImage(QStringLiteral(":/icon/crosshair.png")),
                        QImage(QStringLiteral(":/icon/fullscreen.png")),
                        QImage(QStringLiteral(":/icon/fullscreen_undo.png")),
                        QImage(QStringLiteral(":/icon/zoom_in.png")),
                        QImage(QStringLiteral(":/icon/zoom_out.png")));
    this->hud.setScaleProvider([](double lon0, double lat0, double lon1, double lat1,
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
    this->hud_clock.start();
    this->debug_overlay_enabled = qEnvironmentVariableIntValue("AOWIS_BGFX_DEBUG") != 0;
    this->frame_timer.setTimerType(Qt::PreciseTimer);
    connect(&this->frame_timer, &QTimer::timeout, this, &MapBgfxSurface::renderFrame);

#ifndef Q_OS_WASM
    setFocusPolicy(Qt::StrongFocus);
    this->native_window = new QWindow();
    // createWindowContainer embeds a separate native QWindow. Its key events
    // are not delivered to MapWidget once keyboard focus moves to bgfx.
    this->native_window->installEventFilter(this);
    // The surface type only decides what Qt prepares for the native view.
    // macOS: bgfx selects Metal there, which needs a CAMetalLayer-backed
    // NSView. Elsewhere one HWND / X11 window / wl_surface serves OpenGL,
    // Vulkan and Direct3D alike.
#ifdef Q_OS_MACOS
    this->native_window->setSurfaceType(QSurface::MetalSurface);
#else
    this->native_window->setSurfaceType(QSurface::OpenGLSurface);
#endif
    QWidget *container = QWidget::createWindowContainer(this->native_window, this);
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(container);
#endif
    // The frame timer samples the continuous camera, NOT integer tile zoom.
    // Signal connections are deliberately unnecessary: a camera move causes
    // no scene changes or uploads in this early backend.
}

MapBgfxSurface::~MapBgfxSurface()
{
    if (MapRenderDiagnostics::enabled())
        qInfo() << "AOWIS map shutdown: bgfx surface destructor starting";
    stopFrameLoop();
#ifdef Q_OS_WASM
    if (g_bgfx_frame_loop_surface == this)
        g_bgfx_frame_loop_surface = nullptr;
#endif
    if (this->initialized) {
        releaseGpuResources();
        bgfx::shutdown();
        MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::BgfxDeviceStopped);
        this->initialized = false;
        if (g_bgfx_device_owner == this)
            g_bgfx_device_owner = nullptr;
    }
    if (g_bgfx_device_owner == this)
        g_bgfx_device_owner = nullptr;
#ifdef Q_OS_WASM
    // The canvas is shared; only the last surface removes it.
    if (g_bgfx_device_owner == nullptr)
        aowisBgfxMapRemove();
#endif
    if (MapRenderDiagnostics::enabled())
        qInfo() << "AOWIS map shutdown: bgfx surface destructor completed";
}

void MapBgfxSurface::setNavigationHandlers(NavigationKeyHandler press,
                                           NavigationKeyHandler release,
                                           std::function<void()> cancel)
{
    this->navigation_key_press = std::move(press);
    this->navigation_key_release = std::move(release);
    this->navigation_cancel = std::move(cancel);
}

void MapBgfxSurface::setNavigationWheelHandler(NavigationWheelHandler wheel)
{
    this->navigation_wheel = std::move(wheel);
}

void MapBgfxSurface::setNavigationFrameHandler(NavigationFrameHandler advance)
{
    this->navigation_frame = std::move(advance);
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
    // Monitor: QWidget focus on this surface delivers the keys (keyPressEvent)
    // wherever an embedded window cannot be activated by itself (Wayland: a
    // subsurface never gets keyboard focus of its own, so requestActivate()
    // alone left the keys with whatever widget had them); where it can
    // (X11, Windows, macOS) the native window then takes them.
    window()->activateWindow();
    setFocus(reason);
    if (this->native_window != nullptr)
        this->native_window->requestActivate();
#else
    Q_UNUSED(reason)
#endif
}

void MapBgfxSurface::setKeyboardFocusWidget(QWidget *widget)
{
    this->keyboard_focus_widget = widget;
#ifndef Q_OS_WASM
    // Clicks must not activate the embedded window, or the top-level loses
    // its focus widget (and the editor's window shortcuts with it).
    if (this->native_window != nullptr)
        this->native_window->setFlag(Qt::WindowDoesNotAcceptFocus, widget != nullptr);
#endif
}

bool MapBgfxSurface::eventFilter(QObject *watched, QEvent *event)
{
#ifndef Q_OS_WASM
    // Platforms that destroy and recreate the whole native window announce
    // it here: bgfx must let go of the old surface first and start again on
    // the new one. This is only the fallback; Qt Wayland destroys the
    // wl_surface on reparent WITHOUT this event, so planned reparents
    // (fullscreen) go through suspendForNativeWindowChange() beforehand.
    if (watched == this->native_window && event->type() == QEvent::PlatformSurface)
    {
        const QPlatformSurfaceEvent *surface_event = static_cast<QPlatformSurfaceEvent *>(event);
        if (surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        {
            shutdownBackend();
        }
        else if (surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated
                 && this->reinitialize_on_surface)
        {
            scheduleBackendRestart();
        }
    }
    // A suspended backend restarts once the (new) surface is on screen.
    // Exposed again (uncovered, restored): present a fresh frame.
    if (watched == this->native_window && event->type() == QEvent::Expose)
        requestRedraw();
    if (watched == this->native_window && event->type() == QEvent::Expose
        && this->reinitialize_on_surface && this->native_window->isExposed())
    {
        scheduleBackendRestart();
    }
    if (watched == this->native_window && this->initialized)
    {
        if (event->type() == QEvent::KeyPress && handleHudKeyPress(static_cast<QKeyEvent *>(event)))
            return true;
        if (event->type() == QEvent::KeyPress && this->navigation_key_press)
            return this->navigation_key_press(static_cast<QKeyEvent *>(event));
        if (event->type() == QEvent::KeyRelease && this->navigation_key_release)
            return this->navigation_key_release(static_cast<QKeyEvent *>(event));
        // The native window fills this surface, which has the same geometry
        // as MapWidget in the stack, so the position is a valid anchor as-is.
        // The HUD sees the wheel first: over a HUD panel it scrolls a list
        // or is swallowed, never zooming the map underneath.
        if (event->type() == QEvent::Wheel && this->navigation_wheel)
        {
            QWheelEvent *wheel = static_cast<QWheelEvent *>(event);
            if (!handleHudWheelEvent(wheel))
                this->navigation_wheel(wheel);
            return true;
        }
        // Mouse: same coordinates as MapWidget for the same reason. A QWindow
        // receives every move (no mouse-tracking flag), and it receives the
        // moves/release of a drag that started on it (implicit grab).
        // Double clicks are forwarded unchanged (type DblClick) through the
        // press handler, so the receiver sees exactly the press + double-click
        // sequence Qt delivers to an ordinary QWidget.
        if ((event->type() == QEvent::MouseButtonPress
             || event->type() == QEvent::MouseButtonDblClick)
            && this->navigation_mouse_press)
        {
            // A click (on the map or a HUD element) must also bring keyboard
            // focus back to the map, otherwise zoom/pan keys keep going to
            // the previous widget.
            focusNavigationInput(Qt::MouseFocusReason);
            return this->navigation_mouse_press(static_cast<QMouseEvent *>(event));
        }
        if (event->type() == QEvent::MouseButtonRelease && this->navigation_mouse_release)
            return this->navigation_mouse_release(static_cast<QMouseEvent *>(event));
        if (event->type() == QEvent::MouseMove && this->navigation_mouse_move)
            return this->navigation_mouse_move(static_cast<QMouseEvent *>(event));
        if (event->type() == QEvent::FocusOut && this->navigation_cancel)
            this->navigation_cancel();
        // Like a Qt popup, an open HUD popup closes when the map loses focus.
        if (event->type() == QEvent::FocusOut)
            this->hud.closePopup();
        // The pointer left the map: no HUD button stays hover-highlighted.
        if (event->type() == QEvent::Leave)
            this->hud.clearHover();
    }
#else
    Q_UNUSED(watched)
    Q_UNUSED(event)
#endif
    return QWidget::eventFilter(watched, event);
}

void MapBgfxSurface::keyPressEvent(QKeyEvent *event)
{
    if (handleHudKeyPress(event))
        return;
    if (this->initialized && this->navigation_key_press
        && this->navigation_key_press(event))
        return;
    QWidget::keyPressEvent(event);
}

void MapBgfxSurface::keyReleaseEvent(QKeyEvent *event)
{
    if (this->initialized && this->navigation_key_release
        && this->navigation_key_release(event))
        return;
    QWidget::keyReleaseEvent(event);
}

void MapBgfxSurface::focusOutEvent(QFocusEvent *event)
{
    if (this->navigation_cancel)
        this->navigation_cancel();
    QWidget::focusOutEvent(event);
}

QString MapBgfxSurface::graphicsApiName() const
{
    if (!this->initialized)
        return QStringLiteral("bgfx (not initialized)");
#ifdef Q_OS_WASM
    return QStringLiteral("bgfx / WebGL 2");
#else
    return QStringLiteral("bgfx / %1")
        .arg(QString::fromLatin1(bgfx::getRendererName(bgfx::getRendererType())));
#endif
}

void MapBgfxSurface::setNavigationMouseHandlers(NavigationMouseHandler press,
                                                NavigationMouseHandler release,
                                                NavigationMouseHandler move)
{
    this->navigation_mouse_press = std::move(press);
    this->navigation_mouse_release = std::move(release);
    this->navigation_mouse_move = std::move(move);
}

QWindow *MapBgfxSurface::nativeInputWindow() const
{
#ifdef Q_OS_WASM
    return nullptr;
#else
    return this->native_window;
#endif
}

void MapBgfxSurface::setTerrainRepository(MapTerrainRepository *terrain_repository)
{
    requestRedraw();
    if (this->terrain_repository == terrain_repository)
        return;
    if (this->terrain_repository != nullptr)
        QObject::disconnect(this->terrain_repository, nullptr, this, nullptr);
    this->terrain_repository = terrain_repository;
    this->globe_overlay_height_cache.clear();
    this->globe_surface->setTerrainRepository(terrain_repository);
    this->terrain_navigation.setTerrainRepository(terrain_repository);
    if (this->terrain_repository == nullptr)
        return;
    // DEM tiles feed the terrain meshes (built asynchronously, applied by
    // the next frames); a retryable tile is requested again by the next
    // frame's preparation.
    connect(this->terrain_repository, &MapTerrainRepository::signalTerrainTileAvailable, this,
            [this](const QString &key)
    {
        this->globe_surface->notifyTerrainTileAvailable(key);
        // The DEM under the eye may just have arrived.
        syncGlobeCameraHeight(false);
        if (this->underground_mode == MapUndergroundMode::XRay)
        {
            this->globe_underground_refresh_requested = true;
            scheduleGlobeUndergroundRefresh();
        }
        // Only the globe draws DEM tiles: in 2D (and while hidden) an
        // arriving tile is no reason for a frame. Both map tabs share the
        // repository, so this ran for both surfaces on every arrival.
        if (this->globe_ready && this->map_model->viewMode() == MapViewMode::Globe)
            requestRedraw();
    });
    connect(this->terrain_repository, &MapTerrainRepository::signalTerrainTileRetryReady, this,
            [this](const QString &)
    {
        if (this->globe_ready && this->map_model->viewMode() == MapViewMode::Globe)
            requestRedraw();
    });
    // A terrain cache deletion dropped tiles (Map Editor). Nothing is torn
    // down: the drawn meshes stay until their tiles arrive again - the next
    // frame's requestMissingTerrainTiles() asks for them (the terrain
    // service regenerates them) and each arrival rebuilds its mesh
    // (notifyTerrainTileAvailable()), no flash, no full rebuild. The camera
    // height follows the terrain under it once more.
    connect(this->terrain_repository, &MapTerrainRepository::signalTerrainTilesDeleted, this,
            [this]
    {
        syncGlobeCameraHeight(true);
        requestRedraw();
    });
    // The terrain catalog arrived (or was reset): the next frame's
    // preparation takes the dataset's zoom range and rebuilds the window.
    connect(this->terrain_repository, &MapTerrainRepository::signalTerrainDatasetChanged, this,
            [this]
    {
        syncGlobeCameraHeight(true);
        requestRedraw();
    });
    // Possibly already in Globe mode: the height under the camera now, with
    // priority for that exact DEM tile.
    syncGlobeCameraHeight(true);
}

QSize MapBgfxSurface::globeViewportSize() const
{
    const QSizeF logical = logicalSize();
    return QSize(std::max(1, qRound(logical.width())), std::max(1, qRound(logical.height())));
}

void MapBgfxSurface::syncGlobeCameraHeight(bool request_missing_tile)
{
    if (!this->globe_ready)
        return;
    this->terrain_navigation.setViewportSize(globeViewportSize());
    this->terrain_navigation.syncCameraHeight(request_missing_tile);
}

bool MapBgfxSurface::terrainCoordinateAtScreen(const QPointF &screen_position,
                                               CoordinateWGS84 *coordinate, bool)
{
    // Without the bgfx globe drawn, a Globe ray meets the ellipsoid (no
    // retained meshes); 2D reads the flat map with a cached DEM altitude.
    this->terrain_navigation.setViewportSize(globeViewportSize());
    return this->terrain_navigation.terrainCoordinateAtScreen(screen_position, coordinate);
}

bool MapBgfxSurface::cachedTerrainElevationAtCoordinate(const CoordinateWGS84 &coordinate,
                                                        double *elevation_m) const
{
    return this->terrain_navigation.cachedTerrainElevationAtCoordinate(coordinate, elevation_m);
}

bool MapBgfxSurface::globeSurfaceRayHitAtScreen(const QPointF &screen_position,
                                                MapGlobeSurfaceHit *hit) const
{
    // Viewport as of the last frame or resize.
    return this->terrain_navigation.surfaceRayHitAtScreen(screen_position, hit);
}

bool MapBgfxSurface::globeSurfacePointAtScreen(const QPointF &screen_position,
                                               GeoWgs84Ellipsoid::EcefPositionD *point)
{
    if (!this->globe_ready)
        return false;
    this->terrain_navigation.setViewportSize(globeViewportSize());
    MapGlobeSurfaceHit hit;
    if (!this->terrain_navigation.surfaceRayHitAtScreen(screen_position, &hit))
        return false;
    *point = hit.ecef_position;
    return true;
}

void MapBgfxSurface::setTerrainWireframeVisible(bool visible)
{
    requestRedraw();
    this->globe_surface->setWireframeVisible(visible);
}

void MapBgfxSurface::setMapTilesVisible(bool visible)
{
    requestRedraw();
    this->globe_surface->setMapVisible(visible);
}

void MapBgfxSurface::setTerrainHillshadeVisible(bool visible)
{
    requestRedraw();
    this->globe_surface->setHillshadeVisible(visible);
}

bool MapBgfxSurface::terrainHillshadeVisible() const
{
    return this->globe_surface->hillshadeVisible();
}

bool MapBgfxSurface::terrainWireframeVisible() const
{
    return this->globe_surface->preparation().wireframeVisible();
}

bool MapBgfxSurface::mapTilesVisible() const
{
    return this->globe_surface->mapVisible();
}

void MapBgfxSurface::setNetworkSnapshot(const NetworkRenderSnapshot &snapshot)
{
    requestRedraw();
    // The globe builds its geometry on its next frame (implicitly shared).
    this->globe_network.setNetworkSnapshot(snapshot);
    // Retained geometry revisions change only with model changes, not zoom.
    const MapRetainedNetworkUpdate update = this->retained_scene.setSnapshot(snapshot);
    // New entities or renamed ones: the labels read the IDs again.
    if (update.geometry_changed || update.visual_changed)
        this->labels_dirty = true;
    if (update.geometry_changed)
    {
        // Identity for picking (hit -> uuid) and highlights (uuid -> entity).
        this->uuid_by_entity_key.clear();
        this->entity_key_by_uuid.clear();
        const NetworkRenderSnapshot &retained = this->retained_scene.snapshot();
        for (const NetworkRenderNode &node : retained.nodes)
        {
            const quint64 key = MapBgfx2dScene::entityKey(node.entity_type, node.render_id);
            this->uuid_by_entity_key.insert(key, node.uuid);
            this->entity_key_by_uuid.insert(node.uuid, key);
        }
        for (const NetworkRenderLink &link : retained.links)
        {
            const quint64 key = MapBgfx2dScene::entityKey(link.entity_type, link.render_id);
            this->uuid_by_entity_key.insert(key, link.uuid);
            this->entity_key_by_uuid.insert(link.uuid, key);
        }
        this->highlights_dirty = true;
        // New render ids: entity classes and the uuid-based states again.
        this->entity_classes_dirty = true;
        this->entity_state_dirty = true;
    }
    // A network opened in Globe mode: load the DEM of the current camera now
    // and arm the one-shot for the fitted camera that normally follows
    // (signalNetworkLoaded fits after the snapshot).
    if (update.geometry_changed && this->globe_ready && this->terrain_repository != nullptr
        && this->map_model->viewMode() == MapViewMode::Globe)
    {
        this->globe_terrain_prime_pending = true;
        syncGlobeCameraHeight(true);
        this->globe_surface->requestTerrainForCurrentView(globeViewportSize());
    }
    if (update.geometry_changed && this->prepared_2d_scenes.prepare(this->retained_scene))
    {
        MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::Bgfx2dScenePrepared);
        qInfo() << "bgfx retained geometry prepared" << snapshot.geometry_revision
                << "links" << this->prepared_2d_scenes.active().linkPolylines().size()
                << "link segments" << this->prepared_2d_scenes.active().linkSegmentCount()
                // Demand points are snapshot nodes (entity DemandPoint).
                << "nodes incl. demand points" << this->prepared_2d_scenes.active().networkNodes().size()
                << "attachments" << this->prepared_2d_scenes.active().attachments().size();
    }
    // The heatmap's markers sit on the nodes.
    if (update.geometry_changed)
        syncGlobeHeatmap();
}

void MapBgfxSurface::setNodeDeclutteringEnabled(bool enabled)
{
    if (this->prepared_2d_scenes.active().declutteringEnabled() == enabled)
        return;
    requestRedraw();
    // Editor and monitor share one retained geometry revision but need two
    // node placements. Cache both derived scenes; revisiting an unchanged
    // variant must not perform another CPU geometry preparation.
    if (this->prepared_2d_scenes.setDeclutteringEnabled(enabled, this->retained_scene))
        MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::Bgfx2dScenePrepared);
    this->globe_network.setNodeDeclutteringEnabled(enabled);
}

void MapBgfxSurface::setSelectedEntity(InfrastructureEntity entity_type, const QUuid &uuid)
{
    requestRedraw();
    if (this->selected_entity_type == entity_type && this->selected_entity_uuid == uuid)
        return;
    this->selected_entity_type = entity_type;
    this->selected_entity_uuid = uuid;
    this->highlights_dirty = true;
    this->entity_state_dirty = true;
    this->globe_network.setSelectedEntity(entity_type, uuid);
}

void MapBgfxSurface::setSimulationErrorEntities(
    const QHash<QUuid, InfrastructureEntity> &error_entities,
    const QSet<QUuid> &stale_entity_uuids)
{
    requestRedraw();
    if (this->simulation_error_entities == error_entities
        && this->simulation_stale_entity_uuids == stale_entity_uuids)
        return;
    this->simulation_error_entities = error_entities;
    this->simulation_stale_entity_uuids = stale_entity_uuids;
    this->highlights_dirty = true;
    this->entity_state_dirty = true;
    this->globe_network.setSimulationErrorEntities(error_entities, stale_entity_uuids);
}

MapRenderHit MapBgfxSurface::hitTest(const QPointF &screen_position) const
{
    MapRenderHit no_hit;
    if (!this->distribution_network_visible || this->map_model == nullptr
        || !this->prepared_2d_scenes.active().isPrepared())
        return no_hit;
    const QSizeF logical = logicalSize();
    if (!std::isfinite(screen_position.x()) || !std::isfinite(screen_position.y())
        || screen_position.x() < 0.0 || screen_position.y() < 0.0
        || screen_position.x() > logical.width() || screen_position.y() > logical.height())
        return no_hit;
    // The model's current camera, not the last presented frame: a click
    // right after a zoom step must hit what the next frame shows.
    const MapCameraViewState state = this->map_model->cameraViewState();
    if (state.mode == MapViewMode::Globe)
    {
        // The network as the globe draws it (only while it is shown).
        if (!this->globe_network_ready)
            return no_hit;
        const QSize viewport = globeViewportSize();
        MapGlobeCamera camera;
        camera.setViewportSize(viewport);
        camera.syncFromMapModel(*this->map_model);
        return this->globe_network.hitTest(screen_position, camera, viewport);
    }
    if (state.mode != MapViewMode::TwoD)
        return no_hit;
    const MapBgfx2d::Camera camera = MapBgfx2d::cameraFor(
        state.center_lon_deg, state.center_lat_deg, state.continuous_2d_zoom,
        logical.width(), logical.height());
    const MapBgfx2dSymbology::Sizes sizes = MapBgfx2dSymbology::sizes(
        this->symbology, state.center_lat_deg, state.continuous_2d_zoom);
    MapBgfx2dPicking::Options options;
    options.icon_size_px = sizes.icon_size_px;
    options.node_half_px = sizes.node_half_px;
    options.link_width_px = sizes.link_width_px;
    options.show_icons = this->symbology.show_icons;
    options.show_junctions = this->symbology.show_junctions;
    options.has_icon = [](InfrastructureEntity entity_type) { return mapHasIcon(entity_type); };
    const MapBgfx2dPicking::Hit hit = MapBgfx2dPicking::pick(
        this->prepared_2d_scenes.active(), camera, {screen_position.x(), screen_position.y()}, options);
    if (!hit.isValid())
        return no_hit;
    MapRenderHit result;
    result.render_id = hit.render_id;
    result.entity_type = hit.entity_type;
    result.uuid = this->uuid_by_entity_key.value(
        MapBgfx2dScene::entityKey(hit.entity_type, hit.render_id));
    return result;
}

void MapBgfxSurface::synchronizeHighlights()
{
    if (!this->prepared_2d_scenes.active().isPrepared())
        return;
    if (!this->highlights_dirty
        && this->highlight_preparation_id == this->prepared_2d_scenes.active().preparationId()
        && this->highlight_show_junctions == this->symbology.show_junctions)
        return;

    // A uuid only counts when it still resolves to an entity of that type.
    const std::function<bool(const QUuid &, InfrastructureEntity, quint64 *)> resolve =
        [this](const QUuid &uuid, InfrastructureEntity entity_type, quint64 *key)
    {
        if (uuid.isNull() || entity_type == InfrastructureEntity::Unknown)
            return false;
        const QHash<QUuid, quint64>::const_iterator found = this->entity_key_by_uuid.constFind(uuid);
        if (found == this->entity_key_by_uuid.cend()
            || found.value() != MapBgfx2dScene::entityKey(entity_type, quint32(found.value())))
            return false;
        *key = found.value();
        return true;
    };

    // Colours (selection; errors red, stale results grey) come from the
    // entity flags (synchronizeEntityStyle()); only the geometry is here.
    QSet<quint64> selected_keys;
    quint64 selected_key = 0;
    const bool selected_has_error =
        resolve(this->selected_entity_uuid, this->selected_entity_type, &selected_key)
        && this->simulation_error_entities.value(this->selected_entity_uuid,
                                                 InfrastructureEntity::Unknown)
            == this->selected_entity_type;
    if (selected_key != 0)
        selected_keys.insert(selected_key);

    QSet<quint64> diagnostic_keys;
    for (QHash<QUuid, InfrastructureEntity>::const_iterator error =
             this->simulation_error_entities.cbegin();
         error != this->simulation_error_entities.cend(); ++error)
    {
        quint64 key = 0;
        if (resolve(error.key(), error.value(), &key))
            diagnostic_keys.insert(key);
    }

    const bool show_junctions = this->symbology.show_junctions;
    if (!this->gpu_2d_scene.setHighlights(this->prepared_2d_scenes.active().filtered(selected_keys), selected_has_error,
                                          this->prepared_2d_scenes.active().filtered(diagnostic_keys)))
        qWarning() << "bgfx selection/error highlight upload failed";
    this->highlights_dirty = false;
    this->highlight_preparation_id = this->prepared_2d_scenes.active().preparationId();
    this->highlight_show_junctions = show_junctions;
}

void MapBgfxSurface::releaseGpuResources()
{
    this->entity_style.release();
    this->globe_network.release();
    this->globe_network_ready = false;
    this->globe_surface->release();
    this->globe_ready = false;
    this->imagery_gpu.release();
    this->gpu_2d_scene.release();
    this->hud_gpu.release();
    this->overlay_gpu.release();
    this->label_gpu.release();
    // Everything staged on the GPU is gone with the device.
    this->highlights_dirty = true;
    this->labels_dirty = true;
    this->imagery_layout_key = 0;
    this->imagery_requests_dirty = true;
    this->last_physical_size = QSize();
}

void MapBgfxSurface::shutdownBackend()
{
    if (!this->initialized)
        return;
    stopFrameLoop();
    this->globe_overlay_height_cache.clear();
    releaseGpuResources();
    bgfx::shutdown();
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::BgfxDeviceStopped);
    this->initialized = false;
    if (g_bgfx_device_owner == this)
        g_bgfx_device_owner = nullptr;
    this->reinitialize_on_surface = true;
}

void MapBgfxSurface::scheduleBackendRestart()
{
    // Queued: the platform window must be fully set up, and the widget may
    // become visible only after the surface was created (or the other way
    // round); show, SurfaceCreated and Expose all end here and the first one
    // that finds a mapped, exposed window starts the backend. Starting on an
    // unexposed Wayland surface would block the first vsync swap.
    QTimer::singleShot(0, this, [this]()
    {
        if (!this->reinitialize_on_surface || this->initialized || this->failed || !isVisible())
            return;
#ifndef Q_OS_WASM
        if (this->native_window == nullptr || this->native_window->handle() == nullptr
            || !this->native_window->isExposed())
        {
            return;
        }
#endif
        this->reinitialize_on_surface = false;
        if (initializeBackend())
        {
            startFrameLoop();
            return;
        }
        if (this->device_busy)
        {
            // Another visible map still renders; try again shortly.
            this->reinitialize_on_surface = true;
            QTimer::singleShot(250, this, [this]() { scheduleBackendRestart(); });
            return;
        }
        fail(QStringLiteral("bgfx could not restart on the recreated map window"));
    });
}

void MapBgfxSurface::suspendForNativeWindowChange()
{
    // bgfx::shutdown() destroys the swapchain and the VkSurface / EGL window
    // while the native surface still exists. Without this, Qt Wayland
    // destroys the wl_surface under a live Vulkan swapchain on reparent and
    // Mesa's explicit-sync object then hits a dead surface: fatal Wayland
    // protocol error ("Surface got destroyed already").
    shutdownBackend();
}

void MapBgfxSurface::resumeAfterNativeWindowChange()
{
    requestRedraw();
    // Restarts when the window is exposed again (see the Expose handling in
    // eventFilter()); no-op when the backend was not running.
    scheduleBackendRestart();
}

void MapBgfxSurface::setHudActionHandler(std::function<void(const MapBgfxHud::Command &)> handler)
{
    this->hud_action_handler = std::move(handler);
}

void MapBgfxSurface::setHudLegend(const MapBgfxHud::Legend &legend)
{
    requestRedraw();
    this->hud.setLegend(legend);
}

void MapBgfxSurface::closeHudPopup()
{
    requestRedraw();
    this->hud.closePopup();
}

void MapBgfxSurface::setHudViewSwitchVisible(bool visible)
{
    requestRedraw();
    this->hud.setViewSwitchVisible(visible);
}

void MapBgfxSurface::requestRedraw()
{
    this->redraw_requested = true;
}

void MapBgfxSurface::setOverlayBuilder(OverlayBuilder builder)
{
    requestRedraw();
    this->overlay_builder = std::move(builder);
}

void MapBgfxSurface::setGlobeOverlayBuilder(GlobeOverlayBuilder builder)
{
    requestRedraw();
    this->globe_overlay_builder = std::move(builder);
}

void MapBgfxSurface::setLabelSettings(const MapLabelSettings &settings)
{
    requestRedraw();
    if (this->label_settings == settings)
        return;
    this->label_settings = settings;
    this->labels_dirty = true;
}

void MapBgfxSurface::setLabelValueFormats(const MapBgfxLabelValueFormat &node_format,
                                          const MapBgfxLabelValueFormat &link_format)
{
    requestRedraw();
    if (this->label_node_format == node_format && this->label_link_format == link_format)
        return;
    this->label_node_format = node_format;
    this->label_link_format = link_format;
    this->labels_dirty = true;
}

void MapBgfxSurface::setLabelSymbolRects(MapBgfxLabels::SymbolRect node_symbol,
                                         MapBgfxLabels::SymbolRect link_symbol)
{
    requestRedraw();
    this->label_node_symbol = std::move(node_symbol);
    this->label_link_symbol = std::move(link_symbol);
    this->labels_dirty = true;
}

void MapBgfxSurface::setHiddenEntityUuids(const QSet<QUuid> &uuids)
{
    requestRedraw();
    if (this->hidden_entity_uuids == uuids)
        return;
    this->hidden_entity_uuids = uuids;
    // Only an entity flag changes (the shaders drop hidden entities).
    this->entity_state_dirty = true;
    this->globe_network.setHiddenEntityUuids(uuids);
    syncGlobeHeatmap();
    // Moved entities lose their labels during the move.
    this->labels_dirty = true;
}

void MapBgfxSurface::setNetworkScreenTranslation(const QPointF &translation)
{
    requestRedraw();
    this->network_screen_translation = translation;
}

bool MapBgfxSurface::handleHudWheelEvent(QWheelEvent *event)
{
    requestRedraw();
    if (event == nullptr || !this->initialized)
        return false;
    const QPointF position = event->position();
    if (this->hud.wheelZoomsAtCrosshair(position.x(), position.y()) && this->navigation_wheel)
    {
        // Over the joystick / zoom buttons: the map zooms about the
        // crosshair, as if the pointer were there.
        const QSizeF logical = logicalSize();
        const QPointF centre(0.5 * logical.width(), 0.5 * logical.height());
        QWheelEvent anchored(centre, event->globalPosition() - position + centre, event->pixelDelta(),
                             event->angleDelta(), event->buttons(), event->modifiers(), event->phase(),
                             event->inverted(), event->source());
        this->navigation_wheel(&anchored);
        return true;
    }
    const bool consumed = this->hud.mouseWheel(position.x(), position.y(), double(event->angleDelta().y()));
    // The wheel over the compass turns the globe.
    dispatchHudCommands();
    return consumed;
}

void MapBgfxSurface::dispatchHudCommands()
{
    const QVector<MapBgfxHud::Command> commands = this->hud.takeCommands();
    if (commands.isEmpty() || !this->hud_action_handler)
        return;
    // Deferred as the button actions (handleHudMouseEvent), in order and in
    // one go: a drag's Begin / Move / End reach the handler as they came.
    const std::function<void(const MapBgfxHud::Command &)> handler = this->hud_action_handler;
    QTimer::singleShot(0, this, [handler, commands]()
    {
        for (const MapBgfxHud::Command &command : commands)
            handler(command);
    });
}

bool MapBgfxSurface::handleHudKeyPress(QKeyEvent *event)
{
    requestRedraw();
    if (event == nullptr || !this->initialized || event->key() != Qt::Key_Escape)
        return false;
    return this->hud.closePopup();
}

void MapBgfxSurface::setHudGpsPosition(bool valid, double longitude_deg, double latitude_deg)
{
    requestRedraw();
    this->hud.setGps(valid, longitude_deg, latitude_deg);
}

void MapBgfxSurface::setHudDownloadActivity(int tiles_active, int tiles_queued,
                                            int terrain_active, int terrain_queued)
{
    // The monitor polls these every 400 ms: unchanged numbers present no
    // frame (while busy, the busy bars animate the HUD anyway).
    const int activity[4] = {tiles_active, tiles_queued, terrain_active, terrain_queued};
    if (!std::equal(std::begin(activity), std::end(activity), std::begin(this->hud_download_activity)))
        requestRedraw();
    std::copy(std::begin(activity), std::end(activity), std::begin(this->hud_download_activity));
    this->hud.setDownloadActivity(tiles_active, tiles_queued, terrain_active, terrain_queued);
}

void MapBgfxSurface::setHudFullscreen(bool active)
{
    requestRedraw();
    this->hud.setFullscreen(active);
}

void MapBgfxSurface::setHudJoystick(const QPointF &deflection, bool active)
{
    requestRedraw();
    this->hud.setJoystick(deflection, active);
}

void MapBgfxSurface::setHudKeyboardPan(const QPointF &direction)
{
    requestRedraw();
    this->hud.setKeyboardPan(direction);
}

MapBgfxSurface::HudPointer MapBgfxSurface::handleHudMouseEvent(QMouseEvent *event)
{
    HudPointer result;
    if (event == nullptr || !this->initialized)
        return result;
    const QPointF position = event->position();
    result.over_panel = this->hud.blocksInput(position.x(), position.y());
    switch (event->type())
    {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            requestRedraw();
            if (event->button() == Qt::LeftButton)
                result.consumed = this->hud.mousePress(position.x(), position.y());
            else if (event->button() == Qt::RightButton)
                result.consumed = this->hud.mousePressRight(position.x(), position.y());
            else
                result.consumed = result.over_panel;
            // A press on a globe slider or the compass starts its drag.
            dispatchHudCommands();
            break;
        case QEvent::MouseButtonRelease:
            requestRedraw();
            if (event->button() == Qt::LeftButton)
            {
                MapBgfxHud::Command command;
                const MapBgfxHud::Action action = this->hud.mouseRelease(
                    position.x(), position.y(), &result.consumed, &command);
                if (action != MapBgfxHud::Action::None && this->hud_action_handler)
                {
                    // Deferred: an action may reparent this widget (fullscreen),
                    // which must not happen inside the native window's event.
                    const std::function<void(const MapBgfxHud::Command &)> handler =
                        this->hud_action_handler;
                    QTimer::singleShot(0, this, [handler, command]() { handler(command); });
                }
                // The end of a slider / compass drag (after the button
                // action, which a drag never has).
                dispatchHudCommands();
            }
            else
            {
                result.consumed = result.over_panel;
            }
            break;
        case QEvent::MouseMove:
        {
            result.consumed = this->hud.mouseMove(position.x(), position.y(), nullptr);
            // Slider and compass drags.
            dispatchHudCommands();
            switch (this->hud.cursor())
            {
                case MapBgfxHud::Cursor::PointingHand:
                    result.cursor = Qt::PointingHandCursor;
                    break;
                case MapBgfxHud::Cursor::Cross:
                    result.cursor = Qt::CrossCursor;
                    break;
                case MapBgfxHud::Cursor::Arrow:
                    result.cursor = Qt::ArrowCursor;
                    break;
            }
            // Hover feedback (button highlight, ramp swatch, tooltip) only
            // where the HUD is under the pointer, and once more when it
            // leaves it: plain moves over the map present nothing.
            const bool hud_involved = result.consumed || result.over_panel
                || result.cursor != Qt::ArrowCursor;
            if (hud_involved || this->hud_pointer_involved)
                requestRedraw();
            this->hud_pointer_involved = hud_involved;
            break;
        }
        default:
            break;
    }
    return result;
}

void MapBgfxSurface::setMapCursor(Qt::CursorShape shape)
{
#ifndef Q_OS_WASM
    // The embedded native window has its own cursor; MapWidget's cursor is
    // never shown over it.
    if (this->native_window == nullptr)
        return;
    if (shape != Qt::ArrowCursor)
        this->native_window->setCursor(shape);
    else
        this->native_window->unsetCursor();
#else
    if (shape != Qt::ArrowCursor)
        setCursor(shape);
    else
        unsetCursor();
#endif
}

void MapBgfxSurface::renderOverlay(const QSizeF &logical, const MapBgfx2d::Camera &camera)
{
    if (!this->overlay_builder || !this->overlay_gpu.ready())
        return;
#ifdef Q_OS_WASM
    const qreal ratio = devicePixelRatioF();
    const QScreen *overlay_screen = screen();
#else
    const qreal ratio = this->native_window->devicePixelRatio();
    const QScreen *overlay_screen = this->native_window->screen();
#endif
    this->overlay_text_cache.setDevicePixelRatio(ratio);
    if (overlay_screen != nullptr)
        this->overlay_text_cache.setLogicalDpi(overlay_screen->logicalDotsPerInch());
    const QPointF offset =
        this->overlay_builder(&this->overlay_canvas, &this->overlay_text_cache, camera, logical);
    this->overlay_gpu.uploadAtlas(&this->overlay_text_cache);
    this->overlay_gpu.draw(kMapBgfxView, this->overlay_canvas, logical, offset);
}

void MapBgfxSurface::renderGlobe(const QSizeF &logical)
{
    // The camera is the model's, sampled for this frame like the 2D one; its
    // viewport is logical pixels (as the QRhi globe's), its sticky origin is
    // advanced once per frame.
    const QSize viewport(std::max(1, qRound(logical.width())), std::max(1, qRound(logical.height())));
    this->globe_camera.setViewportSize(viewport);
    // Rays between frames use the viewport drawn last.
    this->terrain_navigation.setViewportSize(viewport);
    this->globe_camera.syncFromMapModel(*this->map_model);
    this->globe_camera.updateRenderOrigin();
    MapBgfxGlobeSurface::FrameInputs inputs;
    inputs.logical_viewport = viewport;
    inputs.background = palette().color(QPalette::Window);
    inputs.background_opacity = double(this->background_opacity) / 100.0;
    inputs.upload_budget = kGlobeImageryUploadsPerFrame;
    inputs.heatmap_opacity = this->distribution_network_visible
        && this->symbology.visual_heatmap != VisualHeatmap::None
        ? double(this->symbology.heatmap_opacity) / 100.0 : 0.0;
    const MapBgfxGlobeSurface::FrameResult result =
        this->globe_surface->render(kMapBgfxView, this->globe_camera, &this->imagery_gpu, inputs);
    // The atlas' frame (LRU, budget trim) advances in either mode.
    this->imagery_gpu.endFrame();
    this->globe_pending = result.pending;
    this->imagery_quad_count = result.tiles_drawn;
    // The network on top of the terrain, in the same view (its depth
    // tests run against the terrain's depth).
    if (this->distribution_network_visible && this->globe_network_ready)
    {
        // The terrain as drawn this frame (demand points are draped on it).
        this->globe_network.setTerrainSurfaceRevision(this->globe_surface->surfaceRevision());
        // The themed icon fill of the globe (globeSymbology()).
        this->entity_style.setIconDefaultFill(globeSymbology().icon_default_fill_color);
        synchronizeEntityStyle();
        const MapBgfxGlobeNetwork::FrameResult network = this->globe_network.render(
            kMapBgfxView, this->globe_camera, viewport, this->gpu_2d_scene.iconAtlasTexture(),
            this->entity_style);
        this->network_frame_stats.draw_calls += network.draw_calls;
    }
}

void MapBgfxSurface::renderGlobeOverlay(const QSizeF &logical)
{
    if (!this->globe_overlay_builder || !this->overlay_gpu.ready())
        return;
#ifdef Q_OS_WASM
    const qreal ratio = devicePixelRatioF();
    const QScreen *overlay_screen = screen();
#else
    const qreal ratio = this->native_window->devicePixelRatio();
    const QScreen *overlay_screen = this->native_window->screen();
#endif
    this->overlay_text_cache.setDevicePixelRatio(ratio);
    if (overlay_screen != nullptr)
        this->overlay_text_cache.setLogicalDpi(overlay_screen->logicalDotsPerInch());

    // The camera renderGlobe() just drew with (its viewport and sticky
    // render origin): canonical clip space as the globe network's picking
    // projects (projectToScreen() there), NDC y up -> logical px y down.
    const QSize viewport(std::max(1, qRound(logical.width())), std::max(1, qRound(logical.height())));
    MapGlobeCameraBasis basis;
    const QMatrix4x4 view_projection = this->globe_camera.viewProjectionMatrix(&basis);
    const GeoWgs84Ellipsoid::EcefPositionD origin = this->globe_camera.renderOriginEcef();
    const double eye_x = origin.x + double(basis.eye.x());
    const double eye_y = origin.y + double(basis.eye.y());
    const double eye_z = origin.z + double(basis.eye.z());
    const MapGlobeSurfacePreparation &preparation = this->globe_surface->preparation();
    // drawnSurfaceHeightAt()'s ray start, above any exaggerated relief (as
    // MapGlobeTerrainNavigation::drawnSurfaceHeightAtCoordinate()).
    const double ray_start_height_m = 9000.0 * MapModel::MaxView3dVerticalExaggeration + 1000.0;
    const MapGlobeTerrainNavigation &navigation = this->terrain_navigation;
    const double vertical_exaggeration = this->map_model->view3dVerticalExaggeration();
    this->globe_overlay_height_cache.synchronize(
        this->globe_surface->surfaceRevision(), vertical_exaggeration);
    const GlobeProjector projector =
        [this, &view_projection, &origin, &preparation, &navigation, ray_start_height_m,
         vertical_exaggeration, eye_x, eye_y, eye_z, viewport](
            double longitude_deg, double latitude_deg, QPointF *screen_position,
            double *distance_m) -> bool
    {
        if (!std::isfinite(longitude_deg) || !std::isfinite(latitude_deg))
            return false;
        // Beyond the horizon: the eye lies below the point's tangent plane
        // (geodetic up), so the globe hides it. Tested on the ellipsoid
        // first, before any relief lookup (a peak just past the horizon is
        // not worth one).
        const double longitude_rad = qDegreesToRadians(longitude_deg);
        const double latitude_rad = qDegreesToRadians(latitude_deg);
        const double up_x = std::cos(latitude_rad) * std::cos(longitude_rad);
        const double up_y = std::cos(latitude_rad) * std::sin(longitude_rad);
        const double up_z = std::sin(latitude_rad);
        GeoWgs84Ellipsoid::EcefPositionD point =
            GeoWgs84Ellipsoid::geodeticToEcefD(longitude_deg, latitude_deg, 0.0);
        if ((eye_x - point.x) * up_x + (eye_y - point.y) * up_y + (eye_z - point.z) * up_z < 0.0)
            return false;
        // On the surface as drawn: the relief (vertical exaggeration
        // included) of the drawn tile under the coordinate, the ellipsoid
        // where none is drawn (off screen, flat tiles).
        //
        // The mesh ray (drawnSurfaceHeightAt()) misses at many lattice points:
        // the lattice is aligned with tile borders, so its points lie on the
        // drawn meshes' cell edges, vertices and tile borders, where the
        // ray-triangle test can slip through every adjacent triangle (and a
        // tile border's neighbour tile is never tried). Those points fell to
        // sea level (spikes down through the terrain). For them the DEM the
        // drawn tile was built from gives the same height: its samples are
        // the mesh's vertices (elevation x exaggeration, as
        // MapGlobeVerticalTransform::terrainHeightM()), bilinear between.
        // Not the navigation's all-tiles ray scan: far too slow for a
        // lattice per frame.
        CoordinateWGS84 coordinate;
        coordinate.longitude_deg = longitude_deg;
        coordinate.latitude_deg = latitude_deg;
        double height_m = 0.0;
        const bool height_known = this->globe_overlay_height_cache.height(
            longitude_deg, latitude_deg, &height_m,
            [&preparation, &navigation, &coordinate, ray_start_height_m,
             vertical_exaggeration](double *sampled_height_m) -> bool
        {
            if (preparation.drawnSurfaceHeightAt(coordinate, ray_start_height_m,
                                                  sampled_height_m)
                    == MapGlobeSurfacePreparation::DrawnSurfaceHeight::Relief
                && std::isfinite(*sampled_height_m))
                return true;
            int visible_terrain_zoom = -1;
            double visible_cell_size_m = 0.0;
            if (!preparation.visibleTerrainSamplingAtCoordinate(
                    coordinate, &visible_terrain_zoom, &visible_cell_size_m))
                return false;
            double elevation_m = 0.0;
            double cell_size_m = 0.0;
            if (!navigation.terrainElevationAtCoordinate(coordinate, &elevation_m, &cell_size_m)
                || !std::isfinite(elevation_m))
                return false;
            *sampled_height_m = elevation_m * vertical_exaggeration;
            return std::isfinite(*sampled_height_m);
        });
        if (height_known)
            point = GeoWgs84Ellipsoid::geodeticToEcefD(longitude_deg, latitude_deg, height_m);
        // Relative to the render origin before the float conversion
        // (precision at Earth scale).
        const QVector4D clip = view_projection * QVector4D(float(point.x - origin.x),
                                                           float(point.y - origin.y),
                                                           float(point.z - origin.z), 1.0f);
        if (!(clip.w() > 1e-6f))
            return false;
        const double ndc_x = double(clip.x()) / double(clip.w());
        const double ndc_y = double(clip.y()) / double(clip.w());
        // Far off screen (grazing views): no use to a line, only float trouble.
        if (!(std::abs(ndc_x) < 1.0e4) || !(std::abs(ndc_y) < 1.0e4))
            return false;
        *screen_position = QPointF((ndc_x + 1.0) * double(viewport.width()) / 2.0,
                                   (1.0 - ndc_y) * double(viewport.height()) / 2.0);
        if (distance_m != nullptr)
        {
            const double dx = point.x - eye_x;
            const double dy = point.y - eye_y;
            const double dz = point.z - eye_z;
            *distance_m = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        return true;
    };
    // Query retained ECEF icon anchors against the view before consulting
    // terrain. Use only side planes: near/far clipping can differ between
    // Vulkan/GL/WebGL, whereas these four side inequalities are universal.
    // The same margin used by the Editor pixel cull keeps edge sprites.
    const double logical_margin = 2.0 * 240.0 + 24.0; // maximum editor icon width (40 px * 600%)
    const double x_extension = 1.0 + 2.0 * logical_margin / double(viewport.width());
    const double y_extension = 1.0 + 2.0 * logical_margin / double(viewport.height());
    MapBgfxGlobeIconIndex::Frustum frustum;
    frustum.valid = true;
    // The ray's start uses the *maximum possible* exaggeration, but the
    // index can bound the actually drawn height at the current exaggeration.
    // A conservative 9000 m DEM elevation cap plus 1000 m covers negative
    // elevations and leaves headroom for displaced terrain.
    frustum.relief_allowance_m = 9000.0 * vertical_exaggeration + 1000.0;
    const int side_axis[4] = {0, 0, 1, 1};
    const double side_sign[4] = {1.0, -1.0, 1.0, -1.0};
    for (int side = 0; side < 4; ++side)
    {
        const int row = side_axis[side];
        const double sign = side_sign[side];
        const double expansion = row == 0 ? x_extension : y_extension;
        MapBgfxGlobeIconIndex::Plane &plane = frustum.sides[std::size_t(side)];
        plane.x = expansion * double(view_projection(3, 0))
                  + sign * double(view_projection(row, 0));
        plane.y = expansion * double(view_projection(3, 1))
                  + sign * double(view_projection(row, 1));
        plane.z = expansion * double(view_projection(3, 2))
                  + sign * double(view_projection(row, 2));
        plane.offset = expansion * double(view_projection(3, 3))
                     + sign * double(view_projection(row, 3))
                     - plane.x * origin.x - plane.y * origin.y - plane.z * origin.z;
    }
    const std::uint64_t cache_hits_before = this->globe_overlay_height_cache.hits();
    const std::uint64_t terrain_samples_before = this->globe_overlay_height_cache.samples();
    const std::uint64_t unavailable_reuses_before = this->globe_overlay_height_cache.unavailableReuses();
    const std::uint64_t evictions_before = this->globe_overlay_height_cache.evictionCount();
    this->globe_overlay_builder(&this->overlay_canvas, &this->overlay_text_cache,
                                projector, logical, frustum);
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::GlobeTerrainHeightCacheHit,
                              quint64(this->globe_overlay_height_cache.hits() - cache_hits_before));
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::GlobeTerrainHeightSample,
                              quint64(this->globe_overlay_height_cache.samples() - terrain_samples_before));
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::GlobeTerrainHeightUnavailableReuse,
                              quint64(this->globe_overlay_height_cache.unavailableReuses()
                                      - unavailable_reuses_before));
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::GlobeTerrainHeightEviction,
                              quint64(this->globe_overlay_height_cache.evictionCount()
                                      - evictions_before));
    this->overlay_gpu.uploadAtlas(&this->overlay_text_cache);
    this->overlay_gpu.draw(kMapBgfxView, this->overlay_canvas, logical, QPointF());
}

void MapBgfxSurface::renderLabels(const QSizeF &logical, const MapBgfx2d::Camera &network_camera,
                                  const MapBgfx2dSymbology::Sizes &sizes)
{
    if (!this->label_settings.anyEnabled() || !this->label_gpu.ready()
        || !this->retained_scene.hasSnapshot() || !this->prepared_2d_scenes.active().isPrepared())
    {
        return;
    }
#ifdef Q_OS_WASM
    const qreal ratio = devicePixelRatioF();
    const QScreen *label_screen = screen();
#else
    const qreal ratio = this->native_window->devicePixelRatio();
    const QScreen *label_screen = this->native_window->screen();
#endif
    if (!this->label_text_cache)
        this->label_text_cache = std::make_unique<MapBgfxHudTextCache>(2048);
    MapBgfxHudTextCache *cache = this->label_text_cache.get();
    cache->setDevicePixelRatio(ratio);
    const double dpi = label_screen != nullptr ? label_screen->logicalDotsPerInch() : 96.0;
    cache->setLogicalDpi(dpi);

    LabelBuildKey key;
    key.world_pixels = network_camera.world_pixels;
    key.viewport_width = logical.width();
    key.viewport_height = logical.height();
    key.node_half_px = sizes.node_half_px;
    key.icon_size_px = sizes.icon_size_px;
    key.device_pixel_ratio = ratio;
    key.preparation_id = this->prepared_2d_scenes.active().preparationId();
    key.atlas_generation = cache->generation();
    // Unchanged content and zoom: a pure pan draws the last build translated
    // (it was built with a margin around the viewport, map_bgfx_pan_reuse.h).
    QPointF offset;
    if (this->labels_dirty || !(key == this->label_build_key)
        || !this->label_pan.reuse(network_camera, logical, ratio, &offset))
    {
        // The application font one step smaller, pixel sized (the halo
        // outline path and the metrics then agree; see haloText()).
        QFont font = QGuiApplication::font();
        const double point_size = font.pointSizeF() > 0.0 ? font.pointSizeF() : 9.0;
        font.setPixelSize(std::max(9, int(std::lround(0.92 * point_size * dpi / 72.0))));

        // Hidden entities (editor move preview) by entity key.
        QSet<quint64> hidden_keys;
        for (const QUuid &uuid : this->hidden_entity_uuids)
        {
            const QHash<QUuid, quint64>::const_iterator found =
                this->entity_key_by_uuid.constFind(uuid);
            if (found != this->entity_key_by_uuid.cend())
                hidden_keys.insert(found.value());
        }
        // Default symbol boxes: what the GPU network draws (dots, or icons
        // of icon_size_px with the atlas aspect; pipes have none).
        const MapNetworkRenderSymbology &symbology = this->symbology;
        const MapBgfxLabels::SymbolRect icon_box = [sizes](InfrastructureEntity entity_type)
        {
            const MapIconAtlasEntry entry = mapIconAtlasEntry(entity_type);
            const double half_width = sizes.icon_size_px * entry.width_ratio * 0.5;
            const double half_height = sizes.icon_size_px * entry.height_ratio * 0.5;
            return MapBgfxHudRect{-half_width, -half_height, 2.0 * half_width, 2.0 * half_height};
        };
        const MapBgfxLabels::SymbolRect default_node_symbol =
            [&symbology, sizes, icon_box](InfrastructureEntity entity_type)
        {
            if (entity_type != InfrastructureEntity::Junction && symbology.show_icons
                && mapHasIcon(entity_type))
                return icon_box(entity_type);
            if (entity_type == InfrastructureEntity::Junction && !symbology.show_junctions)
                return MapBgfxHudRect();
            const double half = sizes.node_half_px;
            return MapBgfxHudRect{-half, -half, 2.0 * half, 2.0 * half};
        };
        const MapBgfxLabels::SymbolRect default_link_symbol =
            [&symbology, icon_box](InfrastructureEntity entity_type)
        {
            if (entity_type != InfrastructureEntity::Pipe && symbology.show_icons
                && mapHasIcon(entity_type))
                return icon_box(entity_type);
            return MapBgfxHudRect();
        };

        // Labels have no screen-anchored parts: a rebuild while panning gets
        // the margin, so the labels scrolling in from the edges are placed
        // already (and decluttered against what lies beyond the edge).
        const MapBgfxPanReuse::Build build =
            this->label_pan.begin(network_camera, logical, ratio, true);
        offset = build.offset;
        MapBgfxLabels::Inputs inputs;
        inputs.viewport = build.viewport;
        inputs.camera = build.camera;
        inputs.scene = &this->prepared_2d_scenes.active();
        inputs.snapshot = &this->retained_scene.snapshot();
        inputs.settings = this->label_settings;
        inputs.node_values = &this->symbology.node_values;
        inputs.link_values = &this->symbology.link_values;
        inputs.node_value_format = this->label_node_format;
        inputs.link_value_format = this->label_link_format;
        inputs.hidden_keys = &hidden_keys;
        inputs.node_symbol = this->label_node_symbol ? this->label_node_symbol : default_node_symbol;
        inputs.link_symbol = this->label_link_symbol ? this->label_link_symbol : default_link_symbol;
        inputs.font = font;
        const bool complete = this->labels.build(&this->label_canvas, cache, inputs);
        // Deferred labels: build again next frame even without a change.
        this->labels_dirty = !complete;
        key.atlas_generation = cache->generation();
        this->label_build_key = key;
    }
    this->label_gpu.uploadAtlas(cache);
    this->label_gpu.draw(kMapBgfxView, this->label_canvas, logical, offset);
}

void MapBgfxSurface::renderHud(const QSizeF &logical, const MapBgfx2d::Camera &camera)
{
    if (!this->hud_gpu.ready())
        return;
#ifdef Q_OS_WASM
    const qreal ratio = devicePixelRatioF();
    const QScreen *hud_screen = screen();
#else
    const qreal ratio = this->native_window->devicePixelRatio();
    const QScreen *hud_screen = this->native_window->screen();
#endif
    this->hud_text_cache.setDevicePixelRatio(ratio);
    if (hud_screen != nullptr)
        this->hud_text_cache.setLogicalDpi(hud_screen->logicalDotsPerInch());
    // 3D is offered when the globe surface initialized.
    this->hud.setViewMode(this->camera_state.mode == MapViewMode::TwoD, this->globe_ready);
    MapBgfxHud::FrameInputs inputs;
    inputs.viewport = logical;
    inputs.two_d = this->camera_state.mode == MapViewMode::TwoD;
    inputs.camera = camera;
    inputs.time_s = double(this->hud_clock.elapsed()) / 1000.0;
    // Globe controls (slice 5): camera state from the model, the toggles,
    // underground mode and terrain-mesh progress from this surface.
    MapBgfxHud::Globe globe;
    globe.visible = this->camera_state.mode == MapViewMode::Globe && this->globe_ready;
    if (globe.visible)
    {
        MapBgfxGlobeHudController::fillHudState(*this->map_model, globeViewportSize(), &globe);
        globe.underground_mode = int(this->underground_mode);
        globe.wireframe = terrainWireframeVisible();
        globe.map_tiles = mapTilesVisible();
        globe.hillshade = terrainHillshadeVisible();
        this->globe_surface->terrainMeshProgress(&globe.mesh_completed, &globe.mesh_total, &globe.mesh_active);
    }
    this->hud.setGlobe(globe);
    this->hud.build(&this->hud_canvas, &this->hud_text_cache, inputs,
                    MapBgfxHud::Palette::fromQPalette(palette()));
    this->hud_gpu.uploadAtlas(&this->hud_text_cache);
    this->hud_gpu.draw(kMapBgfxView, this->hud_canvas, logical);
}

void MapBgfxSurface::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (this->initialized) {
        synchronizeGeometry();
        startFrameLoop();
        return;
    }
    if (this->reinitialize_on_surface)
    {
        scheduleBackendRestart();
        return;
    }
    if (this->failed || this->initialization_attempted)
        return;
    beginInitialization();
}

void MapBgfxSurface::beginInitialization()
{
    // Native platform window and Qt's WASM shadow-root must exist first.
    QTimer::singleShot(0, this, [this]() {
        if (!isVisible() || this->failed || this->initialization_attempted || this->initialized)
            return;
        this->initialization_attempted = true;
        qInfo() << "bgfx map: initializing at" << physicalSize() << "physical pixels";
        if (initializeBackend())
        {
            qInfo() << "bgfx map: initialized, waiting for the first frame";
            startFrameLoop();
            return;
        }
        if (this->device_busy)
        {
            // The other map tab has not released the device yet (it is
            // still visible); not a failure.
            this->initialization_attempted = false;
            QTimer::singleShot(250, this, [this]() { beginInitialization(); });
            return;
        }
        fail(QStringLiteral("bgfx initialization failed (%1)").arg(startupDiagnostics()));
    });
}

void MapBgfxSurface::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    stopFrameLoop();
    // No popup survives the map being hidden (tab switch, fullscreen).
    this->hud.closePopup();
    // A main-navigation tab change hides this widget, not its native window
    // *identity*. Retain the bgfx device, shaders, textures and terrain GPU
    // caches; only rendering pauses. Re-entering the tab takes the cheap
    // initialized branch of showEvent() instead of bgfx::init(). Genuine
    // native-surface destruction is handled in eventFilter(), and explicit
    // native-window replacement still calls suspendForNativeWindowChange().
#ifdef Q_OS_WASM
    // Unlike the desktop's embedded QWindow, the browser canvas lives above
    // Qt's widgets. Hide it explicitly so it cannot cover another main page.
    // Never hide a canvas already handed over to a different surface.
    if (g_bgfx_device_owner == this || g_bgfx_device_owner == nullptr)
        aowisBgfxMapGeometry(0, 0, 0, 0, 0);
#endif
}

void MapBgfxSurface::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange
        || event->type() == QEvent::StyleChange || event->type() == QEvent::ApplicationPaletteChange)
    {
        requestRedraw();
        // The globe network's icon fill follows the theme.
        if (event->type() != QEvent::FontChange)
            this->globe_network.setSymbology(globeSymbology());
    }
    QWidget::changeEvent(event);
}

void MapBgfxSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    synchronizeGeometry();
}

QSize MapBgfxSurface::physicalSize() const
{
#ifdef Q_OS_WASM
    const QWidget *viewport = this;
#else
    const QWindow *viewport = this->native_window;
#endif
    const qreal ratio = viewport->devicePixelRatio();
    return QSize(std::max(1, qRound(viewport->width() * ratio)),
                 std::max(1, qRound(viewport->height() * ratio)));
}

// The camera works in logical (device-independent) pixels like MapModel,
// MapWidget, the scale bar and the CPU renderer: one 2D zoom level means
// 256 * 2^zoom *logical* pixels per world. Passing physical pixels made the
// bgfx map one octave too small at devicePixelRatio 2 (and fixed-pixel
// markers half-size). The clip transform only needs the ratio, so the
// physical framebuffer size is still used for the view rectangle.
QSizeF MapBgfxSurface::logicalSize() const
{
#ifdef Q_OS_WASM
    const QWidget *viewport = this;
#else
    const QWindow *viewport = this->native_window;
#endif
    return QSizeF(std::max(1, viewport->width()), std::max(1, viewport->height()));
}

double MapBgfxSurface::refreshPeriodSeconds() const
{
#ifdef Q_OS_WASM
    // Unknown in the browser; MapFramePacer estimates it from rAF stamps.
    return 0.0;
#else
    const QScreen *screen = this->native_window != nullptr
        ? this->native_window->screen() : nullptr;
    const double rate = screen != nullptr ? double(screen->refreshRate()) : 0.0;
    if (!std::isfinite(rate) || rate < 20.0 || rate > 500.0)
        return 0.0;
    return 1.0 / rate;
#endif
}

void MapBgfxSurface::startFrameLoop()
{
    if (this->frame_loop_running || !this->initialized || this->failed)
        return;
    this->frame_loop_running = true;
    // A (re)started loop presents its first frame unconditionally.
    this->presented_valid = false;
    this->redraw_requested = true;
    this->frame_pacer.reset();
    this->frame_clock.invalidate();
    this->last_frame_time_ms = -1.0;
#ifdef Q_OS_WASM
    g_bgfx_frame_loop_surface = this;
    ++g_bgfx_frame_loop_generation;
    emscripten_request_animation_frame_loop(
        &MapBgfxSurface::animationFrameCallback,
        reinterpret_cast<void *>(g_bgfx_frame_loop_generation));
#else
    // Fire slightly faster than the display: bgfx::frame() blocks in the
    // vsync swap, so every tick presents exactly one refresh. A timer
    // *slower* than the display (e.g. 17 ms at 59.94 Hz) would instead
    // repeat a frame every ~1 s. One timer only -- see MapWidget.
    const double period_s = refreshPeriodSeconds() > 0.0
        ? refreshPeriodSeconds() : 1.0 / kFallbackRefreshHz;
    this->frame_timer.setInterval(std::max(1, int(std::floor(period_s * 1000.0)) - 1));
    this->frame_timer.start();
#endif
}

void MapBgfxSurface::stopFrameLoop()
{
    this->frame_loop_running = false;
    this->frame_timer.stop();
#ifdef Q_OS_WASM
    ++g_bgfx_frame_loop_generation;
#endif
}

#ifdef Q_OS_WASM
EM_BOOL MapBgfxSurface::animationFrameCallback(double time_ms, void *user_data)
{
    MapBgfxSurface *surface = g_bgfx_frame_loop_surface;
    if (surface == nullptr || !surface->frame_loop_running
        || reinterpret_cast<quintptr>(user_data) != g_bgfx_frame_loop_generation)
        return EM_FALSE;

    // rAF timestamps are the browser's frame (vsync) times: they are the
    // presentation clock, unlike Qt's setTimeout-based QTimer on WASM.
    double elapsed_seconds = 0.0;
    if (surface->last_frame_time_ms >= 0.0)
        elapsed_seconds = surface->frame_pacer.advance(
            (time_ms - surface->last_frame_time_ms) / 1000.0, 0.0);
    surface->last_frame_time_ms = time_ms;
    surface->renderFrameAt(elapsed_seconds);
    return surface->frame_loop_running ? EM_TRUE : EM_FALSE;
}
#endif

void MapBgfxSurface::synchronizeGeometry()
{
#ifdef Q_OS_WASM
    // The shared canvas follows the map that owns the device.
    if (g_bgfx_device_owner == this || g_bgfx_device_owner == nullptr)
    {
        const QPoint position = mapToGlobal(QPoint(0, 0));
        aowisBgfxMapGeometry(position.x(), position.y(), width(), height(),
                             isVisible() && this->initialized ? 1 : 0);
    }
#endif
    if (!this->initialized)
        return;
#ifndef Q_OS_WASM
    // Tab changes unmap the embedded QWindow. Do not reset a Vulkan/GL
    // swapchain until the native window is exposed again; Qt may resize it
    // while its page is hidden. The first resumed frame applies that resize.
    if (!isVisible() || this->native_window == nullptr || !this->native_window->isExposed())
        return;
#endif
    const QSize pixels = physicalSize();
    if (pixels == this->last_physical_size)
        return;
    this->last_physical_size = pixels;
    bgfx::SwapChain swap_chain{};
    swap_chain.width = static_cast<uint32_t>(pixels.width());
    swap_chain.height = static_cast<uint32_t>(pixels.height());
    bgfx::reset(kMapBgfxResetFlags, &swap_chain);
}

bool MapBgfxSurface::initializeBackend()
{
    this->device_busy = false;
    if (g_bgfx_device_owner != nullptr && g_bgfx_device_owner != this)
    {
        if (g_bgfx_device_owner->isVisible())
        {
            // Two maps visible at once (not a layout the application has):
            // wait for the other one to be hidden.
            qInfo() << "bgfx map: the device is in use by another visible map; waiting";
            this->device_busy = true;
            return false;
        }
        // Take the device over from a hidden map (it restarts when shown).
        g_bgfx_device_owner->shutdownBackend();
    }
    bgfx::Init settings{};
    settings.limits.maxTransientVbSize = kTransientVertexBytes;
    settings.limits.maxTransientIbSize = kTransientIndexBytes;
    this->startup_step = "preparing the render target";
#ifdef Q_OS_WASM
    if (!aowisBgfxMapPrepare())
    {
        // window.aowisBgfxMap missing (aowis-bgfx-map.js not loaded), Qt's
        // screen element not found, or specialHTMLTargets unavailable.
        qWarning() << "bgfx map: the browser canvas could not be prepared";
        return false;
    }
    settings.swapChain.nwh = const_cast<char *>("!aowis-bgfx-map");
    {
        const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = createBrowserContext();
        if (context <= 0)
        {
            qWarning() << "bgfx map: the WebGL 2 context could not be created, error"
                       << int(context);
            return false;
        }
        settings.platformData.context =
            reinterpret_cast<void *>(static_cast<intptr_t>(context));
    }
#else
    settings.swapChain.nwh = reinterpret_cast<void *>(this->native_window->winId());
#if defined(Q_OS_LINUX)
    // The native handles are renderer-independent: OpenGL (EGL/GLX) and
    // Vulkan (VK_KHR_wayland/xlib surface) both consume display + surface.
    const QString platform = QGuiApplication::platformName();
    if (platform.startsWith(QStringLiteral("wayland"))) {
#if defined(AOWIS_BGFX_NATIVE_WAYLAND_PRIVATE)
        QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
        settings.swapChain.ndt = native
            ? native->nativeResourceForIntegration(QByteArrayLiteral("display")) : nullptr;
        settings.platformData.type = bgfx::NativeWindowHandleType::Wayland;
        settings.swapChain.nwh = native
            ? native->nativeResourceForWindow(QByteArrayLiteral("surface"), this->native_window)
            : nullptr;
#else
        qWarning() << "bgfx monitor requires Qt6::GuiPrivate for Wayland";
        return false;
#endif
    } else if (platform == QStringLiteral("xcb")) {
#if defined(AOWIS_BGFX_NATIVE_WAYLAND_PRIVATE)
        QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
        settings.swapChain.ndt = native
            ? native->nativeResourceForIntegration(QByteArrayLiteral("display")) : nullptr;
#else
        qWarning() << "bgfx monitor requires Qt6::GuiPrivate for X11";
        return false;
#endif
    }
#endif
    if (settings.swapChain.nwh == nullptr)
        return false;
#endif
    const QSize pixels = physicalSize();
    settings.swapChain.width = static_cast<uint32_t>(pixels.width());
    settings.swapChain.height = static_cast<uint32_t>(pixels.height());
    settings.reset = kMapBgfxResetFlags;

    // Renderer choice is delegated to bgfx (RendererType::Count ranks the
    // platform's renderers and falls through when one fails to initialise).
    // AOWIS only adds two things on top: an optional diagnostic override, and
    // a retry when bgfx picks a renderer for which this build embeds no
    // shaders (e.g. D3D shaders need a Windows-hosted shaderc).
    QVector<bgfx::RendererType::Enum> attempts;
#ifdef Q_OS_WASM
    // WebGL 2 is the only renderer compiled for the browser build.
    attempts.append(bgfx::RendererType::OpenGLES);
#else
    const bgfx::RendererType::Enum requested = MapBgfxBackend::requestedRenderer();
    if (requested != bgfx::RendererType::Count)
        attempts.append(requested);
    attempts.append(bgfx::RendererType::Count);
    const QVector<bgfx::RendererType::Enum> usable = MapBgfxBackend::usableRenderers();
    for (const bgfx::RendererType::Enum renderer : usable)
    {
        if (!attempts.contains(renderer))
            attempts.append(renderer);
    }
#endif

    for (const bgfx::RendererType::Enum attempt : attempts)
    {
        settings.type = attempt;
        this->startup_step = "bgfx::init()";
        qInfo() << "bgfx map: bgfx::init() for"
                << (attempt == bgfx::RendererType::Count
                    ? "automatic selection" : bgfx::getRendererName(attempt));
        if (!bgfx::init(settings))
        {
            qWarning() << "bgfx init failed for"
                       << (attempt == bgfx::RendererType::Count
                           ? "automatic selection" : bgfx::getRendererName(attempt));
            continue;
        }
        const bgfx::RendererType::Enum selected = bgfx::getRendererType();
        if (!MapBgfxBackend::shadersAvailable(selected))
        {
            qWarning() << "bgfx selected" << bgfx::getRendererName(selected)
                       << "but this build has no shaders for it; trying the next renderer";
            bgfx::shutdown();
            continue;
        }
        this->initialized = true;
        g_bgfx_device_owner = this;
        this->startup_step = "creating shaders and GPU resources";
        // Each part reports what it rejected; name the part here as well.
        // The network's colours (entity data textures) for both views.
        const bool network_ready = this->entity_style.initialize()
            && this->gpu_2d_scene.initialize(mapIconAtlasImage());
        const bool imagery_ready = network_ready && this->imagery_gpu.initialize();
        const bool hud_ready = imagery_ready && this->hud_gpu.initialize()
            && this->overlay_gpu.initialize() && this->label_gpu.initialize();
        // The globe is optional: without it (editor, or its GPU setup failed)
        // the 2D map works as before and the HUD offers no 3D.
        this->globe_ready = network_ready && imagery_ready && hud_ready
            && this->globe_offered && this->globe_surface->initialize();
        // The network on the globe: without it the terrain still works.
        this->globe_network_ready = this->globe_ready && this->globe_network.initialize();
        if (!network_ready || !imagery_ready || !hud_ready)
        {
            const bgfx::Caps *caps = bgfx::getCaps();
            qWarning() << "bgfx map shader initialization failed on"
                       << bgfx::getRendererName(selected) << "in"
                       << (!network_ready ? "network" : !imagery_ready ? "imagery" : "HUD")
                       << "- max texture size" << (caps != nullptr ? int(caps->limits.maxTextureSize) : -1)
                       << "RGBA8 caps" << Qt::hex
                       << (caps != nullptr ? int(caps->formats[bgfx::TextureFormat::RGBA8]) : -1);
            releaseGpuResources();
            bgfx::shutdown();
            this->initialized = false;
            g_bgfx_device_owner = nullptr;
            continue;
        }
        this->startup_step = "running";
        MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::BgfxDeviceStarted);
        qInfo() << "Map Monitor renderer:" << graphicsApiName();
        // bgfx's text overlay covers the HUD; diagnostics only on request.
        bgfx::setDebug(this->debug_overlay_enabled ? BGFX_DEBUG_TEXT : BGFX_DEBUG_NONE);
        synchronizeGeometry();
        // Already in Globe mode (device restart): the camera height resumes,
        // the heatmap is baked again.
        syncGlobeCameraHeight(true);
        syncGlobeHeatmap();
        return true;
    }
    return false;
}

void MapBgfxSurface::renderFrame()
{
    // Desktop timer tick. Measure from the start of the previous tick; with
    // the blocking vsync swap that is one refresh +- dispatch jitter, which
    // the pacer turns into exactly one refresh period.
    //
    // Never present into an unexposed window (minimized, other virtual
    // desktop, fully covered where the platform reports it). With
    // BGFX_RESET_VSYNC the swap can block until the compositor's next frame
    // callback, which a hidden Wayland surface may never receive; that would
    // freeze the GUI thread. The next exposed frame restarts with dt = 0.
#ifndef Q_OS_WASM
    if (this->native_window != nullptr && !this->native_window->isExposed())
    {
        this->frame_clock.invalidate();
        this->frame_pacer.reset();
        return;
    }
#endif
    double elapsed_seconds = 0.0;
    if (this->frame_clock.isValid())
    {
        elapsed_seconds = this->frame_pacer.advance(
            double(this->frame_clock.nsecsElapsed()) * 1e-9, refreshPeriodSeconds());
    }
    this->frame_clock.start();
    renderFrameAt(elapsed_seconds);
}

void MapBgfxSurface::renderFrameAt(double elapsed_seconds)
{
    // Counted before any early return: tells a dead frame loop apart from a
    // loop that runs but skips (startupDiagnostics()).
    ++this->frame_callback_count;
    if (!this->initialized || !isVisible())
        return;
    synchronizeGeometry();
    // Advance navigation (keyboard/wheel zoom, keyboard pan) by exactly the
    // time this frame represents, *then* sample the camera. Each presented
    // frame therefore shows one uniform camera step.
    if (this->navigation_frame && elapsed_seconds > 0.0)
        this->navigation_frame(elapsed_seconds);
    if (!this->initialized)
        return;
    // Terrain follow of the globe camera (slice 2): one filter step per frame
    // while it settles; it moves the camera, which presents the frame.
    if (this->terrain_navigation.following())
        syncGlobeCameraHeight(false);
    this->camera_state = this->map_model->cameraViewState();
    const QSize pixels = physicalSize();
    const QSizeF logical = logicalSize();
    // On-demand rendering: present only when something visible changed (the
    // camera, the size, any input of the surface, see requestRedraw()) or
    // while something animates by itself. An idle map then costs this
    // early return per display refresh instead of a full frame: in the
    // browser the map no longer keeps a CPU core busy. The navigation clock
    // above still runs every tick (keyboard pan, inertia, zoom glides move
    // the camera and so present frames).
    const bool camera_moved = !this->presented_valid
        || mapCameraUpdate(this->presented_camera, this->camera_state, 0, 0).camera_changed;
    const bool size_changed = pixels != this->presented_pixels
        || logical != this->presented_logical;
    const bool globe_shown = this->globe_ready && this->camera_state.mode == MapViewMode::Globe;
    const bool animating = !this->ready_reported || this->debug_overlay_enabled
        || this->hud.animating() || this->imagery_uploads_pending
        || (globe_shown && this->globe_pending)
        || (this->distribution_network_visible && this->label_settings.anyEnabled()
            && this->labels_dirty);
    if (!this->redraw_requested && !camera_moved && !size_changed && !animating)
        return;
    // Cleared before rendering: whatever asks for a frame from now on
    // (also during this one) gets the next.
    this->redraw_requested = false;
    // AOWIS_MAP_RENDER_DIAGNOSTICS: counts this frame and its CPU time.
    const MapRenderDiagnostics::Scope frame_timing(MapRenderDiagnostics::Counter::Frame);
    bgfx::setViewRect(kMapBgfxView, 0, 0,
                      static_cast<uint16_t>(std::min(65535, pixels.width())),
                      static_cast<uint16_t>(std::min(65535, pixels.height())));
    bgfx::setViewClear(kMapBgfxView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                       globe_shown ? MapBgfxGlobeSurface::SpaceColorRgba : 0x16324affu, 1.0f, 0);
    // The repository keeps one request batch: entering a mode requests its
    // own visible tiles again.
    if (this->camera_state.mode != this->presented_view_mode)
    {
        this->presented_view_mode = this->camera_state.mode;
        this->imagery_layout_key = 0;
        this->globe_surface->requestAgain();
    }
    bgfx::setViewMode(kMapBgfxView, bgfx::ViewMode::Sequential);
    bgfx::setViewTransform(kMapBgfxView, nullptr, nullptr);
    bgfx::touch(kMapBgfxView);
    this->network_frame_stats = MapBgfx2dGpu::FrameStats();
    // Cache/terrain selection overlays belong to the active input mode, not
    // to the distribution network layer. In Map Management the latter is
    // hidden, while green/brown selection rectangles must remain visible.
    const MapBgfxLayerVisibility layers = mapBgfxLayerVisibility(
        this->distribution_network_visible, bool(this->overlay_builder),
        bool(this->globe_overlay_builder));
    if (this->camera_state.mode == MapViewMode::TwoD) {
        // Imagery first (opaque), network on top; the view is Sequential.
        renderImagery(logical);
        if (layers.distribution_network)
        {
            // Icon placement comes from the shared QRhi atlas layout. Atlas v
            // runs downwards (uv_rect.top() < bottom()); see appendIcon().
            const MapBgfx2dMesh::IconLookup icon_lookup = [](InfrastructureEntity entity_type)
            {
                const MapIconAtlasEntry entry = mapIconAtlasEntry(entity_type);
                MapBgfx2dIconSprite sprite;
                sprite.valid = entry.valid;
                sprite.u0 = float(entry.uv_rect.left());
                sprite.v0 = float(entry.uv_rect.top());
                sprite.u1 = float(entry.uv_rect.right());
                sprite.v1 = float(entry.uv_rect.bottom());
                sprite.half_width_ratio = float(entry.width_ratio * 0.5);
                sprite.half_height_ratio = float(entry.height_ratio * 0.5);
                return sprite;
            };
            if (!this->gpu_2d_scene.synchronize(this->prepared_2d_scenes.active(), icon_lookup)) {
                fail(QStringLiteral("bgfx network vertex-buffer upload failed"));
                return;
            }
            const MapBgfx2d::Camera view = MapBgfx2d::cameraFor(
                this->camera_state.center_lon_deg, this->camera_state.center_lat_deg,
                this->camera_state.continuous_2d_zoom,
                logical.width(), logical.height());
            // Colours: entity data textures, uploaded only when values,
            // visibility, selection or diagnostics changed (never per frame, and
            // never a vertex buffer).
            this->entity_style.setIconDefaultFill(this->symbology.icon_default_fill_color);
            synchronizeEntityStyle();
            synchronizeHighlights();
            // Sizes are uniforms from the continuous camera: metre-based widths
            // scale smoothly with the zoom.
            const MapBgfx2dSymbology::Sizes sizes = MapBgfx2dSymbology::sizes(
                this->symbology, this->camera_state.center_lat_deg,
                this->camera_state.continuous_2d_zoom);
            MapBgfx2dGpu::Style style;
            style.link_width_px = sizes.link_width_px;
            style.attachment_width_px = sizes.attachment_width_px;
            style.node_half_px = sizes.node_half_px;
            style.icon_size_px = sizes.icon_size_px;
            style.flow_length_px = this->symbology.show_flow_direction ? sizes.flow_length_px : 0.0;
            style.flow_half_stroke_px = sizes.flow_half_stroke_px;
            style.heatmap_radius_px = sizes.heatmap_radius_px;
            style.heatmap_opacity = sizes.heatmap_opacity;
            style.heatmap_solid_fraction = sizes.heatmap_solid_fraction;
            // Editor full-network move: the network is drawn shifted (camera
            // offset), the imagery underneath is not.
            MapBgfx2d::Camera network_view = view;
            if (!this->network_screen_translation.isNull() && view.world_pixels > 0.0)
            {
                network_view.center.x -= this->network_screen_translation.x() / view.world_pixels;
                network_view.center.y -= this->network_screen_translation.y() / view.world_pixels;
            }
            this->network_frame_stats = this->gpu_2d_scene.draw(network_view, style, this->entity_style);
            if (layers.editor_overlay_2d)
                renderOverlay(logical, view);
            // Labels above the network and the editor's icons, below the HUD.
            renderLabels(logical, network_view, sizes);
        }
        else if (layers.editor_overlay_2d)
        {
            // No network mesh or labels in Map Management, but the editor
            // canvas still draws its green/brown cache-selection overlay.
            const MapBgfx2d::Camera view = MapBgfx2d::cameraFor(
                this->camera_state.center_lon_deg, this->camera_state.center_lat_deg,
                this->camera_state.continuous_2d_zoom,
                logical.width(), logical.height());
            renderOverlay(logical, view);
        }
    }
    else
    {
        // 2D imagery uploads are not pending while the 2D map is not drawn.
        this->imagery_uploads_pending = false;
        if (globe_shown)
        {
            renderGlobe(logical);
            // Project the selected map/terrain tile lattice independently of
            // the distribution-network visibility, also on the 3D globe.
            if (layers.editor_overlay_3d)
                renderGlobeOverlay(logical);
        }
    }
    // The HUD is submitted last into the Sequential map view: always on top.
    renderHud(logical, MapBgfx2d::cameraFor(
        this->camera_state.center_lon_deg, this->camera_state.center_lat_deg,
        this->camera_state.continuous_2d_zoom, logical.width(), logical.height()));
    if (this->debug_overlay_enabled)
        renderDebugOverlay();
    bgfx::frame();
    ++this->frame_count;
    MapRenderDiagnostics::reportIfDue();
    this->presented_valid = true;
    this->presented_camera = this->camera_state;
    this->presented_pixels = pixels;
    this->presented_logical = logical;
    if (!this->ready_reported) {
        this->ready_reported = true;
        qInfo() << "bgfx map: first frame presented";
        emit signalRendererReady();
    }
}


// AOWIS_BGFX_DEBUG=1: bgfx's text overlay with renderer statistics (it would
// cover the HUD, so it is off by default).
void MapBgfxSurface::renderDebugOverlay()
{
    bgfx::dbgTextClear();
    bgfx::dbgTextPrintf(2, 2, 0x0f, "AOWIS bgfx map");
    bgfx::dbgTextPrintf(2, 4, 0x0e, "2D network, imagery, heatmap, picking, HUD active");
    bgfx::dbgTextPrintf(2, 6, 0x0f, "Camera lon %.5f lat %.5f",
                        this->camera_state.center_lon_deg, this->camera_state.center_lat_deg);
    if (this->camera_state.mode == MapViewMode::TwoD)
        bgfx::dbgTextPrintf(2, 8, 0x0f, "Continuous zoom %.4f", this->camera_state.continuous_2d_zoom);
    else
        bgfx::dbgTextPrintf(2, 8, 0x0f, "Globe distance %.2f m", this->camera_state.globe_distance_m);
    // Nodes include demand points (snapshot nodes of entity DemandPoint).
    bgfx::dbgTextPrintf(2, 10, 0x0f, "Links %lld (%lld segments) Nodes %lld Attachments %lld",
                        static_cast<long long>(this->prepared_2d_scenes.active().linkPolylines().size()),
                        static_cast<long long>(this->prepared_2d_scenes.active().linkSegmentCount()),
                        static_cast<long long>(this->prepared_2d_scenes.active().networkNodes().size()),
                        static_cast<long long>(this->prepared_2d_scenes.active().attachments().size()));
    bgfx::dbgTextPrintf(2, 11, 0x0f,
                        "Preparations %llu GPU uploads %llu (%llu bytes) Frames %llu",
                        static_cast<unsigned long long>(this->prepared_2d_scenes.active().preparationCount()),
                        static_cast<unsigned long long>(this->gpu_2d_scene.uploadCount()),
                        static_cast<unsigned long long>(this->gpu_2d_scene.uploadedBytes()),
                        static_cast<unsigned long long>(this->frame_count));
    bgfx::dbgTextPrintf(2, 14, 0x0f, "Entity data uploads %llu (%llu bytes), flow chevrons %lld",
                        static_cast<unsigned long long>(this->entity_style.textureUploadCount()),
                        static_cast<unsigned long long>(this->entity_style.textureUploadBytes()),
                        static_cast<long long>(this->gpu_2d_scene.chevronCount()));
    bgfx::dbgTextPrintf(2, 13, 0x0f, "Network chunks %d/%d drawn, %d draw calls",
                        this->network_frame_stats.chunks_drawn,
                        this->network_frame_stats.chunks_total,
                        this->network_frame_stats.draw_calls);
    bgfx::dbgTextPrintf(2, 12, 0x0f,
                        "Imagery z%d quads %d draw calls %d tiles %d uploads %llu",
                        this->imagery_zoom, this->imagery_quad_count,
                        this->imagery_gpu.drawCallCount(), this->imagery_gpu.textureCount(),
                        static_cast<unsigned long long>(this->imagery_gpu.uploadCount()));
}

QString MapBgfxSurface::startupDiagnostics() const
{
    QString stage;
    if (this->failed)
        stage = QStringLiteral("failed");
    else if (!this->initialization_attempted)
        stage = isVisible() ? QStringLiteral("not initialized yet (waiting for the event loop)")
                            : QStringLiteral("not initialized: map surface not visible");
    else if (!this->initialized)
        stage = QStringLiteral("initialization did not complete (stuck in: %1)")
                    .arg(QString::fromLatin1(this->startup_step));
    else if (!this->frame_loop_running)
        stage = QStringLiteral("initialized, frame loop not running");
    else if (this->frame_count == 0)
        stage = QStringLiteral("initialized, no frame presented yet");
    else
        stage = QStringLiteral("presenting frames");
    const QSize pixels = physicalSize();
    return QStringLiteral("%1; visible %2, size %3x%4 px, frame callbacks %5, frames %6")
        .arg(stage)
        .arg(isVisible() ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(pixels.width())
        .arg(pixels.height())
        .arg(this->frame_callback_count)
        .arg(this->frame_count)
#ifdef Q_OS_WASM
        // A JavaScript exception thrown inside bgfx/Emscripten code unwinds
        // the Qt callback silently; the page recorded it.
        + []()
        {
            QByteArray buffer(512, '\0');
            const int length = aowisLastBrowserError(buffer.data(), int(buffer.size()));
            const QString error = QString::fromUtf8(buffer.constData(), std::max(0, length));
            return error.isEmpty() ? QString()
                                   : QStringLiteral("\nlast browser error: %1").arg(error);
        }()
#endif
        ;
}

void MapBgfxSurface::fail(const QString &reason)
{
    this->failed = true;
    stopFrameLoop();
    if (this->initialized) {
        releaseGpuResources();
        bgfx::shutdown();
        this->initialized = false;
        if (g_bgfx_device_owner == this)
            g_bgfx_device_owner = nullptr;
    }
    qWarning().noquote() << reason;
    emit signalRendererFailed(reason);
}

void MapBgfxSurface::setTileRepository(MapTileRepository *tile_repository)
{
    requestRedraw();
    if (this->tile_repository == tile_repository)
        return;
    if (this->tile_repository != nullptr)
        QObject::disconnect(this->tile_repository, nullptr, this, nullptr);
    this->tile_repository = tile_repository;
    this->imagery_layout_key = 0;
    this->imagery_requests_dirty = true;
    this->globe_surface->setTileRepository(tile_repository);
    if (this->tile_repository == nullptr)
        return;
    // A failed tile becoming retryable needs a fresh request; an arriving
    // tile needs nothing (the next frame finds it in the repository cache).
    connect(this->tile_repository, &MapTileRepository::signalTileRetryReady, this,
            [this](const QString &)
    {
        this->imagery_requests_dirty = true;
        this->globe_surface->requestAgain();
        requestRedraw();
    });
    // On-demand rendering: an arriving tile is drawn by the next frame.
    connect(this->tile_repository, &MapTileRepository::signalTileAvailable, this,
            [this](const QString &) { requestRedraw(); });
    connect(this->tile_repository, &MapTileRepository::signalTilesDeleted, this, [this]()
    {
        if (this->initialized)
            this->imagery_gpu.clearTextures();
        this->imagery_requests_dirty = true;
        this->globe_surface->requestAgain();
        requestRedraw();
    });
}

void MapBgfxSurface::setBackgroundOpacity(int opacity)
{
    requestRedraw();
    this->background_opacity = qBound(0, opacity, 100);
}

void MapBgfxSurface::renderImagery(const QSizeF &logical)
{
    this->imagery_quad_count = 0;
    this->imagery_uploads_pending = false;
    const MapBgfx2d::WorldPoint center = MapBgfx2d::worldFromWgs84(
        this->camera_state.center_lon_deg, this->camera_state.center_lat_deg);
    MapBgfx2dImagery::Camera camera;
    camera.center_x = center.x;
    camera.center_y = center.y;
    camera.zoom = this->camera_state.continuous_2d_zoom;
    camera.viewport_width = logical.width();
    camera.viewport_height = logical.height();

    // Only GPU-resident textures count as available for drawing; repository
    // tiles are uploaded below within a per-frame budget, nearest first.
    const MapBgfx2dImagery::Availability resident =
        [this](const MapBgfx2dImagery::TileId &tile) { return this->imagery_gpu.hasTexture(tile); };
    MapBgfx2dImagery::Layout layout = MapBgfx2dImagery::buildLayout(
        camera, MapModel::MinZoom, this->map_model->imageryMaximumZoom(), resident);
    this->imagery_zoom = layout.imagery_zoom;

    if (this->tile_repository != nullptr &&
        !this->map_model->tileSourcePath(layout.imagery_zoom).isEmpty() &&
        !layout.requests.empty())
    {
        if (layout.layout_key != this->imagery_layout_key)
        {
            // New target tile set: a new batch lets the repository drop
            // queued requests that are no longer visible.
            this->imagery_layout_key = layout.layout_key;
            this->imagery_request_batch = this->tile_repository->beginTileRequestBatch(
                this, QStringLiteral("bgfx|%1|%2")
                    .arg(this->map_model->tileCachePrefix(layout.imagery_zoom))
                    .arg(layout.layout_key));
            this->imagery_requests_dirty = true;
        }
        // Uploads take atlas cells from the least recently used tiles: the
        // ones this frame draws are taken as used first.
        this->imagery_gpu.markUsed(layout);
        int uploads = 0;
        for (const MapBgfx2dImagery::Request &request : layout.requests)
        {
            const QString key = this->map_model->tileCacheKey(
                request.tile.x, request.tile.y, request.tile.zoom);
            const QImage *image = this->tile_repository->tile(key);
            if (image != nullptr)
            {
                // Bounded so a burst of arriving tiles never stalls a frame.
                if (uploads < kImageryUploadsPerFrame
                    && this->imagery_gpu.uploadTexture(request.tile, *image))
                {
                    ++uploads;
                }
                else if (uploads >= kImageryUploadsPerFrame)
                {
                    // Over this frame's budget: upload it in the next one.
                    this->imagery_uploads_pending = true;
                }
                continue;
            }
            if (this->imagery_requests_dirty)
            {
                this->tile_repository->requestTile(
                    this->map_model->tileEndpoint(
                        request.tile.x, request.tile.y, request.tile.zoom),
                    key, request.tile.x, request.tile.y, request.priority,
                    this->imagery_request_batch);
            }
        }
        this->imagery_requests_dirty = false;
        if (uploads > 0)
            layout = MapBgfx2dImagery::buildLayout(
                camera, MapModel::MinZoom, this->map_model->imageryMaximumZoom(), resident);
    }

    this->imagery_quad_count = int(layout.quads.size());
    this->imagery_gpu.draw(kMapBgfxView, layout, logical.width(), logical.height(),
                           palette().color(QPalette::Window),
                           double(this->background_opacity) / 100.0);
    this->imagery_gpu.endFrame();
}

void MapBgfxSurface::setDistributionNetworkVisible(bool visible)
{
    if (this->distribution_network_visible == visible)
        return;
    this->distribution_network_visible = visible;
    // Do not clear or recreate retained geometry, prepared scene variants,
    // or GPU buffers. Only schedule another presentation frame.
    requestRedraw();
}

void MapBgfxSurface::setSymbology(const MapNetworkRenderSymbology &symbology)
{
    requestRedraw();
    this->symbology = symbology;
    // Values, ranges, palettes: data textures and uniforms, no geometry.
    this->entity_style.setSymbology(this->symbology);
    // New values (time step, metric) and symbol sizes.
    this->labels_dirty = true;
    this->globe_network.setSymbology(globeSymbology());
    syncGlobeHeatmap();
}

void MapBgfxSurface::setVisualControlSettings(const NetworkSymbologySettings &settings)
{
    requestRedraw();
    // Cheap path for slider/toggle and palette changes: sizes, the heatmap
    // radius, opacity and solid centre, the junction and flow-arrow toggles
    // are uniforms, the palettes the palette texture. Values arrive through
    // setSymbology().
    const NetworkSymbologySettings bounded = settings.bounded();
    this->symbology.node_palette = bounded.node_palette;
    this->symbology.node_palette_flipped = bounded.node_palette_flipped;
    this->symbology.link_palette = bounded.link_palette;
    this->symbology.link_palette_flipped = bounded.link_palette_flipped;
    this->symbology.heatmap_palette = bounded.heatmap_palette;
    this->symbology.heatmap_palette_flipped = bounded.heatmap_palette_flipped;
    this->symbology.node_size_unit = bounded.node_size_unit;
    this->symbology.node_size_px = bounded.node_size_px;
    this->symbology.node_size_m = bounded.node_size_m;
    this->symbology.link_thickness_unit = bounded.link_thickness_unit;
    this->symbology.link_thickness_px = bounded.link_thickness_px;
    this->symbology.link_thickness_m = bounded.link_thickness_m;
    this->symbology.show_junctions = bounded.show_junctions;
    this->symbology.icon_size_unit = bounded.icon_size_unit;
    this->symbology.icon_size_px = bounded.icon_size_px;
    this->symbology.icon_size_m = bounded.icon_size_m;
    this->symbology.show_flow_direction = bounded.show_flow_direction;
    this->symbology.flow_direction_size_px = bounded.flow_direction_size_px;
    this->symbology.heatmap_opacity = bounded.heatmap_opacity;
    this->symbology.heatmap_radius_unit = bounded.heatmap_radius_unit;
    this->symbology.heatmap_radius_m = bounded.heatmap_radius_m;
    this->symbology.heatmap_radius_px = bounded.heatmap_radius_px;
    this->symbology.heatmap_solid_center_percent = bounded.heatmap_solid_center_percent;
    this->entity_style.setSymbology(this->symbology);
    // Junction visibility and icon sizes move the labels' symbol boxes.
    this->labels_dirty = true;
    this->globe_network.setSymbology(globeSymbology());
    // Radius and solid centre.
    syncGlobeHeatmap();
}

void MapBgfxSurface::syncGlobeHeatmap()
{
    QVector<MapGlobeHeatmapMarker> markers;
    if (this->globe_ready && this->map_model->viewMode() == MapViewMode::Globe
        && this->symbology.visual_heatmap != VisualHeatmap::None && this->retained_scene.hasSnapshot())
    {
        const NetworkRenderSnapshot &snapshot = this->retained_scene.snapshot();
        const double heat_minimum = this->symbology.ranges.heatmap_minimum;
        const double heat_maximum = this->symbology.ranges.heatmap_maximum;
        markers.reserve(snapshot.nodes.size());
        for (const NetworkRenderNode &node : snapshot.nodes)
        {
            if (this->hidden_entity_uuids.contains(node.uuid) || !std::isfinite(node.coordinate_wgs84.longitude_deg)
                || !std::isfinite(node.coordinate_wgs84.latitude_deg))
                continue;
            MapGlobeHeatmapMarker marker;
            marker.render_id = node.render_id;
            marker.longitude_deg = node.coordinate_wgs84.longitude_deg;
            marker.latitude_deg = node.coordinate_wgs84.latitude_deg;
            const double value = mapNetworkSymbologyValue(this->symbology.heatmap_values, node.render_id);
            marker.active = std::isfinite(value) && std::isfinite(heat_minimum) && std::isfinite(heat_maximum);
            if (marker.active)
            {
                const double fraction = heat_minimum == heat_maximum
                    ? 0.5 : std::clamp((value - heat_minimum) / (heat_maximum - heat_minimum), 0.0, 1.0);
                marker.color = networkSymbologyInterpolatedRampColor(
                    fraction, this->symbology.heatmap_palette, this->symbology.heatmap_palette_flipped);
            }
            markers.append(marker);
        }
        // Stable marker indices across time steps (the scene's stamp layout
        // cache) whatever order the snapshot has.
        std::sort(markers.begin(), markers.end(),
                  [](const MapGlobeHeatmapMarker &first, const MapGlobeHeatmapMarker &second)
        {
            if (first.render_id != second.render_id)
                return first.render_id < second.render_id;
            if (first.longitude_deg != second.longitude_deg)
                return first.longitude_deg < second.longitude_deg;
            return first.latitude_deg < second.latitude_deg;
        });
    }
    double radius_m = 0.0;
    if (!markers.isEmpty())
    {
        if (this->symbology.heatmap_radius_unit != HeatmapRadiusUnit::Pixels)
        {
            // Terrain tiles are real-world sized: metres as they are.
            radius_m = std::max(0.0, double(this->symbology.heatmap_radius_m));
        }
        else
        {
            // Stable on screen at the orbit distance, as the node sizes.
            const double pixel_radius = std::max(1, this->symbology.heatmap_radius_px);
            const double viewport_height = std::max(1, globeViewportSize().height());
            const double reference_depth = std::max(1.0, this->map_model->viewGlobeDistanceM());
            radius_m = 2.0 * pixel_radius * reference_depth
                * std::tan(qDegreesToRadians(MapModel::GlobeFieldOfViewDeg / 2.0)) / viewport_height;
        }
    }
    const double solid_fraction =
        std::clamp(double(this->symbology.heatmap_solid_center_percent) / 100.0, 0.0, 0.9);
    this->globe_surface->setHeatmapOverlay(markers, radius_m, solid_fraction);
    requestRedraw();
}

MapNetworkRenderSymbology MapBgfxSurface::globeSymbology() const
{
    // As the former QRhi monitor: a neutral icon fill
    // on dark windows, a soft pink on light ones (visibility on imagery).
    MapNetworkRenderSymbology themed = this->symbology;
    if (this->globe_flat_entity_icons_enabled)
        themed.show_icons = false;
    // An explicit per-mode icon fill (e.g. Monitor's white demand-point
    // stars) must survive 2D/3D switching. Only the unconfigured/default
    // colour is replaced by the globe's window-theme colour.
    if (themed.icon_default_fill_color != MapNetworkRenderSymbology().icon_default_fill_color)
        return themed;
    const QColor window_color = palette().color(QPalette::Window);
    themed.icon_default_fill_color = networkSymbologyIconDefaultFillColor(window_color);
    const double luminance = 0.2126 * window_color.red() + 0.7152 * window_color.green()
        + 0.0722 * window_color.blue();
    if (luminance >= 128.0)
        themed.icon_default_fill_color = qRgb(244, 174, 194);
    return themed;
}

void MapBgfxSurface::synchronizeEntityStyle()
{
    if (this->entity_classes_dirty && this->retained_scene.hasSnapshot())
    {
        this->entity_style.setEntities(this->retained_scene.snapshot());
        this->entity_classes_dirty = false;
    }
    if (this->entity_state_dirty)
    {
        // uuid states -> entity keys (render ids are only unique per class).
        QSet<quint64> hidden_keys;
        for (const QUuid &uuid : this->hidden_entity_uuids)
        {
            const QHash<QUuid, quint64>::const_iterator found = this->entity_key_by_uuid.constFind(uuid);
            if (found != this->entity_key_by_uuid.cend())
                hidden_keys.insert(found.value());
        }
        quint64 selected_key = 0;
        const QHash<QUuid, quint64>::const_iterator selected =
            this->entity_key_by_uuid.constFind(this->selected_entity_uuid);
        if (!this->selected_entity_uuid.isNull() && selected != this->entity_key_by_uuid.cend()
            && selected.value() == MapBgfx2dScene::entityKey(this->selected_entity_type, quint32(selected.value())))
            selected_key = selected.value();
        QHash<quint64, bool> diagnostics;
        for (QHash<QUuid, InfrastructureEntity>::const_iterator error = this->simulation_error_entities.cbegin();
             error != this->simulation_error_entities.cend(); ++error)
        {
            const QHash<QUuid, quint64>::const_iterator found = this->entity_key_by_uuid.constFind(error.key());
            if (found == this->entity_key_by_uuid.cend()
                || found.value() != MapBgfx2dScene::entityKey(error.value(), quint32(found.value())))
                continue;
            diagnostics.insert(found.value(), this->simulation_stale_entity_uuids.contains(error.key()));
        }
        this->entity_style.setHiddenEntities(hidden_keys);
        this->entity_style.setSelectedEntity(selected_key);
        this->entity_style.setDiagnosticEntities(diagnostics);
        this->entity_state_dirty = false;
    }
    this->entity_style.synchronize();
}

void MapBgfxSurface::setGlobeFlatEntityIconsEnabled(bool enabled)
{
    if (this->globe_flat_entity_icons_enabled == enabled)
        return;
    this->globe_flat_entity_icons_enabled = enabled;
    this->globe_network.setSymbology(globeSymbology());
    requestRedraw();
}

void MapBgfxSurface::setGlobe3dIconsEnabled(bool enabled)
{
    requestRedraw();
    this->globe_network.setUse3dIconModels(enabled);
}

void MapBgfxSurface::setUndergroundMode(MapUndergroundMode mode)
{
    requestRedraw();
    if (this->underground_mode == mode)
        return;
    this->underground_mode = mode;
    this->globe_network.setUndergroundMode(mode);
    if (mode == MapUndergroundMode::XRay)
    {
        this->globe_underground_refresh_requested = true;
        scheduleGlobeUndergroundRefresh();
    }
    else
    {
        this->globe_underground_refresh_requested = false;
        ++this->globe_underground_refresh_generation;
    }
}

void MapBgfxSurface::scheduleGlobeUndergroundRefresh()
{
    if (!this->globe_underground_refresh_requested || !this->globe_network_ready
        || this->underground_mode != MapUndergroundMode::XRay
        || this->map_model->viewMode() != MapViewMode::Globe)
        return;
    const quint64 generation = ++this->globe_underground_refresh_generation;
    QTimer::singleShot(500, this, [this, generation]()
    {
        if (generation != this->globe_underground_refresh_generation
            || !this->globe_underground_refresh_requested || !this->globe_network_ready
            || this->underground_mode != MapUndergroundMode::XRay
            || this->map_model->viewMode() != MapViewMode::Globe)
            return;
        // Terrain meshes still being built: the classification would only
        // be replaced again; wait for the next quiet period.
        if (this->globe_surface->hasPendingTerrainMeshes())
        {
            scheduleGlobeUndergroundRefresh();
            return;
        }
        this->globe_underground_refresh_requested = false;
        if (this->globe_network.refreshUndergroundXRay())
            requestRedraw();
    });
}
