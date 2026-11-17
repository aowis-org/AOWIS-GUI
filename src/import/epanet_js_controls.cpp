#include "import/epanet_js_controls.h"
#include "import/epanet_js_conversion_common.h"
#include "import/epanet_js_schema.h"
#include "import/epanet_js_units.h"
#include "import/epanet_js_geometry.h"
#include "import/epanet_js_pipes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace EpanetJsControls
{
using namespace EpanetJsConversionCommon;
using namespace EpanetJsSchema;
using namespace EpanetJsUnits;
using namespace EpanetJsGeometry;
using namespace EpanetJsPipes;

bool networkHasPump(const NetworkHydraulicDistribution &network, const QUuid &uuid)
{
    for (const HydraulicLinkPump &pump : network.links_pumps)
    {
        if (pump.uuid == uuid)
            return true;
    }
    return false;
}

bool networkHasTank(const NetworkHydraulicDistribution &network, const QUuid &uuid)
{
    for (const HydraulicNodeTank &tank : network.nodes_tanks)
    {
        if (tank.uuid == uuid)
            return true;
    }
    return false;
}

QString structuredControlSourceId(const QJsonObject &object, qsizetype index)
{
    const QString id = object.value(QStringLiteral("id")).toString().trimmed();
    if (!id.isEmpty())
        return id;
    return QStringLiteral("control-%1").arg(index + 1);
}

bool importStructuredPumpLevelAction(
    const QJsonObject &action_object,
    const QString &source_control_id,
    const QString &suffix,
    HydraulicControlSimpleType control_type,
    HydraulicControlActionType fallback_action,
    const QUuid &pump_uuid,
    const QUuid &tank_uuid,
    const QString &level_unit,
    EpanetJsProjectConversionResult &result)
{
    const std::optional<double> source_level = finiteDouble(
        action_object.value(QStringLiteral("level")));
    const std::optional<double> level_m = source_level.has_value()
        ? lengthToM(*source_level, level_unit)
        : std::nullopt;
    if (!level_m.has_value())
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-level-setting-control-level"),
            QStringLiteral("epanet-js level-setting control '%1' has an invalid %2 level for unit '%3'.")
                .arg(source_control_id, suffix, level_unit),
            QStringLiteral("controls"));
        return false;
    }

    HydraulicControlSimple control;
    control.id = QStringLiteral("%1_%2").arg(source_control_id, suffix.toUpper());
    control.uuid = uuidV5(
        result.network.uuid,
        QStringLiteral("epanet-js/controls/%1/%2")
            .arg(source_control_id, suffix)
            .toUtf8());
    control.type = control_type;
    control.link_uuid = pump_uuid;
    control.trigger_node_uuid = tank_uuid;
    control.trigger_water_level_m = *level_m;
    control.action = fallback_action;

    if (action_object.contains(QStringLiteral("setting"))
        && !action_object.value(QStringLiteral("setting")).isNull())
    {
        const std::optional<double> setting = finiteDouble(
            action_object.value(QStringLiteral("setting")));
        if (!setting.has_value() || *setting < 0.0)
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("invalid-level-setting-control-setting"),
                QStringLiteral("epanet-js level-setting control '%1' has an invalid %2 pump setting.")
                    .arg(source_control_id, suffix),
                QStringLiteral("controls"));
            return false;
        }
        control.action = HydraulicControlActionType::Setting;
        control.setting.pump_speed_ratio = *setting;
    }

    result.network.controls_simple.append(control);
    return true;
}

void importStructuredControls(
    const EpanetJsProjectSnapshot &project,
    const QJsonObject &project_settings,
    EpanetJsProjectConversionResult &result)
{
    const EpanetJsTableSnapshot *table = tableByName(project, QStringLiteral("controls"));
    if (table == nullptr || table->rows.isEmpty())
        return;

    const QString data = table->rows.first().value(QStringLiteral("data")).toString().trimmed();
    if (data.isEmpty())
        return;

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(data.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isArray())
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-controls-json"),
            QStringLiteral("epanet-js controls.data is not a valid JSON array."),
            QStringLiteral("controls"));
        return;
    }

    const QJsonObject units = projectUnitsObject(project_settings);
    const QString level_unit = firstUnit(units, QStringList{QStringLiteral("level")});

    const QJsonArray controls = document.array();
    for (qsizetype index = 0; index < controls.size(); ++index)
    {
        const QJsonValue control_value = controls.at(index);
        if (!control_value.isObject())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("invalid-structured-control"),
                QStringLiteral("epanet-js structured control %1 is not an object.")
                    .arg(index + 1),
                QStringLiteral("controls"));
            continue;
        }

        const QJsonObject control_object = control_value.toObject();
        const QString source_control_id = structuredControlSourceId(control_object, index);
        const QString type = control_object.value(QStringLiteral("type"))
            .toString().trimmed().toLower();
        if (type != QStringLiteral("level-setting"))
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-structured-control-type"),
                QStringLiteral("epanet-js structured control '%1' uses unsupported type '%2'.")
                    .arg(source_control_id, type),
                QStringLiteral("controls"));
            continue;
        }

        const std::optional<qint64> link_id = integerValue(
            control_object.value(QStringLiteral("linkId")).toVariant());
        const std::optional<qint64> tank_id = integerValue(
            control_object.value(QStringLiteral("tankId")).toVariant());
        const QUuid pump_uuid = link_id.has_value()
            ? result.id_map.uuidFor(QStringLiteral("pumps"), *link_id)
            : QUuid();
        const QUuid tank_uuid = tank_id.has_value()
            ? result.id_map.uuidFor(QStringLiteral("tanks"), *tank_id)
            : QUuid();

        if (!link_id.has_value() || pump_uuid.isNull()
            || !networkHasPump(result.network, pump_uuid))
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("missing-level-setting-link-reference"),
                QStringLiteral("epanet-js level-setting control '%1' references a missing or non-pump link.")
                    .arg(source_control_id),
                QStringLiteral("controls"));
            continue;
        }
        if (!tank_id.has_value() || tank_uuid.isNull()
            || !networkHasTank(result.network, tank_uuid))
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("missing-level-setting-tank-reference"),
                QStringLiteral("epanet-js level-setting control '%1' references a missing tank.")
                    .arg(source_control_id),
                QStringLiteral("controls"));
            continue;
        }

        bool imported_action = false;
        const QJsonValue on_value = control_object.value(QStringLiteral("on"));
        if (on_value.isObject())
        {
            imported_action = importStructuredPumpLevelAction(
                on_value.toObject(),
                source_control_id,
                QStringLiteral("on"),
                HydraulicControlSimpleType::LowLevel,
                HydraulicControlActionType::Open,
                pump_uuid,
                tank_uuid,
                level_unit,
                result) || imported_action;
        }

        const QJsonValue off_value = control_object.value(QStringLiteral("off"));
        if (off_value.isObject())
        {
            imported_action = importStructuredPumpLevelAction(
                off_value.toObject(),
                source_control_id,
                QStringLiteral("off"),
                HydraulicControlSimpleType::HighLevel,
                HydraulicControlActionType::Close,
                pump_uuid,
                tank_uuid,
                level_unit,
                result) || imported_action;
        }

        if (!imported_action && !on_value.isObject() && !off_value.isObject())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("empty-level-setting-control"),
                QStringLiteral("epanet-js level-setting control '%1' has neither an on nor off action.")
                    .arg(source_control_id),
                QStringLiteral("controls"));
        }
    }
}

