#include "map/bgfx/map_bgfx_globe_hud_controller.h"

#include "config/unit_profile_manager.h"
#include "map/core/map_model.h"
#include "map/core/map_scale_distance.h"
#include "map/monitor/map_hud_length_units.h"

#include <QCoreApplication>
#include <QEasingCurve>
#include <QPoint>
#include <QVariantAnimation>
#include <QtMath>

#include <algorithm>
#include <cmath>

// The constants and mappings of map_monitor_hud_controls.cpp.
namespace
{
constexpr double CompassWheelStepDeg = 5.0;
constexpr int NorthAnimationDurationMs = 280;
constexpr int SliderResetAnimationDurationMs = 240;
constexpr int CompassWheelRotateEndDelayMs = 350;
constexpr int CameraDistanceSliderSteps = 1000;
constexpr int NetworkGroundOffsetSliderSteps = static_cast<int>(MapModel::MaxView3dNetworkGroundOffsetM * 10.0);
constexpr int VerticalExaggerationSliderScale = 10;
// The widest scale bar the HUD's scale panel draws (as its 2D scale).
constexpr double ScaleMaximumBarWidthPx = MapBgfxHud::ScaleWidthPx - 2.0 * MapBgfxHud::ScalePaddingPx;

int tiltMinimumDeg()
{
    return qRound(MapModel::MinViewGlobePitchDeg);
}

int tiltMaximumDeg()
{
    return qRound(MapModel::MaxViewGlobePitchDeg);
}

int exaggerationMinimumStep()
{
    return qRound(MapModel::MinView3dVerticalExaggeration * VerticalExaggerationSliderScale);
}

int exaggerationMaximumStep()
{
    return qRound(MapModel::MaxView3dVerticalExaggeration * VerticalExaggerationSliderScale);
}

// Logarithmic distance slider (cameraDistanceSliderValue / Meters).
int distanceSliderValue(double distance_m)
{
    const double minimum_m = MapModel::MinViewGlobeDistanceM;
    const double maximum_m = std::max(minimum_m, MapModel::MaxViewGlobeDistanceM);
    if (maximum_m <= minimum_m)
        return 0;
    const double bounded_m = std::clamp(distance_m, minimum_m, maximum_m);
    const double ratio = (std::log(bounded_m) - std::log(minimum_m)) / (std::log(maximum_m) - std::log(minimum_m));
    return qRound(std::clamp(ratio, 0.0, 1.0) * CameraDistanceSliderSteps);
}

double distanceMeters(int slider_value)
{
    const double minimum_m = MapModel::MinViewGlobeDistanceM;
    const double maximum_m = std::max(minimum_m, MapModel::MaxViewGlobeDistanceM);
    const double ratio = std::clamp(slider_value, 0, CameraDistanceSliderSteps) / double(CameraDistanceSliderSteps);
    return std::exp(std::log(minimum_m) + ratio * (std::log(maximum_m) - std::log(minimum_m)));
}

// Integer slider position from a 0..1 fraction of [minimum, maximum].
int sliderStep(double fraction, int minimum, int maximum)
{
    return qRound(double(minimum) + std::clamp(fraction, 0.0, 1.0) * double(maximum - minimum));
}

double stepFraction(double step, int minimum, int maximum)
{
    return maximum > minimum ? std::clamp((step - double(minimum)) / double(maximum - minimum), 0.0, 1.0) : 0.0;
}

// Tilt and distance drags hold a rotate interaction (QWidget HUD).
bool sliderRotates(int index)
{
    return index == MapBgfxHud::TiltSlider || index == MapBgfxHud::DistanceSlider;
}
}

