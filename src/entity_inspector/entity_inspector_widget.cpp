#include "entity_inspector/entity_inspector_text.h"
#include "config/unit_quantity_key.h"
#include "config/unit_numeric_spin_box.h"
#include "config/unit_profile_manager.h"
#include "config/unit_aware_view.h"
#include "simulation/simulation_inspector_unit_presentation.h"
#include "entity_inspector/entity_inspector_widget.h"
#include "entity_inspector/coordinate_numeric_spin_box.h"
#include "config/geographic_altitude_presentation.h"
#include "entity_inspector/emitter_numeric_spin_box.h"

#include "services/interface_server_map_rest.h"
#include "services/map_server_transport_factory.h"
#ifdef AOWIS_STANDALONE
#include "services/interface_server_map_standalone.h"
#endif

#include <cmath>
#include <optional>

#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaEnum>
#include <QMessageBox>
#include <QPixmap>
#include <QSignalBlocker>
#include <QTimer>

namespace
{
// Presentation-only translations for simulation values, diagnostics and editor
// messages. The context is independent of the C++ file location and of MODEL.
QString inspectorText(const char *source)
{
    return QCoreApplication::translate("EntityInspectorPresentation", source);
}

constexpr int pattern_mode_role = Qt::UserRole;
constexpr int pattern_uuid_role = Qt::UserRole + 1;
QString terrainDatumDisplayName(const QString &datum_id)
{
    if (datum_id == QStringLiteral("egm2008"))
        return QStringLiteral("EGM2008");
    if (datum_id == QStringLiteral("egm96"))
        return QStringLiteral("EGM96");
    if (datum_id == QStringLiteral("wgs84-ellipsoid"))
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "WGS84 ellipsoid"));
    if (datum_id == QStringLiteral("local"))
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "local datum"));
    if (datum_id.isEmpty() || datum_id == QStringLiteral("unknown"))
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "unknown vertical datum"));
    return datum_id;
}

// Node-quality properties have the same physical meaning for Junctions,
// Reservoirs, and Tanks. Read each property from the authoritative node,
// never from an editor's rounded presentation value.
enum class NodeQualityNumber { InitialChemical, InitialWaterAge, SourceConcentration, SourceMassFlow };

template<typename Node>
double nodeQualityNumber(const Node &node, NodeQualityNumber field)
{
    switch (field) {
    case NodeQualityNumber::InitialChemical:
        return node.initial_chemical_concentration_mg_per_l;
    case NodeQualityNumber::InitialWaterAge:
        return node.initial_water_age_h;
    case NodeQualityNumber::SourceConcentration:
        return node.quality_source.chemical_concentration_mg_per_l;
    case NodeQualityNumber::SourceMassFlow:
        return node.quality_source.chemical_mass_flow_mg_per_min;
    }
    return 0.0;
}