const HydraulicLinkValve *networkValveByUuid(
    const NetworkHydraulicDistribution &network,
    const QUuid &uuid)
{
    for (const HydraulicLinkValve &valve : network.links_valves)
    {
        if (valve.uuid == uuid)
            return &valve;
    }
    return nullptr;
}

bool applyRawSimpleControlAction(
    const QString &action_text,
    const QUuid &link_uuid,
    const QJsonObject &project_settings,
    const QString &source_control_id,
    HydraulicControlSimple &control,
    EpanetJsProjectConversionResult &result)
{
    const QString normalized = action_text.trimmed().toUpper();
    if (normalized == QStringLiteral("OPEN"))
    {
        control.action = HydraulicControlActionType::Open;
        return true;
    }
    if (normalized == QStringLiteral("CLOSED") || normalized == QStringLiteral("CLOSE"))
    {
        control.action = HydraulicControlActionType::Close;
        return true;
    }

    bool number_ok = false;
    const double setting = action_text.toDouble(&number_ok);
    if (!number_ok || !std::isfinite(setting) || setting < 0.0)
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-simple-control-action"),
            QStringLiteral("epanet-js raw simple control '%1' has unsupported action '%2'.")
                .arg(source_control_id, action_text),
            QStringLiteral("raw_controls"));
        return false;
    }

    control.action = HydraulicControlActionType::Setting;
    if (networkHasPump(result.network, link_uuid))
    {
        control.setting.pump_speed_ratio = setting;
        return true;
    }

    const HydraulicLinkValve *valve = networkValveByUuid(result.network, link_uuid);
    if (valve == nullptr)
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("unsupported-raw-simple-control-setting-target"),
            QStringLiteral("epanet-js raw simple control '%1' uses numeric setting %2 on a link type that cannot accept a numeric AOWIS simple-control setting.")
                .arg(source_control_id)
                .arg(setting),
            QStringLiteral("raw_controls"));
        return false;
    }

    const QJsonObject units = projectUnitsObject(project_settings);
    switch (valve->type)
    {
    case HydraulicLinkValveType::PRV:
    case HydraulicLinkValveType::PSV:
    case HydraulicLinkValveType::PBV:
    {
        const QString pressure_unit = firstUnit(
            units, QStringList{QStringLiteral("pressure")});
        const std::optional<double> converted = pressureToHeadM(
            setting, pressure_unit, result.network.options_hydraulic.specific_gravity);
        if (!converted.has_value())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-raw-control-valve-pressure-unit"),
                QStringLiteral("AOWIS cannot convert the pressure setting in epanet-js raw simple control '%1' with unit '%2'.")
                    .arg(source_control_id, pressure_unit),
                QStringLiteral("raw_controls"));
            return false;
        }
        control.setting.valve_pressure_head_m = *converted;
        return true;
    }
    case HydraulicLinkValveType::FCV:
    {
        const QString flow_unit = firstUnit(
            units, QStringList{QStringLiteral("flow")});
        const std::optional<double> converted = flowToM3PerH(setting, flow_unit);
        if (!converted.has_value())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-raw-control-valve-flow-unit"),
                QStringLiteral("AOWIS cannot convert the flow setting in epanet-js raw simple control '%1' with unit '%2'.")
                    .arg(source_control_id, flow_unit),
                QStringLiteral("raw_controls"));
            return false;
        }
        control.setting.valve_flow_m3_per_h = *converted;
        return true;
    }
    case HydraulicLinkValveType::TCV:
        control.setting.valve_loss_coefficient = setting;
        return true;
    case HydraulicLinkValveType::PCV:
        control.setting.valve_position_percent = setting;
        return true;
    case HydraulicLinkValveType::GPV:
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("unsupported-raw-control-gpv-setting"),
            QStringLiteral("epanet-js raw simple control '%1' uses a numeric GPV setting that the AOWIS simple-control model cannot represent faithfully.")
                .arg(source_control_id),
            QStringLiteral("raw_controls"));
        return false;
    }

    return false;
}

std::optional<qint64> rawControlAssetId(
    const QJsonArray &asset_references,
    int placeholder_index,
    bool expected_action_target,
    const QString &source_control_id,
    EpanetJsProjectConversionResult &result)
{
    if (placeholder_index < 0 || placeholder_index >= asset_references.size()
        || !asset_references.at(placeholder_index).isObject())
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-control-asset-reference"),
            QStringLiteral("epanet-js raw simple control '%1' references missing asset placeholder %2.")
                .arg(source_control_id)
                .arg(placeholder_index),
            QStringLiteral("raw_controls"));
        return std::nullopt;
    }

    const QJsonObject reference = asset_references.at(placeholder_index).toObject();
    const std::optional<qint64> asset_id = integerValue(
        reference.value(QStringLiteral("assetId")).toVariant());
    if (!asset_id.has_value() || *asset_id <= 0)
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-control-asset-reference"),
            QStringLiteral("epanet-js raw simple control '%1' has an invalid asset id at placeholder %2.")
                .arg(source_control_id)
                .arg(placeholder_index),
            QStringLiteral("raw_controls"));
        return std::nullopt;
    }

    const bool is_action_target = reference.value(QStringLiteral("isActionTarget")).toBool(false);
    if (is_action_target != expected_action_target)
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-control-asset-role"),
            QStringLiteral("epanet-js raw simple control '%1' has an unexpected asset role at placeholder %2.")
                .arg(source_control_id)
                .arg(placeholder_index),
            QStringLiteral("raw_controls"));
        return std::nullopt;
    }

    return asset_id;
}