MapBgfxGlobeHudController::MapBgfxGlobeHudController(MapModel *map_model, QObject *parent)
    : QObject(parent),
      map_model(map_model),
      north_animation(new QVariantAnimation(this))
{
    Q_ASSERT(this->map_model != nullptr);
    // Slider resets: animate the model value back to its default.
    for (int index = 0; index < MapBgfxHud::GlobeSliderCount; ++index)
    {
        QVariantAnimation *animation = new QVariantAnimation(this);
        animation->setDuration(SliderResetAnimationDurationMs);
        animation->setEasingCurve(QEasingCurve::InOutCubic);
        connect(animation, &QVariantAnimation::valueChanged, this, [this, index](const QVariant &value)
        {
            const double number = value.toDouble();
            switch (index)
            {
                case MapBgfxHud::NetworkOffsetSlider:
                    this->map_model->setView3dNetworkGroundOffsetM(number);
                    break;
                case MapBgfxHud::TiltSlider:
                    this->map_model->setViewGlobePitchDeg(number);
                    break;
                case MapBgfxHud::DistanceSlider:
                    this->map_model->setViewGlobeDistanceM(number);
                    break;
                default:
                    this->map_model->setView3dVerticalExaggeration(number);
                    break;
            }
        });
        if (sliderRotates(index))
        {
            connect(animation, &QVariantAnimation::stateChanged, this,
                    [this](QAbstractAnimation::State new_state, QAbstractAnimation::State old_state)
            {
                if (old_state == QAbstractAnimation::Stopped && new_state != QAbstractAnimation::Stopped)
                    this->map_model->beginView3dRotateInteraction();
                else if (old_state != QAbstractAnimation::Stopped && new_state == QAbstractAnimation::Stopped)
                    this->map_model->endView3dRotateInteraction();
            });
        }
        this->reset_animations[std::size_t(index)] = animation;
    }

    // North: the remaining yaw in progress steps, inside a rotate
    // interaction, snapped to exactly 0 at the end (MapCompassWidget).
    this->north_animation->setDuration(NorthAnimationDurationMs);
    this->north_animation->setStartValue(0.0);
    this->north_animation->setEndValue(1.0);
    this->north_animation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(this->north_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value)
    {
        const double progress = value.toDouble();
        const double step = progress - this->north_last_progress;
        this->north_last_progress = progress;
        if (globeShown())
            this->map_model->orbitViewGlobe(this->north_delta_yaw_deg * step, 0.0);
    });
    connect(this->north_animation, &QVariantAnimation::stateChanged, this,
            [this](QAbstractAnimation::State new_state, QAbstractAnimation::State old_state)
    {
        if (!globeShown())
            return;
        if (old_state == QAbstractAnimation::Stopped && new_state != QAbstractAnimation::Stopped)
            this->map_model->beginView3dRotateInteraction();
        else if (old_state != QAbstractAnimation::Stopped && new_state == QAbstractAnimation::Stopped)
            this->map_model->endView3dRotateInteraction();
    });
    connect(this->north_animation, &QAbstractAnimation::finished, this, [this]()
    {
        if (globeShown())
            this->map_model->setViewGlobeYawDeg(0.0);
    });

    this->wheel_end_timer.setSingleShot(true);
    this->wheel_end_timer.setInterval(CompassWheelRotateEndDelayMs);
    connect(&this->wheel_end_timer, &QTimer::timeout, this, [this]() { finishWheelRotation(); });

    connect(this->map_model, &MapModel::viewModeChanged, this, [this](MapViewMode) { stopEverything(); });
}

MapBgfxGlobeHudController::~MapBgfxGlobeHudController()
{
    finishWheelRotation();
}

bool MapBgfxGlobeHudController::globeShown() const
{
    return this->map_model->viewMode() == MapViewMode::Globe;
}

