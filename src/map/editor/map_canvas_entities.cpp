#include "map/editor/map_canvas_entities.h"
#include "map/editor/map_canvas_devicelinks.h"
#include "map/editor/map_canvas_markers.h"
#include "map/editor/map_canvas_pipes.h"
#include "map/editor/map_canvas_placement.h"
#include "map/editor/map_canvas_projection.h"
#include "map/editor/map_canvas_selection.h"
#include "map/editor/map_canvas_widget.h"

#include "geo/geo_metric_projection.h"
#include "geo/geo_web_mercator.h"
#include "network/infrastructure_entity_traits.h"
#ifdef Q_OS_WASM
#include "widgets/wasm_popup_menu.h"
#endif

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QMessageBox>
#include <QScopedValueRollback>

#include <cmath>
#include <functional>
#include <memory>
#include <utility>

#include <QtMath>

namespace
{
CoordinateWGS84 midpoint(const CoordinateWGS84 &from, const CoordinateWGS84 &to)
{
    CoordinateWGS84 coordinate;
    coordinate.latitude_deg = (from.latitude_deg + to.latitude_deg) / 2.0;
    const double longitude_delta = GeoWebMercator::normalizeLongitude(
        to.longitude_deg - from.longitude_deg);
    coordinate.longitude_deg = GeoWebMercator::normalizeLongitude(
        from.longitude_deg + longitude_delta / 2.0);
    return coordinate;
}

}

MapCanvasEntities::MapCanvasEntities(MapModel *map_model, HydraulicData *hydraulic_data,
                                     MapCanvasWidget *map_canvas)
    : QObject(map_canvas), map_model(map_model), hydraulic_data(hydraulic_data),
      map_canvas(map_canvas)
{
    this->point_markers = new MapCanvasMarkers(
        this->map_model, this->map_canvas, &this->pixmap_renderer, this);
    this->device_links = new MapCanvasDeviceLinks(
        this->map_model, this->map_canvas, &this->pixmap_renderer, this);
    this->pipes = new MapCanvasPipes(this->map_model, this->map_canvas, this);
    this->selection = new MapCanvasSelection(
        this->map_model, this->map_canvas, this->point_markers,
        this->device_links, this->pipes, this);
    this->placement = new MapCanvasPlacement(this->map_canvas, this);

    this->projection = std::make_unique<MapCanvasFlatProjection>(this->map_model);
    this->point_markers->setProjection(this->projection.get());
    this->device_links->setProjection(this->projection.get());
    this->pipes->setProjection(this->projection.get());

    setWrapReferenceLongitude(this->map_model->centerLon());

    connect(this->pipes, &MapCanvasPipes::pipeSelectionRequested,
            this, &MapCanvasEntities::selectPipe);
    connect(this->pipes, &MapCanvasPipes::pipeVertexAddRequested,
            this, &MapCanvasEntities::addPipeVertex);
    connect(this->pipes, &MapCanvasPipes::pipeVertexDeleteRequested,
            this, &MapCanvasEntities::deletePipeVertex);
    connect(this->pipes, &MapCanvasPipes::pipeVertexMoveRequested,
            this, &MapCanvasEntities::startPipeVertexMove);
    connect(this->pipes, &MapCanvasPipes::pipeVertexConversionRequested,
            this, &MapCanvasEntities::convertPipeVertexToJunction);
    connect(this->pipes, &MapCanvasPipes::signalCanvasUpdateRequested,
            this, &MapCanvasEntities::repaintCanvas);
    connect(this->device_links, &MapCanvasDeviceLinks::signalCanvasUpdateRequested,
            this, &MapCanvasEntities::repaintCanvas);

    // Icon widths follow the continuous zoom (MapCanvasMarkers::entityWidth());
    // rescale whenever the whole-pixel width actually changes.
    connect(this->map_model, &MapModel::view2dContinuousZoomChanged, this, [this](double)
    {
        updateMarkerScale();
    });
    // Panning moves no marker in map coordinates; the bgfx overlay follows
    // the camera itself (MapCanvasWidget::signalCameraChanged()).

    if (this->hydraulic_data)
    {
        connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]()
        {
            loadNetwork(this->hydraulic_data->networkHydraulic());
        });
        connect(this->hydraulic_data, &HydraulicData::signalNodeChanged,
                this, &MapCanvasEntities::onNodeChanged);
        connect(this->hydraulic_data, &HydraulicData::signalDemandPointChanged,
                this, &MapCanvasEntities::onDemandPointChanged);
        connect(this->hydraulic_data,
                &HydraulicData::signalDemandPointAttachmentSelectionRequested,
                this, [this](const QUuid &uuid)
        {
            applyExternalSelection(InfrastructureEntity::DemandPoint, uuid);
            startDemandPointAttachmentSelection(uuid);
        });
        connect(this->hydraulic_data, &HydraulicData::signalLinkChanged,
                this, &MapCanvasEntities::onLinkChanged);
        connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged,
                this, [this](bool)
        {
            updateCanvas();
        });
        connect(this->hydraulic_data, &HydraulicData::signalEntityLocateRequested,
                this, &MapCanvasEntities::onEntityLocateRequested);
        connect(this->hydraulic_data, &HydraulicData::signalSelectedTank, this,
            [this](const HydraulicNodeTank &tank)
        {
            applyExternalSelection(InfrastructureEntity::Tank, tank.uuid);
        });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedReservoir, this,
            [this](const HydraulicNodeReservoir &reservoir)
        {
            applyExternalSelection(InfrastructureEntity::Reservoir, reservoir.uuid);
        });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedJunction, this,
            [this](const HydraulicNodeJunction &junction)
        {
            applyExternalSelection(InfrastructureEntity::Junction, junction.uuid);
        });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedPipe, this,
            [this](const HydraulicLinkPipe &pipe)
        {
            applyExternalSelection(InfrastructureEntity::Pipe, pipe.uuid);
        });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedPump, this,
            [this](const HydraulicLinkPump &pump)
        {
            applyExternalSelection(InfrastructureEntity::Pump, pump.uuid);
        });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedValve, this,
            [this](const HydraulicLinkValve &valve)
        {
            applyExternalSelection(InfrastructureEntity::Valve, valve.uuid);
        });
        connect(this->hydraulic_data, &HydraulicData::signalSelectedDemandPoint, this,
            [this](const HydraulicDemandPoint &demand_point)
        {
            applyExternalSelection(InfrastructureEntity::DemandPoint, demand_point.uuid);
        });
        loadNetwork(this->hydraulic_data->networkHydraulic());
    }
}

MapCanvasEntities::~MapCanvasEntities() = default;

void MapCanvasEntities::setTerrainElevationResolver(TerrainElevationResolver resolver)
{
    this->terrain_elevation_resolver = std::move(resolver);
}

void MapCanvasEntities::setProjection(std::unique_ptr<MapCanvasProjection> projection)
{
    if (!projection)
        return;

    // End in-flight interactions before changing how their coordinates are
    // interpreted. The same selection/placement machinery stays in use.
    stopEntityPositioning();
    this->projection = std::move(projection);
    this->point_markers->setProjection(this->projection.get());
    this->device_links->setProjection(this->projection.get());
    this->pipes->setProjection(this->projection.get());
    updateCanvas();
}

void MapCanvasEntities::onNodeChanged(InfrastructureEntity entity_type, const QUuid &uuid)
{
    if (!this->hydraulic_data || this->synchronizing_geometry)
        return;

    const std::optional<HydraulicNodeCommonData> node =
        this->hydraulic_data->nodeCommonData(entity_type, uuid);
    if (!node.has_value() || !this->point_markers->setCoordinate(uuid, node->coordinate_wgs84))
        return;

    recalculateWrapReferenceLongitude();
    updateCanvas();
}

void MapCanvasEntities::onDemandPointChanged(const QUuid &uuid)
{
    if (this->synchronizing_geometry || !this->hydraulic_data)
        return;

    const std::optional<HydraulicDemandPoint> demand_point = this->hydraulic_data->demandPoint(uuid);
    if (!demand_point.has_value())
        return;

    this->point_markers->setCoordinate(uuid, demand_point->coordinate_wgs84);
    recalculateWrapReferenceLongitude();
    updateCanvas();
}

void MapCanvasEntities::onLinkChanged(InfrastructureEntity entity_type, const QUuid &uuid)
{
    if (!this->hydraulic_data || this->synchronizing_geometry)
        return;

    if (entity_type == InfrastructureEntity::Pipe)
    {
        const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(uuid);
        if (!pipe.has_value())
            return;

        QList<CoordinateWGS84> vertices;
        vertices.reserve(pipe->vertices.size());
        for (const HydraulicLinkVertex &vertex : pipe->vertices)
            vertices.append(vertex.coordinate_wgs84);
        this->pipes->setIntermediateVertices(uuid, vertices);
        return;
    }

    QUuid start_node_uuid;
    QUuid end_node_uuid;
    QList<HydraulicLinkVertex> vertices;
    if (entity_type == InfrastructureEntity::Pump)
    {
        const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(uuid);
        if (!pump.has_value())
            return;
        start_node_uuid = pump->node_uuid_from;
        end_node_uuid = pump->node_uuid_to;
        vertices = pump->vertices;
    }
    else if (entity_type == InfrastructureEntity::Valve)
    {
        const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(uuid);
        if (!valve.has_value())
            return;
        start_node_uuid = valve->node_uuid_from;
        end_node_uuid = valve->node_uuid_to;
        vertices = valve->vertices;
    }
    else
    {
        return;
    }

    CoordinateWGS84 center_coordinate;
    if (!vertices.isEmpty())
    {
        center_coordinate = vertices.first().coordinate_wgs84;
    }
    else
    {
        const std::optional<MapEntityMarker> start_marker = markerByUuid(start_node_uuid);
        const std::optional<MapEntityMarker> end_marker = markerByUuid(end_node_uuid);
        if (!start_marker.has_value() || !end_marker.has_value())
            return;
        center_coordinate = midpoint(start_marker->coord_wgs84, end_marker->coord_wgs84);
    }

    this->device_links->setCenterCoordinate(uuid, center_coordinate);
}

void MapCanvasEntities::onEntityLocateRequested(InfrastructureEntity entity_type, const QUuid &uuid)
{
    if (!this->hydraulic_data || !this->map_canvas)
        return;

    if (entity_type == InfrastructureEntity::DemandPoint)
    {
        const std::optional<HydraulicDemandPoint> demand_point =
            this->hydraulic_data->demandPoint(uuid);
        if (!demand_point.has_value())
            return;

        this->map_model->setCenter(demand_point->coordinate_wgs84.longitude_deg,
                                   demand_point->coordinate_wgs84.latitude_deg,
                                   this->map_canvas->size());
        return;
    }

    if (InfrastructureEntityTraits::isHydraulicConnectionNode(entity_type))
    {
        const std::optional<HydraulicNodeCommonData> node =
            this->hydraulic_data->nodeCommonData(entity_type, uuid);
        if (!node.has_value())
            return;

        this->map_model->setCenter(node->coordinate_wgs84.longitude_deg,
                                   node->coordinate_wgs84.latitude_deg,
                                   this->map_canvas->size());
        return;
    }

    QUuid start_node_uuid;
    QUuid end_node_uuid;
    if (entity_type == InfrastructureEntity::Pipe)
    {
        const std::optional<HydraulicLinkPipe> pipe = this->hydraulic_data->pipe(uuid);
        if (!pipe.has_value())
            return;
        start_node_uuid = pipe->node_uuid_from;
        end_node_uuid = pipe->node_uuid_to;
    }
    else if (entity_type == InfrastructureEntity::Pump)
    {
        const std::optional<HydraulicLinkPump> pump = this->hydraulic_data->pump(uuid);
        if (!pump.has_value())
            return;
        start_node_uuid = pump->node_uuid_from;
        end_node_uuid = pump->node_uuid_to;
    }
    else if (entity_type == InfrastructureEntity::Valve)
    {
        const std::optional<HydraulicLinkValve> valve = this->hydraulic_data->valve(uuid);
        if (!valve.has_value())
            return;
        start_node_uuid = valve->node_uuid_from;
        end_node_uuid = valve->node_uuid_to;
    }
    else
    {
        return;
    }

    const std::optional<MapEntityMarker> start_marker = markerByUuid(start_node_uuid);
    const std::optional<MapEntityMarker> end_marker = markerByUuid(end_node_uuid);
    if (!start_marker.has_value() || !end_marker.has_value())
        return;

    const CoordinateWGS84 coordinate = midpoint(
        start_marker->coord_wgs84, end_marker->coord_wgs84);
    this->map_model->setCenter(coordinate.longitude_deg,
                               coordinate.latitude_deg,
                               this->map_canvas->size());
}