bool importRawSimpleLevelControl(
    const QJsonObject &simple_control,
    qsizetype index,
    const QJsonObject &project_settings,
    EpanetJsProjectConversionResult &result)
{
    const QString source_control_id = QStringLiteral("raw-simple-%1").arg(index + 1);
    const QString control_template = simple_control.value(QStringLiteral("template"))
        .toString().trimmed();

    const QString number_pattern = QStringLiteral(
        "[-+]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][-+]?\\d+)?");
    const QRegularExpression expression(
        QStringLiteral(
            "^\\s*LINK\\s+\\{\\{(\\d+)\\}\\}\\s+(OPEN|CLOSED|CLOSE|%1)"
            "\\s+IF\\s+NODE\\s+\\{\\{(\\d+)\\}\\}\\s+(BELOW|ABOVE)\\s+(%1)\\s*$")
            .arg(number_pattern),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(control_template);
    if (!match.hasMatch())
        return false;

    bool link_placeholder_ok = false;
    bool node_placeholder_ok = false;
    const int link_placeholder = match.captured(1).toInt(&link_placeholder_ok);
    const int node_placeholder = match.captured(3).toInt(&node_placeholder_ok);
    if (!link_placeholder_ok || !node_placeholder_ok)
        return false;

    const QJsonArray asset_references = simple_control.value(
        QStringLiteral("assetReferences")).toArray();
    const std::optional<qint64> link_id = rawControlAssetId(
        asset_references, link_placeholder, true, source_control_id, result);
    const std::optional<qint64> node_id = rawControlAssetId(
        asset_references, node_placeholder, false, source_control_id, result);
    if (!link_id.has_value() || !node_id.has_value())
        return true;

    const QUuid link_uuid = result.id_map.linkUuid(*link_id);
    const QUuid node_uuid = result.id_map.nodeUuid(*node_id);
    if (link_uuid.isNull())
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("missing-raw-control-link-reference"),
            QStringLiteral("epanet-js raw simple control '%1' references missing or ambiguous link id %2.")
                .arg(source_control_id)
                .arg(*link_id),
            QStringLiteral("raw_controls"));
        return true;
    }
    if (node_uuid.isNull())
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("missing-raw-control-node-reference"),
            QStringLiteral("epanet-js raw simple control '%1' references missing or ambiguous node id %2.")
                .arg(source_control_id)
                .arg(*node_id),
            QStringLiteral("raw_controls"));
        return true;
    }

    bool threshold_ok = false;
    const double source_threshold = match.captured(5).toDouble(&threshold_ok);
    if (!threshold_ok || !std::isfinite(source_threshold))
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-control-trigger-value"),
            QStringLiteral("epanet-js raw simple control '%1' has an invalid trigger value.")
                .arg(source_control_id),
            QStringLiteral("raw_controls"));
        return true;
    }

    HydraulicControlSimple control;
    control.id = QStringLiteral("RAW_SIMPLE_%1").arg(index + 1);
    control.uuid = uuidV5(
        result.network.uuid,
        QStringLiteral("epanet-js/raw-controls/simple/%1").arg(index).toUtf8());
    control.type = match.captured(4).compare(
        QStringLiteral("BELOW"), Qt::CaseInsensitive) == 0
        ? HydraulicControlSimpleType::LowLevel
        : HydraulicControlSimpleType::HighLevel;
    control.link_uuid = link_uuid;
    control.trigger_node_uuid = node_uuid;

    const QList<QString> node_tables = result.id_map.nodeTablesForId(*node_id);
    if (node_tables.size() != 1)
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("ambiguous-raw-control-node-reference"),
            QStringLiteral("epanet-js raw simple control '%1' cannot determine the node type for id %2.")
                .arg(source_control_id)
                .arg(*node_id),
            QStringLiteral("raw_controls"));
        return true;
    }

    const QJsonObject units = projectUnitsObject(project_settings);
    if (node_tables.first() == QStringLiteral("junctions"))
    {
        const QString pressure_unit = firstUnit(
            units, QStringList{QStringLiteral("pressure")});
        const std::optional<double> threshold = pressureToHeadM(
            source_threshold, pressure_unit, result.network.options_hydraulic.specific_gravity);
        if (!threshold.has_value())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-raw-control-pressure-unit"),
                QStringLiteral("AOWIS cannot convert the junction-pressure trigger in epanet-js raw simple control '%1' with unit '%2'.")
                    .arg(source_control_id, pressure_unit),
                QStringLiteral("raw_controls"));
            return true;
        }
        control.trigger_pressure_head_m = *threshold;
    }
    else
    {
        const QString level_unit = firstUnit(units, QStringList{QStringLiteral("level")});
        const std::optional<double> threshold = lengthToM(source_threshold, level_unit);
        if (!threshold.has_value())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-raw-control-level-unit"),
                QStringLiteral("AOWIS cannot convert the tank/reservoir level trigger in epanet-js raw simple control '%1' with unit '%2'.")
                    .arg(source_control_id, level_unit),
                QStringLiteral("raw_controls"));
            return true;
        }
        control.trigger_water_level_m = *threshold;
    }

    if (!applyRawSimpleControlAction(
            match.captured(2), link_uuid, project_settings, source_control_id, control, result))
    {
        return true;
    }

    result.network.controls_simple.append(control);
    return true;
}

struct RawRuleStatement
{
    QString keyword;
    QString body;
};

