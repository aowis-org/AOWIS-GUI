#include "entity_inspector/entity_inspector_text.h"
#include <aowis/model/units/quantity.h>
#include "entity_inspector/entity_inspector_valve.h"

#include "config/unit_aware_view.h"
#include "config/unit_numeric_spin_box.h"
#include "config/unit_profile_manager.h"

#include <optional>

#include <QSignalBlocker>

namespace
{
constexpr int uuid_role = Qt::UserRole + 1;

QString entityName(const QString &id, const QUuid &uuid)
{
    return id.isEmpty() ? uuid.toString(QUuid::WithoutBraces) : id;
}
}

EntityInspectorValve::EntityInspectorValve(HydraulicData *hydraulic_data, const HydraulicLinkValve &valve, QWidget *parent)
    : EntityInspectorWidget(hydraulic_data, parent),
      hydraulic_data(hydraulic_data),
      valve_uuid(valve.uuid)
{
    addGroupOverviewImage(":/icon/valve.png", valve.id);

    addGroupGeneral(QString());
    bindHydraulicLink(InfrastructureEntity::Valve, this->valve_uuid, "Valve");

    addGroupEndpoints();
    addGroupValveConfiguration();
    addGroupSimulation();
    addGroupWaterQualitySimulation();
    
    addGroupNoEntitySpecificQualityInputs(QStringLiteral("valve"));
    addGroupAlerts();
    addGroupHistory();
    
    addStretches();

    bindValve();
}