std::optional<double> readNodeQualityNumber(HydraulicData *data,
    InfrastructureEntity entity_type, const QUuid &uuid, NodeQualityNumber field)
{
    if (!data || uuid.isNull()) return std::nullopt;
    switch (entity_type) {
    case InfrastructureEntity::Junction: {
        const std::optional<HydraulicNodeJunction> node = data->junction(uuid);
        return node ? std::optional<double>(nodeQualityNumber(*node, field)) : std::nullopt;
    }
    case InfrastructureEntity::Reservoir: {
        const std::optional<HydraulicNodeReservoir> node = data->reservoir(uuid);
        return node ? std::optional<double>(nodeQualityNumber(*node, field)) : std::nullopt;
    }
    case InfrastructureEntity::Tank: {
        const std::optional<HydraulicNodeTank> node = data->tank(uuid);
        return node ? std::optional<double>(nodeQualityNumber(*node, field)) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

template<typename Node>
HydraulicNodeQualitySourceType nodeQualitySourceType(const Node &node)
{
    return node.quality_source.type;
}

std::optional<HydraulicNodeQualitySourceType> readNodeQualitySourceType(
    HydraulicData *data, InfrastructureEntity entity_type, const QUuid &uuid)
{
    if (!data || uuid.isNull()) return std::nullopt;
    switch (entity_type) {
    case InfrastructureEntity::Junction: {
        const std::optional<HydraulicNodeJunction> node = data->junction(uuid);
        return node ? std::optional<HydraulicNodeQualitySourceType>(nodeQualitySourceType(*node)) : std::nullopt;
    }
    case InfrastructureEntity::Reservoir: {
        const std::optional<HydraulicNodeReservoir> node = data->reservoir(uuid);
        return node ? std::optional<HydraulicNodeQualitySourceType>(nodeQualitySourceType(*node)) : std::nullopt;
    }
    case InfrastructureEntity::Tank: {
        const std::optional<HydraulicNodeTank> node = data->tank(uuid);
        return node ? std::optional<HydraulicNodeQualitySourceType>(nodeQualitySourceType(*node)) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

bool usesNodeQualityConcentration(HydraulicNodeQualitySourceType source_type)
{
    return source_type == HydraulicNodeQualitySourceType::Concentration
        || source_type == HydraulicNodeQualitySourceType::FlowPacedBooster
        || source_type == HydraulicNodeQualitySourceType::SetpointBooster;
}

QString hydraulicNodeTypeName(InfrastructureEntity entity_type)
{
    switch (entity_type)
    {
    case InfrastructureEntity::Junction:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Junction"));
    case InfrastructureEntity::Reservoir:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Reservoir"));
    case InfrastructureEntity::Tank:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Tank"));
    default:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Node"));
    }
}

QString formatSimulationElapsedTime(quint64 time_elapsed_s)
{
    const quint64 days = time_elapsed_s / 86400;
    const quint64 remainder_after_days = time_elapsed_s % 86400;
    const quint64 hours = remainder_after_days / 3600;
    const quint64 minutes = (remainder_after_days % 3600) / 60;
    const quint64 seconds = remainder_after_days % 60;

    const QString clock = QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));

    if (days == 0)
        return clock;

    return QStringLiteral("%1d %2").arg(days).arg(clock);
}

QString formatSimulationNumber(double value, int decimals, const QString &unit)
{
    return UnitTablePresentation::formatNumber(value, decimals, unit);
}

template<typename ResultType>
const ResultType *simulationResultByUuid(const QList<ResultType> &results, const QUuid &uuid)
{
    for (const ResultType &result : results)
    {
        if (result.uuid == uuid)
            return &result;
    }

    return nullptr;
}

QString pumpStateText(HydraulicSimulationPumpState state)
{
    switch (state)
    {
    case HydraulicSimulationPumpState::CannotSupplyHead:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Cannot supply head"));
    case HydraulicSimulationPumpState::Closed:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Closed"));
    case HydraulicSimulationPumpState::Open:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Open"));
    case HydraulicSimulationPumpState::CannotSupplyFlow:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Cannot supply flow"));
    }

    return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Unknown"));
}

QString pipeRoughnessText(const HydraulicSimulationResultLinkPipe &result,
                          const UnitPresentationContext &presentation)
{
    if (result.roughness_hazen_williams.has_value())
    {
        return formatSimulationNumber(result.roughness_hazen_williams.value(), 3, QString())
            + QStringLiteral(" (Hazen-Williams)");
    }
    if (result.roughness_darcy_weisbach_mm.has_value())
    {
        return SimulationInspectorUnitPresentation::format(
            presentation, AOWIS_QUANTITY("darcy_weisbach_roughness_height"),
            result.roughness_darcy_weisbach_mm.value(), 6)
            + QStringLiteral(" (Darcy-Weisbach)");
    }
    if (result.roughness_chezy_manning.has_value())
    {
        return formatSimulationNumber(result.roughness_chezy_manning.value(), 6, QString())
            + QStringLiteral(" (Chezy-Manning)");
    }

    return QStringLiteral("—");
}

bool isSimulationErrorSeverity(HydraulicSimulationDiagnosticSeverity severity)
{
    return severity == HydraulicSimulationDiagnosticSeverity::Error
        || severity == HydraulicSimulationDiagnosticSeverity::Fatal;
}

bool isSimulationWarningSeverity(HydraulicSimulationDiagnosticSeverity severity)
{
    return severity == HydraulicSimulationDiagnosticSeverity::Warning;
}

QString simulationDiagnosticSeverityText(HydraulicSimulationDiagnosticSeverity severity)
{
    switch (severity)
    {
    case HydraulicSimulationDiagnosticSeverity::Fatal:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Fatal"));
    case HydraulicSimulationDiagnosticSeverity::Error:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Error"));
    case HydraulicSimulationDiagnosticSeverity::Warning:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Warning"));
    case HydraulicSimulationDiagnosticSeverity::Information:
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Info"));
    }

    return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Error"));
}

QString simulationDiagnosticSeverityColor(HydraulicSimulationDiagnosticSeverity severity)
{
    // Matches the severity colors already used for the simulation results'
    // diagnostics list (see simulation_diagnostics_widget.cpp).
    return severity == HydraulicSimulationDiagnosticSeverity::Warning
        ? QStringLiteral("#c77800") : QStringLiteral("#d00000");
}

QString simulationStatusEnumText(const char *enumerator_name, int value)
{
    const QMetaObject &meta_object = HydraulicSimulationStatusEnums::staticMetaObject;
    const int enumerator_index = meta_object.indexOfEnumerator(enumerator_name);
    if (enumerator_index < 0)
        return QString();

    const QMetaEnum meta_enum = meta_object.enumerator(enumerator_index);
    const char *key = meta_enum.valueToKey(value);
    return key == nullptr ? QString() : QString::fromLatin1(key);
}

QString simulationDiagnosticContext(const HydraulicSimulationDiagnostic &diagnostic)
{
    QStringList context;

    if (diagnostic.stage != HydraulicSimulationStatusStage::None)
    {
        context.append(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Stage: %1")).arg(
            simulationStatusEnumText("Stage", static_cast<int>(diagnostic.stage))));
    }
    if (diagnostic.operation != HydraulicSimulationStatusOperation::None)
    {
        context.append(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Operation: %1")).arg(
            simulationStatusEnumText("Operation", static_cast<int>(diagnostic.operation))));
    }
    if (diagnostic.property != HydraulicSimulationStatusProperty::None)
    {
        context.append(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Property: %1")).arg(
            simulationStatusEnumText("Property", static_cast<int>(diagnostic.property))));
    }
    if (!diagnostic.backend_name.isEmpty())
        context.append(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Backend: %1")).arg(diagnostic.backend_name));
    if (!diagnostic.backend_operation.isEmpty())
    {
        context.append(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Backend operation: %1"))
                           .arg(diagnostic.backend_operation));
    }
    if (diagnostic.backend_error_code != 0)
        context.append(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Backend code: %1")).arg(diagnostic.backend_error_code));

    return context.join(QStringLiteral(" · "));
}

HydraulicSimulationDiagnostic simulationErrorFromStatus(
    const HydraulicSimulationStatus &status)
{
    HydraulicSimulationDiagnostic diagnostic;
    diagnostic.severity = HydraulicSimulationDiagnosticSeverity::Error;
    diagnostic.stage = status.stage;
    diagnostic.operation = status.operation;
    diagnostic.property = status.property;
    diagnostic.entity = status.entity;
    diagnostic.message = status.message;
    diagnostic.details = status.details;
    diagnostic.backend_name = status.backend_name;
    diagnostic.backend_error_code = status.backend_error_code;
    diagnostic.backend_operation = status.backend_operation;
    diagnostic.message_backend = status.message_backend;
    return diagnostic;
}

QString simulationDiagnosticHtml(const HydraulicSimulationDiagnostic &diagnostic, bool stale)
{
    const QString color = stale
        ? QStringLiteral("#808080")
        : simulationDiagnosticSeverityColor(diagnostic.severity);
    const QString default_message = isSimulationErrorSeverity(diagnostic.severity)
        ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Simulation error")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Simulation warning"));
    const QString message = diagnostic.message.isEmpty() ? default_message : diagnostic.message;

    QString html = QStringLiteral(
        "<p><span style=\"color:%1; font-weight:600;\">%2</span><br>"
        "<b>%3</b>")
        .arg(color,
             simulationDiagnosticSeverityText(diagnostic.severity).toHtmlEscaped(),
             message.toHtmlEscaped());

    const QString context = simulationDiagnosticContext(diagnostic);
    if (!context.isEmpty())
    {
        html += QStringLiteral("<br><span style=\"color:%1;\">%2</span>")
                    .arg(stale ? QStringLiteral("#808080") : QStringLiteral("#606060"),
                         context.toHtmlEscaped());
    }

    if (!diagnostic.message_backend.isEmpty()
        && diagnostic.message_backend != diagnostic.message)
    {
        html += QStringLiteral("<br><b>%1</b> %2")
                    .arg(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Backend message:")).toHtmlEscaped(),
                         diagnostic.message_backend.toHtmlEscaped());
    }

    if (!diagnostic.details.isEmpty())
    {
        html += QStringLiteral("<br><b>%1</b><ul>").arg(
            inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Details")).toHtmlEscaped());
        for (const QString &detail : diagnostic.details)
            html += QStringLiteral("<li>%1</li>").arg(detail.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    }

    html += QStringLiteral("</p>");
    return html;
}

}

EntityInspectorWidget::EntityInspectorWidget(HydraulicData *hydraulic_data, QWidget *parent)
    : QWidget(parent),
    hydraulic_data(hydraulic_data),
    tabs(new QTabWidget(this)),
    layout_main(new QVBoxLayout(this)),
    label_title(new QLabel(this)),
    
    scroll_overview(new QScrollArea(this)),
    widget_overview(new QWidget()),
    layout_overview(new QVBoxLayout(this->widget_overview)),
    
    scroll_configuration(new QScrollArea(this)),
    widget_configuration(new QWidget()),
    layout_configuration(new QVBoxLayout(this->widget_configuration)),
    
    scroll_sim_meas(new QScrollArea(this)),
    widget_sim_meas(new QWidget()),
    layout_sim_meas(new QVBoxLayout(this->widget_sim_meas)),
    
    scroll_quality(new QScrollArea(this)),
    widget_quality(new QWidget()),
    layout_quality(new QVBoxLayout(this->widget_quality)),

    scroll_quality_sim_meas(new QScrollArea(this)),
    widget_quality_sim_meas(new QWidget()),
    layout_quality_sim_meas(new QVBoxLayout(this->widget_quality_sim_meas))
{
    this->scroll_overview->setWidgetResizable(true);
    this->scroll_overview->setWidget(this->widget_overview);
    
    this->scroll_configuration->setWidgetResizable(true);
    this->scroll_configuration->setWidget(this->widget_configuration);
    
    this->scroll_sim_meas->setWidgetResizable(true);
    this->scroll_sim_meas->setWidget(this->widget_sim_meas);
    
    this->scroll_quality->setWidgetResizable(true);
    this->scroll_quality->setWidget(this->widget_quality);

    this->scroll_quality_sim_meas->setWidgetResizable(true);
    this->scroll_quality_sim_meas->setWidget(this->widget_quality_sim_meas);
    
    // Every one of these QVBoxLayouts otherwise falls back to the style's default
    // layout margin/spacing (commonly ~9-11px per side). Stacked across layout_main,
    // the per-tab content layout, and each group's own grid layout inside it, those
    // untouched defaults add up to real, avoidable width in a fixed-width sidebar
    // panel. Tighten them here to match the compact margins already used by the
    // other right-hand docks (see EntityMapLegendDock).
    const QMargins compact_margins(6, 6, 6, 6);
    constexpr int compact_spacing = 4;
    this->layout_main->setContentsMargins(compact_margins);
    this->layout_main->setSpacing(compact_spacing);
    this->layout_overview->setContentsMargins(compact_margins);
    this->layout_overview->setSpacing(compact_spacing);
    this->layout_configuration->setContentsMargins(compact_margins);
    this->layout_configuration->setSpacing(compact_spacing);
    this->layout_sim_meas->setContentsMargins(compact_margins);
    this->layout_sim_meas->setSpacing(compact_spacing);
    this->layout_quality->setContentsMargins(compact_margins);
    this->layout_quality->setSpacing(compact_spacing);
    this->layout_quality_sim_meas->setContentsMargins(compact_margins);
    this->layout_quality_sim_meas->setSpacing(compact_spacing);
    
    this->tabs->setIconSize(QSize(40, 40));
    this->tabs->tabBar()->setStyleSheet(
        "QTabBar::tab"
        "{"
        "    max-width: 40px;"
        "    padding: 5px;"
        "}"
    );
    
    this->tabs->addTab(this->scroll_overview, QIcon(":/icon/inspector_dash.png"), "");
    
    this->tabs->addTab(this->scroll_configuration, QIcon(":/icon/settings_2.png"), "");
    
    this->tabs->addTab(this->scroll_sim_meas, QIcon(":/icon/sim_meas.png"), "");
    
    this->tabs->addTab(this->scroll_quality, QIcon(":/icon/inspector_quality.png"), "");

    this->tabs->addTab(this->scroll_quality_sim_meas, QIcon(":/icon/sensor.png"), "");

    LiveUiText::bind(this->label_title, [this] { refreshEntityTitle(); });
    LiveUiText::bind(this->tabs, [this] {
        this->tabs->setTabToolTip(0, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "General")));
        this->tabs->setTabToolTip(1, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Hydraulic Input")));
        this->tabs->setTabToolTip(2, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Hydraulic Simulation & Measurement")));
        this->tabs->setTabToolTip(3, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Water Quality Input")));
        this->tabs->setTabToolTip(4, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Water Quality Simulation & Measurement")));
    });
    // Refresh cached result captions on the existing inspector. Unlike rebuilding
    // the inspector, this never replaces editors, selection or the current entity.
    // Demand summaries refresh independently of demand-row editors.
    LiveUiText::bind(this->tabs, [this] {
        refreshSimulation();
        refreshWaterQualitySimulation();
        refreshSimulationErrors();
        refreshSimulationWarnings();
        refreshDemandSummary();
    });
    // Presentation-only refresh on every LanguageChange. Do not reconstruct the
    // inspector, reload a hydraulic model, or touch in-progress numeric edits.
    LiveUiText::bind(this, [this] {
        refreshTerrainElevationPresentation();
        refreshSourceTraceInputs();
    });
    connect(this->tabs, &QTabWidget::currentChanged, this, &EntityInspectorWidget::signalCurrentTabChanged);
    
    this->layout_main->addWidget(this->label_title);
    this->layout_main->addWidget(this->tabs);
}

HydraulicData *EntityInspectorWidget::hydraulicData() const
{
    return this->hydraulic_data;
}

void EntityInspectorWidget::setCurrentTabIndex(int index)
{
    if (index < 0 || index >= this->tabs->count())
        return;

    this->tabs->setCurrentIndex(index);
}

QVBoxLayout *EntityInspectorWidget::layoutOverview()
{
    return this->layout_overview;
}
QVBoxLayout *EntityInspectorWidget::layoutConfiguration()
{
    return this->layout_configuration;
}
QVBoxLayout *EntityInspectorWidget::layoutSimMeas()
{
    return this->layout_sim_meas;
}
QVBoxLayout *EntityInspectorWidget::layoutQuality()
{
    return this->layout_quality;
}
QVBoxLayout *EntityInspectorWidget::layoutQualitySimMeas()
{
    return this->layout_quality_sim_meas;
}

void EntityInspectorWidget::refreshEntityTitle()
{
    if (!this->entity_title_prefix.isEmpty())
        setTitle(LiveUiText::translate(this->entity_title_prefix.toUtf8().constData())
                 + QLatin1Char(' ') + this->displayed_entity_id);
}

void EntityInspectorWidget::setTitle(const QString &title)
{
    this->label_title->setText("<b>" + title.toHtmlEscaped() + "</b>");
}

void EntityInspectorWidget::constrainComboWidth(QComboBox *combo, int minimum_contents_length)
{
    if (combo == nullptr)
        return;

    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(minimum_contents_length);
}

void EntityInspectorWidget::constrainSpinWidth(QAbstractSpinBox *spin, int maximum_width)
{
    if (spin == nullptr)
        return;

    spin->setMaximumWidth(maximum_width);
}

void EntityInspectorWidget::addSimulationRow(QGridLayout *grid, int &row,
                                              SimulationField field,
                                              const char *name,
                                              const char *tooltip)
{
    QLabel *label_name = LiveUiText::caption<QLabel>(name, nullptr);
    label_name->setWordWrap(true);
    if (tooltip != nullptr)
        LiveUiText::bind(label_name, [label_name, tooltip] { label_name->setToolTip(LiveUiText::translate(tooltip)); });

    QLabel *label_value = new QLabel(QStringLiteral("—"));
    label_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label_value->setMinimumWidth(105);
    if (tooltip != nullptr)
        LiveUiText::bind(label_value, [label_value, tooltip] { label_value->setToolTip(LiveUiText::translate(tooltip)); });

    grid->addWidget(label_name, row, 0);
    grid->addWidget(label_value, row, 1);

    SimulationRowWidgets widgets;
    widgets.name = label_name;
    widgets.value = label_value;
    this->simulation_rows.insert(static_cast<int>(field), widgets);
    row++;
}

void EntityInspectorWidget::addGroupSimulation()
{
    if (this->label_simulation_message != nullptr)
        return;

    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Simulation"));
    QGridLayout *grid = new QGridLayout(group);
    int row = 0;

    this->label_simulation_message = new QLabel();
    this->label_simulation_message->setWordWrap(true);
    grid->addWidget(this->label_simulation_message, row++, 0, 1, 2);

    addSimulationRow(grid, row, SimulationField::ResultTime, QT_TRANSLATE_NOOP("LiveUiText", "Time"));

    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
        addSimulationRow(grid, row, SimulationField::DemandRequested, QT_TRANSLATE_NOOP("LiveUiText", "Requested Demand"), QT_TRANSLATE_NOOP("LiveUiText", "Demand requested before pressure-driven demand reduction."));
        addSimulationRow(grid, row, SimulationField::DemandDelivered, QT_TRANSLATE_NOOP("LiveUiText", "Delivered Demand"), QT_TRANSLATE_NOOP("LiveUiText", "Consumer demand actually delivered at the selected timestep."));
        addSimulationRow(grid, row, SimulationField::DemandDeficit, QT_TRANSLATE_NOOP("LiveUiText", "Demand Deficit"));
        addSimulationRow(grid, row, SimulationField::TotalDemand, QT_TRANSLATE_NOOP("LiveUiText", "Total Demand"), QT_TRANSLATE_NOOP("LiveUiText", "Total junction outflow: delivered demand, emitter flow, and assigned pipe leakage."));
        addSimulationRow(grid, row, SimulationField::EmitterFlow, QT_TRANSLATE_NOOP("LiveUiText", "Emitter Flow"));
        addSimulationRow(grid, row, SimulationField::LeakageFlow, QT_TRANSLATE_NOOP("LiveUiText", "Leakage Flow"));
        addSimulationRow(grid, row, SimulationField::Head, QT_TRANSLATE_NOOP("LiveUiText", "Head"));
        addSimulationRow(grid, row, SimulationField::PressureHead, QT_TRANSLATE_NOOP("LiveUiText", "Pressure"));
        addSimulationRow(grid, row, SimulationField::ReferencedByControl, QT_TRANSLATE_NOOP("LiveUiText", "Referenced by Control"));
        break;
    case InfrastructureEntity::Reservoir:
        addSimulationRow(grid, row, SimulationField::NetDemand, QT_TRANSLATE_NOOP("LiveUiText", "Net Demand"), QT_TRANSLATE_NOOP("LiveUiText", "Negative means the reservoir supplies the network; positive means it receives water."));
        addSimulationRow(grid, row, SimulationField::Head, QT_TRANSLATE_NOOP("LiveUiText", "Head"));
        addSimulationRow(grid, row, SimulationField::PressureHead, QT_TRANSLATE_NOOP("LiveUiText", "Pressure"));
        addSimulationRow(grid, row, SimulationField::ReferencedByControl, QT_TRANSLATE_NOOP("LiveUiText", "Referenced by Control"));
        break;
    case InfrastructureEntity::Tank:
        addSimulationRow(grid, row, SimulationField::NetDemand, QT_TRANSLATE_NOOP("LiveUiText", "Net Demand"), QT_TRANSLATE_NOOP("LiveUiText", "Positive means the tank is filling; negative means it supplies the network."));
        addSimulationRow(grid, row, SimulationField::Head, QT_TRANSLATE_NOOP("LiveUiText", "Head"));
        addSimulationRow(grid, row, SimulationField::PressureHead, QT_TRANSLATE_NOOP("LiveUiText", "Pressure"));
        addSimulationRow(grid, row, SimulationField::WaterLevel, QT_TRANSLATE_NOOP("LiveUiText", "Water Level"));
        addSimulationRow(grid, row, SimulationField::Volume, QT_TRANSLATE_NOOP("LiveUiText", "Volume"));
        addSimulationRow(grid, row, SimulationField::MixingZoneVolume, QT_TRANSLATE_NOOP("LiveUiText", "Mixing-Zone Volume"));
        addSimulationRow(grid, row, SimulationField::ReferencedByControl, QT_TRANSLATE_NOOP("LiveUiText", "Referenced by Control"));
        break;
    case InfrastructureEntity::Pipe:
        addSimulationRow(grid, row, SimulationField::Flow, QT_TRANSLATE_NOOP("LiveUiText", "Flow"));
        addSimulationRow(grid, row, SimulationField::LeakageFlow, QT_TRANSLATE_NOOP("LiveUiText", "Leakage Flow"));
        addSimulationRow(grid, row, SimulationField::Velocity, QT_TRANSLATE_NOOP("LiveUiText", "Velocity"));
        addSimulationRow(grid, row, SimulationField::HeadLoss, QT_TRANSLATE_NOOP("LiveUiText", "Head Loss"));
        addSimulationRow(grid, row, SimulationField::UnitHeadLoss, QT_TRANSLATE_NOOP("LiveUiText", "Unit Head Loss"));
        addSimulationRow(grid, row, SimulationField::FrictionFactor, QT_TRANSLATE_NOOP("LiveUiText", "Friction Factor"));
        addSimulationRow(grid, row, SimulationField::Status, QT_TRANSLATE_NOOP("LiveUiText", "Status"));
        addSimulationRow(grid, row, SimulationField::Roughness, QT_TRANSLATE_NOOP("LiveUiText", "Roughness"), QT_TRANSLATE_NOOP("LiveUiText", "Effective roughness returned by the hydraulic engine for the configured headloss formula."));
        addSimulationRow(grid, row, SimulationField::ReferencedByControl, QT_TRANSLATE_NOOP("LiveUiText", "Referenced by Control"));
        break;
    case InfrastructureEntity::Pump:
        addSimulationRow(grid, row, SimulationField::Flow, QT_TRANSLATE_NOOP("LiveUiText", "Flow"));
        addSimulationRow(grid, row, SimulationField::Velocity, QT_TRANSLATE_NOOP("LiveUiText", "Velocity"));
        addSimulationRow(grid, row, SimulationField::Head, QT_TRANSLATE_NOOP("LiveUiText", "Head Gain"));
        addSimulationRow(grid, row, SimulationField::Status, QT_TRANSLATE_NOOP("LiveUiText", "Status"));
        addSimulationRow(grid, row, SimulationField::PumpState, QT_TRANSLATE_NOOP("LiveUiText", "Operating State"));
        addSimulationRow(grid, row, SimulationField::Speed, QT_TRANSLATE_NOOP("LiveUiText", "Speed"));
        addSimulationRow(grid, row, SimulationField::Efficiency, QT_TRANSLATE_NOOP("LiveUiText", "Efficiency"));
        addSimulationRow(grid, row, SimulationField::Power, QT_TRANSLATE_NOOP("LiveUiText", "Power"));
        addSimulationRow(grid, row, SimulationField::ReferencedByControl, QT_TRANSLATE_NOOP("LiveUiText", "Referenced by Control"));
        break;
    case InfrastructureEntity::Valve:
        addSimulationRow(grid, row, SimulationField::Flow, QT_TRANSLATE_NOOP("LiveUiText", "Flow"));
        addSimulationRow(grid, row, SimulationField::Velocity, QT_TRANSLATE_NOOP("LiveUiText", "Velocity"));
        addSimulationRow(grid, row, SimulationField::HeadLoss, QT_TRANSLATE_NOOP("LiveUiText", "Head Loss"));
        addSimulationRow(grid, row, SimulationField::Status, QT_TRANSLATE_NOOP("LiveUiText", "Status"));
        addSimulationRow(grid, row, SimulationField::ValveRegulating, QT_TRANSLATE_NOOP("LiveUiText", "Regulating"));
        addSimulationRow(grid, row, SimulationField::Setting, QT_TRANSLATE_NOOP("LiveUiText", "Setting"));
        addSimulationRow(grid, row, SimulationField::ReferencedByControl, QT_TRANSLATE_NOOP("LiveUiText", "Referenced by Control"));
        break;
    default:
        break;
    }

    grid->setColumnStretch(1, 1);
    layoutSimMeas()->addWidget(group);

    if (this->entity_type == InfrastructureEntity::Pump)
    {
        GroupBoxCollapsible *energy_group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Simulation Energy Summary"));
        QGridLayout *energy_grid = new QGridLayout(energy_group);
        int energy_row = 0;

        this->label_simulation_energy_message = new QLabel();
        this->label_simulation_energy_message->setWordWrap(true);
        energy_grid->addWidget(this->label_simulation_energy_message, energy_row++, 0, 1, 2);

        addSimulationRow(energy_grid, energy_row, SimulationField::TimeOnline, QT_TRANSLATE_NOOP("LiveUiText", "Time Online"));
        addSimulationRow(energy_grid, energy_row, SimulationField::AverageEfficiency, QT_TRANSLATE_NOOP("LiveUiText", "Average Efficiency"));
        addSimulationRow(energy_grid, energy_row, SimulationField::AverageEnergyIntensity,
                         QT_TRANSLATE_NOOP("LiveUiText", "Average Energy Intensity"));
        addSimulationRow(energy_grid, energy_row, SimulationField::AveragePower, QT_TRANSLATE_NOOP("LiveUiText", "Average Power"));
        addSimulationRow(energy_grid, energy_row, SimulationField::PeakPower, QT_TRANSLATE_NOOP("LiveUiText", "Peak Power"));
        addSimulationRow(energy_grid, energy_row, SimulationField::AverageCostPerDay,
                         QT_TRANSLATE_NOOP("LiveUiText", "Average Energy Cost"));
        energy_grid->setColumnStretch(1, 1);
        layoutSimMeas()->addWidget(energy_group);
    }

    connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged,
            this, [this](bool)
    {
        refreshSimulation();
    });
    connect(this->hydraulic_data, &HydraulicData::signalCurrentSimulationResultChanged,
            this, [this](int)
    {
        refreshSimulation();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, &EntityInspectorWidget::refreshSimulation);

    new UnitAwareView(this->scroll_sim_meas, UnitProfileManager::instance().presentation(),
                      [this] { refreshSimulation(); });
    refreshSimulation();
}

void EntityInspectorWidget::resetSimulationValues()
{
    for (const SimulationRowWidgets &widgets : this->simulation_rows)
    {
        if (widgets.value != nullptr)
            widgets.value->setText(QStringLiteral("—"));
    }
}

void EntityInspectorWidget::setSimulationText(SimulationField field, const QString &text)
{
    const int key = static_cast<int>(field);
    if (!this->simulation_rows.contains(key))
        return;

    QLabel *label = this->simulation_rows.value(key).value;
    if (label != nullptr)
        label->setText(text);
}

void EntityInspectorWidget::setSimulationValue(SimulationField field, double value,
                                                int decimals, const QString &unit)
{
    // Field identities determine physical dimensions; valve settings use
    // an explicit quantity below because their dimensions depend on type.
    std::string_view quantity;
    switch (field)
    {
    case SimulationField::DemandRequested:
    case SimulationField::DemandDelivered:
    case SimulationField::DemandDeficit:
    case SimulationField::TotalDemand:
    case SimulationField::NetDemand:
    case SimulationField::EmitterFlow:
    case SimulationField::LeakageFlow:
    case SimulationField::Flow:
        quantity = AOWIS_QUANTITY("volumetric_flow_rate");
        break;
    case SimulationField::Head:
        quantity = this->entity_type == InfrastructureEntity::Pump
            ? AOWIS_QUANTITY("head_gain") : AOWIS_QUANTITY("hydraulic_head");
        break;
    case SimulationField::PressureHead:
        quantity = AOWIS_QUANTITY("pressure_head");
        break;
    case SimulationField::WaterLevel:
        quantity = AOWIS_QUANTITY("water_level");
        break;
    case SimulationField::Volume:
    case SimulationField::MixingZoneVolume:
        quantity = AOWIS_QUANTITY("volume");
        break;
    case SimulationField::Velocity:
        quantity = AOWIS_QUANTITY("velocity");
        break;
    case SimulationField::HeadLoss:
        quantity = AOWIS_QUANTITY("head_loss");
        break;
    case SimulationField::UnitHeadLoss:
        quantity = AOWIS_QUANTITY("head_loss_gradient");
        break;
    case SimulationField::Efficiency:
    case SimulationField::AverageEfficiency:
        quantity = AOWIS_QUANTITY("efficiency");
        break;
    case SimulationField::TimeOnline:
        quantity = AOWIS_QUANTITY("operating_time_percentage");
        break;
    case SimulationField::Power:
    case SimulationField::AveragePower:
    case SimulationField::PeakPower:
        quantity = AOWIS_QUANTITY("power");
        break;
    case SimulationField::AverageEnergyIntensity:
        quantity = AOWIS_QUANTITY("energy_intensity");
        break;
    default:
        break;
    }

    if (!quantity.empty())
    {
        setSimulationText(field, SimulationInspectorUnitPresentation::format(
            UnitProfileManager::instance().presentation(), quantity, value, decimals));
        return;
    }
    // Dimensionless values, pump speed (×), and currency/day do not inherit
    // arbitrary measurement units.
    setSimulationText(field, formatSimulationNumber(value, decimals, unit));
}

void EntityInspectorWidget::setSimulationEntityAvailable(bool available)
{
    if (this->label_simulation_message == nullptr)
        return;

    this->label_simulation_message->setVisible(!available);
    if (!available)
        this->label_simulation_message->setText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No result for this entity at the selected timestep.")));
}

void EntityInspectorWidget::refreshPumpEnergySummary(
    const HydraulicSimulationResultTimeline &timeline)
{
    if (this->label_simulation_energy_message == nullptr || timeline.results.isEmpty())
        return;

    const HydraulicSimulationResult &final_result = timeline.results.constLast();
    const HydraulicSimulationResultLinkPumpEnergyUsage *energy_usage = nullptr;
    for (const HydraulicSimulationResultLinkPumpEnergyUsage &candidate :
         final_result.links_pump_energy_usage)
    {
        if (candidate.pump_uuid == this->entity_uuid)
        {
            energy_usage = &candidate;
            break;
        }
    }

    const bool available = energy_usage != nullptr;
    this->label_simulation_energy_message->setVisible(!available);
    if (!available)
    {
        this->label_simulation_energy_message->setText(
            inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No complete-run energy summary is available for this pump.")));
        return;
    }

    setSimulationValue(SimulationField::TimeOnline, energy_usage->time_online_percent,
                       2, QStringLiteral(" %"));
    setSimulationValue(SimulationField::AverageEfficiency,
                       energy_usage->average_efficiency_percent, 2, QStringLiteral(" %"));
    setSimulationValue(SimulationField::AverageEnergyIntensity,
                       energy_usage->average_energy_intensity_kw_h_per_m3, 6,
                       QStringLiteral(" kWh/m³"));
    setSimulationValue(SimulationField::AveragePower, energy_usage->average_power_kw,
                       3, QStringLiteral(" kW"));
    setSimulationValue(SimulationField::PeakPower, energy_usage->peak_power_kw,
                       3, QStringLiteral(" kW"));
    const QString cost_suffix = energy_usage->currency_iso4217.isEmpty()
        ? QStringLiteral(" /day")
        : QStringLiteral(" %1/day").arg(energy_usage->currency_iso4217);
    setSimulationValue(SimulationField::AverageCostPerDay,
                       energy_usage->average_cost_per_day, 4, cost_suffix);
}

void EntityInspectorWidget::refreshSimulation()
{
    if (this->label_simulation_message == nullptr)
        return;

    resetSimulationValues();
    if (this->label_simulation_energy_message != nullptr)
    {
        this->label_simulation_energy_message->show();
        this->label_simulation_energy_message->setText(
            inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No valid simulation results available.")));
    }

    if (this->hydraulic_data == nullptr || !this->hydraulic_data->hasSimulationResults())
    {
        this->label_simulation_message->show();
        this->label_simulation_message->setText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No valid simulation results available.")));
        return;
    }

    const HydraulicSimulationResult *result = this->hydraulic_data->currentSimulationResult();
    const std::optional<HydraulicSimulationResultTimeline> &timeline =
        this->hydraulic_data->simulationResultTimeline();
    if (result == nullptr || !timeline.has_value())
    {
        this->label_simulation_message->show();
        this->label_simulation_message->setText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No simulation timestep is selected.")));
        return;
    }

    setSimulationText(SimulationField::ResultTime,
                      formatSimulationElapsedTime(result->time_elapsed_s));

    bool entity_available = false;
    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
    {
        const HydraulicSimulationResultNodeJunction *junction =
            simulationResultByUuid(result->nodes_junctions, this->entity_uuid);
        if (junction == nullptr)
            break;

        entity_available = true;
        setSimulationValue(SimulationField::DemandRequested,
                           junction->demand_requested_m3_per_h, 3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::DemandDelivered,
                           junction->demand_delivered_m3_per_h, 3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::DemandDeficit,
                           junction->demand_deficit_m3_per_h, 3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::TotalDemand,
                           junction->total_demand_m3_per_h, 3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::EmitterFlow,
                           junction->emitter_flow_m3_per_h, 3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::LeakageFlow,
                           junction->leakage_flow_m3_per_h, 3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::Head, junction->hydraulic_head_m, 3, QStringLiteral(" m"));
        setSimulationValue(SimulationField::PressureHead, junction->pressure_head_m,
                           3, QStringLiteral(" m"));
        setSimulationText(SimulationField::ReferencedByControl,
                          junction->appears_in_control ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Yes")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No")));
        break;
    }
    case InfrastructureEntity::Reservoir:
    {
        const HydraulicSimulationResultNodeReservoir *reservoir =
            simulationResultByUuid(result->nodes_reservoirs, this->entity_uuid);
        if (reservoir == nullptr)
            break;

        entity_available = true;
        setSimulationValue(SimulationField::NetDemand, reservoir->net_demand_m3_per_h,
                           3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::Head, reservoir->hydraulic_head_m, 3, QStringLiteral(" m"));
        setSimulationValue(SimulationField::PressureHead, reservoir->pressure_head_m,
                           3, QStringLiteral(" m"));
        setSimulationText(SimulationField::ReferencedByControl,
                          reservoir->appears_in_control ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Yes")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No")));
        break;
    }
    case InfrastructureEntity::Tank:
    {
        const HydraulicSimulationResultNodeTank *tank =
            simulationResultByUuid(result->nodes_tanks, this->entity_uuid);
        if (tank == nullptr)
            break;

        entity_available = true;
        setSimulationValue(SimulationField::NetDemand, tank->net_demand_m3_per_h,
                           3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::Head, tank->hydraulic_head_m, 3, QStringLiteral(" m"));
        setSimulationValue(SimulationField::PressureHead, tank->pressure_head_m,
                           3, QStringLiteral(" m"));
        setSimulationValue(SimulationField::WaterLevel, tank->water_level_m,
                           3, QStringLiteral(" m"));
        setSimulationValue(SimulationField::Volume, tank->volume_m3,
                           3, QStringLiteral(" m³"));
        setSimulationValue(SimulationField::MixingZoneVolume, tank->mixing_zone_volume_m3,
                           3, QStringLiteral(" m³"));
        setSimulationText(SimulationField::ReferencedByControl,
                          tank->appears_in_control ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Yes")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No")));
        break;
    }
    case InfrastructureEntity::Pipe:
    {
        const HydraulicSimulationResultLinkPipe *pipe =
            simulationResultByUuid(result->links_pipes, this->entity_uuid);
        if (pipe == nullptr)
            break;

        entity_available = true;
        setSimulationValue(SimulationField::Flow, pipe->flow_m3_per_h,
                           3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::LeakageFlow, pipe->leakage_flow_m3_per_h,
                           3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::Velocity, pipe->velocity_m_per_s,
                           3, QStringLiteral(" m/s"));
        setSimulationValue(SimulationField::HeadLoss, pipe->head_loss_m,
                           3, QStringLiteral(" m"));
        setSimulationValue(SimulationField::UnitHeadLoss,
                           pipe->head_loss_gradient_m_per_km, 3, QStringLiteral(" m/km"));
        setSimulationValue(SimulationField::FrictionFactor, pipe->friction_factor, 6);
        setSimulationText(SimulationField::Status,
                          pipe->open ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Open")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Closed")));
        setSimulationText(SimulationField::Roughness, pipeRoughnessText(*pipe, UnitProfileManager::instance().presentation()));
        setSimulationText(SimulationField::ReferencedByControl,
                          pipe->appears_in_control ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Yes")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No")));
        break;
    }
    case InfrastructureEntity::Pump:
    {
        const HydraulicSimulationResultLinkPump *pump =
            simulationResultByUuid(result->links_pumps, this->entity_uuid);
        if (pump == nullptr)
            break;

        entity_available = true;
        setSimulationValue(SimulationField::Flow, pump->flow_m3_per_h,
                           3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::Velocity, pump->velocity_m_per_s,
                           3, QStringLiteral(" m/s"));
        setSimulationValue(SimulationField::Head, pump->head_gain_m,
                           3, QStringLiteral(" m"));
        setSimulationText(SimulationField::Status,
                          pump->open ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Open")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Closed")));
        setSimulationText(SimulationField::PumpState, pumpStateText(pump->state));
        setSimulationValue(SimulationField::Speed, pump->speed_ratio, 3, QStringLiteral(" ×"));
        setSimulationValue(SimulationField::Efficiency, pump->efficiency_percent,
                           2, QStringLiteral(" %"));
        setSimulationValue(SimulationField::Power, pump->power_kw,
                           3, QStringLiteral(" kW"));
        setSimulationText(SimulationField::ReferencedByControl,
                          pump->appears_in_control ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Yes")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No")));
        break;
    }
    case InfrastructureEntity::Valve:
    {
        const HydraulicSimulationResultLinkValve *valve_result =
            simulationResultByUuid(result->links_valves, this->entity_uuid);
        if (valve_result == nullptr)
            break;

        entity_available = true;
        setSimulationValue(SimulationField::Flow, valve_result->flow_m3_per_h,
                           3, QStringLiteral(" m³/h"));
        setSimulationValue(SimulationField::Velocity, valve_result->velocity_m_per_s,
                           3, QStringLiteral(" m/s"));
        setSimulationValue(SimulationField::HeadLoss, valve_result->head_loss_m,
                           3, QStringLiteral(" m"));
        setSimulationText(SimulationField::Status,
                          valve_result->open ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Open")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Closed")));
        setSimulationText(SimulationField::ValveRegulating,
                          valve_result->active ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Yes")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No")));

        switch (valve_result->type)
        {
        case HydraulicLinkValveType::PRV:
        case HydraulicLinkValveType::PSV:
        case HydraulicLinkValveType::PBV:
            setSimulationText(SimulationField::Setting,
                SimulationInspectorUnitPresentation::format(
                    UnitProfileManager::instance().presentation(),
                    AOWIS_QUANTITY("pressure_head"), valve_result->setting_pressure_head_m, 3));
            break;
        case HydraulicLinkValveType::FCV:
            setSimulationText(SimulationField::Setting,
                SimulationInspectorUnitPresentation::format(
                    UnitProfileManager::instance().presentation(),
                    AOWIS_QUANTITY("volumetric_flow_rate"), valve_result->setting_flow_m3_per_h, 3));
            break;
        case HydraulicLinkValveType::TCV:
            setSimulationValue(SimulationField::Setting, valve_result->setting_loss_coefficient, 6);
            break;
        case HydraulicLinkValveType::PCV:
            setSimulationText(SimulationField::Setting,
                SimulationInspectorUnitPresentation::format(
                    UnitProfileManager::instance().presentation(),
                    AOWIS_QUANTITY("valve_position"), valve_result->setting_position_percent, 2));
            break;
        case HydraulicLinkValveType::GPV:
            setSimulationText(SimulationField::Setting, inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Curve-defined")));
            break;
        }
        setSimulationText(SimulationField::ReferencedByControl,
                          valve_result->appears_in_control ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Yes")) : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No")));
        break;
    }
    default:
        break;
    }

    setSimulationEntityAvailable(entity_available);
    if (this->entity_type == InfrastructureEntity::Pump)
        refreshPumpEnergySummary(timeline.value());
}

