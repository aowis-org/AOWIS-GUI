#include "map/editor/map_canvas_placement.h"
#include "test_harness.h"

#include <QApplication>
#include <QPointF>
#include <QUuid>

namespace
{
AowisTestHarness test_harness;

void testMissingGlobeCoordinateDiscardsPreview()
{
    // No map widget is needed to exercise the shared placement state.
    MapCanvasPlacement placement(nullptr);
    placement.startCreate(InfrastructureEntity::Tank, QStringLiteral(":/tank.png"), 32);
    test_harness.expectTrue(placement.isCreating() && placement.floatingMarkerVisible(),
                            "ordinary 2D creation still starts with a floating icon");
    test_harness.expectTrue(!placement.mouseCoordinateValid(),
                            "creation has no fabricated initial ground coordinate");

    CoordinateWGS84 coordinate;
    coordinate.longitude_deg = 9.0;
    coordinate.latitude_deg = 47.0;
    placement.updateMousePosition(QPointF(240.0, 180.0), coordinate);
    placement.setConnectionTarget(QUuid::createUuid());
    test_harness.expectTrue(placement.mouseCoordinateValid(),
                            "valid globe terrain hit supplies the shared preview coordinate");

    placement.invalidateMousePosition(QPointF(900.0, 400.0));
    test_harness.expectTrue(!placement.mouseCoordinateValid(),
                            "globe ray miss invalidates the previous geographic pointer");
    test_harness.expectTrue(!placement.floatingMarkerVisible(),
                            "floating icon is hidden when the pointer leaves the globe");
    test_harness.expectTrue(placement.connectionTargetUuid().isNull(),
                            "the old snapped endpoint is cleared with the ray miss");
    test_harness.expectTrue(placement.mousePosition() == QPointF(900.0, 400.0),
                            "invalid pointer does not reuse the previous screen position");

    // Even returning near (0,0) must work (the 2D just-created-marker
    // suppression also uses that point as its initial sentinel).
    placement.updateMousePosition(QPointF(3.0, 4.0), coordinate);
    test_harness.expectTrue(placement.revealFloatingMarkerIfReady(),
                            "floating icon reappears when the terrain hit becomes valid");
    test_harness.expectTrue(placement.floatingMarkerVisible(),
                            "valid globe pointer restores normal shared placement state");
    placement.stop();
    test_harness.expectTrue(placement.isIdle() && !placement.floatingMarkerVisible(),
                            "tool cancellation clears its preview");
}
void testGlobeMoveRecoveryAfterTerrainMiss()
{
    MapCanvasPlacement placement(nullptr);
    CoordinateWGS84 start;
    start.longitude_deg = 8.0;
    start.latitude_deg = 50.0;
    const QUuid moving_uuid = QUuid::createUuid();
    test_harness.expectTrue(placement.startMove(
        InfrastructureEntity::Junction, moving_uuid,
        QStringLiteral(":/junction.png"), 32, QPointF(100.0, 100.0), start),
        "begin a shared 3D node move at a valid geographic position");
    placement.setMovingSelected(true);

    CoordinateWGS84 first;
    first.longitude_deg = 8.1;
    first.latitude_deg = 50.1;
    placement.updateMousePosition(QPointF(200.0, 200.0), first);
    placement.invalidateMousePosition(QPointF(900.0, 700.0));
    test_harness.expectTrue(placement.isMoving() && !placement.mouseCoordinateValid()
                            && !placement.floatingMarkerVisible(),
                            "off-globe movement hides preview without canceling the move");

    CoordinateWGS84 resumed;
    resumed.longitude_deg = 8.3;
    resumed.latitude_deg = 50.3;
    placement.updateMousePosition(QPointF(300.0, 300.0), resumed);
    test_harness.expectNear(placement.previousMouseCoordinate().longitude_deg,
                            first.longitude_deg, 1e-12,
                            "selected-group movement resumes from last valid longitude");
    test_harness.expectNear(placement.previousMouseCoordinate().latitude_deg,
                            first.latitude_deg, 1e-12,
                            "selected-group movement resumes from last valid latitude");
    test_harness.expectTrue(placement.mouseCoordinateValid()
                            && placement.revealFloatingMarkerIfReady(),
                            "preview reappears after a valid terrain hit");
    placement.stop();
    test_harness.expectTrue(placement.isIdle() && !placement.mouseCoordinateValid(),
                            "canceling movement clears pending coordinate validity");
}

void testGlobeLinkSnapAcrossMissingTerrainRay()
{
    MapCanvasPlacement placement(nullptr);
    placement.startCreate(InfrastructureEntity::Pipe, QStringLiteral(":/pipe.png"), 32);
    CoordinateWGS84 ground;
    ground.longitude_deg = 8.0;
    ground.latitude_deg = 50.0;
    placement.updateMousePosition(QPointF(120.0, 140.0), ground);
    const QUuid previous_target = QUuid::createUuid();
    placement.setConnectionTarget(previous_target);
    placement.invalidateMousePosition(QPointF(440.0, 240.0));
    test_harness.expectTrue(placement.connectionTargetUuid().isNull() &&
                            !placement.mouseCoordinateValid(),
                            "globe link preview cannot reuse a stale snap after a missed terrain ray");

    // A renderer hit on an elevated node may still be valid even when the
    // screen ray does not intersect the terrain under the pointer.
    const QUuid through_terrain_target = QUuid::createUuid();
    placement.setConnectionTarget(through_terrain_target);
    test_harness.expectTrue(placement.isCreating() &&
                            placement.connectionTargetUuid() == through_terrain_target &&
                            !placement.mouseCoordinateValid(),
                            "through-terrain network snapping does not require a DEM cursor hit");
    placement.stop();
    test_harness.expectTrue(placement.connectionTargetUuid().isNull() && placement.isIdle(),
                            "cancelling a 3D link clears its snap and placement state");
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    test_harness.runCase("3D shared creation preview ray miss", testMissingGlobeCoordinateDiscardsPreview);
    test_harness.runCase("3D link snap survives terrain-ray miss", testGlobeLinkSnapAcrossMissingTerrainRay);
    test_harness.runCase("3D selected-group move recovers from terrain-ray miss",
                         testGlobeMoveRecoveryAfterTerrainMiss);
    return test_harness.finish();
}