void EntityInspectorValve::addGroupValveConfiguration()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Valve Configuration"));
    QGridLayout *grid = new QGridLayout(group);
    
    QLabel *label_valve_type = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Valve Type"), nullptr);
    this->combo_valve_type = new QComboBox();
    EntityInspectorText::addChoice(this->combo_valve_type,
        QT_TRANSLATE_NOOP("LiveUiText", "PRV: Pressure Reducing Valve"),
        static_cast<int>(HydraulicLinkValveType::PRV));
    EntityInspectorText::addChoice(this->combo_valve_type,
        QT_TRANSLATE_NOOP("LiveUiText", "PSV: Pressure Sustaining Valve"),
        static_cast<int>(HydraulicLinkValveType::PSV));
    EntityInspectorText::addChoice(this->combo_valve_type,
        QT_TRANSLATE_NOOP("LiveUiText", "FCV: Flow Control Valve"),
        static_cast<int>(HydraulicLinkValveType::FCV));
    EntityInspectorText::addChoice(this->combo_valve_type,
        QT_TRANSLATE_NOOP("LiveUiText", "PBV: Pressure Breaker Valve"),
        static_cast<int>(HydraulicLinkValveType::PBV));
    EntityInspectorText::addChoice(this->combo_valve_type,
        QT_TRANSLATE_NOOP("LiveUiText", "TCV: Throttle Control Valve"),
        static_cast<int>(HydraulicLinkValveType::TCV));
    EntityInspectorText::addChoice(this->combo_valve_type,
        QT_TRANSLATE_NOOP("LiveUiText", "GPV: General Purpose Valve"),
        static_cast<int>(HydraulicLinkValveType::GPV));
    EntityInspectorText::addChoice(this->combo_valve_type,
        QT_TRANSLATE_NOOP("LiveUiText", "PCV: Positional Control Valve"),
        static_cast<int>(HydraulicLinkValveType::PCV));
    constrainComboWidth(this->combo_valve_type);
    
    this->label_setting = new QLabel();
    EntityInspectorText::setCaption(this->label_setting, QT_TRANSLATE_NOOP("LiveUiText", "Setting"));
    this->stack_setting = new QStackedWidget(group);
    // Each setting has its own binding and physical dimension. Changing the
    // valve type swaps editors; it never reinterprets a previous editor value.
    this->spin_setting_pressure = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            if (!valve || (valve->type != HydraulicLinkValveType::PRV &&
                           valve->type != HydraulicLinkValveType::PSV &&
                           valve->type != HydraulicLinkValveType::PBV)) return std::nullopt;
            return valve->setting_pressure_head_m;
        },
        [this](double value) {
            if (!this->hydraulic_data) return false;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            if (!valve || (valve->type != HydraulicLinkValveType::PRV &&
                           valve->type != HydraulicLinkValveType::PSV &&
                           valve->type != HydraulicLinkValveType::PBV)) return false;
            return this->hydraulic_data->setValveSettingPressureHeadM(this->valve_uuid, value);
        }, AOWIS_QUANTITY("pressure_head"), "m", 0.0, 1000000.0, 1.0, 2, this->stack_setting);
    this->spin_setting_flow = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            if (!valve || valve->type != HydraulicLinkValveType::FCV) return std::nullopt;
            return valve->setting_flow_m3_per_h;
        },
        [this](double value) {
            if (!this->hydraulic_data) return false;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            if (!valve || valve->type != HydraulicLinkValveType::FCV) return false;
            return this->hydraulic_data->setValveSettingFlowM3PerH(this->valve_uuid, value);
        }, AOWIS_QUANTITY("volumetric_flow_rate"), "m3/h", 0.0, 1000000.0, 1.0, 3, this->stack_setting);
    this->spin_setting_loss = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            if (!valve || valve->type != HydraulicLinkValveType::TCV) return std::nullopt;
            return valve->setting_loss_coefficient;
        },
        [this](double value) {
            if (!this->hydraulic_data) return false;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            if (!valve || valve->type != HydraulicLinkValveType::TCV) return false;
            return this->hydraulic_data->setValveSettingLossCoefficient(this->valve_uuid, value);
        }, AOWIS_QUANTITY("minor_loss_coefficient"), "1", 0.0, 1000000.0, 0.1, 3, this->stack_setting,
        [](const QString &) { return QString(); });
    this->spin_setting_position = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            if (!valve || valve->type != HydraulicLinkValveType::PCV) return std::nullopt;
            return valve->setting_position_percent;
        },
        [this](double value) {
            if (!this->hydraulic_data) return false;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            if (!valve || valve->type != HydraulicLinkValveType::PCV) return false;
            return this->hydraulic_data->setValveSettingPositionPercent(this->valve_uuid, value);
        }, AOWIS_QUANTITY("valve_position"), "%", 0.0, 100.0, 1.0, 2, this->stack_setting,
        [](const QString &unit) {
            return unit == QStringLiteral("1") ? QString() : NumericText::suffix(unit);
        });
    this->stack_setting->addWidget(this->spin_setting_pressure);
    this->stack_setting->addWidget(this->spin_setting_flow);
    this->stack_setting->addWidget(this->spin_setting_loss);
    this->stack_setting->addWidget(this->spin_setting_position);
    
    this->label_setting_curve = new QLabel();
    EntityInspectorText::setCaption(this->label_setting_curve, QT_TRANSLATE_NOOP("LiveUiText", "Curve"));
    this->combo_setting_curve = new QComboBox();
    constrainComboWidth(this->combo_setting_curve);
    EntityInspectorText::addChoice(this->combo_setting_curve,
        QT_TRANSLATE_NOOP("LiveUiText", "Select curve..."), QUuid());
    this->label_setting_curve->hide();
    this->combo_setting_curve->hide();
    
    QLabel *label_status_initial = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Initial Status"), nullptr);
    this->combo_status_initial = new QComboBox();
    EntityInspectorText::addChoice(this->combo_status_initial, QT_TRANSLATE_NOOP("LiveUiText", "Active"), static_cast<int>(HydraulicLinkValveInitialStatus::Active));
    EntityInspectorText::addChoice(this->combo_status_initial, QT_TRANSLATE_NOOP("LiveUiText", "Open"), static_cast<int>(HydraulicLinkValveInitialStatus::Open));
    EntityInspectorText::addChoice(this->combo_status_initial, QT_TRANSLATE_NOOP("LiveUiText", "Closed"), static_cast<int>(HydraulicLinkValveInitialStatus::Closed));
    
    // Model values remain authoritative; the display spin boxes never own the
    // rounded diameter or loss coefficient. Both read the live valve record.
    QLabel *label_diameter = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Diameter"), nullptr);
    this->spin_diameter = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            return valve ? std::optional<double>(valve->diameter_mm) : std::nullopt;
        },
        [this](double diameter_mm) {
            if (!this->hydraulic_data || !this->hydraulic_data->valve(this->valve_uuid)) return false;
            return this->hydraulic_data->setValveDiameterMm(this->valve_uuid, diameter_mm);
        }, AOWIS_QUANTITY("link_diameter"), "mm", 0.0, 1000000.0, 10.0, 2, group);

    QLabel *label_loss_coeff = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Loss Coefficient"), nullptr);
    label_loss_coeff->setWordWrap(true);
    this->spin_loss_coeff = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->valve_uuid);
            return valve ? std::optional<double>(valve->minor_loss_coefficient) : std::nullopt;
        },
        [this](double coefficient) {
            if (!this->hydraulic_data || !this->hydraulic_data->valve(this->valve_uuid)) return false;
            return this->hydraulic_data->setValveMinorLoss(this->valve_uuid, coefficient);
        }, AOWIS_QUANTITY("minor_loss_coefficient"), "1", 0.0, 1000000.0, 0.1, 4, group);
    
    grid->addWidget(label_valve_type, 0, 0, 1, 2);
    grid->addWidget(this->combo_valve_type, 1, 0, 1, 2);
    
    grid->addWidget(this->label_setting, 2, 0);
    grid->addWidget(this->stack_setting, 2, 1);

    grid->addWidget(this->label_setting_curve, 3, 0);
    grid->addWidget(this->combo_setting_curve, 3, 1);
    
    grid->addWidget(label_status_initial, 4, 0);
    grid->addWidget(this->combo_status_initial, 4, 1);
    
    grid->addWidget(label_diameter, 5, 0);
    grid->addWidget(this->spin_diameter, 5, 1);
    
    grid->addWidget(label_loss_coeff, 6, 0);
    grid->addWidget(this->spin_loss_coeff, 6, 1);
    
    layoutConfiguration()->addWidget(group);
    // The active type's tooltip is presentation only. Do not call
    // onValveTypeChanged() from LanguageChange: that cancels numeric drafts
    // when switching between hydraulic setting types.
    LiveUiText::bind(this->combo_valve_type, [this] { refreshValveSettingTooltips(); });
    // The diameter is hidden when the configuration group is collapsed. One
    // subscription refreshes all active valve measurements when shown.
    new UnitAwareView(this->spin_diameter, UnitProfileManager::instance().presentation(),
        [this] { refreshValveMeasurements(); });
}

