#include "map/editor/map_canvas_hit_box.h"
#include "map/editor/map_canvas_projection.h"
#include "test_harness.h"

#include <QPointF>
#include <QSize>

#include <optional>

namespace
{
AowisTestHarness test_harness;

class TestProjection final : public MapCanvasProjection
{
public:
    bool mercator = true;
    bool miss = false;

    std::optional<CoordinateWGS84> coordinateFromScreen(
        const QPointF &point, const QSize &) const override
    {
        if (this->miss)
            return std::nullopt;
        CoordinateWGS84 result;
        result.longitude_deg = point.x() / 10.0;
        result.latitude_deg = point.y() / 10.0;
        return result;
    }

    std::optional<QPointF> screenFromCoordinate(
        const CoordinateWGS84 &coordinate, const QSize &, double) const override
    {
        if (this->miss)
            return std::nullopt;
        return QPointF(coordinate.longitude_deg * 10.0,
                       coordinate.latitude_deg * 10.0);
    }

    bool supportsMercatorHitBox() const override { return this->mercator; }
};

CoordinateWGS84 coordinate(double longitude, double latitude)
{
    CoordinateWGS84 result;
    result.longitude_deg = longitude;
    result.latitude_deg = latitude;
    return result;
}

void testWorldScreenRoundTrip()
{
    TestProjection projection;
    const QSize viewport(800, 600);
    const std::optional<QPointF> screen = projection.screenFromCoordinate(
        coordinate(12.5, 32.2), viewport, 12.5);
    test_harness.expectTrue(screen.has_value(), "project a valid coordinate");
    if (!screen.has_value())
        return;
    const std::optional<CoordinateWGS84> world =
        projection.coordinateFromScreen(screen.value(), viewport);
    test_harness.expectTrue(world.has_value(), "unproject a valid screen point");
    if (world.has_value())
    {
        test_harness.expectNear(world->longitude_deg, 12.5, 1e-12,
                                "longitude round-trip");
        test_harness.expectNear(world->latitude_deg, 32.2, 1e-12,
                                "latitude round-trip");
    }
}

void testMercatorHitBox()
{
    TestProjection projection;
    const MapCanvasHitBox box = MapCanvasHitBox::around(
        projection, QPointF(200.0, 100.0), 20.0, QSize(800, 600));
    test_harness.expectTrue(box.contains(coordinate(20.0, 10.0)),
                            "center remains inside hit box");
    test_harness.expectTrue(box.contains(coordinate(21.0, 11.0)),
                            "nearby entity remains inside hit box");
    test_harness.expectTrue(!box.contains(coordinate(30.0, 10.0)),
                            "faraway entity is rejected in 2D");
    const MapCanvasHitBox wrapped = MapCanvasHitBox::around(
        projection, QPointF(1790.0, 100.0), 20.0, QSize(800, 600));
    test_harness.expectTrue(wrapped.contains(coordinate(-179.0, 10.0)),
                            "antimeridian window uses shortest wrapped longitude");
}

void testNoFalseRejectionWhenProjectionMisses()
{
    TestProjection projection;
    projection.mercator = false;
    const MapCanvasHitBox globe = MapCanvasHitBox::around(
        projection, QPointF(200.0, 100.0), 20.0, QSize(800, 600));
    test_harness.expectTrue(globe.contains(coordinate(150.0, -60.0)),
                            "non-Mercator projection does not use 2D geographic culling");

    projection.mercator = true;
    projection.miss = true;
    test_harness.expectTrue(!projection.coordinateFromScreen(
        QPointF(200.0, 100.0), QSize(800, 600)).has_value(),
        "ray miss does not fabricate a geographic coordinate");
    test_harness.expectTrue(!projection.screenFromCoordinate(
        coordinate(12.0, 40.0), QSize(800, 600), 12.0).has_value(),
        "unprojectable entity does not fabricate a screen position");
    const MapCanvasHitBox missing = MapCanvasHitBox::around(
        projection, QPointF(200.0, 100.0), 20.0, QSize(800, 600));
    test_harness.expectTrue(missing.contains(coordinate(-165.0, 80.0)),
                            "missing projection cannot incorrectly cull a hit");
}
void testGlobeEntityHitCallback()
{
    const QSize viewport(800, 600);
    int hits = 0;
    MapCanvasGlobeProjection projection(
        [](const QPointF &, const QSize &) -> std::optional<CoordinateWGS84>
        {
            return coordinate(10.0, 20.0);
        },
        [](const CoordinateWGS84 &, const QSize &) -> std::optional<QPointF>
        {
            return std::nullopt;
        },
        [&hits](const QPointF &, const QSize &)
            -> std::optional<InfrastructureEntityReference>
        {
            ++hits;
            InfrastructureEntityReference ref;
            ref.type = InfrastructureEntity::Pipe;
            ref.uuid = QUuid(QStringLiteral("{145c49c0-aa74-432c-93aa-412fc397472a}"));
            return ref;
        });
    const std::optional<InfrastructureEntityReference> hit = projection.hitEntity(
        QPointF(100.0, 200.0), viewport);
    test_harness.expectTrue(hit.has_value() && hit->type == InfrastructureEntity::Pipe,
                            "3D hit returned as shared editor entity reference");
    test_harness.expectTrue(hits == 1, "hit callback invoked for valid screen position");
    test_harness.expectTrue(!projection.hitEntity(QPointF(-1.0, 0.0), viewport).has_value(),
                            "outside viewport cannot select an entity");
    test_harness.expectTrue(!projection.hitEntity(QPointF(800.0, 0.0), viewport).has_value(),
                            "right border is outside the viewport");
    test_harness.expectTrue(!projection.hitEntity(QPointF(10.0, 20.0), QSize()).has_value(),
                            "zero-sized viewport cannot pick an entity");
    test_harness.expectTrue(hits == 1, "invalid positions never call renderer hit test");

    MapCanvasGlobeProjection invalid(
        [](const QPointF &, const QSize &) -> std::optional<CoordinateWGS84>
        { return std::nullopt; },
        [](const CoordinateWGS84 &, const QSize &) -> std::optional<QPointF>
        { return std::nullopt; },
        [](const QPointF &, const QSize &) -> std::optional<InfrastructureEntityReference>
        {
            InfrastructureEntityReference ref;
            ref.type = InfrastructureEntity::Pipe;
            return ref; // invalid UUID must not select any pipe
        });
    test_harness.expectTrue(!invalid.hitEntity(QPointF(10.0, 20.0), viewport).has_value(),
                            "invalid UUID is rejected by the shared projection");
}

void testGlobeCallbackProjection()
{
    int screen_calls = 0;
    int world_calls = 0;
    bool ray_misses = false;
    MapCanvasGlobeProjection globe(
        [&screen_calls, &ray_misses](const QPointF &point, const QSize &)
            -> std::optional<CoordinateWGS84>
        {
            ++screen_calls;
            if (ray_misses)
                return std::nullopt;
            return coordinate(9.0 + (point.x() - 400.0) / 10000.0,
                              47.0 - (point.y() - 300.0) / 10000.0);
        },
        [&world_calls, &ray_misses](const CoordinateWGS84 &world, const QSize &)
            -> std::optional<QPointF>
        {
            ++world_calls;
            if (ray_misses)
                return std::nullopt;
            return QPointF(400.0 + (world.longitude_deg - 9.0) * 10000.0,
                           300.0 - (world.latitude_deg - 47.0) * 10000.0);
        });
    const QSize viewport(800, 600);
    test_harness.expectTrue(!globe.supportsMercatorHitBox(),
                            "globe never enables the Mercator geographic hit box");
    const std::optional<QPointF> projected = globe.screenFromCoordinate(
        coordinate(9.01, 47.02), viewport, 180.0);
    test_harness.expectTrue(projected.has_value(), "project the drawn-surface coordinate");
    if (projected.has_value())
    {
        const std::optional<CoordinateWGS84> unprojected = globe.coordinateFromScreen(
            projected.value(), viewport);
        test_harness.expectTrue(unprojected.has_value(), "unproject the globe screen point");
        if (unprojected.has_value())
        {
            test_harness.expectNear(unprojected->longitude_deg, 9.01, 1e-10,
                                    "globe longitude round trip");
            test_harness.expectNear(unprojected->latitude_deg, 47.02, 1e-10,
                                    "globe latitude round trip");
        }
    }
    test_harness.expectTrue(screen_calls == 1 && world_calls == 1,
                            "globe delegates to the injected projection callbacks");
    ray_misses = true;
    test_harness.expectTrue(!globe.coordinateFromScreen(QPointF(0, 0), viewport).has_value(),
                            "terrain ray miss has no invented coordinate");
    test_harness.expectTrue(!globe.screenFromCoordinate(
        coordinate(9.0, 47.0), viewport, 9.0).has_value(),
        "off-horizon world projection has no invented screen point");
    const int calls_before = screen_calls + world_calls;
    test_harness.expectTrue(!globe.coordinateFromScreen(
        QPointF(-1.0, 200.0), viewport).has_value(),
        "globe movement cannot use a coordinate outside the left viewport edge");
    test_harness.expectTrue(!globe.coordinateFromScreen(
        QPointF(800.0, 200.0), viewport).has_value(),
        "globe placement cannot use a coordinate outside the right viewport edge");
    test_harness.expectTrue(!globe.coordinateFromScreen(
        QPointF(50.0, 600.0), viewport).has_value(),
        "globe placement cannot use a coordinate outside the bottom viewport edge");
    test_harness.expectTrue(!globe.coordinateFromScreen(
        QPointF(0, 0), QSize()).has_value(), "empty viewport is rejected");
    test_harness.expectTrue(!globe.screenFromCoordinate(
        coordinate(9.0, 95.0), viewport, 0.0).has_value(),
        "invalid latitude is rejected before callback");
    test_harness.expectTrue(screen_calls + world_calls == calls_before,
                            "invalid globe projections never invoke callbacks");
}
}

int main()
{
    test_harness.runCase("screen/world contract", testWorldScreenRoundTrip);
    test_harness.runCase("2D geographic hit box", testMercatorHitBox);
    test_harness.runCase("miss and globe broadphase", testNoFalseRejectionWhenProjectionMisses);
    test_harness.runCase("globe projection callbacks", testGlobeCallbackProjection);
    test_harness.runCase("globe renderer hit adapter", testGlobeEntityHitCallback);
    return test_harness.finish();
}