void EntityInspectorWidget::addGroupWaterQualitySimulation()
{
    if (this->label_quality_simulation_message != nullptr)
        return;

    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Simulation"));
    QGridLayout *grid = new QGridLayout(group);
    int row = 0;

    this->label_quality_simulation_message = new QLabel();
    this->label_quality_simulation_message->setWordWrap(true);
    grid->addWidget(this->label_quality_simulation_message, row++, 0, 1, 2);

    QLabel *label_time_name = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Time"), nullptr);
    this->label_quality_simulation_time = new QLabel(QStringLiteral("—"));
    this->label_quality_simulation_time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(label_time_name, row, 0);
    grid->addWidget(this->label_quality_simulation_time, row++, 1);

    this->label_quality_simulation_value_name = new QLabel();
    EntityInspectorText::setCaption(this->label_quality_simulation_value_name, QT_TRANSLATE_NOOP("LiveUiText", "Water Quality"));
    this->label_quality_simulation_value = new QLabel(QStringLiteral("—"));
    this->label_quality_simulation_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(this->label_quality_simulation_value_name, row, 0);
    grid->addWidget(this->label_quality_simulation_value, row++, 1);

    this->label_quality_simulation_source_mass_name = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Source Mass Flow"), nullptr);
    this->label_quality_simulation_source_mass = new QLabel(QStringLiteral("—"));
    this->label_quality_simulation_source_mass->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(this->label_quality_simulation_source_mass_name, row, 0);
    grid->addWidget(this->label_quality_simulation_source_mass, row++, 1);

    grid->setColumnStretch(1, 1);
    layoutQualitySimMeas()->addWidget(group);

    connect(this->hydraulic_data, &HydraulicData::signalWaterQualitySimulationResultTimelineChanged,
            this, [this](bool)
    {
        refreshWaterQualitySimulation();
    });
    connect(this->hydraulic_data, &HydraulicData::signalCurrentSimulationResultChanged,
            this, [this](int)
    {
        refreshWaterQualitySimulation();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, &EntityInspectorWidget::refreshWaterQualitySimulation);

    new UnitAwareView(this->scroll_quality_sim_meas, UnitProfileManager::instance().presentation(),
                      [this] { refreshWaterQualitySimulation(); });
    refreshWaterQualitySimulation();
}

void EntityInspectorWidget::refreshWaterQualitySimulation()
{
    if (this->label_quality_simulation_message == nullptr)
        return;

    this->label_quality_simulation_time->setText(QStringLiteral("—"));
    this->label_quality_simulation_value->setText(QStringLiteral("—"));
    this->label_quality_simulation_source_mass->setText(QStringLiteral("—"));
    this->label_quality_simulation_source_mass_name->setVisible(false);
    this->label_quality_simulation_source_mass->setVisible(false);

    const std::optional<WaterQualitySimulationResultTimeline> &timeline =
        this->hydraulic_data->waterQualitySimulationResultTimeline();
    if (!timeline.has_value() || timeline->analysis == WaterQualityAnalysisType::None)
    {
        this->label_quality_simulation_message->setVisible(true);
        this->label_quality_simulation_message->setText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Water-quality simulation was not run.")));
        EntityInspectorText::setCaption(this->label_quality_simulation_value_name, QT_TRANSLATE_NOOP("LiveUiText", "Water Quality"));
        return;
    }

    const WaterQualitySimulationResult *result =
        this->hydraulic_data->currentWaterQualitySimulationResult();
    if (result == nullptr)
    {
        this->label_quality_simulation_message->setVisible(true);
        this->label_quality_simulation_message->setText(
            inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No water-quality result is available for the selected hydraulic timestep.")));
        return;
    }

    double value = 0.0;
    double source_mass_flow_mg_per_min = 0.0;
    bool entity_available = false;
    bool node_entity = false;

    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
    {
        const WaterQualitySimulationResultNodeJunction *entity_result =
            simulationResultByUuid(result->nodes_junctions, this->entity_uuid);
        if (entity_result != nullptr)
        {
            entity_available = true;
            node_entity = true;
            source_mass_flow_mg_per_min = entity_result->source_mass_flow_mg_per_min;
            if (timeline->analysis == WaterQualityAnalysisType::Chemical)
                value = entity_result->chemical_concentration_mg_per_l;
            else if (timeline->analysis == WaterQualityAnalysisType::WaterAge)
                value = entity_result->water_age_h;
            else
                value = entity_result->source_trace_percent;
        }
        break;
    }
    case InfrastructureEntity::Reservoir:
    {
        const WaterQualitySimulationResultNodeReservoir *entity_result =
            simulationResultByUuid(result->nodes_reservoirs, this->entity_uuid);
        if (entity_result != nullptr)
        {
            entity_available = true;
            node_entity = true;
            source_mass_flow_mg_per_min = entity_result->source_mass_flow_mg_per_min;
            if (timeline->analysis == WaterQualityAnalysisType::Chemical)
                value = entity_result->chemical_concentration_mg_per_l;
            else if (timeline->analysis == WaterQualityAnalysisType::WaterAge)
                value = entity_result->water_age_h;
            else
                value = entity_result->source_trace_percent;
        }
        break;
    }
    case InfrastructureEntity::Tank:
    {
        const WaterQualitySimulationResultNodeTank *entity_result =
            simulationResultByUuid(result->nodes_tanks, this->entity_uuid);
        if (entity_result != nullptr)
        {
            entity_available = true;
            node_entity = true;
            source_mass_flow_mg_per_min = entity_result->source_mass_flow_mg_per_min;
            if (timeline->analysis == WaterQualityAnalysisType::Chemical)
                value = entity_result->chemical_concentration_mg_per_l;
            else if (timeline->analysis == WaterQualityAnalysisType::WaterAge)
                value = entity_result->water_age_h;
            else
                value = entity_result->source_trace_percent;
        }
        break;
    }
    case InfrastructureEntity::Pipe:
    {
        const WaterQualitySimulationResultLinkPipe *entity_result =
            simulationResultByUuid(result->links_pipes, this->entity_uuid);
        if (entity_result != nullptr)
        {
            entity_available = true;
            if (timeline->analysis == WaterQualityAnalysisType::Chemical)
                value = entity_result->chemical_concentration_mg_per_l;
            else if (timeline->analysis == WaterQualityAnalysisType::WaterAge)
                value = entity_result->water_age_h;
            else
                value = entity_result->source_trace_percent;
        }
        break;
    }
    case InfrastructureEntity::Pump:
    {
        const WaterQualitySimulationResultLinkPump *entity_result =
            simulationResultByUuid(result->links_pumps, this->entity_uuid);
        if (entity_result != nullptr)
        {
            entity_available = true;
            if (timeline->analysis == WaterQualityAnalysisType::Chemical)
                value = entity_result->chemical_concentration_mg_per_l;
            else if (timeline->analysis == WaterQualityAnalysisType::WaterAge)
                value = entity_result->water_age_h;
            else
                value = entity_result->source_trace_percent;
        }
        break;
    }
    case InfrastructureEntity::Valve:
    {
        const WaterQualitySimulationResultLinkValve *entity_result =
            simulationResultByUuid(result->links_valves, this->entity_uuid);
        if (entity_result != nullptr)
        {
            entity_available = true;
            if (timeline->analysis == WaterQualityAnalysisType::Chemical)
                value = entity_result->chemical_concentration_mg_per_l;
            else if (timeline->analysis == WaterQualityAnalysisType::WaterAge)
                value = entity_result->water_age_h;
            else
                value = entity_result->source_trace_percent;
        }
        break;
    }
    default:
        break;
    }

    this->label_quality_simulation_message->setVisible(!entity_available);
    if (!entity_available)
    {
        this->label_quality_simulation_message->setText(
            inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "No water-quality result for this entity at the selected timestep.")));
        return;
    }

    this->label_quality_simulation_time->setText(
        formatSimulationElapsedTime(result->time_elapsed_s));
    if (timeline->analysis == WaterQualityAnalysisType::Chemical)
    {
        EntityInspectorText::setCaption(this->label_quality_simulation_value_name, QT_TRANSLATE_NOOP("LiveUiText", "Chemical Concentration"));
        this->label_quality_simulation_value->setText(
            SimulationInspectorUnitPresentation::format(
                UnitProfileManager::instance().presentation(),
                AOWIS_QUANTITY("chemical_mass_concentration"), value, 6));
        if (node_entity)
        {
            this->label_quality_simulation_source_mass_name->setVisible(true);
            this->label_quality_simulation_source_mass->setVisible(true);
            this->label_quality_simulation_source_mass->setText(
                SimulationInspectorUnitPresentation::format(
                    UnitProfileManager::instance().presentation(),
                    AOWIS_QUANTITY("chemical_mass_flow_rate"), source_mass_flow_mg_per_min, 6));
        }
    }
    else if (timeline->analysis == WaterQualityAnalysisType::WaterAge)
    {
        EntityInspectorText::setCaption(this->label_quality_simulation_value_name, QT_TRANSLATE_NOOP("LiveUiText", "Water Age"));
        this->label_quality_simulation_value->setText(
            SimulationInspectorUnitPresentation::format(
                UnitProfileManager::instance().presentation(), AOWIS_QUANTITY("water_age"), value, 6));
    }
    else
    {
        EntityInspectorText::setCaption(this->label_quality_simulation_value_name, QT_TRANSLATE_NOOP("LiveUiText", "Source Trace"));
        this->label_quality_simulation_value->setText(
            SimulationInspectorUnitPresentation::format(
                UnitProfileManager::instance().presentation(),
                AOWIS_QUANTITY("source_trace_percentage"), value, 6));
    }
}

void EntityInspectorWidget::addGroupOverviewImage(const QString &icon_path, const QString &name)
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "General"));
    QGridLayout *grid = new QGridLayout(group);
    
    QLabel *picture = new QLabel();
    QPixmap pixmap(icon_path);
    
    picture->setPixmap(pixmap.scaledToHeight(
        Sizes::SidebarRightImageHeight,
        Qt::SmoothTransformation
        ));
    picture->setAlignment(Qt::AlignCenter);
    
    grid->addWidget(picture, 0, 0, 1, 2);

    this->button_overview_find_on_map = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Find on Map"), nullptr);
    this->button_overview_find_on_map->setEnabled(false);
    this->button_overview_find_on_map->hide();
    grid->addWidget(this->button_overview_find_on_map, 1, 0, 1, 2);
    
    this->layoutOverview()->addWidget(group);
}

void EntityInspectorWidget::addGroupSimulationErrors()
{
    if (this->group_simulation_errors != nullptr)
        return;

    this->group_simulation_errors = new GroupBoxCollapsible(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Simulation Error")));
    QVBoxLayout *layout = new QVBoxLayout(this->group_simulation_errors);

    this->label_simulation_errors = new QLabel();
    this->label_simulation_errors->setTextFormat(Qt::RichText);
    this->label_simulation_errors->setWordWrap(true);
    this->label_simulation_errors->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->label_simulation_errors->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(this->label_simulation_errors);

    this->group_simulation_errors->hide();
    this->layoutOverview()->addWidget(this->group_simulation_errors);

    if (this->hydraulic_data != nullptr)
    {
        connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged,
                this, [this](bool)
        {
            refreshSimulationErrors();
        });
        connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
                this, &EntityInspectorWidget::refreshSimulationErrors);
    }

    refreshSimulationErrors();
}

void EntityInspectorWidget::refreshSimulationErrors()
{
    if (this->group_simulation_errors == nullptr || this->label_simulation_errors == nullptr)
        return;

    QList<HydraulicSimulationDiagnostic> entity_errors;
    if (this->hydraulic_data != nullptr && !this->entity_uuid.isNull())
    {
        const std::optional<HydraulicSimulationResultTimeline> &timeline =
            this->hydraulic_data->simulationResultTimeline();
        if (timeline.has_value())
        {
            for (const HydraulicSimulationDiagnostic &diagnostic : timeline->diagnostics)
            {
                if (diagnostic.entity.uuid == this->entity_uuid
                    && isSimulationErrorSeverity(diagnostic.severity))
                {
                    entity_errors.append(diagnostic);
                }
            }

            if (entity_errors.isEmpty() && !timeline->status.success
                && timeline->status.entity.uuid == this->entity_uuid)
            {
                entity_errors.append(simulationErrorFromStatus(timeline->status));
            }
        }
    }

    if (entity_errors.isEmpty())
    {
        this->label_simulation_errors->clear();
        this->group_simulation_errors->hide();
        return;
    }

    const bool stale = this->hydraulic_data->simulationDiagnosticEntityStale(this->entity_uuid);
    QString html;
    if (stale)
    {
        html += QStringLiteral("<p style=\"color:#808080; font-weight:600;\">%1</p>").arg(
            inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
                "Outdated diagnostic — this entity has been edited since this simulation.")).toHtmlEscaped());
    }

    for (const HydraulicSimulationDiagnostic &diagnostic : entity_errors)
        html += simulationDiagnosticHtml(diagnostic, stale);

    this->group_simulation_errors->setTitle(
        entity_errors.size() == 1
            ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Simulation Error"))
            : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Simulation Errors (%1)"))
                  .arg(static_cast<int>(entity_errors.size())));
    this->label_simulation_errors->setText(html);
    this->group_simulation_errors->show();
}

void EntityInspectorWidget::addGroupSimulationWarnings()
{
    if (this->group_simulation_warnings != nullptr)
        return;

    this->group_simulation_warnings = new GroupBoxCollapsible(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Simulation Warning")));
    QVBoxLayout *layout = new QVBoxLayout(this->group_simulation_warnings);

    this->label_simulation_warnings = new QLabel();
    this->label_simulation_warnings->setTextFormat(Qt::RichText);
    this->label_simulation_warnings->setWordWrap(true);
    this->label_simulation_warnings->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->label_simulation_warnings->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(this->label_simulation_warnings);

    this->group_simulation_warnings->hide();
    this->layoutOverview()->addWidget(this->group_simulation_warnings);

    if (this->hydraulic_data != nullptr)
    {
        connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged,
                this, [this](bool)
        {
            refreshSimulationWarnings();
        });
        connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
                this, &EntityInspectorWidget::refreshSimulationWarnings);
    }

    refreshSimulationWarnings();
}

