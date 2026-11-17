#include "network/hydraulic_network_editor.h"
#include "network/network_render_snapshot_builder.h"
#include "test_harness.h"

#include <QDate>

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

void testPipeMaterialLibraryModel()
{
    NetworkHydraulicDistribution network;
    HydraulicNetworkEditor editor(network);

    HydraulicPipeMaterial ductile_iron;
    ductile_iron.id = QStringLiteral("Ductile Iron");
    ductile_iron.uuid = QUuid::createUuid();
    ductile_iron.description = QStringLiteral("Imported material definition");

    HydraulicPipeMaterialRoughnessAtAge new_pipe;
    new_pipe.age_years = 0;
    new_pipe.roughness_hazen_williams = 140.0;
    new_pipe.roughness_darcy_weisbach_mm = 0.26;
    ductile_iron.roughness_by_age.append(new_pipe);

    HydraulicPipeMaterialRoughnessAtAge aged_pipe;
    aged_pipe.age_years = 40;
    aged_pipe.roughness_hazen_williams = 115.0;
    ductile_iron.roughness_by_age.append(aged_pipe);
    network.pipe_materials.append(ductile_iron);

    CoordinateWGS84 from_coordinate;
    from_coordinate.latitude_deg = 11.0;
    from_coordinate.longitude_deg = 18.0;
    CoordinateWGS84 to_coordinate;
    to_coordinate.latitude_deg = 11.0;
    to_coordinate.longitude_deg = 18.1;
    const QUuid from_uuid = editor.addJunction(from_coordinate);
    const QUuid to_uuid = editor.addJunction(to_coordinate);
    const QUuid pipe_uuid = editor.addPipe(from_uuid, to_uuid, {});

    expectTrue(editor.setPipeMaterialUuid(pipe_uuid, ductile_iron.uuid),
               "pipe can reference a material from the network material library");
    expectTrue(editor.setPipeRoughnessMode(
                   pipe_uuid, HydraulicPipeRoughnessMode::MaterialLibrary),
               "pipe can select material-library roughness mode");
    std::optional<HydraulicLinkPipe> pipe = editor.pipe(pipe_uuid);
    expectTrue(pipe.has_value(), "material test pipe remains available");
    if (pipe.has_value())
    {
        expectTrue(pipe->material_uuid == ductile_iron.uuid,
                   "pipe stores material UUID instead of free-form material text");
        expectTrue(pipe->roughness_mode == HydraulicPipeRoughnessMode::MaterialLibrary,
                   "pipe stores material-library roughness mode");
    }

    expectTrue(!editor.setPipeMaterialUuid(pipe_uuid, QUuid::createUuid()),
               "pipe rejects a material UUID that is not present in the network library");
    pipe = editor.pipe(pipe_uuid);
    if (pipe.has_value())
    {
        expectTrue(pipe->material_uuid == ductile_iron.uuid,
                   "rejected material UUID does not alter the existing pipe material");
    }

    expectTrue(editor.setPipeMaterialUuid(pipe_uuid, QUuid()),
               "pipe material can be explicitly cleared");
    pipe = editor.pipe(pipe_uuid);
    if (pipe.has_value())
    {
        expectTrue(pipe->material_uuid.isNull(), "cleared pipe material stores a null UUID");
        expectTrue(pipe->roughness_mode == HydraulicPipeRoughnessMode::Explicit,
                   "clearing a material returns the pipe to explicit roughness mode");
    }
    expectTrue(!editor.setPipeRoughnessMode(
                    pipe_uuid, HydraulicPipeRoughnessMode::MaterialLibrary),
               "material-library roughness mode requires a selected material");

    expectTrue(network.pipe_materials.size() == 1,
               "network retains its material library entry");
    expectTrue(network.pipe_materials.first().roughness_by_age.size() == 2,
               "material retains multiple age-dependent roughness rows");
    expectTrue(network.pipe_materials.first().roughness_by_age.at(1).age_years == 40,
               "material age row retains its age");
    expectTrue(network.pipe_materials.first().roughness_by_age.at(1).roughness_hazen_williams.has_value(),
               "material age row retains formula-specific roughness values");
    if (network.pipe_materials.first().roughness_by_age.at(1).roughness_hazen_williams.has_value())
    {
        expectNear(network.pipe_materials.first().roughness_by_age.at(1).roughness_hazen_williams.value(),
                   115.0, 1e-12,
                   "material age row retains Hazen-Williams roughness");
    }

    HydraulicPipeMaterialRoughnessAtAge middle_age;
    middle_age.age_years = 20;
    middle_age.roughness_hazen_williams = 125.0;
    middle_age.roughness_darcy_weisbach_mm = 0.18;
    network.pipe_materials.first().roughness_by_age.append(middle_age);

    const std::optional<double> young_hw = resolveHydraulicPipeMaterialRoughness(
        network.pipe_materials.first(), HydraulicHeadlossFormula::HazenWilliams, 5);
    const std::optional<double> exact_hw = resolveHydraulicPipeMaterialRoughness(
        network.pipe_materials.first(), HydraulicHeadlossFormula::HazenWilliams, 20);
    const std::optional<double> between_hw = resolveHydraulicPipeMaterialRoughness(
        network.pipe_materials.first(), HydraulicHeadlossFormula::HazenWilliams, 25);
    const std::optional<double> between_dw = resolveHydraulicPipeMaterialRoughness(
        network.pipe_materials.first(), HydraulicHeadlossFormula::DarcyWeisbach, 25);
    const std::optional<double> aged_dw = resolveHydraulicPipeMaterialRoughness(
        network.pipe_materials.first(), HydraulicHeadlossFormula::DarcyWeisbach, 45);

    expectTrue(young_hw.has_value(), "material roughness resolves the age-zero row for a young pipe");
    expectTrue(exact_hw.has_value(), "material roughness resolves an exact age row");
    expectTrue(between_hw.has_value(), "material roughness resolves the last row not newer than the pipe");
    expectTrue(between_dw.has_value(), "material roughness resolves formula-specific values");
    expectTrue(aged_dw.has_value(), "missing formula data on a newer row falls back to the newest applicable formula row");
    if (young_hw.has_value())
        expectNear(young_hw.value(), 140.0, 1e-12, "young pipe uses age-zero Hazen-Williams roughness");
    if (exact_hw.has_value())
        expectNear(exact_hw.value(), 125.0, 1e-12, "exact age uses matching Hazen-Williams roughness");
    if (between_hw.has_value())
        expectNear(between_hw.value(), 125.0, 1e-12, "between ages uses previous Hazen-Williams roughness");
    if (between_dw.has_value())
        expectNear(between_dw.value(), 0.18, 1e-12, "Darcy-Weisbach uses formula-specific age row");
    if (aged_dw.has_value())
        expectNear(aged_dw.value(), 0.18, 1e-12, "Darcy-Weisbach ignores newer rows without Darcy-Weisbach data");

    HydraulicPipeMaterial future_only_material;
    future_only_material.id = QStringLiteral("Future only");
    future_only_material.uuid = QUuid::createUuid();
    HydraulicPipeMaterialRoughnessAtAge future_entry;
    future_entry.age_years = 30;
    future_entry.roughness_hazen_williams = 100.0;
    future_only_material.roughness_by_age.append(future_entry);
    expectTrue(!resolveHydraulicPipeMaterialRoughness(
                    future_only_material, HydraulicHeadlossFormula::HazenWilliams, 25).has_value(),
               "material roughness does not use an age row newer than the pipe");

    const std::optional<int> imported_year_age = hydraulicPipeAgeYears(
        QDate(2000, 1, 1), QDate(2026, 10, 2));
    expectTrue(imported_year_age.has_value() && imported_year_age.value() == 26,
               "pipe age calculation follows epanet-js installation-year semantics");
    expectTrue(!hydraulicPipeAgeYears(QDate(2030, 1, 1), QDate(2026, 10, 2)).has_value(),
               "future installation years are rejected");
}
}

int main()
{
    test_harness.runCase("pipe material library", testPipeMaterialLibraryModel);
    return test_harness.finish();
}