void MapBgfxGlobeHudController::handleCommand(const MapBgfxHud::Command &command)
{
    if (!globeShown())
        return;
    switch (command.action)
    {
        case MapBgfxHud::Action::GlobeSlider:
        {
            const int index = command.group;
            if (index < 0 || index >= MapBgfxHud::GlobeSliderCount)
                return;
            if (command.phase == MapBgfxHud::Phase::Begin)
            {
                // A press stops the slider's reset animation.
                this->reset_animations[std::size_t(index)]->stop();
                this->dragged_slider = index;
                if (sliderRotates(index))
                    this->map_model->beginView3dRotateInteraction();
            }
            setSliderFraction(index, command.number);
            if (command.phase == MapBgfxHud::Phase::End && this->dragged_slider == index)
            {
                this->dragged_slider = -1;
                if (sliderRotates(index))
                    this->map_model->endView3dRotateInteraction();
            }
            break;
        }
        case MapBgfxHud::Action::GlobeSliderReset:
            if (command.group >= 0 && command.group < MapBgfxHud::GlobeSliderCount)
                resetSlider(command.group);
            break;
        case MapBgfxHud::Action::CompassDrag:
            if (command.phase == MapBgfxHud::Phase::Begin)
                compassPressed();
            else if (command.phase == MapBgfxHud::Phase::Move)
                compassDragged(command.number);
            else if (command.phase == MapBgfxHud::Phase::End)
                compassReleased(command.value != 0);
            break;
        case MapBgfxHud::Action::CompassWheel:
            compassWheel(command.number);
            break;
        default:
            break;
    }
}

void MapBgfxGlobeHudController::setSliderFraction(int index, double fraction)
{
    switch (index)
    {
        case MapBgfxHud::NetworkOffsetSlider:
            this->map_model->setView3dNetworkGroundOffsetM(
                double(sliderStep(fraction, 0, NetworkGroundOffsetSliderSteps)) / 10.0);
            break;
        case MapBgfxHud::TiltSlider:
            this->map_model->setViewGlobePitchDeg(double(sliderStep(fraction, tiltMinimumDeg(), tiltMaximumDeg())));
            break;
        case MapBgfxHud::DistanceSlider:
            this->map_model->setViewGlobeDistanceM(distanceMeters(sliderStep(fraction, 0, CameraDistanceSliderSteps)));
            break;
        default:
            this->map_model->setView3dVerticalExaggeration(
                double(sliderStep(fraction, exaggerationMinimumStep(), exaggerationMaximumStep()))
                / VerticalExaggerationSliderScale);
            break;
    }
}

void MapBgfxGlobeHudController::resetSlider(int index)
{
    QVariantAnimation *animation = this->reset_animations[std::size_t(index)];
    animation->stop();
    switch (index)
    {
        case MapBgfxHud::NetworkOffsetSlider:
            animation->setStartValue(this->map_model->view3dNetworkGroundOffsetM());
            animation->setEndValue(MapModel::MinView3dNetworkGroundOffsetM);
            break;
        case MapBgfxHud::TiltSlider:
            animation->setStartValue(this->map_model->viewGlobePitchDeg());
            animation->setEndValue(MapModel::DefaultViewGlobePitchDeg);
            break;
        case MapBgfxHud::DistanceSlider:
            animation->setStartValue(this->map_model->viewGlobeDistanceM());
            animation->setEndValue(MapModel::DefaultViewGlobeDistanceM);
            break;
        default:
            animation->setStartValue(this->map_model->view3dVerticalExaggeration());
            animation->setEndValue(MapModel::DefaultView3dVerticalExaggeration);
            break;
    }
    animation->start();
}

void MapBgfxGlobeHudController::compassPressed()
{
    stopNorthAnimation();
    finishWheelRotation();
    this->map_model->beginView3dRotateInteraction();
    this->compass_drag_active = true;
    this->compass_remainder_x = 0.0;
}

void MapBgfxGlobeHudController::compassDragged(double delta_x)
{
    if (!this->compass_drag_active)
        return;
    // Whole pixels, as the QWidget compass's pointer deltas.
    this->compass_remainder_x += delta_x;
    const int pixels = int(this->compass_remainder_x);
    if (pixels == 0)
        return;
    this->compass_remainder_x -= double(pixels);
    this->map_model->orbitViewGlobeByPointerDelta(QPoint(pixels, 0), false);
}

