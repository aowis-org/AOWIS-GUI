#ifndef MAP_CANVAS_PROJECTION_H
#define MAP_CANVAS_PROJECTION_H

#include "map/core/map_model.h"

#include <QPointF>
#include <QSize>

#include <optional>

// Coordinate conversion shared by the hydraulic editor's interaction helpers.
// A globe ray can miss the terrain or a world point can be behind the camera;
// callers must not turn either case into an edit at an invented coordinate.
// This interface deliberately owns no tool, selection, or placement state.
class MapCanvasProjection
{
public:
    virtual ~MapCanvasProjection() = default;

    virtual std::optional<CoordinateWGS84> coordinateFromScreen(
        const QPointF &screen_position, const QSize &viewport) const = 0;
    virtual std::optional<QPointF> screenFromCoordinate(
        const CoordinateWGS84 &coordinate, const QSize &viewport,
        double wrap_reference_lon) const = 0;

    // The 2D Mercator hit-box optimization relies on a rectangular geographic
    // window. A globe projection must use a different broadphase (or none).
    virtual bool supportsMercatorHitBox() const { return false; }
};

// The default adapter delegates EXACTLY to the existing 2D MapModel methods.
// Its screen-to-world conversion retains QPointF::toPoint() rounding and its
// world-to-screen conversion retains antimeridian wrap-reference behavior.
class MapCanvasFlatProjection final : public MapCanvasProjection
{
public:
    explicit MapCanvasFlatProjection(MapModel *model) : map_model(model) {}

    std::optional<CoordinateWGS84> coordinateFromScreen(
        const QPointF &screen_position, const QSize &viewport) const override
    {
        if (!this->map_model)
            return std::nullopt;
        return this->map_model->wgs84FromScreen(screen_position.toPoint(), viewport);
    }

    std::optional<QPointF> screenFromCoordinate(
        const CoordinateWGS84 &coordinate, const QSize &viewport,
        double wrap_reference_lon) const override
    {
        if (!this->map_model)
            return std::nullopt;
        return this->map_model->screenFromWgs84(coordinate, viewport, wrap_reference_lon);
    }

    bool supportsMercatorHitBox() const override { return true; }

private:
    MapModel *map_model = nullptr;
};

#endif // MAP_CANVAS_PROJECTION_H
