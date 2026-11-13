#include "config/live_ui_text.h"
#include <aowis/model/units/quantity.h>
#include "tabs/tab_map_monitor_container.h"
#include "tabs/map_workspace_mode.h"
#include "tabs/map_mode_tool_panel.h"

#include "network/hydraulic_data.h"
#include "network/network_symbology_values.h"
#include "network/infrastructure_entity_traits.h"

#include "config/gui_configuration.h"
#include "config/unit_aware_view.h"
#include "config/unit_profile_manager.h"
#include "config/unit_table_presentation.h"
#include "config/shortcut_registry.h"
#include "map/data/map_terrain_repository.h"
#include "map/render/map_network_symbology_resolver.h"
#include "map/render/map_render_types.h"
#include "map/bgfx/map_bgfx_globe_hud_controller.h"
#include "map/bgfx/map_bgfx_editor_icons.h"
#include "map/bgfx/map_bgfx_editor_overlay.h"
#include "map/bgfx/map_bgfx_legend_content.h"
#include "map/bgfx/map_bgfx_surface.h"

#include <QAbstractButton>
#include <QColor>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPalette>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string_view>

#ifndef Q_OS_WASM
#include <QGeoCoordinate>
#include <QGeoPositionInfo>
#endif

namespace
{
class SymbologySlider final : public QSlider
{
public:
    SymbologySlider(int value_minimum, int value_maximum, int value_default, const QString &description, const QString &unit_suffix, QWidget *parent = nullptr, double display_scale = 1.0, int display_decimals = 0)
        : QSlider(Qt::Horizontal, parent),
          value_default(value_default),
          description(description),
          unit_suffix(unit_suffix)
    {
        this->display_scale = display_scale;
        this->display_decimals = display_decimals;
        setRange(value_minimum, value_maximum);
        setValue(this->value_default);
        connect(this, &QSlider::valueChanged, this, [this]
        {
            updateToolTip();
        });
        updateToolTip();
    }

    void setConfiguration(int value_minimum, int value_maximum, int value_default,
                          int value, const QString &description, const QString &unit_suffix,
                          double display_scale = 1.0, int display_decimals = 0)
    {
        const QSignalBlocker blocker(this);
        this->value_default = value_default;
        this->description = description;
        this->unit_suffix = unit_suffix;
        this->display_scale = display_scale;
        this->display_decimals = display_decimals;
        setRange(value_minimum, value_maximum);
        setValue(value);
        updateToolTip();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QSlider::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            updateToolTip();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton)
        {
            setValue(this->value_default);
            event->accept();
            return;
        }

        QSlider::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        setValue(this->value_default);
        event->accept();
    }

private:
    QString displayValue(int raw_value) const
    {
        return QString::number(raw_value * this->display_scale, 'f', this->display_decimals);
    }

    void updateToolTip()
    {
        setToolTip(QCoreApplication::translate("MapMonitorMenuWidget",
            "%1\nCurrent: %2%3\nRange: %4%3 to %5%3\nDefault: %6%3\nRight-click to reset.")
                       .arg(QCoreApplication::translate("MapMonitorMenuWidget", this->description.toUtf8().constData()))
                       .arg(displayValue(value()))
                       .arg(this->unit_suffix)
                       .arg(displayValue(minimum()))
                       .arg(displayValue(maximum()))
                       .arg(displayValue(this->value_default)));
    }

    int value_default;
    QString description;
    QString unit_suffix;
    double display_scale = 1.0;
    int display_decimals = 0;
};


class JunctionSizeSlider final : public QSlider
{
public:
    JunctionSizeSlider(int active_minimum, int active_maximum, int value_default,
                       const QString &description, const QString &unit_suffix,
                       QWidget *parent = nullptr, double display_scale = 1.0,
                       int display_decimals = 0)
        : QSlider(Qt::Horizontal, parent)
    {
        setConfiguration(active_minimum, active_maximum, value_default, value_default,
                         description, unit_suffix, display_scale, display_decimals);
        connect(this, &QSlider::valueChanged, this, [this]
        {
            if (value() > 0 && value() <= this->off_detent_steps)
            {
                setValue(0);
                return;
            }

            updateToolTip();
        });
    }

    void setConfiguration(int active_minimum, int active_maximum, int value_default,
                          int value, const QString &description, const QString &unit_suffix,
                          double display_scale = 1.0, int display_decimals = 0)
    {
        const QSignalBlocker blocker(this);
        this->active_minimum = active_minimum;
        this->active_maximum = active_maximum;
        this->value_default = qBound(active_minimum, value_default, active_maximum);
        this->description = description;
        this->unit_suffix = unit_suffix;
        this->display_scale = display_scale;
        this->display_decimals = display_decimals;
        const int active_position_count = active_maximum - active_minimum + 1;
        this->off_detent_steps = qMax(1, qRound(active_position_count / 19.0));
        setRange(0, this->off_detent_steps + active_position_count);
        setSingleStep(1);
        setPageStep(1);
        setValue(positionForSize(value));
        updateToolTip();
    }

    int junctionSizeRaw() const
    {
        if (value() <= this->off_detent_steps)
            return 0;
        return this->active_minimum + value() - this->off_detent_steps - 1;
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QSlider::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            updateToolTip();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton)
        {
            setValue(positionForSize(this->value_default));
            event->accept();
            return;
        }

        QSlider::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        setValue(positionForSize(this->value_default));
        event->accept();
    }

private:
    int positionForSize(int raw_size) const
    {
        if (raw_size <= 0)
            return 0;
        return qBound(this->off_detent_steps + 1,
                      this->off_detent_steps + raw_size - this->active_minimum + 1,
                      maximum());
    }

    QString displayValue(int raw_value) const
    {
        return QString::number(raw_value * this->display_scale, 'f', this->display_decimals);
    }

    void updateToolTip()
    {
        const int raw_size = junctionSizeRaw();
        const QString current = raw_size == 0
            ? QCoreApplication::translate("MapMonitorMenuWidget", "Off")
            : QStringLiteral("%1%2").arg(displayValue(raw_size), this->unit_suffix);
        setToolTip(QCoreApplication::translate("MapMonitorMenuWidget",
            "%1\nThe far-left detent is Off; crossing it snaps between Off and the minimum useful size.\n"
            "Current: %2\nActive range: %3%5 to %4%5\nDefault: %6%5\nRight-click to reset.")
            .arg(QCoreApplication::translate("MapMonitorMenuWidget", this->description.toUtf8().constData()))
            .arg(current)
            .arg(displayValue(this->active_minimum))
            .arg(displayValue(this->active_maximum))
            .arg(this->unit_suffix)
            .arg(displayValue(this->value_default)));
    }

    int active_minimum = 1;
    int active_maximum = 1;
    int value_default = 1;
    int off_detent_steps = 1;
    QString description;
    QString unit_suffix;
    double display_scale = 1.0;
    int display_decimals = 0;
};


class FlowDirectionSizeSlider final : public QSlider
{
public:
    explicit FlowDirectionSizeSlider(QWidget *parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        setRange(0, 19);
        setSingleStep(1);
        setPageStep(1);
        setValue(positionForArrowSize(10));
        connect(this, &QSlider::valueChanged, this, [this]
        {
            updateToolTip();
        });
        updateToolTip();
    }

    int arrowSizePx() const
    {
        if (value() == 0)
            return 0;
        return 5 + value();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QSlider::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            updateToolTip();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton)
        {
            setValue(positionForArrowSize(10));
            event->accept();
            return;
        }

        QSlider::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        setValue(positionForArrowSize(10));
        event->accept();
    }

private:
    static int positionForArrowSize(int size_px)
    {
        if (size_px <= 0)
            return 0;
        return qBound(1, size_px - 5, 19);
    }

    void updateToolTip()
    {
        const int size_px = arrowSizePx();
        const QString current = size_px == 0
            ? QCoreApplication::translate("MapMonitorMenuWidget", "Off")
            : QStringLiteral("%1 px").arg(size_px);
        setToolTip(QCoreApplication::translate("MapMonitorMenuWidget",
            "Controls the flow-direction arrow size for the current simulation timestep.\n"
            "The far-left detent is Off; moving one step right snaps directly to the minimum useful size.\n"
            "Current: %1\nActive range: 6 px to 24 px\nDefault: 10 px\nRight-click to reset.")
            .arg(current));
    }
};
}

