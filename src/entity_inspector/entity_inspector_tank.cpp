#include "entity_inspector/entity_inspector_text.h"
#include <aowis/model/units/quantity.h>
#include "entity_inspector/entity_inspector_tank.h"

#include "config/unit_aware_view.h"
#include "config/unit_bulk_reaction_display.h"
#include "config/unit_numeric_spin_box.h"
#include "config/unit_profile_manager.h"

#include <QAbstractSpinBox>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QtMath>

namespace
{
constexpr int volume_curve_uuid_role = Qt::UserRole;
// The bindings own all unit-dependent editor state (range, decimals, step and
// suffix). Presentation-only styling must not overwrite the active profile.
constexpr double geometry_spin_minimum = -1000000.0;
constexpr double geometry_spin_maximum = 1000000.0;

void configureTankSpinAppearance(QDoubleSpinBox *spin)
{
    spin->setAlignment(Qt::AlignRight);
}

std::optional<double> equivalentDiameter(double area_m2)
{
    if (!qIsFinite(area_m2) || area_m2 < 0.0)
        return std::nullopt;

    return qSqrt(4.0 * area_m2 / M_PI);
}
}

EntityInspectorTank::EntityInspectorTank(HydraulicData *hydraulic_data, const QUuid &uuid, QWidget *parent)
    : EntityInspectorWidget(hydraulic_data, parent),
      hydraulic_data(hydraulic_data),
      tank_uuid(uuid)
{
    addGroupOverviewImage(":/icon/tower_large.png", QString());
    addGroupGeneral(QString());
    addGroupPosition();
    bindHydraulicNode(InfrastructureEntity::Tank, uuid, "Tank");
    addGroupElevation();
    addGroupGeometry();
    bindGeometry();
    // Treat the always-present initial-level editor as the geometry panel's
    // visibility boundary. Collapsed panels do not receive unit refresh work;
    // on expansion all three level editors catch up in a single pass.
    new UnitAwareView(this->spin_level_initial, UnitProfileManager::instance().presentation(),
        [this] { refreshGeometry(); });
    addGroupSimulation();
    addGroupWaterQualitySimulation();
    addGroupQuality();
    addGroupAlerts();
    addGroupHistory();
    addStretches();
}

