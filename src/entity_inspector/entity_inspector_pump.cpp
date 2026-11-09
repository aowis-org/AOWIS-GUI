#include "entity_inspector/entity_inspector_text.h"
#include <aowis/model/units/quantity.h>
#include "entity_inspector/entity_inspector_pump.h"

#include "config/unit_aware_view.h"
#include "config/unit_energy_price_spin_box.h"
#include "config/unit_numeric_spin_box.h"
#include "config/unit_profile_manager.h"

#include <cmath>
#include <optional>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QHeaderView>
#include <QHBoxLayout>

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTime>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QSignalBlocker>

namespace
{
constexpr int value_role = Qt::UserRole;
constexpr int uuid_role = Qt::UserRole + 1;

QString entityName(const QString &id, const QUuid &uuid)
{
    return id.isEmpty() ? uuid.toString(QUuid::WithoutBraces) : id;
}

bool isJunction(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicNodeJunction &junction : network.nodes_junctions)
    {
        if (junction.uuid == uuid)
            return true;
    }
    return false;
}

bool isTank(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicNodeTank &tank : network.nodes_tanks)
    {
        if (tank.uuid == uuid)
            return true;
    }
    return false;
}

bool isReservoir(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicNodeReservoir &reservoir : network.nodes_reservoirs)
    {
        if (reservoir.uuid == uuid)
            return true;
    }
    return false;
}

bool isLevelControl(HydraulicControlSimpleType type)
{
    return type == HydraulicControlSimpleType::LowLevel
        || type == HydraulicControlSimpleType::HighLevel;
}

int ruleCountForPump(const NetworkHydraulic &network, const QUuid &pump_uuid)
{
    int count = 0;
    for (const HydraulicControlRule &rule : network.controls_rules)
    {
        bool targets_pump = false;
        for (const HydraulicControlRuleAction &action : rule.actions_then)
            targets_pump = targets_pump || action.link_uuid == pump_uuid;
        for (const HydraulicControlRuleAction &action : rule.actions_else)
            targets_pump = targets_pump || action.link_uuid == pump_uuid;
        if (targets_pump)
            count++;
    }
    return count;
}
}

EntityInspectorPump::EntityInspectorPump(HydraulicData *hydraulic_data,
                                         const HydraulicLinkPump &pump,
                                         QWidget *parent)
    : EntityInspectorWidget(hydraulic_data, parent),
      hydraulic_data(hydraulic_data),
      pump_uuid(pump.uuid)
{
    addGroupOverviewImage(":/icon/pump.png", pump.id);

    addGroupGeneral(QString());
    bindHydraulicLink(InfrastructureEntity::Pump, this->pump_uuid, "Pump");
    addGroupEndpoints();

    addGroupPumpInput();
    addGroupControls();
    addGroupEnergyCostInput();
    addGroupEnergy();
    addGroupSimulation();
    addGroupWaterQualitySimulation();
    addGroupNoEntitySpecificQualityInputs(QStringLiteral("pump"));
    addGroupAlerts();
    addGroupHistory();
    addStretches();

    bindPump();
}

void EntityInspectorPump::addGroupPumpInput()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Pump Input"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_type = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Type"), nullptr);
    this->combo_type = new QComboBox();
    EntityInspectorText::addChoice(this->combo_type, QT_TRANSLATE_NOOP("LiveUiText", "Constant power"), static_cast<int>(HydraulicLinkPumpDefinitionType::ConstantPower));
    EntityInspectorText::addChoice(this->combo_type, QT_TRANSLATE_NOOP("LiveUiText", "1-point curve"), static_cast<int>(HydraulicLinkPumpDefinitionType::OnePointCurve));
    EntityInspectorText::addChoice(this->combo_type, QT_TRANSLATE_NOOP("LiveUiText", "3-point curve"), static_cast<int>(HydraulicLinkPumpDefinitionType::ThreePointCurve));
    EntityInspectorText::addChoice(this->combo_type, QT_TRANSLATE_NOOP("LiveUiText", "From Library"), static_cast<int>(HydraulicLinkPumpDefinitionType::Library));

    this->label_constant_power = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Power"), nullptr);
    this->spin_constant_power = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(this->pump_uuid);
            if (!pump || pump->definition_type != HydraulicLinkPumpDefinitionType::ConstantPower)
                return std::nullopt;
            return pump->constant_power_kw;
        },
        [this](double kilowatts) {
            if (!this->hydraulic_data) return false;
            const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(this->pump_uuid);
            if (!pump || pump->definition_type != HydraulicLinkPumpDefinitionType::ConstantPower)
                return false;
            return this->hydraulic_data->setPumpConstantPowerKw(this->pump_uuid, kilowatts);
        }, AOWIS_QUANTITY("power"), "kW", 0.0, 1000000.0, 1.0, 3, group);
    LiveUiText::bind(this->spin_constant_power, [this] {
        // A LanguageChange must not replace a partially typed numeric draft.
        if (!this->spin_constant_power->hasFocus())
            this->spin_constant_power->setSpecialValueText(
                LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Not configured")));
    });
    connect(this->spin_constant_power, &QAbstractSpinBox::editingFinished, this, [this] {
        this->spin_constant_power->setSpecialValueText(
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Not configured")));
    });

    QLabel *label_speed_initial = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Initial Speed"), nullptr);
    this->spin_speed_initial = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(this->pump_uuid);
            return pump ? std::optional<double>(pump->initial_speed_ratio) : std::nullopt;
        },
        [this](double speed_ratio) {
            if (!this->hydraulic_data || !this->hydraulic_data->pump(this->pump_uuid))
                return false;
            return this->hydraulic_data->setPumpInitialSpeed(this->pump_uuid, speed_ratio);
        }, AOWIS_QUANTITY("pump_speed_ratio"), "1", 0.0, 10.0, 0.05, 2, group,
        [](const QString &) { return QStringLiteral(" ×"); });
    LiveUiText::bind(this->spin_speed_initial, [this] {
        if (!this->spin_speed_initial->hasFocus())
            this->spin_speed_initial->setSpecialValueText(
                LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "0.00 × (off)")));
    });
    connect(this->spin_speed_initial, &QAbstractSpinBox::editingFinished, this, [this] {
        this->spin_speed_initial->setSpecialValueText(
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "0.00 × (off)")));
    });

    QLabel *label_status_initial = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Initial Status"), nullptr);
    this->combo_status_initial = new QComboBox();
    EntityInspectorText::addChoice(this->combo_status_initial, QT_TRANSLATE_NOOP("LiveUiText", "On"), static_cast<int>(HydraulicLinkPumpInitialStatus::On));
    EntityInspectorText::addChoice(this->combo_status_initial, QT_TRANSLATE_NOOP("LiveUiText", "Off"), static_cast<int>(HydraulicLinkPumpInitialStatus::Off));

    QLabel *label_speed_pattern = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Speed Pattern"), nullptr);
    this->combo_speed_pattern = new QComboBox();
    constrainComboWidth(this->combo_speed_pattern);

    grid->addWidget(label_type, 0, 0);
    grid->addWidget(this->combo_type, 0, 1);
    grid->addWidget(this->label_constant_power, 1, 0);
    grid->addWidget(this->spin_constant_power, 1, 1);
    grid->addWidget(label_speed_initial, 2, 0);
    grid->addWidget(this->spin_speed_initial, 2, 1);
    grid->addWidget(label_status_initial, 3, 0);
    grid->addWidget(this->combo_status_initial, 3, 1);
    grid->addWidget(label_speed_pattern, 4, 0);
    grid->addWidget(this->combo_speed_pattern, 4, 1);

    layoutConfiguration()->addWidget(group);
    // Subscribe once for this small editor region. The view generation updates
    // its controls only while the pump input is visible.
    new UnitAwareView(this->spin_speed_initial, UnitProfileManager::instance().presentation(),
        [this] { refreshPumpMeasurements(); });
}

