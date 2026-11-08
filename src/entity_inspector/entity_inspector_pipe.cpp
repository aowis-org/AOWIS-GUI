#include "entity_inspector/entity_inspector_text.h"
#include <aowis/model/units/quantity.h>
#include "entity_inspector/entity_inspector_pipe.h"

#include "config/unit_aware_view.h"
#include "config/unit_bulk_reaction_display.h"
#include "config/unit_numeric_spin_box.h"
#include "config/unit_profile_manager.h"

#include <optional>
#include <cmath>

#include <QDate>
#include <QSignalBlocker>

EntityInspectorPipe::EntityInspectorPipe(HydraulicData *hydraulic_data, const HydraulicLinkPipe &pipe, QWidget *parent)
    : EntityInspectorWidget(hydraulic_data, parent), hydraulic_data(hydraulic_data), pipe_uuid(pipe.uuid)
{
    addGroupOverviewImage(":/icon/pipe.png", pipe.id);

    addGroupGeneral(QString());
    addGroupEndpoints();
    addGroupGeometry();
    addGroupRoughness();
    addGroupQuality();
    addGroupAlerts();
    addGroupHistory();

    bindHydraulicLink(InfrastructureEntity::Pipe, this->pipe_uuid, "Pipe");
    addGroupSimulation();
    addGroupWaterQualitySimulation();
    addStretches();

    connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, this,
            [this](InfrastructureEntity entity_type, const QUuid &uuid)
    {
        if (entity_type == InfrastructureEntity::Pipe && uuid == this->pipe_uuid)
            refreshPipe();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]()
    {
        refreshPipe();
    });

    refreshPipe();
}

void EntityInspectorPipe::addGroupGeometry()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Geometry"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_status_initial = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Initial Status"), nullptr);
    this->combo_status_initial = new QComboBox();
    EntityInspectorText::addChoice(this->combo_status_initial, QT_TRANSLATE_NOOP("LiveUiText", "Open"), static_cast<int>(HydraulicLinkPipeInitialStatus::Open));
    EntityInspectorText::addChoice(this->combo_status_initial, QT_TRANSLATE_NOOP("LiveUiText", "Closed"), static_cast<int>(HydraulicLinkPipeInitialStatus::Closed));
    EntityInspectorText::addChoice(this->combo_status_initial, QT_TRANSLATE_NOOP("LiveUiText", "Check Valve"), static_cast<int>(HydraulicLinkPipeInitialStatus::CheckValve));

    // The widget value is only a representation. Bindings read the authoritative
    // pipe property on every refresh, so conversion/rounding cannot modify it.
    const auto geometry_editor = [this, group](int field) -> QDoubleSpinBox * {
        const std::string quantity = field == 0 ? AOWIS_QUANTITY("link_diameter") : AOWIS_QUANTITY("length");
        const std::string storage_unit = field == 0 ? "mm" : "m";
        const double minimum = field == 0 ? 1.0 : 0.0;
        const double maximum = field == 0 ? 5000.0 : 1000000000.0;
        const double step = field == 0 ? 10.0 : 1.0;
        const int decimals = field == 0 ? 1 : 2;
        return new UnitNumericSpinBox(
            [this, field]() -> std::optional<double> {
                if (!this->hydraulic_data) return std::nullopt;
                const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
                if (!pipe) return std::nullopt;
                if (field == 0) return pipe->diameter_mm;
                if (field == 1) return pipe->length_calculated_m;
                return pipe->length_measured_m.value_or(pipe->length_calculated_m);
            },
            [this, field](double storage_value) {
                if (!this->hydraulic_data) return false;
                const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
                if (!pipe) return false;
                if (field == 0)
                    return this->hydraulic_data->setPipeDiameterMm(this->pipe_uuid, storage_value);
                if (field == 2 && pipe->length_measured_m.has_value())
                    return this->hydraulic_data->setPipeMeasuredLengthM(this->pipe_uuid, storage_value);
                return false; // Calculated length can never be written from the UI.
            }, quantity, storage_unit, minimum, maximum, step, decimals, group);
    };

    QLabel *label_diameter = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Diameter"), nullptr);
    this->spin_diameter = geometry_editor(0);

    QLabel *label_length_calculated = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Calculated Length"), nullptr);
    this->spin_length_calculated = geometry_editor(1);
    this->spin_length_calculated->setReadOnly(true);
    this->spin_length_calculated->setButtonSymbols(QAbstractSpinBox::NoButtons);
    this->spin_length_calculated->setToolTip("Calculated from the endpoint and intermediate vertex coordinates.");

    this->check_length_measured = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Override with measured length"), nullptr);

    QLabel *label_length_measured = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Measured Length"), nullptr);
    this->spin_length_measured = geometry_editor(2);
    this->spin_length_measured->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    this->spin_length_measured->setEnabled(false);

    grid->addWidget(label_status_initial, 0, 0);
    grid->addWidget(this->combo_status_initial, 0, 1);
    grid->addWidget(label_diameter, 1, 0);
    grid->addWidget(this->spin_diameter, 1, 1);
    grid->addWidget(label_length_calculated, 2, 0);
    grid->addWidget(this->spin_length_calculated, 2, 1);
    grid->addWidget(this->check_length_measured, 3, 0, 1, 2);
    grid->addWidget(label_length_measured, 4, 0);
    grid->addWidget(this->spin_length_measured, 4, 1);

    connect(this->combo_status_initial, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const HydraulicLinkPipeInitialStatus initial_status = static_cast<HydraulicLinkPipeInitialStatus>(
            this->combo_status_initial->currentData().toInt());
        this->hydraulic_data->setPipeInitialStatus(this->pipe_uuid, initial_status);
    });
    connect(this->check_length_measured, &QCheckBox::toggled, this, [this](bool use_measured_length)
    {
        if (!use_measured_length)
        {
            this->hydraulic_data->setPipeMeasuredLengthM(this->pipe_uuid, std::nullopt);
            return;
        }

        const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
        if (!pipe.has_value())
            return;

        const double measured_length_m = pipe->length_measured_m.value_or(pipe->length_calculated_m);
        this->hydraulic_data->setPipeMeasuredLengthM(this->pipe_uuid, measured_length_m);
    });

    layoutConfiguration()->addWidget(group);
    // The group's children, unlike the checkable group itself, are hidden on
    // collapse. Listen at one child so collapsed geometry costs no unit work.
    new UnitAwareView(this->spin_diameter, UnitProfileManager::instance().presentation(),
        [this] { refreshPipeGeometry(); });
}

