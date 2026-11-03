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
}

int main()
{
    test_harness.runCase("screen/world contract", testWorldScreenRoundTrip);
    test_harness.runCase("2D geographic hit box", testMercatorHitBox);
    test_harness.runCase("miss and globe broadphase", testNoFalseRejectionWhenProjectionMisses);
    return test_harness.finish();
}
