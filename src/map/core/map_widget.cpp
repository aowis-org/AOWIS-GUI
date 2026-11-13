#include "map/core/map_widget.h"
#include "map/core/map_input_focus_policy.h"
#include "map/core/map_zoom_animation.h"
#include "config/gui_configuration.h"
#include "config/shortcut_registry.h"

#include <QApplication>

#ifndef Q_OS_WASM
#include <QCursor>
#include <QGeoCoordinate>
#include <QScreen>
#include <QWindow>
#else
#include <emscripten.h>
#endif

#include <cmath>
#include <utility>

#ifdef Q_OS_WASM
// Browser viewport and focus state for edge panning (pointer position comes
// from the window-level Emscripten mouse callbacks below).
EM_JS(int, aowisBrowserViewportWidth, (),
{
    return window.innerWidth | 0;
});

EM_JS(int, aowisBrowserViewportHeight, (),
{
    return window.innerHeight | 0;
});

EM_JS(int, aowisBrowserDocumentActive, (),
{
    return document.hasFocus() && !document.hidden ? 1 : 0;
});

#endif

namespace
{

constexpr int PanFrameIntervalMs = 16;
constexpr int EdgePanMarginPixels = 2;
constexpr int EdgePanPollIntervalMs = 50;
constexpr double PanMaximumSpeedPixelsPerSecond = 900.0;
constexpr double PanFastSpeedMultiplier = 2.0;
constexpr double PanFastAccelerationMultiplier = 1.75;
constexpr double PanAccelerationPixelsPerSecondSquared = 2800.0;
constexpr double PanDecelerationPixelsPerSecondSquared = 3600.0;
constexpr double PanVelocityStopThreshold = 0.5;
constexpr int MousePanReleaseTimeoutMs = 100;
constexpr double MousePanVelocitySmoothing = 0.65;
constexpr double MousePanMaximumSpeedPixelsPerSecond = 2400.0;
constexpr double MousePanMinimumInertiaSpeedPixelsPerSecond = 70.0;
constexpr double MousePanInertiaDecelerationPixelsPerSecondSquared = 2600.0;
// Held-key zoom rate, identical in 2D and on the globe (both in octaves).
// Shift doubles it, as for keyboard panning.
constexpr double KeyboardZoomOctavesPerSecond = 1.5;
constexpr double KeyboardZoomFastMultiplier = 2.0;
// Globe wheel notches have always scaled the orbit distance by 0.85; in the
// shared octave units that is -log2(0.85) ~= 0.2345 octave per notch.
const double GlobeOctavesPerWheelStep = -std::log2(0.85);

qreal vectorLength(const QPointF &vector)
{
    return std::hypot(vector.x(), vector.y());
}

QPointF normalized(const QPointF &vector)
{
    const qreal length = vectorLength(vector);
    if (qFuzzyIsNull(length))
        return QPointF();

    return vector / length;
}

QPointF moveTowards(const QPointF &current, const QPointF &target, qreal maximum_delta)
{
    const QPointF delta = target - current;
    const qreal distance = vectorLength(delta);

    if (distance <= maximum_delta || qFuzzyIsNull(distance))
        return target;

    return current + delta / distance * maximum_delta;
}
}

MapWidget::MapWidget(MapModel *model, MapTileRepository *tile_repository, GpsProvider *gps, QWidget *parent)
    : QWidget(parent),
    gps(gps),
    m_model(model),
    tile_repository(tile_repository)
{
    Q_ASSERT(this->m_model);
    Q_ASSERT(this->tile_repository);

    this->init();
    // QWidget::focusOutEvent is not guaranteed when keyboard focus belongs to
    // the embedded bgfx QWindow (or when the compositor deactivates the app).
    // Watch the application as well as the native surface's own focus events.
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state)
    {
        if (mapApplicationInputFocusLost(state))
            cancelNavigationInput();
    });
}

MapWidget::~MapWidget()
{
#ifndef Q_OS_WASM
    endView3dOrbit(false);
#endif
#ifdef Q_OS_WASM
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, nullptr);
    emscripten_set_mouseleave_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, nullptr);
#endif
}

MapModel *MapWidget::model() const
{
    return this->m_model;
}

void MapWidget::fitViewToBounds(
    const CoordinateWGS84 &minimum, const CoordinateWGS84 &maximum,
    double elevation_minimum_m, double elevation_maximum_m)
{
    if (this->m_model == nullptr)
        return;

    cancelSmoothZoom();
    this->m_model->fitViewToBounds(
        minimum, maximum, size(), elevation_minimum_m, elevation_maximum_m);
}

void MapWidget::deleteCachedTiles(int zoom, int tile_x_min, int tile_x_max, int tile_y_min, int tile_y_max)
{
    const int bounded_zoom = qBound(MapModel::MinZoom, zoom, MapModel::MaxZoom);
    const int source_zoom = this->m_model->imageryTileZoom(bounded_zoom);
    if (source_zoom > bounded_zoom)
        return;
    const int reduction = bounded_zoom - source_zoom;
    this->tile_repository->deleteTiles(
        this->m_model->tileSourcePath(source_zoom),
        source_zoom,
        tile_x_min >> reduction,
        tile_x_max >> reduction,
        tile_y_min >> reduction,
        tile_y_max >> reduction);
}

void MapWidget::init()
{
    // Input only: no background, no painting (paintEvent() is QWidget's).
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);

    this->setMinimumHeight(500);
    this->setMinimumWidth(550);
    this->setContentsMargins(0, 0, 0, 0);

    this->setFocusPolicy(Qt::StrongFocus);
    this->setFocus();
    this->setMouseTracking(true);

    connect(this->m_model, &MapModel::view2dContinuousZoomChanged, this, [this](double zoom)
    {
        if (this->m_model->viewMode() == MapViewMode::TwoD)
            emit signalZoomLevelChanged(zoom);
    });

    connect(this->m_model, &MapModel::viewGlobeCameraChanged, this, [this]
    {
        if (this->m_model->viewMode() == MapViewMode::Globe)
            emit signalZoomLevelChanged(this->m_model->viewGlobeZoomLevel(size()));
    });

    connect(this->m_model, &MapModel::centerChangedWGS84, this, [this](CoordinateWGS84 wgs)
    {
        if (this->m_model->viewMode() == MapViewMode::Globe)
            emit signalZoomLevelChanged(this->m_model->viewGlobeZoomLevel(size()));
        if (this->m_model->viewMode() == MapViewMode::Globe
            && this->render_surface_screen_coordinate_resolver)
        {
            updatePointerCoordinates(
                QPoint(width() / 2, height() / 2));
        }
        else
        {
            // Keep 2D centre readouts aware of any cached 3D terrain too.
            emit signalCoordsChangedWgs84(withCachedTerrainAltitude(wgs));
        }
    });

    connect(this->m_model, &MapModel::centerChangedUTM, this,
            [this](CoordinateUTM utm)
    {
        if (this->m_model->viewMode() == MapViewMode::Globe
            && this->render_surface_screen_coordinate_resolver)
        {
            return;
        }
        emit signalCoordsChangedUTM(utm);
    });

    connect(this->m_model, &MapModel::viewModeChanged, this, [this](MapViewMode view_mode)
    {
        // The motion's octave units differ per mode (2D zoom level versus
        // -log2(globe distance)); never carry a glide across a mode switch.
        // A still-held zoom key restarts in the new mode on the next frame.
        cancelSmoothZoom();
        if (hasKeyboardZoomInput())
            ensurePanAnimationRunning();
        const bool orbit_camera_mode = view_mode == MapViewMode::Globe;
        if (!orbit_camera_mode)
        {
            if (this->view_3d_orbit_active)
                endView3dOrbit();
            stopPanAnimationIfIdle();
        }

        if (this->render_surface_screen_coordinate_resolver)
        {
            updatePointerCoordinates(
                QPoint(width() / 2, height() / 2));
        }

        if (view_mode == MapViewMode::Globe)
            emit signalZoomLevelChanged(this->m_model->viewGlobeZoomLevel(size()));
        else
            emit signalZoomLevelChanged(this->m_model->view2dContinuousZoom());
    });

    // No CPU map painting: imagery (and the GPS dot) are drawn by the bgfx
    // surface, which follows the tile repository itself.
    if (this->gps)
    {
        connect(this->gps, &GpsProvider::statusMessage, this, [](const QString &message)
        {
            qDebug() << message;
        });
    }

    QTimer::singleShot(0, this, [this]
    {
        emit signalZoomLevelChanged(this->m_model->viewMode() == MapViewMode::Globe
                                        ? this->m_model->viewGlobeZoomLevel(size())
                                        : this->m_model->view2dContinuousZoom());

        CoordinateWGS84 wgs;
        wgs.latitude_deg = this->m_model->centerLat();
        wgs.longitude_deg = this->m_model->centerLon();
        emit signalCoordsChangedWgs84(wgs);

        GeoMetricProjection projection;
        const CoordinateUTM utm = projection.wgs84ToUtm(wgs);
        emit signalCoordsChangedUTM(utm);
    });

    this->initPanAnimation();
}