void EntityInspectorPipe::addGroupRoughness()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Roughness"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_material = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Material"), nullptr);
    this->combo_material = new QComboBox();
    this->combo_material->setEditable(false);
    this->combo_material->setToolTip(
        "Pipe materials are defined by the network material library.");

    QLabel *label_roughness_mode = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Roughness<br>Source"), nullptr);
    this->combo_roughness_mode = new QComboBox();
    EntityInspectorText::addChoice(this->combo_roughness_mode, QT_TRANSLATE_NOOP("LiveUiText", "Explicit"), static_cast<int>(HydraulicPipeRoughnessMode::Explicit));
    EntityInspectorText::addChoice(this->combo_roughness_mode, QT_TRANSLATE_NOOP("LiveUiText", "Material + age"), static_cast<int>(HydraulicPipeRoughnessMode::MaterialLibrary));
    this->combo_roughness_mode->setToolTip(
        "Explicit uses the pipe roughness values below. Material + installation age "
        "uses the newest applicable age row in the selected material library entry.");

    // Roughness coefficients are independent quantities. Only absolute
    // Darcy-Weisbach roughness has a length dimension; H-W, C-M, and the
    // minor-loss coefficient retain dimensionless presentation.
    const auto roughness_editor = [this, group](int field) -> QDoubleSpinBox * {
        const std::string quantity = field == 0 ? AOWIS_QUANTITY("hazen_williams_roughness_coefficient")
            : field == 1 ? AOWIS_QUANTITY("darcy_weisbach_roughness_height")
            : field == 2 ? AOWIS_QUANTITY("chezy_manning_roughness_coefficient") : AOWIS_QUANTITY("minor_loss_coefficient");
        const std::string storage_unit = field == 1 ? "mm" : "1";
        const double minimum = field == 0 ? 1.0 : field == 1 ? 0.000001 : field == 2 ? 0.001 : 0.0;
        const double maximum = field == 0 ? 200.0 : field == 1 ? 100.0 : field == 2 ? 0.1 : 1000.0;
        const double step = field == 0 ? 1.0 : field == 1 ? 0.001 : field == 2 ? 0.001 : 0.1;
        const int decimals = field == 0 ? 0 : field == 1 ? 6 : field == 2 ? 4 : 3;
        return new UnitNumericSpinBox(
            [this, field]() -> std::optional<double> {
                if (!this->hydraulic_data) return std::nullopt;
                const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
                if (!pipe) return std::nullopt;
                if (field == 0) return pipe->roughness_hazen_williams;
                if (field == 1) return pipe->roughness_darcy_weisbach_mm;
                if (field == 2) return pipe->roughness_chezy_manning;
                return pipe->minor_loss_coefficient;
            },
            [this, field](double storage_value) {
                if (!this->hydraulic_data) return false;
                const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
                if (!pipe) return false;
                if (field == 3)
                    return this->hydraulic_data->setPipeMinorLoss(this->pipe_uuid, storage_value);
                // A pending edit cannot turn a material-derived roughness into
                // an explicit value, or modify an inactive headloss formula.
                if (pipe->roughness_mode != HydraulicPipeRoughnessMode::Explicit)
                    return false;
                const HydraulicHeadlossFormula formula =
                    this->hydraulic_data->networkHydraulic().options_hydraulic.headloss_formula;
                if (field == 0 && formula == HydraulicHeadlossFormula::HazenWilliams)
                    return this->hydraulic_data->setPipeRoughnessHw(this->pipe_uuid, storage_value);
                if (field == 1 && formula == HydraulicHeadlossFormula::DarcyWeisbach)
                    return this->hydraulic_data->setPipeRoughnessDwMm(this->pipe_uuid, storage_value);
                if (field == 2 && formula == HydraulicHeadlossFormula::ChezyManning)
                    return this->hydraulic_data->setPipeRoughnessCm(this->pipe_uuid, storage_value);
                return false;
            }, quantity, storage_unit, minimum, maximum, step, decimals, group,
            field == 1 ? std::function<QString(const QString &)>()
                       : std::function<QString(const QString &)>(
                             [](const QString &) { return QString(); }));
    };

    QLabel *label_roughness_hw = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Roughness<br>Hazen-Williams"), nullptr);
    label_roughness_hw->setWordWrap(true);
    this->spin_roughness_hw = roughness_editor(0);
    this->spin_roughness_hw->setToolTip("Pipe roughness coefficient C");

    QLabel *label_roughness_dw = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Roughness<br>Darcy-Weisbach"), nullptr);
    label_roughness_dw->setWordWrap(true);
    this->spin_roughness_dw = roughness_editor(1);
    this->spin_roughness_dw->setToolTip("Absolute pipe roughness ε in the selected display unit");

    QLabel *label_roughness_cm = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Roughness<br>Chezy-Manning"), nullptr);
    label_roughness_cm->setWordWrap(true);
    this->spin_roughness_cm = roughness_editor(2);
    this->spin_roughness_cm->setToolTip("Manning roughness coefficient n");

    QLabel *label_loss_coefficient = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Loss Coefficient"), nullptr);
    label_loss_coefficient->setWordWrap(true);
    this->spin_loss_coefficient = roughness_editor(3);
    this->spin_loss_coefficient->setToolTip(QStringLiteral("Dimensionless minor loss coefficient K<br>for local losses from bends, fittings,<br>entrances, exits, etc.<br><br>Default is 0 for no additional minor losses."));

    grid->addWidget(label_material, 0, 0);
    grid->addWidget(this->combo_material, 0, 1);
    grid->addWidget(label_roughness_mode, 1, 0);
    grid->addWidget(this->combo_roughness_mode, 1, 1);
    grid->addWidget(label_roughness_hw, 2, 0);
    grid->addWidget(this->spin_roughness_hw, 2, 1);
    grid->addWidget(label_roughness_dw, 3, 0);
    grid->addWidget(this->spin_roughness_dw, 3, 1);
    grid->addWidget(label_roughness_cm, 4, 0);
    grid->addWidget(this->spin_roughness_cm, 4, 1);
    grid->addWidget(label_loss_coefficient, 5, 0);
    grid->addWidget(this->spin_loss_coefficient, 5, 1);

    connect(this->combo_material, &QComboBox::currentIndexChanged, this, [this](int index)
    {
        if (index < 0)
            return;
        this->hydraulic_data->setPipeMaterialUuid(
            this->pipe_uuid, this->combo_material->currentData().toUuid());
    });
    connect(this->combo_roughness_mode, &QComboBox::currentIndexChanged, this, [this](int index)
    {
        if (index < 0)
            return;
        if (!this->hydraulic_data->setPipeRoughnessMode(
                this->pipe_uuid,
                static_cast<HydraulicPipeRoughnessMode>(
                    this->combo_roughness_mode->currentData().toInt())))
        {
            refreshPipe();
        }
    });
    layoutConfiguration()->addWidget(group);
    // Collapsed group children remain hidden: changing the unit profile then
    // costs no roughness conversions until the group becomes visible again.
    new UnitAwareView(this->spin_roughness_hw, UnitProfileManager::instance().presentation(),
        [this] {
            if (!this->hydraulic_data) return;
            const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
            if (pipe) updateRoughnessUi(*pipe);
        });
}