void EntityInspectorPump::addGroupControls()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Controls"));
    QVBoxLayout *layout = new QVBoxLayout(group);

    this->table_controls = new QTableWidget(group);
    this->table_controls->setColumnCount(7);
    LiveUiText::bind(this->table_controls, [this] {
        this->table_controls->setHorizontalHeaderLabels({
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Enabled")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Trigger")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Trigger Node")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Threshold / Time")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Action")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Speed")),
            QString()});
    });
    this->table_controls->verticalHeader()->setVisible(false);
    // Fixed, modest column widths rather than ResizeToContents: ResizeToContents sizes
    // every column (including the header text itself, even with zero rows) from its
    // content, and that summed width was propagating into the panel's own layout,
    // forcing this tab wider than Sizes::SidebarRightWidth. With explicit widths, any
    // cell content that doesn't fit is handled by the table's own horizontal scrollbar
    // instead of by growing the whole sidebar.
    this->table_controls->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    this->table_controls->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    this->table_controls->setColumnWidth(0, 55);
    this->table_controls->setColumnWidth(1, 90);
    this->table_controls->setColumnWidth(3, 80);
    this->table_controls->setColumnWidth(4, 65);
    this->table_controls->setColumnWidth(5, 60);
    this->table_controls->setColumnWidth(6, 60);
    this->table_controls->setSelectionMode(QAbstractItemView::NoSelection);
    this->table_controls->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->table_controls->setMinimumHeight(170);
    layout->addWidget(this->table_controls);

    QHBoxLayout *add_layout = new QHBoxLayout();
    this->combo_new_control_type = new QComboBox(group);
    EntityInspectorText::addChoice(this->combo_new_control_type, QT_TRANSLATE_NOOP("LiveUiText", "Below level / pressure"), static_cast<int>(HydraulicControlSimpleType::LowLevel));
    EntityInspectorText::addChoice(this->combo_new_control_type, QT_TRANSLATE_NOOP("LiveUiText", "Above level / pressure"), static_cast<int>(HydraulicControlSimpleType::HighLevel));
    EntityInspectorText::addChoice(this->combo_new_control_type, QT_TRANSLATE_NOOP("LiveUiText", "Elapsed time"), static_cast<int>(HydraulicControlSimpleType::Timer));
    EntityInspectorText::addChoice(this->combo_new_control_type, QT_TRANSLATE_NOOP("LiveUiText", "Time of day"), static_cast<int>(HydraulicControlSimpleType::TimeOfDay));
    this->button_add_control = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Add Control"), group);
    constrainComboWidth(this->combo_new_control_type);
    add_layout->addWidget(this->combo_new_control_type, 1);
    add_layout->addWidget(this->button_add_control);
    layout->addLayout(add_layout);

    this->label_rule_controls = new QLabel(group);
    this->label_rule_controls->setWordWrap(true);
    this->label_rule_controls->setVisible(false);
    layout->addWidget(this->label_rule_controls);
    LiveUiText::bind(this->label_rule_controls, [this] { refreshPumpRuleSummary(); });

    layoutConfiguration()->addWidget(group);
    // One subscription for the control table, not one for each cell. Rows are
    // bound to canonical control properties and are only reformatted when shown.
    new UnitAwareView(this->table_controls, UnitProfileManager::instance().presentation(),
        [this] { refreshPumpControlUnits(); });
}