void MapCanvasEntities::loadNetwork(const NetworkHydraulic &network)
{
    cancelDemandPointAttachmentInteraction();
    setWrapReferenceLongitude(this->map_model->centerLon());
    this->device_links->clearPlacement();
    this->pipes->clearPlacement();
    this->pipes->cancelPipeVertexMove();
    this->placement->stop();
    clearMoveSnapshot();
    this->selection->clear();
    this->device_links->clear();
    this->pipes->clear();
    this->point_markers->clear();

    QHash<QUuid, MapEntityMarker> node_markers;
    const std::function<void(InfrastructureEntity, const QUuid &, const CoordinateWGS84 &)> add_node =
        [this, &node_markers](InfrastructureEntity type, const QUuid &uuid,
                              const CoordinateWGS84 &coordinate)
    {
        if (uuid.isNull() || node_markers.contains(uuid))
        {
            qWarning() << "Cannot load hydraulic node with invalid or duplicate UUID:" << uuid;
            return;
        }

        InfrastructureEntityReference reference;
        reference.type = type;
        reference.uuid = uuid;
        const MapEntityMarker marker = this->point_markers->addMarker(
            reference, coordinate, this->point_markers->pixmapPathForEntity(type),
            this->point_markers->entityWidth());
        node_markers.insert(uuid, marker);
    };

    for (const HydraulicNodeJunction &junction : network.nodes_junctions)
        add_node(InfrastructureEntity::Junction, junction.uuid, junction.coordinate_wgs84);
    for (const HydraulicNodeReservoir &reservoir : network.nodes_reservoirs)
        add_node(InfrastructureEntity::Reservoir, reservoir.uuid, reservoir.coordinate_wgs84);
    for (const HydraulicNodeTank &tank : network.nodes_tanks)
        add_node(InfrastructureEntity::Tank, tank.uuid, tank.coordinate_wgs84);
    for (const HydraulicDemandPoint &demand_point : network.demand_points)
    {
        if (demand_point.uuid.isNull() || markerByUuid(demand_point.uuid).has_value())
        {
            qWarning() << "Cannot load demand point with invalid or duplicate UUID:"
                       << demand_point.uuid;
            continue;
        }

        InfrastructureEntityReference reference;
        reference.type = InfrastructureEntity::DemandPoint;
        reference.uuid = demand_point.uuid;
        this->point_markers->addMarker(
            reference, demand_point.coordinate_wgs84,
            this->point_markers->pixmapPathForEntity(reference.type),
            this->point_markers->entityWidth());
    }

    recalculateWrapReferenceLongitude();

    const std::function<std::optional<MapEntityMarker>(const QUuid &)> node_marker =
        [&node_markers](const QUuid &uuid) -> std::optional<MapEntityMarker>
    {
        const QHash<QUuid, MapEntityMarker>::const_iterator iterator = node_markers.constFind(uuid);
        if (iterator == node_markers.constEnd())
            return std::nullopt;
        return iterator.value();
    };

    for (const HydraulicLinkPipe &pipe : network.links_pipes)
    {
        const std::optional<MapEntityMarker> start_marker = node_marker(pipe.node_uuid_from);
        const std::optional<MapEntityMarker> end_marker = node_marker(pipe.node_uuid_to);
        if (!start_marker.has_value() || !end_marker.has_value())
        {
            qWarning() << "Cannot load pipe with missing endpoint node:" << pipe.uuid;
            continue;
        }

        QList<CoordinateWGS84> vertices;
        vertices.reserve(pipe.vertices.size());
        for (const HydraulicLinkVertex &vertex : pipe.vertices)
            vertices.append(vertex.coordinate_wgs84);

        InfrastructureEntityReference reference;
        reference.type = InfrastructureEntity::Pipe;
        reference.uuid = pipe.uuid;
        if (!this->pipes->addPipe(reference, start_marker->entity,
                                  end_marker->entity, vertices))
        {
            qWarning() << "Cannot load pipe:" << pipe.uuid;
        }
    }

    const std::function<void(InfrastructureEntity, const QUuid &, const QUuid &,
                             const QUuid &, const QList<HydraulicLinkVertex> &)> load_device_link =
        [this, &node_marker](InfrastructureEntity type, const QUuid &uuid,
                             const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                             const QList<HydraulicLinkVertex> &vertices)
    {
        const std::optional<MapEntityMarker> start_marker = node_marker(node_uuid_from);
        const std::optional<MapEntityMarker> end_marker = node_marker(node_uuid_to);
        if (!start_marker.has_value() || !end_marker.has_value())
        {
            qWarning() << "Cannot load device link with missing endpoint node:" << uuid;
            return;
        }

        DeviceLinkGeometry geometry;
        geometry.start_node = start_marker->entity;
        geometry.end_node = end_marker->entity;
        geometry.center_coordinate = vertices.isEmpty()
            ? midpoint(start_marker->coord_wgs84, end_marker->coord_wgs84)
            : vertices.first().coordinate_wgs84;

        InfrastructureEntityReference reference;
        reference.type = type;
        reference.uuid = uuid;
        if (!this->device_links->addDeviceLink(
                reference, geometry, this->point_markers->pixmapPathForEntity(type),
                this->point_markers->entityWidth()))
        {
            qWarning() << "Cannot load device link:" << uuid;
        }
    };

    for (const HydraulicLinkPump &pump : network.links_pumps)
    {
        load_device_link(InfrastructureEntity::Pump, pump.uuid,
                         pump.node_uuid_from, pump.node_uuid_to, pump.vertices);
    }
    for (const HydraulicLinkValve &valve : network.links_valves)
    {
        load_device_link(InfrastructureEntity::Valve, valve.uuid,
                         valve.node_uuid_from, valve.node_uuid_to, valve.vertices);
    }

    emit signalEntityMarkerSelected(false);
    updateCanvas();
}

void MapCanvasEntities::startEntityPositioning(InfrastructureEntity entity)
{
    stopEntityPositioning();
    const int width = InfrastructureEntityTraits::isHydraulicNetworkLink(entity)
        ? this->point_markers->entityWidth()
        : qMax(1, qRound(150.0 * this->point_markers->iconSizePercent() / 100.0));
    this->placement->startCreate(
        entity, this->point_markers->pixmapPathForEntity(entity), width);
}

void MapCanvasEntities::stopEntityPositioning()
{
    cancelDemandPointAttachmentInteraction();
    restoreMoveSnapshot();
    this->device_links->clearPlacement();
    this->pipes->clearPlacement();
    this->pipes->cancelPipeVertexMove();
    this->placement->stop();
    clearMoveSnapshot();
    updateCanvas();
}

bool MapCanvasEntities::cancelActiveMove()
{
    if (this->demand_point_attachment_selecting || this->demand_point_attachment_moving)
    {
        cancelDemandPointAttachmentInteraction();
        updateCanvas();
        return true;
    }
    if (!this->placement->isMoving())
        return false;
    stopEntityPositioning();
    return true;
}

bool MapCanvasEntities::positioningActive() const
{
    return !this->placement->isIdle() || this->demand_point_attachment_selecting ||
           this->demand_point_attachment_moving;
}

bool MapCanvasEntities::creatingPreviewActive() const
{
    return this->placement->isCreating();
}

bool MapCanvasEntities::creatingGlobeNode() const
{
    if (!this->placement->isCreating())
        return false;
    const InfrastructureEntity type = this->placement->entity();
    return type == InfrastructureEntity::Junction || type == InfrastructureEntity::Tank
        || type == InfrastructureEntity::Reservoir;
}

bool MapCanvasEntities::floatEntity(const QPointF &position)
{
    if (this->demand_point_attachment_moving)
    {
        std::optional<HydraulicDemandPoint> demand_point;
        if (this->hydraulic_data)
            demand_point = this->hydraulic_data->demandPoint(
                this->demand_point_attachment_interaction_uuid);
        if (!demand_point.has_value() ||
            demand_point->attachment.type != HydraulicDemandPointAttachmentType::Pipe)
        {
            cancelDemandPointAttachmentInteraction();
            updateCanvas();
            return true;
        }

        const MapCanvasPipes::PipePositionHit hit = this->pipes->nearestPositionOnPipe(
            demand_point->attachment.pipe_uuid, position, *this->point_markers);
        this->demand_point_attachment_preview_valid = hit.isValid();
        if (hit.isValid())
        {
            this->demand_point_attachment_preview_coordinate = hit.coordinate_wgs84;
        }
        updateCanvas();
        return true;
    }

    if (this->demand_point_attachment_selecting)
    {
        this->demand_point_attachment_preview_valid = false;

        const std::optional<InfrastructureEntityReference> marker =
            this->point_markers->markerAt(position);
        if (marker.has_value() && marker->type == InfrastructureEntity::Junction)
        {
            const std::optional<MapEntityMarker> junction_marker =
                this->point_markers->markerByUuid(marker->uuid);
            if (junction_marker.has_value())
            {
                this->demand_point_attachment_preview_coordinate =
                    junction_marker->coord_wgs84;
                this->demand_point_attachment_preview_valid = true;
            }
        }

        if (!this->demand_point_attachment_preview_valid)
        {
            const MapCanvasPipes::PipePositionHit pipe_hit =
                this->pipes->pipePositionAt(position, *this->point_markers);
            if (pipe_hit.isValid())
            {
                this->demand_point_attachment_preview_coordinate =
                    pipe_hit.coordinate_wgs84;
                this->demand_point_attachment_preview_valid = true;
            }
        }

        if (!this->demand_point_attachment_preview_valid &&
            this->map_model && this->map_canvas && !this->map_canvas->size().isEmpty())
        {
            const std::optional<CoordinateWGS84> coordinate =
                this->projection->coordinateFromScreen(position, this->map_canvas->size());
            if (coordinate.has_value())
            {
                this->demand_point_attachment_preview_coordinate = coordinate.value();
                this->demand_point_attachment_preview_valid = true;
            }
        }

        updateCanvas();
        return true;
    }

    if (this->placement->isMoving())
        this->placement->setMoveCursor(true);

    const std::optional<CoordinateWGS84> mouse_coordinate =
        this->projection->coordinateFromScreen(position, this->map_canvas->size());
    if (!mouse_coordinate.has_value())
    {
        if (!this->projection->supportsMercatorHitBox() && this->placement->isCreating())
            this->placement->invalidateMousePosition(position);
        updateCanvas();
        return true;
    }
    this->placement->updateMousePosition(position, mouse_coordinate.value());

    if (this->placement->movingSelected())
    {
        this->selection->moveSelected(this->placement->previousMouseCoordinate(),
                                      this->placement->mouseCoordinate(),
                                      this->move_translated_pipe_uuids);
        updateCanvas();
        return true;
    }

    updateConnectionTarget(position);

    if (this->placement->isMoving() && this->pipes->isPipeVertexMoveActive())
    {
        if (!this->pipes->updatePipeVertexMove(position))
            stopEntityPositioning();
        else
            updateCanvas();
        return true;
    }

    if (!this->placement->hasFloatingMarker() ||
        !this->placement->revealFloatingMarkerIfReady())
    {
        return false;
    }

    if (this->placement->isMoving())
    {
        const QUuid moving_uuid = this->placement->floatingUuid();
        bool moved = this->device_links->updateMove(moving_uuid, position);
        if (!moved)
        {
            const std::optional<CoordinateWGS84> coordinate =
                this->projection->coordinateFromScreen(position, this->map_canvas->size());
            if (!coordinate.has_value())
                return true;
            moved = this->point_markers->setCoordinate(moving_uuid, coordinate.value());
        }

        if (!moved)
        {
            stopEntityPositioning();
            return true;
        }
    }

    updateCanvas();
    return true;
}