void EntityInspectorPipe::refreshPipeGeometry()
{
    if (!this->hydraulic_data || this->pipe_uuid.isNull())
        return;
    const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
    if (!pipe) return;

    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    static_cast<UnitNumericSpinBox *>(this->spin_diameter)->refreshUnits(context);
    // Calculated length is read from the Model, even if its rounded visual
    // representation is too coarse to show the original precision.
    static_cast<UnitNumericSpinBox *>(this->spin_length_calculated)->refreshDerived(
        context, pipe->length_calculated_m);

    const bool measured = pipe->length_measured_m.has_value();
    const QSignalBlocker override_blocker(this->check_length_measured);
    this->check_length_measured->setChecked(measured);
    UnitNumericSpinBox *measured_editor = static_cast<UnitNumericSpinBox *>(this->spin_length_measured);
    measured_editor->setReadOnly(!measured);
    if (measured) {
        measured_editor->setSpecialValueText(QString());
        measured_editor->refreshUnits(context);
    } else {
        // Switching off the override abandons any stale pending edit.
        measured_editor->refreshDerived(context, pipe->length_calculated_m);
    }
    measured_editor->setEnabled(measured);
}

void EntityInspectorPipe::refreshPipe()
{
    if (!this->hydraulic_data || this->pipe_uuid.isNull())
        return;

    const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
    if (!pipe.has_value())
        return;

    const QSignalBlocker status_blocker(this->combo_status_initial);
    const QSignalBlocker diameter_blocker(this->spin_diameter);
    const QSignalBlocker calculated_length_blocker(this->spin_length_calculated);
    const QSignalBlocker measured_length_enabled_blocker(this->check_length_measured);
    const QSignalBlocker measured_length_blocker(this->spin_length_measured);
    const QSignalBlocker material_blocker(this->combo_material);
    const QSignalBlocker roughness_mode_blocker(this->combo_roughness_mode);
    const QSignalBlocker roughness_hw_blocker(this->spin_roughness_hw);
    const QSignalBlocker roughness_dw_blocker(this->spin_roughness_dw);
    const QSignalBlocker roughness_cm_blocker(this->spin_roughness_cm);
    const QSignalBlocker minor_loss_blocker(this->spin_loss_coefficient);
    const QSignalBlocker quality_bulk_override_blocker(this->check_override_bulk);
    const QSignalBlocker quality_wall_override_blocker(this->check_override_wall);
    const QSignalBlocker quality_bulk_blocker(this->spin_bulk_reaction);
    const QSignalBlocker quality_wall_blocker(this->spin_wall_reaction);
    const QSignalBlocker quality_wall_zero_blocker(this->spin_wall_reaction_zero);

    const int status_index = this->combo_status_initial->findData(static_cast<int>(pipe->initial_status));
    this->combo_status_initial->setCurrentIndex(status_index >= 0 ? status_index : 0);
    refreshPipeGeometry();

    this->combo_material->clear();
    EntityInspectorText::addChoice(this->combo_material, QT_TRANSLATE_NOOP("LiveUiText", "[Not set]"), QUuid());
    for (const HydraulicPipeMaterial &material : this->hydraulic_data->networkHydraulic().pipe_materials)
        this->combo_material->addItem(material.id, material.uuid);

    const int material_index = this->combo_material->findData(pipe->material_uuid);
    this->combo_material->setCurrentIndex(material_index >= 0 ? material_index : 0);

    const int roughness_mode_index = this->combo_roughness_mode->findData(
        static_cast<int>(pipe->roughness_mode));
    this->combo_roughness_mode->setCurrentIndex(
        roughness_mode_index >= 0 ? roughness_mode_index : 0);

    updateRoughnessUi(pipe.value());
    refreshPipeQuality();
}