void EntityInspectorTank::addGroupGeometry()
{
    this->group_geometry = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Geometry / Levels"));
    QGridLayout *grid = new QGridLayout(this->group_geometry);

    connect(this->group_geometry, &GroupBoxCollapsible::signalExpanded, this, &EntityInspectorTank::onGroupExpand);

    // The numerical widgets are disposable presentations of the precise
    // canonical metre values. Construction and unit refresh never write them
    // back to the hydraulic Model.
    const auto level_editor = [this](int field) -> QDoubleSpinBox * {
        return new UnitNumericSpinBox(
            [this, field]() -> std::optional<double> {
                if (!this->hydraulic_data) return std::nullopt;
                const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
                if (!tank) return std::nullopt;
                if (field == 0) return tank->water_level_initial_m;
                if (field == 1) return tank->water_level_minimum_m;
                return tank->water_level_maximum_m;
            },
            [this, field](double metres) {
                if (!this->hydraulic_data || !this->hydraulic_data->tank(this->tank_uuid))
                    return false;
                if (field == 0) return this->hydraulic_data->setTankWaterLevelInitialM(this->tank_uuid, metres);
                if (field == 1) return this->hydraulic_data->setTankWaterLevelMinimumM(this->tank_uuid, metres);
                return this->hydraulic_data->setTankWaterLevelMaximumM(this->tank_uuid, metres);
            }, field == 0 ? AOWIS_QUANTITY("tank_initial_level")
                : field == 1 ? AOWIS_QUANTITY("tank_minimum_level") : AOWIS_QUANTITY("tank_maximum_level"),
            "m", -1000000.0, 1000000.0, 0.1, 3, this->group_geometry);
    };

    QLabel *label_level_initial = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Initial Level"), nullptr);
    this->spin_level_initial = level_editor(0);
    configureTankSpinAppearance(this->spin_level_initial);

    QLabel *label_level_minimum = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Minimum Level"), nullptr);
    this->spin_level_minimum = level_editor(1);
    configureTankSpinAppearance(this->spin_level_minimum);

    QLabel *label_level_maximum = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Maximum Level"), nullptr);
    this->spin_level_maximum = level_editor(2);
    configureTankSpinAppearance(this->spin_level_maximum);

    this->check_overflow = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Overflow Allowed"), nullptr);

    QLabel *label_geometry_type = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Geometry Input"), nullptr);
    this->combo_geometry_type = new QComboBox();
    EntityInspectorText::addChoice(this->combo_geometry_type, QT_TRANSLATE_NOOP("LiveUiText", "Cylindrical"), static_cast<int>(HydraulicNodeTankGeometryInputType::Cylindrical));
    EntityInspectorText::addChoice(this->combo_geometry_type, QT_TRANSLATE_NOOP("LiveUiText", "Uniform Area"), static_cast<int>(HydraulicNodeTankGeometryInputType::UniformArea));
    EntityInspectorText::addChoice(this->combo_geometry_type, QT_TRANSLATE_NOOP("LiveUiText", "Maximum Volume"), static_cast<int>(HydraulicNodeTankGeometryInputType::VolumeAtMaximumLevel));
    EntityInspectorText::addChoice(this->combo_geometry_type, QT_TRANSLATE_NOOP("LiveUiText", "Volume Curve"), static_cast<int>(HydraulicNodeTankGeometryInputType::VolumeCurve));

    // The same four controls can be input editors or calculated read-only
    // results, depending on the geometry mode. In either case the Model values
    // remain canonical and no control refresh may write rounded values back.
    const auto geometry_editor = [this](int field) -> QDoubleSpinBox * {
        const std::string quantity = field == 0 ? AOWIS_QUANTITY("tank_diameter")
            : field == 1 ? AOWIS_QUANTITY("area") : field == 2 ? AOWIS_QUANTITY("tank_minimum_volume") : AOWIS_QUANTITY("volume");
        const std::string storage_unit = field == 0 ? "m" : field == 1 ? "m2" : "m3";
        const double step = field == 0 ? 0.1 : 1.0;
        return new UnitNumericSpinBox(
            [this, field]() -> std::optional<double> {
                if (!this->hydraulic_data) return std::nullopt;
                const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
                if (!tank) return std::nullopt;
                if (field == 0) return tank->diameter_m;
                if (field == 1) return tank->cross_section_area_m2;
                if (field == 2) return tank->minimum_volume_m3;
                return tank->volume_at_maximum_level_m3;
            },
            [this, field](double canonical_value) {
                if (!this->hydraulic_data) return false;
                const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
                if (!tank) return false;
                const HydraulicNodeTankGeometryInputType mode = tank->geometry_input_type;
                // A stale callback cannot overwrite a derived property after
                // another action changes the geometry input mode.
                if (field == 0 && mode != HydraulicNodeTankGeometryInputType::Cylindrical) return false;
                if (field == 1 && mode != HydraulicNodeTankGeometryInputType::UniformArea) return false;
                if (field == 2 && mode == HydraulicNodeTankGeometryInputType::VolumeCurve) return false;
                if (field == 3 && mode != HydraulicNodeTankGeometryInputType::VolumeAtMaximumLevel) return false;
                if (field == 0) return this->hydraulic_data->setTankDiameterM(this->tank_uuid, canonical_value);
                if (field == 1) return this->hydraulic_data->setTankCrossSectionAreaM2(this->tank_uuid, canonical_value);
                if (field == 2) return this->hydraulic_data->setTankMinimumVolumeM3(this->tank_uuid, canonical_value);
                return this->hydraulic_data->setTankVolumeAtMaximumLevelM3(this->tank_uuid, canonical_value);
            }, quantity, storage_unit, geometry_spin_minimum, geometry_spin_maximum,
            step, 3, this->group_geometry);
    };

    this->label_diameter = new QLabel();
    EntityInspectorText::setCaption(this->label_diameter, QT_TRANSLATE_NOOP("LiveUiText", "Diameter"));
    this->spin_diameter = geometry_editor(0);
    configureTankSpinAppearance(this->spin_diameter);

    this->label_area = new QLabel();
    EntityInspectorText::setCaption(this->label_area, QT_TRANSLATE_NOOP("LiveUiText", "Cross-Section Area"));
    this->label_area->setWordWrap(true);
    this->spin_area = geometry_editor(1);
    configureTankSpinAppearance(this->spin_area);

    this->label_volume_minimum = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Minimum Volume"), nullptr);
    this->label_volume_minimum->setWordWrap(true);
    this->spin_volume_minimum = geometry_editor(2);
    configureTankSpinAppearance(this->spin_volume_minimum);

    this->label_volume_maximum = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Maximum Volume"), nullptr);
    this->label_volume_maximum->setWordWrap(true);
    this->spin_volume_maximum = geometry_editor(3);
    configureTankSpinAppearance(this->spin_volume_maximum);

    this->label_volume_curve = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Volume Curve"), nullptr);
    this->combo_volume_curve = new QComboBox();
    constrainComboWidth(this->combo_volume_curve);

    grid->addWidget(label_level_initial, 0, 0);
    grid->addWidget(this->spin_level_initial, 0, 1);
    grid->addWidget(label_level_minimum, 1, 0);
    grid->addWidget(this->spin_level_minimum, 1, 1);
    grid->addWidget(label_level_maximum, 2, 0);
    grid->addWidget(this->spin_level_maximum, 2, 1);
    grid->addWidget(this->check_overflow, 3, 0, 1, 2);
    grid->addWidget(label_geometry_type, 4, 0);
    grid->addWidget(this->combo_geometry_type, 4, 1);
    grid->addWidget(this->label_diameter, 5, 0);
    grid->addWidget(this->spin_diameter, 5, 1);
    grid->addWidget(this->label_area, 6, 0);
    grid->addWidget(this->spin_area, 6, 1);
    grid->addWidget(this->label_volume_minimum, 7, 0);
    grid->addWidget(this->spin_volume_minimum, 7, 1);
    grid->addWidget(this->label_volume_maximum, 8, 0);
    grid->addWidget(this->spin_volume_maximum, 8, 1);
    grid->addWidget(this->label_volume_curve, 9, 0);
    grid->addWidget(this->combo_volume_curve, 9, 1);

    layoutConfiguration()->addWidget(this->group_geometry);
}