void EntityInspectorPump::addGroupEnergyCostInput()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Energy Cost Input"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_efficiency_curve = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Efficiency Curve"), nullptr);
    this->combo_efficiency_curve = new QComboBox();
    constrainComboWidth(this->combo_efficiency_curve);

    QLabel *label_energy_price = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Energy Price"), nullptr);
    this->spin_energy_price = new UnitEnergyPriceSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(this->pump_uuid);
            return pump ? std::optional<double>(pump->energy_price_per_kw_h) : std::nullopt;
        },
        [this](double price_per_kwh) {
            if (!this->hydraulic_data) return false;
            const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(this->pump_uuid);
            if (!pump || pump->energy_price_input_type != HydraulicLinkPumpEnergyPriceInputType::Constant)
                return false;
            return this->hydraulic_data->setPumpEnergyPricePerKwh(this->pump_uuid, price_per_kwh);
        }, group);

    QLabel *label_price_pattern = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Price Pattern"), nullptr);
    this->combo_price_pattern = new QComboBox();
    constrainComboWidth(this->combo_price_pattern);

    grid->addWidget(label_efficiency_curve, 0, 0);
    grid->addWidget(this->combo_efficiency_curve, 0, 1);
    grid->addWidget(label_energy_price, 1, 0);
    grid->addWidget(this->spin_energy_price, 1, 1);
    grid->addWidget(label_price_pattern, 2, 0);
    grid->addWidget(this->combo_price_pattern, 2, 1);

    layoutConfiguration()->addWidget(group);
    new UnitAwareView(this->spin_energy_price, UnitProfileManager::instance().presentation(),
        [this] { refreshPumpEnergyPrice(); });
}

void EntityInspectorPump::addGroupEnergy()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Energy"));
    new QGridLayout(group);
    layoutConfiguration()->addWidget(group);
}

void EntityInspectorPump::bindPump()
{
    connect(this->combo_type, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicLinkPumpDefinitionType definition_type =
            static_cast<HydraulicLinkPumpDefinitionType>(
                this->combo_type->currentData(value_role).toInt());
        this->hydraulic_data->setPumpDefinitionType(this->pump_uuid, definition_type);
    });
    connect(this->combo_status_initial, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicLinkPumpInitialStatus initial_status =
            static_cast<HydraulicLinkPumpInitialStatus>(
                this->combo_status_initial->currentData(value_role).toInt());
        this->hydraulic_data->setPumpInitialStatus(this->pump_uuid, initial_status);
    });
    connect(this->combo_speed_pattern, &QComboBox::currentIndexChanged, this, [this](int)
    {
        this->hydraulic_data->setPumpSpeedPatternUuid(
            this->pump_uuid, this->combo_speed_pattern->currentData(uuid_role).toUuid());
    });
    connect(this->button_add_control, &QPushButton::clicked, this, [this]()
    {
        const HydraulicControlSimpleType type = static_cast<HydraulicControlSimpleType>(
            this->combo_new_control_type->currentData(value_role).toInt());
        const QUuid trigger_node_uuid = isLevelControl(type)
            ? firstPumpControlTriggerNodeUuid() : QUuid();
        this->hydraulic_data->addPumpSimpleControl(this->pump_uuid, type, trigger_node_uuid);
    });
    connect(this->combo_efficiency_curve, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicLinkPumpEfficiencyInputType input_type =
            static_cast<HydraulicLinkPumpEfficiencyInputType>(
                this->combo_efficiency_curve->currentData(value_role).toInt());
        this->hydraulic_data->setPumpEfficiencyInput(
            this->pump_uuid, input_type,
            this->combo_efficiency_curve->currentData(uuid_role).toUuid());
    });
    connect(this->combo_price_pattern, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicLinkPumpEnergyPriceInputType input_type =
            static_cast<HydraulicLinkPumpEnergyPriceInputType>(
                this->combo_price_pattern->currentData(value_role).toInt());
        this->hydraulic_data->setPumpEnergyPriceInput(
            this->pump_uuid, input_type,
            this->combo_price_pattern->currentData(uuid_role).toUuid());
    });

    connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, this,
            [this](InfrastructureEntity entity_type, const QUuid &uuid)
    {
        if (entity_type == InfrastructureEntity::Pump && uuid == this->pump_uuid)
            QTimer::singleShot(0, this, &EntityInspectorPump::refreshPump);
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, &EntityInspectorPump::refreshPump);

    refreshPump();
}

void EntityInspectorPump::refreshPump()
{
    if (this->hydraulic_data == nullptr || this->pump_uuid.isNull())
        return;

    const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(this->pump_uuid);
    if (!pump.has_value())
        return;

    const QSignalBlocker type_blocker(this->combo_type);
    const QSignalBlocker status_blocker(this->combo_status_initial);
    const QSignalBlocker speed_pattern_blocker(this->combo_speed_pattern);
    const QSignalBlocker efficiency_blocker(this->combo_efficiency_curve);
    const QSignalBlocker price_pattern_blocker(this->combo_price_pattern);

    const int type_index = this->combo_type->findData(
        static_cast<int>(pump->definition_type), value_role);
    this->combo_type->setCurrentIndex(type_index >= 0 ? type_index : 0);
    const bool constant_power = pump->definition_type == HydraulicLinkPumpDefinitionType::ConstantPower;
    // A pending constant-power edit must not survive switching to a curve or
    // library-defined pump. Display refreshes never write the rounded values.
    if (!constant_power) this->spin_constant_power->cancelEdit();
    this->label_constant_power->setVisible(constant_power);
    this->spin_constant_power->setVisible(constant_power);
    refreshPumpMeasurements();

    const int status_index = this->combo_status_initial->findData(
        static_cast<int>(pump->initial_status), value_role);
    this->combo_status_initial->setCurrentIndex(status_index >= 0 ? status_index : 0);

    populateSpeedPatternCombo(pump->speed_pattern_uuid);

    populateEfficiencyInputCombo(pump.value());
    if (pump->energy_price_input_type != HydraulicLinkPumpEnergyPriceInputType::Constant)
        this->spin_energy_price->cancelEdit();
    refreshPumpEnergyPrice();
    populatePricePatternCombo(pump.value());
    refreshPumpControls();
}