void EntityInspectorPipe::addGroupQuality()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Chemical — Pipe Reactions"));
    QGridLayout *grid = new QGridLayout(group);
    int row = 0;

    this->check_override_bulk = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Override global bulk reaction"), nullptr);
    this->check_override_wall = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Override global wall reaction"), nullptr);

    QLabel *label_bulk = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Bulk Reaction Coefficient"), nullptr);
    label_bulk->setWordWrap(true);
    this->spin_bulk_reaction = new UnitNumericSpinBox(
        [this]() -> std::optional<double> {
            if (!this->hydraulic_data) return std::nullopt;
            const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
            return pipe ? std::optional<double>(pipe->bulk_reaction.coefficient) : std::nullopt;
        },
        [this](double coefficient) {
            if (!this->hydraulic_data) return false;
            const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
            if (!pipe || !pipe->override_bulk_reaction || !std::isfinite(pipe->bulk_reaction.order))
                return false;
            return this->hydraulic_data->setPipeBulkReactionCoefficient(this->pipe_uuid, coefficient);
        }, AOWIS_QUANTITY("first_order_bulk_reaction_coefficient"), "/d", -1000000.0, 1000000.0,
        0.001, 12, group,
        [this](const QString &inverse_time) {
            if (!this->hydraulic_data) return QString();
            const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
            return pipe ? UnitBulkReactionDisplay::suffix(pipe->bulk_reaction.order, inverse_time)
                        : QString();
        });
    this->spin_bulk_reaction->setToolTip(
        "Coefficient dimensions depend on bulk reaction order; the profile changes the "
        "inverse-time unit while retaining canonical concentration (mg/L).");

    QLabel *label_wall = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Wall Reaction Coefficient"), nullptr);
    label_wall->setWordWrap(true);
    this->stack_wall_reaction = new QStackedWidget(group);
    // First-order wall coefficient is length/time (m/day); zero-order wall
    // coefficient is mass/surface/time (mg/m²/day). Their unit registries must
    // remain distinct, so use two bindings for the two physical dimensions.
    const auto wall_reader = [this]() -> std::optional<double> {
        if (!this->hydraulic_data) return std::nullopt;
        const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
        return pipe ? std::optional<double>(pipe->wall_reaction.coefficient) : std::nullopt;
    };
    const auto wall_writer = [this](double expected_order, double coefficient) {
        if (!this->hydraulic_data) return false;
        const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
        if (!pipe || !pipe->override_wall_reaction || pipe->wall_reaction.order != expected_order)
            return false;
        return this->hydraulic_data->setPipeWallReactionCoefficient(this->pipe_uuid, coefficient);
    };
    this->spin_wall_reaction = new UnitNumericSpinBox(
        wall_reader,
        [wall_writer](double coefficient) { return wall_writer(1.0, coefficient); },
        AOWIS_QUANTITY("first_order_wall_reaction_coefficient"), "m/d", -1000000.0, 1000000.0,
        0.001, 12, this->stack_wall_reaction);
    this->spin_wall_reaction_zero = new UnitNumericSpinBox(
        wall_reader,
        [wall_writer](double coefficient) { return wall_writer(0.0, coefficient); },
        AOWIS_QUANTITY("zero_order_wall_reaction_coefficient"), "mg/m2/d", -1000000.0, 1000000.0,
        0.001, 12, this->stack_wall_reaction);
    this->stack_wall_reaction->addWidget(this->spin_wall_reaction);
    this->stack_wall_reaction->addWidget(this->spin_wall_reaction_zero);
    this->stack_wall_reaction->setToolTip(
        "First-order wall reaction: length/time (m/day or ft/day). "
        "Zero-order wall reaction: mass/area/time (mg/m²/day or selected compatible unit).");

    grid->addWidget(this->check_override_bulk, row++, 0, 1, 2);
    grid->addWidget(label_bulk, row, 0);
    grid->addWidget(this->spin_bulk_reaction, row++, 1);
    grid->addWidget(this->check_override_wall, row++, 0, 1, 2);
    grid->addWidget(label_wall, row, 0);
    grid->addWidget(this->stack_wall_reaction, row++, 1);

    connect(this->check_override_bulk, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!enabled) static_cast<UnitNumericSpinBox *>(this->spin_bulk_reaction)->cancelEdit();
        this->hydraulic_data->setPipeOverrideBulkReaction(this->pipe_uuid, enabled);
        updateQualityUi();
    });
    connect(this->check_override_wall, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!enabled) {
            static_cast<UnitNumericSpinBox *>(this->spin_wall_reaction)->cancelEdit();
            static_cast<UnitNumericSpinBox *>(this->spin_wall_reaction_zero)->cancelEdit();
        }
        this->hydraulic_data->setPipeOverrideWallReaction(this->pipe_uuid, enabled);
        updateQualityUi();
    });

    layoutQuality()->addWidget(group);
    // A single refresh subscription for the entire reaction group. Collapsed
    // groups do not convert either coefficient until their contents appear.
    new UnitAwareView(this->spin_bulk_reaction, UnitProfileManager::instance().presentation(),
        [this] { refreshPipeQuality(); });
}