void EntityInspectorWidget::refreshSimulationWarnings()
{
    if (this->group_simulation_warnings == nullptr || this->label_simulation_warnings == nullptr)
        return;

    QList<HydraulicSimulationDiagnostic> entity_warnings;
    if (this->hydraulic_data != nullptr && !this->entity_uuid.isNull())
    {
        const std::optional<HydraulicSimulationResultTimeline> &timeline =
            this->hydraulic_data->simulationResultTimeline();
        if (timeline.has_value())
        {
            for (const HydraulicSimulationDiagnostic &diagnostic : timeline->diagnostics)
            {
                if (diagnostic.entity.uuid == this->entity_uuid
                    && isSimulationWarningSeverity(diagnostic.severity))
                {
                    entity_warnings.append(diagnostic);
                }
            }
        }
    }

    if (entity_warnings.isEmpty())
    {
        this->label_simulation_warnings->clear();
        this->group_simulation_warnings->hide();
        return;
    }

    const bool stale = this->hydraulic_data->simulationDiagnosticEntityStale(this->entity_uuid);
    QString html;
    if (stale)
    {
        html += QStringLiteral("<p style=\"color:#808080; font-weight:600;\">%1</p>").arg(
            inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
                "Outdated diagnostic — this entity has been edited since this simulation.")).toHtmlEscaped());
    }

    for (const HydraulicSimulationDiagnostic &diagnostic : entity_warnings)
        html += simulationDiagnosticHtml(diagnostic, stale);

    this->group_simulation_warnings->setTitle(
        entity_warnings.size() == 1
            ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Simulation Warning"))
            : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Simulation Warnings (%1)"))
                  .arg(static_cast<int>(entity_warnings.size())));
    this->label_simulation_warnings->setText(html);
    this->group_simulation_warnings->show();
}

void EntityInspectorWidget::addGroupGeneral(const QString &name)
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "General"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_name = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Name"), nullptr);
    this->line_name = new QLineEdit();
    this->line_name->setObjectName(QStringLiteral("entityInspectorName"));
    this->line_name->setText(name);

    QLabel *label_model_role = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Model Role"), nullptr);
    this->combo_model_role = new QComboBox();
    EntityInspectorText::addChoice(this->combo_model_role, QT_TRANSLATE_NOOP("LiveUiText", "[Unspecified]"), static_cast<int>(EntityModelRole::Unspecified));
    EntityInspectorText::addChoice(this->combo_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Existing Asset"), static_cast<int>(EntityModelRole::ExistingAsset));
    EntityInspectorText::addChoice(this->combo_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Planned Asset"), static_cast<int>(EntityModelRole::PlannedAsset));
    EntityInspectorText::addChoice(this->combo_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Virtual / Model-Only"), static_cast<int>(EntityModelRole::VirtualModelElement));
    EntityInspectorText::addChoice(this->combo_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Boundary Condition"), static_cast<int>(EntityModelRole::BoundaryCondition));
    EntityInspectorText::addChoice(this->combo_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Temporary / Testing"), static_cast<int>(EntityModelRole::TemporaryTesting));
    EntityInspectorText::addChoice(this->combo_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Retired Asset"), static_cast<int>(EntityModelRole::RetiredAsset));
    LiveUiText::bind(this->combo_model_role, [this] {
        this->combo_model_role->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
            "Describes whether this entity represents a real asset, a planned asset, a model-only helper, a boundary condition, or a temporary/testing element.")));
    });

    const QDate date_unset(100, 1, 1);

    QLabel *label_date_added = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Date Added"), nullptr);
    this->date_added = new QDateEdit();
    this->date_added->setCalendarPopup(true);
    this->date_added->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    LiveUiText::bind(this->date_added, [this] {
        this->date_added->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Date the entity was added to the model. yyyy-MM-dd")));
        this->date_added->setSpecialValueText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "[Not set]")));
    });
    this->date_added->setMinimumDate(date_unset);
    this->date_added->setDate(date_unset);

    QLabel *label_date_installed = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Installation<br>Date"), nullptr);
    this->date_installed = new QDateEdit();
    this->date_installed->setCalendarPopup(true);
    this->date_installed->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    LiveUiText::bind(this->date_installed, [this] {
        this->date_installed->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Date the physical asset was installed. yyyy-MM-dd")));
        this->date_installed->setSpecialValueText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "[Not set]")));
    });
    this->date_installed->setMinimumDate(date_unset);
    this->date_installed->setDate(date_unset);

    this->check_enabled = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Enabled"), nullptr);
    this->check_enabled->setChecked(true);

    grid->addWidget(label_name, 0, 0);
    grid->addWidget(this->line_name, 0, 1);
    grid->addWidget(label_model_role, 1, 0);
    grid->addWidget(this->combo_model_role, 1, 1);
    grid->addWidget(label_date_added, 2, 0);
    grid->addWidget(this->date_added, 2, 1);
    grid->addWidget(label_date_installed, 3, 0);
    grid->addWidget(this->date_installed, 3, 1);
    grid->addWidget(this->check_enabled, 4, 0, 1, 2);

    layoutConfiguration()->addWidget(group);
}

void EntityInspectorWidget::addGroupEndpoints()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Endpoints"));
    QGridLayout *grid = new QGridLayout(group);
    
    QLabel *label_node_1 = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Node 1"), nullptr);
    this->label_node_1_id = new QLabel();
    this->label_node_1_id->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->label_node_1_id->setAlignment(Qt::AlignCenter);
    this->button_node_1_locate = new QPushButton(QIcon(":/icon/geomarker.png"), "");
    this->button_node_1_locate->setIconSize(QSize(20, 20));
    LiveUiText::bind(this->button_node_1_locate, [this] {
        this->button_node_1_locate->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Find on Map")));
    });
    this->button_node_1_locate->setMaximumWidth(35);
    this->button_node_1_inspect = new QPushButton(QIcon(":/icon/target.png"), "");
    this->button_node_1_inspect->setIconSize(QSize(20, 20));
    LiveUiText::bind(this->button_node_1_inspect, [this] {
        this->button_node_1_inspect->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Show in Inspector")));
    });
    this->button_node_1_inspect->setMaximumWidth(35);
    
    QLabel *label_node_2 = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Node 2"), nullptr);
    this->label_node_2_id = new QLabel();
    this->label_node_2_id->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->label_node_2_id->setAlignment(Qt::AlignCenter);
    this->button_node_2_locate = new QPushButton(QIcon(":/icon/geomarker.png"), "");
    this->button_node_2_locate->setIconSize(QSize(20, 20));
    LiveUiText::bind(this->button_node_2_locate, [this] {
        this->button_node_2_locate->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Find on Map")));
    });
    this->button_node_2_locate->setMaximumWidth(35);
    this->button_node_2_inspect = new QPushButton(QIcon(":/icon/target.png"), "");
    this->button_node_2_inspect->setIconSize(QSize(20, 20));
    LiveUiText::bind(this->button_node_2_inspect, [this] {
        this->button_node_2_inspect->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Show in Inspector")));
    });
    this->button_node_2_inspect->setMaximumWidth(35);
    
    grid->addWidget(label_node_1, 0, 0);
    grid->addWidget(this->label_node_1_id, 0, 1);
    grid->addWidget(this->button_node_1_locate, 0, 2);
    grid->addWidget(this->button_node_1_inspect, 0, 3);
    
    grid->addWidget(label_node_2, 1, 0);
    grid->addWidget(this->label_node_2_id, 1, 1);
    grid->addWidget(this->button_node_2_locate, 1, 2);
    grid->addWidget(this->button_node_2_inspect, 1, 3);
    grid->setColumnStretch(1, 1);

    connect(this->button_node_1_locate, &QPushButton::clicked, this, [this]()
    {
        locateHydraulicEndpoint(this->node_uuid_1);
    });
    connect(this->button_node_1_inspect, &QPushButton::clicked, this, [this]()
    {
        inspectHydraulicEndpoint(this->node_uuid_1);
    });
    connect(this->button_node_2_locate, &QPushButton::clicked, this, [this]()
    {
        locateHydraulicEndpoint(this->node_uuid_2);
    });
    connect(this->button_node_2_inspect, &QPushButton::clicked, this, [this]()
    {
        inspectHydraulicEndpoint(this->node_uuid_2);
    });
    connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this,
            [this](InfrastructureEntity, const QUuid &uuid_changed)
    {
        if (uuid_changed == this->node_uuid_1 || uuid_changed == this->node_uuid_2)
            refreshHydraulicEndpoints();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this,
            [this]()
    {
        refreshHydraulicEndpoints();
    });

    refreshHydraulicEndpoints();
    
    this->layoutConfiguration()->addWidget(group);
}

void EntityInspectorWidget::addGroupPosition()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Position"));
    QGridLayout *grid = new QGridLayout(group);
    
    // Resolve the selected entity for every read/write, not just when the
    // inspector is created. No raw model pointer or rounded companion field.
    const CoordinatePropertyBinding::CoordinateReader read_coordinate = [this]()
        -> std::optional<CoordinateWGS84>
    {
        if (!this->hydraulic_data || this->entity_uuid.isNull())
            return std::nullopt;
        if (this->entity_type == InfrastructureEntity::DemandPoint)
        {
            const std::optional<HydraulicDemandPoint> point =
                this->hydraulic_data->demandPoint(this->entity_uuid);
            return point ? std::optional<CoordinateWGS84>(point->coordinate_wgs84)
                         : std::nullopt;
        }
        const std::optional<HydraulicNodeCommonData> node =
            this->hydraulic_data->nodeCommonData(this->entity_type, this->entity_uuid);
        return node ? std::optional<CoordinateWGS84>(node->coordinate_wgs84)
                    : std::nullopt;
    };
    const CoordinatePropertyBinding::CoordinateWriter write_coordinate = [this](
        const CoordinateWGS84 &coordinate) -> bool
    {
        if (!this->hydraulic_data || this->entity_uuid.isNull())
            return false;
        if (this->entity_type == InfrastructureEntity::DemandPoint)
            return this->hydraulic_data->setDemandPointCoordinate(this->entity_uuid, coordinate);
        return this->hydraulic_data->setNodeCoordinate(this->entity_uuid, coordinate);
    };

    QLabel *label_latitude = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Latitude"), nullptr);
    this->spin_latitude = new CoordinateNumericSpinBox(
        read_coordinate, write_coordinate, CoordinatePropertyBinding::Component::Latitude);

    QLabel *label_longitude = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Longitude"), nullptr);
    this->spin_longitude = new CoordinateNumericSpinBox(
        read_coordinate, write_coordinate, CoordinatePropertyBinding::Component::Longitude);

    this->label_geographic_altitude = new QLabel(group);
    this->label_geographic_altitude->setTextInteractionFlags(Qt::TextSelectableByMouse);
    LiveUiText::bind(this->label_geographic_altitude, [this] {
        this->label_geographic_altitude->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
            "Recorded geographic/GPS altitude, when available; not hydraulic elevation.")));
    });

    this->button_find_on_map = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Find on Map"), nullptr);
    
    grid->addWidget(label_latitude, 0, 0);
    grid->addWidget(this->spin_latitude, 0, 1);
    grid->addWidget(label_longitude, 1, 0);
    grid->addWidget(this->spin_longitude, 1, 1);
    grid->addWidget(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Geographic altitude"), group), 2, 0);
    grid->addWidget(this->label_geographic_altitude, 2, 1);
    grid->addWidget(this->button_find_on_map, 3, 0, 1, 2);

    new UnitAwareView(group, UnitProfileManager::instance().presentation(), [this] {
        this->refreshGeographicAltitude();
    });
    
    layoutConfiguration()->addWidget(group);
}

void EntityInspectorWidget::refreshGeographicAltitude()
{
    if (!this->label_geographic_altitude || !this->hydraulic_data || this->entity_uuid.isNull())
        return;

    std::optional<double> altitude_m;
    if (this->entity_type == InfrastructureEntity::DemandPoint)
    {
        const std::optional<HydraulicDemandPoint> point =
            this->hydraulic_data->demandPoint(this->entity_uuid);
        if (point) altitude_m = point->coordinate_wgs84.altitude_m;
    }
    else
    {
        const std::optional<HydraulicNodeCommonData> node =
            this->hydraulic_data->nodeCommonData(this->entity_type, this->entity_uuid);
        if (node) altitude_m = node->coordinate_wgs84.altitude_m;
    }
    this->label_geographic_altitude->setText(GeographicAltitudePresentation::format(
        UnitProfileManager::instance().presentation(), altitude_m));
}

void EntityInspectorWidget::bindHydraulicNode(InfrastructureEntity entity_type, const QUuid &uuid,
                                               const QString &title_prefix)
{
    this->entity_type = entity_type;
    this->entity_uuid = uuid;
    this->entity_title_prefix = title_prefix;

    addGroupSimulationErrors();
    addGroupSimulationWarnings();

    refreshHydraulicNode();

    connect(this->line_name, &QLineEdit::textEdited, this, [this](const QString &id)
    {
        this->hydraulic_data->setNodeId(this->entity_uuid, id);
    });
    connect(this->combo_model_role, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const EntityModelRole model_role = static_cast<EntityModelRole>(
            this->combo_model_role->currentData().toInt());
        this->hydraulic_data->setNodeModelRole(this->entity_uuid, model_role);
    });
    connect(this->date_added, &QDateEdit::dateChanged, this, [this](const QDate &)
    {
        this->hydraulic_data->setNodeDateAdded(this->entity_uuid, optionalDate(this->date_added));
    });
    connect(this->date_installed, &QDateEdit::dateChanged, this, [this](const QDate &)
    {
        this->hydraulic_data->setNodeDateInstalled(
            this->entity_uuid, optionalDate(this->date_installed));
    });
    connect(this->check_enabled, &QCheckBox::toggled, this, [this](bool enabled)
    {
        this->hydraulic_data->setNodeEnabled(this->entity_uuid, enabled);
    });
    connect(this->button_find_on_map, &QPushButton::clicked, this, [this]()
    {
        this->hydraulic_data->requestEntityLocate(this->entity_type, this->entity_uuid);
    });
    if (this->button_overview_find_on_map != nullptr)
    {
        this->button_overview_find_on_map->setEnabled(true);
        this->button_overview_find_on_map->show();
        connect(this->button_overview_find_on_map, &QPushButton::clicked, this, [this]()
        {
            this->hydraulic_data->requestEntityLocate(this->entity_type, this->entity_uuid);
        });
    }
    connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this, [this](InfrastructureEntity entity_type_changed, const QUuid &uuid_changed)
    {
        if (entity_type_changed == this->entity_type && uuid_changed == this->entity_uuid)
        {
            refreshHydraulicNode();
            scheduleDemandsRefresh();
        }
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]()
    {
        this->demands_structure_stale = true;
        refreshHydraulicNode();
        scheduleDemandsRefresh();
    });
}

void EntityInspectorWidget::bindHydraulicLink(InfrastructureEntity entity_type, const QUuid &uuid,
                                               const QString &title_prefix)
{
    this->entity_type = entity_type;
    this->entity_uuid = uuid;
    this->entity_title_prefix = title_prefix;

    addGroupSimulationErrors();
    addGroupSimulationWarnings();

    refreshHydraulicLink();

    connect(this->line_name, &QLineEdit::textEdited, this, [this](const QString &id)
    {
        this->hydraulic_data->setLinkId(this->entity_uuid, id);
    });
    connect(this->combo_model_role, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const EntityModelRole model_role = static_cast<EntityModelRole>(
            this->combo_model_role->currentData().toInt());
        this->hydraulic_data->setLinkModelRole(this->entity_uuid, model_role);
    });
    connect(this->date_added, &QDateEdit::dateChanged, this, [this](const QDate &)
    {
        this->hydraulic_data->setLinkDateAdded(this->entity_uuid, optionalDate(this->date_added));
    });
    connect(this->date_installed, &QDateEdit::dateChanged, this, [this](const QDate &)
    {
        this->hydraulic_data->setLinkDateInstalled(
            this->entity_uuid, optionalDate(this->date_installed));
    });
    connect(this->check_enabled, &QCheckBox::toggled, this, [this](bool enabled)
    {
        this->hydraulic_data->setLinkEnabled(this->entity_uuid, enabled);
    });
    if (this->button_overview_find_on_map != nullptr)
    {
        this->button_overview_find_on_map->setEnabled(true);
        this->button_overview_find_on_map->show();
        connect(this->button_overview_find_on_map, &QPushButton::clicked, this, [this]()
        {
            this->hydraulic_data->requestEntityLocate(this->entity_type, this->entity_uuid);
        });
    }
    connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, this,
            [this](InfrastructureEntity entity_type_changed, const QUuid &uuid_changed)
    {
        if (entity_type_changed == this->entity_type && uuid_changed == this->entity_uuid)
            refreshHydraulicLink();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]()
    {
        refreshHydraulicLink();
    });
}

void EntityInspectorWidget::bindDemandPoint(const QUuid &uuid, const QString &title_prefix)
{
    this->entity_type = InfrastructureEntity::DemandPoint;
    this->entity_uuid = uuid;
    this->entity_title_prefix = title_prefix;

    addGroupSimulationErrors();
    addGroupSimulationWarnings();

    refreshDemandPoint();

    connect(this->line_name, &QLineEdit::textEdited, this, [this](const QString &id)
    {
        this->hydraulic_data->setDemandPointId(this->entity_uuid, id);
    });
    connect(this->combo_model_role, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const EntityModelRole model_role = static_cast<EntityModelRole>(
            this->combo_model_role->currentData().toInt());
        this->hydraulic_data->setDemandPointModelRole(this->entity_uuid, model_role);
    });
    connect(this->date_added, &QDateEdit::dateChanged, this, [this](const QDate &)
    {
        this->hydraulic_data->setDemandPointDateAdded(
            this->entity_uuid, optionalDate(this->date_added));
    });
    connect(this->date_installed, &QDateEdit::dateChanged, this, [this](const QDate &)
    {
        this->hydraulic_data->setDemandPointDateInstalled(
            this->entity_uuid, optionalDate(this->date_installed));
    });
    connect(this->check_enabled, &QCheckBox::toggled, this, [this](bool enabled)
    {
        this->hydraulic_data->setDemandPointEnabled(this->entity_uuid, enabled);
    });
    connect(this->button_find_on_map, &QPushButton::clicked, this, [this]()
    {
        this->hydraulic_data->requestEntityLocate(this->entity_type, this->entity_uuid);
    });
    if (this->button_overview_find_on_map != nullptr)
    {
        this->button_overview_find_on_map->setEnabled(true);
        this->button_overview_find_on_map->show();
        connect(this->button_overview_find_on_map, &QPushButton::clicked, this, [this]()
        {
            this->hydraulic_data->requestEntityLocate(this->entity_type, this->entity_uuid);
        });
    }
    connect(this->hydraulic_data, &HydraulicData::signalDemandPointChanged,
            this, [this](const QUuid &uuid_changed)
    {
        if (uuid_changed != this->entity_uuid)
            return;

        refreshDemandPoint();
        scheduleDemandsRefresh();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]()
    {
        refreshDemandPoint();
        scheduleDemandsRefresh();
    });
}

void EntityInspectorWidget::refreshHydraulicGeneral(
    const QString &id, const HydraulicEntityMetadata &metadata)
{
    const QSignalBlocker name_blocker(this->line_name);
    const QSignalBlocker model_role_blocker(this->combo_model_role);
    const QSignalBlocker date_added_blocker(this->date_added);
    const QSignalBlocker date_installed_blocker(this->date_installed);
    const QSignalBlocker enabled_blocker(this->check_enabled);

    this->line_name->setText(id);
    const int model_role_index = this->combo_model_role->findData(
        static_cast<int>(metadata.model_role));
    this->combo_model_role->setCurrentIndex(model_role_index >= 0 ? model_role_index : 0);
    setOptionalDate(this->date_added, metadata.date_added);
    setOptionalDate(this->date_installed, metadata.date_installed);
    this->check_enabled->setChecked(metadata.enabled);
    this->displayed_entity_id = id;
    refreshEntityTitle();
}

void EntityInspectorWidget::refreshHydraulicNode()
{
    if (!this->hydraulic_data || this->entity_uuid.isNull())
        return;

    const std::optional<HydraulicNodeCommonData> node =
        this->hydraulic_data->nodeCommonData(this->entity_type, this->entity_uuid);
    if (!node.has_value())
        return;

    refreshHydraulicGeneral(node->id, node->metadata);

    // Presentation only: an active edit stays pending; six displayed decimals
    // must never be written back as a side effect of refreshing the inspector.
    static_cast<CoordinateNumericSpinBox *>(this->spin_latitude)->refreshCoordinate();
    static_cast<CoordinateNumericSpinBox *>(this->spin_longitude)->refreshCoordinate();
    this->refreshGeographicAltitude();

    refreshHydraulicNodeElevation();
}