void MapWidget::initPanAnimation()
{
    this->pan_timer = new QTimer(this);
    this->pan_timer->setInterval(PanFrameIntervalMs);
    this->pan_timer->setTimerType(Qt::PreciseTimer);
    connect(this->pan_timer, &QTimer::timeout, this, &MapWidget::updatePanAnimation);

    this->edge_pan_poll_timer = new QTimer(this);
    this->edge_pan_poll_timer->setInterval(EdgePanPollIntervalMs);
    connect(this->edge_pan_poll_timer, &QTimer::timeout, this, &MapWidget::pollEdgePan);

#ifdef Q_OS_WASM
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, &MapWidget::browserMouseMoveCallback);
    emscripten_set_mouseleave_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, &MapWidget::browserMouseLeaveCallback);
#endif
}

void MapWidget::ensurePanAnimationRunning()
{
    // With an external frame driver the render surface ticks every
    // presented frame anyway; a second timer would reintroduce beating.
    if (this->external_navigation_frame_driver || this->pan_timer->isActive())
        return;

    this->pan_elapsed_timer.start();
    this->pan_timer->start();
}

void MapWidget::setNavigationFrameDriver(bool external)
{
    if (this->external_navigation_frame_driver == external)
        return;

    this->external_navigation_frame_driver = external;
    if (external)
    {
        this->pan_timer->stop();
        return;
    }
    if (this->zoom_motion.active() || hasKeyboardPanInput() || hasKeyboardZoomInput()
        || vectorLength(this->pan_velocity) >= PanVelocityStopThreshold)
    {
        ensurePanAnimationRunning();
    }
}

void MapWidget::advanceNavigationFrame(double elapsed_seconds)
{
    if (!this->external_navigation_frame_driver)
        return;
    advanceNavigation(qBound<qreal>(0.0, qreal(elapsed_seconds), 0.05));
}

#ifndef Q_OS_WASM
void MapWidget::setOrbitPointerLockWindow(QWindow *window)
{
    // Never retarget a live lock; the next orbit gesture uses the new window.
    if (this->view_3d_orbit_active)
        endView3dOrbit(false);
    this->orbit_pointer_lock_window = window;
}
#endif

void MapWidget::stopPanAnimationIfIdle()
{
    if (this->zoom_motion.active() || hasKeyboardPanInput() || hasKeyboardZoomInput() || joystickPanActive()
        || vectorLength(this->pan_velocity) >= PanVelocityStopThreshold)
    {
        return;
    }

    this->pan_velocity = QPointF();
    this->pan_fractional_delta = QPointF();
    this->pan_timer->stop();
}

void MapWidget::stopAllPanMovement()
{
    this->pan_key_left_pressed = false;
    this->pan_key_right_pressed = false;
    this->pan_key_up_pressed = false;
    this->pan_key_down_pressed = false;
    updateKeyboardPanIndicator();
    this->pan_fast_modifier_pressed = false;
    this->zoom_in_key_pressed = false;
    this->zoom_out_key_pressed = false;
    cancelSmoothZoom();
    this->keyboard_pan_motion_active = false;
    this->mouse_pan_active = false;
    this->mouse_pan_sensitivity_fractional_delta = QPointF();
    this->pan_velocity = QPointF();
    this->pan_fractional_delta = QPointF();
    this->mouse_pan_velocity = QPointF();
    this->mouse_pan_inertia_active = false;
    this->mouse_pan_drag_distance = 0;

    this->pan_timer->stop();
}

void MapWidget::updatePanAnimation()
{
    const qint64 elapsed_ms = this->pan_elapsed_timer.restart();
    const qreal elapsed_seconds = qBound<qreal>(0.0, elapsed_ms / 1000.0, 0.05);

    if (elapsed_seconds <= 0.0)
        return;

    advanceNavigation(elapsed_seconds);
}