MapMonitorContainer::MapMonitorContainer(MapModel *map_model, MapTileRepository *tile_repository,
                                         MapTerrainRepository *terrain_repository,
                                         HydraulicData *hydraulic_data, GpsProvider *gps, QWidget *parent)
    : QWidget{parent},
    layout( new QHBoxLayout(this) ),
    gps( gps ),
    map_model( map_model ),
    tile_repository( tile_repository ),
    terrain_repository( terrain_repository ),
    hydraulic_data( hydraulic_data ),
    map_stack( new QWidget(this) ),
    map_stack_layout( new QStackedLayout(this->map_stack) ),
    map( new MapWidget(this->map_model, this->tile_repository, this->gps, this->map_stack) ),
    renderer_status( new QLabel(this->map_stack) ),
    map_menu( new MapMonitorMenuWidget(this->map, this) )
{
    const GuiSymbologyPaletteConfiguration &palette_configuration =
        guiConfiguration().symbology_palettes;
    this->symbology_settings.node_palette = palette_configuration.node_palette;
    this->symbology_settings.node_palette_flipped = palette_configuration.node_palette_flipped;
    this->symbology_settings.link_palette = palette_configuration.link_palette;
    this->symbology_settings.link_palette_flipped = palette_configuration.link_palette_flipped;
    this->symbology_settings.heatmap_palette = palette_configuration.heatmap_palette;
    this->symbology_settings.heatmap_palette_flipped =
        palette_configuration.heatmap_palette_flipped;

    setContentsMargins(0, 0, 0, 0);
    this->layout->setContentsMargins(0, 0, 0, 0);
    this->layout->setSpacing(0);
    
    this->tool_panel = new MapModeToolPanel(
        this->map_menu, Sizes::SidebarLeftWidth,
        MapModeToolPanel::ScrollbarWidthPolicy::IncludeVisibleScrollbar, false, this);

    this->map_stack_layout->setContentsMargins(0, 0, 0, 0);
    this->map_stack_layout->setSpacing(0);
    this->map_stack_layout->setStackingMode(QStackedLayout::StackAll);
    connect(this->map_menu->mapNavigationWidget(), &MapNavigationWidget::signalSlideOpacityChanged,
        this, &MapMonitorContainer::setNetworkBackgroundOpacity);
    // The opacity slider follows whether there is a network to show:
    // leftmost without one, NetworkOpacityPercent once one is loaded, left
    // again when it is deleted (MapNavigationWidget::setNetworkPresent()).
    const std::function<void()> sync_network_present = [this]()
    {
        const NetworkRenderSnapshot &snapshot = this->hydraulic_data->networkRenderSnapshot();
        this->map_menu->mapNavigationWidget()->setNetworkPresent(
            !snapshot.nodes.isEmpty() || !snapshot.links.isEmpty());
    };
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, sync_network_present);
    connect(this->hydraulic_data, &HydraulicData::signalNetworkGeometryChanged, this,
            [sync_network_present](quint64) { sync_network_present(); });
    sync_network_present();
    // The 3D Monitor overlays the Editor's actual pixmaps; its percent slider
    // must resize that overlay rather than only the underlying GPU symbology.
    connect(this->map_menu->mapNavigationWidget(),
        &MapNavigationWidget::signalIconSizeChanged, this, [this](int percent)
    {
        this->monitor_globe_icon_size_percent = qBound(50, percent, 600);
        if (this->bgfx_surface != nullptr && !this->editor_mode
            && this->map_model->viewMode() == MapViewMode::Globe)
            this->bgfx_surface->requestRedraw();
    });
    connect(this->map_menu->mapNavigationWidget(),
        &MapNavigationWidget::signalMonitorIconSizeUnitChanged, this,
        [this](NetworkSymbologySizeUnit unit)
    {
        this->symbology_settings.icon_size_unit = unit;
        applyVisualControlSymbology();
    });
    connect(this->map_menu->mapNavigationWidget(),
        &MapNavigationWidget::signalMonitorIconSizeChanged, this,
        [this](NetworkSymbologySizeUnit unit, double size)
    {
        this->symbology_settings.icon_size_unit = unit;
        if (unit == NetworkSymbologySizeUnit::Meters)
            this->symbology_settings.icon_size_m = size;
        else
            this->symbology_settings.icon_size_px = qRound(size);
        applyVisualControlSymbology();
    });
    this->map->installEventFilter(this);
    this->map_stack->installEventFilter(this);
    this->map_stack_layout->addWidget(this->map);
    // There is no CPU or JavaScript map renderer any more (desktop or
    // browser): until a GPU surface reports its first frame (or if it
    // fails), this label is shown instead. MapWidget stays as the
    // input/navigation owner but never paints map tiles itself.
    this->renderer_status->setAlignment(Qt::AlignCenter);
    this->renderer_status->setWordWrap(true);
    // Text only, never an opaque cover over the stack: MapWidget below
    // provides the background, and a GPU surface added later must stay
    // visible to present (and report) its first frame.
    this->renderer_status->setAutoFillBackground(false);
    this->renderer_status->setAttribute(Qt::WA_TranslucentBackground);
    this->renderer_status->setAttribute(Qt::WA_TransparentForMouseEvents);
    this->map_stack_layout->addWidget(this->renderer_status);
    showRendererStatus(RendererStatusKind::Starting);
    // bgfx is the only map renderer.
    {
        // Do not promote until bgfx has actually produced a frame; until then
        // the status label is shown. MapWidget keeps map navigation input.
        MapBgfxSurface *surface = new MapBgfxSurface(this->map_model, this->map_stack);
        // The surface owns its labels, not its legend text. Refresh just the
        // presentation strings when a translator changes; preserve network
        // rendering, selections and the editor/monitor mode.
        connect(surface, &MapBgfxSurface::localizedPresentationRefreshRequested,
                this, &MapMonitorContainer::refreshHudLegendText);
        surface->setNavigationHandlers(
            [this](QKeyEvent *event)
            {
                // Map fullscreen hides the left menu, whose buttons'
                // shortcuts (map providers, visuals) then cannot fire.
                if (event->type() == QEvent::KeyPress && this->bgfx_fullscreen_active
                    && MapNavigationWidget::triggerButtonShortcut(this->map_menu, event))
                {
                    event->accept();
                    return true;
                }
                return this->map->handleKeyPressEvent(event);
            },
            [this](QKeyEvent *event) { return this->map->handleKeyReleaseEvent(event); },
            [this]() { this->map->cancelNavigationInput(); });
        surface->setNavigationWheelHandler(
            [this](QWheelEvent *event) { this->map->handleWheelEvent(event); });
        surface->setNavigationFrameHandler(
            [this](double elapsed_seconds) { this->map->advanceNavigationFrame(elapsed_seconds); });
        // Desktop: the embedded native window receives all pointer input over
        // the map. Deliver it to MapWidget as if MapWidget had received it
        // (same geometry, same coordinates) via sendEvent, NOT by calling the
        // handlers directly: this container's eventFilter on this->map (click
        // selection) must see these events too. MapWidget stays the single
        // navigation owner (pan, inertia, Ctrl/middle orbit, coordinates).
        surface->setNavigationMouseHandlers(
            [this](QMouseEvent *event) { return QCoreApplication::sendEvent(this->map, event); },
            [this](QMouseEvent *event) { return QCoreApplication::sendEvent(this->map, event); },
            [this](QMouseEvent *event) { return QCoreApplication::sendEvent(this->map, event); });
        // 2D imagery is drawn by bgfx itself from the shared tile repository;
        // the "slide opacity" fades it towards the background under the network.
        surface->setTileRepository(this->tile_repository);
        // DEM tiles for the globe's terrain relief.
        surface->setTerrainRepository(this->terrain_repository);
        // Terrain-aware navigation (Phase 3, slice 2): pointer coordinates
        // with DEM altitude (the 2D footer reads cached DEM tiles too), globe
        // pans on the drawn terrain at the crosshair. MapWidget keeps owning the
        // navigation.
        this->map->setRenderSurfaceScreenCoordinateResolver(
            [surface](const QPointF &screen_position, CoordinateWGS84 *coordinate)
        {
            return surface->terrainCoordinateAtScreen(screen_position, coordinate, true);
        });
        this->map->setCachedTerrainElevationResolver(
            [surface](const CoordinateWGS84 &coordinate, double *elevation_m)
        {
            return surface->cachedTerrainElevationAtCoordinate(coordinate, elevation_m);
        });
        this->map->setRenderSurfaceGlobeSurfacePointResolver(
            [surface](const QPointF &screen_position, GeoWgs84Ellipsoid::EcefPositionD *point)
        {
            return surface->globeSurfacePointAtScreen(screen_position, point);
        });
        // Keep the 3D-model preference disabled in both distribution workspaces.
        connect(this->map_menu->mapNavigationWidget(),
                &MapNavigationWidget::signal3dIconsChanged,
                surface, [this, surface](bool enabled)
                {
                    this->monitor_globe_3d_icons = enabled;
                    if (!this->editor_mode && !this->basemap_only)
                        surface->setGlobe3dIconsEnabled(MapWorkspace::useGlobe3dIconModels(
                            MapWorkspace::DistributionMonitor, enabled));
                });
        surface->setBackgroundOpacity(this->network_background_opacity);
        connect(this->map_menu->mapNavigationWidget(),
                &MapNavigationWidget::signalSlideOpacityChanged,
                surface, [this, surface](int opacity) { if (!this->editor_mode && !this->basemap_only) surface->setBackgroundOpacity(opacity); });
        // Map labels (IDs / values) from the left menu's switches.
        surface->setLabelSettings(this->map_menu->mapNavigationWidget()->labelSettings());
        connect(this->map_menu->mapNavigationWidget(),
                &MapNavigationWidget::signalLabelSettingsChanged,
                surface, [this, surface](const MapLabelSettings &settings)
        {
            if (!this->editor_mode && !this->basemap_only)
                surface->setLabelSettings(settings);
        });
        this->map_stack_layout->addWidget(surface);
        connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
                surface, [this, surface]() {
            surface->setNetworkSnapshot(this->hydraulic_data->networkRenderSnapshot());
        });
        connect(this->hydraulic_data, &HydraulicData::signalNetworkGeometryChanged,
                surface, [this, surface](quint64) {
            surface->setNetworkSnapshot(this->hydraulic_data->networkRenderSnapshot());
        });
        // Renames (no geometry change): the labels' IDs follow the snapshot.
        connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, surface,
                [this, surface](InfrastructureEntity, const QUuid &) {
            surface->setNetworkSnapshot(this->hydraulic_data->networkRenderSnapshot());
        });
        connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, surface,
                [this, surface](InfrastructureEntity, const QUuid &) {
            surface->setNetworkSnapshot(this->hydraulic_data->networkRenderSnapshot());
        });
        connect(this->hydraulic_data, &HydraulicData::signalDemandPointChanged, surface,
                [this, surface](const QUuid &) {
            surface->setNetworkSnapshot(this->hydraulic_data->networkRenderSnapshot());
        });
        surface->setNetworkSnapshot(this->hydraulic_data->networkRenderSnapshot());

        // Symbology: the resolved MapNetworkRenderSymbology (per-entity values
        // by render id). Every data change that can alter values re-resolves
        // it, coalesced to one resolve per event-loop burst by
        // applyVisualControlSymbology(). On the GPU this only re-uploads the
        // entity data textures, never geometry; palettes and ranges are
        // uniforms (MapBgfxEntityStyle).
        this->bgfx_surface = surface;

        // --- Native HUD (slices 6a, 6b; globe controls Phase 3 slice 5) -----
        // Button clicks arrive queued from the surface (never inside the
        // native window's event dispatch, fullscreen reparents the map).
        // Globe slider and compass interactions carry a phase and a number
        // and go to the globe HUD controller (map model camera).
        this->bgfx_globe_hud = new MapBgfxGlobeHudController(this->map_model, this);
        surface->setHudActionHandler([this](const MapBgfxHud::Command &command)
        {
            switch (command.action)
            {
                case MapBgfxHud::Action::GlobeSlider:
                case MapBgfxHud::Action::GlobeSliderReset:
                case MapBgfxHud::Action::CompassDrag:
                case MapBgfxHud::Action::CompassWheel:
                    this->bgfx_globe_hud->handleCommand(command);
                    break;
                case MapBgfxHud::Action::JoystickDrag:
                    this->map->beginJoystickPan(command.position);
                    break;
                default:
                    handleBgfxHudAction(static_cast<int>(command.action), command.group,
                                        command.value, command.flipped);
                    break;
            }
        });
        // Legend texts carry units: refresh them when the unit profile
        // changes (ranges and metrics follow applySymbology()).
        new UnitAwareView(surface, UnitProfileManager::instance().presentation(), [this]()
        {
            refreshBgfxLegend();
        });
#ifndef Q_OS_WASM
        // GPS position dot (same source as MapRenderHudWidget).
        if (this->gps != nullptr)
        {
            connect(this->gps, &GpsProvider::positionChanged, surface,
                    [surface](const QGeoPositionInfo &info)
            {
                const QGeoCoordinate coordinate = info.coordinate();
                surface->setHudGpsPosition(info.isValid() && coordinate.isValid(),
                                           coordinate.longitude(), coordinate.latitude());
            });
            connect(this->gps, &GpsProvider::gpsDisconnected, surface, [surface]()
            {
                surface->setHudGpsPosition(false, 0.0, 0.0);
            });
        }