bool MapCanvasEntities::anchorMarker(const QPointF &position, bool commit_to_model)
{
    // The shared placement path supports terrain-aware node creation on the
    // globe. Link completion and existing-entity movement remain guarded.
    if (!commit_to_model && !this->placement->isCreating())
        return false;
    if (commit_to_model && anchorPipeVertexMove(position))
        return true;

    if (!this->placement->hasFloatingMarker())
        return false;

    if (this->placement->movingSelected())
    {
        const std::optional<CoordinateWGS84> coordinate =
            this->projection->coordinateFromScreen(position, this->map_canvas->size());
        if (!coordinate.has_value())
            return true;
        const CoordinateWGS84 from_coordinate = this->placement->mouseCoordinateValid()
            ? this->placement->mouseCoordinate() : coordinate.value();
        this->selection->moveSelected(from_coordinate, coordinate.value(),
                                      this->move_translated_pipe_uuids);
        const bool synchronized = synchronizeSelectedGeometry();
        if (!synchronized)
            restoreMoveSnapshot();
        recalculateWrapReferenceLongitude();
        this->placement->completeMove();
        clearMoveSnapshot();
        updateCanvas();
        return true;
    }

    if (this->placement->isCreating())
    {
        if (InfrastructureEntityTraits::isHydraulicDeviceLink(this->placement->entity()))
            return anchorDeviceLink(position, commit_to_model);
        if (this->placement->entity() == InfrastructureEntity::Pipe)
            return anchorPipe(position, commit_to_model);
    }

    if (!commit_to_model)
        return true; // Floating junction/tank/reservoir/demand icons only.

    const std::optional<CoordinateWGS84> projected_coordinate =
        this->projection->coordinateFromScreen(position, this->map_canvas->size());
    if (!projected_coordinate.has_value())
        return true;
    const CoordinateWGS84 coordinate = projected_coordinate.value();

    if (this->placement->isMoving())
    {
        const QUuid moving_uuid = this->placement->floatingUuid();
        const bool moved = this->point_markers->setCoordinate(moving_uuid, coordinate) ||
                           this->device_links->setCenterCoordinate(moving_uuid, coordinate);
        const bool synchronized = moved && synchronizeMarkerCoordinate(moving_uuid);
        if (!synchronized)
            restoreMoveSnapshot();
        recalculateWrapReferenceLongitude();
        this->placement->completeMove();
        clearMoveSnapshot();
        updateCanvas();
        return synchronized;
    }

    if (!this->placement->isCreating())
        return false;

    InfrastructureEntityReference reference;
    reference.type = this->placement->entity();
    reference.uuid = createHydraulicPointEntity(reference.type, coordinate);
    if (reference.uuid.isNull())
    {
        // A missing globe DEM sample is not a completed creation. Keep the
        // shared placement tool armed; preserve 2D's original failure path.
        if (this->projection && !this->projection->supportsMercatorHitBox())
            return true;
        this->placement->stop();
        return false;
    }

    this->point_markers->addMarker(
        reference, coordinate, this->point_markers->pixmapPathForEntity(reference.type),
        this->point_markers->entityWidth());
    recalculateWrapReferenceLongitude();
    this->placement->setFloatingHiddenUntil(position.toPoint());
    this->placement->consumeCreatedMarker();
    this->placement->rearmCreate(
        this->point_markers->pixmapPathForEntity(reference.type),
        qMax(1, qRound(150.0 * this->point_markers->iconSizePercent() / 100.0)));
    updateCanvas();
    return true;
}

QUuid MapCanvasEntities::createHydraulicPointEntity(InfrastructureEntity entity,
                                             const CoordinateWGS84 &coordinate)
{
    if (!this->hydraulic_data)
        return QUuid();

    // In Globe mode these are the same hydraulic creation operations as 2D,
    // with a terrain sample supplied atomically at construction time.
    // Missing DEM data must never turn into a zero elevation.
    std::optional<double> terrain_elevation_m;
    if (this->projection && !this->projection->supportsMercatorHitBox()
        && (entity == InfrastructureEntity::Junction || entity == InfrastructureEntity::Tank
            || entity == InfrastructureEntity::Reservoir))
    {
        if (!this->terrain_elevation_resolver)
            return QUuid();
        terrain_elevation_m = this->terrain_elevation_resolver(coordinate);
        if (!terrain_elevation_m.has_value() || !std::isfinite(terrain_elevation_m.value()))
            return QUuid();
    }

    switch (entity)
    {
    case InfrastructureEntity::Junction:
        return this->hydraulic_data->addJunction(coordinate, terrain_elevation_m);
    case InfrastructureEntity::Reservoir:
        return this->hydraulic_data->addReservoir(coordinate, terrain_elevation_m);
    case InfrastructureEntity::Tank:
        return this->hydraulic_data->addTank(coordinate, terrain_elevation_m);
    case InfrastructureEntity::DemandPoint:
        return this->hydraulic_data->addDemandPoint(coordinate);
    default:
        return QUuid();
    }
}

bool MapCanvasEntities::anchorDeviceLink(const QPointF &position, bool commit_to_model)
{
    // The start-node choice is shared preview state. Completion would add
    // a hydraulic device, so the globe cannot do it yet.
    if (!commit_to_model && this->device_links->hasStartNode())
        return true;
    const InfrastructureEntity entity = this->placement->entity();
    InfrastructureEntityReference reference;
    reference.type = entity;

    if (this->device_links->hasStartNode())
    {
        const std::optional<DeviceLinkGeometry> geometry =
            this->device_links->completionGeometry(
                this->placement->connectionTargetUuid(), *this->point_markers);
        if (geometry.has_value())
        {
            reference.uuid = createHydraulicDeviceLink(entity, geometry.value());
            if (reference.uuid.isNull())
                return true;
        }
    }

    const MapCanvasDeviceLinks::AnchorResult result = this->device_links->anchor(
        reference, this->placement->connectionTargetUuid(), *this->point_markers,
        this->point_markers->pixmapPathForEntity(entity),
        this->point_markers->entityWidth());

    if (result.status == MapCanvasDeviceLinks::AnchorStatus::StartSet)
    {
        this->placement->clearConnectionTarget();
        updateConnectionTarget(position);
        updateCanvas();
        return true;
    }

    if (result.status != MapCanvasDeviceLinks::AnchorStatus::Completed)
    {
        if (!reference.uuid.isNull())
            deleteHydraulicLink(reference);
        return true;
    }

    this->placement->setFloatingHiddenUntil(position.toPoint());
    this->placement->consumeCreatedMarker();
    this->placement->clearConnectionTarget();
    this->placement->rearmCreate(
        this->point_markers->pixmapPathForEntity(entity),
        this->point_markers->entityWidth());
    updateCanvas();
    return true;
}

QUuid MapCanvasEntities::createHydraulicDeviceLink(
    InfrastructureEntity entity, const DeviceLinkGeometry &geometry)
{
    if (!this->hydraulic_data)
        return QUuid();

    switch (entity)
    {
    case InfrastructureEntity::Pump:
        return this->hydraulic_data->addPump(
            geometry.start_node.uuid, geometry.end_node.uuid, geometry.center_coordinate);
    case InfrastructureEntity::Valve:
        return this->hydraulic_data->addValve(
            geometry.start_node.uuid, geometry.end_node.uuid, geometry.center_coordinate);
    default:
        return QUuid();
    }
}

bool MapCanvasEntities::synchronizeMarkerCoordinate(const QUuid &uuid)
{
    if (!this->hydraulic_data || uuid.isNull())
        return false;

    const std::optional<MapEntityMarker> marker = markerByUuid(uuid);
    if (!marker.has_value())
        return false;

    HydraulicGeometryBatch batch;
    switch (marker->entity.type)
    {
    case InfrastructureEntity::Junction:
    case InfrastructureEntity::Reservoir:
    case InfrastructureEntity::Tank:
    {
        batch.node_coordinates.insert(marker->entity.uuid, marker->coord_wgs84);
        if (this->projection && !this->projection->supportsMercatorHitBox())
        {
            // The existing hydraulic node retains its elevation input mode.
            // A terrain-relative node changes only the DEM component; an
            // absolute node changes coordinates without touching its height.
            bool terrain_relative = false;
            if (marker->entity.type == InfrastructureEntity::Junction)
            {
                const std::optional<HydraulicNodeJunction> node =
                    this->hydraulic_data->junction(marker->entity.uuid);
                if (!node.has_value())
                    return false;
                terrain_relative = node->elevation_input_type ==
                    HydraulicNodeElevationInputType::TerrainElevationAndOffset;
            }
            else if (marker->entity.type == InfrastructureEntity::Reservoir)
            {
                const std::optional<HydraulicNodeReservoir> node =
                    this->hydraulic_data->reservoir(marker->entity.uuid);
                if (!node.has_value())
                    return false;
                terrain_relative = node->head_input_type ==
                    HydraulicNodeElevationInputType::TerrainElevationAndOffset;
            }
            else
            {
                const std::optional<HydraulicNodeTank> node =
                    this->hydraulic_data->tank(marker->entity.uuid);
                if (!node.has_value())
                    return false;
                terrain_relative = node->elevation_input_type ==
                    HydraulicNodeTankElevationInputType::TerrainElevationAndOffset;
            }
            if (terrain_relative)
            {
                if (!this->terrain_elevation_resolver)
                    return false;
                const std::optional<double> elevation =
                    this->terrain_elevation_resolver(marker->coord_wgs84);
                if (!elevation.has_value() || !std::isfinite(elevation.value()))
                    return false;
                batch.node_terrain_elevations_m.insert(marker->entity.uuid, elevation.value());
            }
        }
        break;
    }
    case InfrastructureEntity::DemandPoint:
        batch.demand_point_coordinates.insert(marker->entity.uuid, marker->coord_wgs84);
        break;
    case InfrastructureEntity::Pump:
        batch.pump_center_coordinates.insert(marker->entity.uuid, marker->coord_wgs84);
        break;
    case InfrastructureEntity::Valve:
        batch.valve_center_coordinates.insert(marker->entity.uuid, marker->coord_wgs84);
        break;
    default:
        return false;
    }

    const QScopedValueRollback<bool> synchronization_guard(
        this->synchronizing_geometry, true);
    return this->hydraulic_data->applyGeometryBatch(batch);
}

bool MapCanvasEntities::synchronizeSelectedGeometry()
{
    if (!this->hydraulic_data)
        return false;

    HydraulicGeometryBatch batch;
    for (const MapEntityMarker &original_marker : this->move_marker_snapshot)
    {
        std::optional<MapEntityMarker> marker = this->point_markers->markerByUuid(
            original_marker.entity.uuid);
        if (!marker.has_value())
            marker = this->device_links->markerByUuid(original_marker.entity.uuid);
        if (!marker.has_value())
            return false;

        if (InfrastructureEntityTraits::isHydraulicConnectionNode(marker->entity.type))
            batch.node_coordinates.insert(marker->entity.uuid, marker->coord_wgs84);
        else if (marker->entity.type == InfrastructureEntity::DemandPoint)
            batch.demand_point_coordinates.insert(marker->entity.uuid, marker->coord_wgs84);
        else if (marker->entity.type == InfrastructureEntity::Pump)
            batch.pump_center_coordinates.insert(marker->entity.uuid, marker->coord_wgs84);
        else if (marker->entity.type == InfrastructureEntity::Valve)
            batch.valve_center_coordinates.insert(marker->entity.uuid, marker->coord_wgs84);
    }

    for (const QUuid &pipe_uuid : this->move_translated_pipe_uuids)
    {
        batch.pipe_vertices.insert(
            pipe_uuid, this->pipes->intermediateVertices(pipe_uuid));
    }

    const QScopedValueRollback<bool> synchronization_guard(
        this->synchronizing_geometry, true);
    return this->hydraulic_data->applyGeometryBatch(batch);
}

void MapCanvasEntities::captureMarkerMoveSnapshot(const QUuid &uuid)
{
    clearMoveSnapshot();
    const std::optional<MapEntityMarker> marker = markerByUuid(uuid);
    if (marker.has_value())
        this->move_marker_snapshot.append(marker.value());
}

void MapCanvasEntities::captureSelectedMoveSnapshot()
{
    clearMoveSnapshot();
    for (const QUuid &uuid : this->selection->selectedMarkerUuids())
    {
        const std::optional<MapEntityMarker> marker = markerByUuid(uuid);
        if (marker.has_value())
            this->move_marker_snapshot.append(marker.value());
    }

    for (const QUuid &pipe_uuid : this->pipes->selectedPipeUuids())
    {
        this->move_pipe_vertices_snapshot.insert(
            pipe_uuid, this->pipes->intermediateVertices(pipe_uuid));
    }
}