void EntityInspectorPump::refreshPumpEnergyPrice()
{
    if (!this->hydraulic_data)
        return;
    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    const QString currency = this->hydraulic_data->networkHydraulic().options_energy.currency_iso4217;
    const bool refreshed = this->spin_energy_price->refreshUnits(context, currency);
    const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(this->pump_uuid);
    this->spin_energy_price->setEnabled(refreshed && pump &&
        pump->energy_price_input_type == HydraulicLinkPumpEnergyPriceInputType::Constant);
}

void EntityInspectorPump::refreshPumpMeasurements()
{
    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    this->spin_constant_power->refreshUnits(context);
    this->spin_speed_initial->refreshUnits(context);
}

QUuid EntityInspectorPump::firstPumpControlTriggerNodeUuid() const
{
    if (this->hydraulic_data == nullptr)
        return QUuid();

    const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
    for (const HydraulicNodeTank &tank : network.nodes_tanks)
    {
        if (tank.metadata.enabled)
            return tank.uuid;
    }
    for (const HydraulicNodeReservoir &reservoir : network.nodes_reservoirs)
    {
        if (reservoir.metadata.enabled)
            return reservoir.uuid;
    }
    for (const HydraulicNodeJunction &junction : network.nodes_junctions)
    {
        if (junction.metadata.enabled)
            return junction.uuid;
    }
    return QUuid();
}

bool EntityInspectorPump::updatePumpSimpleControl(
    const QUuid &control_uuid,
    const std::function<void(HydraulicControlSimple &)> &mutation)
{
    if (this->hydraulic_data == nullptr)
        return false;

    const QList<HydraulicControlSimple> &controls =
        this->hydraulic_data->networkHydraulic().controls_simple;
    for (const HydraulicControlSimple &control : controls)
    {
        if (control.uuid != control_uuid || control.link_uuid != this->pump_uuid)
            continue;

        HydraulicControlSimple updated = control;
        mutation(updated);
        return this->hydraulic_data->setPumpSimpleControl(this->pump_uuid, updated);
    }

    return false;
}

void EntityInspectorPump::refreshPumpControlUnits()
{
    if (!this->table_controls) return;
    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    for (int row = 0; row < this->table_controls->rowCount(); ++row)
    {
        if (UnitNumericSpinBox *threshold =
                dynamic_cast<UnitNumericSpinBox *>(this->table_controls->cellWidget(row, 3)))
            threshold->refreshUnits(context);
        if (UnitNumericSpinBox *speed =
                dynamic_cast<UnitNumericSpinBox *>(this->table_controls->cellWidget(row, 5)))
            speed->refreshUnits(context);
    }
}