void MapBgfxGlobeHudController::compassReleased(bool clicked)
{
    if (!this->compass_drag_active)
        return;
    this->compass_drag_active = false;
    this->map_model->endView3dRotateInteraction();
    if (clicked)
        animateNorth();
}

void MapBgfxGlobeHudController::compassWheel(double notches)
{
    stopNorthAnimation();
    if (notches == 0.0)
        return;
    if (!this->wheel_rotate_active)
    {
        this->map_model->beginView3dRotateInteraction();
        this->wheel_rotate_active = true;
    }
    this->map_model->orbitViewGlobe(notches * CompassWheelStepDeg, 0.0);
    this->wheel_end_timer.start();
}

void MapBgfxGlobeHudController::animateNorth()
{
    stopNorthAnimation();
    this->north_delta_yaw_deg = std::remainder(-this->map_model->viewGlobeYawDeg(), 360.0);
    if (std::abs(this->north_delta_yaw_deg) < 0.01)
    {
        this->map_model->setViewGlobeYawDeg(0.0);
        return;
    }
    this->north_last_progress = 0.0;
    this->north_animation->start();
}

void MapBgfxGlobeHudController::stopNorthAnimation()
{
    if (this->north_animation->state() != QAbstractAnimation::Stopped)
        this->north_animation->stop();
}

void MapBgfxGlobeHudController::finishWheelRotation()
{
    if (!this->wheel_rotate_active)
        return;
    this->wheel_end_timer.stop();
    this->wheel_rotate_active = false;
    this->map_model->endView3dRotateInteraction();
}

void MapBgfxGlobeHudController::stopEverything()
{
    stopNorthAnimation();
    finishWheelRotation();
    for (QVariantAnimation *animation : this->reset_animations)
        animation->stop();
    if (this->compass_drag_active)
    {
        this->compass_drag_active = false;
        this->map_model->endView3dRotateInteraction();
    }
    if (this->dragged_slider >= 0)
    {
        if (sliderRotates(this->dragged_slider))
            this->map_model->endView3dRotateInteraction();
        this->dragged_slider = -1;
    }
}