void EntityInspectorTank::bindGeometry()
{
    connect(this->check_overflow, &QCheckBox::toggled, this, [this](bool can_overflow)
    {
        this->hydraulic_data->setTankCanOverflow(this->tank_uuid, can_overflow);
    });
    connect(this->combo_geometry_type, &QComboBox::currentIndexChanged, this, &EntityInspectorTank::onGeometryTypeChanged);
    connect(this->combo_volume_curve, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const QUuid curve_uuid = this->combo_volume_curve->currentData(volume_curve_uuid_role).toUuid();
        this->hydraulic_data->setTankVolumeCurveUuid(this->tank_uuid, curve_uuid);
    });
    connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this, [this](InfrastructureEntity entity_type, const QUuid &uuid)
    {
        if (entity_type == InfrastructureEntity::Tank && uuid == this->tank_uuid)
            refreshGeometry();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, &EntityInspectorTank::refreshGeometry);

    refreshGeometry();
}

void EntityInspectorTank::refreshGeometry()
{
    if (!this->hydraulic_data || this->tank_uuid.isNull())
        return;

    const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
    if (!tank.has_value())
        return;

    const QSignalBlocker initial_level_blocker(this->spin_level_initial);
    const QSignalBlocker minimum_level_blocker(this->spin_level_minimum);
    const QSignalBlocker maximum_level_blocker(this->spin_level_maximum);
    const QSignalBlocker overflow_blocker(this->check_overflow);
    const QSignalBlocker geometry_type_blocker(this->combo_geometry_type);
    const QSignalBlocker diameter_blocker(this->spin_diameter);
    const QSignalBlocker area_blocker(this->spin_area);
    const QSignalBlocker minimum_volume_blocker(this->spin_volume_minimum);
    const QSignalBlocker maximum_volume_blocker(this->spin_volume_maximum);

    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    static_cast<UnitNumericSpinBox *>(this->spin_level_initial)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_level_minimum)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_level_maximum)->refreshUnits(context);
    this->check_overflow->setChecked(tank->can_overflow);

    const int geometry_index = this->combo_geometry_type->findData(
        static_cast<int>(tank->geometry_input_type));
    this->combo_geometry_type->setCurrentIndex(geometry_index >= 0 ? geometry_index : 0);

    static_cast<UnitNumericSpinBox *>(this->spin_diameter)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_area)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_volume_minimum)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_volume_maximum)->refreshUnits(context);

    populateVolumeCurveCombo(tank->volume_curve_uuid);
    updateGeometryModeUi(tank->geometry_input_type);
    updateDerivedGeometry(tank.value());
}