void MapWidget::advanceNavigation(qreal elapsed_seconds)
{
    if (elapsed_seconds <= 0.0)
        return;

    // The map stack shows all its pages (QStackedLayout::StackAll), so this
    // widget is visible exactly while the map is; stop when its tab hides.
    if (!isVisible())
    {
        stopAllPanMovement();
        return;
    }

    updateSmoothZoom(elapsed_seconds);

    if (this->mouse_pan_active)
    {
        this->pan_velocity = QPointF();
        this->pan_fractional_delta = QPointF();
        return;
    }

    if (joystickPanActive())
    {
        // Pushing the knob up moves the view up: the map content moves
        // the other way (a pan delta is the content's motion).
        const QPointF deflection = joystickDeflection();
        const double strength = std::hypot(deflection.x(), deflection.y());
        this->pan_velocity = -deflection * (strength * JoystickMaximumSpeedPixelsPerSecond);
        this->mouse_pan_inertia_active = false;
        this->keyboard_pan_motion_active = false;
        const QPointF precise_delta = this->pan_velocity * elapsed_seconds + this->pan_fractional_delta;
        const QPoint delta(int(std::trunc(precise_delta.x())), int(std::trunc(precise_delta.y())));
        this->pan_fractional_delta = precise_delta - QPointF(delta);
        if (!delta.isNull())
            panMapByPixels(delta);
        return;
    }

    QPointF direction = keyboardPanDirection();
    const bool keyboard_pan_active = !direction.isNull();
    bool edge_pan_active = false;
    if (keyboard_pan_active)
    {
        this->keyboard_pan_motion_active = true;
    }
    else
    {
        direction = edgePanDirection();
        edge_pan_active = !direction.isNull();
        if (edge_pan_active)
            this->keyboard_pan_motion_active = false;
    }
    direction = normalized(direction);

    if (!direction.isNull())
    {
        this->mouse_pan_inertia_active = false;
    }

    bool fast_pan_active = keyboard_pan_active && hasFastKeyboardPanInput();
#ifndef Q_OS_WASM
    if (edge_pan_active && QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
        fast_pan_active = true;
#endif

    qreal maximum_speed = PanMaximumSpeedPixelsPerSecond;
    qreal acceleration = PanAccelerationPixelsPerSecondSquared;
    if (fast_pan_active)
    {
        maximum_speed *= PanFastSpeedMultiplier;
        acceleration *= PanFastAccelerationMultiplier;
    }

    if (direction.isNull() && this->mouse_pan_inertia_active)
        acceleration = MousePanInertiaDecelerationPixelsPerSecondSquared;
    else if (direction.isNull())
        acceleration = PanDecelerationPixelsPerSecondSquared;

    const QPointF target_velocity = direction * maximum_speed;
    this->pan_velocity = moveTowards(this->pan_velocity, target_velocity, acceleration * elapsed_seconds);

    if (direction.isNull() && vectorLength(this->pan_velocity) < PanVelocityStopThreshold)
    {
        this->pan_velocity = QPointF();
        this->mouse_pan_inertia_active = false;
        this->keyboard_pan_motion_active = false;
    }

    const QPointF precise_delta = this->pan_velocity * elapsed_seconds + this->pan_fractional_delta;
    const QPoint delta(int(std::trunc(precise_delta.x())), int(std::trunc(precise_delta.y())));
    this->pan_fractional_delta = precise_delta - QPointF(delta);

    if (!delta.isNull())
        panMapByPixels(delta, this->keyboard_pan_motion_active);

    stopPanAnimationIfIdle();
}

void MapWidget::pollEdgePan()
{
    if (!this->edgePanDirection().isNull())
        this->ensurePanAnimationRunning();
}

#ifdef Q_OS_WASM
EM_BOOL MapWidget::browserMouseMoveCallback(int event_type, const EmscriptenMouseEvent *event, void *user_data)
{
    Q_UNUSED(event_type)

    MapWidget *map_widget = static_cast<MapWidget *>(user_data);
    if (map_widget == nullptr || event == nullptr)
        return EM_FALSE;

    map_widget->browser_pointer_position = QPoint(event->clientX, event->clientY);
    map_widget->browser_pointer_inside = true;

    if (map_widget->edge_panning_enabled && map_widget->isVisible() && !map_widget->edgePanDirection().isNull())
        map_widget->ensurePanAnimationRunning();

    return EM_FALSE;
}

EM_BOOL MapWidget::browserMouseLeaveCallback(int event_type, const EmscriptenMouseEvent *event, void *user_data)
{
    Q_UNUSED(event_type)
    Q_UNUSED(event)

    MapWidget *map_widget = static_cast<MapWidget *>(user_data);
    if (map_widget == nullptr)
        return EM_FALSE;

    map_widget->browser_pointer_inside = false;
    return EM_FALSE;
}
#endif

MapWidget::NavigationKeyIdentity MapWidget::navigationKeyIdentity(const QKeyEvent *event)
{
    NavigationKeyIdentity identity;
    identity.scan_code = event->nativeScanCode();
    identity.virtual_key = event->nativeVirtualKey();
    identity.key = event->key();
    return identity;
}

bool MapWidget::sameNavigationKey(const NavigationKeyIdentity &first,
                                  const NavigationKeyIdentity &second)
{
    // The most physical identity both events carry: the scan code (X11,
    // Wayland, Windows), the virtual key (macOS), else the Qt key (browser).
    if (first.scan_code != 0 && second.scan_code != 0)
        return first.scan_code == second.scan_code;
    if (first.virtual_key != 0 && second.virtual_key != 0)
        return first.virtual_key == second.virtual_key;
    return first.key == second.key;
}

bool MapWidget::pressKeyboardPanKey(QKeyEvent *event)
{
    const Qt::KeyboardModifiers arrow_modifiers =
        event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);

    bool *pressed = nullptr;
    NavigationKeyIdentity *identity = nullptr;
    if ((event->key() == Qt::Key_Left && arrow_modifiers == Qt::NoModifier)
        || guiShortcutMatches(event, guiShortcutRegistry().shortcut(GuiShortcutId::MapPanLeft), Qt::ShiftModifier))
    {
        pressed = &this->pan_key_left_pressed;
        identity = &this->pan_key_left_identity;
    }
    else if ((event->key() == Qt::Key_Right && arrow_modifiers == Qt::NoModifier)
             || guiShortcutMatches(event, guiShortcutRegistry().shortcut(GuiShortcutId::MapPanRight), Qt::ShiftModifier))
    {
        pressed = &this->pan_key_right_pressed;
        identity = &this->pan_key_right_identity;
    }
    else if ((event->key() == Qt::Key_Up && arrow_modifiers == Qt::NoModifier)
             || guiShortcutMatches(event, guiShortcutRegistry().shortcut(GuiShortcutId::MapPanUp), Qt::ShiftModifier))
    {
        pressed = &this->pan_key_up_pressed;
        identity = &this->pan_key_up_identity;
    }
    else if ((event->key() == Qt::Key_Down && arrow_modifiers == Qt::NoModifier)
             || guiShortcutMatches(event, guiShortcutRegistry().shortcut(GuiShortcutId::MapPanDown), Qt::ShiftModifier))
    {
        pressed = &this->pan_key_down_pressed;
        identity = &this->pan_key_down_identity;
    }
    if (pressed == nullptr)
        return false;

    *pressed = true;
    *identity = navigationKeyIdentity(event);
    updateKeyboardPanIndicator();
    return true;
}

void MapWidget::updateKeyboardPanIndicator()
{
    // keyboardPanDirection() is the content's motion; the view moves the
    // other way.
    const QPointF direction = -keyboardPanDirection();
    if (direction == this->keyboard_pan_indicator)
        return;
    this->keyboard_pan_indicator = direction;
    emit keyboardPanChanged(direction);
}

bool MapWidget::hasKeyboardPanInput() const
{
    return this->pan_key_left_pressed || this->pan_key_right_pressed || this->pan_key_up_pressed || this->pan_key_down_pressed;
}

bool MapWidget::hasFastKeyboardPanInput() const
{
    return this->pan_fast_modifier_pressed && this->hasKeyboardPanInput();
}

QPointF MapWidget::viewportCenter() const
{
    // Exact geometric centre. QRect::center() is (w-1)/2 truncated, i.e. one
    // pixel left/up of the crosshair for even sizes, which made every
    // keyboard zoom step drift the map by a fraction of a pixel.
    return QPointF(double(width()) * 0.5, double(height()) * 0.5);
}

double MapWidget::zoomMotionOctaves() const
{
    if (this->zoom_motion_mode == MapViewMode::Globe)
        return -std::log2(qMax(MapModel::MinViewGlobeDistanceM,
                               this->m_model->viewGlobeDistanceM()));
    return this->m_model->view2dContinuousZoom();
}

void MapWidget::applyZoomMotionOctaves(double octaves)
{
    if (this->zoom_motion_mode == MapViewMode::Globe)
    {
        this->m_model->setViewGlobeDistanceM(std::exp2(-octaves));
        return;
    }
    const double delta = octaves - this->m_model->view2dContinuousZoom();
    if (delta != 0.0)
        this->m_model->zoomByAt(delta, this->zoom_motion_anchor, size());
}

// Binds the motion to the current view mode. Returns false when no model.
bool MapWidget::prepareZoomMotion()
{
    if (this->m_model == nullptr)
        return false;

    const MapViewMode mode = this->m_model->viewMode() == MapViewMode::Globe
        ? MapViewMode::Globe
        : MapViewMode::TwoD;
    if (this->zoom_motion.active() && this->zoom_motion_mode != mode)
        this->zoom_motion.cancel();
    this->zoom_motion_mode = mode;
    if (mode == MapViewMode::Globe)
    {
        this->zoom_motion.setLimits(
            -std::log2(MapModel::MaxViewGlobeDistanceM),
            -std::log2(MapModel::MinViewGlobeDistanceM));
    }
    else
    {
        this->zoom_motion.setLimits(double(MapModel::MinZoom), double(MapModel::MaxZoom));
    }
    return true;
}

void MapWidget::queueZoomImpulse(double octaves, const QPointF &anchor)
{
    if (!std::isfinite(octaves) || octaves == 0.0 || !prepareZoomMotion())
        return;

    this->zoom_motion_anchor = anchor;
    this->zoom_motion.addImpulse(zoomMotionOctaves(), octaves);
    ensurePanAnimationRunning();
}