QList<RawRuleStatement> rawRuleStatements(const QString &rule_template)
{
    QString without_comments;
    const QStringList lines = rule_template.split(QRegularExpression(QStringLiteral("[\\r\\n]+")));
    for (const QString &line : lines)
    {
        const QString content = line.section(QLatin1Char(';'), 0, 0).trimmed();
        if (!content.isEmpty())
        {
            if (!without_comments.isEmpty())
                without_comments.append(QLatin1Char(' '));
            without_comments.append(content);
        }
    }

    QList<RawRuleStatement> statements;
    const QRegularExpression expression(
        QStringLiteral("\\b(RULE|IF|THEN|ELSE|AND|OR|PRIORITY|DISABLED|ENABLED)\\b(?:\\s+(.+?))?(?=\\b(?:RULE|IF|THEN|ELSE|AND|OR|PRIORITY|DISABLED|ENABLED)\\b|$)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator iterator = expression.globalMatch(without_comments);
    while (iterator.hasNext())
    {
        const QRegularExpressionMatch match = iterator.next();
        RawRuleStatement statement;
        statement.keyword = match.captured(1).trimmed().toUpper();
        statement.body = match.captured(2).trimmed();
        statements.append(statement);
    }
    return statements;
}

std::optional<HydraulicControlRuleOperator> rawRuleComparison(const QString &text)
{
    QString normalized = text.trimmed().toUpper();
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    if (normalized == QStringLiteral("=") || normalized == QStringLiteral("IS"))
        return HydraulicControlRuleOperator::Equal;
    if (normalized == QStringLiteral("<>") || normalized == QStringLiteral("!=")
        || normalized == QStringLiteral("IS NOT"))
        return HydraulicControlRuleOperator::NotEqual;
    if (normalized == QStringLiteral("<="))
        return HydraulicControlRuleOperator::LessOrEqual;
    if (normalized == QStringLiteral(">="))
        return HydraulicControlRuleOperator::GreaterOrEqual;
    if (normalized == QStringLiteral("<") || normalized == QStringLiteral("BELOW"))
        return HydraulicControlRuleOperator::Less;
    if (normalized == QStringLiteral(">") || normalized == QStringLiteral("ABOVE"))
        return HydraulicControlRuleOperator::Greater;
    return std::nullopt;
}

std::optional<HydraulicControlRuleStatus> rawRuleStatus(const QString &text)
{
    const QString normalized = text.trimmed().toUpper();
    if (normalized == QStringLiteral("OPEN"))
        return HydraulicControlRuleStatus::Open;
    if (normalized == QStringLiteral("CLOSED") || normalized == QStringLiteral("CLOSE"))
        return HydraulicControlRuleStatus::Closed;
    if (normalized == QStringLiteral("ACTIVE"))
        return HydraulicControlRuleStatus::Active;
    return std::nullopt;
}

std::optional<quint64> rawRuleTimeSeconds(const QString &text, bool clock_time)
{
    QString normalized = text.trimmed().toUpper();
    bool is_am = false;
    bool is_pm = false;
    if (normalized.endsWith(QStringLiteral(" AM")))
    {
        is_am = true;
        normalized.chop(3);
        normalized = normalized.trimmed();
    }
    else if (normalized.endsWith(QStringLiteral(" PM")))
    {
        is_pm = true;
        normalized.chop(3);
        normalized = normalized.trimmed();
    }

    double seconds = 0.0;
    if (normalized.contains(QLatin1Char(':')))
    {
        const QStringList pieces = normalized.split(QLatin1Char(':'));
        if (pieces.size() < 2 || pieces.size() > 3)
            return std::nullopt;

        bool hours_ok = false;
        bool minutes_ok = false;
        bool seconds_ok = true;
        double hours = pieces.at(0).toDouble(&hours_ok);
        const double minutes = pieces.at(1).toDouble(&minutes_ok);
        const double seconds_component = pieces.size() == 3
            ? pieces.at(2).toDouble(&seconds_ok)
            : 0.0;
        if (!hours_ok || !minutes_ok || !seconds_ok
            || !std::isfinite(hours) || !std::isfinite(minutes)
            || !std::isfinite(seconds_component)
            || hours < 0.0 || minutes < 0.0 || minutes >= 60.0
            || seconds_component < 0.0 || seconds_component >= 60.0)
        {
            return std::nullopt;
        }

        if (is_am || is_pm)
        {
            if (hours < 1.0 || hours > 12.0 || std::floor(hours) != hours)
                return std::nullopt;
            if (is_am && hours == 12.0)
                hours = 0.0;
            else if (is_pm && hours != 12.0)
                hours += 12.0;
        }

        seconds = hours * 3600.0 + minutes * 60.0 + seconds_component;
    }
    else
    {
        bool hours_ok = false;
        double hours = normalized.toDouble(&hours_ok);
        if (!hours_ok || !std::isfinite(hours) || hours < 0.0)
            return std::nullopt;
        if (is_am || is_pm)
        {
            if (hours < 1.0 || hours > 12.0 || std::floor(hours) != hours)
                return std::nullopt;
            if (is_am && hours == 12.0)
                hours = 0.0;
            else if (is_pm && hours != 12.0)
                hours += 12.0;
        }
        seconds = hours * 3600.0;
    }

    if (!std::isfinite(seconds) || seconds < 0.0
        || seconds > static_cast<double>(std::numeric_limits<quint64>::max()))
    {
        return std::nullopt;
    }
    if (clock_time && seconds >= 24.0 * 3600.0)
        return std::nullopt;
    return static_cast<quint64>(std::llround(seconds));
}

std::optional<qint64> rawRuleAssetId(
    const QJsonArray &asset_references,
    int placeholder_index,
    bool expected_action_target,
    const QString &source_rule_id,
    EpanetJsProjectConversionResult &result)
{
    if (placeholder_index < 0 || placeholder_index >= asset_references.size()
        || !asset_references.at(placeholder_index).isObject())
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-rule-asset-reference"),
            QStringLiteral("epanet-js raw rule '%1' references missing asset placeholder %2.")
                .arg(source_rule_id)
                .arg(placeholder_index),
            QStringLiteral("raw_controls"));
        return std::nullopt;
    }

    const QJsonObject reference = asset_references.at(placeholder_index).toObject();
    const std::optional<qint64> asset_id = integerValue(
        reference.value(QStringLiteral("assetId")).toVariant());
    if (!asset_id.has_value() || *asset_id <= 0)
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-rule-asset-reference"),
            QStringLiteral("epanet-js raw rule '%1' has an invalid asset id at placeholder %2.")
                .arg(source_rule_id)
                .arg(placeholder_index),
            QStringLiteral("raw_controls"));
        return std::nullopt;
    }

    const bool is_action_target = reference.value(QStringLiteral("isActionTarget")).toBool(false);
    if (is_action_target != expected_action_target)
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-rule-asset-role"),
            QStringLiteral("epanet-js raw rule '%1' has an unexpected asset role at placeholder %2.")
                .arg(source_rule_id)
                .arg(placeholder_index),
            QStringLiteral("raw_controls"));
        return std::nullopt;
    }
    return asset_id;
}