void EntityInspectorTank::populateVolumeCurveCombo(const QUuid &volume_curve_uuid)
{
    const QList<HydraulicCurveTankVolume> &curves =
        this->hydraulic_data->networkHydraulic().curves_tank_volume;

    QString signature = volume_curve_uuid.toString(QUuid::WithoutBraces);
    signature += QLatin1Char('\n');
    for (const HydraulicCurveTankVolume &curve : curves)
    {
        signature += curve.uuid.toString(QUuid::WithoutBraces);
        signature += QLatin1Char(':');
        signature += curve.id;
        signature += QLatin1Char('\n');
    }

    const QSignalBlocker curve_blocker(this->combo_volume_curve);
    if (this->combo_volume_curve->property("aowisVolumeCurveSignature").toString() != signature)
    {
        this->combo_volume_curve->clear();
        this->combo_volume_curve->addItem("[Select Curve]");
        this->combo_volume_curve->setItemData(
            this->combo_volume_curve->count() - 1, QUuid(), volume_curve_uuid_role);

        bool selected_curve_exists = volume_curve_uuid.isNull();
        for (const HydraulicCurveTankVolume &curve : curves)
        {
            const QString curve_name = curve.id.isEmpty()
                ? curve.uuid.toString(QUuid::WithoutBraces)
                : curve.id;
            this->combo_volume_curve->addItem(curve_name);
            this->combo_volume_curve->setItemData(
                this->combo_volume_curve->count() - 1, curve.uuid, volume_curve_uuid_role);
            if (curve.uuid == volume_curve_uuid)
                selected_curve_exists = true;
        }

        if (!volume_curve_uuid.isNull() && !selected_curve_exists)
        {
            this->combo_volume_curve->addItem(
                QStringLiteral("[Missing Curve] %1").arg(
                    volume_curve_uuid.toString(QUuid::WithoutBraces)));
            this->combo_volume_curve->setItemData(
                this->combo_volume_curve->count() - 1,
                volume_curve_uuid, volume_curve_uuid_role);
        }

        this->combo_volume_curve->setProperty("aowisVolumeCurveSignature", signature);
    }

    int selected_index = 0;
    for (int index = 0; index < this->combo_volume_curve->count(); index++)
    {
        if (this->combo_volume_curve->itemData(index, volume_curve_uuid_role).toUuid() ==
            volume_curve_uuid)
        {
            selected_index = index;
            break;
        }
    }
    this->combo_volume_curve->setCurrentIndex(selected_index);
}

void EntityInspectorTank::updateGeometryModeUi(
    HydraulicNodeTankGeometryInputType geometry_input_type)
{
    const bool is_cylindrical =
        geometry_input_type == HydraulicNodeTankGeometryInputType::Cylindrical;
    const bool is_uniform_area =
        geometry_input_type == HydraulicNodeTankGeometryInputType::UniformArea;
    const bool is_maximum_volume =
        geometry_input_type == HydraulicNodeTankGeometryInputType::VolumeAtMaximumLevel;
    const bool is_volume_curve =
        geometry_input_type == HydraulicNodeTankGeometryInputType::VolumeCurve;
    const bool group_expanded = !this->group_geometry->isCollapsed();

    this->label_diameter->setVisible(group_expanded && !is_volume_curve);
    this->spin_diameter->setVisible(group_expanded && !is_volume_curve);
    this->label_area->setVisible(group_expanded && !is_volume_curve);
    this->spin_area->setVisible(group_expanded && !is_volume_curve);
    this->label_volume_minimum->setVisible(group_expanded);
    this->spin_volume_minimum->setVisible(group_expanded);
    this->label_volume_maximum->setVisible(group_expanded);
    this->spin_volume_maximum->setVisible(group_expanded);
    this->label_volume_curve->setVisible(group_expanded && is_volume_curve);
    this->combo_volume_curve->setVisible(group_expanded && is_volume_curve);

    EntityInspectorText::setCaption(this->label_diameter, is_cylindrical ? QT_TRANSLATE_NOOP("LiveUiText", "Diameter") : QT_TRANSLATE_NOOP("LiveUiText", "Equivalent<br>Diameter"));
    EntityInspectorText::setCaption(this->label_area, is_uniform_area ? QT_TRANSLATE_NOOP("LiveUiText", "Cross-Section Area") : QT_TRANSLATE_NOOP("LiveUiText", "Derived Cross-Section Area"));

    if (is_cylindrical)
    {
        setSpinEditable(this->spin_diameter, "Stored cylindrical tank diameter.");
        setSpinDerived(this->spin_area, std::nullopt);
        setSpinEditable(this->spin_volume_minimum,
                        "A positive value is the explicit volume at minimum level. "
                        "Zero lets EPANET derive it from area × minimum level.");
        setSpinDerived(this->spin_volume_maximum, std::nullopt);
    }
    else if (is_uniform_area)
    {
        setSpinDerived(this->spin_diameter, std::nullopt);
        setSpinEditable(this->spin_area, "Stored uniform cross-sectional tank area.");
        setSpinEditable(this->spin_volume_minimum,
                        "A positive value is the explicit volume at minimum level. "
                        "Zero lets EPANET derive it from area × minimum level.");
        setSpinDerived(this->spin_volume_maximum, std::nullopt);
    }
    else if (is_maximum_volume)
    {
        setSpinDerived(this->spin_diameter, std::nullopt);
        setSpinDerived(this->spin_area, std::nullopt);
        setSpinEditable(this->spin_volume_minimum,
                        "A positive value is the explicit volume at minimum level. "
                        "Zero derives geometry from maximum volume and maximum level.");
        setSpinEditable(this->spin_volume_maximum, "Stored tank volume at the maximum water level.");
    }
    else if (is_volume_curve)
    {
        setSpinDerived(this->spin_volume_minimum, std::nullopt);
        setSpinDerived(this->spin_volume_maximum, std::nullopt);
    }
}