void EntityInspectorPipe::refreshPipeQuality()
{
    if (!this->hydraulic_data) return;
    const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
    if (!pipe) return;

    const QSignalBlocker bulk_override_blocker(this->check_override_bulk);
    const QSignalBlocker wall_override_blocker(this->check_override_wall);
    this->check_override_bulk->setChecked(pipe->override_bulk_reaction);
    this->check_override_wall->setChecked(pipe->override_wall_reaction);

    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    static_cast<UnitNumericSpinBox *>(this->spin_bulk_reaction)->refreshUnits(context);
    const bool first_order = pipe->wall_reaction.order == 1.0;
    const bool zero_order = pipe->wall_reaction.order == 0.0;
    const int selected_index = first_order ? 0 : zero_order ? 1 : -1;
    if (selected_index != this->stack_wall_reaction->currentIndex()) {
        static_cast<UnitNumericSpinBox *>(this->spin_wall_reaction)->cancelEdit();
        static_cast<UnitNumericSpinBox *>(this->spin_wall_reaction_zero)->cancelEdit();
        if (selected_index >= 0) this->stack_wall_reaction->setCurrentIndex(selected_index);
    }
    if (first_order)
        static_cast<UnitNumericSpinBox *>(this->spin_wall_reaction)->refreshUnits(context);
    if (zero_order)
        static_cast<UnitNumericSpinBox *>(this->spin_wall_reaction_zero)->refreshUnits(context);
    this->stack_wall_reaction->setToolTip(first_order || zero_order
        ? QStringLiteral("First-order wall reaction: length/time (m/day or ft/day). "
                         "Zero-order wall reaction: mass/area/time (mg/m²/day or compatible unit).")
        : QStringLiteral("Unsupported wall reaction order: expected 0 or 1."));
    updateQualityUi();
}

