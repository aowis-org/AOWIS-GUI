#include "config/unit_table_presentation.h"
#include "entity_inspector/map_legend_unit_presentation.h"
#include "map/core/map_symbology_length_units.h"
#include "simulation/simulation_statistics_unit_presentation.h"
#include "simulation/simulation_inspector_unit_presentation.h"

#include <QCoreApplication>
#include <QTranslator>
#include <QJsonObject>

#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include "unit_test_check.h"
#include <string_view>

namespace
{
void near(double actual, double expected)
{
    CHECK(std::isfinite(actual));
    CHECK(std::abs(actual - expected) <= 1.0e-9 * (1.0 + std::abs(expected)));
}
}

namespace
{
// Step 11c: a cross-section of actual simulation-result table headings.
// All source values are canonical. Display conversion must be reversible and
// may neither change the source nor mistake two physically different units.
void testSimulationResultColumnUnits()
{
    // The presentation API prints unit symbols; the Model IDs remain ASCII.
    CHECK(UnitTablePresentation::unitSymbol(QStringLiteral("m3/h")) == QStringLiteral("m³/h"));
    CHECK(UnitTablePresentation::unitSymbol(QStringLiteral("ft2")) == QStringLiteral("ft²"));
    CHECK(UnitTablePresentation::unitSymbol(QStringLiteral("ft3")) == QStringLiteral("ft³"));
    CHECK(UnitTablePresentation::unitSymbol(QStringLiteral("in2")) == QStringLiteral("in²"));
    CHECK(UnitTablePresentation::unitSymbol(QStringLiteral("kW.h")) == QStringLiteral("kWh"));
    CHECK(UnitTablePresentation::unitSymbol(QStringLiteral("(mg/L)^-1/h"))
          == QStringLiteral("(mg/L)⁻¹/h"));
    CHECK(UnitTablePresentation::formatNumber(1.5, 1, QStringLiteral(" ft3"))
          == QStringLiteral("1.5 ft³"));
    UnitPresentationContext context;
    QJsonObject profile;
    profile.insert(QStringLiteral("elevation"), QStringLiteral("ft"));
    profile.insert(QStringLiteral("hydraulic_head"), QStringLiteral("ft"));
    profile.insert(QStringLiteral("pressure_head"), QStringLiteral("ft"));
    profile.insert(QStringLiteral("head_gain"), QStringLiteral("ft"));
    profile.insert(QStringLiteral("head_loss"), QStringLiteral("ft"));
    profile.insert(QStringLiteral("water_level"), QStringLiteral("ft"));
    profile.insert(QStringLiteral("head_loss_gradient"), QStringLiteral("ft/1000ft"));
    profile.insert(QStringLiteral("volumetric_flow_rate"), QStringLiteral("L/s"));
    profile.insert(QStringLiteral("velocity"), QStringLiteral("ft/s"));
    profile.insert(QStringLiteral("volume"), QStringLiteral("ft3"));
    profile.insert(QStringLiteral("tank_diameter"), QStringLiteral("ft"));
    profile.insert(QStringLiteral("link_diameter"), QStringLiteral("in"));
    profile.insert(QStringLiteral("darcy_weisbach_roughness_height"), QStringLiteral("in"));
    profile.insert(QStringLiteral("chemical_mass_concentration"), QStringLiteral("ug/L"));
    profile.insert(QStringLiteral("chemical_mass_flow_rate"), QStringLiteral("g/h"));
    profile.insert(QStringLiteral("water_age"), QStringLiteral("min"));
    profile.insert(QStringLiteral("source_trace_percentage"), QStringLiteral("1"));
    profile.insert(QStringLiteral("efficiency"), QStringLiteral("1"));
    profile.insert(QStringLiteral("operating_time_percentage"), QStringLiteral("1"));
    profile.insert(QStringLiteral("valve_position"), QStringLiteral("1"));
    profile.insert(QStringLiteral("power"), QStringLiteral("hp"));
    profile.insert(QStringLiteral("energy_intensity"), QStringLiteral("kW.h/ft3"));
    context.initialize(profile);

    struct SimulationColumnCase
    {
        QString title;
        std::string_view quantity;
        double canonical;
        double expected_display;
    };
    // All headings below are present in the live node/link/result tables.
    const SimulationColumnCase cases[] = {
        {QStringLiteral("Elevation Direct [m]"), "elevation", 0.3048, 1.0},
        {QStringLiteral("Head [m]"), "hydraulic_head", 0.3048, 1.0},
        {QStringLiteral("Pressure [m]"), "pressure_head", 0.3048, 1.0},
        {QStringLiteral("Head Gain [m]"), "head_gain", 0.3048, 1.0},
        {QStringLiteral("Head Loss [m]"), "head_loss", 0.3048, 1.0},
        {QStringLiteral("Level Water [m]"), "water_level", 0.3048, 1.0},
        {QStringLiteral("Head Loss Unit [m/km]"), "head_loss_gradient", 2.5, 2.5},
        {QStringLiteral("Demand Delivered [m³/h]"), "volumetric_flow_rate", 3.6, 1.0},
        {QStringLiteral("Flow Leakage [m³/h]"), "volumetric_flow_rate", 3.6, 1.0},
        {QStringLiteral("Velocity [m/s]"), "velocity", 0.3048, 1.0},
        {QStringLiteral("Volume Mixing-Zone [m³]"), "volume", 0.028316846592, 1.0},
        {QStringLiteral("Diameter [m]"), "tank_diameter", 0.3048, 1.0},
        {QStringLiteral("Diameter [mm]"), "link_diameter", 25.4, 1.0},
        {QStringLiteral("Roughness DW Effective [mm]"), "darcy_weisbach_roughness_height", 25.4, 1.0},
        {QStringLiteral("Chemical Concentration [mg/L]"), "chemical_mass_concentration", 0.25, 250.0},
        {QStringLiteral("Initial Water Age [h]"), "water_age", 1.5, 90.0},
        {QStringLiteral("Quality Source Mass Flow [mg/min]"), "chemical_mass_flow_rate", 1000.0, 60.0},
        {QStringLiteral("Source Trace [%]"), "source_trace_percentage", 25.0, 0.25},
        {QStringLiteral("Efficiency [%]"), "efficiency", 75.0, 0.75},
        {QStringLiteral("Time Online [%]"), "operating_time_percentage", 20.0, 0.20},
        {QStringLiteral("Power [kW]"), "power", 0.7456998715822702, 1.0},
        {QStringLiteral("Energy Intensity Average [kWh/m³]"), "energy_intensity", 1.0, 0.028316846592}
    };
    for (const SimulationColumnCase &test : cases)
    {
        const std::optional<double> displayed = UnitTablePresentation::toDisplayForColumn(
            context, {test.quantity}, test.canonical);
        CHECK(displayed.has_value());
        near(*displayed, test.expected_display);
        const std::optional<double> restored = UnitTablePresentation::toCanonicalForColumn(
            context, {test.quantity}, *displayed);
        CHECK(restored.has_value());
        near(*restored, test.canonical);
        const QString formatted = UnitTablePresentation::formattedTitle(test.title, context, {test.quantity});
        CHECK(formatted != test.title);
        CHECK(formatted.endsWith(QStringLiteral("[")
            + UnitTablePresentation::unitSymbol(
                UnitTablePresentation::selectedUnit(context, test.quantity))
            + QStringLiteral("]")));
    }

    // A valve's pressure, flow and position settings share the same column.
    // Their quantities must remain row-specific rather than being inferred
    // from the column title (which deliberately has no physical unit).
    near(*UnitTablePresentation::toDisplay(context, "pressure_head", 0.3048), 1.0);
    QJsonObject pressure_units = profile;
    pressure_units.insert(QStringLiteral("pressure_head"), QStringLiteral("psi"));
    context.update(pressure_units);
    const double pressure_psi = 0.4333 / 0.3048;
    near(*UnitTablePresentation::toDisplay(context, "pressure_head", 1.0), pressure_psi);
    near(*UnitTablePresentation::toCanonical(context, "pressure_head", pressure_psi), 1.0);
    CHECK(context.setSpecificGravity(1.2));
    near(*UnitTablePresentation::toDisplay(context, "pressure_head", 1.0), pressure_psi * 1.2);
    near(*UnitTablePresentation::toCanonical(context, "pressure_head", pressure_psi * 1.2), 1.0);
    CHECK(!context.setSpecificGravity(0.0));
    near(context.specificGravity(), 1.2);
    // Loading a new project with invalid gravity resets the previous project's
    // presentation state, including values shown as pressure.
    const UnitPresentationContext::Generation previous_generation = context.generation();
    CHECK(context.setNetworkSpecificGravity(std::numeric_limits<double>::quiet_NaN()));
    CHECK(context.generation() > previous_generation);
    near(context.specificGravity(), 1.0);
    near(*UnitTablePresentation::toDisplay(context, "pressure_head", 1.0), pressure_psi);
    CHECK(context.setNetworkSpecificGravity(1.35));
    near(*UnitTablePresentation::toDisplay(context, "pressure_head", 1.0), pressure_psi * 1.35);
    CHECK(context.setNetworkSpecificGravity(-1.0));
    near(context.specificGravity(), 1.0);
    CHECK(context.setNetworkSpecificGravity(1.4));
    CHECK(context.setNetworkSpecificGravity(std::numeric_limits<double>::infinity()));
    near(context.specificGravity(), 1.0);
    CHECK(!context.setNetworkSpecificGravity(0.0));
    CHECK(context.update(profile));

    near(*UnitTablePresentation::toDisplay(context, "volumetric_flow_rate", 3.6), 1.0);
    near(*UnitTablePresentation::toDisplay(context, "valve_position", 50.0), 0.5);
    CHECK(UnitTablePresentation::toDisplayForColumn(
        context, {{}, UnitTablePresentation::ColumnKind::RowQuantity}, 50.0).value_or(-1.0) == 50.0);

}

// Exercise the public column contract with arbitrary and misleading headings.
// Canonical storage is also the sort/filter input, independent of displayed text.
void testExplicitColumnMetadata()
{
    using UnitTablePresentation::ColumnUnits;
    using UnitTablePresentation::ColumnKind;
    UnitPresentationContext context;
    const QJsonObject metric{{QStringLiteral("length"), QStringLiteral("m")}};
    const QJsonObject feet{{QStringLiteral("length"), QStringLiteral("ft")}};
    context.initialize(feet);
    const ColumnUnits length{"length"};
    const QString titles[] = {QStringLiteral("Distance"), QStringLiteral("Longueur"),
                              QStringLiteral("Renamed [kW]")};
    for (const QString &title : titles)
    {
        CHECK(UnitTablePresentation::formattedTitle(title, context, length).endsWith(
            QStringLiteral(" [ft]")));
        near(*UnitTablePresentation::toDisplayForColumn(context, length, 0.3048), 1.0);
        near(*UnitTablePresentation::toCanonicalForColumn(context, length, 1.0), 0.3048);
    }
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Longueur"), context, length)
          == QStringLiteral("Longueur [ft]"));
    // A suffix cannot turn a plain value into a physical measurement.
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Status [m]"), context, {})
          == QStringLiteral("Status [m]"));
    near(*UnitTablePresentation::toDisplayForColumn(context, {}, 42.0), 42.0);
    CHECK(!UnitTablePresentation::toDisplayForColumn(context, {"unknown_quantity"}, 1.0));
    CHECK(!UnitTablePresentation::toCanonicalForColumn(context, {"unknown_quantity"}, 1.0));

    const ColumnUnits price{{}, ColumnKind::EnergyPrice, QStringLiteral("XAF")};
    context.update({{QStringLiteral("energy"), QStringLiteral("MJ")}});
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Tarif"), context, price)
          == QStringLiteral("Tarif [XAF/MJ]"));
    near(*UnitTablePresentation::toDisplayForColumn(context, price, 0.18), 0.05);
    near(*UnitTablePresentation::toCanonicalForColumn(context, price, 0.05), 0.18);

    // Model measurements and independently selected filter bounds must survive
    // repeated presentation changes bit-for-bit. Only an explicit bound edit
    // is converted back; the other precise bound is never read from formatting.
    double canonical[] = {10.1234567890123, 2.1234567890123};
    double original[2];
    std::memcpy(original, canonical, sizeof canonical);
    const double upper_bound = canonical[0];
    for (int i = 0; i < 1000; ++i)
    {
        context.update(i % 2 == 0 ? metric : feet);
        for (double value : canonical)
        {
            const std::optional<double> displayed =
                UnitTablePresentation::toDisplayForColumn(context, length, value);
            CHECK(displayed.has_value());
            CHECK(!UnitTablePresentation::formatNumber(*displayed, 3).isEmpty());
        }
        CHECK(std::memcmp(original, canonical, sizeof canonical) == 0);
        CHECK(canonical[1] < canonical[0]);
    }
    context.update(feet);
    const std::optional<double> edited_bound =
        UnitTablePresentation::toCanonicalForColumn(context, length, 10.0);
    CHECK(edited_bound.has_value());
    near(*edited_bound, 3.048);
    CHECK(std::memcmp(&upper_bound, &original[0], sizeof upper_bound) == 0);
}

} // namespace