void EntityInspectorValve::refreshValveMeasurements()
{
    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    static_cast<UnitNumericSpinBox *>(this->spin_diameter)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_loss_coeff)->refreshUnits(context);
    if (this->displayed_setting_type_ &&
        *this->displayed_setting_type_ != HydraulicLinkValveType::GPV)
        static_cast<UnitNumericSpinBox *>(this->stack_setting->currentWidget())->refreshUnits(context);
}

void EntityInspectorValve::bindValve()
{
    connect(this->combo_valve_type, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicLinkValveType type = static_cast<HydraulicLinkValveType>(
            this->combo_valve_type->currentData().toInt());
        if (!this->hydraulic_data->setValveType(this->valve_uuid, type))
            refreshValve();
    });
    connect(this->combo_setting_curve, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicLinkValveType type = static_cast<HydraulicLinkValveType>(
            this->combo_valve_type->currentData().toInt());
        const QUuid curve_uuid = this->combo_setting_curve->currentData(uuid_role).toUuid();
        if (type == HydraulicLinkValveType::GPV)
            this->hydraulic_data->setValveHeadLossCurveUuid(this->valve_uuid, curve_uuid);
        else if (type == HydraulicLinkValveType::PCV)
            this->hydraulic_data->setValveCharacteristicCurveUuid(this->valve_uuid, curve_uuid);
    });
    connect(this->combo_status_initial, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicLinkValveInitialStatus initial_status =
            static_cast<HydraulicLinkValveInitialStatus>(
                this->combo_status_initial->currentData().toInt());
        this->hydraulic_data->setValveInitialStatus(this->valve_uuid, initial_status);
    });
    connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, this,
            [this](InfrastructureEntity entity_type, const QUuid &uuid)
    {
        if (entity_type == InfrastructureEntity::Valve && uuid == this->valve_uuid)
            refreshValve();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, &EntityInspectorValve::refreshValve);

    refreshValve();
}