void EntityInspectorPipe::updateQualityUi()
{
    if (!this->hydraulic_data) return;
    const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
    if (!pipe) return;
    this->spin_bulk_reaction->setEnabled(pipe->override_bulk_reaction &&
                                          std::isfinite(pipe->bulk_reaction.order));
    const bool wall_order_supported = pipe->wall_reaction.order == 0.0 ||
                                      pipe->wall_reaction.order == 1.0;
    this->stack_wall_reaction->setEnabled(pipe->override_wall_reaction && wall_order_supported);
}

void EntityInspectorPipe::updateRoughnessUi(const HydraulicLinkPipe &pipe)
{
    const bool explicit_mode = pipe.roughness_mode == HydraulicPipeRoughnessMode::Explicit;
    std::optional<double> derived_hw;
    std::optional<double> derived_dw;
    std::optional<double> derived_cm;

    if (!explicit_mode && !pipe.material_uuid.isNull() && pipe.metadata.date_installed.has_value())
    {
        const std::optional<int> age_years = hydraulicPipeAgeYears(
            pipe.metadata.date_installed.value(), QDate::currentDate());
        if (age_years.has_value())
        {
            for (const HydraulicPipeMaterial &material : this->hydraulic_data->networkHydraulic().pipe_materials)
            {
                if (material.uuid != pipe.material_uuid) continue;
                derived_hw = resolveHydraulicPipeMaterialRoughness(
                    material, HydraulicHeadlossFormula::HazenWilliams, age_years.value());
                derived_dw = resolveHydraulicPipeMaterialRoughness(
                    material, HydraulicHeadlossFormula::DarcyWeisbach, age_years.value());
                derived_cm = resolveHydraulicPipeMaterialRoughness(
                    material, HydraulicHeadlossFormula::ChezyManning, age_years.value());
                break;
            }
        }
    }

    const HydraulicHeadlossFormula formula =
        this->hydraulic_data->networkHydraulic().options_hydraulic.headloss_formula;
    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    const auto refresh_roughness = [explicit_mode, &context](
        QDoubleSpinBox *spin, const std::optional<double> &derived, bool selected) {
        UnitNumericSpinBox *editor = static_cast<UnitNumericSpinBox *>(spin);
        editor->setReadOnly(!explicit_mode || !selected);
        if (explicit_mode) {
            editor->setSpecialValueText(QString());
            editor->refreshUnits(context);
        } else {
            // Material values are calculated, not the pipe's stored explicit
            // roughness. Even when unavailable we must not show the latter.
            editor->refreshDerived(context, derived);
        }
        editor->setEnabled(explicit_mode && selected);
    };

    refresh_roughness(this->spin_roughness_hw, derived_hw,
                      formula == HydraulicHeadlossFormula::HazenWilliams);
    refresh_roughness(this->spin_roughness_dw, derived_dw,
                      formula == HydraulicHeadlossFormula::DarcyWeisbach);
    refresh_roughness(this->spin_roughness_cm, derived_cm,
                      formula == HydraulicHeadlossFormula::ChezyManning);
    static_cast<UnitNumericSpinBox *>(this->spin_loss_coefficient)->refreshUnits(context);
}