void MapWidget::synchronizeKeyboardZoomHold()
{
    if (!prepareZoomMotion())
        return;

    int direction = 0;
    if (this->zoom_in_key_pressed)
        ++direction;
    if (this->zoom_out_key_pressed)
        --direction;

    double octaves_per_second = KeyboardZoomOctavesPerSecond;
    if (this->pan_fast_modifier_pressed)
        octaves_per_second *= KeyboardZoomFastMultiplier;

    // Keyboard zoom always keeps the crosshair fixed, also when the hold
    // continues a wheel glide that was anchored at the mouse pointer.
    if (direction != 0)
        this->zoom_motion_anchor = viewportCenter();
    this->zoom_motion.setHold(zoomMotionOctaves(), direction, octaves_per_second,
                              MapZoomAnimation::KeyboardTapOctaves);
    if (this->zoom_motion.active())
        ensurePanAnimationRunning();
}

void MapWidget::updateSmoothZoom(qreal elapsed_seconds)
{
    if (!this->zoom_motion.active() || this->m_model == nullptr)
        return;

    const MapViewMode mode = this->m_model->viewMode() == MapViewMode::Globe
        ? MapViewMode::Globe
        : MapViewMode::TwoD;
    if (mode != this->zoom_motion_mode)
    {
        cancelSmoothZoom();
        if (hasKeyboardZoomInput())
            synchronizeKeyboardZoomHold();
        return;
    }

    const double current = zoomMotionOctaves();
    const double next = this->zoom_motion.advance(current, double(elapsed_seconds));
    if (next != current)
        applyZoomMotionOctaves(next);
}

void MapWidget::cancelSmoothZoom()
{
    this->zoom_motion.cancel();
}

bool MapWidget::hasKeyboardZoomInput() const
{
    // Keyboard camera motion is never gated by the selected map renderer and
    // never falls back to integer zoom steps.
    return this->m_model != nullptr
        && (this->zoom_in_key_pressed || this->zoom_out_key_pressed);
}

QPointF MapWidget::keyboardPanDirection() const
{
    QPointF direction;

    if (this->pan_key_left_pressed)
        direction.rx() += 1.0;
    if (this->pan_key_right_pressed)
        direction.rx() -= 1.0;
    if (this->pan_key_up_pressed)
        direction.ry() += 1.0;
    if (this->pan_key_down_pressed)
        direction.ry() -= 1.0;

    return direction;
}

QPointF MapWidget::edgePanDirection() const
{
    if (!this->edge_panning_enabled || !this->isVisible())
        return QPointF();

    if (QApplication::activePopupWidget() || QApplication::activeModalWidget())
        return QPointF();

#ifndef Q_OS_WASM
    if (!this->window()->isFullScreen() || !this->window()->isActiveWindow())
        return QPointF();

    QWindow *window_handle = this->window()->windowHandle();
    if (!window_handle || !window_handle->screen())
        return QPointF();

    const QRect screen_geometry = window_handle->screen()->geometry();
    const QPoint cursor_position = QCursor::pos();
    if (!screen_geometry.contains(cursor_position))
        return QPointF();

    QPointF direction;

    if (cursor_position.x() <= screen_geometry.left() + EdgePanMarginPixels)
        direction.rx() += 1.0;
    else if (cursor_position.x() >= screen_geometry.right() - EdgePanMarginPixels)
        direction.rx() -= 1.0;

    if (cursor_position.y() <= screen_geometry.top() + EdgePanMarginPixels)
        direction.ry() += 1.0;
    else if (cursor_position.y() >= screen_geometry.bottom() - EdgePanMarginPixels)
        direction.ry() -= 1.0;

    return direction;
#else
    if (!this->browser_pointer_inside || aowisBrowserDocumentActive() == 0)
        return QPointF();

    const int viewport_width = aowisBrowserViewportWidth();
    const int viewport_height = aowisBrowserViewportHeight();
    if (viewport_width <= 0 || viewport_height <= 0)
        return QPointF();

    const int pointer_x = this->browser_pointer_position.x();
    const int pointer_y = this->browser_pointer_position.y();
    if (pointer_x < 0 || pointer_y < 0 || pointer_x >= viewport_width || pointer_y >= viewport_height)
        return QPointF();

    QPointF direction;

    if (pointer_x <= EdgePanMarginPixels)
        direction.rx() += 1.0;
    else if (pointer_x >= viewport_width - EdgePanMarginPixels - 1)
        direction.rx() -= 1.0;

    if (pointer_y <= EdgePanMarginPixels)
        direction.ry() += 1.0;
    else if (pointer_y >= viewport_height - EdgePanMarginPixels - 1)
        direction.ry() -= 1.0;

    return direction;
#endif
}

bool MapWidget::handleKeyPressEvent(QKeyEvent *event)
{
    if (!event)
        return false;

#ifndef Q_OS_WASM
    const GuiShortcutRegistry &gesture_keys = guiShortcutRegistry();
    const Qt::Key orbit_key = gesture_keys.holdModifierKey(GuiHoldModifierId::GlobeOrbit);
    const Qt::Key look_key = gesture_keys.holdModifierKey(GuiHoldModifierId::GlobeLook);
    if ((event->key() == orbit_key || event->key() == look_key)
        && !event->isAutoRepeat()
        && this->m_model->viewMode() == MapViewMode::Globe
        && !this->view_3d_orbit_active
        && QApplication::mouseButtons() == Qt::NoButton)
    {
        const QPoint global_position = QCursor::pos();
        const QPoint local_position = mapFromGlobal(global_position);
        if (rect().contains(local_position))
        {
            // The orbit key (default Ctrl) orbits around the terrain under
            // the crosshair, the look key (default Alt) looks around from
            // the eye (FPS-style, no gravity).
            beginView3dOrbit(
                local_position, global_position,
                event->key() == look_key ? View3dOrbitInput::LookModifier
                                         : View3dOrbitInput::OrbitModifier);
            this->pan_velocity = QPointF();
            this->mouse_pan_inertia_active = false;
            stopPanAnimationIfIdle();
            event->accept();
            return true;
        }
    }
#endif

    if (event->key() == Qt::Key_Shift)
    {
        this->pan_fast_modifier_pressed = true;
        if (this->hasKeyboardPanInput())
            this->ensurePanAnimationRunning();
        if (hasKeyboardZoomInput())
            synchronizeKeyboardZoomHold();

        event->accept();
        return true;
    }

    if (pressKeyboardPanKey(event))
    {
        this->pan_fast_modifier_pressed = event->modifiers().testFlag(Qt::ShiftModifier);
        this->mouse_pan_inertia_active = false;
        this->ensurePanAnimationRunning();
        event->accept();
        return true;
    }

    const bool zoom_in = guiShortcutMatches(event, guiShortcutRegistry().shortcut(GuiShortcutId::MapZoomIn), Qt::ShiftModifier);
    const bool zoom_out = guiShortcutMatches(event, guiShortcutRegistry().shortcut(GuiShortcutId::MapZoomOut), Qt::ShiftModifier);
    if (zoom_in || zoom_out)
    {
        // All keyboard zoom uses the one continuous camera motion, with every
        // renderer and in every view mode. The key state is level-triggered:
        // press/release only switch the motion's hold direction; OS key
        // auto-repeat (isAutoRepeat) never adds zoom.
        this->pan_fast_modifier_pressed = event->modifiers().testFlag(Qt::ShiftModifier);
        bool &pressed = zoom_in ? this->zoom_in_key_pressed : this->zoom_out_key_pressed;
        if (zoom_in)
            this->zoom_in_key_identity = navigationKeyIdentity(event);
        else
            this->zoom_out_key_identity = navigationKeyIdentity(event);
        if (!event->isAutoRepeat() || !pressed)
        {
            pressed = true;
            synchronizeKeyboardZoomHold();
        }
        event->accept();
        return true;
    }

    const Qt::KeyboardModifiers blocked_modifiers = Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    if (event->modifiers() & blocked_modifiers)
        return false;

    return false;
}