void EntityInspectorPump::refreshPumpControls()
{
    if (this->hydraulic_data == nullptr || this->table_controls == nullptr)
        return;

    const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
    QList<HydraulicControlSimple> controls;
    for (const HydraulicControlSimple &control : network.controls_simple)
    {
        if (control.link_uuid == this->pump_uuid)
            controls.append(control);
    }

    this->table_controls->setRowCount(controls.size());
    for (int row = 0; row < controls.size(); row++)
    {
        const HydraulicControlSimple control = controls.at(row);

        QCheckBox *enabled = new QCheckBox(this->table_controls);
        enabled->setChecked(control.enabled);
        LiveUiText::bind(enabled, [enabled] {
            enabled->setToolTip(LiveUiText::translate(
                QT_TRANSLATE_NOOP("LiveUiText", "Enable or disable this EPANET control.")));
        });
        this->table_controls->setCellWidget(row, 0, enabled);
        connect(enabled, &QCheckBox::toggled, this, [this, control_uuid = control.uuid](bool checked)
        {
            updatePumpSimpleControl(control_uuid, [checked](HydraulicControlSimple &updated)
            {
                updated.enabled = checked;
            });
        });

        QComboBox *trigger_type = new QComboBox(this->table_controls);
        EntityInspectorText::addChoice(trigger_type, QT_TRANSLATE_NOOP("LiveUiText", "Below level / pressure"),
                                       static_cast<int>(HydraulicControlSimpleType::LowLevel));
        EntityInspectorText::addChoice(trigger_type, QT_TRANSLATE_NOOP("LiveUiText", "Above level / pressure"),
                                       static_cast<int>(HydraulicControlSimpleType::HighLevel));
        EntityInspectorText::addChoice(trigger_type, QT_TRANSLATE_NOOP("LiveUiText", "Elapsed time"),
                                       static_cast<int>(HydraulicControlSimpleType::Timer));
        EntityInspectorText::addChoice(trigger_type, QT_TRANSLATE_NOOP("LiveUiText", "Time of day"),
                                       static_cast<int>(HydraulicControlSimpleType::TimeOfDay));
        constrainComboWidth(trigger_type);
        const int trigger_type_index = trigger_type->findData(
            static_cast<int>(control.type), value_role);
        trigger_type->setCurrentIndex(trigger_type_index >= 0 ? trigger_type_index : 0);
        this->table_controls->setCellWidget(row, 1, trigger_type);
        connect(trigger_type, &QComboBox::currentIndexChanged, this,
                [this, trigger_type, control_uuid = control.uuid](int)
        {
            const HydraulicControlSimpleType type = static_cast<HydraulicControlSimpleType>(
                trigger_type->currentData(value_role).toInt());
            updatePumpSimpleControl(control_uuid, [this, type](HydraulicControlSimple &updated)
            {
                updated.type = type;
                if (isLevelControl(type))
                {
                    const NetworkHydraulic &current_network =
                        this->hydraulic_data->networkHydraulic();
                    if (!isJunction(current_network, updated.trigger_node_uuid)
                        && !isTank(current_network, updated.trigger_node_uuid)
                        && !isReservoir(current_network, updated.trigger_node_uuid))
                    {
                        updated.trigger_node_uuid = firstPumpControlTriggerNodeUuid();
                    }
                }
                else
                {
                    updated.trigger_node_uuid = QUuid();
                }
            });
        });

        const bool level_control = isLevelControl(control.type);
        if (level_control)
        {
            QComboBox *trigger_node = new QComboBox(this->table_controls);
            EntityInspectorText::addChoice(trigger_node,
                QT_TRANSLATE_NOOP("LiveUiText", "Select node"));
            trigger_node->setItemData(0, QUuid(), uuid_role);

            bool selected_node_present = control.trigger_node_uuid.isNull();
            for (const HydraulicNodeTank &tank : network.nodes_tanks)
            {
                if (!tank.metadata.enabled)
                    continue;
                EntityInspectorText::addFormattedChoice(trigger_node,
                    QT_TRANSLATE_NOOP("LiveUiText", "Tank: %1"), entityName(tank.id, tank.uuid));
                trigger_node->setItemData(trigger_node->count() - 1, tank.uuid, uuid_role);
                selected_node_present = selected_node_present || tank.uuid == control.trigger_node_uuid;
            }
            for (const HydraulicNodeReservoir &reservoir : network.nodes_reservoirs)
            {
                if (!reservoir.metadata.enabled)
                    continue;
                EntityInspectorText::addFormattedChoice(trigger_node,
                    QT_TRANSLATE_NOOP("LiveUiText", "Reservoir: %1"),
                    entityName(reservoir.id, reservoir.uuid));
                trigger_node->setItemData(trigger_node->count() - 1, reservoir.uuid, uuid_role);
                selected_node_present = selected_node_present || reservoir.uuid == control.trigger_node_uuid;
            }
            for (const HydraulicNodeJunction &junction : network.nodes_junctions)
            {
                if (!junction.metadata.enabled)
                    continue;
                EntityInspectorText::addFormattedChoice(trigger_node,
                    QT_TRANSLATE_NOOP("LiveUiText", "Junction: %1"),
                    entityName(junction.id, junction.uuid));
                trigger_node->setItemData(trigger_node->count() - 1, junction.uuid, uuid_role);
                selected_node_present = selected_node_present || junction.uuid == control.trigger_node_uuid;
            }
            if (!control.trigger_node_uuid.isNull() && !selected_node_present)
            {
                EntityInspectorText::addFormattedChoice(trigger_node,
                    QT_TRANSLATE_NOOP("LiveUiText", "[Missing or disabled] %1"),
                    control.trigger_node_uuid.toString(QUuid::WithoutBraces));
                trigger_node->setItemData(
                    trigger_node->count() - 1, control.trigger_node_uuid, uuid_role);
            }

            int trigger_node_index = 0;
            for (int index = 0; index < trigger_node->count(); index++)
            {
                if (trigger_node->itemData(index, uuid_role).toUuid() == control.trigger_node_uuid)
                {
                    trigger_node_index = index;
                    break;
                }
            }
            trigger_node->setCurrentIndex(trigger_node_index);
            this->table_controls->setCellWidget(row, 2, trigger_node);
            connect(trigger_node, &QComboBox::currentIndexChanged, this,
                    [this, trigger_node, control_uuid = control.uuid](int)
            {
                const QUuid node_uuid = trigger_node->currentData(uuid_role).toUuid();
                updatePumpSimpleControl(control_uuid, [node_uuid](HydraulicControlSimple &updated)
                {
                    updated.trigger_node_uuid = node_uuid;
                });
            });

            const bool trigger_is_junction = isJunction(network, control.trigger_node_uuid);
            const QUuid trigger_node_uuid = control.trigger_node_uuid;
            const HydraulicControlSimpleType trigger_control_type = control.type;
            // A control's threshold changes physical quantity when its trigger
            // node changes between a junction and a tank/reservoir. Pin the
            // binding to the original node and control type to reject stale edits.
            const auto read_threshold = [this, control_uuid = control.uuid,
                                         trigger_node_uuid, trigger_control_type,
                                         trigger_is_junction]() -> std::optional<double> {
                if (!this->hydraulic_data) return std::nullopt;
                const NetworkHydraulic &current = this->hydraulic_data->networkHydraulic();
                if (trigger_is_junction != isJunction(current, trigger_node_uuid) ||
                    (!trigger_is_junction && !isTank(current, trigger_node_uuid) &&
                     !isReservoir(current, trigger_node_uuid))) return std::nullopt;
                for (const HydraulicControlSimple &candidate : current.controls_simple)
                {
                    if (candidate.uuid == control_uuid && candidate.link_uuid == this->pump_uuid &&
                        candidate.type == trigger_control_type &&
                        candidate.trigger_node_uuid == trigger_node_uuid)
                        return trigger_is_junction ? candidate.trigger_pressure_head_m
                                                   : candidate.trigger_water_level_m;
                }
                return std::nullopt;
            };
            UnitNumericSpinBox *threshold = new UnitNumericSpinBox(
                read_threshold,
                [this, control_uuid = control.uuid, trigger_node_uuid,
                 trigger_control_type, trigger_is_junction](double value) {
                    if (!this->hydraulic_data) return false;
                    const NetworkHydraulic &current = this->hydraulic_data->networkHydraulic();
                    if (trigger_is_junction != isJunction(current, trigger_node_uuid) ||
                        (!trigger_is_junction && !isTank(current, trigger_node_uuid) &&
                         !isReservoir(current, trigger_node_uuid))) return false;
                    const QList<HydraulicControlSimple> &controls = current.controls_simple;
                    for (const HydraulicControlSimple &candidate : controls)
                    {
                        if (candidate.uuid != control_uuid || candidate.link_uuid != this->pump_uuid)
                            continue;
                        if (candidate.type != trigger_control_type ||
                            candidate.trigger_node_uuid != trigger_node_uuid) return false;
                        return updatePumpSimpleControl(control_uuid,
                            [trigger_is_junction, value](HydraulicControlSimple &updated) {
                                if (trigger_is_junction) updated.trigger_pressure_head_m = value;
                                else updated.trigger_water_level_m = value;
                            });
                    }
                    return false;
                },
                trigger_is_junction ? AOWIS_QUANTITY("pressure_head") : AOWIS_QUANTITY("water_level"), "m",
                -10000.0, 10000.0, 0.1, 3, this->table_controls,
                [trigger_is_junction](const QString &unit) {
                    return NumericText::suffix(unit) +
                        (trigger_is_junction ? QStringLiteral(" pressure") : QStringLiteral(" level"));
                });
            threshold->refreshUnits(UnitProfileManager::instance().presentation());
            this->table_controls->setCellWidget(row, 3, threshold);
        }
        else
        {
            QLabel *no_node = new QLabel(QStringLiteral("—"), this->table_controls);
            this->table_controls->setCellWidget(row, 2, no_node);

            if (control.type == HydraulicControlSimpleType::Timer)
            {
                // Timer stores an integral number of seconds. The editor may
                // present hours/minutes/days, but commits only an explicit edit
                // converted back into integral seconds.
                UnitNumericSpinBox *elapsed_time = new UnitNumericSpinBox(
                    [this, control_uuid = control.uuid]() -> std::optional<double> {
                        if (!this->hydraulic_data) return std::nullopt;
                        for (const HydraulicControlSimple &candidate :
                             this->hydraulic_data->networkHydraulic().controls_simple)
                        {
                            if (candidate.uuid == control_uuid && candidate.link_uuid == this->pump_uuid &&
                                candidate.type == HydraulicControlSimpleType::Timer)
                                return static_cast<double>(candidate.trigger_elapsed_time_s);
                        }
                        return std::nullopt;
                    },
                    [this, control_uuid = control.uuid](double seconds) {
                        if (!this->hydraulic_data || !std::isfinite(seconds) ||
                            seconds < 0.0 || seconds > 596523.0 * 3600.0) return false;
                        for (const HydraulicControlSimple &candidate :
                             this->hydraulic_data->networkHydraulic().controls_simple)
                        {
                            if (candidate.uuid != control_uuid || candidate.link_uuid != this->pump_uuid)
                                continue;
                            if (candidate.type != HydraulicControlSimpleType::Timer) return false;
                            const quint64 integral_seconds = static_cast<quint64>(std::llround(seconds));
                            return updatePumpSimpleControl(control_uuid,
                                [integral_seconds](HydraulicControlSimple &updated) {
                                    updated.trigger_elapsed_time_s = integral_seconds;
                                });
                        }
                        return false;
                    },
                    AOWIS_QUANTITY("elapsed_time"), "s", 0.0, 596523.0 * 3600.0, 900.0, 3,
                    this->table_controls,
                    [](const QString &unit) { return NumericText::suffix(unit) + QStringLiteral(" elapsed"); });
                elapsed_time->refreshUnits(UnitProfileManager::instance().presentation());
                this->table_controls->setCellWidget(row, 3, elapsed_time);
            }
            else
            {
                QTimeEdit *time_of_day = new QTimeEdit(this->table_controls);
                time_of_day->setDisplayFormat(QStringLiteral("HH:mm:ss"));
                time_of_day->setTime(QTime(0, 0, 0).addSecs(
                    static_cast<int>(control.trigger_time_of_day_s % (24 * 60 * 60))));
                this->table_controls->setCellWidget(row, 3, time_of_day);
                connect(time_of_day, &QTimeEdit::timeChanged, this,
                        [this, control_uuid = control.uuid](const QTime &time)
                {
                    const quint64 seconds = static_cast<quint64>(QTime(0, 0, 0).secsTo(time));
                    updatePumpSimpleControl(control_uuid, [seconds](HydraulicControlSimple &updated)
                    {
                        updated.trigger_time_of_day_s = seconds;
                    });
                });
            }
        }

        QComboBox *action = new QComboBox(this->table_controls);
        EntityInspectorText::addChoice(action, QT_TRANSLATE_NOOP("LiveUiText", "Turn on"),
                                       static_cast<int>(HydraulicControlActionType::Open));
        EntityInspectorText::addChoice(action, QT_TRANSLATE_NOOP("LiveUiText", "Turn off"),
                                       static_cast<int>(HydraulicControlActionType::Close));
        EntityInspectorText::addChoice(action, QT_TRANSLATE_NOOP("LiveUiText", "Set speed"),
                                       static_cast<int>(HydraulicControlActionType::Setting));
        const int action_index = action->findData(static_cast<int>(control.action), value_role);
        action->setCurrentIndex(action_index >= 0 ? action_index : 0);
        this->table_controls->setCellWidget(row, 4, action);
        connect(action, &QComboBox::currentIndexChanged, this,
                [this, action, control_uuid = control.uuid](int)
        {
            const HydraulicControlActionType action_type = static_cast<HydraulicControlActionType>(
                action->currentData(value_role).toInt());
            updatePumpSimpleControl(control_uuid, [action_type](HydraulicControlSimple &updated)
            {
                updated.action = action_type;
                updated.setting = HydraulicControlLinkSetting();
                if (action_type == HydraulicControlActionType::Setting)
                    updated.setting.pump_speed_ratio = 1.0;
            });
        });

        UnitNumericSpinBox *speed = new UnitNumericSpinBox(
            [this, control_uuid = control.uuid]() -> std::optional<double> {
                if (!this->hydraulic_data) return std::nullopt;
                for (const HydraulicControlSimple &candidate :
                     this->hydraulic_data->networkHydraulic().controls_simple)
                {
                    if (candidate.uuid == control_uuid && candidate.link_uuid == this->pump_uuid &&
                        candidate.action == HydraulicControlActionType::Setting)
                        return candidate.setting.pump_speed_ratio.value_or(1.0);
                }
                return std::nullopt;
            },
            [this, control_uuid = control.uuid](double speed_ratio) {
                if (!this->hydraulic_data) return false;
                for (const HydraulicControlSimple &candidate :
                     this->hydraulic_data->networkHydraulic().controls_simple)
                {
                    if (candidate.uuid != control_uuid || candidate.link_uuid != this->pump_uuid)
                        continue;
                    if (candidate.action != HydraulicControlActionType::Setting) return false;
                    return updatePumpSimpleControl(control_uuid,
                        [speed_ratio](HydraulicControlSimple &updated) {
                            updated.setting = HydraulicControlLinkSetting();
                            updated.setting.pump_speed_ratio = speed_ratio;
                            updated.action = HydraulicControlActionType::Setting;
                        });
                }
                return false;
            },
            AOWIS_QUANTITY("pump_speed_ratio"), "1", 0.0, 1000.0, 0.05, 3, this->table_controls,
            [](const QString &) { return QStringLiteral(" ×"); });
        speed->refreshUnits(UnitProfileManager::instance().presentation());
        this->table_controls->setCellWidget(row, 5, speed);
        QPushButton *remove = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Remove"), this->table_controls);
        this->table_controls->setCellWidget(row, 6, remove);
        connect(remove, &QPushButton::clicked, this, [this, control_uuid = control.uuid]()
        {
            this->hydraulic_data->removePumpSimpleControl(this->pump_uuid, control_uuid);
        });
    }

    this->table_controls->resizeRowsToContents();

    refreshPumpRuleSummary();
}