void EntityInspectorTank::updateDerivedGeometry(const HydraulicNodeTank &tank)
{
    const double usable_height_m = tank.water_level_maximum_m - tank.water_level_minimum_m;

    switch (tank.geometry_input_type)
    {
    case HydraulicNodeTankGeometryInputType::Cylindrical:
    {
        std::optional<double> area_m2;
        std::optional<double> maximum_volume_m3;
        if (qIsFinite(tank.diameter_m) && tank.diameter_m >= 0.0 &&
            qIsFinite(tank.minimum_volume_m3) && tank.minimum_volume_m3 >= 0.0)
        {
            area_m2 = M_PI * tank.diameter_m * tank.diameter_m / 4.0;
            const double effective_minimum_volume_m3 = tank.minimum_volume_m3 > 0.0
                ? tank.minimum_volume_m3
                : area_m2.value() * tank.water_level_minimum_m;
            const double derived_volume_m3 =
                effective_minimum_volume_m3 + area_m2.value() * usable_height_m;
            if (qIsFinite(derived_volume_m3))
                maximum_volume_m3 = derived_volume_m3;
        }
        setSpinDerived(this->spin_area, area_m2, "Calculated from the cylindrical diameter.");
        setSpinDerived(this->spin_volume_maximum, maximum_volume_m3,
                       "Calculated using EPANET minimum-volume semantics and the maximum level.");
        break;
    }
    case HydraulicNodeTankGeometryInputType::UniformArea:
    {
        const std::optional<double> diameter_m = equivalentDiameter(tank.cross_section_area_m2);
        std::optional<double> maximum_volume_m3;
        if (qIsFinite(tank.cross_section_area_m2) && tank.cross_section_area_m2 >= 0.0 &&
            qIsFinite(tank.minimum_volume_m3) && tank.minimum_volume_m3 >= 0.0)
        {
            const double effective_minimum_volume_m3 = tank.minimum_volume_m3 > 0.0
                ? tank.minimum_volume_m3
                : tank.cross_section_area_m2 * tank.water_level_minimum_m;
            const double derived_volume_m3 =
                effective_minimum_volume_m3 + tank.cross_section_area_m2 * usable_height_m;
            if (qIsFinite(derived_volume_m3))
                maximum_volume_m3 = derived_volume_m3;
        }
        setSpinDerived(this->spin_diameter, diameter_m,
                       "Equivalent cylindrical diameter calculated from the stored area.");
        setSpinDerived(this->spin_volume_maximum, maximum_volume_m3,
                       "Calculated using EPANET minimum-volume semantics and the maximum level.");
        break;
    }
    case HydraulicNodeTankGeometryInputType::VolumeAtMaximumLevel:
    {
        std::optional<double> area_m2;
        double reference_height_m = 0.0;
        double reference_volume_m3 = 0.0;
        if (tank.minimum_volume_m3 > 0.0)
        {
            reference_height_m = usable_height_m;
            reference_volume_m3 =
                tank.volume_at_maximum_level_m3 - tank.minimum_volume_m3;
        }
        else if (tank.minimum_volume_m3 == 0.0)
        {
            reference_height_m = tank.water_level_maximum_m;
            reference_volume_m3 = tank.volume_at_maximum_level_m3;
        }

        if (qIsFinite(reference_height_m) && !qFuzzyIsNull(reference_height_m) &&
            qIsFinite(reference_volume_m3))
        {
            const double derived_area_m2 = reference_volume_m3 / reference_height_m;
            if (qIsFinite(derived_area_m2))
                area_m2 = derived_area_m2;
        }
        setSpinDerived(this->spin_area, area_m2,
                       "Calculated from the minimum/maximum volumes and levels.");
        setSpinDerived(this->spin_diameter,
                       area_m2.has_value() ? equivalentDiameter(area_m2.value()) : std::nullopt,
                       "Equivalent cylindrical diameter calculated from the derived area.");
        break;
    }
    case HydraulicNodeTankGeometryInputType::VolumeCurve:
    {
        const std::optional<double> minimum_volume_m3 =
            volumeCurveVolumeAtLevel(tank.volume_curve_uuid, tank.water_level_minimum_m);
        const std::optional<double> maximum_volume_m3 =
            volumeCurveVolumeAtLevel(tank.volume_curve_uuid, tank.water_level_maximum_m);
        setSpinDerived(this->spin_volume_minimum, minimum_volume_m3,
                       "Interpolated from the selected volume curve at the minimum level.");
        setSpinDerived(this->spin_volume_maximum, maximum_volume_m3,
                       "Interpolated from the selected volume curve at the maximum level.");
        break;
    }
    }
}