void EntityInspectorWidget::refreshHydraulicLink()
{
    if (!this->hydraulic_data || this->entity_uuid.isNull())
        return;

    const std::optional<HydraulicLinkCommonData> link =
        this->hydraulic_data->linkCommonData(this->entity_type, this->entity_uuid);
    if (!link.has_value())
        return;

    refreshHydraulicGeneral(link->id, link->metadata);
    refreshHydraulicEndpoints();
}

void EntityInspectorWidget::refreshDemandPoint()
{
    if (!this->hydraulic_data || this->entity_uuid.isNull())
        return;

    const std::optional<HydraulicDemandPoint> demand_point =
        this->hydraulic_data->demandPoint(this->entity_uuid);
    if (!demand_point.has_value())
        return;

    refreshHydraulicGeneral(demand_point->id, demand_point->metadata);

    static_cast<CoordinateNumericSpinBox *>(this->spin_latitude)->refreshCoordinate();
    static_cast<CoordinateNumericSpinBox *>(this->spin_longitude)->refreshCoordinate();
    this->refreshGeographicAltitude();
}

void EntityInspectorWidget::refreshHydraulicEndpoints()
{
    if (!this->label_node_1_id || !this->label_node_2_id)
        return;

    this->node_uuid_1 = QUuid();
    this->node_uuid_2 = QUuid();

    if (this->hydraulic_data)
    {
        switch (this->entity_type)
        {
        case InfrastructureEntity::Pipe:
        {
            const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(this->entity_uuid);
            if (pipe.has_value())
            {
                this->node_uuid_1 = pipe->node_uuid_from;
                this->node_uuid_2 = pipe->node_uuid_to;
            }
            break;
        }
        case InfrastructureEntity::Pump:
        {
            const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(this->entity_uuid);
            if (pump.has_value())
            {
                this->node_uuid_1 = pump->node_uuid_from;
                this->node_uuid_2 = pump->node_uuid_to;
            }
            break;
        }
        case InfrastructureEntity::Valve:
        {
            const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(this->entity_uuid);
            if (valve.has_value())
            {
                this->node_uuid_1 = valve->node_uuid_from;
                this->node_uuid_2 = valve->node_uuid_to;
            }
            break;
        }
        default:
            break;
        }
    }

    refreshHydraulicEndpoint(this->node_uuid_1, this->label_node_1_id,
                             this->button_node_1_locate, this->button_node_1_inspect);
    refreshHydraulicEndpoint(this->node_uuid_2, this->label_node_2_id,
                             this->button_node_2_locate, this->button_node_2_inspect);
}

void EntityInspectorWidget::refreshHydraulicEndpoint(
    const QUuid &node_uuid, QLabel *label_node_id,
    QPushButton *button_locate, QPushButton *button_inspect)
{
    if (!label_node_id || !button_locate || !button_inspect)
        return;

    QString label_text = "[Not set]";
    QString tooltip;
    bool available = false;

    if (!node_uuid.isNull() && this->hydraulic_data)
    {
        const std::optional<InfrastructureEntity> node_type =
            this->hydraulic_data->nodeEntityType(node_uuid);
        if (node_type.has_value())
        {
            const std::optional<HydraulicNodeCommonData> node =
                this->hydraulic_data->nodeCommonData(node_type.value(), node_uuid);
            if (node.has_value())
            {
                label_text = node->id.isEmpty() ? "[Unnamed Node]" : node->id;
                tooltip = node_uuid.toString(QUuid::WithoutBraces);
                available = true;
            }
        }

        if (!available)
        {
            label_text = "[Missing Node]";
            tooltip = node_uuid.toString(QUuid::WithoutBraces);
        }
    }

    label_node_id->setText(label_text);
    label_node_id->setToolTip(tooltip);
    button_locate->setEnabled(available);
    button_inspect->setEnabled(available);
}

void EntityInspectorWidget::locateHydraulicEndpoint(const QUuid &node_uuid)
{
    if (!this->hydraulic_data || node_uuid.isNull())
        return;

    const std::optional<InfrastructureEntity> node_type =
        this->hydraulic_data->nodeEntityType(node_uuid);
    if (!node_type.has_value())
        return;

    this->hydraulic_data->requestEntityLocate(node_type.value(), node_uuid);
}

void EntityInspectorWidget::inspectHydraulicEndpoint(const QUuid &node_uuid)
{
    if (!this->hydraulic_data || node_uuid.isNull())
        return;

    const std::optional<InfrastructureEntity> node_type =
        this->hydraulic_data->nodeEntityType(node_uuid);
    if (!node_type.has_value())
        return;

    this->hydraulic_data->setSelectedUuid(node_type.value(), node_uuid);
}

std::optional<QDate> EntityInspectorWidget::optionalDate(const QDateEdit *date_edit) const
{
    if (!date_edit || date_edit->date() == date_edit->minimumDate())
        return std::nullopt;

    return date_edit->date();
}

void EntityInspectorWidget::setOptionalDate(QDateEdit *date_edit, const std::optional<QDate> &date)
{
    if (!date_edit)
        return;

    date_edit->setDate(date.has_value() ? date.value() : date_edit->minimumDate());
}

void EntityInspectorWidget::addGroupElevation()
{
    const char *group_title = nullptr;
    const char *absolute_input_text = nullptr;
    const char *offset_label_text = nullptr;
    const char *value_label_text = nullptr;
    const char *offset_tooltip = nullptr;

    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
        group_title = QT_TRANSLATE_NOOP("LiveUiText", "Elevation");
        absolute_input_text = QT_TRANSLATE_NOOP("LiveUiText", "Total Elevation");
        offset_label_text = QT_TRANSLATE_NOOP("LiveUiText", "Elevation Offset");
        value_label_text = QT_TRANSLATE_NOOP("LiveUiText", "Total Elevation");
        offset_tooltip =
            QT_TRANSLATE_NOOP("LiveUiText", "Distance from <i>Terrain Elevation</i>.<br>Positive: Above Ground.<br>Negative: Below Ground.");
        break;
    case InfrastructureEntity::Reservoir:
        group_title = QT_TRANSLATE_NOOP("LiveUiText", "Head");
        absolute_input_text = QT_TRANSLATE_NOOP("LiveUiText", "Total Head");
        offset_label_text = QT_TRANSLATE_NOOP("LiveUiText", "Head Offset");
        value_label_text = QT_TRANSLATE_NOOP("LiveUiText", "Total Head");
        offset_tooltip =
            QT_TRANSLATE_NOOP("LiveUiText", "Hydraulic head relative to <i>Terrain Elevation</i>. Positive values are above terrain.");
        break;
    case InfrastructureEntity::Tank:
        group_title = QT_TRANSLATE_NOOP("LiveUiText", "Bottom Elevation");
        absolute_input_text = QT_TRANSLATE_NOOP("LiveUiText", "Bottom Elevation");
        offset_label_text = QT_TRANSLATE_NOOP("LiveUiText", "Bottom Offset");
        value_label_text = QT_TRANSLATE_NOOP("LiveUiText", "Bottom Elevation");
        offset_tooltip =
            QT_TRANSLATE_NOOP("LiveUiText", "Tank bottom relative to terrain. Positive = above ground, negative = below ground.");
        break;
    default:
        return;
    }

    this->group_elevation = EntityInspectorText::group(group_title);
    QGridLayout *grid = new QGridLayout(this->group_elevation);
    
    this->combo_elevation_input_type = new QComboBox();
    EntityInspectorText::addChoice(this->combo_elevation_input_type, absolute_input_text,
        static_cast<int>(HydraulicNodeElevationInputType::AbsoluteElevation));
    EntityInspectorText::addChoice(this->combo_elevation_input_type, QT_TRANSLATE_NOOP("LiveUiText", "Terrain Elevation + Offset"), static_cast<int>(HydraulicNodeElevationInputType::TerrainElevationAndOffset));
    
    this->button_terrain_elevation = new QPushButton();
    LiveUiText::bind(this->button_terrain_elevation, [this] {
        refreshTerrainElevationPresentation();
    });
    this->label_terrain_elevation_status = new QLabel();
    this->label_terrain_elevation_status->setWordWrap(true);
    this->label_terrain_elevation_status->hide();

    if (!this->terrain_elevation_client)
    {
#ifdef AOWIS_STANDALONE
        this->terrain_elevation_client = new InterfaceServerMapStandalone(this);
#else
        this->terrain_elevation_client = new InterfaceServerMapREST(
            guiMapServerRestConfiguration(), this);
#endif
        connect(this->terrain_elevation_client,
                &InterfaceServerMap::signalTerrainElevationDataReceived, this,
                &EntityInspectorWidget::handleTerrainElevationResponse);
        connect(this->terrain_elevation_client,
                &InterfaceServerMap::signalTerrainElevationFailed, this,
                &EntityInspectorWidget::handleTerrainElevationError);
    }
    
    // All three node types use canonical metre values. Editors only present a
    // selected unit and must never write back a rounded display value.
    const auto node_editor = [this](int field) -> QDoubleSpinBox * {
        const std::string quantity = field == 1 ? AOWIS_QUANTITY("vertical_offset") :
            this->entity_type == InfrastructureEntity::Reservoir && field == 2
                ? AOWIS_QUANTITY("hydraulic_head") : AOWIS_QUANTITY("elevation");
        return new UnitNumericSpinBox(
            [this, field]() -> std::optional<double> {
                if (!this->hydraulic_data) return std::nullopt;
                switch (this->entity_type) {
                case InfrastructureEntity::Junction: {
                    const auto node = this->hydraulic_data->junction(this->entity_uuid);
                    if (!node) return std::nullopt;
                    if (field == 0) return node->terrain_elevation_m;
                    if (field == 1) return node->elevation_offset_m;
                    return node->elevation_input_type == HydraulicNodeElevationInputType::TerrainElevationAndOffset
                        ? node->terrain_elevation_m + node->elevation_offset_m : node->elevation_m;
                }
                case InfrastructureEntity::Reservoir: {
                    const auto node = this->hydraulic_data->reservoir(this->entity_uuid);
                    if (!node) return std::nullopt;
                    if (field == 0) return node->terrain_elevation_m;
                    if (field == 1) return node->hydraulic_head_offset_m;
                    return node->head_input_type == HydraulicNodeElevationInputType::TerrainElevationAndOffset
                        ? node->terrain_elevation_m + node->hydraulic_head_offset_m : node->hydraulic_head_m;
                }
                case InfrastructureEntity::Tank: {
                    const auto node = this->hydraulic_data->tank(this->entity_uuid);
                    if (!node) return std::nullopt;
                    if (field == 0) return node->terrain_elevation_m;
                    if (field == 1) return node->bottom_offset_m;
                    return node->elevation_input_type == HydraulicNodeElevationInputType::TerrainElevationAndOffset
                        ? node->terrain_elevation_m + node->bottom_offset_m : node->bottom_elevation_m;
                }
                default:
                    return std::nullopt;
                }
            },
            [this, field](double value) {
                if (!this->hydraulic_data) return false;
                if (field == 0) return setTerrainElevation(value);
                if (field == 1) return setElevationOffset(value);
                // The total is calculated from the two canonical properties in
                // terrain-relative mode, and must not be independently edited.
                if (!this->combo_elevation_input_type ||
                    static_cast<HydraulicNodeElevationInputType>(
                        this->combo_elevation_input_type->currentData().toInt())
                        != HydraulicNodeElevationInputType::AbsoluteElevation)
                    return false;
                return setElevationValue(value);
            }, quantity, "m", -10000.0, 10000.0, 0.1, 3, this);
    };

    this->label_terrain_elevation = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Terrain Elevation"), nullptr);
    this->spin_terrain_elevation = node_editor(0);

    this->label_elevation_offset = LiveUiText::caption<QLabel>(offset_label_text, nullptr);
    this->label_elevation_offset->setWordWrap(true);
    this->spin_elevation_offset = node_editor(1);
    LiveUiText::bind(this->spin_elevation_offset, [this, offset_tooltip] { this->spin_elevation_offset->setToolTip(LiveUiText::translate(offset_tooltip)); });

    this->label_elevation_value = LiveUiText::caption<QLabel>(value_label_text, nullptr);
    this->label_elevation_value->setWordWrap(true);
    this->spin_elevation_value = node_editor(2);
    LiveUiText::bind(this->spin_elevation_value, [this] {
        if (this->combo_elevation_input_type != nullptr
            && this->combo_elevation_input_type->currentData().toInt()
                == static_cast<int>(HydraulicNodeElevationInputType::TerrainElevationAndOffset))
        {
            this->spin_elevation_value->setToolTip(inspectorText(QT_TRANSLATE_NOOP(
                "EntityInspectorPresentation",
                "Calculated automatically from <i>Terrain Elevation</i> + <i>Offset</i>")));
        }
        else
        {
            this->spin_elevation_value->setToolTip(QString());
        }
    });

    grid->addWidget(this->combo_elevation_input_type, 0, 0, 1, 2);
    grid->addWidget(this->button_terrain_elevation, 1, 0, 1, 2);
    grid->addWidget(this->label_terrain_elevation_status, 2, 0, 1, 2);
    grid->addWidget(this->label_terrain_elevation, 3, 0);
    grid->addWidget(this->spin_terrain_elevation, 3, 1);
    grid->addWidget(this->label_elevation_offset, 4, 0);
    grid->addWidget(this->spin_elevation_offset, 4, 1);
    grid->addWidget(this->label_elevation_value, 5, 0);
    grid->addWidget(this->spin_elevation_value, 5, 1);

    if (this->entity_type == InfrastructureEntity::Reservoir)
    {
        QLabel *label_head_pattern = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Head Pattern"), nullptr);
        this->combo_head_pattern = new QComboBox();
        LiveUiText::bind(this->combo_head_pattern, [this] {
            this->combo_head_pattern->setToolTip(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
                "Select a time pattern for variable reservoir head, or Constant for a fixed head.")));
        });
        constrainComboWidth(this->combo_head_pattern);
        grid->addWidget(label_head_pattern, 6, 0);
        grid->addWidget(this->combo_head_pattern, 6, 1);

        connect(this->combo_head_pattern, &QComboBox::currentIndexChanged, this, [this](int)
        {
            const HydraulicTimePatternMode pattern_mode = static_cast<HydraulicTimePatternMode>(this->combo_head_pattern->currentData(pattern_mode_role).toInt());
            const QUuid pattern_uuid = this->combo_head_pattern->currentData(pattern_uuid_role).toUuid();

            this->hydraulic_data->setReservoirHeadPatternMode(this->entity_uuid, pattern_mode);
            this->hydraulic_data->setReservoirHeadPatternUuid(this->entity_uuid, pattern_uuid);
        });
    }
    
    connect(this->button_terrain_elevation, &QPushButton::clicked, this,
            &EntityInspectorWidget::requestTerrainElevation);
    connect(this->combo_elevation_input_type, &QComboBox::currentIndexChanged, this, &EntityInspectorWidget::onElevationInputTypeChanged);
    connect(this->group_elevation, &GroupBoxCollapsible::signalExpanded, this, &EntityInspectorWidget::onGroupExpand);
    
    this->layoutConfiguration()->addWidget(this->group_elevation);

    refreshHydraulicNodeElevation();
    new UnitAwareView(this->group_elevation, UnitProfileManager::instance().presentation(),
        [this] { refreshHydraulicNodeElevation(); });
}

void EntityInspectorWidget::onGroupExpand(GroupBoxCollapsible *group)
{
    if (group == this->group_elevation)
        updateElevationModeUi();
}

void EntityInspectorWidget::refreshHydraulicNodeElevation()
{
    if (!this->hydraulic_data || this->entity_uuid.isNull() ||
        !this->combo_elevation_input_type || !this->spin_terrain_elevation ||
        !this->spin_elevation_offset || !this->spin_elevation_value)
        return;

    HydraulicNodeElevationInputType input_type = HydraulicNodeElevationInputType::AbsoluteElevation;
    HydraulicTimePatternMode head_pattern_mode = HydraulicTimePatternMode::Constant;
    QUuid head_pattern_uuid;

    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
    {
        const std::optional<HydraulicNodeJunction> junction =
            this->hydraulic_data->junction(this->entity_uuid);
        if (!junction.has_value())
            return;

        input_type = junction->elevation_input_type;
        break;
    }
    case InfrastructureEntity::Reservoir:
    {
        const std::optional<HydraulicNodeReservoir> reservoir =
            this->hydraulic_data->reservoir(this->entity_uuid);
        if (!reservoir.has_value())
            return;

        input_type = reservoir->head_input_type;
        head_pattern_mode = reservoir->head_pattern_mode;
        head_pattern_uuid = reservoir->head_pattern_uuid;
        break;
    }
    case InfrastructureEntity::Tank:
    {
        const std::optional<HydraulicNodeTank> tank =
            this->hydraulic_data->tank(this->entity_uuid);
        if (!tank.has_value())
            return;

        input_type = tank->elevation_input_type;
        break;
    }
    default:
        return;
    }

    const QSignalBlocker input_type_blocker(this->combo_elevation_input_type);
    const QSignalBlocker terrain_blocker(this->spin_terrain_elevation);
    const QSignalBlocker offset_blocker(this->spin_elevation_offset);
    const QSignalBlocker value_blocker(this->spin_elevation_value);

    const int input_type_index = this->combo_elevation_input_type->findData(
        static_cast<int>(input_type));
    this->combo_elevation_input_type->setCurrentIndex(input_type_index >= 0 ? input_type_index : 0);
    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    static_cast<UnitNumericSpinBox *>(this->spin_terrain_elevation)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_elevation_offset)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_elevation_value)->refreshUnits(context);

    if (this->combo_head_pattern)
        populateTimePatternCombo(this->combo_head_pattern, head_pattern_mode, head_pattern_uuid);

    updateElevationModeUi();
}

void EntityInspectorWidget::updateElevationModeUi()
{
    if (!this->combo_elevation_input_type)
        return;

    const HydraulicNodeElevationInputType input_type =
        static_cast<HydraulicNodeElevationInputType>(
            this->combo_elevation_input_type->currentData().toInt());
    const bool uses_terrain =
        input_type == HydraulicNodeElevationInputType::TerrainElevationAndOffset;

    this->button_terrain_elevation->setVisible(uses_terrain);
    this->button_terrain_elevation->setEnabled(
        uses_terrain && !this->terrain_elevation_request_active);
    if (this->label_terrain_elevation_status)
        this->label_terrain_elevation_status->setVisible(
            uses_terrain && !this->label_terrain_elevation_status->text().isEmpty());
    this->label_terrain_elevation->setVisible(uses_terrain);
    this->spin_terrain_elevation->setVisible(uses_terrain);
    this->label_elevation_offset->setVisible(uses_terrain);
    this->spin_elevation_offset->setVisible(uses_terrain);
    this->spin_elevation_value->setReadOnly(uses_terrain);

    if (uses_terrain)
    {
        this->spin_elevation_value->setToolTip(
            inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
                "Calculated automatically from <i>Terrain Elevation</i> + <i>Offset</i>")));
        updateCalculatedElevation();
    }
    else
    {
        this->spin_elevation_value->setToolTip("");
    }
}

void EntityInspectorWidget::updateCalculatedElevation()
{
    if (!this->combo_elevation_input_type || !this->spin_elevation_value)
        return;

    const HydraulicNodeElevationInputType input_type =
        static_cast<HydraulicNodeElevationInputType>(
            this->combo_elevation_input_type->currentData().toInt());
    if (input_type != HydraulicNodeElevationInputType::TerrainElevationAndOffset)
        return;

    // The calculated total is read from canonical model fields, never from
    // rounded or differently unit-scaled editor values.
    static_cast<UnitNumericSpinBox *>(this->spin_elevation_value)->refreshUnits(
        UnitProfileManager::instance().presentation());
}