void EntityInspectorPump::refreshPumpRuleSummary()
{
    if (!this->hydraulic_data || !this->label_rule_controls)
        return;
    const int count = ruleCountForPump(this->hydraulic_data->networkHydraulic(), this->pump_uuid);
    this->label_rule_controls->setVisible(count > 0);
    if (count > 0)
        this->label_rule_controls->setText(QCoreApplication::translate(
            "EntityInspectorPump",
            "%n rule-based control also targets this pump. The controls above are the pump's simple EPANET controls.",
            nullptr, count));
}

void EntityInspectorPump::populateSpeedPatternCombo(const QUuid &pattern_uuid)
{
    this->combo_speed_pattern->clear();
    EntityInspectorText::addChoice(this->combo_speed_pattern,
        QT_TRANSLATE_NOOP("LiveUiText", "Fixed speed"));
    this->combo_speed_pattern->setItemData(
        this->combo_speed_pattern->count() - 1, QUuid(), uuid_role);

    bool selected_pattern_exists = pattern_uuid.isNull();
    const QList<HydraulicPatternTime> &patterns =
        this->hydraulic_data->networkHydraulic().patterns_time;
    for (const HydraulicPatternTime &pattern : patterns)
    {
        this->combo_speed_pattern->addItem(entityName(pattern.id, pattern.uuid));
        this->combo_speed_pattern->setItemData(
            this->combo_speed_pattern->count() - 1, pattern.uuid, uuid_role);
        if (pattern.uuid == pattern_uuid)
            selected_pattern_exists = true;
    }

    if (!pattern_uuid.isNull() && !selected_pattern_exists)
    {
        EntityInspectorText::addFormattedChoice(this->combo_speed_pattern,
            QT_TRANSLATE_NOOP("LiveUiText", "[Missing Pattern] %1"),
            pattern_uuid.toString(QUuid::WithoutBraces));
        this->combo_speed_pattern->setItemData(
            this->combo_speed_pattern->count() - 1, pattern_uuid, uuid_role);
    }

    int selected_index = 0;
    for (int index = 0; index < this->combo_speed_pattern->count(); index++)
    {
        if (this->combo_speed_pattern->itemData(index, uuid_role).toUuid() == pattern_uuid)
        {
            selected_index = index;
            break;
        }
    }
    this->combo_speed_pattern->setCurrentIndex(selected_index);
}