void MapCanvasEntities::capturePipeMoveSnapshot(const QUuid &pipe_uuid)
{
    clearMoveSnapshot();
    this->move_pipe_vertices_snapshot.insert(
        pipe_uuid, this->pipes->intermediateVertices(pipe_uuid));
}

void MapCanvasEntities::prepareMoveVisualState()
{
    this->move_dynamic_pipe_uuids.clear();
    this->move_dynamic_device_link_uuids.clear();
    this->move_translated_pipe_uuids.clear();

    QSet<QUuid> moved_node_uuids;
    for (const MapEntityMarker &marker : this->move_marker_snapshot)
    {
        if (InfrastructureEntityTraits::isHydraulicConnectionNode(marker.entity.type))
            moved_node_uuids.insert(marker.entity.uuid);
        else if (InfrastructureEntityTraits::isHydraulicDeviceLink(marker.entity.type))
            this->move_dynamic_device_link_uuids.append(marker.entity.uuid);
    }

    if (!moved_node_uuids.isEmpty())
    {
        this->move_dynamic_pipe_uuids = this->pipes->connectedPipeUuids(moved_node_uuids);
        const QList<QUuid> connected_device_links =
            this->device_links->connectedLinkUuids(moved_node_uuids);
        for (const QUuid &uuid : connected_device_links)
        {
            if (!this->move_dynamic_device_link_uuids.contains(uuid))
                this->move_dynamic_device_link_uuids.append(uuid);
        }
    }

    if (this->pipes->isPipeVertexMoveActive())
    {
        const std::optional<QUuid> pipe_uuid = this->pipes->activePipeVertexMoveUuid();
        if (pipe_uuid.has_value() && !this->move_dynamic_pipe_uuids.contains(pipe_uuid.value()))
            this->move_dynamic_pipe_uuids.append(pipe_uuid.value());
    }

    if (this->placement->movingSelected())
        this->move_translated_pipe_uuids = this->pipes->selectedPipeUuids();

    ++this->move_session_id;
}

void MapCanvasEntities::restoreMoveSnapshot()
{
    for (const MapEntityMarker &marker : this->move_marker_snapshot)
    {
        if (InfrastructureEntityTraits::isHydraulicConnectionNode(marker.entity.type) ||
            marker.entity.type == InfrastructureEntity::DemandPoint)
            this->point_markers->setCoordinate(marker.entity.uuid, marker.coord_wgs84);
        else if (InfrastructureEntityTraits::isHydraulicDeviceLink(marker.entity.type))
            this->device_links->setCenterCoordinate(marker.entity.uuid, marker.coord_wgs84);
    }

    auto iterator = this->move_pipe_vertices_snapshot.constBegin();
    while (iterator != this->move_pipe_vertices_snapshot.constEnd())
    {
        this->pipes->setIntermediateVertices(iterator.key(), iterator.value());
        ++iterator;
    }
}

void MapCanvasEntities::clearMoveSnapshot()
{
    this->move_marker_snapshot.clear();
    this->move_pipe_vertices_snapshot.clear();
    this->move_dynamic_pipe_uuids.clear();
    this->move_dynamic_device_link_uuids.clear();
    this->move_translated_pipe_uuids.clear();
}

bool MapCanvasEntities::deleteHydraulicLink(const InfrastructureEntityReference &reference)
{
    if (!this->hydraulic_data || reference.uuid.isNull())
        return false;

    switch (reference.type)
    {
    case InfrastructureEntity::Pipe:
        return this->hydraulic_data->deletePipe(reference.uuid);
    case InfrastructureEntity::Pump:
        return this->hydraulic_data->deletePump(reference.uuid);
    case InfrastructureEntity::Valve:
        return this->hydraulic_data->deleteValve(reference.uuid);
    default:
        return false;
    }
}

bool MapCanvasEntities::deleteHydraulicNode(const InfrastructureEntityReference &reference)
{
    if (!this->hydraulic_data || reference.uuid.isNull())
        return false;

    switch (reference.type)
    {
    case InfrastructureEntity::Junction:
        return this->hydraulic_data->deleteJunction(reference.uuid);
    case InfrastructureEntity::Reservoir:
        return this->hydraulic_data->deleteReservoir(reference.uuid);
    case InfrastructureEntity::Tank:
        return this->hydraulic_data->deleteTank(reference.uuid);
    default:
        return false;
    }
}

void MapCanvasEntities::addPipeVertex(const QUuid &pipe_uuid, int insert_index,
                                      const CoordinateWGS84 &coordinate)
{
    const QList<CoordinateWGS84> previous_vertices =
        this->pipes->intermediateVertices(pipe_uuid);
    if (!this->pipes->addPipeVertex(pipe_uuid, insert_index, coordinate))
        return;

    if (!this->hydraulic_data ||
        !this->hydraulic_data->setPipeVertices(
            pipe_uuid, this->pipes->intermediateVertices(pipe_uuid)))
    {
        this->pipes->setIntermediateVertices(pipe_uuid, previous_vertices);
    }
}

void MapCanvasEntities::deletePipeVertex(const QUuid &pipe_uuid, int vertex_index)
{
    const QList<CoordinateWGS84> previous_vertices =
        this->pipes->intermediateVertices(pipe_uuid);
    if (!this->pipes->deletePipeVertex(pipe_uuid, vertex_index))
        return;

    if (!this->hydraulic_data ||
        !this->hydraulic_data->setPipeVertices(
            pipe_uuid, this->pipes->intermediateVertices(pipe_uuid)))
    {
        this->pipes->setIntermediateVertices(pipe_uuid, previous_vertices);
    }
}

bool MapCanvasEntities::anchorPipe(const QPointF &position, bool commit_to_model)
{
    const QUuid connection_target_uuid = this->placement->connectionTargetUuid();
    if (!this->pipes->hasStartNode())
    {
        if (connection_target_uuid.isNull())
            return true;

        const std::optional<MapEntityMarker> start_marker = markerByUuid(connection_target_uuid);
        if (!start_marker.has_value() ||
            !InfrastructureEntityTraits::isHydraulicConnectionNode(start_marker->entity.type))
            return true;

        this->pipes->startPipe(connection_target_uuid);
        this->placement->clearConnectionTarget();
        updateCanvas();
        return true;
    }

    if (!connection_target_uuid.isNull())
    {
        if (!commit_to_model)
            return true; // Show the snapped completion preview; do not add a pipe.
        if (connection_target_uuid == this->pipes->startNodeUuid())
            return true;

        const std::optional<MapEntityMarker> start_marker = markerByUuid(
            this->pipes->startNodeUuid());
        const std::optional<MapEntityMarker> end_marker = markerByUuid(connection_target_uuid);
        if (!start_marker.has_value() || !end_marker.has_value() ||
            !InfrastructureEntityTraits::isHydraulicConnectionNode(start_marker->entity.type) ||
            !InfrastructureEntityTraits::isHydraulicConnectionNode(end_marker->entity.type))
        {
            return true;
        }

        InfrastructureEntityReference pipe_reference;
        pipe_reference.type = InfrastructureEntity::Pipe;
        if (this->hydraulic_data)
        {
            pipe_reference.uuid = this->hydraulic_data->addPipe(
                start_marker->entity.uuid, end_marker->entity.uuid,
                this->pipes->intermediateVertices());
        }
        if (pipe_reference.uuid.isNull())
            return true;

        if (!this->pipes->completePipe(
                pipe_reference, start_marker->entity, end_marker->entity))
        {
            deleteHydraulicLink(pipe_reference);
            return true;
        }

        this->placement->setFloatingHiddenUntil(position.toPoint());
        this->placement->consumeCreatedMarker();
        this->placement->clearConnectionTarget();
        this->placement->rearmCreate(
            this->point_markers->pixmapPathForEntity(InfrastructureEntity::Pipe),
            this->point_markers->entityWidth());
        updateCanvas();
        return true;
    }

    const std::optional<CoordinateWGS84> intermediate_vertex =
        this->projection->coordinateFromScreen(position, this->map_canvas->size());
    if (!intermediate_vertex.has_value())
        return true;
    this->pipes->appendIntermediateVertex(intermediate_vertex.value());
    updateCanvas();
    return true;
}

bool MapCanvasEntities::anchorPipeVertexMove(const QPointF &position)
{
    if (!this->placement->isMoving() || !this->pipes->isPipeVertexMoveActive())
        return false;

    const std::optional<QUuid> pipe_uuid = this->pipes->activePipeVertexMoveUuid();
    const bool moved = this->pipes->finishPipeVertexMove(position);
    bool synchronized = false;
    if (moved && pipe_uuid.has_value() && this->hydraulic_data)
    {
        HydraulicGeometryBatch batch;
        batch.pipe_vertices.insert(
            pipe_uuid.value(), this->pipes->intermediateVertices(pipe_uuid.value()));
        const QScopedValueRollback<bool> synchronization_guard(
            this->synchronizing_geometry, true);
        synchronized = this->hydraulic_data->applyGeometryBatch(batch);
    }

    if (!synchronized)
        restoreMoveSnapshot();

    this->placement->completeMove();
    clearMoveSnapshot();
    updateCanvas();
    return true;
}

void MapCanvasEntities::setWrapReferenceLongitude(double longitude)
{
    const double normalized_longitude = GeoWebMercator::normalizeLongitude(longitude);
    this->wrap_reference_longitude = normalized_longitude;
    this->point_markers->setWrapReferenceLongitude(normalized_longitude);
    this->device_links->setWrapReferenceLongitude(normalized_longitude);
    this->pipes->setWrapReferenceLongitude(normalized_longitude);
}

void MapCanvasEntities::recalculateWrapReferenceLongitude()
{
    const QList<MapEntityMarker> &markers = this->point_markers->markers();
    if (markers.isEmpty())
    {
        setWrapReferenceLongitude(this->map_model->centerLon());
        return;
    }

    double longitude_sin_sum = 0.0;
    double longitude_cos_sum = 0.0;
    for (const MapEntityMarker &marker : markers)
    {
        const double longitude_radians = qDegreesToRadians(
            GeoWebMercator::normalizeLongitude(marker.coord_wgs84.longitude_deg));
        longitude_sin_sum += std::sin(longitude_radians);
        longitude_cos_sum += std::cos(longitude_radians);
    }

    double reference_longitude = markers.first().coord_wgs84.longitude_deg;
    if (std::hypot(longitude_sin_sum, longitude_cos_sum) > 1e-9)
    {
        reference_longitude = qRadiansToDegrees(
            std::atan2(longitude_sin_sum, longitude_cos_sum));
    }
    setWrapReferenceLongitude(reference_longitude);
}

void MapCanvasEntities::setIconSizePercent(int size_percent)
{
    this->point_markers->setIconSizePercent(size_percent);
    scaleMarkers();
}

void MapCanvasEntities::updateMarkerScale()
{
    if (this->point_markers->entityWidth() == this->applied_marker_width)
        return;
    scaleMarkers();
}

void MapCanvasEntities::scaleMarkers()
{
    const int width = this->point_markers->entityWidth();
    this->applied_marker_width = width;
    this->point_markers->scaleMarkers(width);
    this->device_links->scaleMarkers(width);

    if (this->placement->hasFloatingMarker())
    {
        int floating_width = width;
        if (this->placement->isCreating() &&
            !InfrastructureEntityTraits::isHydraulicNetworkLink(this->placement->entity()))
            floating_width = qMax(1, qRound(150.0 * this->point_markers->iconSizePercent() / 100.0));
        this->placement->scaleFloatingMarker(
            this->point_markers->pixmapPathForEntity(this->placement->entity()), floating_width);
    }
    updateCanvas();
}

bool MapCanvasEntities::isMoveActive() const
{
    return this->placement->isMoving();
}

void MapCanvasEntities::positionMarkers()
{
    repaintCanvas();
}

int MapCanvasEntities::iconSizePercent() const
{
    return this->point_markers->iconSizePercent();
}

