#ifndef MAP_CANVAS_PIPES_H
#define MAP_CANVAS_PIPES_H

#include <optional>
#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QPointer>
#include <QPoint>
#include <QPointF>
#include <QUuid>

#include "map/core/map_model.h"
#include "map/core/map_models.h"

class MapCanvasMarkers;
class MapCanvasWidget;
class MapCanvasProjection;

class MapCanvasPipes : public QObject
{
    Q_OBJECT

public:
    struct PipeVertexHit
    {
        QUuid pipe_uuid;
        int vertex_index = -1;
        bool isValid() const;
    };

    struct PipeSegmentHit
    {
        QUuid pipe_uuid;
        int insert_index = -1;
        QPointF nearest_point;
        bool isValid() const;
    };

    struct PipePositionHit
    {
        QUuid pipe_uuid;
        int insert_index = -1;
        double pipe_position = 0.0;
        QPointF nearest_point;
        CoordinateWGS84 coordinate_wgs84;
        bool isValid() const;
    };

    explicit MapCanvasPipes(MapModel *map_model, MapCanvasWidget *map_canvas,
                            QObject *parent = nullptr);

    void setProjection(MapCanvasProjection *projection);
    void setWrapReferenceLongitude(double longitude);

    void clear();
    void clearPlacement();
    bool hasStartNode() const;
    QUuid startNodeUuid() const;
    void startPipe(const QUuid &start_node_uuid);
    void appendIntermediateVertex(const CoordinateWGS84 &coordinate);
    QList<CoordinateWGS84> intermediateVertices() const;
    QList<CoordinateWGS84> intermediateVertices(const QUuid &pipe_uuid) const;
    std::optional<PipeGeometry> geometryByUuid(const QUuid &pipe_uuid) const;
    QList<QUuid> connectedPipeUuids(const QSet<QUuid> &node_uuids) const;

    bool addPipe(const InfrastructureEntityReference &pipe_reference,
                 const InfrastructureEntityReference &start_node,
                 const InfrastructureEntityReference &end_node,
                 const QList<CoordinateWGS84> &intermediate_vertices = {});
    bool completePipe(const InfrastructureEntityReference &pipe_reference,
                      const InfrastructureEntityReference &start_node,
                      const InfrastructureEntityReference &end_node);
    bool hasSelection() const;
    QList<QUuid> selectedPipeUuids() const;
    void clearSelection();
    std::optional<InfrastructureEntityReference> selectPipe(const QUuid &pipe_uuid);
    std::optional<InfrastructureEntityReference> togglePipe(const QUuid &pipe_uuid);
    bool removePipe(const QUuid &pipe_uuid);
    void selectPipesWithSelectedEndpoints(const QList<QUuid> &selected_marker_uuids);
    void moveIntermediateVertices(const QList<QUuid> &pipe_uuids,
                                  double longitude_delta, double latitude_delta);

    // Node positions come from the node markers' uuid index (O(1) per pipe
    // end); never rebuild a lookup of all markers per pipe or per demand
    // point: with thousands of demand points that was quadratic (seconds
    // per editor visual state, i.e. per zoom level change).
    std::optional<InfrastructureEntityReference> pipeAt(
        const QPointF &position, const MapCanvasMarkers &markers) const;
    PipeVertexHit pipeVertexAt(const QPointF &position) const;
    PipeSegmentHit pipeSegmentAt(const QPointF &position,
                                 const MapCanvasMarkers &markers) const;
    PipePositionHit pipePositionAt(const QPointF &position,
                                   const MapCanvasMarkers &markers) const;
    PipePositionHit nearestPositionOnPipe(const QUuid &pipe_uuid, const QPointF &position,
                                          const MapCanvasMarkers &markers) const;
    std::optional<CoordinateWGS84> coordinateAtPosition(
        const QUuid &pipe_uuid, double pipe_position,
        const MapCanvasMarkers &markers) const;
    bool showContextMenuAt(const QPointF &position, const QPoint &global_position,
                           const MapCanvasMarkers &markers);

    bool addPipeVertex(const QUuid &pipe_uuid, int insert_index,
                       const CoordinateWGS84 &coordinate);
    bool deletePipeVertex(const QUuid &pipe_uuid, int vertex_index);
    bool setIntermediateVertices(const QUuid &pipe_uuid,
                                 const QList<CoordinateWGS84> &intermediate_vertices);
    std::optional<CoordinateWGS84> pipeVertexCoordinate(const QUuid &pipe_uuid,
                                                        int vertex_index) const;
    bool splitPipeAtVertex(const QUuid &pipe_uuid, int vertex_index,
                           const InfrastructureEntityReference &junction_reference,
                           const InfrastructureEntityReference &second_pipe_reference);

    bool startPipeVertexMove(const QUuid &pipe_uuid, int vertex_index);
    bool isPipeVertexMoveActive() const;
    std::optional<QUuid> activePipeVertexMoveUuid() const;
    int activePipeVertexMoveIndex() const;
    bool updatePipeVertexMove(const QPointF &screen_position);
    bool finishPipeVertexMove(const QPointF &screen_position);
    void cancelPipeVertexMove();

    void removeConnectedToUuid(const QUuid &uuid);

signals:
    void signalCanvasUpdateRequested();
    void pipeSelectionRequested(const QUuid &pipe_uuid);
    void pipeVertexAddRequested(const QUuid &pipe_uuid, int insert_index,
                                const CoordinateWGS84 &coordinate);
    void pipeVertexDeleteRequested(const QUuid &pipe_uuid, int vertex_index);
    void pipeVertexMoveRequested(const QUuid &pipe_uuid, int vertex_index);
    void pipeVertexConversionRequested(const QUuid &pipe_uuid, int vertex_index);

private:
    struct PipeCanvasItem
    {
        InfrastructureEntityReference entity;
        PipeGeometry geometry;
        bool selected = false;
    };

    QPointF screenFromWgs84(const CoordinateWGS84 &coordinate) const;
    PipeCanvasItem *pipeByUuid(const QUuid &pipe_uuid);
    const PipeCanvasItem *pipeByUuid(const QUuid &pipe_uuid) const;
    int pipeIndexByUuid(const QUuid &pipe_uuid) const;
    PipePositionHit nearestPositionOnPipeInternal(
        const PipeCanvasItem &pipe, const QPointF &position,
        const MapCanvasMarkers &markers, double max_distance) const;
    QList<CoordinateWGS84> pipeCoordinates(
        const PipeCanvasItem &pipe, const MapCanvasMarkers &markers) const;
    void confirmVertexConversion(const QUuid &pipe_uuid, int vertex_index);
    void rebuildUuidIndex();
    void updateCanvas();

    MapModel *map_model = nullptr;
    MapCanvasProjection *projection = nullptr;
    QPointer<MapCanvasWidget> map_canvas;
    double wrap_reference_lon = 0.0;
    QList<PipeCanvasItem> list_pipes;
    QHash<QUuid, int> pipe_indices_by_uuid;
    QUuid pipe_start_node_uuid;
    QList<CoordinateWGS84> pipe_intermediate_vertices;
    std::optional<QUuid> pipe_vertex_move_pipe_uuid;
    int pipe_vertex_move_index = -1;
};

#endif // MAP_CANVAS_PIPES_H