#endif
        // Download activity of the shared tile/terrain repositories, exactly
        // like MapMonitorDownloadActivityHudWidget: change signals plus a
        // 400 ms poll while the surface is active.
        if (this->tile_repository != nullptr)
        {
            connect(this->tile_repository, &MapTileRepository::signalUpstreamActivityChanged,
                    surface, [this](const MapUpstreamActivity &activity)
            {
                this->bgfx_tiles_active = activity.active;
                this->bgfx_tiles_queued = activity.queued;
                pushBgfxDownloadActivity();
            });
        }
        if (this->terrain_repository != nullptr)
        {
            connect(this->terrain_repository,
                    &MapTerrainRepository::signalUpstreamActivityChanged,
                    surface, [this](const MapUpstreamActivity &activity)
            {
                this->bgfx_terrain_active = activity.active;
                this->bgfx_terrain_queued = activity.queued;
                pushBgfxDownloadActivity();
            });
        }
        this->bgfx_download_poll_timer = new QTimer(this);
        this->bgfx_download_poll_timer->setInterval(400);
        connect(this->bgfx_download_poll_timer, &QTimer::timeout, this, [this]()
        {
            refreshBgfxDownloadActivity();
        });
        // The joystick's knob follows MapWidget's captured drag, its arrows
        // also the held keyboard pan keys.
        connect(this->map, &MapWidget::joystickChanged, surface,
                [surface](QPointF deflection, bool active) { surface->setHudJoystick(deflection, active); });
        connect(this->map, &MapWidget::navigationGestureChanged, surface,
                [surface](int gesture) { surface->setHudNavigationGesture(gesture); });
        connect(this->map, &MapWidget::keyboardPanChanged, surface,
                [surface](QPointF direction) { surface->setHudKeyboardPan(direction); });
        // Fullscreen shortcut, enabled while the surface presents frames and
        // no shortcut is being captured.
        this->bgfx_fullscreen_shortcut = new QShortcut(
            guiShortcutRegistry().keySequence(GuiShortcutId::MapMonitorFullscreen),
            this->map_stack);
        this->bgfx_fullscreen_shortcut->setContext(Qt::ApplicationShortcut);
        this->bgfx_fullscreen_shortcut->setAutoRepeat(false);
        this->bgfx_fullscreen_shortcut->setEnabled(false);
        connect(this->bgfx_fullscreen_shortcut, &QShortcut::activated, this, [this]()
        {
            if (!this->bgfx_render_surface_active)
                return;
            if (!this->bgfx_fullscreen_active && !isVisible())
                return;
            setBgfxFullscreen(!this->bgfx_fullscreen_active);
        });
        connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged, this,
                [this](GuiShortcutId id)
        {
            if (id != GuiShortcutId::MapMonitorFullscreen || this->bgfx_fullscreen_shortcut == nullptr)
                return;
            this->bgfx_fullscreen_shortcut->setKey(
                guiShortcutRegistry().keySequence(GuiShortcutId::MapMonitorFullscreen));
        });
        connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutCaptureActiveChanged,
                this, [this](bool active)
        {
            if (this->bgfx_fullscreen_shortcut != nullptr)
                this->bgfx_fullscreen_shortcut->setEnabled(this->bgfx_render_surface_active && !this->editor_mode && !active);
        });

        // Model/inspector selection can change while the editor is active.
        // Track it, but only the monitor profile owns the GPU highlight.
        connect(this->hydraulic_data, &HydraulicData::signalSelectedTank, this,
                [this](const HydraulicNodeTank &tank)
        { rememberMonitorSelection(InfrastructureEntity::Tank, tank.uuid); });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedReservoir, this,
                [this](const HydraulicNodeReservoir &reservoir)
        { rememberMonitorSelection(InfrastructureEntity::Reservoir, reservoir.uuid); });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedJunction, this,
                [this](const HydraulicNodeJunction &junction)
        { rememberMonitorSelection(InfrastructureEntity::Junction, junction.uuid); });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedPipe, this,
                [this](const HydraulicLinkPipe &pipe)
        { rememberMonitorSelection(InfrastructureEntity::Pipe, pipe.uuid); });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedPump, this,
                [this](const HydraulicLinkPump &pump)
        { rememberMonitorSelection(InfrastructureEntity::Pump, pump.uuid); });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedValve, this,
                [this](const HydraulicLinkValve &valve)
        { rememberMonitorSelection(InfrastructureEntity::Valve, valve.uuid); });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedDemandPoint, this,
                [this](const HydraulicDemandPoint &point)
        { rememberMonitorSelection(InfrastructureEntity::DemandPoint, point.uuid); });

        // Simulation errors are monitor-only visual state. Keep accepting
        // model updates in the editor without overwriting the editor HUD.
        connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
                this, [this]() { applyMonitorDiagnostics(); });
        connect(this->hydraulic_data, &HydraulicData::signalNetworkGeometryChanged,
                this, [this](quint64) { applyMonitorDiagnostics(); });
        connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this,
                [this](InfrastructureEntity, const QUuid &) { applyMonitorDiagnostics(); });
        connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, this,
                [this](InfrastructureEntity, const QUuid &) { applyMonitorDiagnostics(); });
        connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged,
                this, [this](bool) { applyMonitorDiagnostics(); });
        applyMonitorDiagnostics();

        const std::function<void()> request_bgfx_symbology = [this]()
        {
            // Data, not a slider: values and colours must be resolved again.
            this->symbology_resolve_requested = true;
            applyVisualControlSymbology();
        };
        connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
                surface, request_bgfx_symbology);
        connect(this->hydraulic_data, &HydraulicData::signalNetworkGeometryChanged,
                surface, [request_bgfx_symbology](quint64) { request_bgfx_symbology(); });
        connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, surface,
                [request_bgfx_symbology](InfrastructureEntity, const QUuid &)
                { request_bgfx_symbology(); });
        connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, surface,
                [request_bgfx_symbology](InfrastructureEntity, const QUuid &)
                { request_bgfx_symbology(); });
        connect(this->hydraulic_data, &HydraulicData::signalSimulationHeadlossFormulaChanged,
                surface, request_bgfx_symbology);
        connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged,
                surface, [request_bgfx_symbology](bool) { request_bgfx_symbology(); });
        connect(this->hydraulic_data,
                &HydraulicData::signalWaterQualitySimulationResultTimelineChanged,
                surface, [request_bgfx_symbology](bool) { request_bgfx_symbology(); });
        connect(this->hydraulic_data, &HydraulicData::signalCurrentSimulationResultChanged,
                surface, [request_bgfx_symbology](int) { request_bgfx_symbology(); });
        applySymbology();

        connect(surface, &MapBgfxSurface::signalRendererReady, this, [this, surface]() {
            this->bgfx_render_surface_active = true;
            this->renderer_status->hide();
#ifndef Q_OS_WASM
            this->map_stack_layout->setCurrentWidget(surface);
#else
            // The bgfx canvas is an HTML canvas above the Qt canvas with
            // pointer-events:none; MapWidget stays on top of the Qt stack and
            // receives the pointer input.
            this->map_stack_layout->setCurrentWidget(this->map);
#endif
            // bgfx presents the frames, so it is the only navigation clock.
            this->map->setNavigationFrameDriver(true);
#ifndef Q_OS_WASM
            this->map->setOrbitPointerLockWindow(surface->nativeInputWindow());
#endif
            surface->focusNavigationInput();
            // The map legend moves from the dock onto the HUD: hide the
            // dock's groups and show the HUD legend with the current
            // symbology.
            emit signalShowMapLegendNode(VisualNode::None);
            emit signalShowMapLegendLink(VisualLink::None);
            emit signalShowMapLegendHeatmap(VisualHeatmap::None);
            if (!this->editor_mode && !this->basemap_only)
                refreshBgfxLegend();
            // HUD inputs: current download numbers now, then the poll.
            refreshBgfxDownloadActivity();
            if (this->bgfx_download_poll_timer != nullptr)
                this->bgfx_download_poll_timer->start();
            if (this->bgfx_fullscreen_shortcut != nullptr)
            {
                this->bgfx_fullscreen_shortcut->setEnabled(
                    !this->editor_mode && !guiShortcutRegistry().shortcutCaptureActive());
            }
            // WASM: MapWidget keeps pointer input on the Qt layer (the bgfx
            // canvas has pointer-events:none); picking and the HUD are routed
            // from this container's event filter on the map.
            qInfo() << "bgfx Map Monitor surface active";
        });
        connect(surface, &MapBgfxSurface::signalRendererFailed, this,
                [this, surface](const QString &reason) {
            qWarning() << "bgfx Map Monitor failed; no map renderer is available:" << reason;
            if (this->bgfx_fullscreen_active)
                setBgfxFullscreen(false);
            if (this->bgfx_fullscreen_shortcut != nullptr)
                this->bgfx_fullscreen_shortcut->setEnabled(false);
            if (this->bgfx_download_poll_timer != nullptr)
                this->bgfx_download_poll_timer->stop();
            setBgfxHudCursor(Qt::ArrowCursor);
            this->bgfx_render_surface_active = false;
            // Without a map the legend lives in the dock again.
            surface->closeHudPopup();
            emit signalShowMapLegendNode(this->symbology_settings.visual_node);
            emit signalShowMapLegendLink(this->symbology_settings.visual_link);
            emit signalShowMapLegendHeatmap(this->symbology_settings.visual_heatmap);
            this->map->setNavigationFrameDriver(false);
#ifndef Q_OS_WASM
            this->map->setOrbitPointerLockWindow(nullptr);
#endif
            showRendererStatus(RendererStatusKind::Failed, reason);
            surface->hide();
        });
        surface->show();
        showRendererStatus(RendererStatusKind::Starting);
        // Startup watchdog: a renderer that neither presents a frame nor
        // fails must not leave an endless "Starting..." behind. Report
        // where it stands instead (also in the log / browser console).
        QTimer::singleShot(10000, surface, [this, surface]()
        {
            if (this->bgfx_render_surface_active || !surface->isVisible())
                return;
            const QString diagnostics = surface->startupDiagnostics();
            qWarning().noquote() << "bgfx map did not start within 10 s:" << diagnostics;
            showRendererStatus(RendererStatusKind::Stalled, diagnostics);
        });
    }

    this->layout->addWidget(this->tool_panel);
    this->layout->addWidget(this->map_stack);
    
    connect(this->map_menu, &MapMonitorMenuWidget::signalNodeVisualClicked, this,
        [this](VisualNode visual_node)
    {
        this->symbology_settings.visual_node = visual_node;
        showMapLegendNode(visual_node);
        applySymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalNodeSizeUnitChanged, this,
        [this](NetworkSymbologySizeUnit unit)
    {
        this->symbology_settings.node_size_unit = unit;
        applyVisualControlSymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalNodeSizeChanged, this,
        [this](NetworkSymbologySizeUnit unit, double size)
    {
        this->symbology_settings.node_size_unit = unit;
        this->symbology_settings.show_junctions = size > 0.0;
        if (size > 0.0)
        {
            if (unit == NetworkSymbologySizeUnit::Meters)
                this->symbology_settings.node_size_m = size;
            else
                this->symbology_settings.node_size_px = qRound(size);
        }
        applyVisualControlSymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalNodeDeclutteringChanged, this,
        [this](bool enabled)
    {
        this->monitor_node_decluttering = enabled;
        if (this->bgfx_surface != nullptr && !this->editor_mode)
            this->bgfx_surface->setNodeDeclutteringEnabled(enabled);
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalLinkVisualClicked, this,
        [this](VisualLink visual_link)
    {
        this->symbology_settings.visual_link = visual_link;
        showMapLegendLink(visual_link);
        applySymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalLinkThicknessUnitChanged, this,
        [this](NetworkSymbologySizeUnit unit)
    {
        this->symbology_settings.link_thickness_unit = unit;
        applyVisualControlSymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalLinkThicknessChanged, this,
        [this](NetworkSymbologySizeUnit unit, double thickness)
    {
        this->symbology_settings.link_thickness_unit = unit;
        if (unit == NetworkSymbologySizeUnit::Meters)
            this->symbology_settings.link_thickness_m = thickness;
        else
            this->symbology_settings.link_thickness_px = qRound(thickness);
        applyVisualControlSymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalFlowDirectionSizeChanged, this, [this](int size_px)
    {
        const bool was_visible = this->symbology_settings.show_flow_direction;
        this->symbology_settings.flow_direction_size_px = size_px;
        this->symbology_settings.show_flow_direction = size_px > 0;
        if (was_visible != this->symbology_settings.show_flow_direction)
            applySymbology();
        else
            applyVisualControlSymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalHeatmapVisualClicked, this, [this](VisualHeatmap visual_heatmap)
    {
        this->symbology_settings.visual_heatmap = visual_heatmap;
        showMapLegendHeatmap(visual_heatmap);
        applySymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalHeatmapOpacityChanged, this, [this](int opacity)
    {
        this->symbology_settings.heatmap_opacity = opacity;
        applyVisualControlSymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalHeatmapRadiusUnitChanged, this,
        [this](HeatmapRadiusUnit unit)
    {
        this->symbology_settings.heatmap_radius_unit = unit;
        applyVisualControlSymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalHeatmapRadiusChanged, this,
        [this](HeatmapRadiusUnit unit, int radius)
    {
        if (unit == HeatmapRadiusUnit::Pixels)
            this->symbology_settings.heatmap_radius_px = radius;
        else
            this->symbology_settings.heatmap_radius_m = radius;
        applyVisualControlSymbology();
    });
    connect(this->map_menu, &MapMonitorMenuWidget::signalHeatmapSolidCenterChanged, this, [this](int percent)
    {
        this->symbology_settings.heatmap_solid_center_percent = percent;
        applyVisualControlSymbology();
    });
}

MapMonitorContainer::~MapMonitorContainer()
{
    // The resolvers capture the bgfx surface.
    this->map->setRenderSurfaceScreenCoordinateResolver(MapWidget::ScreenCoordinateResolver());
    this->map->setRenderSurfaceGlobeSurfacePointResolver(MapWidget::GlobeSurfacePointResolver());
    this->map->setCachedTerrainElevationResolver(MapWidget::CachedTerrainElevationResolver());
}

void MapMonitorContainer::setNodePalette(NetworkSymbologyPalette palette, bool flipped)
{
    const bool changed =
        this->symbology_settings.node_palette != palette
        || this->symbology_settings.node_palette_flipped != flipped;
    this->symbology_settings.node_palette = palette;
    this->symbology_settings.node_palette_flipped = flipped;


    if (!changed)
        return;

    if (!saveGuiNodeSymbologyPalette(palette, flipped))
        qWarning() << "Failed to save node symbology palette preference.";

    emit signalNodePaletteChanged(palette, flipped);
    applyPaletteSymbology();
}

void MapMonitorContainer::setLinkPalette(NetworkSymbologyPalette palette, bool flipped)
{
    const bool changed =
        this->symbology_settings.link_palette != palette
        || this->symbology_settings.link_palette_flipped != flipped;
    this->symbology_settings.link_palette = palette;
    this->symbology_settings.link_palette_flipped = flipped;


    if (!changed)
        return;

    if (!saveGuiLinkSymbologyPalette(palette, flipped))
        qWarning() << "Failed to save link symbology palette preference.";

    emit signalLinkPaletteChanged(palette, flipped);
    applyPaletteSymbology();
}

void MapMonitorContainer::setHeatmapPalette(NetworkSymbologyPalette palette, bool flipped)
{
    const bool changed =
        this->symbology_settings.heatmap_palette != palette
        || this->symbology_settings.heatmap_palette_flipped != flipped;
    this->symbology_settings.heatmap_palette = palette;
    this->symbology_settings.heatmap_palette_flipped = flipped;


    if (!changed)
        return;

    if (!saveGuiHeatmapSymbologyPalette(palette, flipped))
        qWarning() << "Failed to save heatmap symbology palette preference.";

    emit signalHeatmapPaletteChanged(palette, flipped);
    applyPaletteSymbology();
}

void MapMonitorContainer::showMapLegendNode(VisualNode visual_node)
{
    // The bgfx HUD legend follows applySymbology(), which every caller
    // runs next; the dock stays hidden while the bgfx map is active.
    if (this->bgfx_render_surface_active)
        return;

    emit signalShowMapLegendNode(visual_node);
}

void MapMonitorContainer::showMapLegendLink(VisualLink visual_link)
{
    // The bgfx HUD legend follows applySymbology(), which every caller
    // runs next; the dock stays hidden while the bgfx map is active.
    if (this->bgfx_render_surface_active)
        return;

    emit signalShowMapLegendLink(visual_link);
}

void MapMonitorContainer::showMapLegendHeatmap(VisualHeatmap visual_heatmap)
{
    // The bgfx HUD legend follows applySymbology(), which every caller
    // runs next; the dock stays hidden while the bgfx map is active.
    if (this->bgfx_render_surface_active)
        return;

    emit signalShowMapLegendHeatmap(visual_heatmap);
}


bool MapMonitorContainer::eventFilter(QObject *watched, QEvent *event)
{
    // The controller canvas is not a visual child of the live viewport:
    // keep its logical size synchronized for hit-testing and overlays.
    if (watched == this->map && event->type() == QEvent::Resize
        && this->editor_input_canvas != nullptr)
        this->editor_input_canvas->resize(this->map->size());
    // WASM sends map input through the Qt MapWidget. Desktop events enter
    // through MapBgfxSurface's native-window handlers instead.
    if (this->editor_mode && watched == this->map && this->editor_input_canvas != nullptr)
    {
        switch (event->type())
        {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseButtonRelease:
            case QEvent::MouseMove:
            case QEvent::Wheel:
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
            case QEvent::FocusOut:
                QCoreApplication::sendEvent(this->editor_input_canvas, event);
                return true;
            default:
                break;
        }
    }

    // Map fullscreen: the hidden left menu's button shortcuts, for keys the
    // map widget receives itself (WASM; desktop keys of the native window
    // take the navigation key handler, see setNavigationHandlers()).
    if (watched == this->map && event->type() == QEvent::KeyPress && this->bgfx_fullscreen_active
        && MapNavigationWidget::triggerButtonShortcut(this->map_menu, static_cast<QKeyEvent *>(event)))
    {
        event->accept();
        return true;
    }
#ifdef Q_OS_WASM
    // A press on the map or a HUD element brings the keys back to the map
    // (desktop: MapBgfxSurface does it for its native window), also when
    // the HUD consumes the press.
    if (watched == this->map && event->type() == QEvent::MouseButtonPress)
        this->map->setFocus(Qt::MouseFocusReason);
#endif
    // The bgfx HUD sits on top of the map: it sees every pointer event first
    // (desktop: forwarded from the native window through MapWidget; WASM:
    // MapWidget receives them directly), and what it consumes never reaches
    // navigation, picking or hover.
    bool over_bgfx_hud_panel = false;
    if (watched == this->map
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseMove)
        && routeBgfxHudMouseEvent(static_cast<QMouseEvent *>(event), &over_bgfx_hud_panel))
    {
        event->accept();
        return true;
    }
    // WASM: MapWidget receives wheel and keys directly (desktop: the native
    // window, routed inside the surface). Wheel over a HUD panel scrolls a
    // legend list or is swallowed; Escape closes a HUD popup; losing focus
    // closes it like a Qt popup.
    if (watched == this->map && this->bgfx_surface != nullptr && this->bgfx_render_surface_active)
    {
        if (event->type() == QEvent::Wheel
            && this->bgfx_surface->handleHudWheelEvent(static_cast<QWheelEvent *>(event)))
        {
            event->accept();
            return true;
        }
        if (event->type() == QEvent::KeyPress
            && this->bgfx_surface->handleHudKeyPress(static_cast<QKeyEvent *>(event)))
        {
            event->accept();
            return true;
        }
#ifdef Q_OS_WASM
        if (event->type() == QEvent::FocusOut)
            this->bgfx_surface->closeHudPopup();
#endif
    }

    if (watched == this->map && !this->basemap_only)
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            QMouseEvent *mouse_event = static_cast<QMouseEvent *>(event);
            if (mouse_event->button() == Qt::LeftButton)
            {
                if (selectBgfxNetworkEntityAt(mouse_event->position()))
                {
                    mouse_event->accept();
                    return true;
                }
            }
        }
        else if (event->type() == QEvent::MouseMove)
        {
            QMouseEvent *mouse_event = static_cast<QMouseEvent *>(event);
            // No network picking through a HUD panel.
            if (over_bgfx_hud_panel)
                setNetworkHovered(false);
            else
                updateNetworkHover(
                    mouse_event->position(), mouse_event->buttons(), mouse_event->modifiers());
        }
        else if (event->type() == QEvent::Leave || event->type() == QEvent::Hide)
        {
            setNetworkHovered(false);
        }
    }

    return QWidget::eventFilter(watched, event);
}

bool MapMonitorContainer::selectBgfxNetworkEntityAt(const QPointF &position)
{
    if (this->basemap_only || !this->bgfx_render_surface_active || this->bgfx_surface == nullptr
        || !this->bgfx_surface->isVisible())
        return false;
    // A hit selects the entity (the highlight follows HydraulicData's selection signal), a miss clears it.
    const MapRenderHit hit = this->bgfx_surface->hitTest(position);
    if (hit.isValid() && selectNetworkEntity(hit.render_id, hit.entity_type, hit.uuid))
    {
        this->bgfx_surface->setSelectedEntity(hit.entity_type, hit.uuid);
        return true;
    }
    rememberMonitorSelection(InfrastructureEntity::Unknown, QUuid());
    return false;
}

bool MapMonitorContainer::selectNetworkEntity(quint32 render_id, InfrastructureEntity entity_type, const QUuid &uuid)
{
    if (this->hydraulic_data == nullptr || render_id == 0 || entity_type == InfrastructureEntity::Unknown)
        return false;

    if (!uuid.isNull())
    {
        this->hydraulic_data->setSelectedUuid(entity_type, uuid);
        return true;
    }

    const NetworkRenderSnapshot &snapshot = this->hydraulic_data->networkRenderSnapshot();
    if (InfrastructureEntityTraits::isHydraulicPointEntity(entity_type))
    {
        for (const NetworkRenderNode &node : snapshot.nodes)
        {
            if (node.render_id != render_id || node.entity_type != entity_type)
                continue;

            this->hydraulic_data->setSelectedUuid(entity_type, node.uuid);
            return true;
        }
        return false;
    }

    if (InfrastructureEntityTraits::isHydraulicNetworkLink(entity_type))
    {
        for (const NetworkRenderLink &link : snapshot.links)
        {
            if (link.render_id != render_id || link.entity_type != entity_type)
                continue;

            this->hydraulic_data->setSelectedUuid(entity_type, link.uuid);
            return true;
        }
    }

    return false;
}

void MapMonitorContainer::updateNetworkHover(
    const QPointF &position, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers)
{
    // Ctrl-without-a-button is the desktop orbit gesture. MouseMove reaches
    // this container's event filter before MapWidget starts/updates that
    // gesture, so checking buttons alone used to run MapBgfxSurface::hitTest()
    // first. Globe hit-testing projects every junction, icon, node and pipe
    // segment on the CPU; doing that for every KY4 orbit event is pure work
    // whose result is immediately hidden with the cursor. The navigation
    // state covers all subsequent Ctrl-orbit events (and keyboard zoom),
    // while the modifier covers the first event before that state changes.
    const bool orbit_view = this->map_model != nullptr
        && this->map_model->viewMode() == MapViewMode::Globe;
    // The orbit and the free-look hold keys alike (configurable).
    const bool ctrl_orbit_requested = orbit_view
        && (modifiers.testFlag(guiShortcutRegistry().holdModifier(GuiHoldModifierId::GlobeOrbit))
            || modifiers.testFlag(guiShortcutRegistry().holdModifier(GuiHoldModifierId::GlobeLook)));
    const bool camera_navigation_active = orbit_view
        && this->map_model->view3dNavigationState()
            == MapView3dNavigationState::Rotate;
    if (buttons != Qt::NoButton
        || ctrl_orbit_requested
        || camera_navigation_active)
    {
        setNetworkHovered(false);
        return;
    }


    if (this->bgfx_render_surface_active && this->bgfx_surface != nullptr
        && this->bgfx_surface->isVisible())
    {
        setNetworkHovered(this->bgfx_surface->hitTest(position).isValid());
        return;
    }
    setNetworkHovered(false);
}

void MapMonitorContainer::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::LanguageChange)
        return;

    if (this->renderer_status != nullptr)
        refreshRendererStatusText();
    // The legend is GUI presentation data. Recreate only its translated
    // captions from retained ranges, independently of whether the Render
    // view is currently presenting frames or its host callback has fired.
    // In editor/basemap modes this is intentionally a no-op.
    refreshHudLegendText();
}

void MapMonitorContainer::refreshRendererStatusText()
{
    switch (this->renderer_status_kind)
    {
    case RendererStatusKind::Starting:
        this->renderer_status->setText(tr("Starting map renderer…"));
        break;
    case RendererStatusKind::Failed:
        this->renderer_status->setText(
            tr("The bgfx map renderer could not start:\n%1").arg(this->renderer_status_details));
        break;
    case RendererStatusKind::Stalled:
        this->renderer_status->setText(tr("The bgfx map renderer has not presented a frame "
                                          "after 10 s.\n%1").arg(this->renderer_status_details));
        break;
    }
}

void MapMonitorContainer::showRendererStatus(RendererStatusKind kind, const QString &details)
{
    this->renderer_status_kind = kind;
    this->renderer_status_details = details;
    refreshRendererStatusText();
    this->renderer_status->show();
    this->map_stack_layout->setCurrentWidget(this->renderer_status);
    this->renderer_status->raise();
}

void MapMonitorContainer::setNetworkHovered(bool hovered)
{
    if (this->network_hovered == hovered)
        return;

    this->network_hovered = hovered;
    applyMapHoverCursor();
}

void MapMonitorContainer::applyMapHoverCursor()
{
    // The HUD element under the pointer decides first (button, ramp), then
    // a pickable network entity.
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (this->bgfx_hud_cursor != Qt::ArrowCursor)
        shape = this->bgfx_hud_cursor;
    else if (this->network_hovered)
        shape = Qt::PointingHandCursor;
    if (shape != Qt::ArrowCursor)
        this->map->setCursor(shape);
    else
        this->map->unsetCursor();
    // Desktop: the visible cursor over the bgfx map is the native window's.
    if (this->bgfx_surface != nullptr)
        this->bgfx_surface->setMapCursor(shape);
}

void MapMonitorContainer::setBgfxHudCursor(Qt::CursorShape shape)
{
    if (this->bgfx_hud_cursor == shape)
        return;
    this->bgfx_hud_cursor = shape;
    applyMapHoverCursor();
}

bool MapMonitorContainer::routeBgfxHudMouseEvent(QMouseEvent *event, bool *over_hud_panel)
{
    *over_hud_panel = false;
    if (this->bgfx_surface == nullptr || !this->bgfx_render_surface_active
        || !this->bgfx_surface->isVisible())
    {
        return false;
    }
    const MapBgfxSurface::HudPointer pointer = this->bgfx_surface->handleHudMouseEvent(event);
    *over_hud_panel = pointer.over_panel;
    if (event->type() == QEvent::MouseMove)
        setBgfxHudCursor(pointer.cursor);
    return pointer.consumed;
}

void MapMonitorContainer::handleBgfxHudAction(int action, int group, int value, bool flipped)
{
    switch (static_cast<MapBgfxHud::Action>(action))
    {
        case MapBgfxHud::Action::CancelMapTiles:
            if (this->tile_repository != nullptr)
                this->tile_repository->cancelUpstreamDownloads();
            // Same short settle delay as the QWidget HUD before re-reading.
            QTimer::singleShot(100, this, [this]() { refreshBgfxDownloadActivity(); });
            break;
        case MapBgfxHud::Action::CancelTerrain:
            if (this->terrain_repository != nullptr)
                this->terrain_repository->cancelUpstreamDownloads();
            QTimer::singleShot(100, this, [this]() { refreshBgfxDownloadActivity(); });
            break;
        case MapBgfxHud::Action::ViewMode2d:
            if (this->map_model->viewMode() != MapViewMode::TwoD)
                this->map_model->setViewMode(MapViewMode::TwoD);
            break;
        case MapBgfxHud::Action::ViewMode3d:
            // Offered (enabled) only when the globe surface initialized
            // (MapBgfxSurface::globeAvailable()).
            if (this->bgfx_surface != nullptr && this->bgfx_surface->globeAvailable()
                && this->map_model->viewMode() != MapViewMode::Globe)
                this->map_model->setViewMode(MapViewMode::Globe, this->map->size());
            break;
        case MapBgfxHud::Action::ToggleFullscreen:
            setBgfxFullscreen(!this->bgfx_fullscreen_active);
            break;
        case MapBgfxHud::Action::ZoomIn:
            this->map->zoomIn();
            break;
        case MapBgfxHud::Action::ZoomOut:
            this->map->zoomOut();
            break;
        case MapBgfxHud::Action::LegendVisual:
            // Same as the left menu's radio buttons: settings, the radio
            // buttons themselves, then one symbology resolve (which also pushes
            // the new legend range).
            if (group == 0)
            {
                const VisualNode visual_node = static_cast<VisualNode>(value);
                this->symbology_settings.visual_node = visual_node;
                this->map_menu->setNodeVisualSelection(visual_node);
                showMapLegendNode(visual_node);
            }
            else if (group == 1)
            {
                const VisualLink visual_link = static_cast<VisualLink>(value);
                this->symbology_settings.visual_link = visual_link;
                this->map_menu->setLinkVisualSelection(visual_link);
                showMapLegendLink(visual_link);
            }
            else if (group == 2)
            {
                const VisualHeatmap visual_heatmap = static_cast<VisualHeatmap>(value);
                this->symbology_settings.visual_heatmap = visual_heatmap;
                this->map_menu->setHeatmapVisualSelection(visual_heatmap);
                showMapLegendHeatmap(visual_heatmap);
            }
            else
            {
                break;
            }
            applySymbology();
            break;
        case MapBgfxHud::Action::LegendPalette:
        {
            // Saved as preference and mirrored to the dock like a palette
            // chosen there (setNodePalette() & co.).
            const NetworkSymbologyPalette palette = static_cast<NetworkSymbologyPalette>(value);
            if (group == 0)
                setNodePalette(palette, flipped);
            else if (group == 1)
                setLinkPalette(palette, flipped);
            else if (group == 2)
                setHeatmapPalette(palette, flipped);
            break;
        }
        // Globe (slice 5): the same surface setters as the QWidget HUD's
        // "wireframe" / "map" check boxes and underground combo.
        case MapBgfxHud::Action::ToggleWireframe:
            if (this->bgfx_surface != nullptr)
                this->bgfx_surface->setTerrainWireframeVisible(!this->bgfx_surface->terrainWireframeVisible());
            break;
        case MapBgfxHud::Action::ToggleMapTiles:
            if (this->bgfx_surface != nullptr)
                this->bgfx_surface->setMapTilesVisible(!this->bgfx_surface->mapTilesVisible());
            break;
        case MapBgfxHud::Action::ToggleHillshade:
            if (this->bgfx_surface != nullptr)
                this->bgfx_surface->setTerrainHillshadeVisible(!this->bgfx_surface->terrainHillshadeVisible());
            break;
        case MapBgfxHud::Action::UndergroundMode:
            if (this->bgfx_surface != nullptr)
                this->bgfx_surface->setUndergroundMode(static_cast<MapUndergroundMode>(value));
            break;
        // Handled by bgfx_globe_hud (setHudActionHandler()).
        case MapBgfxHud::Action::GlobeSlider:
        case MapBgfxHud::Action::GlobeSliderReset:
        case MapBgfxHud::Action::CompassDrag:
        case MapBgfxHud::Action::CompassWheel:
        case MapBgfxHud::Action::JoystickDrag:
        case MapBgfxHud::Action::None:
            break;
    }
}

void MapMonitorContainer::refreshBgfxDownloadActivity()
{
    // Answers arrive through signalUpstreamActivityChanged.
    if (this->tile_repository != nullptr)
        this->tile_repository->requestUpstreamActivity();
    if (this->terrain_repository != nullptr)
        this->terrain_repository->requestUpstreamActivity();
}

void MapMonitorContainer::pushBgfxDownloadActivity()
{
    if (this->bgfx_surface != nullptr)
    {
        this->bgfx_surface->setHudDownloadActivity(
            this->bgfx_tiles_active, this->bgfx_tiles_queued,
            this->bgfx_terrain_active, this->bgfx_terrain_queued);
    }
}

void MapMonitorContainer::setBgfxFullscreen(bool fullscreen)
{
    if (fullscreen == this->bgfx_fullscreen_active)
        return;
    if (fullscreen && (!this->bgfx_render_surface_active || this->bgfx_surface == nullptr))
        return;

    // In place (MainWindow::setMapFullscreen()): the main window goes full
    // screen and hides everything but this map. The map stack is never
    // reparented, so its native window (desktop) or the shared browser
    // canvas (WASM) stays as it is: bgfx keeps running, nothing is shut
    // down or uploaded again. (A reparented map stack forced a full bgfx
    // restart, and in the browser the full-screen Qt window covered the bgfx
    // canvas: an empty page.)
    applyMapFullscreen(fullscreen);
    emit signalMapFullscreenRequested(fullscreen);
}

void MapMonitorContainer::applyMapFullscreen(bool fullscreen)
{
    if (fullscreen == this->bgfx_fullscreen_active)
        return;

    this->bgfx_fullscreen_active = fullscreen;
    // Sidebar visibility belongs to MapWorkspaceViewportPage: hiding only
    // this child leaves the QStackedWidget's width allocated in fullscreen.
    if (this->bgfx_surface != nullptr)
    {
        this->bgfx_surface->setHudFullscreen(fullscreen);
        this->bgfx_surface->focusNavigationInput();
    }
}

void MapMonitorContainer::applySymbology()
{
    if (this->editor_mode || this->basemap_only)
        return;
    this->symbology_settings = this->symbology_settings.bounded();

    const NetworkSymbologyRanges ranges =
        this->hydraulic_data->symbologyRanges(this->symbology_settings);
    if (this->bgfx_surface != nullptr)
    {
        MapNetworkRenderSymbology monitor_symbology = this->symbology_resolver.resolve(
            *this->hydraulic_data, this->symbology_settings, ranges);
        // Restore the light icon interiors (notably demand-point stars) in
        // Monitor. The editor sets its own symbology on this shared surface.
        // Metric-coloured icons still use their selected palette.
        monitor_symbology.icon_default_fill_color = qRgb(255, 255, 255);
        this->bgfx_surface->setSymbology(monitor_symbology);
        pushBgfxLegend(ranges);
        this->resolved_symbology_settings = this->symbology_settings;
        this->symbology_resolved = true;
        this->symbology_resolve_requested = false;
    }
}

void MapMonitorContainer::pushBgfxLegend(const NetworkSymbologyRanges &ranges)
{
    if (this->editor_mode || this->basemap_only)
        return;
    if (this->bgfx_surface == nullptr || this->hydraulic_data == nullptr)
        return;
    this->hud_legend_settings = this->symbology_settings;
    this->hud_legend_ranges = ranges;
    refreshHudLegendText();
    // Value labels show the numbers of the legend ramps (same unit).
    this->bgfx_surface->setLabelValueFormats(
        mapBgfxLabelValueFormat(*this->hydraulic_data, this->symbology_settings, ranges, false),
        mapBgfxLabelValueFormat(*this->hydraulic_data, this->symbology_settings, ranges, true));
}

void MapMonitorContainer::refreshHudLegendText()
{
    if (this->editor_mode || this->basemap_only || !this->hud_legend_ranges
        || this->bgfx_surface == nullptr || this->hydraulic_data == nullptr)
        return;
    MapBgfxHud::Legend legend = mapBgfxLegendContent(
        *this->hydraulic_data, this->hud_legend_settings, *this->hud_legend_ranges);
    legend.visible = this->bgfx_render_surface_active;
    this->bgfx_surface->setHudLegend(legend);
}

void MapMonitorContainer::refreshBgfxLegend()
{
    if (this->editor_mode || this->basemap_only)
        return;
    if (this->bgfx_surface == nullptr || this->hydraulic_data == nullptr)
        return;
    this->symbology_settings = this->symbology_settings.bounded();
    pushBgfxLegend(this->hydraulic_data->symbologyRanges(this->symbology_settings));
}

void MapMonitorContainer::applyPaletteSymbology()
{
    if (this->editor_mode || this->basemap_only)
        return;
    // Palettes are GPU state (MapBgfxEntityStyle's palette texture): a
    // palette or flip change re-resolves nothing, only the legend's ramps
    // are drawn again.
    this->symbology_settings = this->symbology_settings.bounded();
    if (this->bgfx_surface == nullptr)
        return;
    if (!this->symbology_resolved)
    {
        applySymbology();
        return;
    }
    this->bgfx_surface->setVisualControlSettings(this->symbology_settings);
    refreshBgfxLegend();
}

void MapMonitorContainer::applyVisualControlSymbology()
{
    this->symbology_settings = this->symbology_settings.bounded();
    if (this->visual_control_symbology_apply_pending)
        return;

    if (this->editor_mode || this->basemap_only)
        return;
    this->visual_control_symbology_apply_pending = true;
    QTimer::singleShot(16, this, [this]
    {
        this->visual_control_symbology_apply_pending = false;
        if (this->editor_mode || this->basemap_only)
            return;
        this->symbology_settings = this->symbology_settings.bounded();

        // Sizes, junction visibility, flow-arrow size, palettes and the
        // heatmap's radius / opacity / solid centre are GPU uniforms or the
        // palette texture (setVisualControlSettings()): nothing is resolved
        // per entity while a slider moves. Metrics and the flow-arrow toggle
        // (flow directions are only resolved while it is on) need the
        // resolve of the per-entity values.
        const NetworkSymbologySettings &resolved = this->resolved_symbology_settings;
        const NetworkSymbologySettings &current = this->symbology_settings;
        const bool resolve_inputs_unchanged = this->symbology_resolved
            && !this->symbology_resolve_requested
            && resolved.visual_node == current.visual_node
            && resolved.visual_link == current.visual_link
            && resolved.visual_heatmap == current.visual_heatmap
            && resolved.show_flow_direction == current.show_flow_direction;
        if (this->bgfx_surface != nullptr && resolve_inputs_unchanged)
        {
            this->bgfx_surface->setVisualControlSettings(current);
            return;
        }
        applySymbology();
    });
}

void MapMonitorContainer::setNetworkBackgroundOpacity(int opacity)
{
    const int bounded_opacity = qBound(0, opacity, 100);
    if (this->network_background_opacity == bounded_opacity)
        return;

    this->network_background_opacity = bounded_opacity;
}


MapBgfxSurface *MapMonitorContainer::sharedBgfxSurface() const
{
    return this->bgfx_surface;
}

bool MapMonitorContainer::surfaceReady() const
{
    return this->bgfx_render_surface_active;
}

void MapMonitorContainer::rememberMonitorSelection(InfrastructureEntity entity_type,
                                                   const QUuid &uuid)
{
    this->monitor_selected_type = entity_type;
    this->monitor_selected_uuid = uuid;
    if (!this->editor_mode && !this->basemap_only && this->bgfx_surface != nullptr)
        this->bgfx_surface->setSelectedEntity(entity_type, uuid);
}

void MapMonitorContainer::applyMonitorDiagnostics()
{
    if (this->editor_mode || this->basemap_only || this->bgfx_surface == nullptr || this->hydraulic_data == nullptr)
        return;
    this->bgfx_surface->setSimulationErrorEntities(
        this->hydraulic_data->simulationErrorEntities(),
        this->hydraulic_data->simulationStaleDiagnosticEntityUuids());
}

void MapMonitorContainer::setEditorInputCanvas(QWidget *canvas)
{
    this->editor_input_canvas = canvas;
}

void MapMonitorContainer::setBasemapOnly(bool enabled)
{
    // Returning from Map Management to Drainage can re-activate the monitor
    // HUD while basemap-only was already enabled. Reapply the empty-layer
    // presentation even then, without rebuilding any geometry.
    if (this->basemap_only == enabled && !enabled)
        return;
    this->basemap_only = enabled;
    if (this->bgfx_surface == nullptr)
        return;

    if (enabled)
    {
        // No renderer shutdown and no change to retained geometry. The
        // surface hides distribution meshes, labels and 3D heatmaps itself.
        setNetworkHovered(false);
        this->bgfx_surface->closeHudPopup();
        this->bgfx_surface->setDistributionNetworkVisible(false);
        // Drainage should show the unfaded basemap even if Monitor or Editor
        // has configured a background-opacity preview.
        this->bgfx_surface->setBackgroundOpacity(0);
        this->bgfx_surface->setHudLegend(MapBgfxHud::Legend());
        this->bgfx_surface->setSimulationErrorEntities({}, {});
        this->bgfx_surface->setSelectedEntity(InfrastructureEntity::Unknown, QUuid());
        this->bgfx_surface->setLabelSettings(MapLabelSettings());
    }
    else
    {
        this->bgfx_surface->setDistributionNetworkVisible(true);
        if (!this->editor_mode)
            activateMonitorSurface();
    }
}

void MapMonitorContainer::setEditorMode(bool enabled)
{
    if (this->editor_mode == enabled)
        return;
    this->editor_mode = enabled;
    this->network_hovered = false;
    if (this->bgfx_fullscreen_shortcut != nullptr)
        this->bgfx_fullscreen_shortcut->setEnabled(
            !enabled && this->bgfx_render_surface_active
            && !guiShortcutRegistry().shortcutCaptureActive());
    if (!enabled)
        activateMonitorSurface();
}

void MapMonitorContainer::activateMonitorSurface()
{
    MapBgfxSurface *surface = this->bgfx_surface;
    if (surface == nullptr)
        return;
    surface->closeHudPopup();
    surface->setKeyboardFocusWidget(nullptr);
    surface->setNavigationHandlers(
        [this](QKeyEvent *event)
        {
            if (event->type() == QEvent::KeyPress && this->bgfx_fullscreen_active
                && MapNavigationWidget::triggerButtonShortcut(this->map_menu, event))
            {
                event->accept();
                return true;
            }
            return this->map->handleKeyPressEvent(event);
        },
        [this](QKeyEvent *event) { return this->map->handleKeyReleaseEvent(event); },
        [this]() { this->map->cancelNavigationInput(); });
    surface->setNavigationMouseHandlers(
        [this](QMouseEvent *event) { return QCoreApplication::sendEvent(this->map, event); },
        [this](QMouseEvent *event) { return QCoreApplication::sendEvent(this->map, event); },
        [this](QMouseEvent *event) { return QCoreApplication::sendEvent(this->map, event); });
    surface->setNavigationWheelHandler(
        [this](QWheelEvent *event) { this->map->handleWheelEvent(event); });
    surface->setNavigationFrameHandler(
        [this](double elapsed_seconds) { this->map->advanceNavigationFrame(elapsed_seconds); });
    surface->setHudActionHandler([this](const MapBgfxHud::Command &command)
    {
        switch (command.action)
        {
            case MapBgfxHud::Action::GlobeSlider:
            case MapBgfxHud::Action::GlobeSliderReset:
            case MapBgfxHud::Action::CompassDrag:
            case MapBgfxHud::Action::CompassWheel:
                this->bgfx_globe_hud->handleCommand(command);
                break;
            case MapBgfxHud::Action::JoystickDrag:
                this->map->beginJoystickPan(command.position);
                break;
            default:
                handleBgfxHudAction(static_cast<int>(command.action), command.group,
                                    command.value, command.flipped);
                break;
        }
    });
    surface->setOverlayBuilder(MapBgfxSurface::OverlayBuilder());
    // The 3D Monitor uses exactly the Editor's full-colour pixmaps and
    // terrain-aware globe projection. The 2D Monitor still draws its own
    // GPU-atlas symbols; no 2D overlay is installed.
    if (this->monitor_globe_icon_overlay == nullptr)
    {
        const std::shared_ptr<MapBgfxEditorIcons> icons =
            std::make_shared<MapBgfxEditorIcons>();
        this->monitor_globe_icon_overlay = std::make_shared<MapBgfxEditorOverlay>();
        this->monitor_globe_icon_overlay->setIconProvider(
            [icons](MapBgfxHudTextCache *cache, InfrastructureEntity entity,
                    const QString &path, int width, MapBgfxEditorOverlay::Highlight highlight)
            {
                return icons->icon(cache, entity, path, width, highlight);
            });
    }
    surface->setGlobeOverlayBuilder(
        [this](MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *cache,
               const MapBgfxSurface::GlobeProjector &projector,
               const MapBgfxSurface::GlobeElevatedProjector &elevated_projector,
               const QSizeF &viewport,
               const MapBgfxGlobeIconIndex::Frustum &frustum)
        {
            const NetworkRenderSnapshot *snapshot =
                this->hydraulic_data != nullptr && !this->basemap_only
                    ? &this->hydraulic_data->networkRenderSnapshot() : nullptr;
            const double zoom = this->map_model->view2dContinuousZoom();
            const double base_width = std::clamp(
                10.0 + (zoom - 16.0) * 10.0, 10.0, 40.0);
            const int width = qMax(1, qRound(base_width
                * this->monitor_globe_icon_size_percent / 100.0));
            this->monitor_globe_icon_overlay->buildGlobe(
                canvas, cache, MapEditorViewportRenderState(), projector,
                snapshot, width, viewport, frustum, nullptr, nullptr, elevated_projector);
        });
    // Suppress only the generic globe icons, not the 2D symbols or the
    // monitor's network colours, legend, simulation values and flow arrows.
    surface->setGlobeFlatEntityIconsEnabled(true);
    surface->setLabelSymbolRects(MapBgfxLabels::SymbolRect(), MapBgfxLabels::SymbolRect());
    surface->setNodeDeclutteringEnabled(this->monitor_node_decluttering);
    surface->setGlobe3dIconsEnabled(MapWorkspace::useGlobe3dIconModels(
        MapWorkspace::DistributionMonitor, this->monitor_globe_3d_icons));
    surface->setBackgroundOpacity(this->network_background_opacity);
    surface->setLabelSettings(this->map_menu->mapNavigationWidget()->labelSettings());
    surface->setNetworkScreenTranslation(QPointF());
    surface->setHiddenEntityUuids(QSet<QUuid>());
    surface->setSelectedEntity(this->monitor_selected_type, this->monitor_selected_uuid);
    applyMonitorDiagnostics();
    surface->setHudFullscreen(this->bgfx_fullscreen_active);
    applySymbology();
    refreshBgfxLegend();
    applyMapHoverCursor();
    surface->focusNavigationInput();
}

MapWidget *MapMonitorContainer::getMap()
{
    return this->map;
}
MapModeToolPanel *MapMonitorContainer::toolPanel() const
{
    return this->tool_panel;
}

QWidget *MapMonitorContainer::mapViewport() const
{
    return this->map_stack;
}

MapNavigationWidget *MapMonitorContainer::mapNavigationWidget()
{
    return this->map_menu->mapNavigationWidget();
}





MapMonitorMenuWidget::MapMonitorMenuWidget(MapWidget *map, QWidget *parent)
    : QWidget{parent},
    layout( new QVBoxLayout(this) ),
    map( map ),
    map_nav( new MapNavigationWidget(this->map, CanvasMode::Monitor, this->map, this) )
{
    setContentsMargins(0, 0, 0, 0);
    setMinimumWidth(Sizes::SidebarLeftWidth);
    setMaximumWidth(Sizes::SidebarLeftWidth);
    
    this->layout->addWidget(this->map_nav);
    
    addGroupVisualSettings();
    addGroupNodeVisuals();
    addGroupLinkVisuals();
    addGroupHeatmapVisuals();

    // The legend already uses the active unit profile; keep the symbology
    // choices consistent without touching canonical symbology values.
    const std::function<void()> refresh_unit_labels = [this] {
        const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
        const std::function<QString(const char *, std::string_view)> label =
            [&context](const char *name, std::string_view quantity) {
            const QString unit = UnitTablePresentation::unitSymbol(
                UnitTablePresentation::selectedUnit(context, quantity));
            return QStringLiteral("%1 [%2]").arg(LiveUiText::translate(name), unit);
        };
        const std::function<void(QButtonGroup *, int, int, int, int)> update_group =
            [&label](QButtonGroup *group, int water_age, int chlorine,
                     int river_water, int lake_water) {
            if (group == nullptr) return;
            const std::function<void(int, const char *, std::string_view)> set_label =
                [group, &label](int id, const char *name, std::string_view quantity) {
                QAbstractButton *button = group->button(id);
                if (button != nullptr) button->setText(label(name, quantity));
            };
            set_label(water_age, QT_TRANSLATE_NOOP("LiveUiText", "Water Age"), AOWIS_QUANTITY("water_age"));
            set_label(chlorine, "Cl₂", AOWIS_QUANTITY("chemical_mass_concentration"));
            set_label(river_water, QT_TRANSLATE_NOOP("LiveUiText", "River Water"), AOWIS_QUANTITY("source_trace_percentage"));
            set_label(lake_water, QT_TRANSLATE_NOOP("LiveUiText", "Lake Water"), AOWIS_QUANTITY("source_trace_percentage"));
        };
        update_group(this->node_visual_group,
                     static_cast<int>(VisualNode::WaterAge), static_cast<int>(VisualNode::Chlorine),
                     static_cast<int>(VisualNode::RiverWater), static_cast<int>(VisualNode::LakeWater));
        update_group(this->link_visual_group,
                     static_cast<int>(VisualLink::WaterAge), static_cast<int>(VisualLink::Chlorine),
                     static_cast<int>(VisualLink::RiverWater), static_cast<int>(VisualLink::LakeWater));
        update_group(this->heatmap_visual_group,
                     static_cast<int>(VisualHeatmap::WaterAge), static_cast<int>(VisualHeatmap::Chlorine),
                     static_cast<int>(VisualHeatmap::RiverWater), static_cast<int>(VisualHeatmap::LakeWater));
    };

    LiveUiText::bind(this, refresh_unit_labels);
    new UnitAwareView(this, UnitProfileManager::instance().presentation(), refresh_unit_labels);
    this->layout->addStretch();
}

MapNavigationWidget *MapMonitorMenuWidget::mapNavigationWidget()
{
    return this->map_nav;
}

void MapMonitorMenuWidget::setNodeVisualSelection(VisualNode visual_node)
{
    if (this->node_visual_group == nullptr)
        return;

    QAbstractButton *button = this->node_visual_group->button(static_cast<int>(visual_node));
    if (button != nullptr)
        button->setChecked(true);
}

void MapMonitorMenuWidget::setLinkVisualSelection(VisualLink visual_link)
{
    if (this->link_visual_group == nullptr)
        return;

    QAbstractButton *button = this->link_visual_group->button(static_cast<int>(visual_link));
    if (button != nullptr)
        button->setChecked(true);
}

void MapMonitorMenuWidget::setHeatmapVisualSelection(VisualHeatmap visual_heatmap)
{
    if (this->heatmap_visual_group == nullptr)
        return;

    QAbstractButton *button = this->heatmap_visual_group->button(static_cast<int>(visual_heatmap));
    if (button != nullptr)
        button->setChecked(true);
}

void MapMonitorMenuWidget::addGroupVisualSettings()
{
    GroupBoxCollapsible *group = new GroupBoxCollapsible(this);
    LiveUiText::bind(group, [group] { group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Visual Settings"))); });
    this->layout->addWidget(group);

    QVBoxLayout *vbox = new QVBoxLayout();
    group->setLayout(vbox);

    QLabel *label_node_section = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "<b>Node</b>"), nullptr);
    QLabel *label_node_size = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Junction Size"), nullptr);
    QComboBox *combo_node_size_unit = new QComboBox(this);
    combo_node_size_unit->addItem(QStringLiteral("m"),
        static_cast<int>(NetworkSymbologySizeUnit::Meters));
    combo_node_size_unit->addItem(QStringLiteral("px"),
        static_cast<int>(NetworkSymbologySizeUnit::Pixels));
    combo_node_size_unit->setCurrentIndex(1);
    LiveUiText::bind(combo_node_size_unit, [combo_node_size_unit] {
        combo_node_size_unit->setToolTip(QCoreApplication::translate("MapMonitorMenuWidget",
            "Meters keep a true world-space junction size. Pixels keep a stable visual size while preserving 3D perspective."));
    });
    JunctionSizeSlider *slider_node_size = new JunctionSizeSlider(4, 64,
        NetworkSymbologyDefaultNodeSizePx,
        QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets the junction marker/orb diameter in screen pixels.")),
        QStringLiteral(" px"), this);
    QCheckBox *check_node_decluttering = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Declutter Overlapping Nodes"), this);
    check_node_decluttering->setChecked(true);
    LiveUiText::bind(check_node_decluttering, [check_node_decluttering] {
        check_node_decluttering->setToolTip(QCoreApplication::translate("MapMonitorMenuWidget",
            "Separates exact or near-exact node coordinates by at least 1 m for rendering and picking. "
            "Disable this to render nodes at their exact geographic positions."));
    });
    connect(check_node_decluttering, &QCheckBox::toggled, this,
        &MapMonitorMenuWidget::signalNodeDeclutteringChanged);
    connect(slider_node_size, &QSlider::valueChanged, this,
        [this, slider_node_size](int)
    {
        const int raw_value = slider_node_size->junctionSizeRaw();
        if (raw_value == 0)
        {
            emit signalNodeSizeChanged(this->node_size_unit, 0.0);
            return;
        }

        if (this->node_size_unit == NetworkSymbologySizeUnit::Meters)
        {
            this->node_size_m = raw_value * 0.1;
            emit signalNodeSizeChanged(this->node_size_unit, this->node_size_m);
        }
        else
        {
            this->node_size_px = raw_value;
            emit signalNodeSizeChanged(this->node_size_unit, this->node_size_px);
        }
    });
    connect(combo_node_size_unit, &QComboBox::currentIndexChanged, this,
        [this, combo_node_size_unit, slider_node_size](int index)
    {
        const NetworkSymbologySizeUnit unit = static_cast<NetworkSymbologySizeUnit>(
            combo_node_size_unit->itemData(index).toInt());
        if (unit == this->node_size_unit)
            return;

        const bool junctions_visible = slider_node_size->junctionSizeRaw() > 0;
        this->node_size_unit = unit;
        if (unit == NetworkSymbologySizeUnit::Meters)
        {
            slider_node_size->setConfiguration(
                qRound(NetworkSymbologyMinimumNodeSizeM * 10.0),
                qRound(NetworkSymbologyMaximumNodeSizeM * 10.0),
                qRound(NetworkSymbologyDefaultNodeSizeM * 10.0),
                junctions_visible ? qRound(this->node_size_m * 10.0) : 0,
                QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets the true world-space junction diameter.")),
                QStringLiteral(" m"), 0.1, 1);
        }
        else
        {
            slider_node_size->setConfiguration(4, 64,
                NetworkSymbologyDefaultNodeSizePx,
                junctions_visible ? this->node_size_px : 0,
                QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets the junction marker/orb diameter in screen pixels.")),
                QStringLiteral(" px"));
        }
        emit signalNodeSizeUnitChanged(unit);
    });

    QLabel *label_link_section = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "<b>Link</b>"), nullptr);
    QLabel *label_link_thickness = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Pipe Thickness"), nullptr);
    QComboBox *combo_link_thickness_unit = new QComboBox(this);
    combo_link_thickness_unit->addItem(QStringLiteral("m"),
        static_cast<int>(NetworkSymbologySizeUnit::Meters));
    combo_link_thickness_unit->addItem(QStringLiteral("px"),
        static_cast<int>(NetworkSymbologySizeUnit::Pixels));
    combo_link_thickness_unit->setCurrentIndex(1);
    LiveUiText::bind(combo_link_thickness_unit, [combo_link_thickness_unit] {
        combo_link_thickness_unit->setToolTip(QCoreApplication::translate("MapMonitorMenuWidget", "Meters keep a true world-space pipe thickness. Pixels keep a constant screen-space thickness."));
    });
    SymbologySlider *slider_link_thickness = new SymbologySlider(1, 24,
        NetworkSymbologyDefaultLinkThicknessPx,
        QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets rendered pipe thickness and keeps pipe hit detection aligned with it.")),
        QStringLiteral(" px"), this);
    connect(slider_link_thickness, &QSlider::valueChanged, this,
        [this](int raw_value)
    {
        if (this->link_thickness_unit == NetworkSymbologySizeUnit::Meters)
        {
            this->link_thickness_m = raw_value * 0.01;
            emit signalLinkThicknessChanged(this->link_thickness_unit, this->link_thickness_m);
        }
        else
        {
            this->link_thickness_px = raw_value;
            emit signalLinkThicknessChanged(this->link_thickness_unit, this->link_thickness_px);
        }
    });
    connect(combo_link_thickness_unit, &QComboBox::currentIndexChanged, this,
        [this, combo_link_thickness_unit, slider_link_thickness](int index)
    {
        const NetworkSymbologySizeUnit unit = static_cast<NetworkSymbologySizeUnit>(
            combo_link_thickness_unit->itemData(index).toInt());
        if (unit == this->link_thickness_unit)
            return;

        this->link_thickness_unit = unit;
        if (unit == NetworkSymbologySizeUnit::Meters)
        {
            slider_link_thickness->setConfiguration(
                qRound(NetworkSymbologyMinimumLinkThicknessM * 100.0),
                qRound(NetworkSymbologyMaximumLinkThicknessM * 100.0),
                qRound(NetworkSymbologyDefaultLinkThicknessM * 100.0),
                qRound(this->link_thickness_m * 100.0),
                QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets the true world-space pipe thickness.")),
                QStringLiteral(" m"), 0.01, 2);
        }
        else
        {
            slider_link_thickness->setConfiguration(1, 24,
                NetworkSymbologyDefaultLinkThicknessPx, this->link_thickness_px,
                QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets rendered pipe thickness and keeps pipe hit detection aligned with it.")),
                QStringLiteral(" px"));
        }
        emit signalLinkThicknessUnitChanged(unit);
    });

    QLabel *label_flow_direction_size = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Flow Arrows [px]"), nullptr);
    FlowDirectionSizeSlider *slider_flow_direction_size = new FlowDirectionSizeSlider(this);
    connect(slider_flow_direction_size, &QSlider::valueChanged, this,
        [this, slider_flow_direction_size](int)
    {
        emit signalFlowDirectionSizeChanged(slider_flow_direction_size->arrowSizePx());
    });

    QLabel *label_heatmap_section = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "<b>Heatmap</b>"), nullptr);
    QLabel *label_heatmap_radius = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Radius"), nullptr);
    QComboBox *combo_heatmap_radius_unit = new QComboBox(this);
    combo_heatmap_radius_unit->addItem(QStringLiteral("m"),
        static_cast<int>(HeatmapRadiusUnit::Meters));
    combo_heatmap_radius_unit->addItem(QStringLiteral("px"),
        static_cast<int>(HeatmapRadiusUnit::Pixels));
    LiveUiText::bind(combo_heatmap_radius_unit, [combo_heatmap_radius_unit] {
        combo_heatmap_radius_unit->setToolTip(QCoreApplication::translate("MapMonitorMenuWidget",
            "Meters keep the heatmap radius geographic as the map zoom changes. "
            "Pixels keep a constant on-screen radius."));
    });

    SymbologySlider *slider_heatmap_radius = new SymbologySlider(10, 1000, 400,
        QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets each heatmap point's geographic influence radius. It remains consistent in meters across zoom levels.")),
        QStringLiteral(" m"), this);
    connect(slider_heatmap_radius, &QSlider::valueChanged, this,
        [this](int radius)
    {
        if (this->heatmap_radius_unit == HeatmapRadiusUnit::Pixels)
            this->heatmap_radius_px = radius;
        else
            this->heatmap_radius_m = radius;
        emit signalHeatmapRadiusChanged(this->heatmap_radius_unit, radius);
    });
    connect(combo_heatmap_radius_unit, &QComboBox::currentIndexChanged, this,
        [this, combo_heatmap_radius_unit, slider_heatmap_radius](int index)
    {
        const HeatmapRadiusUnit unit = static_cast<HeatmapRadiusUnit>(
            combo_heatmap_radius_unit->itemData(index).toInt());
        if (unit == this->heatmap_radius_unit)
            return;

        this->heatmap_radius_unit = unit;
        if (unit == HeatmapRadiusUnit::Pixels)
        {
            slider_heatmap_radius->setConfiguration(5, 250, 50, this->heatmap_radius_px,
                QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets each heatmap point's on-screen influence radius. It remains constant in pixels across zoom levels.")),
                QStringLiteral(" px"));
        }
        else
        {
            slider_heatmap_radius->setConfiguration(10, 1000, 400, this->heatmap_radius_m,
                QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets each heatmap point's geographic influence radius. It remains consistent in meters across zoom levels.")),
                QStringLiteral(" m"));
        }
        emit signalHeatmapRadiusUnitChanged(unit);
    });

    QLabel *label_heatmap_solid_center = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Solid Center [%]"), nullptr);
    QSlider *slider_heatmap_solid_center = new SymbologySlider(0, 100, 70,
        QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Sets how much of the heatmap radius keeps full local opacity before fading to transparency.")), QStringLiteral(" %"), this);
    connect(slider_heatmap_solid_center, &QSlider::valueChanged, this, &MapMonitorMenuWidget::signalHeatmapSolidCenterChanged);

    QLabel *label_heatmap_opacity = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Opacity [%]"), nullptr);
    QSlider *slider_heatmap_opacity = new SymbologySlider(0, 100, 75,
        QString::fromUtf8(QT_TRANSLATE_NOOP("MapMonitorMenuWidget", "Controls the opacity of the complete heatmap layer.")), QStringLiteral(" %"), this);
    connect(slider_heatmap_opacity, &QSlider::valueChanged, this, &MapMonitorMenuWidget::signalHeatmapOpacityChanged);

    vbox->addWidget(label_node_section);
    QHBoxLayout *node_size_header = new QHBoxLayout();
    node_size_header->addWidget(label_node_size);
    node_size_header->addStretch();
    node_size_header->addWidget(combo_node_size_unit);
    vbox->addLayout(node_size_header);
    vbox->addWidget(slider_node_size);
    vbox->addWidget(check_node_decluttering);
    vbox->addSpacing(4);
    vbox->addWidget(label_link_section);
    QHBoxLayout *link_thickness_header = new QHBoxLayout();
    link_thickness_header->addWidget(label_link_thickness);
    link_thickness_header->addStretch();
    link_thickness_header->addWidget(combo_link_thickness_unit);
    vbox->addLayout(link_thickness_header);
    vbox->addWidget(slider_link_thickness);
    vbox->addWidget(label_flow_direction_size);
    vbox->addWidget(slider_flow_direction_size);
    vbox->addSpacing(4);
    vbox->addWidget(label_heatmap_section);
    QHBoxLayout *heatmap_radius_header = new QHBoxLayout();
    heatmap_radius_header->addWidget(label_heatmap_radius);
    heatmap_radius_header->addStretch();
    heatmap_radius_header->addWidget(combo_heatmap_radius_unit);
    vbox->addLayout(heatmap_radius_header);
    vbox->addWidget(slider_heatmap_radius);
    vbox->addWidget(label_heatmap_solid_center);
    vbox->addWidget(slider_heatmap_solid_center);
    vbox->addWidget(label_heatmap_opacity);
    vbox->addWidget(slider_heatmap_opacity);
}

void MapMonitorMenuWidget::addGroupNodeVisuals()
{
    GroupBoxCollapsible *group = new GroupBoxCollapsible(this);
    LiveUiText::bind(group, [group] { group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Node Symbology"))); });
    this->layout->addWidget(group);
    QVBoxLayout *vbox = new QVBoxLayout();
    group->setLayout(vbox);
    
    QRadioButton *radio_node_none = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "None"), nullptr);
    radio_node_none->setChecked(true);
    QRadioButton *radio_node_elevation = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Elevation"), nullptr);
    QRadioButton *radio_node_basedemand = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Base Demand"), nullptr);
    QRadioButton *radio_node_totaldemand = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Total Demand"), nullptr);
    QRadioButton *radio_node_demanddeficit = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Demand Deficit"), nullptr);
    QRadioButton *radio_node_emitterflow = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Emitter Flow"), nullptr);
    QRadioButton *radio_node_leakage = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Leakage"), nullptr);
    QRadioButton *radio_node_head = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Head"), nullptr);
    QRadioButton *radio_node_pressure = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Pressure"), nullptr);
    QRadioButton *radio_node_water_age = new QRadioButton();
    
    QLabel *label_multispecies = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "EPANET Network 3 has<br>"
        "Multi-Species Water<br>"
        "Quality Analysis (MSX).<br>"
        "Modeled variables are:"), nullptr);
    
    QRadioButton *radio_node_chlorine = new QRadioButton("Cl₂");
    QRadioButton *radio_node_river = new QRadioButton();
    QRadioButton *radio_node_lake = new QRadioButton();

    this->node_visual_group = new QButtonGroup(this);
    this->node_visual_group->addButton(radio_node_none, static_cast<int>(VisualNode::None));
    this->node_visual_group->addButton(radio_node_elevation, static_cast<int>(VisualNode::Elevation));
    this->node_visual_group->addButton(radio_node_basedemand, static_cast<int>(VisualNode::BaseDemand));
    this->node_visual_group->addButton(radio_node_totaldemand, static_cast<int>(VisualNode::TotalDemand));
    this->node_visual_group->addButton(radio_node_demanddeficit, static_cast<int>(VisualNode::DemandDeficit));
    this->node_visual_group->addButton(radio_node_emitterflow, static_cast<int>(VisualNode::EmitterFlow));
    this->node_visual_group->addButton(radio_node_leakage, static_cast<int>(VisualNode::Leakage));
    this->node_visual_group->addButton(radio_node_head, static_cast<int>(VisualNode::Head));
    this->node_visual_group->addButton(radio_node_pressure, static_cast<int>(VisualNode::Pressure));
    this->node_visual_group->addButton(radio_node_water_age, static_cast<int>(VisualNode::WaterAge));
    this->node_visual_group->addButton(radio_node_chlorine, static_cast<int>(VisualNode::Chlorine));
    this->node_visual_group->addButton(radio_node_river, static_cast<int>(VisualNode::RiverWater));
    this->node_visual_group->addButton(radio_node_lake, static_cast<int>(VisualNode::LakeWater));
    
    QLabel *label_chlorine = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Chlorine decay constant<br>"
        "depends on the mix of<br>"
        "Lake and River water."), nullptr);
    
    const std::function<void(QRadioButton *, VisualNode)> connect_node_visual =
        [this](QRadioButton *button, VisualNode visual)
    {
        connect(button, &QRadioButton::clicked, this, [this, visual]
        {
            emit signalNodeVisualClicked(visual);
        });
    };
    connect_node_visual(radio_node_none, VisualNode::None);
    connect_node_visual(radio_node_elevation, VisualNode::Elevation);
    connect_node_visual(radio_node_basedemand, VisualNode::BaseDemand);
    connect_node_visual(radio_node_totaldemand, VisualNode::TotalDemand);
    connect_node_visual(radio_node_demanddeficit, VisualNode::DemandDeficit);
    connect_node_visual(radio_node_emitterflow, VisualNode::EmitterFlow);
    connect_node_visual(radio_node_leakage, VisualNode::Leakage);
    connect_node_visual(radio_node_head, VisualNode::Head);
    connect_node_visual(radio_node_pressure, VisualNode::Pressure);
    connect_node_visual(radio_node_water_age, VisualNode::WaterAge);
    connect_node_visual(radio_node_chlorine, VisualNode::Chlorine);
    connect_node_visual(radio_node_river, VisualNode::RiverWater);
    connect_node_visual(radio_node_lake, VisualNode::LakeWater);
    
    vbox->addWidget(radio_node_none);
    vbox->addWidget(radio_node_elevation);
    vbox->addWidget(radio_node_basedemand);
    vbox->addWidget(radio_node_totaldemand);
    vbox->addWidget(radio_node_demanddeficit);
    vbox->addWidget(radio_node_emitterflow);
    vbox->addWidget(radio_node_leakage);
    vbox->addWidget(radio_node_head);
    vbox->addWidget(radio_node_pressure);
    vbox->addWidget(radio_node_water_age);

    vbox->addWidget(label_multispecies);
    
    vbox->addWidget(radio_node_chlorine);
    vbox->addWidget(radio_node_river);
    vbox->addWidget(radio_node_lake);
    
    vbox->addWidget(label_chlorine);
    group->setCollapsed(false);
}
void MapMonitorMenuWidget::addGroupLinkVisuals()
{
    GroupBoxCollapsible *group = new GroupBoxCollapsible(this);
    LiveUiText::bind(group, [group] { group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Link Symbology"))); });
    this->layout->addWidget(group);
    QVBoxLayout *vbox = new QVBoxLayout();
    group->setLayout(vbox);
    
    QRadioButton *radio_link_none = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "None"), nullptr);
    radio_link_none->setChecked(true);
    QRadioButton *radio_link_diameter = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Diameter"), nullptr);
    QRadioButton *radio_link_length = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Length"), nullptr);
    QRadioButton *radio_link_roughness = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Roughness"), nullptr);
    QRadioButton *radio_link_flowrate = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Flow Rate"), nullptr);
    QRadioButton *radio_link_velocity = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Velocity"), nullptr);
    QRadioButton *radio_link_headloss = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Head Loss"), nullptr);
    QRadioButton *radio_link_leakage = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Leakage"), nullptr);
    QRadioButton *radio_link_water_age = new QRadioButton();
    QRadioButton *radio_link_chlorine = new QRadioButton("Cl₂");
    QRadioButton *radio_link_river = new QRadioButton();
    QRadioButton *radio_link_lake = new QRadioButton();

    this->link_visual_group = new QButtonGroup(this);
    this->link_visual_group->addButton(radio_link_none, static_cast<int>(VisualLink::None));
    this->link_visual_group->addButton(radio_link_diameter, static_cast<int>(VisualLink::Diameter));
    this->link_visual_group->addButton(radio_link_length, static_cast<int>(VisualLink::Length));
    this->link_visual_group->addButton(radio_link_roughness, static_cast<int>(VisualLink::Roughness));
    this->link_visual_group->addButton(radio_link_flowrate, static_cast<int>(VisualLink::FlowRate));
    this->link_visual_group->addButton(radio_link_velocity, static_cast<int>(VisualLink::Velocity));
    this->link_visual_group->addButton(radio_link_headloss, static_cast<int>(VisualLink::HeadLoss));
    this->link_visual_group->addButton(radio_link_leakage, static_cast<int>(VisualLink::Leakage));
    this->link_visual_group->addButton(radio_link_water_age, static_cast<int>(VisualLink::WaterAge));
    this->link_visual_group->addButton(radio_link_chlorine, static_cast<int>(VisualLink::Chlorine));
    this->link_visual_group->addButton(radio_link_river, static_cast<int>(VisualLink::RiverWater));
    this->link_visual_group->addButton(radio_link_lake, static_cast<int>(VisualLink::LakeWater));
    
    const std::function<void(QRadioButton *, VisualLink)> connect_link_visual =
        [this](QRadioButton *button, VisualLink visual)
    {
        connect(button, &QRadioButton::clicked, this, [this, visual]
        {
            emit signalLinkVisualClicked(visual);
        });
    };
    connect_link_visual(radio_link_none, VisualLink::None);
    connect_link_visual(radio_link_diameter, VisualLink::Diameter);
    connect_link_visual(radio_link_length, VisualLink::Length);
    connect_link_visual(radio_link_roughness, VisualLink::Roughness);
    connect_link_visual(radio_link_flowrate, VisualLink::FlowRate);
    connect_link_visual(radio_link_velocity, VisualLink::Velocity);
    connect_link_visual(radio_link_headloss, VisualLink::HeadLoss);
    connect_link_visual(radio_link_leakage, VisualLink::Leakage);
    connect_link_visual(radio_link_water_age, VisualLink::WaterAge);
    connect_link_visual(radio_link_chlorine, VisualLink::Chlorine);
    connect_link_visual(radio_link_river, VisualLink::RiverWater);
    connect_link_visual(radio_link_lake, VisualLink::LakeWater);
    
    vbox->addWidget(radio_link_none);
    vbox->addWidget(radio_link_diameter);
    vbox->addWidget(radio_link_length);
    vbox->addWidget(radio_link_roughness);
    vbox->addWidget(radio_link_flowrate);
    vbox->addWidget(radio_link_velocity);
    vbox->addWidget(radio_link_headloss);
    vbox->addWidget(radio_link_leakage);
    vbox->addWidget(radio_link_water_age);
    vbox->addWidget(radio_link_chlorine);
    vbox->addWidget(radio_link_river);
    vbox->addWidget(radio_link_lake);
    group->setCollapsed(false);
}