MapEditorVisualState MapCanvasEntities::visualState() const
{
    MapEditorVisualState state;
    state.revision = this->visual_state_revision;
    state.selected_marker_uuids = this->selection->selectedMarkerUuids();
    state.selected_pipe_uuids = this->pipes->selectedPipeUuids();
    if (this->hydraulic_data)
    {
        state.simulation_error_entities = this->hydraulic_data->simulationErrorEntities();
        state.simulation_stale_diagnostic_entity_uuids =
            this->hydraulic_data->simulationStaleDiagnosticEntityUuids();
    }
    state.wrap_reference_longitude = this->wrap_reference_longitude;
    state.entity_width = this->point_markers->entityWidth();

    state.placement.creating = this->placement->isCreating();
    state.placement.floating_marker_visible =
        this->placement->isCreating() && this->placement->floatingMarkerVisible()
        && (this->projection->supportsMercatorHitBox()
            || this->placement->mouseCoordinateValid());
    state.placement.entity = this->placement->entity();
    state.placement.connection_target_uuid = this->placement->connectionTargetUuid();
    state.placement.pipe_start_node_uuid = this->pipes->startNodeUuid();
    state.placement.pipe_intermediate_vertices = this->pipes->intermediateVertices();
    state.placement.device_link_start_node_uuid = this->device_links->startNodeUuid();
    state.placement.floating_width = this->placement->floatingWidth();
    state.placement.mouse_position = this->placement->mousePosition();
    if (this->map_model && this->map_canvas && !this->map_canvas->size().isEmpty())
    {
        const std::optional<CoordinateWGS84> coordinate = this->projection->coordinateFromScreen(
            this->placement->mousePosition(), this->map_canvas->size());
        if (coordinate.has_value())
        {
            state.placement.mouse_coordinate_wgs84 = coordinate.value();
            state.placement.mouse_coordinate_wgs84_valid = true;
        }
    }

    state.move.active = this->placement->isMoving();
    state.move.session_id = this->move_session_id;
    if (state.move.active)
    {
        state.move.markers.reserve(this->move_marker_snapshot.size());
        for (const MapEntityMarker &original_marker : this->move_marker_snapshot)
        {
            std::optional<MapEntityMarker> marker = this->point_markers->markerByUuid(
                original_marker.entity.uuid);
            if (!marker.has_value())
                marker = this->device_links->markerByUuid(original_marker.entity.uuid);
            if (!marker.has_value())
                continue;

            MapEditorDynamicMarkerVisualState dynamic_marker;
            dynamic_marker.entity = marker->entity.type;
            dynamic_marker.uuid = marker->entity.uuid;
            dynamic_marker.coordinate_wgs84 = marker->coord_wgs84;
            dynamic_marker.pixmap_path = marker->path_pixmap;
            state.move.markers.append(dynamic_marker);
        }

        state.move.links.reserve(this->move_dynamic_pipe_uuids.size() +
                                 this->move_dynamic_device_link_uuids.size());
        for (const QUuid &pipe_uuid : this->move_dynamic_pipe_uuids)
        {
            const std::optional<PipeGeometry> geometry = this->pipes->geometryByUuid(pipe_uuid);
            if (!geometry.has_value())
                continue;
            const std::optional<MapEntityMarker> start_marker =
                this->point_markers->markerByUuid(geometry->start_node.uuid);
            const std::optional<MapEntityMarker> end_marker =
                this->point_markers->markerByUuid(geometry->end_node.uuid);
            if (!start_marker.has_value() || !end_marker.has_value())
                continue;

            MapEditorDynamicLinkVisualState dynamic_link;
            dynamic_link.entity = InfrastructureEntity::Pipe;
            dynamic_link.uuid = pipe_uuid;
            dynamic_link.vertices_wgs84.reserve(geometry->intermediate_vertices.size() + 2);
            dynamic_link.vertices_wgs84.append(start_marker->coord_wgs84);
            dynamic_link.vertices_wgs84.append(geometry->intermediate_vertices);
            dynamic_link.vertices_wgs84.append(end_marker->coord_wgs84);
            state.move.links.append(dynamic_link);
        }

        for (const QUuid &link_uuid : this->move_dynamic_device_link_uuids)
        {
            const std::optional<DeviceLinkGeometry> geometry =
                this->device_links->geometryByUuid(link_uuid);
            const std::optional<MapEntityMarker> marker =
                this->device_links->markerByUuid(link_uuid);
            if (!geometry.has_value() || !marker.has_value())
                continue;
            const std::optional<MapEntityMarker> start_marker =
                this->point_markers->markerByUuid(geometry->start_node.uuid);
            const std::optional<MapEntityMarker> end_marker =
                this->point_markers->markerByUuid(geometry->end_node.uuid);
            if (!start_marker.has_value() || !end_marker.has_value())
                continue;

            MapEditorDynamicLinkVisualState dynamic_link;
            dynamic_link.entity = marker->entity.type;
            dynamic_link.uuid = link_uuid;
            dynamic_link.vertices_wgs84 = {
                start_marker->coord_wgs84, geometry->center_coordinate, end_marker->coord_wgs84};
            state.move.links.append(dynamic_link);
        }
    }

    if (this->hydraulic_data)
    {
        const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
        state.demand_point_attachments.reserve(network.demand_points.size());
        // Hashed once: a list lookup per demand point is quadratic when many
        // markers are selected (rectangle selection of a whole area).
        const QSet<QUuid> selected_marker_set(state.selected_marker_uuids.cbegin(),
                                              state.selected_marker_uuids.cend());
        for (const HydraulicDemandPoint &demand_point : network.demand_points)
        {
            // Demand points are point markers: the uuid index in place, no
            // marker copy (this loop runs on every editor state change).
            const MapEntityMarker *marker = this->point_markers->findMarker(demand_point.uuid);
            if (marker == nullptr)
                continue;

            const bool interacting =
                this->demand_point_attachment_interaction_uuid == demand_point.uuid &&
                (this->demand_point_attachment_selecting ||
                 this->demand_point_attachment_moving);
            const bool has_attachment =
                demand_point.attachment.type != HydraulicDemandPointAttachmentType::None;
            if (!has_attachment && !interacting)
                continue;

            MapEditorDemandPointAttachmentVisualState attachment_state;
            attachment_state.visible = true;
            attachment_state.selected = selected_marker_set.contains(demand_point.uuid);
            attachment_state.demand_point_uuid = demand_point.uuid;
            attachment_state.demand_point_coordinate_wgs84 = marker->coord_wgs84;
            attachment_state.selecting_target =
                interacting && this->demand_point_attachment_selecting;
            attachment_state.moving =
                interacting && this->demand_point_attachment_moving;
            attachment_state.attachment_is_pipe =
                demand_point.attachment.type == HydraulicDemandPointAttachmentType::Pipe;

            if (interacting && this->demand_point_attachment_preview_valid)
            {
                attachment_state.attachment_coordinate_wgs84 =
                    this->demand_point_attachment_preview_coordinate;
                attachment_state.attachment_coordinate_valid = true;
            }
            else
            {
                const std::optional<CoordinateWGS84> attachment_coordinate =
                    demandPointAttachmentCoordinate(demand_point);
                if (attachment_coordinate.has_value())
                {
                    attachment_state.attachment_coordinate_wgs84 =
                        attachment_coordinate.value();
                    attachment_state.attachment_coordinate_valid = true;
                }
            }

            state.demand_point_attachments.append(attachment_state);
        }
    }

    return state;
}

bool MapCanvasEntities::selectEntityAt(const QPointF &position)
{
    if (!this->placement->isIdle() || this->demand_point_attachment_selecting
        || this->demand_point_attachment_moving)
        return false;

    if (this->projection && !this->projection->supportsMercatorHitBox())
    {
        if (!this->map_canvas)
            return false;
        const std::optional<InfrastructureEntityReference> hit =
            this->projection->hitEntity(position, this->map_canvas->size());
        if (!hit.has_value())
            return false;
        if (hit->type == InfrastructureEntity::Pipe)
        {
            if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
            {
                if (!this->pipes->geometryByUuid(hit->uuid).has_value())
                    return false;
                this->selection->togglePipe(hit->uuid);
                emit signalEntityMarkerSelected(this->selection->hasSelection());
                updateCanvas();
            }
            else
            {
                if (!this->pipes->geometryByUuid(hit->uuid).has_value())
                    return false;
                selectPipe(hit->uuid);
            }
            return true;
        }
        const std::optional<MapEntityMarker> marker = markerByUuid(hit->uuid);
        if (!marker.has_value() || marker->entity.type != hit->type)
            return false;
        selectMarker(hit->uuid);
        return true;
    }
    return selectMarkerAt(position) || selectDeviceLinkAt(position) || selectPipeAt(position);
}

bool MapCanvasEntities::isEntityAt(const QPointF &position) const
{
    if (!this->placement->isIdle() || this->demand_point_attachment_selecting
        || this->demand_point_attachment_moving)
        return false;
    if (this->projection && !this->projection->supportsMercatorHitBox())
    {
        if (!this->map_canvas)
            return false;
        const std::optional<InfrastructureEntityReference> hit =
            this->projection->hitEntity(position, this->map_canvas->size());
        if (!hit.has_value())
            return false;
        if (hit->type == InfrastructureEntity::Pipe)
            return this->pipes->geometryByUuid(hit->uuid).has_value();
        const std::optional<MapEntityMarker> marker = markerByUuid(hit->uuid);
        return marker.has_value() && marker->entity.type == hit->type;
    }
    return isMarkerAt(position) || isDeviceLinkAt(position) || isPipeAt(position);
}

bool MapCanvasEntities::selectMarkerAt(const QPointF &position)
{
    if (!this->placement->isIdle() || this->demand_point_attachment_selecting ||
        this->demand_point_attachment_moving)
        return false;

    std::optional<InfrastructureEntityReference> entity = this->point_markers->markerAt(position);
    if (!entity.has_value())
        entity = this->device_links->markerAt(position);
    if (!entity.has_value())
        return false;

    selectMarker(entity->uuid);
    return true;
}

bool MapCanvasEntities::isMarkerAt(const QPointF &position) const
{
    if (!this->placement->isIdle() || this->demand_point_attachment_selecting ||
        this->demand_point_attachment_moving)
        return false;
    return this->point_markers->isMarkerAt(position) ||
           this->device_links->markerAt(position).has_value();
}

bool MapCanvasEntities::showMarkerContextMenuAt(const QPointF &position,
                                                const QPoint &global_position)
{
    if (!this->placement->isIdle() || this->demand_point_attachment_selecting ||
        this->demand_point_attachment_moving)
        return false;

    std::optional<InfrastructureEntityReference> entity;
    if (this->projection && !this->projection->supportsMercatorHitBox())
    {
        if (!this->map_canvas)
            return false;
        entity = this->projection->hitEntity(position, this->map_canvas->size());
        // 3D-00D2B: only node moves have terrain-safe commit rules so far.
        if (!entity.has_value() || !InfrastructureEntityTraits::isHydraulicConnectionNode(entity->type))
            return false;
    }
    else
    {
        entity = this->point_markers->markerAt(position);
        if (!entity.has_value())
            entity = this->device_links->markerAt(position);
    }
    if (!entity.has_value() || !markerByUuid(entity->uuid).has_value())
        return false;

    showMarkerContextMenu(entity->uuid, global_position, position);
    return true;
}

bool MapCanvasEntities::selectDeviceLinkAt(const QPointF &position)
{
    if (!this->placement->isIdle() || this->demand_point_attachment_selecting ||
        this->demand_point_attachment_moving)
        return false;

    const std::optional<InfrastructureEntityReference> device_link =
        this->device_links->linkAt(position, *this->point_markers);
    if (!device_link.has_value())
        return false;

    selectMarker(device_link->uuid);
    return true;
}

bool MapCanvasEntities::isDeviceLinkAt(const QPointF &position) const
{
    return this->placement->isIdle() && !this->demand_point_attachment_selecting &&
           !this->demand_point_attachment_moving &&
           this->device_links->linkAt(position, *this->point_markers).has_value();
}

bool MapCanvasEntities::showPipeContextMenuAt(const QPointF &position,
                                              const QPoint &global_position)
{
    return this->placement->isIdle() && !this->demand_point_attachment_selecting &&
           !this->demand_point_attachment_moving &&
           this->pipes->showContextMenuAt(
               position, global_position, *this->point_markers);
}