std::optional<double> EntityInspectorTank::volumeCurveVolumeAtLevel(
    const QUuid &curve_uuid, double water_level_m) const
{
    if (curve_uuid.isNull() || !qIsFinite(water_level_m))
        return std::nullopt;

    const QList<HydraulicCurveTankVolume> &curves =
        this->hydraulic_data->networkHydraulic().curves_tank_volume;
    const HydraulicCurveTankVolume *selected_curve = nullptr;
    for (const HydraulicCurveTankVolume &curve : curves)
    {
        if (curve.uuid == curve_uuid)
        {
            selected_curve = &curve;
            break;
        }
    }
    if (!selected_curve || selected_curve->points.size() < 2)
        return std::nullopt;

    const QList<HydraulicCurveTankVolumePoint> &points = selected_curve->points;
    for (int index = 0; index < points.size(); index++)
    {
        const HydraulicCurveTankVolumePoint &point = points.at(index);
        if (!qIsFinite(point.water_level_m) || !qIsFinite(point.volume_m3))
            return std::nullopt;
        if (index > 0)
        {
            const HydraulicCurveTankVolumePoint &previous = points.at(index - 1);
            if (point.water_level_m <= previous.water_level_m ||
                point.volume_m3 <= previous.volume_m3)
                return std::nullopt;
        }
        if (qFuzzyCompare(1.0 + point.water_level_m, 1.0 + water_level_m))
            return point.volume_m3;
    }

    if (water_level_m < points.first().water_level_m ||
        water_level_m > points.last().water_level_m)
        return std::nullopt;

    for (int index = 1; index < points.size(); index++)
    {
        const HydraulicCurveTankVolumePoint &lower = points.at(index - 1);
        const HydraulicCurveTankVolumePoint &upper = points.at(index);
        if (water_level_m > upper.water_level_m)
            continue;

        const double level_difference_m = upper.water_level_m - lower.water_level_m;
        if (qFuzzyIsNull(level_difference_m))
            return std::nullopt;

        const double position = (water_level_m - lower.water_level_m) / level_difference_m;
        const double volume_m3 =
            lower.volume_m3 + position * (upper.volume_m3 - lower.volume_m3);
        return qIsFinite(volume_m3) ? std::optional<double>(volume_m3) : std::nullopt;
    }

    return std::nullopt;
}

void EntityInspectorTank::setSpinEditable(QDoubleSpinBox *spin, const QString &tooltip)
{
    spin->setReadOnly(false);
    spin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    spin->setSpecialValueText(QString());
    spin->setToolTip(tooltip);
    static_cast<UnitNumericSpinBox *>(spin)->refreshUnits(
        UnitProfileManager::instance().presentation());
}

void EntityInspectorTank::setSpinDerived(
    QDoubleSpinBox *spin, const std::optional<double> &value, const QString &tooltip)
{
    spin->setReadOnly(true);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setToolTip(tooltip);
    static_cast<UnitNumericSpinBox *>(spin)->refreshDerived(
        UnitProfileManager::instance().presentation(), value);
}