bool MapWidget::handleKeyReleaseEvent(QKeyEvent *event)
{
    if (!event)
        return false;

    if (this->view_3d_orbit_active
        && ((event->key() == guiShortcutRegistry().holdModifierKey(GuiHoldModifierId::GlobeOrbit)
             && this->view_3d_orbit_input == View3dOrbitInput::OrbitModifier)
            || (event->key() == guiShortcutRegistry().holdModifierKey(GuiHoldModifierId::GlobeLook)
                && this->view_3d_orbit_input == View3dOrbitInput::LookModifier)))
    {
        if (!event->isAutoRepeat())
            endView3dOrbit();
        event->accept();
        return true;
    }

    if (event->key() == Qt::Key_Shift)
    {
        if (!event->isAutoRepeat())
            this->pan_fast_modifier_pressed = false;

        if (this->hasKeyboardPanInput())
            this->ensurePanAnimationRunning();
        if (hasKeyboardZoomInput())
            synchronizeKeyboardZoomHold();

        event->accept();
        return true;
    }

    // Every held pan / zoom key whose physical key this is (the press's
    // modifiers and layout level do not have to hold any more). An
    // auto-repeat release keeps it held.
    struct HeldKey
    {
        bool *pressed;
        const NavigationKeyIdentity *identity;
        bool zoom;
    };
    const HeldKey held_keys[] = {
        {&this->pan_key_left_pressed, &this->pan_key_left_identity, false},
        {&this->pan_key_right_pressed, &this->pan_key_right_identity, false},
        {&this->pan_key_up_pressed, &this->pan_key_up_identity, false},
        {&this->pan_key_down_pressed, &this->pan_key_down_identity, false},
        {&this->zoom_in_key_pressed, &this->zoom_in_key_identity, true},
        {&this->zoom_out_key_pressed, &this->zoom_out_key_identity, true},
    };
    const NavigationKeyIdentity released = navigationKeyIdentity(event);
    bool pan_released = false;
    bool zoom_released = false;
    for (const HeldKey &held_key : held_keys)
    {
        if (!*held_key.pressed || !sameNavigationKey(*held_key.identity, released))
            continue;
        if (held_key.zoom)
            zoom_released = true;
        else
            pan_released = true;
        if (!event->isAutoRepeat())
            *held_key.pressed = false;
    }
    if (!pan_released && !zoom_released)
        return false;

    if (!event->isAutoRepeat())
    {
        // Pan decelerates in the animation; a zoom release ends the hold
        // (the motion decelerates over its spring lag and completes a short
        // press to one tap step).
        if (pan_released)
        {
            ensurePanAnimationRunning();
            updateKeyboardPanIndicator();
        }
        if (zoom_released)
        {
            synchronizeKeyboardZoomHold();
            stopPanAnimationIfIdle();
        }
    }
    event->accept();
    return true;
}

void MapWidget::cancelNavigationInput()
{
    // A button/key release can be sent to the newly active window instead of
    // this map. Never carry a drag, orbit/pointer lock, joystick or held key
    // across focus loss. Do not warp the cursor into an inactive window.
    if (this->view_3d_orbit_active)
        endView3dOrbit(false);
    stopAllPanMovement();
}

void MapWidget::clearKeyboardPanInput()
{
    if (this->view_3d_orbit_active
        && (this->view_3d_orbit_input == View3dOrbitInput::OrbitModifier
            || this->view_3d_orbit_input == View3dOrbitInput::LookModifier))
    {
        endView3dOrbit();
    }
    this->pan_key_left_pressed = false;
    this->pan_key_right_pressed = false;
    this->pan_key_up_pressed = false;
    this->pan_key_down_pressed = false;
    this->pan_fast_modifier_pressed = false;
    updateKeyboardPanIndicator();
    const bool zoom_key_was_held = hasKeyboardZoomInput();
    this->zoom_in_key_pressed = false;
    this->zoom_out_key_pressed = false;
    // Focus loss is a release, not an emergency stop: decelerate smoothly.
    if (zoom_key_was_held)
        synchronizeKeyboardZoomHold();
    stopPanAnimationIfIdle();
}

void MapWidget::setEdgePanningEnabled(bool enabled)
{
    if (this->edge_panning_enabled == enabled)
        return;

    this->edge_panning_enabled = enabled;

    if (enabled)
    {
        if (this->isVisible())
        {
            this->edge_pan_poll_timer->start();
            this->pollEdgePan();
        }
    }
    else
    {
        this->edge_pan_poll_timer->stop();
        this->stopPanAnimationIfIdle();
    }
}

void MapWidget::keyPressEvent(QKeyEvent *event)
{
    if (this->handleKeyPressEvent(event))
        return;

    QWidget::keyPressEvent(event);
}

void MapWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (this->handleKeyReleaseEvent(event))
        return;

    QWidget::keyReleaseEvent(event);
}

void MapWidget::focusOutEvent(QFocusEvent *event)
{
    cancelNavigationInput();
    QWidget::focusOutEvent(event);
}

void MapWidget::hideEvent(QHideEvent *event)
{
    if (this->view_3d_orbit_active)
        endView3dOrbit(false);
    this->stopAllPanMovement();
    this->edge_pan_poll_timer->stop();
    QWidget::hideEvent(event);
}

void MapWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    if (this->edge_panning_enabled)
    {
        this->edge_pan_poll_timer->start();
        this->pollEdgePan();
    }
}

void MapWidget::panMapByPixels(const QPoint &delta, bool keyboard_pan)
{
    if (delta.isNull())
        return;

    if (this->m_model->viewMode() != MapViewMode::Globe)
    {
        this->m_model->panByPixels(delta, size());
        return;
    }
    if (keyboard_pan)
    {
        this->m_model->panByPixelsGlobeKeyboard(delta, size());
        return;
    }
    // The ground under the crosshair follows the pointer: the globe turns on
    // the sphere through the drawn surface there (terrain, else the
    // ellipsoid the model finds itself).
    double crosshair_radius_m = 0.0;
    GeoWgs84Ellipsoid::EcefPositionD crosshair_point;
    if (this->render_surface_globe_surface_point_resolver
        && this->render_surface_globe_surface_point_resolver(
            QPointF(width() * 0.5, height() * 0.5), &crosshair_point))
    {
        crosshair_radius_m = std::sqrt(crosshair_point.x * crosshair_point.x
                                       + crosshair_point.y * crosshair_point.y
                                       + crosshair_point.z * crosshair_point.z);
    }
    this->m_model->panByPixelsGlobe(delta, size(), crosshair_radius_m);
}

void MapWidget::wheelEvent(QWheelEvent *event)
{
    this->handleWheelEvent(event);
}

void MapWidget::handleWheelEvent(QWheelEvent *event)
{
    const QPoint angle_delta = event->angleDelta();
    const QPoint pixel_delta = event->pixelDelta();
    // Precise trackpads may report pixels without angle deltas.
    const double wheel_steps = angle_delta.y() != 0
        ? double(angle_delta.y()) / 120.0
        : double(pixel_delta.y()) / 240.0;
    if (!std::isfinite(wheel_steps) || std::abs(wheel_steps) <= 1e-9)
    {
        event->accept();
        return;
    }

    const GuiMapNavigationConfiguration &navigation = guiConfiguration().map_navigation;
    const double requested_steps = wheel_steps * navigation.scroll_zoom_sensitivity;
    // Same motion for every renderer: the old immediate CPU-fallback paths
    // jumped by a whole notch per event.
    // A joystick drag holds the pointer on the HUD joystick: zoom about the
    // crosshair then, not about that corner.
    if (this->m_model->viewMode() == MapViewMode::Globe)
        queueZoomImpulse(requested_steps * GlobeOctavesPerWheelStep, viewportCenter());
    else if (joystickPanActive())
        queueZoomImpulse(requested_steps, viewportCenter());
    else
        queueZoomImpulse(requested_steps, event->position());
    event->accept();
}