bool rawRuleObjectMatchesTable(
    const QString &object_text,
    const QString &table_name)
{
    const QString object = object_text.trimmed().toUpper();
    if (object == QStringLiteral("NODE"))
        return table_name == QStringLiteral("junctions")
            || table_name == QStringLiteral("reservoirs")
            || table_name == QStringLiteral("tanks");
    if (object == QStringLiteral("JUNCTION"))
        return table_name == QStringLiteral("junctions");
    if (object == QStringLiteral("RESERVOIR"))
        return table_name == QStringLiteral("reservoirs");
    if (object == QStringLiteral("TANK"))
        return table_name == QStringLiteral("tanks");
    if (object == QStringLiteral("LINK"))
        return table_name == QStringLiteral("pipes")
            || table_name == QStringLiteral("pumps")
            || table_name == QStringLiteral("valves");
    if (object == QStringLiteral("PIPE"))
        return table_name == QStringLiteral("pipes");
    if (object == QStringLiteral("PUMP"))
        return table_name == QStringLiteral("pumps");
    if (object == QStringLiteral("VALVE"))
        return table_name == QStringLiteral("valves");
    return false;
}

std::optional<QUuid> rawRuleResolveObjectUuid(
    const QString &object_text,
    int placeholder_index,
    bool action_target,
    const QJsonArray &asset_references,
    const QString &source_rule_id,
    EpanetJsProjectConversionResult &result)
{
    const std::optional<qint64> asset_id = rawRuleAssetId(
        asset_references, placeholder_index, action_target, source_rule_id, result);
    if (!asset_id.has_value())
        return std::nullopt;

    const QString object = object_text.trimmed().toUpper();
    const QList<QString> tables = object == QStringLiteral("NODE")
        || object == QStringLiteral("JUNCTION")
        || object == QStringLiteral("RESERVOIR")
        || object == QStringLiteral("TANK")
        ? result.id_map.nodeTablesForId(*asset_id)
        : result.id_map.linkTablesForId(*asset_id);

    QString matching_table;
    for (const QString &table_name : tables)
    {
        if (rawRuleObjectMatchesTable(object, table_name))
        {
            if (!matching_table.isEmpty())
            {
                appendDiagnostic(
                    result,
                    EpanetJsConversionDiagnosticSeverity::Error,
                    QStringLiteral("ambiguous-raw-rule-asset-reference"),
                    QStringLiteral("epanet-js raw rule '%1' cannot unambiguously resolve %2 id %3.")
                        .arg(source_rule_id, object_text)
                        .arg(*asset_id),
                    QStringLiteral("raw_controls"));
                return std::nullopt;
            }
            matching_table = table_name;
        }
    }

    if (matching_table.isEmpty())
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("missing-raw-rule-asset-reference"),
            QStringLiteral("epanet-js raw rule '%1' references missing %2 id %3.")
                .arg(source_rule_id, object_text)
                .arg(*asset_id),
            QStringLiteral("raw_controls"));
        return std::nullopt;
    }

    return result.id_map.uuidFor(matching_table, *asset_id);
}

bool assignRawRuleLinkSetting(
    const QUuid &link_uuid,
    double source_setting,
    const QJsonObject &project_settings,
    const QString &source_rule_id,
    HydraulicControlLinkSetting &setting,
    EpanetJsProjectConversionResult &result)
{
    if (!std::isfinite(source_setting) || source_setting < 0.0)
        return false;

    if (networkHasPump(result.network, link_uuid))
    {
        setting.pump_speed_ratio = source_setting;
        return true;
    }

    const HydraulicLinkValve *valve = networkValveByUuid(result.network, link_uuid);
    if (valve == nullptr)
        return false;

    const QJsonObject units = projectUnitsObject(project_settings);
    switch (valve->type)
    {
    case HydraulicLinkValveType::PRV:
    case HydraulicLinkValveType::PSV:
    case HydraulicLinkValveType::PBV:
    {
        const QString pressure_unit = firstUnit(units, QStringList{QStringLiteral("pressure")});
        const std::optional<double> converted = pressureToHeadM(
            source_setting, pressure_unit, result.network.options_hydraulic.specific_gravity);
        if (!converted.has_value())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-raw-rule-valve-pressure-unit"),
                QStringLiteral("AOWIS cannot convert the valve pressure setting in epanet-js raw rule '%1' with unit '%2'.")
                    .arg(source_rule_id, pressure_unit),
                QStringLiteral("raw_controls"));
            return false;
        }
        setting.valve_pressure_head_m = *converted;
        return true;
    }
    case HydraulicLinkValveType::FCV:
    {
        const QString flow_unit = firstUnit(units, QStringList{QStringLiteral("flow")});
        const std::optional<double> converted = flowToM3PerH(source_setting, flow_unit);
        if (!converted.has_value())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-raw-rule-valve-flow-unit"),
                QStringLiteral("AOWIS cannot convert the valve flow setting in epanet-js raw rule '%1' with unit '%2'.")
                    .arg(source_rule_id, flow_unit),
                QStringLiteral("raw_controls"));
            return false;
        }
        setting.valve_flow_m3_per_h = *converted;
        return true;
    }
    case HydraulicLinkValveType::TCV:
        setting.valve_loss_coefficient = source_setting;
        return true;
    case HydraulicLinkValveType::PCV:
        setting.valve_position_percent = source_setting;
        return true;
    case HydraulicLinkValveType::GPV:
        return false;
    }
    return false;
}