void EntityInspectorWidget::requestTerrainElevation()
{
    if (!this->hydraulic_data || this->entity_uuid.isNull() ||
        !this->terrain_elevation_client || this->terrain_elevation_request_active)
        return;

    const std::optional<HydraulicNodeCommonData> node =
        this->hydraulic_data->nodeCommonData(this->entity_type, this->entity_uuid);
    if (!node.has_value())
    {
        handleTerrainElevationError(
            "The node no longer exists in the hydraulic model.");
        return;
    }

    const double latitude_deg = node->coordinate_wgs84.latitude_deg;
    const double longitude_deg = node->coordinate_wgs84.longitude_deg;
    if (!std::isfinite(latitude_deg) || latitude_deg < -90.0 || latitude_deg > 90.0 ||
        !std::isfinite(longitude_deg) || longitude_deg < -180.0 || longitude_deg > 180.0)
    {
        handleTerrainElevationError(
            "The node does not have a valid WGS84 position.");
        return;
    }

    this->terrain_elevation_request_entity_uuid = this->entity_uuid;
    this->terrain_elevation_request_coordinate = node->coordinate_wgs84;
    this->terrain_result_available = false;
    if (this->label_terrain_elevation_status)
        this->label_terrain_elevation_status->clear();
    setTerrainElevationRequestActive(true);

    const QString endpoint = QStringLiteral(
        "/terrain/v1/elevation?latitude=%1&longitude=%2")
        .arg(latitude_deg, 0, 'f', 8)
        .arg(longitude_deg, 0, 'f', 8);
    this->terrain_elevation_client->requestTerrainElevation(endpoint);
}

void EntityInspectorWidget::handleTerrainElevationResponse(const QByteArray &data)
{
    if (!this->terrain_elevation_request_active)
        return;

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        handleTerrainElevationError(
            QStringLiteral("The AOWIS terrain subsystem returned invalid JSON: %1")
                .arg(parse_error.errorString()));
        return;
    }

    const QJsonObject response = document.object();
    if (response.value(QStringLiteral("status")).toString() != QStringLiteral("ready"))
    {
        const QString service_error = response.value(QStringLiteral("error")).toString();
        handleTerrainElevationError(
            service_error.isEmpty()
                ? QStringLiteral("The AOWIS terrain subsystem did not return a successful response.")
                : service_error);
        return;
    }

    const QJsonValue elevation_value = response.value(QStringLiteral("elevation_m"));
    if (!elevation_value.isDouble())
    {
        handleTerrainElevationError(
            "The AOWIS terrain subsystem returned no elevation for this position.");
        return;
    }

    const double elevation_m = elevation_value.toDouble();
    const QString dataset = response.value(QStringLiteral("dataset")).toString();
    const QString vertical_datum = response.value(QStringLiteral("vertical_datum")).toString();
    const double nominal_resolution_m =
        response.value(QStringLiteral("nominal_resolution_m")).toDouble(-1.0);

    if (this->entity_uuid != this->terrain_elevation_request_entity_uuid)
    {
        setTerrainElevationRequestActive(false);
        return;
    }

    const std::optional<HydraulicNodeCommonData> node =
        this->hydraulic_data->nodeCommonData(this->entity_type, this->entity_uuid);
    if (!node.has_value())
    {
        handleTerrainElevationError(
            "The node no longer exists in the hydraulic model.");
        return;
    }

    constexpr double coordinate_tolerance = 1e-10;
    const bool position_changed =
        std::abs(node->coordinate_wgs84.latitude_deg -
                 this->terrain_elevation_request_coordinate.latitude_deg) >
            coordinate_tolerance ||
        std::abs(node->coordinate_wgs84.longitude_deg -
                 this->terrain_elevation_request_coordinate.longitude_deg) >
            coordinate_tolerance;
    if (position_changed)
    {
        handleTerrainElevationError(
            "The node position changed while the terrain elevation was being retrieved. Please try again.");
        return;
    }

    if (!std::isfinite(elevation_m) || !setTerrainElevation(elevation_m))
    {
        handleTerrainElevationError(
            "The retrieved terrain elevation could not be written to the hydraulic model.");
        return;
    }

    // Retain raw result metadata, not pretranslated text, so language changes
    // can reformat the tooltip and status without requesting the DEM again.
    this->terrain_result_available = true;
    this->terrain_result_elevation_m = elevation_m;
    this->terrain_result_dataset = dataset;
    this->terrain_result_vertical_datum = vertical_datum;
    this->terrain_result_resolution_m = nominal_resolution_m;
    setTerrainElevationRequestActive(false);
}

void EntityInspectorWidget::handleTerrainElevationError(const QString &error)
{
    if (this->label_terrain_elevation_status)
        this->label_terrain_elevation_status->clear();
    this->terrain_result_available = false;

    setTerrainElevationRequestActive(false);
    showTerrainElevationErrorMessage(error);
}

void EntityInspectorWidget::showTerrainElevationErrorMessage(const QString &error)
{
    if (this->terrain_elevation_message_box)
        this->terrain_elevation_message_box->close();

    QWidget *parent_window = this->window();
    if (!parent_window)
        parent_window = this;

    QMessageBox *message_box = new QMessageBox(
        QMessageBox::Warning, QString(), QString(), QMessageBox::NoButton, parent_window);
    message_box->setInformativeText(error);
    QPushButton *button_ok = message_box->addButton(QString(), QMessageBox::AcceptRole);
    LiveUiText::bind(message_box, [message_box] {
        message_box->setWindowTitle(inspectorText(QT_TRANSLATE_NOOP(
            "EntityInspectorPresentation", "Terrain Elevation")));
        message_box->setText(inspectorText(QT_TRANSLATE_NOOP(
            "EntityInspectorPresentation", "The terrain elevation could not be retrieved.")));
    });
    LiveUiText::bind(button_ok, [button_ok] {
        button_ok->setText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "OK")));
    });
    message_box->setAttribute(Qt::WA_DeleteOnClose);

#ifdef Q_OS_WASM
    message_box->setWindowModality(Qt::NonModal);
    message_box->adjustSize();
    const QPoint initial_parent_center = parent_window->mapToGlobal(
        parent_window->rect().center());
    message_box->move(
        initial_parent_center.x() - message_box->width() / 2,
        initial_parent_center.y() - message_box->height() / 2);
#else
    message_box->setWindowModality(Qt::WindowModal);
#endif

    this->terrain_elevation_message_box = message_box;
    message_box->open();

#ifdef Q_OS_WASM
    QPointer<QMessageBox> guarded_message_box = message_box;
    QPointer<QWidget> guarded_parent_window = parent_window;
    QTimer::singleShot(0, message_box, [guarded_message_box, guarded_parent_window]()
    {
        if (!guarded_message_box || !guarded_parent_window)
            return;

        guarded_message_box->adjustSize();
        const QPoint parent_center = guarded_parent_window->mapToGlobal(
            guarded_parent_window->rect().center());
        guarded_message_box->move(
            parent_center.x() - guarded_message_box->width() / 2,
            parent_center.y() - guarded_message_box->height() / 2);
    });
#endif
}

void EntityInspectorWidget::refreshTerrainElevationPresentation()
{
    if (this->button_terrain_elevation == nullptr)
        return;

    this->button_terrain_elevation->setText(this->terrain_elevation_request_active
        ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Retrieving Terrain Elevation..."))
        : LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Retrieve Terrain Elevation")));
    QString tooltip = inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
        "Uses the AOWIS terrain subsystem and its shared DEM cache.<br>Accuracy depends on the dataset and local terrain."));
    if (this->terrain_result_available)
    {
        QStringList metadata;
        if (!this->terrain_result_dataset.isEmpty())
            metadata.append(this->terrain_result_dataset);
        metadata.append(terrainDatumDisplayName(this->terrain_result_vertical_datum));
        if (std::isfinite(this->terrain_result_resolution_m)
            && this->terrain_result_resolution_m > 0.0)
        {
            metadata.append(inspectorText(QT_TRANSLATE_NOOP(
                "EntityInspectorPresentation", "%1 m source resolution"))
                .arg(this->terrain_result_resolution_m, 0, 'f',
                     this->terrain_result_resolution_m < 10.0 ? 1 : 0));
        }
        const QString suffix = metadata.isEmpty() ? QString()
            : QStringLiteral(" (%1)").arg(metadata.join(QStringLiteral(", ")));
        const QString altitude = QString::number(this->terrain_result_elevation_m, 'f', 3);
        tooltip += inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
            "<br>Last result: %1 m%2")).arg(altitude, suffix);
        if (this->label_terrain_elevation_status != nullptr)
        {
            this->label_terrain_elevation_status->setText(inspectorText(QT_TRANSLATE_NOOP(
                "EntityInspectorPresentation", "Retrieved %1 m%2.")).arg(altitude, suffix));
        }
    }
    this->button_terrain_elevation->setToolTip(tooltip);
}

void EntityInspectorWidget::setTerrainElevationRequestActive(bool active)
{
    this->terrain_elevation_request_active = active;

    if (this->button_terrain_elevation == nullptr)
        return;

    refreshTerrainElevationPresentation();
    updateElevationModeUi();
}

void EntityInspectorWidget::onElevationInputTypeChanged(int index)
{
    Q_UNUSED(index)

    const HydraulicNodeElevationInputType input_type =
        static_cast<HydraulicNodeElevationInputType>(
            this->combo_elevation_input_type->currentData().toInt());
    // Update the model first: the displayed calculated value must be read from
    // its new canonical input mode, never derived from rounded spin-box values.
    setElevationInputType(input_type);
    refreshHydraulicNodeElevation();
}

bool EntityInspectorWidget::setElevationInputType(HydraulicNodeElevationInputType input_type)
{
    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
        return this->hydraulic_data->setJunctionElevationInputType(this->entity_uuid, input_type);
    case InfrastructureEntity::Reservoir:
        return this->hydraulic_data->setReservoirHeadInputType(this->entity_uuid, input_type);
    case InfrastructureEntity::Tank:
        return this->hydraulic_data->setTankElevationInputType(this->entity_uuid, input_type);
    default:
        return false;
    }
}

bool EntityInspectorWidget::setElevationValue(double value_m)
{
    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
        return this->hydraulic_data->setJunctionElevationM(this->entity_uuid, value_m);
    case InfrastructureEntity::Reservoir:
        return this->hydraulic_data->setReservoirHeadM(this->entity_uuid, value_m);
    case InfrastructureEntity::Tank:
        return this->hydraulic_data->setTankBottomElevationM(this->entity_uuid, value_m);
    default:
        return false;
    }
}

bool EntityInspectorWidget::setTerrainElevation(double terrain_elevation_m)
{
    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
        return this->hydraulic_data->setJunctionTerrainElevationM(
            this->entity_uuid, terrain_elevation_m);
    case InfrastructureEntity::Reservoir:
        return this->hydraulic_data->setReservoirTerrainElevationM(
            this->entity_uuid, terrain_elevation_m);
    case InfrastructureEntity::Tank:
        return this->hydraulic_data->setTankTerrainElevationM(
            this->entity_uuid, terrain_elevation_m);
    default:
        return false;
    }
}

bool EntityInspectorWidget::setElevationOffset(double offset_m)
{
    switch (this->entity_type)
    {
    case InfrastructureEntity::Junction:
        return this->hydraulic_data->setJunctionElevationOffsetM(this->entity_uuid, offset_m);
    case InfrastructureEntity::Reservoir:
        return this->hydraulic_data->setReservoirHeadOffsetM(this->entity_uuid, offset_m);
    case InfrastructureEntity::Tank:
        return this->hydraulic_data->setTankBottomOffsetM(this->entity_uuid, offset_m);
    default:
        return false;
    }
}

void EntityInspectorWidget::onHeadlossFormulaChanged(HeadlossFormulas formulas)
{
    Q_UNUSED(formulas)
}

void EntityInspectorWidget::addGroupDemands(bool include_emitter)
{
    GroupBoxCollapsible *group = EntityInspectorText::group(include_emitter ? QT_TRANSLATE_NOOP("LiveUiText", "Demands / Emitter") : QT_TRANSLATE_NOOP("LiveUiText", "Demands"));
    QGridLayout *grid = new QGridLayout(group);

    this->label_demands_summary = new QLabel();
    this->label_demands_summary->setWordWrap(true);

    QPushButton *button_editor = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Open Editor"), nullptr);
    connect(button_editor, &QPushButton::clicked, this, [this]()
    {
        openDemandsEditor();
    });

    int row = 0;
    grid->addWidget(this->label_demands_summary, row++, 0, 1, 2);

    if (include_emitter)
    {
        QLabel *label_emitter_coefficient = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Emitter<br>Coefficient"), nullptr);
        const QUuid junction_uuid = this->entity_uuid;
        HydraulicData *data = this->hydraulic_data;
        const auto read_junction = [data, junction_uuid]() -> std::optional<HydraulicNodeJunction> {
            if (!data || junction_uuid.isNull()) return std::nullopt;
            return data->junction(junction_uuid);
        };
        const auto read_exponent = [read_junction]() -> std::optional<double> {
            const std::optional<HydraulicNodeJunction> junction = read_junction();
            return junction ? std::optional<double>(junction->emitter.pressure_exponent)
                            : std::nullopt;
        };

        EmitterCoefficientNumericSpinBox *coefficient = new EmitterCoefficientNumericSpinBox(
            [read_junction]() -> std::optional<double> {
                const std::optional<HydraulicNodeJunction> junction = read_junction();
                return junction ? std::optional<double>(junction->emitter.coefficient)
                                : std::nullopt;
            },
            [data, junction_uuid](double value) {
                return data && data->setJunctionEmitterCoefficient(junction_uuid, value);
            }, read_exponent);
        this->spin_emitter_coefficient = coefficient;
        coefficient->setAlignment(Qt::AlignRight);
        LiveUiText::bind(coefficient, [coefficient] {
            coefficient->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText",
                "Coefficient C in Q = C · hⁿ. Q and h use the selected flow and pressure-head "
                "units. The coefficient dimension changes with the exponent n.")));
        });
        constrainSpinWidth(coefficient);

        QLabel *label_emitter_pressure_exponent = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Emitter<br>Exponent"), nullptr);
        UnitNumericSpinBox *exponent = new UnitNumericSpinBox(
            read_exponent,
            [data, junction_uuid](double value) {
                return data && data->setJunctionEmitterPressureExponent(junction_uuid, value);
            }, AOWIS_QUANTITY("emitter_exponent"), "1", 0.000001, 1000000.0, 0.01, 10,
            nullptr, [](const QString &) { return QString(); });
        this->spin_emitter_pressure_exponent = exponent;
        exponent->setAlignment(Qt::AlignRight);
        LiveUiText::bind(exponent, [exponent] {
            exponent->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText",
                "Dimensionless exponent n in Q = C · hⁿ. EPANET supports one exponent "
                "for the network, so all enabled junctions with a non-zero emitter "
                "coefficient must use the same value.")));
        });
        constrainSpinWidth(exponent);

        grid->addWidget(label_emitter_coefficient, row, 0);
        grid->addWidget(coefficient, row++, 1);
        this->label_emitter_coefficient_unit = new QLabel();
        this->label_emitter_coefficient_unit->setWordWrap(true);
        LiveUiText::bind(this->label_emitter_coefficient_unit, [this] {
            if (this->spin_emitter_coefficient == nullptr)
                return;
            const EmitterCoefficientNumericSpinBox *coefficient =
                static_cast<const EmitterCoefficientNumericSpinBox *>(this->spin_emitter_coefficient);
            this->label_emitter_coefficient_unit->setText(
                inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Unit: %1"))
                    .arg(coefficient->displayedUnitLabel()));
        });
        grid->addWidget(this->label_emitter_coefficient_unit, row++, 0, 1, 2);
        grid->addWidget(label_emitter_pressure_exponent, row, 0);
        grid->addWidget(exponent, row++, 1);
    }

    grid->addWidget(button_editor, row, 0, 1, 2);

    this->layoutConfiguration()->addWidget(group);
    refreshDemands();
    // One lazy presentation region updates the demand summary and its emitter
    // editors together, rather than subscribing each numeric input.
    new UnitAwareView(group, UnitProfileManager::instance().presentation(),
        [this] { refreshDemands(true); });
}

std::optional<QList<HydraulicDemand>> EntityInspectorWidget::currentDemands() const
{
    if (!this->hydraulic_data || this->entity_uuid.isNull())
        return std::nullopt;

    if (this->entity_type == InfrastructureEntity::Junction)
    {
        const std::optional<HydraulicNodeJunction> junction =
            this->hydraulic_data->junction(this->entity_uuid);
        if (!junction.has_value())
            return std::nullopt;
        return junction->demands;
    }

    if (this->entity_type == InfrastructureEntity::DemandPoint)
    {
        const std::optional<HydraulicDemandPoint> demand_point =
            this->hydraulic_data->demandPoint(this->entity_uuid);
        if (!demand_point.has_value())
            return std::nullopt;
        return demand_point->demands;
    }

    return std::nullopt;
}

bool EntityInspectorWidget::addCurrentDemand(const HydraulicDemand &demand)
{
    if (this->entity_type == InfrastructureEntity::Junction)
        return this->hydraulic_data->addJunctionDemand(this->entity_uuid, demand);
    if (this->entity_type == InfrastructureEntity::DemandPoint)
        return this->hydraulic_data->addDemandPointDemand(this->entity_uuid, demand);
    return false;
}

bool EntityInspectorWidget::removeCurrentDemand(int demand_index)
{
    if (this->entity_type == InfrastructureEntity::Junction)
        return this->hydraulic_data->removeJunctionDemand(this->entity_uuid, demand_index);
    if (this->entity_type == InfrastructureEntity::DemandPoint)
        return this->hydraulic_data->removeDemandPointDemand(this->entity_uuid, demand_index);
    return false;
}

bool EntityInspectorWidget::setCurrentDemandCategoryName(
    int demand_index, const QString &category_name)
{
    if (this->entity_type == InfrastructureEntity::Junction)
    {
        return this->hydraulic_data->setJunctionDemandCategoryName(
            this->entity_uuid, demand_index, category_name);
    }
    if (this->entity_type == InfrastructureEntity::DemandPoint)
    {
        return this->hydraulic_data->setDemandPointDemandCategoryName(
            this->entity_uuid, demand_index, category_name);
    }
    return false;
}

bool EntityInspectorWidget::setCurrentDemandBaseDemandM3PerH(
    int demand_index, double base_demand_m3_per_h)
{
    if (this->entity_type == InfrastructureEntity::Junction)
    {
        return this->hydraulic_data->setJunctionDemandBaseDemandM3PerH(
            this->entity_uuid, demand_index, base_demand_m3_per_h);
    }
    if (this->entity_type == InfrastructureEntity::DemandPoint)
    {
        return this->hydraulic_data->setDemandPointDemandBaseDemandM3PerH(
            this->entity_uuid, demand_index, base_demand_m3_per_h);
    }
    return false;
}

bool EntityInspectorWidget::setCurrentDemandPatternMode(
    int demand_index, HydraulicTimePatternMode pattern_mode)
{
    if (this->entity_type == InfrastructureEntity::Junction)
    {
        return this->hydraulic_data->setJunctionDemandPatternMode(
            this->entity_uuid, demand_index, pattern_mode);
    }
    if (this->entity_type == InfrastructureEntity::DemandPoint)
    {
        return this->hydraulic_data->setDemandPointDemandPatternMode(
            this->entity_uuid, demand_index, pattern_mode);
    }
    return false;
}

bool EntityInspectorWidget::setCurrentDemandPatternUuid(
    int demand_index, const QUuid &pattern_uuid)
{
    if (this->entity_type == InfrastructureEntity::Junction)
    {
        return this->hydraulic_data->setJunctionDemandPatternUuid(
            this->entity_uuid, demand_index, pattern_uuid);
    }
    if (this->entity_type == InfrastructureEntity::DemandPoint)
    {
        return this->hydraulic_data->setDemandPointDemandPatternUuid(
            this->entity_uuid, demand_index, pattern_uuid);
    }
    return false;
}

