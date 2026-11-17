#include "import/epanet_js_project_converter.h"
#include "import/epanet_js_project_reader.h"
#include "config/unit_presentation_context.h"
#include "test_harness.h"

#include <aowis/epanet/epanet_runner.h>
#include <aowis/epanet/utility/hydraulic_simulation_status_printer.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace
{
AowisTestHarness test_harness;

void expectTrue(bool condition, const char *message)
{
    test_harness.expectTrue(condition, message);
}

void expectNear(double actual, double expected, double tolerance, const char *message)
{
    test_harness.expectNear(actual, expected, tolerance, message);
}

QString fixturePath(const QString &file_name)
{
    return QDir(QStringLiteral(AOWIS_EPANET_JS_FIXTURE_DIR)).filePath(file_name);
}

void printDiagnostics(const EpanetJsProjectConversionResult &result)
{
    for (const EpanetJsConversionDiagnostic &diagnostic : result.diagnostics)
    {
        std::fprintf(
            stderr,
            "epanet-js conversion diagnostic [%s] %s: %s\n",
            diagnostic.severity == EpanetJsConversionDiagnosticSeverity::Error
                ? "error"
                : "warning",
            diagnostic.code.toUtf8().constData(),
            diagnostic.message.toUtf8().constData());
    }
}

EpanetJsProjectConversionResult readAndConvertFixture(const QString &file_name)
{
    const EpanetJsProjectReadResult read_result = EpanetJsProjectReader::readFile(
        fixturePath(file_name));
    expectTrue(read_result.success, "synthetic epanet-js fixture can be read as SQLite project");
    expectTrue(read_result.project.user_version == 24,
               "synthetic epanet-js fixture uses the version-24 schema contract");

    if (!read_result.success)
    {
        std::fprintf(stderr, "epanet-js reader error: %s\n", read_result.error.toUtf8().constData());
        return {};
    }

    const EpanetJsProjectConversionResult conversion = EpanetJsProjectConverter::convert(
        read_result.project);
    if (!conversion.success)
        printDiagnostics(conversion);
    expectTrue(conversion.success,
               "synthetic epanet-js fixture converts without blocking diagnostics");
    return conversion;
}

EpanetResultImport importPairedInp(const QString &file_name)
{
    const EpanetResultImport result = EpanetRunner().importInp(fixturePath(file_name));
    expectTrue(result.status.success, "paired synthetic INP imports through the EPANET adapter");
    return result;
}


void expectTrueMessage(bool condition, const QString &message)
{
    const QByteArray bytes = message.toUtf8();
    expectTrue(condition, bytes.constData());
}

void expectNearMessage(double actual, double expected, double tolerance, const QString &message)
{
    const QByteArray bytes = message.toUtf8();
    expectNear(actual, expected, tolerance, bytes.constData());
}

void printRunFailureStatus(const char *label, const EpanetResultRun &run)
{
    if (run.status.success && run.result_timeline.status.success)
        return;

    const QByteArray run_status = HydraulicSimulationStatusPrinter::toString(run.status).toUtf8();
    const QByteArray timeline_status =
        HydraulicSimulationStatusPrinter::toString(run.result_timeline.status).toUtf8();
    std::fprintf(
        stderr,
        "\n=== %s: top-level run status ===\n%s"
        "=== %s: hydraulic timeline status ===\n%s",
        label,
        run_status.constData(),
        label,
        timeline_status.constData());
}

EpanetResultRun runConvertedNetwork(
    const NetworkHydraulicDistribution &network,
    const QList<WaterQualitySolverOptions> &quality_runs)
{
    EpanetRunRequest request;
    request.network = network;
    request.quality_runs = quality_runs;
    const EpanetResultRun run = EpanetRunner().run(request);
    printRunFailureStatus("converted synthetic epanet-js network", run);
    expectTrue(run.status.success, "converted synthetic epanet-js network runs through EPANET");
    expectTrue(run.result_timeline.status.success,
               "converted synthetic epanet-js network produces a successful hydraulic timeline");
    return run;
}

EpanetResultRun runImportedInp(const EpanetResultImport &import_result)
{
    const EpanetResultRun run = EpanetRunner().run(import_result.request);
    printRunFailureStatus("paired synthetic INP network", run);
    expectTrue(run.status.success, "paired synthetic INP network runs through EPANET");
    expectTrue(run.result_timeline.status.success,
               "paired synthetic INP produces a successful hydraulic timeline");
    return run;
}

const HydraulicSimulationResultNodeJunction *junctionResultById(
    const HydraulicSimulationResult &result,
    const QString &id)
{
    for (const HydraulicSimulationResultNodeJunction &junction : result.nodes_junctions)
    {
        if (junction.id == id)
            return &junction;
    }
    return nullptr;
}

const HydraulicSimulationResultNodeReservoir *reservoirResultById(
    const HydraulicSimulationResult &result,
    const QString &id)
{
    for (const HydraulicSimulationResultNodeReservoir &reservoir : result.nodes_reservoirs)
    {
        if (reservoir.id == id)
            return &reservoir;
    }
    return nullptr;
}

const HydraulicSimulationResultNodeTank *tankResultById(
    const HydraulicSimulationResult &result,
    const QString &id)
{
    for (const HydraulicSimulationResultNodeTank &tank : result.nodes_tanks)
    {
        if (tank.id == id)
            return &tank;
    }
    return nullptr;
}

const HydraulicSimulationResultLinkPipe *pipeResultById(
    const HydraulicSimulationResult &result,
    const QString &id)
{
    for (const HydraulicSimulationResultLinkPipe &pipe : result.links_pipes)
    {
        if (pipe.id == id)
            return &pipe;
    }
    return nullptr;
}

const HydraulicSimulationResultLinkPump *pumpResultById(
    const HydraulicSimulationResult &result,
    const QString &id)
{
    for (const HydraulicSimulationResultLinkPump &pump : result.links_pumps)
    {
        if (pump.id == id)
            return &pump;
    }
    return nullptr;
}

const HydraulicSimulationResultLinkValve *valveResultById(
    const HydraulicSimulationResult &result,
    const QString &id)
{
    for (const HydraulicSimulationResultLinkValve &valve : result.links_valves)
    {
        if (valve.id == id)
            return &valve;
    }
    return nullptr;
}

void expectOptionalNear(
    const std::optional<double> &actual,
    const std::optional<double> &expected,
    double tolerance,
    const QString &message)
{
    expectTrueMessage(actual.has_value() == expected.has_value(), message + QStringLiteral(" presence"));
    if (!actual.has_value() || !expected.has_value())
        return;
    expectNearMessage(actual.value(), expected.value(), tolerance, message);
}

void compareHydraulicStep(
    const HydraulicSimulationResult &ejsdb_step,
    const HydraulicSimulationResult &inp_step)
{
    constexpr double flow_tolerance = 1.0e-7;
    constexpr double head_tolerance = 1.0e-8;
    constexpr double velocity_tolerance = 1.0e-9;
    constexpr double setting_tolerance = 1.0e-9;
    constexpr double power_tolerance = 1.0e-8;
    constexpr double percent_tolerance = 1.0e-8;
    constexpr double roughness_tolerance = 1.0e-10;

    expectTrue(ejsdb_step.time_elapsed_s == inp_step.time_elapsed_s,
               "paired hydraulic timelines use the same elapsed time");
    expectTrue(ejsdb_step.status.success == inp_step.status.success,
               "paired hydraulic steps have the same status success flag");
    expectTrue(ejsdb_step.nodes_junctions.size() == inp_step.nodes_junctions.size(),
               "paired hydraulic steps contain the same junction-result count");
    expectTrue(ejsdb_step.nodes_reservoirs.size() == inp_step.nodes_reservoirs.size(),
               "paired hydraulic steps contain the same reservoir-result count");
    expectTrue(ejsdb_step.nodes_tanks.size() == inp_step.nodes_tanks.size(),
               "paired hydraulic steps contain the same tank-result count");
    expectTrue(ejsdb_step.links_pipes.size() == inp_step.links_pipes.size(),
               "paired hydraulic steps contain the same pipe-result count");
    expectTrue(ejsdb_step.links_pumps.size() == inp_step.links_pumps.size(),
               "paired hydraulic steps contain the same pump-result count");
    expectTrue(ejsdb_step.links_valves.size() == inp_step.links_valves.size(),
               "paired hydraulic steps contain the same valve-result count");

    const QString time_prefix = QStringLiteral("t=%1s ").arg(ejsdb_step.time_elapsed_s);

    for (const HydraulicSimulationResultNodeJunction &inp_junction : inp_step.nodes_junctions)
    {
        const HydraulicSimulationResultNodeJunction *ejsdb_junction = junctionResultById(
            ejsdb_step, inp_junction.id);
        expectTrueMessage(ejsdb_junction != nullptr,
                          time_prefix + QStringLiteral("junction %1 exists in both runs").arg(inp_junction.id));
        if (ejsdb_junction == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("junction %1 ").arg(inp_junction.id);
        expectNearMessage(ejsdb_junction->demand_requested_m3_per_h,
                          inp_junction.demand_requested_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("requested demand matches"));
        expectNearMessage(ejsdb_junction->demand_delivered_m3_per_h,
                          inp_junction.demand_delivered_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("delivered demand matches"));
        expectNearMessage(ejsdb_junction->demand_deficit_m3_per_h,
                          inp_junction.demand_deficit_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("demand deficit matches"));
        expectNearMessage(ejsdb_junction->total_demand_m3_per_h,
                          inp_junction.total_demand_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("total demand matches"));
        expectNearMessage(ejsdb_junction->emitter_flow_m3_per_h,
                          inp_junction.emitter_flow_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("emitter flow matches"));
        expectNearMessage(ejsdb_junction->leakage_flow_m3_per_h,
                          inp_junction.leakage_flow_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("leakage flow matches"));
        expectNearMessage(ejsdb_junction->hydraulic_head_m,
                          inp_junction.hydraulic_head_m, head_tolerance,
                          prefix + QStringLiteral("hydraulic head matches"));
        expectNearMessage(ejsdb_junction->pressure_head_m,
                          inp_junction.pressure_head_m, head_tolerance,
                          prefix + QStringLiteral("pressure head matches"));
        expectTrueMessage(ejsdb_junction->appears_in_control == inp_junction.appears_in_control,
                          prefix + QStringLiteral("control participation matches"));
    }

    for (const HydraulicSimulationResultNodeReservoir &inp_reservoir : inp_step.nodes_reservoirs)
    {
        const HydraulicSimulationResultNodeReservoir *ejsdb_reservoir = reservoirResultById(
            ejsdb_step, inp_reservoir.id);
        expectTrueMessage(ejsdb_reservoir != nullptr,
                          time_prefix + QStringLiteral("reservoir %1 exists in both runs").arg(inp_reservoir.id));
        if (ejsdb_reservoir == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("reservoir %1 ").arg(inp_reservoir.id);
        expectNearMessage(ejsdb_reservoir->net_demand_m3_per_h,
                          inp_reservoir.net_demand_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("net demand matches"));
        expectNearMessage(ejsdb_reservoir->hydraulic_head_m,
                          inp_reservoir.hydraulic_head_m, head_tolerance,
                          prefix + QStringLiteral("hydraulic head matches"));
        expectNearMessage(ejsdb_reservoir->pressure_head_m,
                          inp_reservoir.pressure_head_m, head_tolerance,
                          prefix + QStringLiteral("pressure head matches"));
        expectTrueMessage(ejsdb_reservoir->appears_in_control == inp_reservoir.appears_in_control,
                          prefix + QStringLiteral("control participation matches"));
    }

    for (const HydraulicSimulationResultNodeTank &inp_tank : inp_step.nodes_tanks)
    {
        const HydraulicSimulationResultNodeTank *ejsdb_tank = tankResultById(ejsdb_step, inp_tank.id);
        expectTrueMessage(ejsdb_tank != nullptr,
                          time_prefix + QStringLiteral("tank %1 exists in both runs").arg(inp_tank.id));
        if (ejsdb_tank == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("tank %1 ").arg(inp_tank.id);
        expectNearMessage(ejsdb_tank->net_demand_m3_per_h,
                          inp_tank.net_demand_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("net demand matches"));
        expectNearMessage(ejsdb_tank->hydraulic_head_m,
                          inp_tank.hydraulic_head_m, head_tolerance,
                          prefix + QStringLiteral("hydraulic head matches"));
        expectNearMessage(ejsdb_tank->pressure_head_m,
                          inp_tank.pressure_head_m, head_tolerance,
                          prefix + QStringLiteral("pressure head matches"));
        expectNearMessage(ejsdb_tank->water_level_m,
                          inp_tank.water_level_m, head_tolerance,
                          prefix + QStringLiteral("water level matches"));
        expectNearMessage(ejsdb_tank->volume_m3,
                          inp_tank.volume_m3, 1.0e-7,
                          prefix + QStringLiteral("volume matches"));
        expectNearMessage(ejsdb_tank->mixing_zone_volume_m3,
                          inp_tank.mixing_zone_volume_m3, 1.0e-7,
                          prefix + QStringLiteral("mixing-zone volume matches"));
        expectTrueMessage(ejsdb_tank->appears_in_control == inp_tank.appears_in_control,
                          prefix + QStringLiteral("control participation matches"));
    }

    for (const HydraulicSimulationResultLinkPipe &inp_pipe : inp_step.links_pipes)
    {
        const HydraulicSimulationResultLinkPipe *ejsdb_pipe = pipeResultById(ejsdb_step, inp_pipe.id);
        expectTrueMessage(ejsdb_pipe != nullptr,
                          time_prefix + QStringLiteral("pipe %1 exists in both runs").arg(inp_pipe.id));
        if (ejsdb_pipe == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("pipe %1 ").arg(inp_pipe.id);
        expectNearMessage(ejsdb_pipe->flow_m3_per_h, inp_pipe.flow_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("flow matches"));
        expectNearMessage(ejsdb_pipe->leakage_flow_m3_per_h, inp_pipe.leakage_flow_m3_per_h,
                          flow_tolerance, prefix + QStringLiteral("leakage flow matches"));
        expectNearMessage(ejsdb_pipe->velocity_m_per_s, inp_pipe.velocity_m_per_s,
                          velocity_tolerance, prefix + QStringLiteral("velocity matches"));
        expectNearMessage(ejsdb_pipe->head_loss_m, inp_pipe.head_loss_m, head_tolerance,
                          prefix + QStringLiteral("head loss matches"));
        expectNearMessage(ejsdb_pipe->head_loss_gradient_m_per_km,
                          inp_pipe.head_loss_gradient_m_per_km, 1.0e-7,
                          prefix + QStringLiteral("head-loss gradient matches"));
        expectTrueMessage(ejsdb_pipe->open == inp_pipe.open,
                          prefix + QStringLiteral("open state matches"));
        expectOptionalNear(ejsdb_pipe->roughness_hazen_williams,
                           inp_pipe.roughness_hazen_williams, roughness_tolerance,
                           prefix + QStringLiteral("Hazen-Williams roughness matches"));
        expectOptionalNear(ejsdb_pipe->roughness_darcy_weisbach_mm,
                           inp_pipe.roughness_darcy_weisbach_mm, roughness_tolerance,
                           prefix + QStringLiteral("Darcy-Weisbach roughness matches"));
        expectOptionalNear(ejsdb_pipe->roughness_chezy_manning,
                           inp_pipe.roughness_chezy_manning, roughness_tolerance,
                           prefix + QStringLiteral("Chezy-Manning roughness matches"));
        expectTrueMessage(ejsdb_pipe->appears_in_control == inp_pipe.appears_in_control,
                          prefix + QStringLiteral("control participation matches"));
    }

    for (const HydraulicSimulationResultLinkPump &inp_pump : inp_step.links_pumps)
    {
        const HydraulicSimulationResultLinkPump *ejsdb_pump = pumpResultById(ejsdb_step, inp_pump.id);
        expectTrueMessage(ejsdb_pump != nullptr,
                          time_prefix + QStringLiteral("pump %1 exists in both runs").arg(inp_pump.id));
        if (ejsdb_pump == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("pump %1 ").arg(inp_pump.id);
        expectNearMessage(ejsdb_pump->flow_m3_per_h, inp_pump.flow_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("flow matches"));
        expectNearMessage(ejsdb_pump->velocity_m_per_s, inp_pump.velocity_m_per_s,
                          velocity_tolerance, prefix + QStringLiteral("velocity matches"));
        expectNearMessage(ejsdb_pump->head_gain_m, inp_pump.head_gain_m, head_tolerance,
                          prefix + QStringLiteral("head gain matches"));
        expectTrueMessage(ejsdb_pump->open == inp_pump.open,
                          prefix + QStringLiteral("open state matches"));
        expectTrueMessage(ejsdb_pump->state == inp_pump.state,
                          prefix + QStringLiteral("pump state matches"));
        if (ejsdb_pump->open && inp_pump.open)
        {
            expectNearMessage(ejsdb_pump->speed_ratio, inp_pump.speed_ratio, setting_tolerance,
                              prefix + QStringLiteral("speed ratio matches while operating"));
        }
        expectNearMessage(ejsdb_pump->efficiency_percent, inp_pump.efficiency_percent,
                          percent_tolerance, prefix + QStringLiteral("efficiency matches"));
        expectNearMessage(ejsdb_pump->power_kw, inp_pump.power_kw, power_tolerance,
                          prefix + QStringLiteral("power matches"));
        expectTrueMessage(ejsdb_pump->appears_in_control == inp_pump.appears_in_control,
                          prefix + QStringLiteral("control participation matches"));
    }

    for (const HydraulicSimulationResultLinkValve &inp_valve : inp_step.links_valves)
    {
        const HydraulicSimulationResultLinkValve *ejsdb_valve = valveResultById(ejsdb_step, inp_valve.id);
        expectTrueMessage(ejsdb_valve != nullptr,
                          time_prefix + QStringLiteral("valve %1 exists in both runs").arg(inp_valve.id));
        if (ejsdb_valve == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("valve %1 ").arg(inp_valve.id);
        expectTrueMessage(ejsdb_valve->type == inp_valve.type,
                          prefix + QStringLiteral("type matches"));
        expectNearMessage(ejsdb_valve->flow_m3_per_h, inp_valve.flow_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("flow matches"));
        expectNearMessage(ejsdb_valve->velocity_m_per_s, inp_valve.velocity_m_per_s,
                          velocity_tolerance, prefix + QStringLiteral("velocity matches"));
        expectNearMessage(ejsdb_valve->head_loss_m, inp_valve.head_loss_m, head_tolerance,
                          prefix + QStringLiteral("head loss matches"));
        expectTrueMessage(ejsdb_valve->open == inp_valve.open,
                          prefix + QStringLiteral("open state matches"));
        expectTrueMessage(ejsdb_valve->active == inp_valve.active,
                          prefix + QStringLiteral("active state matches"));
        expectNearMessage(ejsdb_valve->setting_pressure_head_m,
                          inp_valve.setting_pressure_head_m, head_tolerance,
                          prefix + QStringLiteral("pressure setting matches"));
        expectNearMessage(ejsdb_valve->setting_flow_m3_per_h,
                          inp_valve.setting_flow_m3_per_h, flow_tolerance,
                          prefix + QStringLiteral("flow setting matches"));
        expectNearMessage(ejsdb_valve->setting_loss_coefficient,
                          inp_valve.setting_loss_coefficient, setting_tolerance,
                          prefix + QStringLiteral("loss setting matches"));
        expectNearMessage(ejsdb_valve->setting_position_percent,
                          inp_valve.setting_position_percent, setting_tolerance,
                          prefix + QStringLiteral("position setting matches"));
        expectTrueMessage(ejsdb_valve->appears_in_control == inp_valve.appears_in_control,
                          prefix + QStringLiteral("control participation matches"));
    }

    expectNearMessage(ejsdb_step.flow_balance.total_inflow_m3_per_h,
                      inp_step.flow_balance.total_inflow_m3_per_h, flow_tolerance,
                      time_prefix + QStringLiteral("total inflow matches"));
    expectNearMessage(ejsdb_step.flow_balance.total_outflow_m3_per_h,
                      inp_step.flow_balance.total_outflow_m3_per_h, flow_tolerance,
                      time_prefix + QStringLiteral("total outflow matches"));
    expectNearMessage(ejsdb_step.flow_balance.consumer_demand_m3_per_h,
                      inp_step.flow_balance.consumer_demand_m3_per_h, flow_tolerance,
                      time_prefix + QStringLiteral("consumer demand matches"));
    expectNearMessage(ejsdb_step.flow_balance.demand_deficit_m3_per_h,
                      inp_step.flow_balance.demand_deficit_m3_per_h, flow_tolerance,
                      time_prefix + QStringLiteral("demand deficit matches"));
    expectNearMessage(ejsdb_step.flow_balance.emitter_flow_m3_per_h,
                      inp_step.flow_balance.emitter_flow_m3_per_h, flow_tolerance,
                      time_prefix + QStringLiteral("emitter flow balance matches"));
    expectNearMessage(ejsdb_step.flow_balance.leakage_flow_m3_per_h,
                      inp_step.flow_balance.leakage_flow_m3_per_h, flow_tolerance,
                      time_prefix + QStringLiteral("leakage flow balance matches"));
    expectNearMessage(ejsdb_step.flow_balance.storage_flow_m3_per_h,
                      inp_step.flow_balance.storage_flow_m3_per_h, flow_tolerance,
                      time_prefix + QStringLiteral("storage flow matches"));
    expectNearMessage(ejsdb_step.flow_balance.flow_balance_ratio,
                      inp_step.flow_balance.flow_balance_ratio, 1.0e-9,
                      time_prefix + QStringLiteral("flow-balance ratio matches"));

    expectTrue(ejsdb_step.event_next.type == inp_step.event_next.type,
               "paired hydraulic steps identify the same next event type");
    expectTrue(ejsdb_step.event_next.time_until_event_s == inp_step.event_next.time_until_event_s,
               "paired hydraulic steps identify the same next event time");
}

void compareHydraulicTimelines(const EpanetResultRun &ejsdb_run, const EpanetResultRun &inp_run)
{
    expectTrue(ejsdb_run.state == inp_run.state,
               "paired hydraulic runs finish in the same state");
    expectTrue(ejsdb_run.result_timeline.validity == inp_run.result_timeline.validity,
               "paired hydraulic timelines have the same validity");
    expectTrue(ejsdb_run.result_timeline.results.size() == inp_run.result_timeline.results.size(),
               "paired hydraulic timelines contain the same number of result steps");
    if (ejsdb_run.result_timeline.results.size() != inp_run.result_timeline.results.size())
        return;

    for (qsizetype index = 0; index < ejsdb_run.result_timeline.results.size(); ++index)
    {
        compareHydraulicStep(
            ejsdb_run.result_timeline.results.at(index),
            inp_run.result_timeline.results.at(index));
    }
}

void expectMetricPumpControlFires(const EpanetResultRun &run)
{
    bool saw_open = false;
    bool saw_closed = false;
    bool saw_open_to_closed = false;
    bool saw_closed_to_open = false;
    bool have_previous = false;
    bool previous_open = false;

    for (const HydraulicSimulationResult &step : run.result_timeline.results)
    {
        const HydraulicSimulationResultLinkPump *pump = pumpResultById(step, QStringLiteral("PU1"));
        if (pump == nullptr)
            continue;
        saw_open = saw_open || pump->open;
        saw_closed = saw_closed || !pump->open;
        if (have_previous)
        {
            if (previous_open && !pump->open)
                saw_open_to_closed = true;
            if (!previous_open && pump->open)
                saw_closed_to_open = true;
        }
        previous_open = pump->open;
        have_previous = true;
    }

    expectTrue(saw_open, "metric hydraulic run observes PU1 open");
    expectTrue(saw_closed, "metric hydraulic run observes PU1 closed");
    expectTrue(saw_open_to_closed,
               "metric high-level control actually closes PU1 during the simulation");
    expectTrue(saw_closed_to_open,
               "metric low-level control actually opens PU1 during the simulation");
}

void expectUsPipeControlFires(const NetworkHydraulicDistribution &network, const EpanetResultRun &run)
{
    const HydraulicLinkPipe *model_pipe = nullptr;
    for (const HydraulicLinkPipe &pipe : network.links_pipes)
    {
        if (pipe.id == QStringLiteral("PCTRL"))
        {
            model_pipe = &pipe;
            break;
        }
    }
    expectTrue(model_pipe != nullptr, "US-customary fixture contains controlled pipe PCTRL");
    if (model_pipe == nullptr)
        return;
    expectTrue(model_pipe->initial_status == HydraulicLinkPipeInitialStatus::Closed,
               "US-customary controlled pipe starts closed before controls are evaluated");
    expectTrue(!run.result_timeline.results.isEmpty(),
               "US-customary hydraulic run returns at least one result step");
    if (run.result_timeline.results.isEmpty())
        return;

    const HydraulicSimulationResultLinkPipe *first_pipe = pipeResultById(
        run.result_timeline.results.first(), QStringLiteral("PCTRL"));
    expectTrue(first_pipe != nullptr, "US-customary first hydraulic result contains PCTRL");
    if (first_pipe == nullptr)
        return;
    expectTrue(first_pipe->open,
               "US-customary low-level raw control actually opens initially-closed PCTRL at time zero");
    expectTrue(first_pipe->appears_in_control,
               "US-customary controlled pipe is marked as participating in a control");
}

void expectUsRuleFires(const EpanetResultRun &run)
{
    const double before_setting = 5.0 * 0.22712470704;
    const double after_setting = 4.0 * 0.22712470704;
    bool saw_before = false;
    bool saw_after = false;

    for (const HydraulicSimulationResult &step : run.result_timeline.results)
    {
        const HydraulicSimulationResultLinkValve *valve = valveResultById(
            step, QStringLiteral("V1"));
        if (valve == nullptr)
            continue;
        if (step.time_elapsed_s < 12 * 3600
            && std::abs(valve->setting_flow_m3_per_h - before_setting) <= 1.0e-9)
        {
            saw_before = true;
        }
        if (step.time_elapsed_s >= 12 * 3600
            && std::abs(valve->setting_flow_m3_per_h - after_setting) <= 1.0e-9)
        {
            saw_after = true;
        }
    }

    expectTrue(saw_before,
               "US-customary rule leaves V1 at 5 GPM before 12 hours");
    expectTrue(saw_after,
               "US-customary raw rule changes V1 to 4 GPM from 12 hours onward");
}

const HydraulicNodeJunction *modelJunctionById(const NetworkHydraulicDistribution &network, const QString &id)
{
    for (const HydraulicNodeJunction &junction : network.nodes_junctions)
    {
        if (junction.id == id)
            return &junction;
    }
    return nullptr;
}

const HydraulicNodeReservoir *modelReservoirById(const NetworkHydraulicDistribution &network, const QString &id)
{
    for (const HydraulicNodeReservoir &reservoir : network.nodes_reservoirs)
    {
        if (reservoir.id == id)
            return &reservoir;
    }
    return nullptr;
}

const HydraulicNodeTank *modelTankById(const NetworkHydraulicDistribution &network, const QString &id)
{
    for (const HydraulicNodeTank &tank : network.nodes_tanks)
    {
        if (tank.id == id)
            return &tank;
    }
    return nullptr;
}

const HydraulicLinkPipe *modelPipeById(const NetworkHydraulicDistribution &network, const QString &id)
{
    for (const HydraulicLinkPipe &pipe : network.links_pipes)
    {
        if (pipe.id == id)
            return &pipe;
    }
    return nullptr;
}

const WaterQualitySimulationResultNodeJunction *qualityJunctionById(
    const WaterQualitySimulationResult &result,
    const QString &id)
{
    for (const WaterQualitySimulationResultNodeJunction &junction : result.nodes_junctions)
    {
        if (junction.id == id)
            return &junction;
    }
    return nullptr;
}

const WaterQualitySimulationResultNodeReservoir *qualityReservoirById(
    const WaterQualitySimulationResult &result,
    const QString &id)
{
    for (const WaterQualitySimulationResultNodeReservoir &reservoir : result.nodes_reservoirs)
    {
        if (reservoir.id == id)
            return &reservoir;
    }
    return nullptr;
}

const WaterQualitySimulationResultNodeTank *qualityTankById(
    const WaterQualitySimulationResult &result,
    const QString &id)
{
    for (const WaterQualitySimulationResultNodeTank &tank : result.nodes_tanks)
    {
        if (tank.id == id)
            return &tank;
    }
    return nullptr;
}

const WaterQualitySimulationResultLinkPipe *qualityPipeById(
    const WaterQualitySimulationResult &result,
    const QString &id)
{
    for (const WaterQualitySimulationResultLinkPipe &pipe : result.links_pipes)
    {
        if (pipe.id == id)
            return &pipe;
    }
    return nullptr;
}

const WaterQualitySimulationResultLinkPump *qualityPumpById(
    const WaterQualitySimulationResult &result,
    const QString &id)
{
    for (const WaterQualitySimulationResultLinkPump &pump : result.links_pumps)
    {
        if (pump.id == id)
            return &pump;
    }
    return nullptr;
}

const WaterQualitySimulationResultLinkValve *qualityValveById(
    const WaterQualitySimulationResult &result,
    const QString &id)
{
    for (const WaterQualitySimulationResultLinkValve &valve : result.links_valves)
    {
        if (valve.id == id)
            return &valve;
    }
    return nullptr;
}

void compareChemicalQualityStep(
    const WaterQualitySimulationResult &ejsdb_step,
    const WaterQualitySimulationResult &inp_step)
{
    constexpr double concentration_tolerance = 1.0e-8;
    constexpr double mass_flow_tolerance = 1.0e-8;
    const QString time_prefix = QStringLiteral("quality t=%1s ").arg(ejsdb_step.time_elapsed_s);

    expectTrue(ejsdb_step.time_elapsed_s == inp_step.time_elapsed_s,
               "paired quality timelines use the same elapsed time");
    expectTrue(ejsdb_step.nodes_junctions.size() == inp_step.nodes_junctions.size(),
               "paired quality steps contain the same junction count");
    expectTrue(ejsdb_step.nodes_reservoirs.size() == inp_step.nodes_reservoirs.size(),
               "paired quality steps contain the same reservoir count");
    expectTrue(ejsdb_step.nodes_tanks.size() == inp_step.nodes_tanks.size(),
               "paired quality steps contain the same tank count");
    expectTrue(ejsdb_step.links_pipes.size() == inp_step.links_pipes.size(),
               "paired quality steps contain the same pipe count");
    expectTrue(ejsdb_step.links_pumps.size() == inp_step.links_pumps.size(),
               "paired quality steps contain the same pump count");
    expectTrue(ejsdb_step.links_valves.size() == inp_step.links_valves.size(),
               "paired quality steps contain the same valve count");

    for (const WaterQualitySimulationResultNodeJunction &inp_junction : inp_step.nodes_junctions)
    {
        const WaterQualitySimulationResultNodeJunction *ejsdb_junction = qualityJunctionById(
            ejsdb_step, inp_junction.id);
        expectTrueMessage(
            ejsdb_junction != nullptr,
            time_prefix + QStringLiteral("junction %1 exists in both quality runs").arg(inp_junction.id));
        if (ejsdb_junction == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("junction %1 ").arg(inp_junction.id);
        expectNearMessage(
            ejsdb_junction->chemical_concentration_mg_per_l,
            inp_junction.chemical_concentration_mg_per_l,
            concentration_tolerance,
            prefix + QStringLiteral("chemical concentration matches"));
        expectNearMessage(
            ejsdb_junction->source_mass_flow_mg_per_min,
            inp_junction.source_mass_flow_mg_per_min,
            mass_flow_tolerance,
            prefix + QStringLiteral("source mass flow matches"));
    }

    for (const WaterQualitySimulationResultNodeReservoir &inp_reservoir : inp_step.nodes_reservoirs)
    {
        const WaterQualitySimulationResultNodeReservoir *ejsdb_reservoir = qualityReservoirById(
            ejsdb_step, inp_reservoir.id);
        expectTrueMessage(
            ejsdb_reservoir != nullptr,
            time_prefix + QStringLiteral("reservoir %1 exists in both quality runs").arg(inp_reservoir.id));
        if (ejsdb_reservoir == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("reservoir %1 ").arg(inp_reservoir.id);
        expectNearMessage(
            ejsdb_reservoir->chemical_concentration_mg_per_l,
            inp_reservoir.chemical_concentration_mg_per_l,
            concentration_tolerance,
            prefix + QStringLiteral("chemical concentration matches"));
        expectNearMessage(
            ejsdb_reservoir->source_mass_flow_mg_per_min,
            inp_reservoir.source_mass_flow_mg_per_min,
            mass_flow_tolerance,
            prefix + QStringLiteral("source mass flow matches"));
    }

    for (const WaterQualitySimulationResultNodeTank &inp_tank : inp_step.nodes_tanks)
    {
        const WaterQualitySimulationResultNodeTank *ejsdb_tank = qualityTankById(
            ejsdb_step, inp_tank.id);
        expectTrueMessage(
            ejsdb_tank != nullptr,
            time_prefix + QStringLiteral("tank %1 exists in both quality runs").arg(inp_tank.id));
        if (ejsdb_tank == nullptr)
            continue;
        const QString prefix = time_prefix + QStringLiteral("tank %1 ").arg(inp_tank.id);
        expectNearMessage(
            ejsdb_tank->chemical_concentration_mg_per_l,
            inp_tank.chemical_concentration_mg_per_l,
            concentration_tolerance,
            prefix + QStringLiteral("chemical concentration matches"));
        expectNearMessage(
            ejsdb_tank->source_mass_flow_mg_per_min,
            inp_tank.source_mass_flow_mg_per_min,
            mass_flow_tolerance,
            prefix + QStringLiteral("source mass flow matches"));
    }

    for (const WaterQualitySimulationResultLinkPipe &inp_pipe : inp_step.links_pipes)
    {
        const WaterQualitySimulationResultLinkPipe *ejsdb_pipe = qualityPipeById(
            ejsdb_step, inp_pipe.id);
        expectTrueMessage(
            ejsdb_pipe != nullptr,
            time_prefix + QStringLiteral("pipe %1 exists in both quality runs").arg(inp_pipe.id));
        if (ejsdb_pipe != nullptr)
        {
            expectNearMessage(
                ejsdb_pipe->chemical_concentration_mg_per_l,
                inp_pipe.chemical_concentration_mg_per_l,
                concentration_tolerance,
                time_prefix + QStringLiteral("pipe %1 chemical concentration matches").arg(inp_pipe.id));
        }
    }

    for (const WaterQualitySimulationResultLinkPump &inp_pump : inp_step.links_pumps)
    {
        const WaterQualitySimulationResultLinkPump *ejsdb_pump = qualityPumpById(
            ejsdb_step, inp_pump.id);
        expectTrueMessage(
            ejsdb_pump != nullptr,
            time_prefix + QStringLiteral("pump %1 exists in both quality runs").arg(inp_pump.id));
        if (ejsdb_pump != nullptr)
        {
            expectNearMessage(
                ejsdb_pump->chemical_concentration_mg_per_l,
                inp_pump.chemical_concentration_mg_per_l,
                concentration_tolerance,
                time_prefix + QStringLiteral("pump %1 chemical concentration matches").arg(inp_pump.id));
        }
    }

    for (const WaterQualitySimulationResultLinkValve &inp_valve : inp_step.links_valves)
    {
        const WaterQualitySimulationResultLinkValve *ejsdb_valve = qualityValveById(
            ejsdb_step, inp_valve.id);
        expectTrueMessage(
            ejsdb_valve != nullptr,
            time_prefix + QStringLiteral("valve %1 exists in both quality runs").arg(inp_valve.id));
        if (ejsdb_valve != nullptr)
        {
            expectNearMessage(
                ejsdb_valve->chemical_concentration_mg_per_l,
                inp_valve.chemical_concentration_mg_per_l,
                concentration_tolerance,
                time_prefix + QStringLiteral("valve %1 chemical concentration matches").arg(inp_valve.id));
        }
    }
}

void compareChemicalQualityRuns(
    const EpanetResultRun &ejsdb_run,
    const EpanetResultRun &inp_run)
{
    expectTrue(ejsdb_run.quality_results.size() == 1,
               "converted metric epanet-js network produces one chemical quality result");
    expectTrue(inp_run.quality_results.size() == 1,
               "paired metric INP produces one chemical quality result");
    if (ejsdb_run.quality_results.size() != 1 || inp_run.quality_results.size() != 1)
        return;

    const EpanetQualityResult &ejsdb_quality = ejsdb_run.quality_results.first();
    const EpanetQualityResult &inp_quality = inp_run.quality_results.first();
    expectTrue(ejsdb_quality.options.analysis == WaterQualityAnalysisType::Chemical,
               "converted metric epanet-js run executes chemical analysis");
    expectTrue(inp_quality.options.analysis == WaterQualityAnalysisType::Chemical,
               "paired metric INP executes chemical analysis");
    expectTrue(ejsdb_quality.options.chemical_name == QStringLiteral("CL2"),
               "converted metric epanet-js chemical name is CL2");
    expectTrue(inp_quality.options.chemical_name == QStringLiteral("CL2"),
               "paired metric INP chemical name is CL2");
    expectNear(ejsdb_quality.options.chemical_tolerance_mg_per_l, 0.01, 1.0e-12,
               "converted metric epanet-js chemical tolerance is preserved");
    expectNear(inp_quality.options.chemical_tolerance_mg_per_l, 0.01, 1.0e-12,
               "paired metric INP chemical tolerance is preserved");
    expectNear(ejsdb_quality.options.relative_diffusivity, 1.0, 1.0e-12,
               "converted metric epanet-js relative diffusivity is preserved");
    expectNear(inp_quality.options.relative_diffusivity, 1.0, 1.0e-12,
               "paired metric INP relative diffusivity is preserved");

    expectTrue(
        ejsdb_quality.result_timeline.results.size() == inp_quality.result_timeline.results.size(),
        "paired chemical quality timelines contain the same number of result steps");
    const qsizetype step_count = std::min(
        ejsdb_quality.result_timeline.results.size(),
        inp_quality.result_timeline.results.size());
    for (qsizetype index = 0; index < step_count; ++index)
    {
        compareChemicalQualityStep(
            ejsdb_quality.result_timeline.results.at(index),
            inp_quality.result_timeline.results.at(index));
    }
}

void expectMetricQualityModelData(
    const EpanetJsProjectConversionResult &ejsdb_result,
    const EpanetResultImport &inp_result)
{
    const NetworkHydraulicDistribution &ejsdb_network = ejsdb_result.network;
    const NetworkHydraulicDistribution &inp_network = inp_result.request.network;

    expectTrue(ejsdb_result.quality_runs.size() == 1,
               "metric epanet-js fixture imports one chemical quality run");
    expectTrue(inp_result.request.quality_runs.size() == 1,
               "paired metric INP imports one chemical quality run");

    const HydraulicNodeJunction *ejsdb_j1 = modelJunctionById(ejsdb_network, QStringLiteral("J1"));
    const HydraulicNodeJunction *inp_j1 = modelJunctionById(inp_network, QStringLiteral("J1"));
    expectTrue(ejsdb_j1 != nullptr && inp_j1 != nullptr,
               "metric quality fixtures both contain junction J1");
    if (ejsdb_j1 != nullptr && inp_j1 != nullptr)
    {
        expectNear(ejsdb_j1->initial_chemical_concentration_mg_per_l, 0.2, 1.0e-12,
                   "metric epanet-js J1 initial chemical quality is imported");
        expectNear(inp_j1->initial_chemical_concentration_mg_per_l, 0.2, 1.0e-12,
                   "paired metric INP J1 initial chemical quality matches");
    }

    const HydraulicNodeJunction *ejsdb_j3 = modelJunctionById(ejsdb_network, QStringLiteral("J3"));
    const HydraulicNodeJunction *inp_j3 = modelJunctionById(inp_network, QStringLiteral("J3"));
    expectTrue(ejsdb_j3 != nullptr && inp_j3 != nullptr,
               "metric quality fixtures both contain junction J3");
    if (ejsdb_j3 != nullptr && inp_j3 != nullptr)
    {
        expectTrue(ejsdb_j3->quality_source.type == HydraulicNodeQualitySourceType::MassBooster,
                   "metric epanet-js J3 imports MASS quality source");
        expectTrue(inp_j3->quality_source.type == HydraulicNodeQualitySourceType::MassBooster,
                   "paired metric INP J3 imports MASS quality source");
        expectNear(ejsdb_j3->quality_source.chemical_mass_flow_mg_per_min, 5.0, 1.0e-12,
                   "metric epanet-js J3 MASS source strength is preserved");
        expectNear(inp_j3->quality_source.chemical_mass_flow_mg_per_min, 5.0, 1.0e-12,
                   "paired metric INP J3 MASS source strength matches");
    }

    const HydraulicNodeReservoir *ejsdb_r1 = modelReservoirById(
        ejsdb_network, QStringLiteral("R1"));
    const HydraulicNodeReservoir *inp_r1 = modelReservoirById(
        inp_network, QStringLiteral("R1"));
    expectTrue(ejsdb_r1 != nullptr && inp_r1 != nullptr,
               "metric quality fixtures both contain reservoir R1");
    if (ejsdb_r1 != nullptr && inp_r1 != nullptr)
    {
        expectNear(ejsdb_r1->initial_chemical_concentration_mg_per_l, 1.0, 1.0e-12,
                   "metric epanet-js reservoir initial chemical quality is imported");
        expectNear(inp_r1->initial_chemical_concentration_mg_per_l, 1.0, 1.0e-12,
                   "paired metric INP reservoir initial chemical quality matches");
        expectTrue(ejsdb_r1->quality_source.type == HydraulicNodeQualitySourceType::Concentration,
                   "metric epanet-js reservoir imports CONCEN source");
        expectTrue(inp_r1->quality_source.type == HydraulicNodeQualitySourceType::Concentration,
                   "paired metric INP reservoir imports CONCEN source");
        expectNear(ejsdb_r1->quality_source.chemical_concentration_mg_per_l, 1.0, 1.0e-12,
                   "metric epanet-js reservoir source concentration is preserved");
        expectNear(inp_r1->quality_source.chemical_concentration_mg_per_l, 1.0, 1.0e-12,
                   "paired metric INP reservoir source concentration matches");
        expectTrue(!ejsdb_r1->quality_source.pattern_uuid.isNull(),
                   "metric epanet-js reservoir source pattern reference is imported");
        expectTrue(!inp_r1->quality_source.pattern_uuid.isNull(),
                   "paired metric INP reservoir source pattern reference is imported");
    }

    const HydraulicNodeTank *ejsdb_t1 = modelTankById(ejsdb_network, QStringLiteral("T1"));
    const HydraulicNodeTank *inp_t1 = modelTankById(inp_network, QStringLiteral("T1"));
    expectTrue(ejsdb_t1 != nullptr && inp_t1 != nullptr,
               "metric quality fixtures both contain tank T1");
    if (ejsdb_t1 != nullptr && inp_t1 != nullptr)
    {
        expectNear(ejsdb_t1->initial_chemical_concentration_mg_per_l, 0.4, 1.0e-12,
                   "metric epanet-js tank initial quality is imported");
        expectNear(inp_t1->initial_chemical_concentration_mg_per_l, 0.4, 1.0e-12,
                   "paired metric INP tank initial quality matches");
        expectTrue(ejsdb_t1->mixing_model == HydraulicNodeTankMixingModel::TwoCompartment,
                   "metric epanet-js tank imports two-compartment mixing");
        expectTrue(inp_t1->mixing_model == HydraulicNodeTankMixingModel::TwoCompartment,
                   "paired metric INP tank imports two-compartment mixing");
        expectNear(ejsdb_t1->mixing_fraction, 0.3, 1.0e-12,
                   "metric epanet-js tank mixing fraction is imported");
        expectNear(inp_t1->mixing_fraction, 0.3, 1.0e-12,
                   "paired metric INP tank mixing fraction matches");
        expectTrue(ejsdb_t1->override_bulk_reaction,
                   "metric epanet-js tank imports explicit bulk reaction override");
        expectTrue(inp_t1->override_bulk_reaction,
                   "paired metric INP tank imports explicit bulk reaction override");
        expectNear(ejsdb_t1->bulk_reaction.coefficient, -0.15, 1.0e-12,
                   "metric epanet-js tank bulk reaction coefficient is imported");
        expectNear(inp_t1->bulk_reaction.coefficient, -0.15, 1.0e-12,
                   "paired metric INP tank bulk reaction coefficient matches");
    }

    const HydraulicLinkPipe *ejsdb_p1 = modelPipeById(ejsdb_network, QStringLiteral("P1"));
    const HydraulicLinkPipe *inp_p1 = modelPipeById(inp_network, QStringLiteral("P1"));
    expectTrue(ejsdb_p1 != nullptr && inp_p1 != nullptr,
               "metric quality fixtures both contain pipe P1");
    if (ejsdb_p1 != nullptr && inp_p1 != nullptr)
    {
        expectTrue(ejsdb_p1->override_bulk_reaction && ejsdb_p1->override_wall_reaction,
                   "metric epanet-js pipe imports explicit bulk and wall reactions");
        expectTrue(inp_p1->override_bulk_reaction && inp_p1->override_wall_reaction,
                   "paired metric INP pipe imports explicit bulk and wall reactions");
        expectNear(ejsdb_p1->bulk_reaction.coefficient, -0.2, 1.0e-12,
                   "metric epanet-js pipe bulk reaction coefficient is imported");
        expectNear(inp_p1->bulk_reaction.coefficient, -0.2, 1.0e-12,
                   "paired metric INP pipe bulk reaction coefficient matches");
        expectNear(ejsdb_p1->wall_reaction.coefficient, -0.03, 1.0e-12,
                   "metric epanet-js pipe wall reaction coefficient is imported");
        expectNear(inp_p1->wall_reaction.coefficient, -0.03, 1.0e-12,
                   "paired metric INP pipe wall reaction coefficient matches");
    }

    expectNear(
        ejsdb_network.options_reaction.global_pipe_bulk_reaction.coefficient,
        -0.1,
        1.0e-12,
        "metric epanet-js global bulk reaction coefficient is imported");
    expectNear(
        inp_network.options_reaction.global_pipe_bulk_reaction.coefficient,
        -0.1,
        1.0e-12,
        "paired metric INP global bulk reaction coefficient matches");
    expectNear(
        ejsdb_network.options_reaction.global_pipe_wall_reaction.coefficient,
        -0.02,
        1.0e-12,
        "metric epanet-js global wall reaction coefficient is imported");
    expectNear(
        inp_network.options_reaction.global_pipe_wall_reaction.coefficient,
        -0.02,
        1.0e-12,
        "paired metric INP global wall reaction coefficient matches");
}

const HydraulicPipeMaterial *materialById(const NetworkHydraulicDistribution &network, const QString &id)
{
    for (const HydraulicPipeMaterial &material : network.pipe_materials)
    {
        if (material.id == id)
            return &material;
    }
    return nullptr;
}

double nativeJunctionDemandM3PerH(const NetworkHydraulicDistribution &network)
{
    double total = 0.0;
    for (const HydraulicNodeJunction &junction : network.nodes_junctions)
    {
        for (const HydraulicDemand &demand : junction.demands)
            total += demand.base_demand_m3_per_h;
    }
    return total;
}

double customerDemandM3PerH(const NetworkHydraulicDistribution &network)
{
    double total = 0.0;
    for (const HydraulicDemandPoint &demand_point : network.demand_points)
    {
        for (const HydraulicDemand &demand : demand_point.demands)
            total += demand.base_demand_m3_per_h;
    }
    return total;
}

void expectPhysicalTopologyMatches(
    const NetworkHydraulicDistribution &ejsdb_network,
    const NetworkHydraulicDistribution &inp_network)
{
    expectTrue(ejsdb_network.nodes_junctions.size() == inp_network.nodes_junctions.size(),
               "paired fixtures contain the same junction count");
    expectTrue(ejsdb_network.nodes_reservoirs.size() == inp_network.nodes_reservoirs.size(),
               "paired fixtures contain the same reservoir count");
    expectTrue(ejsdb_network.nodes_tanks.size() == inp_network.nodes_tanks.size(),
               "paired fixtures contain the same tank count");
    expectTrue(ejsdb_network.links_pipes.size() == inp_network.links_pipes.size(),
               "paired fixtures contain the same pipe count");
    expectTrue(ejsdb_network.links_pumps.size() == inp_network.links_pumps.size(),
               "paired fixtures contain the same pump count");
    expectTrue(ejsdb_network.links_valves.size() == inp_network.links_valves.size(),
               "paired fixtures contain the same valve count");
    expectTrue(ejsdb_network.controls_simple.size() == inp_network.controls_simple.size(),
               "paired fixtures contain the same simple-control count");
    expectTrue(ejsdb_network.controls_rules.size() == inp_network.controls_rules.size(),
               "paired fixtures contain the same rule-control count");

    const double ejsdb_total_demand = nativeJunctionDemandM3PerH(ejsdb_network)
        + customerDemandM3PerH(ejsdb_network);
    const double inp_total_demand = nativeJunctionDemandM3PerH(inp_network);
    expectNear(ejsdb_total_demand, inp_total_demand, 1e-9,
               "paired INP aggregates customer demand without changing total base demand");
}

void expectAllCustomerPointsUseAssignedJunctionMode(
    const NetworkHydraulicDistribution &network,
    qsizetype expected_count)
{
    qsizetype assigned_count = 0;
    for (const HydraulicDemandPoint &demand_point : network.demand_points)
    {
        if (demand_point.attachment.type == HydraulicDemandPointAttachmentType::Pipe
            && demand_point.attachment.pipe_allocation_mode
                == HydraulicDemandPointPipeAllocationMode::AssignedJunction
            && !demand_point.attachment.pipe_uuid.isNull()
            && !demand_point.attachment.pipe_assigned_junction_uuid.isNull()
            && demand_point.attachment.pipe_position >= 0.0
            && demand_point.attachment.pipe_position <= 1.0)
        {
            ++assigned_count;
        }
    }
    expectTrue(assigned_count == expected_count,
               "all synthetic customer points preserve pipe attachment plus assigned junction");
}

void testSyntheticMetricProject()
{
    const EpanetJsProjectConversionResult result = readAndConvertFixture(
        QStringLiteral("synthetic-metric.ejsdb"));
    if (!result.success)
        return;

    const EpanetResultImport inp_result = importPairedInp(
        QStringLiteral("synthetic-metric.inp"));
    if (!inp_result.status.success)
        return;

    const NetworkHydraulicDistribution &network = result.network;
    expectTrue(network.id == QStringLiteral("synthetic-metric"),
               "synthetic metric project name is preserved");
    expectTrue(network.nodes_junctions.size() == 3, "metric fixture imports three junctions");
    expectTrue(network.nodes_reservoirs.size() == 1, "metric fixture imports one reservoir");
    expectTrue(network.nodes_tanks.size() == 1, "metric fixture imports one tank");
    expectTrue(network.links_pipes.size() == 3, "metric fixture imports three pipes");
    expectTrue(network.links_pumps.size() == 1, "metric fixture imports one pump");
    expectTrue(network.links_valves.size() == 1, "metric fixture imports one valve");
    expectTrue(network.patterns_time.size() == 1, "metric fixture imports its demand pattern");
    expectTrue(network.pipe_materials.size() == 2, "metric fixture imports two pipe materials");
    expectTrue(network.demand_points.size() == 3, "metric fixture imports three customer points");
    expectTrue(network.controls_simple.size() == 2,
               "metric structured level-setting control becomes two AOWIS simple controls");

    expectNear(nativeJunctionDemandM3PerH(network), 6.12, 1e-12,
               "metric native junction demands convert from l/s to m3/h");
    expectNear(customerDemandM3PerH(network), 1.26, 1e-12,
               "metric customer demands convert from l/s to m3/h");
    expectAllCustomerPointsUseAssignedJunctionMode(network, 3);

    const HydraulicPipeMaterial *pvc = materialById(
        network, QStringLiteral("AOWIS Synthetic PVC"));
    expectTrue(pvc != nullptr, "metric synthetic PVC material is imported");
    if (pvc != nullptr)
    {
        const std::optional<double> age_zero = resolveHydraulicPipeMaterialRoughness(
            *pvc, HydraulicHeadlossFormula::HazenWilliams, 0);
        expectTrue(age_zero.has_value(), "metric material roughness resolves at age zero");
        if (age_zero.has_value())
            expectNear(age_zero.value(), 150.0, 1e-12,
                       "metric material age-zero Hazen-Williams value is preserved");
    }

    expectNear(network.controls_simple.at(0).trigger_water_level_m, 2.0, 1e-12,
               "metric low-level control retains its two-metre threshold");
    expectNear(network.controls_simple.at(1).trigger_water_level_m, 4.0, 1e-12,
               "metric high-level control retains its four-metre threshold");

    expectMetricQualityModelData(result, inp_result);
    expectPhysicalTopologyMatches(network, inp_result.request.network);

    const EpanetResultRun ejsdb_run = runConvertedNetwork(network, result.quality_runs);
    const EpanetResultRun inp_run = runImportedInp(inp_result);
    if (ejsdb_run.status.success && inp_run.status.success)
    {
        compareHydraulicTimelines(ejsdb_run, inp_run);
        compareChemicalQualityRuns(ejsdb_run, inp_run);
        expectMetricPumpControlFires(ejsdb_run);
    }
}

void testSyntheticUsCustomaryProject()
{
    const EpanetJsProjectConversionResult result = readAndConvertFixture(
        QStringLiteral("synthetic-us-customary.ejsdb"));
    if (!result.success)
        return;

    const EpanetResultImport inp_result = importPairedInp(
        QStringLiteral("synthetic-us-customary.inp"));
    if (!inp_result.status.success)
        return;

    const NetworkHydraulicDistribution &network = result.network;
    expectTrue(network.id == QStringLiteral("synthetic-us-customary"),
               "synthetic US-customary project name is preserved");
    expectTrue(network.nodes_junctions.size() == 3,
               "US-customary fixture imports three junctions");
    expectTrue(network.nodes_reservoirs.size() == 1,
               "US-customary fixture imports one reservoir");
    expectTrue(network.nodes_tanks.size() == 1,
               "US-customary fixture imports one tank");
    expectTrue(network.links_pipes.size() == 3,
               "US-customary fixture imports three pipes");
    expectTrue(network.links_pumps.isEmpty(),
               "US-customary fixture intentionally contains no pump");
    expectTrue(network.links_valves.size() == 1,
               "US-customary fixture imports one valve");
    expectTrue(network.pipe_materials.size() == 2,
               "US-customary fixture imports two pipe materials");
    expectTrue(network.demand_points.size() == 3,
               "US-customary fixture imports three customer points");
    expectTrue(network.controls_simple.size() == 2,
               "US-customary raw controls become two AOWIS simple controls");
    expectTrue(network.controls_rules.size() == 1,
               "US-customary raw EPANET rule becomes one AOWIS structured rule");

    expectNear(nativeJunctionDemandM3PerH(network), 35.0 * 0.22712470704, 1e-12,
               "US-customary native junction demands convert from gal/min to m3/h");
    expectNear(customerDemandM3PerH(network), 6.0 * 0.22712470704, 1e-12,
               "US-customary customer demands convert from gal/min to m3/h");
    expectAllCustomerPointsUseAssignedJunctionMode(network, 3);

    const HydraulicPipeMaterial *steel = materialById(
        network, QStringLiteral("AOWIS Synthetic Steel"));
    expectTrue(steel != nullptr, "US-customary synthetic steel material is imported");
    if (steel != nullptr)
    {
        const std::optional<double> age_zero = resolveHydraulicPipeMaterialRoughness(
            *steel, HydraulicHeadlossFormula::HazenWilliams, 0);
        expectTrue(age_zero.has_value(), "US-customary material roughness resolves at age zero");
        if (age_zero.has_value())
            expectNear(age_zero.value(), 120.0, 1e-12,
                       "US-customary material age-zero Hazen-Williams value is preserved");
    }

    expectNear(network.controls_simple.at(0).trigger_water_level_m, 2.4384, 1e-12,
               "US-customary eight-foot low-level trigger converts to metres");
    expectNear(network.controls_simple.at(1).trigger_water_level_m, 4.8768, 1e-12,
               "US-customary sixteen-foot high-level trigger converts to metres");

    if (!network.controls_rules.isEmpty())
    {
        const HydraulicControlRule &rule = network.controls_rules.first();
        expectTrue(rule.id == QStringLiteral("SYNTHETIC_FCV"),
                   "US-customary raw rule id is preserved");
        expectNear(rule.priority, 1.0, 1e-12,
                   "US-customary raw rule priority is preserved");
        expectTrue(rule.premises.size() == 2,
                   "US-customary raw rule imports SYSTEM TIME and node-pressure premises");
        expectTrue(rule.actions_then.size() == 1 && rule.actions_else.size() == 1,
                   "US-customary raw rule imports THEN and ELSE actions");
        if (rule.premises.size() == 2)
        {
            const HydraulicControlRulePremise &time = rule.premises.at(0);
            const HydraulicControlRulePremise &pressure = rule.premises.at(1);
            expectTrue(time.object == HydraulicControlRuleObject::System
                           && time.variable == HydraulicControlRuleVariable::Time
                           && time.elapsed_time_s.has_value()
                           && time.elapsed_time_s.value() == 12 * 3600,
                       "US-customary raw rule converts 12:00 elapsed time to seconds");
            expectTrue(pressure.object == HydraulicControlRuleObject::Node
                           && pressure.variable == HydraulicControlRuleVariable::Pressure
                           && pressure.pressure_head_m.has_value(),
                       "US-customary raw rule preserves node-pressure premise");
        }
        if (!rule.actions_then.isEmpty())
        {
            expectTrue(rule.actions_then.first().setting.valve_flow_m3_per_h.has_value(),
                       "US-customary raw THEN action imports FCV flow setting");
            if (rule.actions_then.first().setting.valve_flow_m3_per_h.has_value())
            {
                expectNear(rule.actions_then.first().setting.valve_flow_m3_per_h.value(),
                           4.0 * 0.22712470704, 1e-12,
                           "US-customary raw THEN FCV setting converts from gal/min to m3/h");
            }
        }
        if (!rule.actions_else.isEmpty())
        {
            expectTrue(rule.actions_else.first().setting.valve_flow_m3_per_h.has_value(),
                       "US-customary raw ELSE action imports FCV flow setting");
            if (rule.actions_else.first().setting.valve_flow_m3_per_h.has_value())
            {
                expectNear(rule.actions_else.first().setting.valve_flow_m3_per_h.value(),
                           5.0 * 0.22712470704, 1e-12,
                           "US-customary raw ELSE FCV setting converts from gal/min to m3/h");
            }
        }
    }

    expectPhysicalTopologyMatches(network, inp_result.request.network);

    const EpanetResultRun ejsdb_run = runConvertedNetwork(network, result.quality_runs);
    const EpanetResultRun inp_run = runImportedInp(inp_result);
    if (ejsdb_run.status.success && inp_run.status.success)
    {
        compareHydraulicTimelines(ejsdb_run, inp_run);
        expectUsPipeControlFires(network, ejsdb_run);
        expectUsRuleFires(ejsdb_run);
    }
}
#include "epanet_inp_unit_roundtrip.inc"
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_harness.runCase(
        "synthetic metric epanet-js and INP interoperability",
        testSyntheticMetricProject);
    test_harness.runCase(
        "synthetic US-customary epanet-js and INP interoperability",
        testSyntheticUsCustomaryProject);
    test_harness.runCase(
        "metric EPANET INP import/export canonical round trip",
        testMetricInpUnitRoundTrip);
    test_harness.runCase(
        "US EPANET INP import/export canonical round trip",
        testUsInpUnitRoundTrip);
    return test_harness.finish();
}