bool assignRawRulePremiseValue(
    HydraulicControlRulePremise &premise,
    const QString &value_text,
    const QJsonObject &project_settings,
    const QString &source_rule_id,
    EpanetJsProjectConversionResult &result)
{
    if (premise.variable == HydraulicControlRuleVariable::Status)
    {
        const std::optional<HydraulicControlRuleStatus> status = rawRuleStatus(value_text);
        if (!status.has_value())
            return false;
        premise.status = *status;
        return true;
    }

    if (premise.variable == HydraulicControlRuleVariable::Time
        || premise.variable == HydraulicControlRuleVariable::ClockTime
        || premise.variable == HydraulicControlRuleVariable::FillTime
        || premise.variable == HydraulicControlRuleVariable::DrainTime)
    {
        const bool clock_time = premise.variable == HydraulicControlRuleVariable::ClockTime;
        const std::optional<quint64> seconds = rawRuleTimeSeconds(value_text, clock_time);
        if (!seconds.has_value())
            return false;
        if (premise.variable == HydraulicControlRuleVariable::Time)
            premise.elapsed_time_s = *seconds;
        else if (premise.variable == HydraulicControlRuleVariable::ClockTime)
            premise.time_of_day_s = *seconds;
        else if (premise.variable == HydraulicControlRuleVariable::FillTime)
            premise.fill_time_s = *seconds;
        else
            premise.drain_time_s = *seconds;
        return true;
    }

    bool value_ok = false;
    const double source_value = value_text.toDouble(&value_ok);
    if (!value_ok || !std::isfinite(source_value))
        return false;

    const QJsonObject units = projectUnitsObject(project_settings);
    if (premise.variable == HydraulicControlRuleVariable::Demand
        || premise.variable == HydraulicControlRuleVariable::Flow)
    {
        const QString flow_unit = firstUnit(units, QStringList{QStringLiteral("flow")});
        const std::optional<double> converted = flowToM3PerH(source_value, flow_unit);
        if (!converted.has_value())
            return false;
        if (premise.variable == HydraulicControlRuleVariable::Demand)
            premise.demand_m3_per_h = *converted;
        else
            premise.flow_m3_per_h = *converted;
        return true;
    }

    if (premise.variable == HydraulicControlRuleVariable::Head
        || premise.variable == HydraulicControlRuleVariable::Grade
        || premise.variable == HydraulicControlRuleVariable::Level)
    {
        const QString length_unit = premise.variable == HydraulicControlRuleVariable::Level
            ? firstUnit(units, QStringList{QStringLiteral("level")})
            : firstUnit(units, QStringList{QStringLiteral("head")});
        const std::optional<double> converted = lengthToM(source_value, length_unit);
        if (!converted.has_value())
            return false;
        if (premise.variable == HydraulicControlRuleVariable::Level)
            premise.water_level_m = *converted;
        else
            premise.hydraulic_head_m = *converted;
        return true;
    }

    if (premise.variable == HydraulicControlRuleVariable::Pressure)
    {
        const QString pressure_unit = firstUnit(units, QStringList{QStringLiteral("pressure")});
        const std::optional<double> converted = pressureToHeadM(
            source_value, pressure_unit, result.network.options_hydraulic.specific_gravity);
        if (!converted.has_value())
            return false;
        premise.pressure_head_m = *converted;
        return true;
    }

    if (premise.variable == HydraulicControlRuleVariable::Setting)
    {
        return assignRawRuleLinkSetting(
            premise.object_uuid, source_value, project_settings, source_rule_id,
            premise.link_setting, result);
    }

    if (premise.variable == HydraulicControlRuleVariable::Power)
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("unsupported-raw-rule-power-premise"),
            QStringLiteral("epanet-js raw rule '%1' uses a POWER premise that the current AOWIS EPANET rule builder cannot execute.")
                .arg(source_rule_id),
            QStringLiteral("raw_controls"));
        return false;
    }
    return false;
}

std::optional<HydraulicControlRuleVariable> rawRuleVariable(const QString &text)
{
    const QString normalized = text.trimmed().toUpper();
    if (normalized == QStringLiteral("DEMAND")) return HydraulicControlRuleVariable::Demand;
    if (normalized == QStringLiteral("HEAD")) return HydraulicControlRuleVariable::Head;
    if (normalized == QStringLiteral("GRADE")) return HydraulicControlRuleVariable::Grade;
    if (normalized == QStringLiteral("LEVEL")) return HydraulicControlRuleVariable::Level;
    if (normalized == QStringLiteral("PRESSURE")) return HydraulicControlRuleVariable::Pressure;
    if (normalized == QStringLiteral("FLOW")) return HydraulicControlRuleVariable::Flow;
    if (normalized == QStringLiteral("STATUS")) return HydraulicControlRuleVariable::Status;
    if (normalized == QStringLiteral("SETTING")) return HydraulicControlRuleVariable::Setting;
    if (normalized == QStringLiteral("POWER")) return HydraulicControlRuleVariable::Power;
    if (normalized == QStringLiteral("TIME")) return HydraulicControlRuleVariable::Time;
    if (normalized == QStringLiteral("CLOCKTIME")) return HydraulicControlRuleVariable::ClockTime;
    if (normalized == QStringLiteral("FILLTIME")) return HydraulicControlRuleVariable::FillTime;
    if (normalized == QStringLiteral("DRAINTIME")) return HydraulicControlRuleVariable::DrainTime;
    return std::nullopt;
}

bool rawRuleVariableAllowedForObject(
    HydraulicControlRuleObject object,
    HydraulicControlRuleVariable variable,
    const QUuid &object_uuid,
    const NetworkHydraulicDistribution &network)
{
    if (object == HydraulicControlRuleObject::System)
    {
        return variable == HydraulicControlRuleVariable::Demand
            || variable == HydraulicControlRuleVariable::Time
            || variable == HydraulicControlRuleVariable::ClockTime;
    }
    if (object == HydraulicControlRuleObject::Link)
    {
        return variable == HydraulicControlRuleVariable::Flow
            || variable == HydraulicControlRuleVariable::Status
            || variable == HydraulicControlRuleVariable::Setting
            || variable == HydraulicControlRuleVariable::Power;
    }

    if (variable == HydraulicControlRuleVariable::FillTime
        || variable == HydraulicControlRuleVariable::DrainTime)
    {
        return networkHasTank(network, object_uuid);
    }
    return variable == HydraulicControlRuleVariable::Demand
        || variable == HydraulicControlRuleVariable::Head
        || variable == HydraulicControlRuleVariable::Grade
        || variable == HydraulicControlRuleVariable::Level
        || variable == HydraulicControlRuleVariable::Pressure;
}