void EntityInspectorPump::populateEfficiencyInputCombo(const HydraulicLinkPump &pump)
{
    this->combo_efficiency_curve->clear();
    EntityInspectorText::addChoice(this->combo_efficiency_curve, QT_TRANSLATE_NOOP("LiveUiText", "Global"), static_cast<int>(HydraulicLinkPumpEfficiencyInputType::Global));
    EntityInspectorText::addFormattedChoice(this->combo_efficiency_curve,
        QT_TRANSLATE_NOOP("LiveUiText", "Constant (%1%)"),
        QString::number(pump.constant_efficiency_percent, 'f', 1),
        static_cast<int>(HydraulicLinkPumpEfficiencyInputType::Constant));

    bool selected_curve_exists = pump.efficiency_curve_uuid.isNull();
    const QList<HydraulicCurvePumpEfficiency> &curves =
        this->hydraulic_data->networkHydraulic().curves_pump_efficiency;
    for (const HydraulicCurvePumpEfficiency &curve : curves)
    {
        this->combo_efficiency_curve->addItem(
            entityName(curve.id, curve.uuid),
            static_cast<int>(HydraulicLinkPumpEfficiencyInputType::Curve));
        this->combo_efficiency_curve->setItemData(
            this->combo_efficiency_curve->count() - 1, curve.uuid, uuid_role);
        if (curve.uuid == pump.efficiency_curve_uuid)
            selected_curve_exists = true;
    }

    if (pump.efficiency_input_type == HydraulicLinkPumpEfficiencyInputType::Curve &&
        !pump.efficiency_curve_uuid.isNull() && !selected_curve_exists)
    {
        EntityInspectorText::addFormattedChoice(this->combo_efficiency_curve,
            QT_TRANSLATE_NOOP("LiveUiText", "[Missing Curve] %1"),
            pump.efficiency_curve_uuid.toString(QUuid::WithoutBraces),
            static_cast<int>(HydraulicLinkPumpEfficiencyInputType::Curve));
        this->combo_efficiency_curve->setItemData(
            this->combo_efficiency_curve->count() - 1,
            pump.efficiency_curve_uuid, uuid_role);
    }

    int selected_index = 0;
    for (int index = 0; index < this->combo_efficiency_curve->count(); index++)
    {
        const HydraulicLinkPumpEfficiencyInputType item_type =
            static_cast<HydraulicLinkPumpEfficiencyInputType>(
                this->combo_efficiency_curve->itemData(index, value_role).toInt());
        const QUuid item_uuid =
            this->combo_efficiency_curve->itemData(index, uuid_role).toUuid();
        if (item_type == pump.efficiency_input_type &&
            (item_type != HydraulicLinkPumpEfficiencyInputType::Curve ||
             item_uuid == pump.efficiency_curve_uuid))
        {
            selected_index = index;
            break;
        }
    }
    this->combo_efficiency_curve->setCurrentIndex(selected_index);
}