void MapCanvasEntities::selectMarker(const QUuid &uuid)
{
    const std::optional<MapEntityMarker> marker = markerByUuid(uuid);
    if (!marker.has_value())
        return;

    if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
    {
        this->selection->toggleMarker(marker.value());
        emit signalEntityMarkerSelected(this->selection->hasSelection());
        updateCanvas();
        return;
    }

    this->selection->replaceWithMarker(marker.value());
    emit signalEntityMarkerSelected(true);
    if (this->hydraulic_data)
        this->hydraulic_data->setSelectedUuid(marker->entity.type, marker->entity.uuid);
    updateCanvas();
}

void MapCanvasEntities::applyExternalSelection(
    InfrastructureEntity entity_type, const QUuid &uuid)
{
    if (uuid.isNull())
        return;

    if (entity_type == InfrastructureEntity::Pipe)
    {
        const std::optional<InfrastructureEntityReference> selected_pipe =
            this->selection->replaceWithPipe(uuid);
        if (!selected_pipe.has_value())
            return;
    }
    else
    {
        const std::optional<MapEntityMarker> marker = markerByUuid(uuid);
        if (!marker.has_value() || marker->entity.type != entity_type)
            return;
        this->selection->replaceWithMarker(marker.value());
    }

    emit signalEntityMarkerSelected(true);
    updateCanvas();
}

void MapCanvasEntities::startMarkerMove(const QUuid &uuid,
                                        std::optional<QPointF> pointer_position)
{
    const std::optional<MapEntityMarker> marker = markerByUuid(uuid);
    if (!marker.has_value())
        return;
    if (this->projection && !this->projection->supportsMercatorHitBox() &&
        !InfrastructureEntityTraits::isHydraulicConnectionNode(marker->entity.type))
        return;

    stopEntityPositioning();
    const QPointF mouse_position = pointer_position.has_value()
        ? pointer_position.value() : QPointF(this->map_canvas->mapFromGlobal(QCursor::pos()));
    std::optional<CoordinateWGS84> mouse_coordinate =
        this->projection->coordinateFromScreen(mouse_position, this->map_canvas->size());
    if (!mouse_coordinate.has_value() && this->projection &&
        !this->projection->supportsMercatorHitBox())
    {
        // A sprite can be picked through terrain even where the terrain
        // ray misses. Its actual model coordinate is the safe start anchor.
        mouse_coordinate = marker->coord_wgs84;
    }
    if (!mouse_coordinate.has_value())
        return;
    captureMarkerMoveSnapshot(uuid);
    if (!this->placement->startMove(
            marker->entity.type, uuid, marker->path_pixmap,
            this->point_markers->entityWidth(), mouse_position, mouse_coordinate.value()))
    {
        clearMoveSnapshot();
    }
    else
    {
        prepareMoveVisualState();
    }
    updateCanvas();
}

void MapCanvasEntities::startSelectedMarkerMove(const QUuid &uuid)
{
    // Group moves require per-node terrain samples; do not use the 2D batch
    // commit path on the globe until those samples are supplied together.
    if (this->projection && !this->projection->supportsMercatorHitBox())
        return;
    if (!this->selection->isMarkerSelected(uuid) ||
        this->selection->selectedMarkerCount() < 2)
    {
        return;
    }

    startMarkerMove(uuid);
    if (!this->placement->isMoving() || this->placement->floatingUuid() != uuid)
        return;

    this->placement->setMovingSelected(true);
    this->pipes->selectPipesWithSelectedEndpoints(
        this->selection->selectedMarkerUuids());
    captureSelectedMoveSnapshot();
    prepareMoveVisualState();
    updateCanvas();
}

void MapCanvasEntities::onMarkerSelectedDeleteRequested()
{
    const QList<QUuid> marker_uuids_to_delete = this->selection->selectedMarkerUuids();
    const QList<QUuid> pipe_uuids_to_delete = this->pipes->selectedPipeUuids();
    for (const QUuid &uuid : marker_uuids_to_delete)
        deleteMarker(uuid);

    for (const QUuid &pipe_uuid : pipe_uuids_to_delete)
    {
        if (this->hydraulic_data)
            this->hydraulic_data->deletePipe(pipe_uuid);
        this->pipes->removePipe(pipe_uuid);
    }

    this->selection->clear();
    updateCanvas();
    emit signalEntityMarkerSelected(false);
}

void MapCanvasEntities::deleteMarker(const QUuid &uuid)
{
    const std::optional<MapEntityMarker> marker = markerByUuid(uuid);
    if (!marker.has_value())
        return;

    if (this->placement->floatingUuid() == uuid)
        stopEntityPositioning();

    if (marker->entity.type == InfrastructureEntity::DemandPoint)
    {
        if (this->demand_point_attachment_interaction_uuid == marker->entity.uuid)
            cancelDemandPointAttachmentInteraction();
        if (!this->hydraulic_data || !this->hydraulic_data->deleteDemandPoint(marker->entity.uuid))
            return;
    }
    if (InfrastructureEntityTraits::isHydraulicConnectionNode(marker->entity.type) &&
        !deleteHydraulicNode(marker->entity))
        return;
    if (InfrastructureEntityTraits::isHydraulicDeviceLink(marker->entity.type) &&
        !deleteHydraulicLink(marker->entity))
        return;

    this->selection->clear();
    if (this->placement->connectionTargetUuid() == uuid)
        this->placement->clearConnectionTarget();
    if (this->pipes->startNodeUuid() == uuid)
        this->pipes->clearPlacement();

    this->device_links->removeConnectedToUuid(uuid);
    this->pipes->removeConnectedToUuid(uuid);
    this->selection->removeMarker(uuid);

    const bool point_marker_removed = this->point_markers->removeMarker(uuid);
    if (point_marker_removed)
        recalculateWrapReferenceLongitude();
}

std::optional<QUuid> MapCanvasEntities::selectedDemandPointUuid() const
{
    const QList<QUuid> &selected_uuids = this->selection->selectedMarkerUuids();
    if (selected_uuids.size() != 1)
        return std::nullopt;

    const std::optional<MapEntityMarker> marker = markerByUuid(selected_uuids.first());
    if (!marker.has_value() || marker->entity.type != InfrastructureEntity::DemandPoint)
        return std::nullopt;
    return marker->entity.uuid;
}

std::optional<CoordinateWGS84> MapCanvasEntities::demandPointAttachmentCoordinate(
    const HydraulicDemandPoint &demand_point) const
{
    if (demand_point.attachment.type == HydraulicDemandPointAttachmentType::Junction)
    {
        const MapEntityMarker *junction_marker =
            this->point_markers->findMarker(demand_point.attachment.junction_uuid);
        if (junction_marker == nullptr ||
            junction_marker->entity.type != InfrastructureEntity::Junction)
        {
            return std::nullopt;
        }
        return junction_marker->coord_wgs84;
    }

    if (demand_point.attachment.type == HydraulicDemandPointAttachmentType::Pipe)
    {
        return this->pipes->coordinateAtPosition(
            demand_point.attachment.pipe_uuid, demand_point.attachment.pipe_position,
            *this->point_markers);
    }

    return std::nullopt;
}

bool MapCanvasEntities::demandPointAttachmentHandleAt(
    const QPointF &position, QUuid *demand_point_uuid) const
{
    if (!this->hydraulic_data || !this->map_model || !this->map_canvas ||
        this->map_canvas->size().isEmpty())
    {
        return false;
    }

    constexpr double handle_hit_radius = 12.0;
    const double handle_hit_radius_squared = handle_hit_radius * handle_hit_radius;

    const auto hit_distance_squared = [this, &position](
        const HydraulicDemandPoint &demand_point) -> std::optional<double>
    {
        const std::optional<CoordinateWGS84> coordinate =
            demandPointAttachmentCoordinate(demand_point);
        if (!coordinate.has_value())
            return std::nullopt;

        const std::optional<QPointF> handle_position = this->projection->screenFromCoordinate(
            coordinate.value(), this->map_canvas->size(), this->wrap_reference_longitude);
        if (!handle_position.has_value())
            return std::nullopt;
        const double delta_x = position.x() - handle_position->x();
        const double delta_y = position.y() - handle_position->y();
        return delta_x * delta_x + delta_y * delta_y;
    };

    const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
    const std::optional<QUuid> selected_uuid = selectedDemandPointUuid();
    if (selected_uuid.has_value())
    {
        for (const HydraulicDemandPoint &demand_point : network.demand_points)
        {
            if (demand_point.uuid != selected_uuid.value())
                continue;
            const std::optional<double> distance_squared =
                hit_distance_squared(demand_point);
            if (distance_squared.has_value() &&
                distance_squared.value() <= handle_hit_radius_squared)
            {
                if (demand_point_uuid)
                    *demand_point_uuid = demand_point.uuid;
                return true;
            }
            break;
        }
    }

    double best_distance_squared = handle_hit_radius_squared;
    QUuid best_uuid;
    for (const HydraulicDemandPoint &demand_point : network.demand_points)
    {
        if (selected_uuid.has_value() && demand_point.uuid == selected_uuid.value())
            continue;

        const std::optional<double> distance_squared =
            hit_distance_squared(demand_point);
        if (!distance_squared.has_value() ||
            distance_squared.value() > best_distance_squared)
        {
            continue;
        }

        best_distance_squared = distance_squared.value();
        best_uuid = demand_point.uuid;
    }

    if (best_uuid.isNull())
        return false;

    if (demand_point_uuid)
        *demand_point_uuid = best_uuid;
    return true;
}

void MapCanvasEntities::cancelDemandPointAttachmentInteraction()
{
    this->demand_point_attachment_interaction_uuid = QUuid();
    this->demand_point_attachment_selecting = false;
    this->demand_point_attachment_moving = false;
    this->demand_point_attachment_preview_valid = false;
    this->demand_point_attachment_preview_coordinate = CoordinateWGS84();
}

void MapCanvasEntities::startDemandPointAttachmentSelection(const QUuid &demand_point_uuid)
{
    if (!this->hydraulic_data || !this->hydraulic_data->demandPoint(demand_point_uuid).has_value())
        return;

    stopEntityPositioning();
    this->demand_point_attachment_interaction_uuid = demand_point_uuid;
    this->demand_point_attachment_selecting = true;
    this->demand_point_attachment_moving = false;
    this->demand_point_attachment_preview_valid = false;
    selectMarker(demand_point_uuid);
    updateCanvas();
}

void MapCanvasEntities::startDemandPointAttachmentMove(const QUuid &demand_point_uuid)
{
    if (!this->hydraulic_data)
        return;
    const std::optional<HydraulicDemandPoint> demand_point =
        this->hydraulic_data->demandPoint(demand_point_uuid);
    if (!demand_point.has_value() ||
        demand_point->attachment.type != HydraulicDemandPointAttachmentType::Pipe)
    {
        return;
    }

    stopEntityPositioning();
    this->demand_point_attachment_interaction_uuid = demand_point_uuid;
    this->demand_point_attachment_selecting = false;
    this->demand_point_attachment_moving = true;
    const std::optional<CoordinateWGS84> coordinate =
        demandPointAttachmentCoordinate(demand_point.value());
    this->demand_point_attachment_preview_valid = coordinate.has_value();
    if (coordinate.has_value())
        this->demand_point_attachment_preview_coordinate = coordinate.value();
    selectMarker(demand_point_uuid);
    updateCanvas();
}