void MapBgfxGlobeHudController::fillHudState(const MapModel &map_model, const QSize &viewport,
                                             MapBgfxHud::Globe *globe)
{
    const UnitPresentationContext &units = UnitProfileManager::instance().presentation();
    globe->yaw_deg = map_model.viewGlobeYawDeg();

    MapBgfxHud::GlobeSliderState &offset = globe->sliders[MapBgfxHud::NetworkOffsetSlider];
    const double offset_m = map_model.view3dNetworkGroundOffsetM();
    offset.fraction = stepFraction(double(qRound(offset_m * 10.0)), 0, NetworkGroundOffsetSliderSteps);
    offset.maximum_label = MapHudLengthUnits::networkOffset(MapModel::MaxView3dNetworkGroundOffsetM, units);
    offset.minimum_label = MapHudLengthUnits::networkOffset(MapModel::MinView3dNetworkGroundOffsetM, units);
    offset.value_label = MapHudLengthUnits::networkOffset(offset_m, units);
    offset.tooltip = QCoreApplication::translate("MapBgfxGlobeHudController",
                                  "Water network height above its real model elevation\n"
                                    "%1 = true elevation\n"
                                    "Up to %2 render-only lift to avoid terrain z-fighting\n"
                                    "Right-click: animate back to %1")
                         .arg(offset.minimum_label, offset.maximum_label);

    MapBgfxHud::GlobeSliderState &tilt = globe->sliders[MapBgfxHud::TiltSlider];
    const int pitch_deg = qRound(map_model.viewGlobePitchDeg());
    tilt.fraction = stepFraction(double(pitch_deg), tiltMinimumDeg(), tiltMaximumDeg());
    tilt.maximum_label = QStringLiteral("%1°").arg(tiltMaximumDeg());
    tilt.minimum_label = QStringLiteral("%1°").arg(tiltMinimumDeg());
    tilt.value_label = QStringLiteral("%1°").arg(pitch_deg);
    tilt.tooltip = QCoreApplication::translate("MapBgfxGlobeHudController",
                                "Camera tilt\n%1° = near horizon\n%2° = straight down\n"
                                  "Right-click: animate back to the default tilt")
                       .arg(tiltMinimumDeg())
                       .arg(tiltMaximumDeg());

    MapBgfxHud::GlobeSliderState &distance = globe->sliders[MapBgfxHud::DistanceSlider];
    const double distance_m = map_model.viewGlobeDistanceM();
    distance.fraction = double(distanceSliderValue(distance_m)) / double(CameraDistanceSliderSteps);
    distance.maximum_label = MapHudLengthUnits::cameraDistance(MapModel::MaxViewGlobeDistanceM, units);
    distance.minimum_label = MapHudLengthUnits::cameraDistance(MapModel::MinViewGlobeDistanceM, units);
    distance.value_label = MapHudLengthUnits::cameraDistance(distance_m, units);
    distance.tooltip = QCoreApplication::translate("MapBgfxGlobeHudController",
                                    "Distance to focus\n"
                                      "Straight-line orbit radius to the globe point under the crosshair\n"
                                      "The logarithmic slider spans the full Globe zoom range\n"
                                      "Right-click: animate back to the full-globe default distance");

    MapBgfxHud::GlobeSliderState &exaggeration = globe->sliders[MapBgfxHud::ExaggerationSlider];
    const double factor = map_model.view3dVerticalExaggeration();
    exaggeration.fraction = stepFraction(double(qRound(factor * VerticalExaggerationSliderScale)),
                                         exaggerationMinimumStep(), exaggerationMaximumStep());
    exaggeration.maximum_label = QStringLiteral("%1×").arg(MapModel::MaxView3dVerticalExaggeration, 0, 'f', 0);
    exaggeration.minimum_label = QStringLiteral("%1×").arg(MapModel::MinView3dVerticalExaggeration, 0, 'f', 1);
    exaggeration.value_label = QStringLiteral("%1×").arg(factor, 0, 'f', 1);
    exaggeration.tooltip = QCoreApplication::translate("MapBgfxGlobeHudController",
                                        "Vertical exaggeration\n"
                                          "Scales terrain relief and water-network elevation differences together\n"
                                          "1.0× = true vertical scale\n"
                                          "The separate network-height control remains a render-only lift\n"
                                          "Right-click: animate back to 1.0×");

    // Globe scale at the focus distance (MapMonitorScaleHudWidget).
    globe->scale = MapBgfxHud::ScaleBar();
    const double orbit_distance_m = std::max(MapModel::MinViewGlobeDistanceM, distance_m);
    const double pitch_rad = qDegreesToRadians(
        std::clamp(map_model.viewGlobePitchDeg(), MapModel::MinViewGlobePitchDeg, MapModel::MaxViewGlobePitchDeg));
    const double horizontal_m = orbit_distance_m * std::cos(pitch_rad);
    const double vertical_m = orbit_distance_m * std::sin(pitch_rad) + map_model.viewGlobeCameraCollisionLiftM();
    const double focus_distance_m = std::hypot(horizontal_m, vertical_m);
    const double meters_per_pixel = 2.0 * focus_distance_m
        * std::tan(qDegreesToRadians(MapModel::GlobeFieldOfViewDeg / 2.0)) / double(std::max(1, viewport.height()));
    const double maximum_distance_m = meters_per_pixel * ScaleMaximumBarWidthPx;
    const MapScaleDistance::Scale scale = MapScaleDistance::forMaximumDistance(maximum_distance_m, units);
    if (maximum_distance_m > 0.0 && scale.distance_m > 0.0 && std::isfinite(maximum_distance_m))
    {
        globe->scale.valid = true;
        globe->scale.width_px = ScaleMaximumBarWidthPx * scale.distance_m / maximum_distance_m;
        globe->scale.label = scale.label;
    }
}