void EntityInspectorPipe::onHeadlossFormulaChanged(HeadlossFormulas formulas)
{
    const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->pipe_uuid);
    const bool explicit_mode = !pipe.has_value()
        || pipe->roughness_mode == HydraulicPipeRoughnessMode::Explicit;
    const bool use_hw = formulas.testFlag(HeadlossFormula::HazenWilliams);
    const bool use_dw = formulas.testFlag(HeadlossFormula::DarcyWeisbach);
    const bool use_cm = formulas.testFlag(HeadlossFormula::ChezyManning);

    this->spin_roughness_hw->setDisabled(!use_hw || !explicit_mode);
    this->spin_roughness_dw->setDisabled(!use_dw || !explicit_mode);
    this->spin_roughness_cm->setDisabled(!use_cm || !explicit_mode);

    const QString derived_suffix = explicit_mode
        ? QString()
        : QStringLiteral("<br><br>Currently derived from the selected material and installation age.");
    this->spin_roughness_hw->setToolTip(
        (use_hw
            ? QStringLiteral("Pipe roughness coefficient C<br>Used by the <b>Hazen-Williams</b> formula.")
            : QStringLiteral("Select <b>Hazen-Williams</b> in the simulation toolbar dropdown<br>to edit the pipe roughness coefficient C."))
        + derived_suffix);
    this->spin_roughness_dw->setToolTip(
        (use_dw
            ? QStringLiteral("Absolute pipe roughness ε in the selected unit<br>Used by the <b>Darcy-Weisbach</b> formula.")
            : QStringLiteral("Select <b>Darcy-Weisbach</b> in the simulation toolbar dropdown<br>to edit the absolute pipe roughness ε."))
        + derived_suffix);
    this->spin_roughness_cm->setToolTip(
        (use_cm
            ? QStringLiteral("Manning roughness coefficient n<br>Used by the <b>Chezy-Manning</b> formula.")
            : QStringLiteral("Select <b>Chezy-Manning</b> in the simulation toolbar dropdown<br>to edit the Manning roughness coefficient n."))
        + derived_suffix);
}