HydraulicNodeTankGeometryInputType EntityInspectorTank::tankGeometryInputType() const
{
    return static_cast<HydraulicNodeTankGeometryInputType>(
        this->combo_geometry_type->currentData().toInt());
}

void EntityInspectorTank::addGroupQuality()
{
    addGroupNodeQualityInputs();

    GroupBoxCollapsible *group_mixing = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Tank Mixing — All Modes"));
    QGridLayout *grid_mixing = new QGridLayout(group_mixing);
    int mixing_row = 0;

    QLabel *label_mixing_model = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Mixing Model"), nullptr);
    this->combo_quality_mixing = new QComboBox();
    EntityInspectorText::addChoice(this->combo_quality_mixing, QT_TRANSLATE_NOOP("LiveUiText", "Complete Mix"), static_cast<int>(HydraulicNodeTankMixingModel::CompleteMix));
    EntityInspectorText::addChoice(this->combo_quality_mixing, QT_TRANSLATE_NOOP("LiveUiText", "Two-Compartment"), static_cast<int>(HydraulicNodeTankMixingModel::TwoCompartment));
    EntityInspectorText::addChoice(this->combo_quality_mixing, QT_TRANSLATE_NOOP("LiveUiText", "First In, First Out"), static_cast<int>(HydraulicNodeTankMixingModel::FirstInFirstOut));
    EntityInspectorText::addChoice(this->combo_quality_mixing, QT_TRANSLATE_NOOP("LiveUiText", "Last In, First Out"), static_cast<int>(HydraulicNodeTankMixingModel::LastInFirstOut));
    constrainComboWidth(this->combo_quality_mixing);

    QLabel *label_mixing_fraction = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Mixing Fraction"), nullptr);
    this->spin_quality_mixing_fraction = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
            return tank ? std::optional<double>(tank->mixing_fraction) : std::nullopt;
        },
        [this](double fraction) {
            if (!this->hydraulic_data || !this->hydraulic_data->tank(this->tank_uuid)) return false;
            const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
            if (tank->mixing_model != HydraulicNodeTankMixingModel::TwoCompartment) return false;
            if (fraction < 0.0 || fraction > 1.0) return false;
            return this->hydraulic_data->setTankQualityMixingFraction(this->tank_uuid, fraction);
        }, AOWIS_QUANTITY("mixing_fraction"), "1", 0.0, 1.0, 0.05, 4, group_mixing,
        [](const QString &) { return QString(); });
    this->spin_quality_mixing_fraction->setToolTip(
        "Fraction of tank volume assigned to the inlet/outlet compartment for the two-compartment model.");

    grid_mixing->addWidget(label_mixing_model, mixing_row, 0);
    grid_mixing->addWidget(this->combo_quality_mixing, mixing_row++, 1);
    grid_mixing->addWidget(label_mixing_fraction, mixing_row, 0);
    grid_mixing->addWidget(this->spin_quality_mixing_fraction, mixing_row++, 1);

    GroupBoxCollapsible *group_reactions = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Chemical — Tank Reactions"));
    QGridLayout *grid_reactions = new QGridLayout(group_reactions);
    int reaction_row = 0;

    this->check_quality_override_bulk_reaction = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Override global tank bulk reaction"), nullptr);

    QLabel *label_bulk_coefficient = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Bulk Reaction Coefficient"), nullptr);
    label_bulk_coefficient->setWordWrap(true);
    this->spin_quality_bulk_reaction_coefficient = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
            return tank ? std::optional<double>(tank->bulk_reaction.coefficient) : std::nullopt;
        },
        [this](double coefficient) {
            if (!this->hydraulic_data) return false;
            const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
            if (!tank || !tank->override_bulk_reaction ||
                !std::isfinite(tank->bulk_reaction.order)) return false;
            return this->hydraulic_data->setTankBulkReactionCoefficient(this->tank_uuid, coefficient);
        }, AOWIS_QUANTITY("first_order_bulk_reaction_coefficient"), "/d", -1000000.0, 1000000.0,
        0.001, 12, group_reactions,
        [this](const QString &inverse_time) {
            if (!this->hydraulic_data) return QString();
            const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
            return tank ? UnitBulkReactionDisplay::suffix(tank->bulk_reaction.order, inverse_time)
                        : QString();
        });
    this->spin_quality_bulk_reaction_coefficient->setToolTip(
        "Coefficient units depend on reaction order; concentration remains in canonical mg/L. "
        "The unit profile changes only the time basis.");

    grid_reactions->addWidget(this->check_quality_override_bulk_reaction, reaction_row++, 0, 1, 2);
    grid_reactions->addWidget(label_bulk_coefficient, reaction_row, 0);
    grid_reactions->addWidget(this->spin_quality_bulk_reaction_coefficient, reaction_row++, 1);

    connect(this->combo_quality_mixing, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicNodeTankMixingModel mixing_model = static_cast<HydraulicNodeTankMixingModel>(
            this->combo_quality_mixing->currentData().toInt());
        this->hydraulic_data->setTankQualityMixingModel(this->tank_uuid, mixing_model);
        updateQualityUi();
    });
    connect(this->check_quality_override_bulk_reaction, &QCheckBox::toggled,
            this, [this](bool enabled)
    {
        this->hydraulic_data->setTankOverrideBulkReaction(this->tank_uuid, enabled);
        updateQualityUi();
    });

    connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this,
            [this](InfrastructureEntity entity_type, const QUuid &uuid)
    {
        if (entity_type == InfrastructureEntity::Tank && uuid == this->tank_uuid)
            refreshQuality();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, &EntityInspectorTank::refreshQuality);

    layoutQuality()->addWidget(group_mixing);
    layoutQuality()->addWidget(group_reactions);
    UnitPresentationContext &presentation = UnitProfileManager::instance().presentation();
    new UnitAwareView(this->spin_quality_mixing_fraction, presentation, [this] {
        static_cast<UnitNumericSpinBox *>(this->spin_quality_mixing_fraction)->refreshUnits(
            UnitProfileManager::instance().presentation());
        updateQualityUi();
    });
    new UnitAwareView(this->spin_quality_bulk_reaction_coefficient, presentation, [this] {
        static_cast<UnitNumericSpinBox *>(this->spin_quality_bulk_reaction_coefficient)->refreshUnits(
            UnitProfileManager::instance().presentation());
        updateQualityUi();
    });
    refreshQuality();
}