bool EntityInspectorWidget::setCurrentDemandSourceMethod(
    int demand_index, HydraulicDemandSourceMethod source_method)
{
    if (this->entity_type == InfrastructureEntity::Junction)
    {
        return this->hydraulic_data->setJunctionDemandSourceMethod(
            this->entity_uuid, demand_index, source_method);
    }
    if (this->entity_type == InfrastructureEntity::DemandPoint)
    {
        return this->hydraulic_data->setDemandPointDemandSourceMethod(
            this->entity_uuid, demand_index, source_method);
    }
    return false;
}

bool EntityInspectorWidget::setCurrentDemandNote(int demand_index, const QString &note)
{
    if (this->entity_type == InfrastructureEntity::Junction)
        return this->hydraulic_data->setJunctionDemandNote(this->entity_uuid, demand_index, note);
    if (this->entity_type == InfrastructureEntity::DemandPoint)
        return this->hydraulic_data->setDemandPointDemandNote(this->entity_uuid, demand_index, note);
    return false;
}

void EntityInspectorWidget::scheduleDemandsRefresh()
{
    const auto demands = currentDemands();
    if (this->table_demands && (!demands || demands->size() != this->table_demands->rowCount()))
        this->demands_structure_stale = true;
    const bool supported = this->entity_type == InfrastructureEntity::Junction ||
        this->entity_type == InfrastructureEntity::DemandPoint;
    if (!supported || this->demands_refresh_pending)
        return;

    this->demands_refresh_pending = true;
    QTimer::singleShot(0, this, [this]()
    {
        this->demands_refresh_pending = false;
        refreshDemands();
    });
}

void EntityInspectorWidget::refreshDemandSummary()
{
    if (this->label_demands_summary == nullptr || this->hydraulic_data == nullptr)
        return;

    const std::optional<QList<HydraulicDemand>> demands_optional = currentDemands();
    if (!demands_optional.has_value())
        return;

    const QList<HydraulicDemand> &demands = demands_optional.value();
    {
        double total_base_demand_m3_per_h = 0.0;
        for (const HydraulicDemand &demand : demands)
            total_base_demand_m3_per_h += demand.base_demand_m3_per_h;

        const int demand_count = demands.size();
        const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
        const QString quantity = this->entity_type == InfrastructureEntity::DemandPoint
            ? AOWIS_QSTRING_QUANTITY("customer_demand_flow_rate")
            : AOWIS_QSTRING_QUANTITY("base_demand_flow_rate");
        const QString unit = context.unit(quantity);
        const std::optional<double> total = aowis::units::convert(
            quantity.toStdString(), total_base_demand_m3_per_h, "m3/h", unit.toStdString());
        const std::optional<QString> text = NumericText::measurement(
            total, unit, context.locale(), context.formatPolicy(quantity));
        this->label_demands_summary->setText(
            QCoreApplication::translate("EntityInspectorPresentation",
                "%n demand categories\nTotal base demand: %1", nullptr, demand_count)
                .arg(text.value_or(QStringLiteral("—"))));
    }

}

void EntityInspectorWidget::refreshDemands(bool presentation_only)
{
    const std::optional<QList<HydraulicDemand>> demands_optional = currentDemands();
    if (!demands_optional.has_value())
        return;

    const QList<HydraulicDemand> &demands = demands_optional.value();

    refreshDemandSummary();

    if (this->entity_type == InfrastructureEntity::Junction)
    {
        const std::optional<HydraulicNodeJunction> junction =
            this->hydraulic_data->junction(this->entity_uuid);
        if (junction.has_value())
        {
            const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
            if (this->spin_emitter_coefficient)
            {
                EmitterCoefficientNumericSpinBox *coefficient =
                    static_cast<EmitterCoefficientNumericSpinBox *>(this->spin_emitter_coefficient);
                coefficient->refreshUnits(context);
                if (this->label_emitter_coefficient_unit)
                    this->label_emitter_coefficient_unit->setText(
                        inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Unit: %1"))
                            .arg(coefficient->displayedUnitLabel()));
            }
            if (this->spin_emitter_pressure_exponent)
            {
                UnitNumericSpinBox *exponent =
                    static_cast<UnitNumericSpinBox *>(this->spin_emitter_pressure_exponent);
                exponent->refreshUnits(context);
            }
        }
    }

    if (!this->table_demands || (presentation_only && !this->table_demands->isVisible()))
        return;

    if (this->demands_structure_stale || this->table_demands->rowCount() != demands.size())
    {
        rebuildDemandRows(demands);
        return;
    }

    for (int demand_index = 0; demand_index < demands.size(); demand_index++)
        updateDemandRow(demand_index, demands.at(demand_index));
}

void EntityInspectorWidget::rebuildDemandRows(const QList<HydraulicDemand> &demands)
{
    if (!this->table_demands)
        return;

    // Row indices are part of the binding identity. Invalidate existing
    // editors before rows are removed/reordered for either entity type.
    for (int row = 0; row < this->table_demands->rowCount(); ++row)
    {
        QWidget *widget = this->table_demands->cellWidget(row, 1);
        if (!widget)
            continue;
        UnitNumericSpinBox *editor = static_cast<UnitNumericSpinBox *>(widget);
        editor->cancelEdit();
        editor->setEnabled(false);
    }
    this->demands_structure_stale = false;
    this->table_demands->setUpdatesEnabled(false);
    this->table_demands->clearContents();
    this->table_demands->setRowCount(demands.size());

    for (int demand_index = 0; demand_index < demands.size(); demand_index++)
        addDemandRow(demand_index, demands.at(demand_index));

    this->table_demands->setUpdatesEnabled(true);
}

void EntityInspectorWidget::addDemandRow(
    int demand_index, const HydraulicDemand &demand)
{
    if (!this->table_demands)
        return;

    QLineEdit *line_category = new QLineEdit(this->table_demands);
    UnitNumericSpinBox *spin_base_demand = new UnitNumericSpinBox(
        [this, demand_index]() -> std::optional<double> {
            const std::optional<QList<HydraulicDemand>> demands = currentDemands();
            if (this->demands_structure_stale || !demands ||
                demand_index < 0 || demand_index >= demands->size())
                return std::nullopt;
            return demands->at(demand_index).base_demand_m3_per_h;
        },
        [this, demand_index](double value) {
            return !this->demands_structure_stale &&
                setCurrentDemandBaseDemandM3PerH(demand_index, value);
        }, this->entity_type == InfrastructureEntity::DemandPoint
               ? AOWIS_QUANTITY("customer_demand_flow_rate") : AOWIS_QUANTITY("base_demand_flow_rate"),
        "m3/h", -1000000000.0, 1000000000.0,
        0.1, 6, this->table_demands);
    spin_base_demand->setAlignment(Qt::AlignRight);

    QComboBox *combo_pattern = new QComboBox(this->table_demands);

    QComboBox *combo_source = new QComboBox(this->table_demands);
    EntityInspectorText::addChoice(combo_source, QT_TRANSLATE_NOOP("LiveUiText", "Manual Estimation"), static_cast<int>(HydraulicDemandSourceMethod::ManualEstimation));
    EntityInspectorText::addChoice(combo_source, QT_TRANSLATE_NOOP("LiveUiText", "Meter Data"), static_cast<int>(HydraulicDemandSourceMethod::MeterData));
    EntityInspectorText::addChoice(combo_source, QT_TRANSLATE_NOOP("LiveUiText", "Scenario"), static_cast<int>(HydraulicDemandSourceMethod::Scenario));

    QLineEdit *line_note = new QLineEdit(this->table_demands);
    QPushButton *button_delete = new QPushButton(QIcon(":/icon/remove.png"), "", this->table_demands);
    LiveUiText::bind(button_delete, [button_delete] {
        button_delete->setToolTip(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "Delete this demand category")));
    });
    button_delete->setMaximumWidth(35);

    this->table_demands->setCellWidget(demand_index, 0, line_category);
    this->table_demands->setCellWidget(demand_index, 1, spin_base_demand);
    this->table_demands->setCellWidget(demand_index, 2, combo_pattern);
    this->table_demands->setCellWidget(demand_index, 3, combo_source);
    this->table_demands->setCellWidget(demand_index, 4, line_note);
    this->table_demands->setCellWidget(demand_index, 5, button_delete);

    updateDemandRow(demand_index, demand);

    connect(line_category, &QLineEdit::textEdited, this,
            [this, demand_index](const QString &category_name)
    {
        setCurrentDemandCategoryName(demand_index, category_name);
    });
    connect(combo_pattern, &QComboBox::currentIndexChanged, this,
            [this, demand_index, combo_pattern](int)
    {
        const HydraulicTimePatternMode pattern_mode =
            static_cast<HydraulicTimePatternMode>(
                combo_pattern->currentData(pattern_mode_role).toInt());
        const QUuid pattern_uuid = combo_pattern->currentData(pattern_uuid_role).toUuid();

        setCurrentDemandPatternMode(demand_index, pattern_mode);
        setCurrentDemandPatternUuid(demand_index, pattern_uuid);
    });
    connect(combo_source, &QComboBox::currentIndexChanged, this,
            [this, demand_index, combo_source](int)
    {
        const HydraulicDemandSourceMethod source_method =
            static_cast<HydraulicDemandSourceMethod>(combo_source->currentData().toInt());
        setCurrentDemandSourceMethod(demand_index, source_method);
    });
    connect(line_note, &QLineEdit::textEdited, this,
            [this, demand_index](const QString &note)
    {
        setCurrentDemandNote(demand_index, note);
    });
    connect(button_delete, &QPushButton::clicked, this, [this, demand_index]()
    {
        removeCurrentDemand(demand_index);
    });
}

void EntityInspectorWidget::updateDemandRow(
    int demand_index, const HydraulicDemand &demand)
{
    if (!this->table_demands || demand_index < 0 || demand_index >= this->table_demands->rowCount())
        return;

    QLineEdit *line_category = qobject_cast<QLineEdit *>(
        this->table_demands->cellWidget(demand_index, 0));
    UnitNumericSpinBox *spin_base_demand = static_cast<UnitNumericSpinBox *>(
        this->table_demands->cellWidget(demand_index, 1));
    QComboBox *combo_pattern = qobject_cast<QComboBox *>(
        this->table_demands->cellWidget(demand_index, 2));
    QComboBox *combo_source = qobject_cast<QComboBox *>(
        this->table_demands->cellWidget(demand_index, 3));
    QLineEdit *line_note = qobject_cast<QLineEdit *>(
        this->table_demands->cellWidget(demand_index, 4));

    if (line_category && line_category->text() != demand.category_name)
    {
        const QSignalBlocker category_blocker(line_category);
        line_category->setText(demand.category_name);
    }

    if (spin_base_demand)
    {
        const QSignalBlocker base_demand_blocker(spin_base_demand);
        spin_base_demand->refreshUnits(UnitProfileManager::instance().presentation());
    }

    if (combo_pattern)
        populateTimePatternCombo(combo_pattern, demand.pattern_mode, demand.pattern_uuid);

    if (combo_source)
    {
        const int source_index = combo_source->findData(static_cast<int>(demand.source_method));
        const QSignalBlocker source_blocker(combo_source);
        combo_source->setCurrentIndex(source_index >= 0 ? source_index : 0);
    }

    if (line_note && line_note->text() != demand.note)
    {
        const QSignalBlocker note_blocker(line_note);
        line_note->setText(demand.note);
    }
}

void EntityInspectorWidget::populateTimePatternCombo(
    QComboBox *combo_pattern, HydraulicTimePatternMode pattern_mode,
    const QUuid &pattern_uuid)
{
    if (!combo_pattern || !this->hydraulic_data)
        return;

    const QList<HydraulicPatternTime> &patterns =
        this->hydraulic_data->networkHydraulic().patterns_time;
    QString pattern_signature = QString::number(static_cast<int>(pattern_mode));
    pattern_signature += QLatin1Char(':');
    pattern_signature += pattern_uuid.toString(QUuid::WithoutBraces);
    pattern_signature += QLatin1Char('\n');

    for (const HydraulicPatternTime &pattern : patterns)
    {
        pattern_signature += pattern.uuid.toString(QUuid::WithoutBraces);
        pattern_signature += QLatin1Char(':');
        pattern_signature += pattern.id;
        pattern_signature += QLatin1Char('\n');
    }

    bool pattern_exists = false;
    if (pattern_mode == HydraulicTimePatternMode::TimePattern && !pattern_uuid.isNull())
    {
        for (const HydraulicPatternTime &pattern : patterns)
        {
            if (pattern.uuid == pattern_uuid)
            {
                pattern_exists = true;
                break;
            }
        }
    }

    const QSignalBlocker pattern_blocker(combo_pattern);
    if (combo_pattern->property("aowisPatternSignature").toString() != pattern_signature)
    {
        combo_pattern->clear();

        EntityInspectorText::addChoice(combo_pattern, QT_TRANSLATE_NOOP("LiveUiText", "Constant"));
        combo_pattern->setItemData(
            combo_pattern->count() - 1,
            static_cast<int>(HydraulicTimePatternMode::Constant), pattern_mode_role);
        combo_pattern->setItemData(
            combo_pattern->count() - 1, QUuid(), pattern_uuid_role);

        EntityInspectorText::addChoice(combo_pattern, QT_TRANSLATE_NOOP("LiveUiText", "[Select Pattern]"));
        combo_pattern->setItemData(
            combo_pattern->count() - 1,
            static_cast<int>(HydraulicTimePatternMode::TimePattern), pattern_mode_role);
        combo_pattern->setItemData(
            combo_pattern->count() - 1, QUuid(), pattern_uuid_role);

        for (const HydraulicPatternTime &pattern : patterns)
        {
            const QString pattern_name = pattern.id.isEmpty()
                ? pattern.uuid.toString(QUuid::WithoutBraces)
                : pattern.id;
            combo_pattern->addItem(pattern_name);
            combo_pattern->setItemData(
                combo_pattern->count() - 1,
                static_cast<int>(HydraulicTimePatternMode::TimePattern), pattern_mode_role);
            combo_pattern->setItemData(
                combo_pattern->count() - 1, pattern.uuid, pattern_uuid_role);
        }

        if (pattern_mode == HydraulicTimePatternMode::TimePattern &&
            !pattern_uuid.isNull() && !pattern_exists)
        {
            combo_pattern->addItem(
                QStringLiteral("[Missing Pattern] %1").arg(
                    pattern_uuid.toString(QUuid::WithoutBraces)));
            combo_pattern->setItemData(
                combo_pattern->count() - 1,
                static_cast<int>(HydraulicTimePatternMode::TimePattern), pattern_mode_role);
            combo_pattern->setItemData(
                combo_pattern->count() - 1, pattern_uuid, pattern_uuid_role);
        }

        combo_pattern->setProperty("aowisPatternSignature", pattern_signature);
    }

    int selected_index = -1;
    for (int item_index = 0; item_index < combo_pattern->count(); item_index++)
    {
        const HydraulicTimePatternMode item_mode =
            static_cast<HydraulicTimePatternMode>(
                combo_pattern->itemData(item_index, pattern_mode_role).toInt());
        const QUuid item_uuid = combo_pattern->itemData(item_index, pattern_uuid_role).toUuid();

        const bool mode_matches = item_mode == pattern_mode;
        const bool uuid_matches = pattern_mode == HydraulicTimePatternMode::Constant ||
            item_uuid == pattern_uuid;

        if (mode_matches && uuid_matches)
        {
            selected_index = item_index;
            break;
        }
    }

    combo_pattern->setCurrentIndex(selected_index >= 0 ? selected_index : 0);
}

void EntityInspectorWidget::openDemandsEditor()
{
    if (this->dialog_demands)
    {
        this->dialog_demands->raise();
        this->dialog_demands->activateWindow();
        return;
    }

    this->dialog_demands = new QDialog(this);
    LiveUiText::bind(this->dialog_demands, [this] {
        this->dialog_demands->setWindowTitle(this->entity_type == InfrastructureEntity::DemandPoint
            ? LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Demand Point Demands")) : LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Junction Demands")));
    });
    this->dialog_demands->resize(950, 420);
    this->dialog_demands->setAttribute(Qt::WA_DeleteOnClose);

    QGridLayout *grid = new QGridLayout(this->dialog_demands);

    this->table_demands = new QTableWidget(this->dialog_demands);
    this->table_demands->setColumnCount(6);
    LiveUiText::bind(this->table_demands, [this] {
        this->table_demands->setHorizontalHeaderLabels({
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Category")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Base Demand")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Pattern")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Source / Method")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Note")), QString()});
    });
    this->table_demands->verticalHeader()->setVisible(false);
    this->table_demands->setSelectionMode(QAbstractItemView::NoSelection);
    this->table_demands->horizontalHeader()->setStretchLastSection(false);
    this->table_demands->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    this->table_demands->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    this->table_demands->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    this->table_demands->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    this->table_demands->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    this->table_demands->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    this->table_demands->setColumnWidth(0, 150);
    this->table_demands->setColumnWidth(2, 150);
    this->table_demands->setColumnWidth(5, 35);

    QPushButton *button_demand = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Add Demand"), nullptr);
    connect(button_demand, &QPushButton::clicked, this, [this]()
    {
        HydraulicDemand demand;
        addCurrentDemand(demand);
    });

    QPushButton *button_patterns = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Manage Patterns"), nullptr);

    grid->addWidget(this->table_demands, 0, 0, 1, 2);
    grid->addWidget(button_demand, 1, 0);
    grid->addWidget(button_patterns, 1, 1);

    connect(this->dialog_demands, &QObject::destroyed, this, [this]()
    {
        this->dialog_demands = nullptr;
        this->table_demands = nullptr;
    });

    new UnitAwareView(this->dialog_demands, UnitProfileManager::instance().presentation(),
        [this] { refreshDemands(true); });
    refreshDemands();
    this->dialog_demands->show();
}