void EntityInspectorPump::populatePricePatternCombo(const HydraulicLinkPump &pump)
{
    this->combo_price_pattern->clear();
    EntityInspectorText::addChoice(this->combo_price_pattern, QT_TRANSLATE_NOOP("LiveUiText", "Global"), static_cast<int>(HydraulicLinkPumpEnergyPriceInputType::Global));
    EntityInspectorText::addChoice(this->combo_price_pattern, QT_TRANSLATE_NOOP("LiveUiText", "Constant"), static_cast<int>(HydraulicLinkPumpEnergyPriceInputType::Constant));

    bool selected_pattern_exists = pump.price_pattern_uuid.isNull();
    const QList<HydraulicPatternTime> &patterns =
        this->hydraulic_data->networkHydraulic().patterns_time;
    for (const HydraulicPatternTime &pattern : patterns)
    {
        this->combo_price_pattern->addItem(
            entityName(pattern.id, pattern.uuid),
            static_cast<int>(HydraulicLinkPumpEnergyPriceInputType::Pattern));
        this->combo_price_pattern->setItemData(
            this->combo_price_pattern->count() - 1, pattern.uuid, uuid_role);
        if (pattern.uuid == pump.price_pattern_uuid)
            selected_pattern_exists = true;
    }

    if (pump.energy_price_input_type == HydraulicLinkPumpEnergyPriceInputType::Pattern &&
        !pump.price_pattern_uuid.isNull() && !selected_pattern_exists)
    {
        EntityInspectorText::addFormattedChoice(this->combo_price_pattern,
            QT_TRANSLATE_NOOP("LiveUiText", "[Missing Pattern] %1"),
            pump.price_pattern_uuid.toString(QUuid::WithoutBraces),
            static_cast<int>(HydraulicLinkPumpEnergyPriceInputType::Pattern));
        this->combo_price_pattern->setItemData(
            this->combo_price_pattern->count() - 1, pump.price_pattern_uuid, uuid_role);
    }

    int selected_index = 0;
    for (int index = 0; index < this->combo_price_pattern->count(); index++)
    {
        const HydraulicLinkPumpEnergyPriceInputType item_type =
            static_cast<HydraulicLinkPumpEnergyPriceInputType>(
                this->combo_price_pattern->itemData(index, value_role).toInt());
        const QUuid item_uuid = this->combo_price_pattern->itemData(index, uuid_role).toUuid();
        if (item_type == pump.energy_price_input_type &&
            (item_type != HydraulicLinkPumpEnergyPriceInputType::Pattern ||
             item_uuid == pump.price_pattern_uuid))
        {
            selected_index = index;
            break;
        }
    }
    this->combo_price_pattern->setCurrentIndex(selected_index);
}