void EntityInspectorValve::refreshValve()
{
    if (this->hydraulic_data == nullptr || this->valve_uuid.isNull())
        return;

    const std::optional<HydraulicLinkValve> valve =
        this->hydraulic_data->valve(this->valve_uuid);
    if (!valve.has_value())
        return;

    const QSignalBlocker type_blocker(this->combo_valve_type);
    const QSignalBlocker curve_blocker(this->combo_setting_curve);
    const QSignalBlocker status_blocker(this->combo_status_initial);

    const int type_index = this->combo_valve_type->findData(static_cast<int>(valve->type));
    this->combo_valve_type->setCurrentIndex(type_index >= 0 ? type_index : 0);
    onValveTypeChanged(valve->type);
    QUuid curve_uuid;
    if (valve->type == HydraulicLinkValveType::GPV)
        curve_uuid = valve->head_loss_curve_uuid;
    else if (valve->type == HydraulicLinkValveType::PCV)
        curve_uuid = valve->characteristic_curve_uuid;
    populateSettingCurveCombo(valve->type, curve_uuid);

    const int status_index =
        this->combo_status_initial->findData(static_cast<int>(valve->initial_status));
    this->combo_status_initial->setCurrentIndex(status_index >= 0 ? status_index : 0);
    refreshValveMeasurements();
}

void EntityInspectorValve::populateSettingCurveCombo(
    HydraulicLinkValveType type, const QUuid &curve_uuid)
{
    this->combo_setting_curve->clear();
    EntityInspectorText::addChoice(this->combo_setting_curve,
        QT_TRANSLATE_NOOP("LiveUiText", "Select curve..."), QUuid());
    this->combo_setting_curve->setItemData(0, QUuid(), uuid_role);

    bool selected_curve_exists = curve_uuid.isNull();
    if (type == HydraulicLinkValveType::GPV)
    {
        const QList<HydraulicCurveValveHeadloss> &curves =
            this->hydraulic_data->networkHydraulic().curves_valve_headloss;
        for (const HydraulicCurveValveHeadloss &curve : curves)
        {
            this->combo_setting_curve->addItem(entityName(curve.id, curve.uuid));
            this->combo_setting_curve->setItemData(
                this->combo_setting_curve->count() - 1, curve.uuid, uuid_role);
            if (curve.uuid == curve_uuid)
                selected_curve_exists = true;
        }
    }
    else if (type == HydraulicLinkValveType::PCV)
    {
        const QList<HydraulicCurveValveCharacteristic> &curves =
            this->hydraulic_data->networkHydraulic().curves_valve_characteristic;
        for (const HydraulicCurveValveCharacteristic &curve : curves)
        {
            this->combo_setting_curve->addItem(entityName(curve.id, curve.uuid));
            this->combo_setting_curve->setItemData(
                this->combo_setting_curve->count() - 1, curve.uuid, uuid_role);
            if (curve.uuid == curve_uuid)
                selected_curve_exists = true;
        }
    }

    if (!curve_uuid.isNull() && !selected_curve_exists)
    {
        EntityInspectorText::addFormattedChoice(this->combo_setting_curve,
            QT_TRANSLATE_NOOP("LiveUiText", "[Missing Curve] %1"),
            curve_uuid.toString(QUuid::WithoutBraces));
        this->combo_setting_curve->setItemData(
            this->combo_setting_curve->count() - 1, curve_uuid, uuid_role);
    }

    int selected_index = 0;
    for (int index = 0; index < this->combo_setting_curve->count(); index++)
    {
        if (this->combo_setting_curve->itemData(index, uuid_role).toUuid() == curve_uuid)
        {
            selected_index = index;
            break;
        }
    }
    this->combo_setting_curve->setCurrentIndex(selected_index);
}