void EntityInspectorWidget::addGroupNodeQualityInputs()
{
    GroupBoxCollapsible *group_chemical = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Chemical — Node Inputs"));
    QGridLayout *grid_chemical = new QGridLayout(group_chemical);
    int chemical_row = 0;

    QLabel *label_initial_chemical = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Initial Chemical Concentration"), nullptr);
    label_initial_chemical->setWordWrap(true);
    // All node types share these canonical-backed editors. Only accepted user
    // edits invoke the HydraulicData setters; unit changes are read-only.
    const auto quality_editor = [this](NodeQualityNumber field) -> QDoubleSpinBox * {
        const std::string quantity = field == NodeQualityNumber::InitialWaterAge ? AOWIS_QUANTITY("water_age")
            : field == NodeQualityNumber::SourceMassFlow ? AOWIS_QUANTITY("chemical_mass_flow_rate")
            : AOWIS_QUANTITY("chemical_mass_concentration");
        const std::string storage_unit = field == NodeQualityNumber::InitialWaterAge ? "h"
            : field == NodeQualityNumber::SourceMassFlow ? "mg/min" : "mg/L";
        const double maximum = field == NodeQualityNumber::SourceMassFlow ? 1e12 : 1e9;
        return new UnitNumericSpinBox(
            [this, field]() -> std::optional<double> {
                return readNodeQualityNumber(this->hydraulic_data, this->entity_type,
                                             this->entity_uuid, field);
            },
            [this, field](double value) {
                if (!this->hydraulic_data ||
                    !readNodeQualityNumber(this->hydraulic_data, this->entity_type,
                                           this->entity_uuid, field)) return false;
                if (field == NodeQualityNumber::InitialChemical)
                    return this->hydraulic_data->setNodeInitialChemicalConcentrationMgPerL(this->entity_uuid, value);
                if (field == NodeQualityNumber::InitialWaterAge)
                    return this->hydraulic_data->setNodeInitialWaterAgeH(this->entity_uuid, value);
                const std::optional<HydraulicNodeQualitySourceType> source_type =
                    readNodeQualitySourceType(this->hydraulic_data, this->entity_type, this->entity_uuid);
                if (!source_type) return false;
                if (field == NodeQualityNumber::SourceConcentration && usesNodeQualityConcentration(*source_type))
                    return this->hydraulic_data->setNodeQualitySourceChemicalConcentrationMgPerL(this->entity_uuid, value);
                if (field == NodeQualityNumber::SourceMassFlow &&
                    *source_type == HydraulicNodeQualitySourceType::MassBooster)
                    return this->hydraulic_data->setNodeQualitySourceMassFlowMgPerMin(this->entity_uuid, value);
                return false;
            }, quantity, storage_unit, 0.0, maximum, 0.1, 6, this);
    };

    this->spin_quality_initial_chemical = quality_editor(NodeQualityNumber::InitialChemical);
    constrainSpinWidth(this->spin_quality_initial_chemical);

    QLabel *label_source_type = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Source Type"), nullptr);
    this->combo_quality_source_type = new QComboBox();
    EntityInspectorText::addChoice(this->combo_quality_source_type, QT_TRANSLATE_NOOP("LiveUiText", "None"), static_cast<int>(HydraulicNodeQualitySourceType::None));
    EntityInspectorText::addChoice(this->combo_quality_source_type, QT_TRANSLATE_NOOP("LiveUiText", "Concentration"), static_cast<int>(HydraulicNodeQualitySourceType::Concentration));
    EntityInspectorText::addChoice(this->combo_quality_source_type, QT_TRANSLATE_NOOP("LiveUiText", "Mass Booster"), static_cast<int>(HydraulicNodeQualitySourceType::MassBooster));
    EntityInspectorText::addChoice(this->combo_quality_source_type, QT_TRANSLATE_NOOP("LiveUiText", "Flow-Paced Booster"), static_cast<int>(HydraulicNodeQualitySourceType::FlowPacedBooster));
    EntityInspectorText::addChoice(this->combo_quality_source_type, QT_TRANSLATE_NOOP("LiveUiText", "Setpoint Booster"), static_cast<int>(HydraulicNodeQualitySourceType::SetpointBooster));

    QLabel *label_source_concentration = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Source<br>Concentration"), nullptr);
    this->spin_quality_source_concentration = quality_editor(NodeQualityNumber::SourceConcentration);
    constrainSpinWidth(this->spin_quality_source_concentration);

    QLabel *label_source_mass_flow = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Source<br>Mass Flow"), nullptr);
    this->spin_quality_source_mass_flow = quality_editor(NodeQualityNumber::SourceMassFlow);
    constrainSpinWidth(this->spin_quality_source_mass_flow);

    QLabel *label_source_pattern = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Source Pattern"), nullptr);
    this->combo_quality_source_pattern = new QComboBox();
    constrainComboWidth(this->combo_quality_source_pattern);

    grid_chemical->addWidget(label_initial_chemical, chemical_row, 0);
    grid_chemical->addWidget(this->spin_quality_initial_chemical, chemical_row++, 1);
    grid_chemical->addWidget(label_source_type, chemical_row, 0);
    grid_chemical->addWidget(this->combo_quality_source_type, chemical_row++, 1);
    grid_chemical->addWidget(label_source_concentration, chemical_row, 0);
    grid_chemical->addWidget(this->spin_quality_source_concentration, chemical_row++, 1);
    grid_chemical->addWidget(label_source_mass_flow, chemical_row, 0);
    grid_chemical->addWidget(this->spin_quality_source_mass_flow, chemical_row++, 1);
    grid_chemical->addWidget(label_source_pattern, chemical_row, 0);
    grid_chemical->addWidget(this->combo_quality_source_pattern, chemical_row++, 1);

    GroupBoxCollapsible *group_water_age = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Water Age — Node Inputs"));
    QGridLayout *grid_water_age = new QGridLayout(group_water_age);

    QLabel *label_initial_water_age = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Initial Water Age"), nullptr);
    this->spin_quality_initial_water_age = quality_editor(NodeQualityNumber::InitialWaterAge);
    constrainSpinWidth(this->spin_quality_initial_water_age);

    grid_water_age->addWidget(label_initial_water_age, 0, 0);
    grid_water_age->addWidget(this->spin_quality_initial_water_age, 0, 1);

    GroupBoxCollapsible *group_source_trace = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Source Trace — Trace Origin"));
    QGridLayout *grid_source_trace = new QGridLayout(group_source_trace);

    QLabel *label_source_trace_origin = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Current Trace Origin:"), nullptr);
    this->label_source_trace_origin_value = new QLabel();
    this->label_source_trace_origin_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->button_source_trace_origin_inspect = new QPushButton(QIcon(":/icon/target.png"), "");
    this->button_source_trace_origin_inspect->setIconSize(QSize(20, 20));
    LiveUiText::bind(this->button_source_trace_origin_inspect, [this] {
        refreshSourceTraceInputs();
    });
    this->button_source_trace_origin_inspect->setMaximumWidth(35);
    this->check_source_trace_origin_current_node = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Use This Node as Trace Origin"), nullptr);
    LiveUiText::bind(this->check_source_trace_origin_current_node, [this] {
        this->check_source_trace_origin_current_node->setToolTip(inspectorText(QT_TRANSLATE_NOOP(
            "EntityInspectorPresentation",
            "Sets this node as the Source Trace origin. Only one trace origin can be configured at a time.")));
    });

    grid_source_trace->addWidget(label_source_trace_origin, 0, 0);
    grid_source_trace->addWidget(this->label_source_trace_origin_value, 0, 1);
    grid_source_trace->addWidget(this->button_source_trace_origin_inspect, 0, 2);
    grid_source_trace->addWidget(this->check_source_trace_origin_current_node, 1, 0, 1, 3);
    grid_source_trace->setColumnStretch(1, 1);

    connect(this->combo_quality_source_type, &QComboBox::currentIndexChanged,
            this, [this](int)
    {
        const HydraulicNodeQualitySourceType source_type = static_cast<HydraulicNodeQualitySourceType>(
            this->combo_quality_source_type->currentData().toInt());
        static_cast<UnitNumericSpinBox *>(this->spin_quality_source_concentration)->cancelEdit();
        static_cast<UnitNumericSpinBox *>(this->spin_quality_source_mass_flow)->cancelEdit();
        this->hydraulic_data->setNodeQualitySourceType(this->entity_uuid, source_type);
        updateQualitySourceInputUi();
    });
    connect(this->combo_quality_source_pattern, &QComboBox::currentIndexChanged,
            this, [this](int)
    {
        this->hydraulic_data->setNodeQualitySourcePatternUuid(
            this->entity_uuid, this->combo_quality_source_pattern->currentData().toUuid());
    });

    connect(this->button_source_trace_origin_inspect, &QPushButton::clicked, this, [this]()
    {
        inspectHydraulicEndpoint(this->hydraulic_data->sourceTraceOriginNodeUuid());
    });
    connect(this->check_source_trace_origin_current_node, &QCheckBox::toggled, this, [this](bool checked)
    {
        if (checked)
        {
            requestUseCurrentNodeAsSourceTraceOrigin();
            return;
        }

        if (this->hydraulic_data->sourceTraceOriginNodeUuid() == this->entity_uuid)
            this->hydraulic_data->setSourceTraceOriginNodeUuid(QUuid());
    });

    connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this,
            [this](InfrastructureEntity, const QUuid &uuid)
    {
        if (uuid == this->entity_uuid)
            refreshNodeQualityInputs();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, &EntityInspectorWidget::refreshNodeQualityInputs);
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, &EntityInspectorWidget::refreshSourceTraceInputs);
    connect(this->hydraulic_data, &HydraulicData::signalWaterQualityOptionsChanged,
            this, &EntityInspectorWidget::refreshSourceTraceInputs);
    connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this,
            [this](InfrastructureEntity, const QUuid &uuid)
    {
        if (uuid == this->hydraulic_data->sourceTraceOriginNodeUuid())
            refreshSourceTraceInputs();
    });

    layoutQuality()->addWidget(group_chemical);
    layoutQuality()->addWidget(group_water_age);
    UnitPresentationContext &presentation = UnitProfileManager::instance().presentation();
    // A collapsed/hidden quality group does no conversion work until shown.
    new UnitAwareView(this->spin_quality_initial_chemical, presentation, [this] {
        const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
        static_cast<UnitNumericSpinBox *>(this->spin_quality_initial_chemical)->refreshUnits(context);
        static_cast<UnitNumericSpinBox *>(this->spin_quality_source_concentration)->refreshUnits(context);
        static_cast<UnitNumericSpinBox *>(this->spin_quality_source_mass_flow)->refreshUnits(context);
        updateQualitySourceInputUi();
    });
    new UnitAwareView(this->spin_quality_initial_water_age, presentation, [this] {
        static_cast<UnitNumericSpinBox *>(this->spin_quality_initial_water_age)->refreshUnits(
            UnitProfileManager::instance().presentation());
    });
    layoutQuality()->addWidget(group_source_trace);
    refreshNodeQualityInputs();
    refreshSourceTraceInputs();
}

QString EntityInspectorWidget::sourceTraceNodeDisplayName(const QUuid &uuid) const
{
    if (this->hydraulic_data == nullptr || uuid.isNull())
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "None"));

    const std::optional<InfrastructureEntity> entity_type = this->hydraulic_data->nodeEntityType(uuid);
    if (!entity_type.has_value())
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Unknown node (%1)"))
            .arg(uuid.toString(QUuid::WithoutBraces));

    const std::optional<HydraulicNodeCommonData> node =
        this->hydraulic_data->nodeCommonData(entity_type.value(), uuid);
    if (!node.has_value())
        return inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Unknown node (%1)"))
            .arg(uuid.toString(QUuid::WithoutBraces));

    const QString node_identifier = node->id.isEmpty()
        ? uuid.toString(QUuid::WithoutBraces)
        : node->id;
    return QStringLiteral("%1 %2")
        .arg(hydraulicNodeTypeName(entity_type.value()), node_identifier);
}

void EntityInspectorWidget::refreshSourceTraceInputs()
{
    if (this->hydraulic_data == nullptr
        || this->label_source_trace_origin_value == nullptr
        || this->button_source_trace_origin_inspect == nullptr
        || this->check_source_trace_origin_current_node == nullptr)
    {
        return;
    }

    const QUuid trace_origin_uuid = this->hydraulic_data->sourceTraceOriginNodeUuid();
    this->label_source_trace_origin_value->setText(sourceTraceNodeDisplayName(trace_origin_uuid));

    const bool trace_origin_exists = !trace_origin_uuid.isNull()
        && this->hydraulic_data->nodeEntityType(trace_origin_uuid).has_value();
    const bool current_node_is_origin = trace_origin_uuid == this->entity_uuid;

    this->button_source_trace_origin_inspect->setEnabled(
        trace_origin_exists && !current_node_is_origin);
    this->button_source_trace_origin_inspect->setToolTip(
        current_node_is_origin
            ? inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
                "This node is already open in the Inspector"))
            : inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
                "Open Trace Origin in Inspector")));

    const QSignalBlocker blocker(this->check_source_trace_origin_current_node);
    this->check_source_trace_origin_current_node->setChecked(current_node_is_origin);
}

void EntityInspectorWidget::requestUseCurrentNodeAsSourceTraceOrigin()
{
    if (this->hydraulic_data == nullptr || this->entity_uuid.isNull())
        return;

    const QUuid existing_origin_uuid = this->hydraulic_data->sourceTraceOriginNodeUuid();
    if (existing_origin_uuid.isNull() || existing_origin_uuid == this->entity_uuid)
    {
        if (!this->hydraulic_data->setSourceTraceOriginNodeUuid(this->entity_uuid))
            refreshSourceTraceInputs();
        return;
    }

    const QSignalBlocker blocker(this->check_source_trace_origin_current_node);
    this->check_source_trace_origin_current_node->setChecked(false);

    if (this->source_trace_origin_message_box)
        this->source_trace_origin_message_box->close();

    QWidget *parent_window = this->window();
    if (parent_window == nullptr)
        parent_window = this;

    QMessageBox *message_box = new QMessageBox(
        QMessageBox::Question, QString(), QString(), QMessageBox::NoButton, parent_window);
    message_box->setTextFormat(Qt::RichText);
    QPushButton *button_use_node = message_box->addButton(QString(), QMessageBox::AcceptRole);
    QPushButton *button_cancel = message_box->addButton(QString(), QMessageBox::RejectRole);
    message_box->setDefaultButton(button_use_node);
    QPointer<EntityInspectorWidget> guarded_inspector = this;
    LiveUiText::bind(message_box, [guarded_inspector, message_box, existing_origin_uuid] {
        if (!guarded_inspector)
            return;
        message_box->setWindowTitle(inspectorText(QT_TRANSLATE_NOOP(
            "EntityInspectorPresentation", "Change Source Trace Origin?")));
        message_box->setText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
            "Source Trace currently uses <b>%1</b> as its origin.<br><br>Replace it with <b>%2</b>?"))
            .arg(guarded_inspector->sourceTraceNodeDisplayName(existing_origin_uuid).toHtmlEscaped(),
                 guarded_inspector->sourceTraceNodeDisplayName(guarded_inspector->entity_uuid).toHtmlEscaped()));
    });
    LiveUiText::bind(button_use_node, [guarded_inspector, button_use_node] {
        if (!guarded_inspector)
            return;
        button_use_node->setText(inspectorText(QT_TRANSLATE_NOOP(
            "EntityInspectorPresentation", "Use %1"))
            .arg(guarded_inspector->sourceTraceNodeDisplayName(guarded_inspector->entity_uuid)));
    });
    LiveUiText::bind(button_cancel, [button_cancel] {
        button_cancel->setText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "Cancel")));
    });
    message_box->setAttribute(Qt::WA_DeleteOnClose);

#ifdef Q_OS_WASM
    message_box->setWindowModality(Qt::NonModal);
    message_box->adjustSize();
    const QPoint initial_parent_center = parent_window->mapToGlobal(parent_window->rect().center());
    message_box->move(
        initial_parent_center.x() - message_box->width() / 2,
        initial_parent_center.y() - message_box->height() / 2);
#else
    message_box->setWindowModality(Qt::WindowModal);
#endif

    this->source_trace_origin_message_box = message_box;
    connect(message_box, &QMessageBox::finished, this, [this, message_box, button_use_node](int)
    {
        if (message_box->clickedButton() == button_use_node)
        {
            if (!this->hydraulic_data->setSourceTraceOriginNodeUuid(this->entity_uuid))
                refreshSourceTraceInputs();
        }
        else
        {
            refreshSourceTraceInputs();
        }
    });

    message_box->open();

#ifdef Q_OS_WASM
    QPointer<QMessageBox> guarded_message_box = message_box;
    QPointer<QWidget> guarded_parent_window = parent_window;
    QTimer::singleShot(0, message_box, [guarded_message_box, guarded_parent_window]()
    {
        if (!guarded_message_box || !guarded_parent_window)
            return;

        guarded_message_box->adjustSize();
        const QPoint parent_center = guarded_parent_window->mapToGlobal(
            guarded_parent_window->rect().center());
        guarded_message_box->move(
            parent_center.x() - guarded_message_box->width() / 2,
            parent_center.y() - guarded_message_box->height() / 2);
    });
#endif
}

void EntityInspectorWidget::addGroupNoEntitySpecificQualityInputs(const QString &entity_name)
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Water Quality Inputs"));
    QGridLayout *grid = new QGridLayout(group);
    QLabel *label = new QLabel();
    LiveUiText::bind(label, [label, entity_name] {
        QString type = entity_name;
        if (entity_name == QStringLiteral("pipe"))
            type = inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "pipe"));
        else if (entity_name == QStringLiteral("pump"))
            type = inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "pump"));
        else if (entity_name == QStringLiteral("valve"))
            type = inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation", "valve"));
        label->setText(inspectorText(QT_TRANSLATE_NOOP("EntityInspectorPresentation",
            "EPANET defines no %1-specific water-quality input parameters.")).arg(type));
    });
    label->setWordWrap(true);
    grid->addWidget(label, 0, 0);
    layoutQuality()->addWidget(group);
}

void EntityInspectorWidget::populateQualitySourcePatternCombo(const QUuid &pattern_uuid)
{
    if (this->combo_quality_source_pattern == nullptr)
        return;

    const QSignalBlocker blocker(this->combo_quality_source_pattern);
    this->combo_quality_source_pattern->clear();
    EntityInspectorText::addChoice(this->combo_quality_source_pattern, QT_TRANSLATE_NOOP("LiveUiText", "None"), QUuid());
    for (const HydraulicPatternTime &pattern : this->hydraulic_data->networkHydraulic().patterns_time)
    {
        const QString label = pattern.id.isEmpty()
            ? pattern.uuid.toString(QUuid::WithoutBraces)
            : pattern.id;
        this->combo_quality_source_pattern->addItem(label, pattern.uuid);
    }
    const int index = this->combo_quality_source_pattern->findData(pattern_uuid);
    this->combo_quality_source_pattern->setCurrentIndex(index >= 0 ? index : 0);
}

void EntityInspectorWidget::updateQualitySourceInputUi()
{
    if (this->combo_quality_source_type == nullptr)
        return;

    const HydraulicNodeQualitySourceType source_type = static_cast<HydraulicNodeQualitySourceType>(
        this->combo_quality_source_type->currentData().toInt());
    const bool uses_concentration = usesNodeQualityConcentration(source_type);
    const bool uses_mass_flow = source_type == HydraulicNodeQualitySourceType::MassBooster;
    const bool has_source = source_type != HydraulicNodeQualitySourceType::None;

    this->spin_quality_source_concentration->setEnabled(uses_concentration);
    this->spin_quality_source_mass_flow->setEnabled(uses_mass_flow);
    this->combo_quality_source_pattern->setEnabled(has_source);
}

void EntityInspectorWidget::refreshNodeQualityInputs()
{
    if (this->combo_quality_source_type == nullptr)
        return;

    HydraulicNodeQualitySource quality_source;
    bool found = false;

    if (this->entity_type == InfrastructureEntity::Junction)
    {
        const std::optional<HydraulicNodeJunction> node = this->hydraulic_data->junction(this->entity_uuid);
        if (node.has_value())
        {
            quality_source = node->quality_source;
            found = true;
        }
    }
    else if (this->entity_type == InfrastructureEntity::Reservoir)
    {
        const std::optional<HydraulicNodeReservoir> node = this->hydraulic_data->reservoir(this->entity_uuid);
        if (node.has_value())
        {
            quality_source = node->quality_source;
            found = true;
        }
    }
    else if (this->entity_type == InfrastructureEntity::Tank)
    {
        const std::optional<HydraulicNodeTank> node = this->hydraulic_data->tank(this->entity_uuid);
        if (node.has_value())
        {
            quality_source = node->quality_source;
            found = true;
        }
    }

    if (!found)
        return;

    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    static_cast<UnitNumericSpinBox *>(this->spin_quality_initial_chemical)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_quality_initial_water_age)->refreshUnits(context);
    const QSignalBlocker source_type_blocker(this->combo_quality_source_type);
    const int source_index = this->combo_quality_source_type->findData(static_cast<int>(quality_source.type));
    this->combo_quality_source_type->setCurrentIndex(source_index >= 0 ? source_index : 0);
    static_cast<UnitNumericSpinBox *>(this->spin_quality_source_concentration)->refreshUnits(context);
    static_cast<UnitNumericSpinBox *>(this->spin_quality_source_mass_flow)->refreshUnits(context);
    populateQualitySourcePatternCombo(quality_source.pattern_uuid);
    updateQualitySourceInputUi();
}

void EntityInspectorWidget::addGroupAlerts()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Alerts"));
    layoutOverview()->addWidget(group);
}

void EntityInspectorWidget::addGroupHistory()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "History"));
    QGridLayout *grid = new QGridLayout(group);
    
    
    
    layoutOverview()->addWidget(group);
}

void EntityInspectorWidget::addStretches()
{
    layoutOverview()->addStretch();
    layoutConfiguration()->addStretch();
    layoutSimMeas()->addStretch();
    layoutQuality()->addStretch();
    layoutQualitySimMeas()->addStretch();
}