void MapWidget::beginView3dOrbit(
    const QPoint &position, const QPoint &global_position, View3dOrbitInput input)
{
    if (this->view_3d_orbit_active || input == View3dOrbitInput::None)
        return;

#ifndef Q_OS_WASM
    // A native Wayland pointer lock is attached to the top-level wl_surface,
    // not to this QWidget. Once locked, Qt is therefore no longer guaranteed
    // to route the matching mouse-button release back through MapWidget. Watch
    // application input while orbit is active so the gesture that started the
    // lock also always terminates it. This is especially important because we
    // intentionally do not call grabMouse() when the Wayland lock succeeds; a
    // second Qt pointer grab can conflict with the compositor pointer lock.
    if (qApp != nullptr)
        qApp->installEventFilter(this);
#endif

#ifdef Q_OS_WASM
    Q_UNUSED(global_position);
#endif

    this->view_3d_orbit_active = true;
    this->view_3d_orbit_input = input;
    if (input != View3dOrbitInput::HudJoystick)
        this->m_model->beginView3dRotateInteraction();
    // The gesture picks the globe camera mode (after the crosshair anchor
    // above): an orbit keeps its 0..90 degree tilt lock (a free-look view
    // above the horizon tilts down to it), the look key turns freely from
    // straight down to straight up. The HUD tilt slider follows the mode.
    if (input == View3dOrbitInput::OrbitModifier || input == View3dOrbitInput::MiddleMouse)
        this->m_model->setViewGlobeCameraMode(MapGlobeCameraMode::Orbit);
    else if (input == View3dOrbitInput::LookModifier)
        this->m_model->setViewGlobeCameraMode(MapGlobeCameraMode::FreeLook);
    // HUD hint (MapBgfxHud::NavigationGesture values).
    if (input == View3dOrbitInput::OrbitModifier)
        emit navigationGestureChanged(1);
    else if (input == View3dOrbitInput::MiddleMouse)
        emit navigationGestureChanged(2);
    else if (input == View3dOrbitInput::LookModifier)
        emit navigationGestureChanged(3);
#ifdef Q_OS_WASM
    this->view_3d_orbit_last_position = position;
#else
    this->view_3d_orbit_restore_global = global_position;
    this->view_3d_orbit_anchor_global = global_position;
    this->view_3d_orbit_last_global_position = global_position;
    this->view_3d_orbit_relative_fractional_delta = QPointF();

    const bool wayland_platform = this->view_3d_orbit_wayland_pointer_lock.isWaylandPlatform();
    bool wayland_pointer_locked = false;
    if (wayland_platform)
    {
        // The top-level window's surface, as the QRhi map always did, plus
        // the embedded native map window (bgfx, a wl_subsurface that has
        // the pointer focus) when there is one: compositors differ in which
        // of the two constraints they honour, see MapWaylandPointerLock.
        QVector<MapWaylandPointerLock::Target> lock_targets;
        QWidget *top_level_widget = window();
        if (top_level_widget != nullptr && top_level_widget->windowHandle() != nullptr)
        {
            MapWaylandPointerLock::Target top_level;
            top_level.window = top_level_widget->windowHandle();
            top_level.restore_surface_position = QPointF(mapTo(top_level_widget, position));
            lock_targets.append(top_level);
        }
        if (!this->orbit_pointer_lock_window.isNull())
        {
            // Its local coordinates are this widget's (same stack geometry).
            MapWaylandPointerLock::Target embedded;
            embedded.window = this->orbit_pointer_lock_window.data();
            embedded.restore_surface_position = QPointF(position);
            lock_targets.append(embedded);
        }

        wayland_pointer_locked = this->view_3d_orbit_wayland_pointer_lock.lock(
            lock_targets,
            [this](const QPointF &delta)
            {
                if (!this->view_3d_orbit_active
                    || !this->view_3d_orbit_wayland_pointer_lock.isLocked())
                {
                    return;
                }

                this->view_3d_orbit_relative_fractional_delta += delta;
                const int delta_x = int(std::trunc(
                    this->view_3d_orbit_relative_fractional_delta.x()));
                const int delta_y = int(std::trunc(
                    this->view_3d_orbit_relative_fractional_delta.y()));
                const QPoint whole_delta(delta_x, delta_y);
                if (whole_delta.isNull())
                    return;

                this->view_3d_orbit_relative_fractional_delta -= QPointF(whole_delta);
                applyView3dOrbitPointerDelta(whole_delta);
            });
    }

    this->view_3d_orbit_pointer_warp_enabled = !wayland_platform;

    // X11/Windows/macOS can keep the hidden cursor pinned to the orbit start.
    // Wayland deliberately forbids client-side cursor warping, so use the
    // pointer-constraints + relative-pointer protocols there. That gives us
    // unbounded relative motion even when the physical pointer would otherwise
    // reach a monitor edge.
    QApplication::setOverrideCursor(Qt::BlankCursor);
    this->view_3d_orbit_cursor_hidden = true;
    this->view_3d_orbit_mouse_grabbed = false;
    if (!wayland_pointer_locked)
    {
        grabMouse();
        this->view_3d_orbit_mouse_grabbed = true;
    }
#endif
}

void MapWidget::endView3dOrbit(bool restore_cursor_position)
{
    if (!this->view_3d_orbit_active
#ifndef Q_OS_WASM
        && !this->view_3d_orbit_mouse_grabbed
        && !this->view_3d_orbit_wayland_pointer_lock.isLocked()
#endif
    )
    {
        return;
    }

    const bool joystick = this->view_3d_orbit_input == View3dOrbitInput::HudJoystick;
    this->view_3d_orbit_active = false;
    this->view_3d_orbit_input = View3dOrbitInput::None;
    if (joystick)
    {
        // The knob springs back; the pan glides out (advanceNavigation()).
        this->joystick_offset_px = QPointF();
        emit joystickChanged(QPointF(), false);
    }
    else
    {
        this->m_model->endView3dRotateInteraction();
        emit navigationGestureChanged(0);
    }
#ifndef Q_OS_WASM
    if (qApp != nullptr)
        qApp->removeEventFilter(this);

    if (this->view_3d_orbit_wayland_pointer_lock.isLocked())
        this->view_3d_orbit_wayland_pointer_lock.unlock(restore_cursor_position);

    if (this->view_3d_orbit_mouse_grabbed)
    {
        releaseMouse();
        this->view_3d_orbit_mouse_grabbed = false;
    }
    if (this->view_3d_orbit_cursor_hidden)
    {
        QApplication::restoreOverrideCursor();
        this->view_3d_orbit_cursor_hidden = false;
    }
    if (restore_cursor_position && this->view_3d_orbit_pointer_warp_enabled)
        QCursor::setPos(this->view_3d_orbit_restore_global);
    this->view_3d_orbit_pointer_warp_enabled = false;
    this->view_3d_orbit_relative_fractional_delta = QPointF();
#endif
}

void MapWidget::applyView3dOrbitPointerDelta(const QPoint &delta)
{
    if (delta.isNull())
        return;

    if (this->view_3d_orbit_input == View3dOrbitInput::HudJoystick)
    {
        // The virtual knob follows the captured motion, held at its reach.
        this->joystick_offset_px += QPointF(delta);
        const double length = std::hypot(this->joystick_offset_px.x(), this->joystick_offset_px.y());
        if (length > JoystickTravelPx)
            this->joystick_offset_px *= JoystickTravelPx / length;
        emit joystickChanged(joystickDeflection(), true);
        return;
    }

    if (this->m_model->viewMode() != MapViewMode::Globe)
        return;
    if (this->view_3d_orbit_input == View3dOrbitInput::LookModifier)
        this->m_model->lookViewGlobeByPointerDelta(delta);
    else
        this->m_model->orbitViewGlobeByPointerDelta(delta, true);
}

bool MapWidget::joystickPanActive() const
{
    return this->view_3d_orbit_active && this->view_3d_orbit_input == View3dOrbitInput::HudJoystick;
}

QPointF MapWidget::joystickDeflection() const
{
    return this->joystick_offset_px / JoystickTravelPx;
}