// Step 2: one locale/policy path for all table cell kinds. Formatting is
// read-only; the canonical value used by sorting and filtering stays intact.
void testTableNumericFormattingPolicy()
{
    UnitPresentationContext context;
    QJsonObject units;
    units.insert(QStringLiteral("base_demand_flow_rate"), QStringLiteral("L/s"));
    units.insert(QStringLiteral("energy"), QStringLiteral("MJ"));
    units.insert(QStringLiteral("first_order_bulk_reaction_coefficient"), QStringLiteral("/h"));
    context.initialize(units);

    const double canonical = 1234.5678901234567;
    const double original = canonical;
    const QLocale english(QLocale::English, QLocale::UnitedStates);
    const QLocale german(QLocale::German, QLocale::Germany);
    (void)context.setLocale(english);
    CHECK(UnitTablePresentation::formatNumber(context, canonical, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1234.568 m"));
    CHECK(UnitTablePresentation::formatNumber(context, canonical, 6, QString(), "length")
          == QStringLiteral("1234.56789"));
    CHECK(UnitTablePresentation::formatNumber(context, 1.0e-12, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1e-12 m"));
    CHECK(UnitTablePresentation::formatNumber(context, -0.0, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("0 m"));

    CHECK(context.setLocale(german));
    CHECK(UnitTablePresentation::formatNumber(context, canonical, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1234,568 m"));
    CHECK(UnitTablePresentation::formatNumber(context, 0.00049, 3, QStringLiteral(" m"), "length")
          != QStringLiteral("0 m"));
    CHECK(UnitTablePresentation::formatDemandRate(context, 3.6)
          == QStringLiteral("1 L/s"));
    CHECK(UnitTablePresentation::formatEnergyPrice(context, 0.18)
          == QStringLiteral("0,05"));
    // Energy prices share the energy quantity's formatting policy with the
    // pump tariff editor. They must not use an unregistered "energy_price" key.
    const NumericFormatPolicy energy_price_format{
        NumericFormatPolicy::Notation::Fixed, 4, false, true};
    CHECK(context.setFormatPolicy(energy_price_format, QStringLiteral("energy")));
    CHECK(UnitTablePresentation::formatEnergyPrice(context, 0.18)
          == QStringLiteral("0,0500"));
    CHECK(context.clearFormatPolicy(QStringLiteral("energy")));
    CHECK(UnitTablePresentation::formatEnergyPrice(context, 0.18)
          == QStringLiteral("0,05"));
    CHECK(UnitTablePresentation::formatReactionCoefficient(
              context, UnitTablePresentation::ReactionKind::Bulk, 1.0, 24.0)
          == QStringLiteral("1 /h"));

    // Changing grouping alone should not erase per-column decimal choices.
    NumericFormatPolicy grouped = context.formatPolicy();
    grouped.grouping = true;
    CHECK(context.setFormatPolicy(grouped));
    CHECK(UnitTablePresentation::formatNumber(context, canonical, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1.234,568 m"));
    CHECK(UnitTablePresentation::formatNumber(context, canonical, 6, QString(), "length")
          == QStringLiteral("1.234,56789"));
    CHECK(UnitTablePresentation::formatCount(context, 1234567)
          == QStringLiteral("1.234.567"));

    // An explicit quantity policy overrides the column's fixed decimals.
    NumericFormatPolicy fixed{NumericFormatPolicy::Notation::Fixed, 2, true, true};
    CHECK(context.setFormatPolicy(fixed, QStringLiteral("length")));
    CHECK(UnitTablePresentation::formatNumber(context, canonical, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1.234,57 m"));
    CHECK(UnitTablePresentation::formatNumber(context, 1.0, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1,00 m"));
    CHECK(UnitTablePresentation::formatNumber(context, 1.0e-12, 3, QStringLiteral(" m"), "length")
          != QStringLiteral("0,00 m"));
    CHECK(context.clearFormatPolicy(QStringLiteral("length")));
    CHECK(UnitTablePresentation::formatNumber(context, 1.0, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1 m"));

    NumericFormatPolicy scientific{NumericFormatPolicy::Notation::Scientific, 2, false, true};
    CHECK(context.setFormatPolicy(scientific));
    CHECK(UnitTablePresentation::formatNumber(context, canonical, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1,23e+03 m"));
    CHECK(UnitTablePresentation::formatDemandRate(context, 3.6)
          == QStringLiteral("1,00e+00 L/s"));
    CHECK(UnitTablePresentation::formatEnergyPrice(context, 0.18)
          == QStringLiteral("5,00e-02"));
    CHECK(UnitTablePresentation::formatCount(context, 1234567)
          == QStringLiteral("1234567"));

    // An explicit 6-significant-digit per-quantity override must be honored,
    // even though the global initial policy also happens to have 6 digits.
    NumericFormatPolicy significant;
    CHECK(context.setFormatPolicy(significant, QStringLiteral("length")));
    CHECK(UnitTablePresentation::formatNumber(context, canonical, 3, QStringLiteral(" m"), "length")
          == QStringLiteral("1234,57 m"));
    CHECK(canonical == original);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testTableNumericFormattingPolicy();
    UnitPresentationContext presentation;
    (void)presentation.setLocale(QLocale::c()); // Existing fixed-string expectations use '.'
    testSimulationResultColumnUnits();
    testExplicitColumnMetadata();
    CHECK(UnitTablePresentation::isUnitDependentTextColumn({{}, UnitTablePresentation::ColumnKind::DemandDetails}));
    CHECK(!UnitTablePresentation::isUnitDependentTextColumn({}));
    CHECK(!UnitTablePresentation::isUnitDependentTextColumn({"base_demand_flow_rate"}));
    // Ensure tiny nonzero numbers are not rounded all the way to displayed zero.
    CHECK(UnitTablePresentation::formatNumber(0.0, 3, QStringLiteral(" m")) == QStringLiteral("0 m"));
    CHECK(UnitTablePresentation::formatNumber(1.0e-12, 3, QStringLiteral(" m")) == QStringLiteral("1e-12 m"));
    CHECK(UnitTablePresentation::formatNumber(-1.0e-12, 3, QStringLiteral(" m")) == QStringLiteral("-1e-12 m"));
    CHECK(UnitTablePresentation::formatNumber(0.00049, 3, QStringLiteral(" m")) == QStringLiteral("0.00049 m"));

    QJsonObject units;
    units.insert(QStringLiteral("volumetric_flow_rate"), QStringLiteral("L/s"));
    units.insert(QStringLiteral("length"), QStringLiteral("ft"));
    units.insert(QStringLiteral("distance"), QStringLiteral("ft"));
    units.insert(QStringLiteral("pressure_head"), QStringLiteral("ft"));
    units.insert(QStringLiteral("link_diameter"), QStringLiteral("in"));
    units.insert(QStringLiteral("chemical_mass_concentration"), QStringLiteral("ug/L"));
    presentation.update(units);

    // Simulation inspectors format canonical results without changing stored
    // doubles, even when the chosen unit makes a value very small.
    {
        const double canonical_head = 1.234567890123;
        CHECK(SimulationInspectorUnitPresentation::format(
                    presentation, "pressure_head", canonical_head, 3)
                == QStringLiteral("4.05 ft"));
        CHECK(SimulationInspectorUnitPresentation::format(
                    presentation, "chemical_mass_concentration", 0.125, 3)
                == QStringLiteral("125 µg/L"));
        CHECK(SimulationInspectorUnitPresentation::format(
                    presentation, "volumetric_flow_rate", 1.0e-12, 3)
                != QStringLiteral("0 m³/h"));
        // Valve settings must not conflate pressure head, flow and position.
        CHECK(SimulationInspectorUnitPresentation::format(
                    presentation, "volumetric_flow_rate", 3.6, 3)
                == QStringLiteral("1 L/s"));
        QJsonObject valve_units = units;
        valve_units.insert(QStringLiteral("valve_position"), QStringLiteral("1"));
        CHECK(presentation.update(valve_units));
        CHECK(SimulationInspectorUnitPresentation::format(
                    presentation, "valve_position", 50.0, 3)
                == QStringLiteral("0.5"));
        CHECK(presentation.update(units));
        CHECK(canonical_head == 1.234567890123);
        QJsonObject simulation_units = units;
        simulation_units.insert(QStringLiteral("water_age"), QStringLiteral("min"));
        simulation_units.insert(QStringLiteral("chemical_mass_flow_rate"), QStringLiteral("g/h"));
        CHECK(presentation.update(simulation_units));
        CHECK(SimulationInspectorUnitPresentation::format(
                    presentation, "water_age", 1.5, 3)
                == QStringLiteral("90 min"));
        CHECK(SimulationInspectorUnitPresentation::format(
                    presentation, "chemical_mass_flow_rate", 1000.0, 3)
                == QStringLiteral("60 g/h"));
        CHECK(presentation.update(units));
    }

    // Legacy physical-pressure fallback is resolved by the Model profile,
    // never reimplemented in the already-resolved GUI context.
    {
        UnitPresentationContext partial;
        partial.initialize(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("psi")}});
        CHECK(partial.unit(QStringLiteral("pressure_head")).isEmpty());
        CHECK(partial.unit(QStringLiteral("pressure")) == QStringLiteral("psi"));
        CHECK(partial.update(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("psi")},
                                        {QStringLiteral("pressure_head"), QStringLiteral("m")}}));
        CHECK(partial.unit(QStringLiteral("pressure_head")) == QStringLiteral("m"));
    }

    // Physical icon-size controls retain integer-metre slider positions.
    // Their tooltip follows map distance, not pipe length.
    {
        const MapSymbologyLengthUnits::Presentation feet =
            MapSymbologyLengthUnits::forContext(presentation);
        CHECK(feet.unit == QStringLiteral("ft"));
        near(150.0 * feet.metres_to_display, 492.1259842519685);
        CHECK(feet.decimals >= 1);
        const double stored_size_m = 150.0;
        QJsonObject alternate_units = units;
        alternate_units.insert(QStringLiteral("length"), QStringLiteral("mi"));
        CHECK(presentation.update(alternate_units));
        const MapSymbologyLengthUnits::Presentation still_feet =
            MapSymbologyLengthUnits::forContext(presentation);
        CHECK(still_feet.unit == QStringLiteral("ft"));
        near(still_feet.metres_to_display, feet.metres_to_display);
        alternate_units.insert(QStringLiteral("distance"), QStringLiteral("mi"));
        CHECK(presentation.update(alternate_units));
        const MapSymbologyLengthUnits::Presentation miles =
            MapSymbologyLengthUnits::forContext(presentation);
        CHECK(miles.unit == QStringLiteral("mi"));
        near(150.0 * miles.metres_to_display, 150.0 / 1609.344);
        CHECK(miles.decimals >= 4);
        CHECK(stored_size_m == 150.0);
        CHECK(presentation.update(units));
    }

    // The simulation statistics dialog stores canonical head/flow errors.
    // Changing the profile affects labels only, never the original doubles.
    {
        UnitPresentationContext simulation_units;
        (void)simulation_units.setLocale(QLocale::c());
        QJsonObject simulation_profile;
        simulation_profile.insert(QStringLiteral("hydraulic_head"), QStringLiteral("ft"));
        simulation_profile.insert(QStringLiteral("volumetric_flow_rate"), QStringLiteral("L/s"));
        simulation_units.initialize(simulation_profile);
        const double original_head_m = 3.123456789;
        const double original_flow_m3_per_h = 3.6;
        CHECK(SimulationStatisticsUnits::selectedUnit(simulation_units, true) == QStringLiteral("ft"));
        CHECK(SimulationStatisticsUnits::selectedUnit(simulation_units, false) == QStringLiteral("L/s"));
        near(*UnitTablePresentation::toDisplay(simulation_units, "hydraulic_head", original_head_m),
             original_head_m / 0.3048);
        near(*UnitTablePresentation::toDisplay(simulation_units, "volumetric_flow_rate", original_flow_m3_per_h), 1.0);
        CHECK(SimulationStatisticsUnits::formatWithUnit(simulation_units, false, original_flow_m3_per_h)
                == QStringLiteral("1 L/s"));
        CHECK(SimulationStatisticsUnits::formatWithUnit(simulation_units, true, 0.0)
                == QStringLiteral("0 ft"));
        CHECK(original_head_m == 3.123456789);
        CHECK(original_flow_m3_per_h == 3.6);
    }

    // All legend visuals have explicit descriptors, shared by dock and HUD.
    for (int value = int(VisualNode::None); value <= int(VisualNode::WaterAge); ++value)
    {
        CHECK(MapLegendMetrics::node(static_cast<VisualNode>(value)).visual == value);
        CHECK(MapLegendMetrics::heatmap(static_cast<VisualHeatmap>(value)).visual == value);
    }
    for (int value = int(VisualLink::None); value <= int(VisualLink::WaterAge); ++value)
        CHECK(MapLegendMetrics::link(static_cast<VisualLink>(value)).visual == value);
    CHECK(MapLegendUnitPresentation::label(MapLegendMetrics::node(VisualNode::Chlorine))
          == QStringLiteral("Cl₂"));
    CHECK(MapLegendUnitPresentation::label(MapLegendMetrics::link(
              VisualLink::Roughness, MapLegendMetrics::RoughnessStyle::DarcyWeisbach))
          == QStringLiteral("Absolute Roughness ε"));
    const MapLegendMetrics::Descriptor &pressure = MapLegendMetrics::heatmap(VisualHeatmap::Pressure);
    const MapLegendUnitPresentation::Range before = MapLegendUnitPresentation::range(presentation, pressure, 1.0, 4.0);
    QTranslator german;
    CHECK(german.load(QStringLiteral(":/i18n/aowis_gui_de.qm")));
    CHECK(QCoreApplication::installTranslator(&german));
    CHECK(MapLegendUnitPresentation::label(pressure) == QStringLiteral("Druck"));
    const MapLegendUnitPresentation::Range after = MapLegendUnitPresentation::range(presentation, pressure, 1.0, 4.0);
    CHECK(before.minimum == after.minimum && before.maximum == after.maximum && before.unit == after.unit);
    CHECK(QCoreApplication::removeTranslator(&german));
    CHECK(MapLegendUnitPresentation::label(pressure) == QStringLiteral("Pressure"));
    // Map legends convert visible tick ranges only. The color ramp retains
    // the canonical values used by network symbology and simulation.
    using MapRange = MapLegendUnitPresentation::Range;
    const double canonical_base_demand = 3.6;
    const MapRange demand_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::node(VisualNode::BaseDemand), 0.0, canonical_base_demand);
    near(demand_legend.maximum, 1.0);
    CHECK(demand_legend.unit == QStringLiteral("L/s"));
    CHECK(canonical_base_demand == 3.6);
    const MapRange diameter_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::link(VisualLink::Diameter), 0.0, 25.4);
    near(diameter_legend.maximum, 1.0);
    CHECK(diameter_legend.unit == QStringLiteral("in"));
    // The quantity identity, not the printed label, determines conversion.
    MapLegendMetrics::Descriptor renamed_chlorine =
        MapLegendMetrics::heatmap(VisualHeatmap::Chlorine);
    renamed_chlorine.label = "A differently worded quality label";
    near(MapLegendUnitPresentation::range(
             presentation, renamed_chlorine, 0.0, 1.0).maximum, 1000.0);
    const MapRange chlorine_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::heatmap(VisualHeatmap::Chlorine), 0.0, 1.0);
    near(chlorine_legend.maximum, 1000.0);
    CHECK(chlorine_legend.unit == QStringLiteral("µg/L"));
    const MapRange no_unit_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::link(VisualLink::Roughness), 10.0, 120.0);
    near(no_unit_legend.minimum, 10.0);
    near(no_unit_legend.maximum, 120.0);
    CHECK(no_unit_legend.unit.isEmpty());

    QJsonObject map_units = units;
    map_units.insert(QStringLiteral("elevation"), QStringLiteral("ft"));
    map_units.insert(QStringLiteral("pressure_head"), QStringLiteral("ft"));
    map_units.insert(QStringLiteral("water_age"), QStringLiteral("min"));
    map_units.insert(QStringLiteral("darcy_weisbach_roughness_height"), QStringLiteral("in"));
    CHECK(presentation.update(map_units));
    const MapRange terrain_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::node(VisualNode::Elevation), 0.0, 100.0);
    near(terrain_legend.maximum, 328.0839895013123);
    CHECK(terrain_legend.unit == QStringLiteral("ft"));
    const MapRange head_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::node(VisualNode::Pressure), 0.0, 1.0);
    near(head_legend.maximum, 3.280839895013123);
    map_units.insert(QStringLiteral("pressure_head"), QStringLiteral("psi"));
    CHECK(presentation.update(map_units));
    CHECK(presentation.setSpecificGravity(1.2));
    CHECK(UnitTablePresentation::formattedTitle(
              QStringLiteral("Pressure [m]"), presentation, {"pressure_head"})
          == QStringLiteral("Pressure [psi]"));
    CHECK(MapLegendMetrics::node(VisualNode::Pressure).quantity == AOWIS_QUANTITY("pressure_head"));
    CHECK(MapLegendUnitPresentation::label(MapLegendMetrics::node(VisualNode::Chlorine))
          == MapLegendUnitPresentation::label(MapLegendMetrics::heatmap(VisualHeatmap::Chlorine)));
    CHECK(MapLegendMetrics::link(VisualLink::Roughness,
              MapLegendMetrics::RoughnessStyle::HazenWilliams).quantity.empty());
    CHECK(MapLegendMetrics::link(VisualLink::Roughness,
              MapLegendMetrics::RoughnessStyle::ChezyManning).quantity.empty());
    const MapRange pressure_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::node(VisualNode::Pressure), 0.0, 1.0);
    near(pressure_legend.maximum,
         1.2 * 0.4333 / 0.3048);
    CHECK(pressure_legend.unit == QStringLiteral("psi"));
    const MapRange age_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::heatmap(VisualHeatmap::WaterAge), 0.0, 1.0);
    near(age_legend.maximum, 60.0);
    CHECK(age_legend.unit == QStringLiteral("min"));
    const MapRange roughness_legend = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::link(VisualLink::Roughness, MapLegendMetrics::RoughnessStyle::DarcyWeisbach), 0.0, 25.4);
    near(roughness_legend.maximum, 1.0);
    CHECK(roughness_legend.unit == QStringLiteral("in"));
    // Length is already set to feet; verify large ranges use miles without
    // forcing a redundant presentation-generation change.
    const MapRange long_length = MapLegendUnitPresentation::range(
        presentation, MapLegendMetrics::link(VisualLink::Length), 0.0, 2000.0);
    CHECK(long_length.unit == QStringLiteral("mi"));
    near(long_length.maximum, 2000.0 / 1609.344);
    // Restore the profile so existing table assertions stay independent.
    CHECK(presentation.update(units));

    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Demand Base [m³/h]"), presentation, {"base_demand_flow_rate"})
            == QStringLiteral("Demand Base [L/s]"));
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Quality Initial [mg/L]"), presentation, {"chemical_mass_concentration"})
            == QStringLiteral("Quality Initial [µg/L]"));
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Valve Type"), presentation, {})
            == QStringLiteral("Valve Type"));

    const double canonical_flow = 1.234567890123;
    const std::optional<double> flow = UnitTablePresentation::toDisplay(presentation, "volumetric_flow_rate", canonical_flow);
    CHECK(flow.has_value());
    near(*flow, canonical_flow * 1000.0 / 3600.0);
    const std::optional<double> restored = UnitTablePresentation::toCanonical(presentation, "volumetric_flow_rate", *flow);
    CHECK(restored.has_value());
    near(*restored, canonical_flow);
    // Formatting never writes to, rounds, or mutates the canonical number.
    CHECK(canonical_flow == 1.234567890123);

    near(*UnitTablePresentation::toDisplay(presentation, "length", 1.0), 3.280839895013123);
    near(*UnitTablePresentation::toDisplay(presentation, "pressure_head", 1.0), 3.280839895013123);
    near(*UnitTablePresentation::toDisplay(presentation, "link_diameter", 25.4), 1.0);
    near(*UnitTablePresentation::toDisplay(presentation, "chemical_mass_concentration", 1.0), 1000.0);

    // Compound demand text must change units on demand without changing storage.
    const double category_rate = 3.123456789;
    CHECK(UnitTablePresentation::formatDemandRate(presentation, category_rate)
            == QStringLiteral("0.868 L/s"));
    CHECK(category_rate == 3.123456789);
    CHECK(UnitTablePresentation::formatDemandRate(presentation, 1.0e-12)
            != QStringLiteral("0 L/s"));

    // Energy prices are currency/energy, so conversion is inverse-energy.
    const UnitTablePresentation::ColumnUnits price_units{{}, UnitTablePresentation::ColumnKind::EnergyPrice, QStringLiteral("XAF")};
    const QString price_title = QStringLiteral("Energy Price [XAF/kWh]");
    units.insert(QStringLiteral("energy"), QStringLiteral("MJ"));
    CHECK(presentation.update(units));
    CHECK(UnitTablePresentation::formattedTitle(price_title, presentation, price_units)
            == QStringLiteral("Energy Price [XAF/MJ]"));
    near(*UnitTablePresentation::toDisplayForColumn(presentation, price_units, 0.18), 0.05);
    near(*UnitTablePresentation::toCanonicalForColumn(presentation, price_units, 0.05), 0.18);

    units.insert(QStringLiteral("energy"), QStringLiteral("J"));
    CHECK(presentation.update(units));
    CHECK(UnitTablePresentation::formattedTitle(price_title, presentation, price_units)
            == QStringLiteral("Energy Price [XAF/J]"));
    const QString per_joule = UnitTablePresentation::formatEnergyPrice(presentation, 0.18);
    CHECK(per_joule != QStringLiteral("0") && per_joule != QStringLiteral("—"));
    near(*UnitTablePresentation::toCanonicalForColumn(presentation, price_units,
         *UnitTablePresentation::toDisplayForColumn(presentation, price_units, 0.18)), 0.18);
    CHECK(UnitTablePresentation::formatEnergyPrice(presentation, 0.0) == QStringLiteral("0"));
    CHECK(!UnitTablePresentation::toDisplayForColumn(presentation, price_units, -1.0).has_value());
    // Control geometry and filtering must retain their original canonical values.
    CHECK(category_rate == 3.123456789);

    // Reaction coefficients retain their canonical value and display
    // order-dependent dimensions instead of a misleading column-wide suffix.
    units.insert(QStringLiteral("first_order_bulk_reaction_coefficient"), QStringLiteral("/h"));
    units.insert(QStringLiteral("first_order_wall_reaction_coefficient"), QStringLiteral("ft/d"));
    units.insert(QStringLiteral("zero_order_wall_reaction_coefficient"), QStringLiteral("mg/m2/h"));
    CHECK(presentation.update(units));
    using ReactionKind = UnitTablePresentation::ReactionKind;
    near(*UnitTablePresentation::reactionValue(presentation, ReactionKind::Bulk, 1.0, 24.0), 1.0);
    near(*UnitTablePresentation::reactionValue(presentation, ReactionKind::Bulk, 0.0, 24.0), 1.0);
    near(*UnitTablePresentation::reactionValue(presentation, ReactionKind::Bulk, -1.0, 24.0), 1.0);
    CHECK(UnitTablePresentation::reactionUnit(presentation, ReactionKind::Bulk, 1.0)
            == QStringLiteral("/h"));
    CHECK(UnitTablePresentation::reactionUnit(presentation, ReactionKind::Bulk, 0.0)
            == QStringLiteral("(mg/L)/h"));
    CHECK(UnitTablePresentation::reactionUnit(presentation, ReactionKind::Bulk, 2.0)
            == QStringLiteral("(mg/L)⁻¹/h"));
    near(*UnitTablePresentation::reactionValue(presentation, ReactionKind::Wall, 1.0, 0.3048), 1.0);
    near(*UnitTablePresentation::reactionValue(presentation, ReactionKind::Wall, 0.0, 24.0), 1.0);
    CHECK(UnitTablePresentation::reactionUnit(presentation, ReactionKind::Wall, 0.0)
            == QStringLiteral("mg/m²/h"));
    CHECK(UnitTablePresentation::formatReactionCoefficient(
                presentation, ReactionKind::Wall, 1.0, 0.3048)
            == QStringLiteral("1 ft/d"));
    CHECK(!UnitTablePresentation::reactionValue(
                presentation, ReactionKind::Wall, 2.0, 5.0).has_value());
    CHECK(UnitTablePresentation::formatReactionCoefficient(
                presentation, ReactionKind::Wall, 2.0, 5.0) == QStringLiteral("—"));
    CHECK(UnitTablePresentation::reactionColumnTooltip(
                presentation, ReactionKind::Bulk).contains(QStringLiteral("/h")));
    CHECK(UnitTablePresentation::reactionCellTooltip(
                presentation, ReactionKind::Wall, 0.0).contains(QStringLiteral("mg/m²/h")));
    const double canonical_reaction = 24.123456789;
    CHECK(UnitTablePresentation::formatReactionCoefficient(
                presentation, ReactionKind::Bulk, 1.0, canonical_reaction)
            == QStringLiteral("1.00514403 /h"));
    CHECK(canonical_reaction == 24.123456789);
    const QString tiny_reaction = UnitTablePresentation::formatReactionCoefficient(
        presentation, ReactionKind::Bulk, 1.0, 1.0e-10);
    CHECK(!tiny_reaction.startsWith(QStringLiteral("0 ")));
    CHECK(!tiny_reaction.startsWith(QStringLiteral("0.00000000 ")));

    const UnitPresentationContext::Generation generation = presentation.generation();
    CHECK(!presentation.update(units));
    CHECK(presentation.generation() == generation);
    units.insert(QStringLiteral("volumetric_flow_rate"), QStringLiteral("m3/h"));
    CHECK(presentation.update(units));
    CHECK(presentation.generation() != generation);
    near(*UnitTablePresentation::toDisplay(presentation, "volumetric_flow_rate", canonical_flow),
         canonical_flow);
    // Independent source preferences do not change canonical hydraulic values.
    units.insert(QStringLiteral("volumetric_flow_rate"), QStringLiteral("L/s"));
    units.insert(QStringLiteral("base_demand_flow_rate"), QStringLiteral("m3/h"));
    units.insert(QStringLiteral("customer_demand_flow_rate"), QStringLiteral("US gal/min"));
    units.insert(QStringLiteral("tank_initial_level"), QStringLiteral("ft"));
    units.insert(QStringLiteral("tank_minimum_level"), QStringLiteral("m"));
    units.insert(QStringLiteral("tank_maximum_level"), QStringLiteral("ft"));
    // Minimum tank volume follows the volume selection, even when an older
    // profile contains a conflicting per-field override.
    units.insert(QStringLiteral("volume"), QStringLiteral("ft3"));
    units.insert(QStringLiteral("tank_minimum_volume"), QStringLiteral("m3"));
    CHECK(presentation.update(units));
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Demand Base [m³/h]"), presentation, {"base_demand_flow_rate"})
            == QStringLiteral("Demand Base [m³/h]"));
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Customer Demand Base [m³/h]"), presentation, {"customer_demand_flow_rate"})
            == QStringLiteral("Customer Demand Base [US gal/min]"));
    CHECK(UnitTablePresentation::formatDemandRate(presentation, 3.6)
            == QStringLiteral("3.6 m³/h"));
    CHECK(UnitTablePresentation::formatDemandRate(
                presentation, 3.6, 3, "customer_demand_flow_rate")
            .endsWith(QStringLiteral(" US gal/min")));
    near(*UnitTablePresentation::toDisplayForColumn(
             presentation, {"customer_demand_flow_rate"}, 3.6),
         3.6 / aowis::units::usGallonsPerMinuteToCubicMetresPerHour(1.0));
    near(*UnitTablePresentation::toCanonicalForColumn(
             presentation, {"customer_demand_flow_rate"},
             *UnitTablePresentation::toDisplayForColumn(
                 presentation, {"customer_demand_flow_rate"}, canonical_flow)),
         canonical_flow);
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Level Initial [m]"), presentation, {"tank_initial_level"})
            == QStringLiteral("Level Initial [ft]"));
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Level Minimum [m]"), presentation, {"tank_minimum_level"})
            == QStringLiteral("Level Minimum [m]"));
    CHECK(UnitTablePresentation::formattedTitle(QStringLiteral("Volume Minimum [m³]"), presentation, {"tank_minimum_volume"})
            == QStringLiteral("Volume Minimum [ft³]"));
    near(*UnitTablePresentation::toDisplayForColumn(
             presentation, {"tank_minimum_volume"}, 1.0),
         1.0 / aowis::units::cubic_metres_per_cubic_foot);
    near(MapLegendUnitPresentation::range(
             presentation, MapLegendMetrics::node(VisualNode::BaseDemand), 0.0, 3.6).maximum, 3.6);
    CHECK(canonical_flow == 1.234567890123);
    return 0;
}
