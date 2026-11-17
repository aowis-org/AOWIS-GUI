// Included by epanet_js_project_integration_tests.cpp in its anonymous namespace.
// Exercise the real EPANET importer/exporter with both SI and US source formats.
// Presentation changes must never become implicit export-unit conversions.

const HydraulicNodeJunction *roundTripJunction(const NetworkHydraulicDistribution &network, const QString &id)
{
    for (const HydraulicNodeJunction &junction : network.nodes_junctions)
    {
        if (junction.id == id)
            return &junction;
    }
    return nullptr;
}

const HydraulicLinkPipe *roundTripPipe(const NetworkHydraulicDistribution &network, const QString &id)
{
    for (const HydraulicLinkPipe &pipe : network.links_pipes)
    {
        if (pipe.id == id)
            return &pipe;
    }
    return nullptr;
}

void testInpUnitRoundTrip(const QString &fixture_name, double expected_elevation_m,
                          double expected_pipe_length_m, double expected_pipe_diameter_mm,
                          double expected_flow_m3_per_h)
{
    EpanetRunner runner;
    const EpanetResultImport original = runner.importInp(fixturePath(fixture_name));
    expectTrueMessage(original.status.success,
                      fixture_name + QStringLiteral(": source INP imports"));
    if (!original.status.success)
        return;

    const HydraulicNodeJunction *source_junction = roundTripJunction(
        original.request.network, QStringLiteral("J1"));
    const HydraulicLinkPipe *source_pipe = roundTripPipe(
        original.request.network, QStringLiteral("P1"));
    expectTrue(source_junction != nullptr && source_pipe != nullptr,
               "source INP includes named junction and pipe");
    if (source_junction == nullptr || source_pipe == nullptr)
        return;

    expectNear(source_junction->elevation_m, expected_elevation_m, 1.0e-9,
               "INP elevation uses the file's SI/US unit system");
    expectTrue(source_pipe->length_measured_m.has_value(),
               "INP pipe retains its physical measured length");
    if (source_pipe->length_measured_m.has_value())
        expectNear(source_pipe->length_measured_m.value(), expected_pipe_length_m,
                   1.0e-9, "INP pipe length uses its file unit system");
    expectNear(source_pipe->diameter_mm, expected_pipe_diameter_mm, 1.0e-9,
               "INP pipe diameter uses its file unit system");
    expectTrue(!source_junction->demands.isEmpty(), "source INP retains junction demand");
    if (!source_junction->demands.isEmpty())
        expectNear(source_junction->demands.first().base_demand_m3_per_h,
                   expected_flow_m3_per_h, 1.0e-9,
                   "INP demand uses its file flow units, not GUI profile");

    UnitPresentationContext presentation;
    QJsonObject metric_units;
    metric_units.insert(QStringLiteral("elevation"), QStringLiteral("m"));
    metric_units.insert(QStringLiteral("length"), QStringLiteral("m"));
    metric_units.insert(QStringLiteral("diameter"), QStringLiteral("mm"));
    metric_units.insert(QStringLiteral("volumetric_flow_rate"), QStringLiteral("m³/h"));
    presentation.initialize(metric_units);

    const EpanetResultInp metric_export = runner.retrieveInp(original.request);
    expectTrueMessage(metric_export.status.success && !metric_export.inp_text.isEmpty(),
                      fixture_name + QStringLiteral(": INP exports with metric GUI presentation"));
    if (!metric_export.status.success || metric_export.inp_text.isEmpty())
        return;

    QJsonObject customary_units;
    customary_units.insert(QStringLiteral("elevation"), QStringLiteral("ft"));
    customary_units.insert(QStringLiteral("length"), QStringLiteral("ft"));
    customary_units.insert(QStringLiteral("diameter"), QStringLiteral("in"));
    customary_units.insert(QStringLiteral("volumetric_flow_rate"), QStringLiteral("US gal/min"));
    expectTrue(presentation.update(customary_units),
               "switching the GUI display unit mapping changes presentation generation");

    const EpanetResultInp customary_export = runner.retrieveInp(original.request);
    expectTrueMessage(customary_export.status.success && !customary_export.inp_text.isEmpty(),
                      fixture_name + QStringLiteral(": INP exports with US GUI presentation"));
    if (!customary_export.status.success)
        return;
    expectTrueMessage(metric_export.inp_text == customary_export.inp_text,
                      fixture_name + QStringLiteral(": switching GUI units cannot alter serialized INP"));

    // Import the exact serialized document, not the original fixture again.
    QTemporaryDir directory;
    expectTrue(directory.isValid(), "temporary INP round-trip directory exists");
    if (!directory.isValid())
        return;
    const QString export_path = directory.filePath(QStringLiteral("roundtrip.inp"));
    QFile output(export_path);
    expectTrue(output.open(QIODevice::WriteOnly), "round-trip INP file opens");
    if (!output.isOpen())
        return;
    const QByteArray bytes = customary_export.inp_text.toUtf8();
    const bool complete_write = output.write(bytes) == bytes.size();
    output.close();
    expectTrue(complete_write, "round-trip INP file writes in full");
    if (!complete_write)
        return;

    const EpanetResultImport imported_again = runner.importInp(export_path);
    expectTrueMessage(imported_again.status.success,
                      fixture_name + QStringLiteral(": exported INP re-imports"));
    if (!imported_again.status.success)
        return;

    const NetworkHydraulicDistribution &before = original.request.network;
    const NetworkHydraulicDistribution &after = imported_again.request.network;
    expectTrue(before.nodes_junctions.size() == after.nodes_junctions.size(),
               "INP round trip retains junction count");
    expectTrue(before.links_pipes.size() == after.links_pipes.size(),
               "INP round trip retains pipe count");
    expectTrue(before.nodes_tanks.size() == after.nodes_tanks.size(),
               "INP round trip retains tank count");
    expectTrue(before.nodes_reservoirs.size() == after.nodes_reservoirs.size(),
               "INP round trip retains reservoir count");
    expectTrue(before.links_valves.size() == after.links_valves.size(),
               "INP round trip retains valve count");
    expectTrue(before.links_pumps.size() == after.links_pumps.size(),
               "INP round trip retains pump count");

    if (before.nodes_reservoirs.size() == 1 && after.nodes_reservoirs.size() == 1)
        expectNear(after.nodes_reservoirs.first().hydraulic_head_m,
                   before.nodes_reservoirs.first().hydraulic_head_m, 1.0e-3,
                   "round-trip reservoir head remains canonical metres");
    if (before.nodes_tanks.size() == 1 && after.nodes_tanks.size() == 1)
    {
        const HydraulicNodeTank &source_tank = before.nodes_tanks.first();
        const HydraulicNodeTank &restored_tank = after.nodes_tanks.first();
        expectNear(restored_tank.bottom_elevation_m, source_tank.bottom_elevation_m, 1.0e-3,
                   "round-trip tank bottom elevation remains canonical metres");
        expectNear(restored_tank.water_level_initial_m, source_tank.water_level_initial_m,
                   1.0e-3, "round-trip tank level remains canonical metres");
        expectNear(restored_tank.diameter_m, source_tank.diameter_m, 1.0e-3,
                   "round-trip tank diameter remains canonical metres");
    }
    if (before.links_valves.size() == 1 && after.links_valves.size() == 1)
    {
        const HydraulicLinkValve &source_valve = before.links_valves.first();
        const HydraulicLinkValve &restored_valve = after.links_valves.first();
        expectTrue(restored_valve.type == source_valve.type,
                   "round-trip valve retains its pressure/flow setting type");
        if (source_valve.type == HydraulicLinkValveType::FCV)
            expectNear(restored_valve.setting_flow_m3_per_h,
                       source_valve.setting_flow_m3_per_h, 1.0e-3,
                       "round-trip FCV setting remains canonical m3/h");
        if (source_valve.type == HydraulicLinkValveType::PRV)
            expectNear(restored_valve.setting_pressure_head_m,
                       source_valve.setting_pressure_head_m, 1.0e-3,
                       "round-trip PRV setting remains canonical metres of head");
    }

    const HydraulicNodeJunction *restored_junction = roundTripJunction(after, QStringLiteral("J1"));
    const HydraulicLinkPipe *restored_pipe = roundTripPipe(after, QStringLiteral("P1"));
    expectTrue(restored_junction != nullptr && restored_pipe != nullptr,
               "INP round trip retains named junction and pipe");
    if (restored_junction == nullptr || restored_pipe == nullptr)
        return;

    // INP is decimal text; allow its textual precision, rather than comparing
    // re-imported floating-point values for bitwise equality.
    expectNear(restored_junction->elevation_m, source_junction->elevation_m, 1.0e-3,
               "round-trip junction elevation remains canonical metres");
    expectNear(restored_pipe->diameter_mm, source_pipe->diameter_mm, 1.0e-3,
               "round-trip pipe diameter remains canonical millimetres");
    expectTrue(restored_pipe->length_measured_m.has_value(),
               "round-trip pipe retains its measured length");
    if (source_pipe->length_measured_m.has_value()
        && restored_pipe->length_measured_m.has_value())
        expectNear(restored_pipe->length_measured_m.value(),
                   source_pipe->length_measured_m.value(), 1.0e-3,
                   "round-trip pipe length remains canonical metres");
    expectTrue(!restored_junction->demands.isEmpty(),
               "round-trip junction still has its base demand");
    if (!source_junction->demands.isEmpty() && !restored_junction->demands.isEmpty())
        expectNear(restored_junction->demands.first().base_demand_m3_per_h,
                   source_junction->demands.first().base_demand_m3_per_h, 1.0e-3,
                   "round-trip junction demand remains canonical m3/h");
}

void testMetricInpUnitRoundTrip()
{
    testInpUnitRoundTrip(QStringLiteral("synthetic-metric.inp"), 80.0, 220.0, 100.0, 3.96);
}

void testUsInpUnitRoundTrip()
{
    testInpUnitRoundTrip(QStringLiteral("synthetic-us-customary.inp"),
                         91.44, 152.4, 203.2, 22.0 * 0.22712470704);
}