void EntityInspectorTank::refreshQuality()
{
    const std::optional<HydraulicNodeTank> tank = this->hydraulic_data->tank(this->tank_uuid);
    if (!tank.has_value())
        return;

    const QSignalBlocker mixing_blocker(this->combo_quality_mixing);
    const QSignalBlocker fraction_blocker(this->spin_quality_mixing_fraction);
    const QSignalBlocker override_blocker(this->check_quality_override_bulk_reaction);
    const QSignalBlocker coefficient_blocker(this->spin_quality_bulk_reaction_coefficient);

    const int mixing_index = this->combo_quality_mixing->findData(static_cast<int>(tank->mixing_model));
    this->combo_quality_mixing->setCurrentIndex(mixing_index >= 0 ? mixing_index : 0);
    this->check_quality_override_bulk_reaction->setChecked(tank->override_bulk_reaction);
    const UnitPresentationContext &presentation = UnitProfileManager::instance().presentation();
    static_cast<UnitNumericSpinBox *>(this->spin_quality_mixing_fraction)->refreshUnits(presentation);
    static_cast<UnitNumericSpinBox *>(this->spin_quality_bulk_reaction_coefficient)->refreshUnits(presentation);
    updateQualityUi();
}

void EntityInspectorTank::updateQualityUi()
{
    const HydraulicNodeTankMixingModel mixing_model = static_cast<HydraulicNodeTankMixingModel>(
        this->combo_quality_mixing->currentData().toInt());
    this->spin_quality_mixing_fraction->setEnabled(
        mixing_model == HydraulicNodeTankMixingModel::TwoCompartment);

    const bool override_reaction = this->check_quality_override_bulk_reaction->isChecked();
    this->spin_quality_bulk_reaction_coefficient->setEnabled(override_reaction);
}

void EntityInspectorTank::onGroupExpand(GroupBoxCollapsible *group)
{
    if (group == this->group_geometry)
        refreshGeometry();
}

void EntityInspectorTank::onGeometryTypeChanged(int index)
{
    Q_UNUSED(index)

    const HydraulicNodeTankGeometryInputType geometry_input_type = tankGeometryInputType();
    updateGeometryModeUi(geometry_input_type);
    this->hydraulic_data->setTankGeometryInputType(this->tank_uuid, geometry_input_type);
}