bool MapCanvasEntities::handleDemandPointAttachmentRightClick(
    const QPointF &position, const QPoint &global_position)
{
    if (!this->hydraulic_data)
        return false;

    if (this->demand_point_attachment_moving)
    {
        const std::optional<HydraulicDemandPoint> demand_point =
            this->hydraulic_data->demandPoint(this->demand_point_attachment_interaction_uuid);
        if (!demand_point.has_value() ||
            demand_point->attachment.type != HydraulicDemandPointAttachmentType::Pipe)
        {
            cancelDemandPointAttachmentInteraction();
            updateCanvas();
            return true;
        }

        const MapCanvasPipes::PipePositionHit hit = this->pipes->nearestPositionOnPipe(
            demand_point->attachment.pipe_uuid, position, *this->point_markers);
        if (hit.isValid())
        {
            this->hydraulic_data->attachDemandPointToPipe(
                demand_point->uuid, hit.pipe_uuid, hit.pipe_position);
        }
        cancelDemandPointAttachmentInteraction();
        updateCanvas();
        return true;
    }

    if (this->demand_point_attachment_selecting)
    {
        const QUuid demand_point_uuid = this->demand_point_attachment_interaction_uuid;
        const std::optional<InfrastructureEntityReference> marker =
            this->point_markers->markerAt(position);
        if (marker.has_value() && marker->type == InfrastructureEntity::Junction)
        {
            this->hydraulic_data->attachDemandPointToJunction(
                demand_point_uuid, marker->uuid);
            cancelDemandPointAttachmentInteraction();
            selectMarker(demand_point_uuid);
            updateCanvas();
            return true;
        }

        const MapCanvasPipes::PipePositionHit pipe_hit = this->pipes->pipePositionAt(
            position, *this->point_markers);
        if (pipe_hit.isValid())
        {
            this->hydraulic_data->attachDemandPointToPipe(
                demand_point_uuid, pipe_hit.pipe_uuid, pipe_hit.pipe_position);
            cancelDemandPointAttachmentInteraction();
            selectMarker(demand_point_uuid);
            updateCanvas();
            return true;
        }

        cancelDemandPointAttachmentInteraction();
        updateCanvas();
        return true;
    }

    QUuid demand_point_uuid;
    if (!demandPointAttachmentHandleAt(position, &demand_point_uuid))
        return false;

    selectMarker(demand_point_uuid);
    updateCanvas();
    showDemandPointAttachmentContextMenu(demand_point_uuid, global_position);
    return true;
}

void MapCanvasEntities::showDemandPointAttachmentContextMenu(
    const QUuid &demand_point_uuid, const QPoint &global_position)
{
    if (!this->hydraulic_data)
        return;
    const std::optional<HydraulicDemandPoint> demand_point =
        this->hydraulic_data->demandPoint(demand_point_uuid);
    if (!demand_point.has_value() ||
        demand_point->attachment.type == HydraulicDemandPointAttachmentType::None)
    {
        return;
    }

#ifdef Q_OS_WASM
    WasmPopupMenu *menu = new WasmPopupMenu(this->map_canvas);
    menu->setDeleteOnClose(true);
    if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Pipe)
    {
        menu->addAction(QStringLiteral("Move attachment"), [this, demand_point_uuid]
        {
            startDemandPointAttachmentMove(demand_point_uuid);
        });
        menu->addAction(QStringLiteral("Convert to junction"), [this, demand_point_uuid]
        {
            QMessageBox *message_box = new QMessageBox(
                QMessageBox::Question, QStringLiteral("Convert demand-point attachment"),
                QStringLiteral("Do you really want to convert this attachment to a junction?"),
                QMessageBox::Yes | QMessageBox::No, this->map_canvas);
            message_box->setDefaultButton(QMessageBox::No);
            connect(message_box, &QMessageBox::finished, this,
                    [this, demand_point_uuid](int result)
            {
                if (result == QMessageBox::Yes)
                    convertDemandPointAttachmentToJunction(demand_point_uuid);
            });
            connect(message_box, &QMessageBox::finished,
                    message_box, &QObject::deleteLater);
            message_box->open();
        });
    }
    menu->addAction(QStringLiteral("Reattach"), [this, demand_point_uuid]
    {
        startDemandPointAttachmentSelection(demand_point_uuid);
    });
    menu->addAction(QStringLiteral("Detach"), [this, demand_point_uuid]
    {
        if (this->hydraulic_data)
            this->hydraulic_data->clearDemandPointAttachment(demand_point_uuid);
        updateCanvas();
    });
    menu->popup(global_position);
#else
    QMenu *menu = new QMenu(this->map_canvas);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Pipe)
    {
        QAction *action_move = menu->addAction("Move attachment");
        QAction *action_convert = menu->addAction("Convert to junction");
        connect(action_move, &QAction::triggered, this, [this, demand_point_uuid]()
        {
            startDemandPointAttachmentMove(demand_point_uuid);
        });
        connect(action_convert, &QAction::triggered, this, [this, demand_point_uuid]()
        {
            QMessageBox *message_box = new QMessageBox(
                QMessageBox::Question, "Convert demand-point attachment",
                "Do you really want to convert this attachment to a junction?",
                QMessageBox::Yes | QMessageBox::No, this->map_canvas);
            message_box->setDefaultButton(QMessageBox::No);
            connect(message_box, &QMessageBox::finished, this,
                    [this, demand_point_uuid](int result)
            {
                if (result == QMessageBox::Yes)
                    convertDemandPointAttachmentToJunction(demand_point_uuid);
            });
            connect(message_box, &QMessageBox::finished,
                    message_box, &QObject::deleteLater);
            message_box->open();
        });
    }
    QAction *action_reattach = menu->addAction("Reattach");
    QAction *action_detach = menu->addAction("Detach");
    connect(action_reattach, &QAction::triggered, this, [this, demand_point_uuid]()
    {
        startDemandPointAttachmentSelection(demand_point_uuid);
    });
    connect(action_detach, &QAction::triggered, this, [this, demand_point_uuid]()
    {
        if (this->hydraulic_data)
            this->hydraulic_data->clearDemandPointAttachment(demand_point_uuid);
        updateCanvas();
    });
    menu->popup(global_position);
#endif
}

void MapCanvasEntities::convertDemandPointAttachmentToJunction(const QUuid &demand_point_uuid)
{
    if (!this->hydraulic_data)
        return;
    const std::optional<HydraulicDemandPoint> demand_point =
        this->hydraulic_data->demandPoint(demand_point_uuid);
    if (!demand_point.has_value() ||
        demand_point->attachment.type != HydraulicDemandPointAttachmentType::Pipe)
    {
        return;
    }

    const std::optional<PipeGeometry> geometry =
        this->pipes->geometryByUuid(demand_point->attachment.pipe_uuid);
    const std::optional<CoordinateWGS84> attachment_coordinate =
        demandPointAttachmentCoordinate(demand_point.value());
    if (!geometry.has_value() || !attachment_coordinate.has_value())
        return;

    constexpr double endpoint_tolerance = 1.0e-12;
    if (demand_point->attachment.pipe_position <= endpoint_tolerance)
    {
        const std::optional<MapEntityMarker> marker = markerByUuid(geometry->start_node.uuid);
        if (marker.has_value() && marker->entity.type == InfrastructureEntity::Junction)
            this->hydraulic_data->attachDemandPointToJunction(demand_point_uuid, marker->entity.uuid);
        updateCanvas();
        return;
    }
    if (demand_point->attachment.pipe_position >= 1.0 - endpoint_tolerance)
    {
        const std::optional<MapEntityMarker> marker = markerByUuid(geometry->end_node.uuid);
        if (marker.has_value() && marker->entity.type == InfrastructureEntity::Junction)
            this->hydraulic_data->attachDemandPointToJunction(demand_point_uuid, marker->entity.uuid);
        updateCanvas();
        return;
    }

    const std::optional<QPointF> attachment_screen = this->projection->screenFromCoordinate(
        attachment_coordinate.value(), this->map_canvas->size(), this->wrap_reference_longitude);
    if (!attachment_screen.has_value())
        return;
    const MapCanvasPipes::PipePositionHit position_hit = this->pipes->nearestPositionOnPipe(
        demand_point->attachment.pipe_uuid, attachment_screen.value(), *this->point_markers);
    if (!position_hit.isValid())
        return;

    int vertex_index = -1;
    const MapCanvasPipes::PipeVertexHit existing_vertex =
        this->pipes->pipeVertexAt(attachment_screen.value());
    if (existing_vertex.isValid() && existing_vertex.pipe_uuid == demand_point->attachment.pipe_uuid)
    {
        const std::optional<CoordinateWGS84> existing_coordinate =
            this->pipes->pipeVertexCoordinate(existing_vertex.pipe_uuid, existing_vertex.vertex_index);
        if (existing_coordinate.has_value() &&
            GeoMetricProjection::distanceMeters(existing_coordinate.value(),
                                                attachment_coordinate.value()) < 0.001)
        {
            vertex_index = existing_vertex.vertex_index;
        }
    }

    bool inserted_vertex = false;
    if (vertex_index < 0)
    {
        vertex_index = position_hit.insert_index;
        const QList<CoordinateWGS84> previous_vertices =
            this->pipes->intermediateVertices(demand_point->attachment.pipe_uuid);
        if (!this->pipes->addPipeVertex(
                demand_point->attachment.pipe_uuid, vertex_index, attachment_coordinate.value()))
        {
            return;
        }
        if (!this->hydraulic_data->setPipeVertices(
                demand_point->attachment.pipe_uuid,
                this->pipes->intermediateVertices(demand_point->attachment.pipe_uuid)))
        {
            this->pipes->setIntermediateVertices(
                demand_point->attachment.pipe_uuid, previous_vertices);
            return;
        }
        inserted_vertex = true;
    }

    const QUuid junction_uuid = convertPipeVertexToJunctionInternal(
        demand_point->attachment.pipe_uuid, vertex_index);
    if (junction_uuid.isNull())
    {
        if (inserted_vertex)
            deletePipeVertex(demand_point->attachment.pipe_uuid, vertex_index);
        return;
    }

    this->hydraulic_data->attachDemandPointToJunction(demand_point_uuid, junction_uuid);
    selectMarker(demand_point_uuid);
    updateCanvas();
}

void MapCanvasEntities::showMarkerContextMenu(const QUuid &uuid,
                                              const QPoint &global_position,
                                              const QPointF &pointer_position)
{
    const std::optional<MapEntityMarker> context_marker = markerByUuid(uuid);
    if (!context_marker.has_value())
        return;

    const bool is_demand_point = context_marker->entity.type == InfrastructureEntity::DemandPoint;
    std::optional<HydraulicDemandPoint> demand_point;
    if (is_demand_point && this->hydraulic_data)
        demand_point = this->hydraulic_data->demandPoint(uuid);
    const bool demand_point_attached = demand_point.has_value() &&
        demand_point->attachment.type != HydraulicDemandPointAttachmentType::None;

    const bool multiple_entities_selected =
        (!this->projection || this->projection->supportsMercatorHitBox()) &&
        this->selection->isMarkerSelected(uuid) && this->selection->selectedMarkerCount() > 1;
#ifdef Q_OS_WASM
    WasmPopupMenu *menu = new WasmPopupMenu(this->map_canvas);
    menu->setDeleteOnClose(true);

    if (multiple_entities_selected)
    {
        menu->addAction(QStringLiteral("Move this entity"), [this, uuid, pointer_position]
        {
            startMarkerMove(uuid, pointer_position);
        });
        menu->addAction(QStringLiteral("Move selected entities"), [this, uuid]
        {
            startSelectedMarkerMove(uuid);
        });
    }
    else
    {
        menu->addAction(QStringLiteral("Move"), [this, uuid, pointer_position]
        {
            startMarkerMove(uuid, pointer_position);
        });
    }

    if (is_demand_point && !multiple_entities_selected)
    {
        menu->addSeparator();
        menu->addAction(
            demand_point_attached ? QStringLiteral("Reattach to network")
                                  : QStringLiteral("Attach to network"),
            [this, uuid]
        {
            startDemandPointAttachmentSelection(uuid);
        });
        if (demand_point_attached)
        {
            if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Pipe)
            {
                menu->addAction(QStringLiteral("Move attachment"), [this, uuid]
                {
                    startDemandPointAttachmentMove(uuid);
                });
                menu->addAction(QStringLiteral("Convert attachment to junction"), [this, uuid]
                {
                    QMessageBox *message_box = new QMessageBox(
                        QMessageBox::Question, QStringLiteral("Convert demand-point attachment"),
                        QStringLiteral("Do you really want to convert this attachment to a junction?"),
                        QMessageBox::Yes | QMessageBox::No, this->map_canvas);
                    message_box->setDefaultButton(QMessageBox::No);
                    connect(message_box, &QMessageBox::finished, this, [this, uuid](int result)
                    {
                        if (result == QMessageBox::Yes)
                            convertDemandPointAttachmentToJunction(uuid);
                    });
                    connect(message_box, &QMessageBox::finished,
                            message_box, &QObject::deleteLater);
                    message_box->open();
                });
            }
            menu->addAction(QStringLiteral("Detach from network"), [this, uuid]
            {
                if (this->hydraulic_data)
                    this->hydraulic_data->clearDemandPointAttachment(uuid);
                updateCanvas();
            });
        }
    }

    menu->addSeparator();
    menu->addAction(
        multiple_entities_selected
            ? QStringLiteral("Delete this entity")
            : QStringLiteral("Delete"),
        [this, uuid]
        {
            QMessageBox *box = new QMessageBox(this->map_canvas);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setIcon(QMessageBox::Question);
            box->setWindowTitle(QStringLiteral("Delete entity"));
            box->setText(QStringLiteral("Do you really want to delete this entity?"));
            box->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            box->setDefaultButton(QMessageBox::No);
            connect(box, &QMessageBox::buttonClicked, this,
                    [this, box, uuid](QAbstractButton *button)
            {
                if (box->standardButton(button) == QMessageBox::Yes)
                {
                    deleteMarker(uuid);
                    updateCanvas();
                }
            });
            box->open();
        });

    if (multiple_entities_selected)
    {
        menu->addAction(QStringLiteral("Delete selected entities"), [this]
        {
            QMessageBox *box = new QMessageBox(this->map_canvas);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setIcon(QMessageBox::Question);
            box->setWindowTitle(QStringLiteral("Delete selected entities"));
            box->setText(QStringLiteral("Do you really want to delete all selected entities?"));
            box->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            box->setDefaultButton(QMessageBox::No);
            connect(box, &QMessageBox::buttonClicked, this,
                    [this, box](QAbstractButton *button)
            {
                if (box->standardButton(button) == QMessageBox::Yes)
                    onMarkerSelectedDeleteRequested();
            });
            box->open();
        });
    }

    menu->popup(global_position);
