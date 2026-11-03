#ifndef MAP_CANVAS_PROJECTION_H
#define MAP_CANVAS_PROJECTION_H

#include "map/core/map_model.h"
#include "map/core/map_models.h"

#include <QPointF>
#include <QSize>

#include <cmath>
#include <functional>
#include <optional>
#include <utility>

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

    // Optional renderer-backed hit test for projections whose drawn entity
    // positions are not represented by flat map hit boxes. The shared editor
    // still owns selection and the hydraulic inspector update.
    virtual std::optional<InfrastructureEntityReference> hitEntity(
        const QPointF &, const QSize &) const { return std::nullopt; }
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

// Globe projection adapter for the SAME editor tools. The owning map surface
// supplies its current terrain-aware pick and drawn-surface projector; neither
// callback is retained by the editor overlay beyond the current frame.
// A missed ray or a point behind the globe is not a valid edit position.
class MapCanvasGlobeProjection final : public MapCanvasProjection
{
public:
    using ScreenToWorld = std::function<std::optional<CoordinateWGS84>(
        const QPointF &, const QSize &)>;
    using WorldToScreen = std::function<std::optional<QPointF>(
        const CoordinateWGS84 &, const QSize &)>;
    using ScreenToEntity = std::function<std::optional<InfrastructureEntityReference>(
        const QPointF &, const QSize &)>;

    MapCanvasGlobeProjection(ScreenToWorld screen_to_world, WorldToScreen world_to_screen,
                             ScreenToEntity screen_to_entity = ScreenToEntity())
        : screen_to_world(std::move(screen_to_world)),
          world_to_screen(std::move(world_to_screen)),
          screen_to_entity(std::move(screen_to_entity)) {}

    std::optional<CoordinateWGS84> coordinateFromScreen(
        const QPointF &screen_position, const QSize &viewport) const override
    {
        if (!this->screen_to_world || viewport.isEmpty()
            || !std::isfinite(screen_position.x()) || !std::isfinite(screen_position.y()))
            return std::nullopt;
        const std::optional<CoordinateWGS84> result =
            this->screen_to_world(screen_position, viewport);
        if (!result.has_value() || !std::isfinite(result->longitude_deg)
            || !std::isfinite(result->latitude_deg)
            || result->latitude_deg < -90.0 || result->latitude_deg > 90.0)
            return std::nullopt;
        return result;
    }

    std::optional<QPointF> screenFromCoordinate(
        const CoordinateWGS84 &coordinate, const QSize &viewport,
        double /*wrap_reference_lon*/) const override
    {
        if (!this->world_to_screen || viewport.isEmpty()
            || !std::isfinite(coordinate.longitude_deg)
            || !std::isfinite(coordinate.latitude_deg)
            || coordinate.latitude_deg < -90.0 || coordinate.latitude_deg > 90.0)
            return std::nullopt;
        const std::optional<QPointF> result = this->world_to_screen(coordinate, viewport);
        if (!result.has_value() || !std::isfinite(result->x()) || !std::isfinite(result->y()))
            return std::nullopt;
        return result;
    }

    std::optional<InfrastructureEntityReference> hitEntity(
        const QPointF &screen_position, const QSize &viewport) const override
    {
        if (!this->screen_to_entity || viewport.isEmpty()
            || !std::isfinite(screen_position.x()) || !std::isfinite(screen_position.y())
            || screen_position.x() < 0.0 || screen_position.y() < 0.0
            || screen_position.x() >= viewport.width()
            || screen_position.y() >= viewport.height())
            return std::nullopt;
        const std::optional<InfrastructureEntityReference> hit =
            this->screen_to_entity(screen_position, viewport);
        if (!hit.has_value() || hit->uuid.isNull()
            || hit->type == InfrastructureEntity::Unknown)
            return std::nullopt;
        return hit;
    }

private:
    ScreenToWorld screen_to_world;
    WorldToScreen world_to_screen;
    ScreenToEntity screen_to_entity;
};

#endif // MAP_CANVAS_PROJECTION_H