bool importRawRulePremise(
    const RawRuleStatement &statement,
    const QJsonArray &asset_references,
    const QJsonObject &project_settings,
    const QString &source_rule_id,
    HydraulicControlRule &rule,
    EpanetJsProjectConversionResult &result)
{
    HydraulicControlRulePremise premise;
    if (statement.keyword == QStringLiteral("IF"))
        premise.logical_operator = HydraulicControlRuleLogicalOperator::If;
    else if (statement.keyword == QStringLiteral("AND"))
        premise.logical_operator = HydraulicControlRuleLogicalOperator::And;
    else if (statement.keyword == QStringLiteral("OR"))
        premise.logical_operator = HydraulicControlRuleLogicalOperator::Or;
    else
        return false;

    const QString comparison_pattern = QStringLiteral("(IS\\s+NOT|<=|>=|<>|!=|=|<|>|IS|BELOW|ABOVE)");
    const QRegularExpression system_expression(
        QStringLiteral("^SYSTEM\\s+(DEMAND|TIME|CLOCKTIME)\\s+%1\\s+(.+?)\\s*$")
            .arg(comparison_pattern),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch system_match = system_expression.match(statement.body);
    if (system_match.hasMatch())
    {
        const std::optional<HydraulicControlRuleVariable> variable = rawRuleVariable(system_match.captured(1));
        const std::optional<HydraulicControlRuleOperator> comparison = rawRuleComparison(system_match.captured(2));
        if (!variable.has_value() || !comparison.has_value())
            return false;
        premise.object = HydraulicControlRuleObject::System;
        premise.variable = *variable;
        premise.comparison = *comparison;
        if (!rawRuleVariableAllowedForObject(
                premise.object, premise.variable, premise.object_uuid, result.network))
        {
            return false;
        }
        if (!assignRawRulePremiseValue(
                premise, system_match.captured(3), project_settings,
                source_rule_id, result))
        {
            return false;
        }
        rule.premises.append(premise);
        return true;
    }

    const QRegularExpression object_expression(
        QStringLiteral("^(NODE|JUNCTION|RESERVOIR|TANK|LINK|PIPE|PUMP|VALVE)\\s+\\{\\{(\\d+)\\}\\}\\s+"
                       "(DEMAND|HEAD|GRADE|LEVEL|PRESSURE|FLOW|STATUS|SETTING|POWER|FILLTIME|DRAINTIME)\\s+%1\\s+(.+?)\\s*$")
            .arg(comparison_pattern),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch object_match = object_expression.match(statement.body);
    if (!object_match.hasMatch())
        return false;

    bool placeholder_ok = false;
    const int placeholder_index = object_match.captured(2).toInt(&placeholder_ok);
    if (!placeholder_ok)
        return false;
    const QString object_text = object_match.captured(1);
    const std::optional<QUuid> object_uuid = rawRuleResolveObjectUuid(
        object_text, placeholder_index, false, asset_references,
        source_rule_id, result);
    if (!object_uuid.has_value())
        return false;

    const QString normalized_object = object_text.trimmed().toUpper();
    premise.object = normalized_object == QStringLiteral("NODE")
            || normalized_object == QStringLiteral("JUNCTION")
            || normalized_object == QStringLiteral("RESERVOIR")
            || normalized_object == QStringLiteral("TANK")
        ? HydraulicControlRuleObject::Node
        : HydraulicControlRuleObject::Link;
    premise.object_uuid = *object_uuid;

    const std::optional<HydraulicControlRuleVariable> variable = rawRuleVariable(object_match.captured(3));
    const std::optional<HydraulicControlRuleOperator> comparison = rawRuleComparison(object_match.captured(4));
    if (!variable.has_value() || !comparison.has_value())
        return false;
    premise.variable = *variable;
    premise.comparison = *comparison;
    if (!rawRuleVariableAllowedForObject(
            premise.object, premise.variable, premise.object_uuid, result.network))
    {
        return false;
    }

    if (!assignRawRulePremiseValue(
            premise, object_match.captured(5), project_settings,
            source_rule_id, result))
    {
        return false;
    }

    rule.premises.append(premise);
    return true;
}

bool importRawRuleAction(
    const RawRuleStatement &statement,
    const QJsonArray &asset_references,
    const QJsonObject &project_settings,
    const QString &source_rule_id,
    QList<HydraulicControlRuleAction> &actions,
    EpanetJsProjectConversionResult &result)
{
    const QRegularExpression expression(
        QStringLiteral("^(LINK|PIPE|PUMP|VALVE)\\s+\\{\\{(\\d+)\\}\\}\\s+"
                       "(?:(STATUS|SETTING)\\s*=\\s*)?(OPEN|CLOSED|CLOSE|ACTIVE|[-+]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][-+]?\\d+)?)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(statement.body);
    if (!match.hasMatch())
        return false;

    bool placeholder_ok = false;
    const int placeholder_index = match.captured(2).toInt(&placeholder_ok);
    if (!placeholder_ok)
        return false;
    const std::optional<QUuid> link_uuid = rawRuleResolveObjectUuid(
        match.captured(1), placeholder_index, true, asset_references,
        source_rule_id, result);
    if (!link_uuid.has_value())
        return false;

    HydraulicControlRuleAction action;
    action.link_uuid = *link_uuid;
    const QString field = match.captured(3).trimmed().toUpper();
    const QString value = match.captured(4).trimmed();
    const std::optional<HydraulicControlRuleStatus> status = rawRuleStatus(value);
    if (field == QStringLiteral("STATUS") || (field.isEmpty() && status.has_value()))
    {
        if (!status.has_value())
            return false;
        action.status = *status;
        actions.append(action);
        return true;
    }

    bool setting_ok = false;
    const double setting_value = value.toDouble(&setting_ok);
    if (!setting_ok || !std::isfinite(setting_value)
        || !assignRawRuleLinkSetting(
            *link_uuid, setting_value, project_settings, source_rule_id,
            action.setting, result))
    {
        return false;
    }
    actions.append(action);
    return true;
}

bool importRawRule(
    const QJsonObject &raw_rule,
    qsizetype index,
    const QJsonObject &project_settings,
    EpanetJsProjectConversionResult &result)
{
    const QString fallback_id = QStringLiteral("RAW_RULE_%1").arg(index + 1);
    const QString rule_template = raw_rule.value(QStringLiteral("template")).toString().trimmed();
    const QList<RawRuleStatement> statements = rawRuleStatements(rule_template);
    if (statements.isEmpty())
        return false;

    QString rule_id;
    for (const RawRuleStatement &statement : statements)
    {
        if (statement.keyword == QStringLiteral("RULE"))
        {
            if (!rule_id.isEmpty())
                return false;
            rule_id = statement.body.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0).trimmed();
        }
    }
    if (rule_id.isEmpty())
        rule_id = fallback_id;

    HydraulicControlRule rule;
    rule.id = rule_id;
    rule.uuid = uuidV5(
        result.network.uuid,
        QStringLiteral("epanet-js/raw-controls/rules/%1").arg(index).toUtf8());
    if (raw_rule.contains(QStringLiteral("enabled")))
        rule.enabled = raw_rule.value(QStringLiteral("enabled")).toBool(true);

    const QJsonArray asset_references = raw_rule.value(QStringLiteral("assetReferences")).toArray();
    enum class RuleSection
    {
        Premises,
        ThenActions,
        ElseActions
    };
    RuleSection section = RuleSection::Premises;
    bool saw_then = false;

    for (const RawRuleStatement &statement : statements)
    {
        if (statement.keyword == QStringLiteral("RULE"))
            continue;
        if (statement.keyword == QStringLiteral("DISABLED"))
        {
            rule.enabled = false;
            continue;
        }
        if (statement.keyword == QStringLiteral("ENABLED"))
        {
            rule.enabled = true;
            continue;
        }
        if (statement.keyword == QStringLiteral("PRIORITY"))
        {
            bool priority_ok = false;
            const double priority = statement.body.toDouble(&priority_ok);
            if (!priority_ok || !std::isfinite(priority))
                return false;
            rule.priority = priority;
            continue;
        }
        if (statement.keyword == QStringLiteral("THEN"))
        {
            section = RuleSection::ThenActions;
            saw_then = true;
            if (!importRawRuleAction(
                    statement, asset_references, project_settings,
                    rule_id, rule.actions_then, result))
            {
                return false;
            }
            continue;
        }
        if (statement.keyword == QStringLiteral("ELSE"))
        {
            if (!saw_then)
                return false;
            section = RuleSection::ElseActions;
            if (!importRawRuleAction(
                    statement, asset_references, project_settings,
                    rule_id, rule.actions_else, result))
            {
                return false;
            }
            continue;
        }
        if (statement.keyword == QStringLiteral("AND") && section != RuleSection::Premises)
        {
            QList<HydraulicControlRuleAction> &actions = section == RuleSection::ThenActions
                ? rule.actions_then
                : rule.actions_else;
            if (!importRawRuleAction(
                    statement, asset_references, project_settings,
                    rule_id, actions, result))
            {
                return false;
            }
            continue;
        }
        if (section != RuleSection::Premises
            || !importRawRulePremise(
                statement, asset_references, project_settings,
                rule_id, rule, result))
        {
            return false;
        }
    }

    if (rule.premises.isEmpty() || rule.actions_then.isEmpty())
        return false;
    if (rule.premises.first().logical_operator != HydraulicControlRuleLogicalOperator::If)
        return false;

    result.network.controls_rules.append(rule);
    return true;
}

void importRawControls(
    const EpanetJsProjectSnapshot &project,
    const QJsonObject &project_settings,
    EpanetJsProjectConversionResult &result)
{
    const EpanetJsTableSnapshot *table = tableByName(project, QStringLiteral("raw_controls"));
    if (table == nullptr || table->rows.isEmpty())
        return;

    const QString data = table->rows.first().value(QStringLiteral("data")).toString().trimmed();
    if (data.isEmpty())
        return;

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(data.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        appendDiagnostic(
            result,
            EpanetJsConversionDiagnosticSeverity::Error,
            QStringLiteral("invalid-raw-controls-json"),
            QStringLiteral("epanet-js raw_controls.data is not a valid JSON object."),
            QStringLiteral("raw_controls"));
        return;
    }

    const QJsonObject object = document.object();
    const QJsonArray simple_controls = object.value(QStringLiteral("simple")).toArray();
    for (qsizetype index = 0; index < simple_controls.size(); ++index)
    {
        if (!simple_controls.at(index).isObject())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("invalid-raw-simple-control"),
                QStringLiteral("epanet-js raw simple control %1 is not an object.")
                    .arg(index + 1),
                QStringLiteral("raw_controls"));
            continue;
        }

        const QJsonObject simple_control = simple_controls.at(index).toObject();
        if (!importRawSimpleLevelControl(
                simple_control, index, project_settings, result))
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-raw-simple-control"),
                QStringLiteral("epanet-js raw simple control %1 uses a template that AOWIS does not translate yet: %2")
                    .arg(index + 1)
                    .arg(simple_control.value(QStringLiteral("template")).toString()),
                QStringLiteral("raw_controls"));
        }
    }

    const QJsonArray rules = object.value(QStringLiteral("rules")).toArray();
    for (qsizetype index = 0; index < rules.size(); ++index)
    {
        if (!rules.at(index).isObject())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("invalid-raw-rule"),
                QStringLiteral("epanet-js raw rule %1 is not an object.").arg(index + 1),
                QStringLiteral("raw_controls"));
            continue;
        }

        const QJsonObject raw_rule = rules.at(index).toObject();
        if (!importRawRule(raw_rule, index, project_settings, result))
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unsupported-raw-rule"),
                QStringLiteral("epanet-js raw rule %1 uses syntax or semantics that AOWIS cannot translate faithfully: %2")
                    .arg(index + 1)
                    .arg(raw_rule.value(QStringLiteral("template")).toString()),
                QStringLiteral("raw_controls"));
        }
    }
}

void importControls(
    const EpanetJsProjectSnapshot &project,
    const QJsonObject &project_settings,
    EpanetJsProjectConversionResult &result)
{
    importStructuredControls(project, project_settings, result);
    importRawControls(project, project_settings, result);
}

}