#else
    QMenu *menu = new QMenu(this->map_canvas);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    if (multiple_entities_selected)
    {
        QAction *action_move_this = menu->addAction("Move this entity");
        QAction *action_move_selected = menu->addAction("Move selected entities");
        connect(action_move_this, &QAction::triggered, this, [this, uuid, pointer_position]()
        {
            startMarkerMove(uuid, pointer_position);
        });
        connect(action_move_selected, &QAction::triggered, this, [this, uuid]()
        {
            startSelectedMarkerMove(uuid);
        });
    }
    else
    {
        QAction *action_move = menu->addAction("Move");
        connect(action_move, &QAction::triggered, this, [this, uuid, pointer_position]()
        {
            startMarkerMove(uuid, pointer_position);
        });
    }

    if (is_demand_point && !multiple_entities_selected)
    {
        menu->addSeparator();
        QAction *action_attach = menu->addAction(
            demand_point_attached ? "Reattach to network" : "Attach to network");
        connect(action_attach, &QAction::triggered, this, [this, uuid]()
        {
            startDemandPointAttachmentSelection(uuid);
        });
        if (demand_point_attached)
        {
            if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Pipe)
            {
                QAction *action_move_attachment = menu->addAction("Move attachment");
                QAction *action_convert_attachment =
                    menu->addAction("Convert attachment to junction");
                connect(action_move_attachment, &QAction::triggered, this, [this, uuid]()
                {
                    startDemandPointAttachmentMove(uuid);
                });
                connect(action_convert_attachment, &QAction::triggered, this, [this, uuid]()
                {
                    QMessageBox *message_box = new QMessageBox(
                        QMessageBox::Question, "Convert demand-point attachment",
                        "Do you really want to convert this attachment to a junction?",
                        QMessageBox::Yes | QMessageBox::No, this->map_canvas);
                    message_box->setDefaultButton(QMessageBox::No);
                    connect(message_box, &QMessageBox::finished, this, [this, uuid](int result)
                    {
                        if (result == QMessageBox::Yes)
                            convertDemandPointAttachmentToJunction(uuid);
                    });
                    connect(message_box, &QMessageBox::finished,
                            message_box, &QObject::deleteLater);
                    message_box->open();
                });
            }
            QAction *action_detach = menu->addAction("Detach from network");
            connect(action_detach, &QAction::triggered, this, [this, uuid]()
            {
                if (this->hydraulic_data)
                    this->hydraulic_data->clearDemandPointAttachment(uuid);
                updateCanvas();
            });
        }
    }

    menu->addSeparator();

    const QString delete_this_text = multiple_entities_selected
                                         ? QStringLiteral("Delete this entity")
                                         : QStringLiteral("Delete");
    QAction *action_delete_this = menu->addAction(delete_this_text);
    connect(action_delete_this, &QAction::triggered, this, [this, uuid]()
    {
        QMessageBox *box = new QMessageBox(this->map_canvas);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setIcon(QMessageBox::Question);
        box->setWindowTitle("Delete entity");
        box->setText("Do you really want to delete this entity?");
        box->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box->setDefaultButton(QMessageBox::No);
        connect(box, &QMessageBox::buttonClicked, this, [this, box, uuid](QAbstractButton *button)
        {
            if (box->standardButton(button) == QMessageBox::Yes)
            {
                deleteMarker(uuid);
                updateCanvas();
            }
        });
        box->open();
    });

    if (multiple_entities_selected)
    {
        QAction *action_delete_selected = menu->addAction("Delete selected entities");
        connect(action_delete_selected, &QAction::triggered, this, [this]()
        {
            QMessageBox *box = new QMessageBox(this->map_canvas);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setIcon(QMessageBox::Question);
            box->setWindowTitle("Delete selected entities");
            box->setText("Do you really want to delete all selected entities?");
            box->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            box->setDefaultButton(QMessageBox::No);
            connect(box, &QMessageBox::buttonClicked, this,
                    [this, box](QAbstractButton *button)
            {
                if (box->standardButton(button) == QMessageBox::Yes)
                    onMarkerSelectedDeleteRequested();
            });
            box->open();
        });
    }

    menu->popup(global_position);
#endif
}

void MapCanvasEntities::onRectangleSelect(const QRect &rect, RectangleSelectMode mode)
{
    const QList<QUuid> previous_marker_uuids = this->selection->selectedMarkerUuids();
    const QList<QUuid> previous_pipe_uuids = this->pipes->selectedPipeUuids();
    this->selection->selectInRectangle(
        rect, this->point_markers->markers(), this->device_links->markers(),
        mode == RectangleSelectMode::Replace);

    if (previous_marker_uuids == this->selection->selectedMarkerUuids() &&
        previous_pipe_uuids == this->pipes->selectedPipeUuids())
    {
        return;
    }

    emit signalEntityMarkerSelected(this->selection->hasSelection());
    updateCanvas();
}

void MapCanvasEntities::updateConnectionTarget(const QPointF &mouse_position)
{
    std::optional<InfrastructureEntityReference> nearest_entity;
    if (this->placement->isCreating() &&
        InfrastructureEntityTraits::isHydraulicNetworkLink(this->placement->entity()))
    {
        QUuid excluded_uuid;
        if (InfrastructureEntityTraits::isHydraulicDeviceLink(this->placement->entity()))
            excluded_uuid = this->device_links->startNodeUuid();
        nearest_entity = this->point_markers->nearestConnectionTarget(
            mouse_position, excluded_uuid);
    }

    const QUuid nearest_uuid = nearest_entity.has_value() ? nearest_entity->uuid : QUuid();
    if (this->placement->connectionTargetUuid() == nearest_uuid)
        return;

    this->placement->setConnectionTarget(nearest_uuid);
    updateCanvas();
}

void MapCanvasEntities::selectPipe(const QUuid &pipe_uuid)
{
    const std::optional<InfrastructureEntityReference> selected_pipe =
        this->selection->replaceWithPipe(pipe_uuid);
    if (!selected_pipe.has_value())
        return;

    emit signalEntityMarkerSelected(true);
    if (this->hydraulic_data)
        this->hydraulic_data->setSelectedUuid(InfrastructureEntity::Pipe, pipe_uuid);
    updateCanvas();
}

void MapCanvasEntities::startPipeVertexMove(const QUuid &pipe_uuid, int vertex_index)
{
    stopEntityPositioning();
    if (!this->pipes->startPipeVertexMove(pipe_uuid, vertex_index))
        return;

    capturePipeMoveSnapshot(pipe_uuid);
    selectPipe(pipe_uuid);
    prepareMoveVisualState();
    const QPointF mouse_position = this->map_canvas->mapFromGlobal(QCursor::pos());
    const std::optional<CoordinateWGS84> mouse_coordinate =
        this->projection->coordinateFromScreen(mouse_position, this->map_canvas->size());
    if (!mouse_coordinate.has_value())
    {
        this->pipes->cancelPipeVertexMove();
        clearMoveSnapshot();
        return;
    }
    this->placement->startVirtualMove(
        InfrastructureEntity::Pipe, mouse_position, mouse_coordinate.value());
}

void MapCanvasEntities::convertPipeVertexToJunction(const QUuid &pipe_uuid,
                                                    int vertex_index)
{
    convertPipeVertexToJunctionInternal(pipe_uuid, vertex_index);
}

QUuid MapCanvasEntities::convertPipeVertexToJunctionInternal(const QUuid &pipe_uuid,
                                                             int vertex_index)
{
    const std::optional<CoordinateWGS84> vertex_coordinate =
        this->pipes->pipeVertexCoordinate(pipe_uuid, vertex_index);
    if (!vertex_coordinate.has_value())
        return QUuid();

    InfrastructureEntityReference junction_reference;
    junction_reference.type = InfrastructureEntity::Junction;
    junction_reference.uuid = createHydraulicPointEntity(
        InfrastructureEntity::Junction, vertex_coordinate.value());
    if (junction_reference.uuid.isNull())
        return QUuid();

    const MapEntityMarker junction_marker = this->point_markers->addMarker(
        junction_reference, vertex_coordinate.value(),
        this->point_markers->pixmapPathForEntity(InfrastructureEntity::Junction),
        this->point_markers->entityWidth());

    InfrastructureEntityReference second_pipe_reference;
    second_pipe_reference.type = InfrastructureEntity::Pipe;
    second_pipe_reference.uuid = this->hydraulic_data->splitPipeAtVertex(
        pipe_uuid, vertex_index, junction_reference.uuid);
    if (second_pipe_reference.uuid.isNull())
    {
        this->point_markers->removeMarker(junction_marker.entity.uuid);
        this->hydraulic_data->deleteJunction(junction_reference.uuid);
        return QUuid();
    }

    if (!this->pipes->splitPipeAtVertex(
            pipe_uuid, vertex_index, junction_reference, second_pipe_reference))
    {
        this->hydraulic_data->undoPipeSplit(
            pipe_uuid, second_pipe_reference.uuid, junction_reference.uuid);
        this->point_markers->removeMarker(junction_marker.entity.uuid);
        this->hydraulic_data->deleteJunction(junction_reference.uuid);
        return QUuid();
    }

    this->selection->replaceWithMarker(junction_marker);
    emit signalEntityMarkerSelected(true);
    recalculateWrapReferenceLongitude();
    updateCanvas();
    return junction_reference.uuid;
}

bool MapCanvasEntities::selectPipeAt(const QPointF &position)
{
    if (!this->placement->isIdle() || this->demand_point_attachment_selecting ||
        this->demand_point_attachment_moving)
        return false;

    const std::optional<InfrastructureEntityReference> pipe =
        this->pipes->pipeAt(position, *this->point_markers);
    if (!pipe.has_value())
        return false;

    if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
    {
        this->selection->togglePipe(pipe->uuid);
        emit signalEntityMarkerSelected(this->selection->hasSelection());
        updateCanvas();
        return true;
    }

    selectPipe(pipe->uuid);
    return true;
}

bool MapCanvasEntities::isPipeAt(const QPointF &position) const
{
    return this->placement->isIdle() && !this->demand_point_attachment_selecting &&
           !this->demand_point_attachment_moving &&
           this->pipes->pipeAt(position, *this->point_markers).has_value();
}

std::optional<MapEntityMarker> MapCanvasEntities::markerByUuid(const QUuid &uuid) const
{
    std::optional<MapEntityMarker> marker = this->point_markers->markerByUuid(uuid);
    if (marker.has_value())
        return marker;
    return this->device_links->markerByUuid(uuid);
}

void MapCanvasEntities::repaintCanvas()
{
    if (this->map_canvas)
        this->map_canvas->requestRenderUpdate();
}

void MapCanvasEntities::updateCanvas()
{
    ++this->visual_state_revision;
    emit signalVisualStateChanged(this->visual_state_revision);
    repaintCanvas();
}