void MapWidget::beginJoystickPan(const QPointF &position)
{
    // The press may already be over (a quick click: the HUD's command is
    // queued); then there is nothing to hold.
    if (this->view_3d_orbit_active || !QGuiApplication::mouseButtons().testFlag(Qt::LeftButton))
        return;
    this->mouse_pan_active = false;
    this->mouse_pan_inertia_active = false;
    this->keyboard_pan_motion_active = false;
    this->joystick_offset_px = QPointF();
    const QPoint local = position.toPoint();
    beginView3dOrbit(local, mapToGlobal(local), View3dOrbitInput::HudJoystick);
    emit joystickChanged(QPointF(), true);
    ensurePanAnimationRunning();
}

bool MapWidget::eventFilter(QObject *watched, QEvent *event)
{
#ifndef Q_OS_WASM
    if (this->view_3d_orbit_active && event != nullptr)
    {
        if (this->view_3d_orbit_input == View3dOrbitInput::MiddleMouse
            && event->type() == QEvent::MouseButtonRelease)
        {
            QMouseEvent *mouse_event = static_cast<QMouseEvent *>(event);
            if (mouse_event->button() == Qt::MiddleButton)
                endView3dOrbit();
        }
        else if (this->view_3d_orbit_input == View3dOrbitInput::HudJoystick
                 && event->type() == QEvent::MouseButtonRelease
                 && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton
                 && qobject_cast<QWidget *>(watched) != nullptr)
        {
            // The release as a widget receives it (its window has seen it:
            // Qt's grab bookkeeping stays intact): it ends the drag and goes
            // no further, the HUD and the editor tool never saw its press.
            endView3dOrbit();
            return true;
        }
        else if (this->view_3d_orbit_input == View3dOrbitInput::OrbitModifier
                 && event->type() == QEvent::KeyRelease)
        {
            QKeyEvent *key_event = static_cast<QKeyEvent *>(event);
            if (key_event->key() == guiShortcutRegistry().holdModifierKey(GuiHoldModifierId::GlobeOrbit)
                && !key_event->isAutoRepeat())
                endView3dOrbit();
        }
        else if (this->view_3d_orbit_input == View3dOrbitInput::LookModifier
                 && event->type() == QEvent::KeyRelease)
        {
            QKeyEvent *key_event = static_cast<QKeyEvent *>(event);
            if (key_event->key() == guiShortcutRegistry().holdModifierKey(GuiHoldModifierId::GlobeLook)
                && !key_event->isAutoRepeat())
            {
                endView3dOrbit();
                // Swallow the release application-wide (this filter sits on
                // qApp while the gesture runs): a bare Alt press + release
                // would otherwise select the window's menu bar on Windows
                // (QMenuBar's Alt navigation acts on the release) and take
                // keyboard focus away from the map after every free look.
                return true;
            }
        }
    }
#else
    Q_UNUSED(watched)
    Q_UNUSED(event)
#endif

    return QWidget::eventFilter(watched, event);
}

void MapWidget::mousePressEvent(QMouseEvent *event)
{
    if (this->handleMousePressEvent(event))
        return;

    QWidget::mousePressEvent(event);
}

bool MapWidget::handleMousePressEvent(QMouseEvent *event)
{
    if (!event)
        return false;

    if (this->m_model->viewMode() == MapViewMode::Globe
        && event->button() == Qt::MiddleButton)
    {
        beginView3dOrbit(
            event->position().toPoint(),
            event->globalPosition().toPoint(),
            View3dOrbitInput::MiddleMouse);
        this->pan_velocity = QPointF();
        this->mouse_pan_inertia_active = false;
        stopPanAnimationIfIdle();
        event->accept();
        return true;
    }

    if (event->button() != Qt::LeftButton)
        return false;

    this->mouse_pan_active = true;
    this->mouse_pan_last_position = event->position().toPoint();
    this->mouse_pan_sensitivity_fractional_delta = QPointF();
    this->pan_velocity = QPointF();
    this->pan_fractional_delta = QPointF();
    this->mouse_pan_velocity = QPointF();
    this->mouse_pan_move_elapsed_timer.start();
    this->mouse_pan_inertia_active = false;
    this->mouse_pan_drag_distance = 0;
    stopPanAnimationIfIdle();
    event->accept();
    return true;
}

void MapWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (this->handleMouseReleaseEvent(event))
        return;

    QWidget::mouseReleaseEvent(event);
}

bool MapWidget::handleMouseReleaseEvent(QMouseEvent *event)
{
    if (!event)
        return false;

    if (event->button() == Qt::MiddleButton && this->view_3d_orbit_active
        && this->view_3d_orbit_input != View3dOrbitInput::HudJoystick)
    {
        endView3dOrbit();
        event->accept();
        return true;
    }
    if (event->button() == Qt::LeftButton && joystickPanActive())
    {
        endView3dOrbit();
        event->accept();
        return true;
    }

    if (event->button() != Qt::LeftButton || !this->mouse_pan_active)
        return false;

    this->mouse_pan_active = false;
    this->mouse_pan_sensitivity_fractional_delta = QPointF();

    const bool movement_is_recent = this->mouse_pan_move_elapsed_timer.isValid() &&
        this->mouse_pan_move_elapsed_timer.elapsed() <= MousePanReleaseTimeoutMs;
    const qreal release_speed = vectorLength(this->mouse_pan_velocity);
    const bool dragged_far_enough = this->mouse_pan_drag_distance >= QApplication::startDragDistance();

    if (dragged_far_enough && movement_is_recent && release_speed >= MousePanMinimumInertiaSpeedPixelsPerSecond)
    {
        this->pan_velocity = this->mouse_pan_velocity;
        this->pan_fractional_delta = QPointF();
        this->mouse_pan_inertia_active = true;
        ensurePanAnimationRunning();
    }
    else
    {
        this->pan_velocity = QPointF();
        this->mouse_pan_inertia_active = false;
    }

    this->mouse_pan_velocity = QPointF();
    this->mouse_pan_drag_distance = 0;
    stopPanAnimationIfIdle();
    event->accept();
    return true;
}

void MapWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (this->handleMouseMoveEvent(event))
        return;

    QWidget::mouseMoveEvent(event);
}

