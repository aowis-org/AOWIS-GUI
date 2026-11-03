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
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    test_harness.runCase("3D shared creation preview ray miss", testMissingGlobeCoordinateDiscardsPreview);
    return test_harness.finish();
}