void MapMonitorMenuWidget::addGroupHeatmapVisuals()
{
    GroupBoxCollapsible *group = new GroupBoxCollapsible(this);
    LiveUiText::bind(group, [group] { group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Heatmap"))); });
    this->layout->addWidget(group);
    
    QVBoxLayout *vbox = new QVBoxLayout();
    group->setLayout(vbox);
    
    QRadioButton *radio_none = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "None"), nullptr);
    radio_none->setChecked(true);
    
    QRadioButton *radio_elevation = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Elevation"), nullptr);
    QRadioButton *radio_base_demand = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Base Demand"), nullptr);
    QRadioButton *radio_total_demand = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Total Demand"), nullptr);
    QRadioButton *radio_demand_deficit = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Demand Deficit"), nullptr);
    QRadioButton *radio_emitter_flow = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Emitter Flow"), nullptr);
    QRadioButton *radio_leakage = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Leakage"), nullptr);
    QRadioButton *radio_head = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Head"), nullptr);
    QRadioButton *radio_pressure = LiveUiText::caption<QRadioButton>(QT_TRANSLATE_NOOP("LiveUiText", "Pressure"), nullptr);
    QRadioButton *radio_water_age = new QRadioButton();
    QRadioButton *radio_chlorine = new QRadioButton("Cl₂");
    QRadioButton *radio_river = new QRadioButton();
    QRadioButton *radio_lake = new QRadioButton();

    this->heatmap_visual_group = new QButtonGroup(this);
    this->heatmap_visual_group->addButton(radio_none, static_cast<int>(VisualHeatmap::None));
    this->heatmap_visual_group->addButton(radio_elevation, static_cast<int>(VisualHeatmap::Elevation));
    this->heatmap_visual_group->addButton(radio_base_demand, static_cast<int>(VisualHeatmap::BaseDemand));
    this->heatmap_visual_group->addButton(radio_total_demand, static_cast<int>(VisualHeatmap::TotalDemand));
    this->heatmap_visual_group->addButton(radio_demand_deficit, static_cast<int>(VisualHeatmap::DemandDeficit));
    this->heatmap_visual_group->addButton(radio_emitter_flow, static_cast<int>(VisualHeatmap::EmitterFlow));
    this->heatmap_visual_group->addButton(radio_leakage, static_cast<int>(VisualHeatmap::Leakage));
    this->heatmap_visual_group->addButton(radio_head, static_cast<int>(VisualHeatmap::Head));
    this->heatmap_visual_group->addButton(radio_pressure, static_cast<int>(VisualHeatmap::Pressure));
    this->heatmap_visual_group->addButton(radio_water_age, static_cast<int>(VisualHeatmap::WaterAge));
    this->heatmap_visual_group->addButton(radio_chlorine, static_cast<int>(VisualHeatmap::Chlorine));
    this->heatmap_visual_group->addButton(radio_river, static_cast<int>(VisualHeatmap::RiverWater));
    this->heatmap_visual_group->addButton(radio_lake, static_cast<int>(VisualHeatmap::LakeWater));
    
    const std::function<void(QRadioButton *, VisualHeatmap)> connect_heatmap_visual =
        [this](QRadioButton *button, VisualHeatmap visual)
    {
        connect(button, &QRadioButton::clicked, this, [this, visual]
        {
            emit signalHeatmapVisualClicked(visual);
        });
    };
    
    connect_heatmap_visual(radio_none, VisualHeatmap::None);
    connect_heatmap_visual(radio_elevation, VisualHeatmap::Elevation);
    connect_heatmap_visual(radio_base_demand, VisualHeatmap::BaseDemand);
    connect_heatmap_visual(radio_total_demand, VisualHeatmap::TotalDemand);
    connect_heatmap_visual(radio_demand_deficit, VisualHeatmap::DemandDeficit);
    connect_heatmap_visual(radio_emitter_flow, VisualHeatmap::EmitterFlow);
    connect_heatmap_visual(radio_leakage, VisualHeatmap::Leakage);
    connect_heatmap_visual(radio_head, VisualHeatmap::Head);
    connect_heatmap_visual(radio_pressure, VisualHeatmap::Pressure);
    connect_heatmap_visual(radio_water_age, VisualHeatmap::WaterAge);
    connect_heatmap_visual(radio_chlorine, VisualHeatmap::Chlorine);
    connect_heatmap_visual(radio_river, VisualHeatmap::RiverWater);
    connect_heatmap_visual(radio_lake, VisualHeatmap::LakeWater);

    vbox->addWidget(radio_none);
    vbox->addWidget(radio_elevation);
    vbox->addWidget(radio_base_demand);
    vbox->addWidget(radio_total_demand);
    vbox->addWidget(radio_demand_deficit);
    vbox->addWidget(radio_emitter_flow);
    vbox->addWidget(radio_leakage);
    vbox->addWidget(radio_head);
    vbox->addWidget(radio_pressure);
    vbox->addWidget(radio_water_age);
    vbox->addWidget(radio_chlorine);
    vbox->addWidget(radio_river);
    vbox->addWidget(radio_lake);
    group->setCollapsed(false);
}