bool MapWidget::handleMouseMoveEvent(QMouseEvent *event)
{
    if (!event)
        return false;

    const QPoint position = event->position().toPoint();
    this->updatePointerCoordinates(position);

    // The configured hold keys (settings: Globe Navigation (hold)).
    const Qt::KeyboardModifier orbit_modifier =
        guiShortcutRegistry().holdModifier(GuiHoldModifierId::GlobeOrbit);
    const Qt::KeyboardModifier look_modifier =
        guiShortcutRegistry().holdModifier(GuiHoldModifierId::GlobeLook);
    const bool orbit_modifier_requested =
        this->m_model->viewMode() == MapViewMode::Globe
        && orbit_modifier != Qt::NoModifier
        && event->modifiers().testFlag(orbit_modifier)
        && event->buttons() == Qt::NoButton;
    // Orbit wins when both keys are held: orbit, not look.
    const bool look_modifier_requested =
        this->m_model->viewMode() == MapViewMode::Globe
        && look_modifier != Qt::NoModifier
        && event->modifiers().testFlag(look_modifier)
        && !orbit_modifier_requested
        && event->buttons() == Qt::NoButton;

    if (!this->view_3d_orbit_active && (orbit_modifier_requested || look_modifier_requested))
    {
        beginView3dOrbit(
            event->position().toPoint(),
            event->globalPosition().toPoint(),
            orbit_modifier_requested ? View3dOrbitInput::OrbitModifier
                                       : View3dOrbitInput::LookModifier);
        this->pan_velocity = QPointF();
        this->mouse_pan_inertia_active = false;
        stopPanAnimationIfIdle();
        event->accept();
        return true;
    }

    if (this->view_3d_orbit_active)
    {
        bool orbit_input_still_active = false;
        if (this->view_3d_orbit_input == View3dOrbitInput::MiddleMouse)
            orbit_input_still_active = event->buttons().testFlag(Qt::MiddleButton);
        else if (this->view_3d_orbit_input == View3dOrbitInput::OrbitModifier)
            orbit_input_still_active = orbit_modifier_requested;
        else if (this->view_3d_orbit_input == View3dOrbitInput::LookModifier)
            orbit_input_still_active = look_modifier_requested;
        else if (this->view_3d_orbit_input == View3dOrbitInput::HudJoystick)
            orbit_input_still_active = event->buttons().testFlag(Qt::LeftButton);

        if (!orbit_input_still_active)
        {
            endView3dOrbit();
            return false;
        }

#ifndef Q_OS_WASM
        if (this->view_3d_orbit_wayland_pointer_lock.isLocked())
        {
            // Relative motion is delivered directly by the Wayland protocol.
            // Absolute QMouseEvent positions intentionally stay fixed while the
            // compositor lock is active, so there is nothing to consume here.
            event->accept();
            return true;
        }

        const QPoint global_position = event->globalPosition().toPoint();
        QPoint delta;

        if (this->view_3d_orbit_pointer_warp_enabled)
        {
            // Treat the activation point as an infinite relative-mouse anchor.
            // The synthetic event generated by setPos() lands exactly on the
            // anchor and therefore has a zero delta and is ignored naturally.
            delta = global_position - this->view_3d_orbit_anchor_global;
        }
        else
        {
            // Fallback for a Wayland compositor/build without relative-pointer
            // support. This remains finite at the monitor edge, but preserves
            // the previous behavior rather than disabling orbit entirely.
            delta = global_position - this->view_3d_orbit_last_global_position;
            this->view_3d_orbit_last_global_position = global_position;
        }

        applyView3dOrbitPointerDelta(delta);
        if (!delta.isNull() && this->view_3d_orbit_pointer_warp_enabled)
            QCursor::setPos(this->view_3d_orbit_anchor_global);
#else
        const QPoint delta = position - this->view_3d_orbit_last_position;
        this->view_3d_orbit_last_position = position;
        applyView3dOrbitPointerDelta(delta);
#endif
        event->accept();
        return true;
    }

#ifdef Q_OS_WASM
    // Qt for WebAssembly receives browser pointer events. A pointerdown event is
    // only generated for the first pressed mouse button, so pressing left while
    // right is already held does not reach handleMousePressEvent(). Recover the
    // missing transition from the buttons state carried by pointermove.
    if (!this->mouse_pan_active && (event->buttons() & Qt::LeftButton))
    {
        this->mouse_pan_active = true;
        this->mouse_pan_last_position = position;
        this->mouse_pan_sensitivity_fractional_delta = QPointF();
        this->pan_velocity = QPointF();
        this->pan_fractional_delta = QPointF();
        this->mouse_pan_velocity = QPointF();
        this->mouse_pan_move_elapsed_timer.start();
        this->mouse_pan_inertia_active = false;
        this->mouse_pan_drag_distance = 0;
        stopPanAnimationIfIdle();
        event->accept();
        return true;
    }
#endif

    if (!this->mouse_pan_active)
        return false;

    if (!(event->buttons() & Qt::LeftButton))
    {
        this->mouse_pan_active = false;
        this->mouse_pan_sensitivity_fractional_delta = QPointF();
        this->mouse_pan_velocity = QPointF();
        this->mouse_pan_inertia_active = false;
        this->mouse_pan_drag_distance = 0;
        return false;
    }

    const QPoint delta = position - this->mouse_pan_last_position;
    this->mouse_pan_last_position = position;

    const bool view_3d_mouse_pan =
        this->m_model->viewMode() == MapViewMode::Globe;
    const double mouse_pan_sensitivity = view_3d_mouse_pan
        ? guiConfiguration().map_navigation.mouse_3d_pan_sensitivity
        : 1.0;

    QPoint pan_delta = delta;
    if (view_3d_mouse_pan && !delta.isNull())
    {
        const QPointF precise_pan_delta =
            QPointF(delta) * mouse_pan_sensitivity
            + this->mouse_pan_sensitivity_fractional_delta;
        pan_delta = QPoint(qRound(precise_pan_delta.x()), qRound(precise_pan_delta.y()));
        this->mouse_pan_sensitivity_fractional_delta =
            precise_pan_delta - QPointF(pan_delta);
    }
    else if (!view_3d_mouse_pan)
    {
        this->mouse_pan_sensitivity_fractional_delta = QPointF();
    }

    const qint64 elapsed_ms = this->mouse_pan_move_elapsed_timer.restart();
    this->mouse_pan_drag_distance += delta.manhattanLength();

    if (!delta.isNull() && elapsed_ms > 0)
    {
        const qreal elapsed_seconds = qBound<qreal>(0.001, elapsed_ms / 1000.0, 0.1);
        QPointF measured_velocity = QPointF(delta) / elapsed_seconds;
        const qreal measured_speed = vectorLength(measured_velocity);
        if (measured_speed > MousePanMaximumSpeedPixelsPerSecond)
            measured_velocity = normalized(measured_velocity) * MousePanMaximumSpeedPixelsPerSecond;
        measured_velocity *= mouse_pan_sensitivity;

        if (this->mouse_pan_velocity.isNull())
        {
            this->mouse_pan_velocity = measured_velocity;
        }
        else
        {
            this->mouse_pan_velocity =
                this->mouse_pan_velocity * (1.0 - MousePanVelocitySmoothing) +
                measured_velocity * MousePanVelocitySmoothing;
        }
    }

    if (!pan_delta.isNull())
    {
        this->keyboard_pan_motion_active = false;
        panMapByPixels(pan_delta);
    }

    event->accept();
    return true;
}

void MapWidget::updatePointerCoordinates(const QPoint &position)
{
    if (this->m_model->viewMode() == MapViewMode::Globe)
    {
        CoordinateWGS84 globe_coordinate;
        bool coordinate_available = false;
        if (this->render_surface_screen_coordinate_resolver)
        {
            coordinate_available = this->render_surface_screen_coordinate_resolver(
                QPointF(position), &globe_coordinate);
        }
        else
        {
            coordinate_available = this->m_model->globeCoordinateAtScreen(
                position, this->size(), &globe_coordinate);
        }

        if (!coordinate_available)
        {
            emit signalCoordsUnavailable();
            return;
        }

        emitPointerCoordinate(globe_coordinate);
        return;
    }

    const CoordinateWGS84 wgs = this->m_model->wgs84FromScreen(position, this->size());
    emitPointerCoordinate(withCachedTerrainAltitude(wgs));
}

CoordinateWGS84 MapWidget::withCachedTerrainAltitude(const CoordinateWGS84 &wgs) const
{
    return mapWithCachedTerrainAltitude(wgs, this->cached_terrain_elevation_resolver);
}

void MapWidget::emitPointerCoordinate(const CoordinateWGS84 &wgs)
{
    emit signalCoordsChangedWgs84(wgs);

    GeoMetricProjection projection;
    const CoordinateUTM utm = projection.wgs84ToUtm(wgs);
    emit signalCoordsChangedUTM(utm);
}

void MapWidget::setRenderSurfaceScreenCoordinateResolver(ScreenCoordinateResolver resolver)
{
    this->render_surface_screen_coordinate_resolver = std::move(resolver);
}

void MapWidget::setRenderSurfaceGlobeSurfacePointResolver(GlobeSurfacePointResolver resolver)
{
    this->render_surface_globe_surface_point_resolver = std::move(resolver);
}

void MapWidget::setCachedTerrainElevationResolver(CachedTerrainElevationResolver resolver)
{
    this->cached_terrain_elevation_resolver = std::move(resolver);
}

// Zoom buttons/menu: one smooth octave about the crosshair.
void MapWidget::zoomIn()
{
    queueZoomImpulse(1.0, viewportCenter());
}

void MapWidget::zoomOut()
{
    queueZoomImpulse(-1.0, viewportCenter());
}

void MapWidget::changeMapProvider(MapProvider provider)
{
    this->m_model->setProvider(provider);
}