void EntityInspectorValve::onValveTypeChanged(HydraulicLinkValveType type)
{
    if (this->displayed_setting_type_ && *this->displayed_setting_type_ != type) {
        // A previously active field may contain uncommitted text. A different
        // physical setting must never inherit or commit that pending input.
        this->spin_setting_pressure->cancelEdit();
        this->spin_setting_flow->cancelEdit();
        this->spin_setting_loss->cancelEdit();
        this->spin_setting_position->cancelEdit();
    }
    this->displayed_setting_type_ = type;
    this->label_setting->show();
    this->stack_setting->show();
    this->label_setting_curve->hide();
    this->combo_setting_curve->hide();

    switch (type) {
    case HydraulicLinkValveType::PRV:
        EntityInspectorText::setCaption(this->label_setting, QT_TRANSLATE_NOOP("LiveUiText", "Pressure Setting"));
        this->stack_setting->setCurrentWidget(this->spin_setting_pressure);
        break;
    case HydraulicLinkValveType::PSV:
        EntityInspectorText::setCaption(this->label_setting, QT_TRANSLATE_NOOP("LiveUiText", "Pressure Setting"));
        this->stack_setting->setCurrentWidget(this->spin_setting_pressure);
        break;
    case HydraulicLinkValveType::PBV:
        EntityInspectorText::setCaption(this->label_setting, QT_TRANSLATE_NOOP("LiveUiText", "Pressure Drop"));
        this->stack_setting->setCurrentWidget(this->spin_setting_pressure);
        break;
    case HydraulicLinkValveType::FCV:
        EntityInspectorText::setCaption(this->label_setting, QT_TRANSLATE_NOOP("LiveUiText", "Flow Setting"));
        this->stack_setting->setCurrentWidget(this->spin_setting_flow);
        break;
    case HydraulicLinkValveType::TCV:
        EntityInspectorText::setCaption(this->label_setting, QT_TRANSLATE_NOOP("LiveUiText", "Loss Coefficient"));
        this->stack_setting->setCurrentWidget(this->spin_setting_loss);
        break;
    case HydraulicLinkValveType::GPV:
        this->label_setting->hide();
        this->stack_setting->hide();
        EntityInspectorText::setCaption(this->label_setting_curve, QT_TRANSLATE_NOOP("LiveUiText", "Head Loss Curve"));
        this->label_setting_curve->show();
        this->combo_setting_curve->show();
        break;
    case HydraulicLinkValveType::PCV:
        EntityInspectorText::setCaption(this->label_setting, QT_TRANSLATE_NOOP("LiveUiText", "Position Setting"));
        this->stack_setting->setCurrentWidget(this->spin_setting_position);
        EntityInspectorText::setCaption(this->label_setting_curve, QT_TRANSLATE_NOOP("LiveUiText", "Characteristic Curve"));
        this->label_setting_curve->show();
        this->combo_setting_curve->show();
        break;
    }
    refreshValveSettingTooltips();
}

void EntityInspectorValve::refreshValveSettingTooltips()
{
    if (!this->displayed_setting_type_.has_value())
        return;

    switch (*this->displayed_setting_type_)
    {
    case HydraulicLinkValveType::PRV:
        this->spin_setting_pressure->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP(
            "LiveUiText", "Target downstream pressure head for the pressure reducing valve.")));
        break;
    case HydraulicLinkValveType::PSV:
        this->spin_setting_pressure->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP(
            "LiveUiText", "Target upstream pressure head for the pressure sustaining valve.")));
        break;
    case HydraulicLinkValveType::PBV:
        this->spin_setting_pressure->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP(
            "LiveUiText", "Fixed pressure drop across the pressure breaker valve.")));
        break;
    case HydraulicLinkValveType::FCV:
        this->spin_setting_flow->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP(
            "LiveUiText", "Target flow through the flow control valve.")));
        break;
    case HydraulicLinkValveType::TCV:
        this->spin_setting_loss->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP(
            "LiveUiText", "Throttle setting expressed as a dimensionless loss coefficient.")));
        break;
    case HydraulicLinkValveType::GPV:
        this->combo_setting_curve->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP(
            "LiveUiText", "Head-loss curve used by the general purpose valve.")));
        break;
    case HydraulicLinkValveType::PCV:
        this->spin_setting_position->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP(
            "LiveUiText", "Valve opening position as a percentage or fraction.<br>PCV support requires EPANET 2.3 or newer.")));
        this->combo_setting_curve->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP(
            "LiveUiText", "Optional valve-characteristic curve used by the positional control valve.")));
        break;
    }
}
