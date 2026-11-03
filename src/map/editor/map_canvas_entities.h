#ifndef MAP_CANVAS_ENTITIES_H
#define MAP_CANVAS_ENTITIES_H

#include <QObject>
#include <QHash>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QUuid>

#include <memory>
#include <optional>

#include "common/_enums_structs.h"
#include "network/hydraulic_data.h"
#include "map/editor/map_editor_visual_state.h"
#include "map/render/map_entity_pixmap_renderer.h"
#include "map/core/map_models.h"

class MapCanvasProjection;
class MapCanvasDeviceLinks;
class MapCanvasMarkers;
class MapCanvasPipes;
class MapCanvasPlacement;
class MapCanvasSelection;
class MapCanvasWidget;
class MapModel;

enum RectangleSelectMode
{
    Add,
    Replace
};

class MapCanvasEntities : public QObject
{
    Q_OBJECT

public:
    explicit MapCanvasEntities(MapModel *map_model, HydraulicData *hydraulic_data,
                               MapCanvasWidget *map_canvas);
    ~MapCanvasEntities() override;

    void setProjection(std::unique_ptr<MapCanvasProjection> projection);
    void loadNetwork(const NetworkHydraulic &network);
    void startEntityPositioning(InfrastructureEntity entity);
    void stopEntityPositioning();
    bool cancelActiveMove();
    bool positioningActive() const;
    // Floating *creation* is preview-only. Moving existing entities changes
    // live geometry and must not be entered by the 3D input bridge yet.
    bool creatingPreviewActive() const;
    bool floatEntity(const QPointF &position);
    // The same placement path can stage a 3D link preview without committing
    // a hydraulic entity. The 2D editor retains its default commit behavior.
    bool anchorMarker(const QPointF &position, bool commit_to_model = true);

    void setIconSizePercent(int size_percent);
    void scaleMarkers();
    // scaleMarkers() when the icon width changed (camera zoom motion).
    void updateMarkerScale();
    void positionMarkers();
    // An entity / network move is in progress (visualState().move.active,
    // without building the whole visual state).
    bool isMoveActive() const;
    MapEditorVisualState visualState() const;
    int iconSizePercent() const;

    // Common selection entry point for 2D marker/pipe hits and 3D renderer
    // hits. The renderer identifies a candidate; only this editor selects it.
    bool selectEntityAt(const QPointF &position);
    bool isEntityAt(const QPointF &position) const;
    bool selectMarkerAt(const QPointF &position);
    bool isMarkerAt(const QPointF &position) const;
    bool showMarkerContextMenuAt(const QPointF &position, const QPoint &global_position);
    bool handleDemandPointAttachmentRightClick(const QPointF &position,
                                               const QPoint &global_position);
    bool selectDeviceLinkAt(const QPointF &position);
    bool isDeviceLinkAt(const QPointF &position) const;
    bool selectPipeAt(const QPointF &position);
    bool isPipeAt(const QPointF &position) const;
    bool showPipeContextMenuAt(const QPointF &position, const QPoint &global_position);

    std::optional<MapEntityMarker> markerByUuid(const QUuid &uuid) const;

private:
    bool anchorDeviceLink(const QPointF &position, bool commit_to_model);
    bool anchorPipe(const QPointF &position, bool commit_to_model);
    bool anchorPipeVertexMove(const QPointF &position);
    QUuid createHydraulicPointEntity(InfrastructureEntity entity,
                              const CoordinateWGS84 &coordinate);
    QUuid createHydraulicDeviceLink(InfrastructureEntity entity,
                                    const DeviceLinkGeometry &geometry);
    bool deleteHydraulicNode(const InfrastructureEntityReference &reference);
    bool deleteHydraulicLink(const InfrastructureEntityReference &reference);
    void addPipeVertex(const QUuid &pipe_uuid, int insert_index,
                       const CoordinateWGS84 &coordinate);
    void deletePipeVertex(const QUuid &pipe_uuid, int vertex_index);
    bool synchronizeMarkerCoordinate(const QUuid &uuid);
    bool synchronizeSelectedGeometry();
    void captureMarkerMoveSnapshot(const QUuid &uuid);
    void captureSelectedMoveSnapshot();
    void capturePipeMoveSnapshot(const QUuid &pipe_uuid);
    void prepareMoveVisualState();
    void restoreMoveSnapshot();
    void clearMoveSnapshot();
    void updateConnectionTarget(const QPointF &mouse_position);
    void recalculateWrapReferenceLongitude();
    void setWrapReferenceLongitude(double longitude);
    void deleteMarker(const QUuid &uuid);
    void selectMarker(const QUuid &uuid);
    void selectPipe(const QUuid &pipe_uuid);
    void applyExternalSelection(InfrastructureEntity entity_type, const QUuid &uuid);
    void startMarkerMove(const QUuid &uuid);
    void startSelectedMarkerMove(const QUuid &uuid);
    void startPipeVertexMove(const QUuid &pipe_uuid, int vertex_index);
    void convertPipeVertexToJunction(const QUuid &pipe_uuid, int vertex_index);
    QUuid convertPipeVertexToJunctionInternal(const QUuid &pipe_uuid, int vertex_index);
    void startDemandPointAttachmentSelection(const QUuid &demand_point_uuid);
    void startDemandPointAttachmentMove(const QUuid &demand_point_uuid);
    void cancelDemandPointAttachmentInteraction();
    void showDemandPointAttachmentContextMenu(const QUuid &demand_point_uuid,
                                              const QPoint &global_position);
    void convertDemandPointAttachmentToJunction(const QUuid &demand_point_uuid);
    std::optional<QUuid> selectedDemandPointUuid() const;
    std::optional<CoordinateWGS84> demandPointAttachmentCoordinate(
        const HydraulicDemandPoint &demand_point) const;
    bool demandPointAttachmentHandleAt(const QPointF &position, QUuid *demand_point_uuid) const;
    void showMarkerContextMenu(const QUuid &uuid, const QPoint &global_position);
    void repaintCanvas();
    void updateCanvas();

    MapModel *map_model = nullptr;
    std::unique_ptr<MapCanvasProjection> projection;
    HydraulicData *hydraulic_data = nullptr;
    QPointer<MapCanvasWidget> map_canvas;
    MapEntityPixmapRenderer pixmap_renderer;
    MapCanvasMarkers *point_markers = nullptr;
    MapCanvasDeviceLinks *device_links = nullptr;
    MapCanvasPipes *pipes = nullptr;
    MapCanvasSelection *selection = nullptr;
    MapCanvasPlacement *placement = nullptr;
    QList<MapEntityMarker> move_marker_snapshot;
    QHash<QUuid, QList<CoordinateWGS84>> move_pipe_vertices_snapshot;
    QList<QUuid> move_dynamic_pipe_uuids;
    QList<QUuid> move_dynamic_device_link_uuids;
    QList<QUuid> move_translated_pipe_uuids;
    quint64 move_session_id = 0;
    double wrap_reference_longitude = 0.0;
    quint64 visual_state_revision = 0;
    // Icon width scaleMarkers() last applied (updateMarkerScale()).
    int applied_marker_width = -1;
    bool synchronizing_geometry = false;
    QUuid demand_point_attachment_interaction_uuid;
    bool demand_point_attachment_selecting = false;
    bool demand_point_attachment_moving = false;
    bool demand_point_attachment_preview_valid = false;
    CoordinateWGS84 demand_point_attachment_preview_coordinate;

private slots:
    void onNodeChanged(InfrastructureEntity entity_type, const QUuid &uuid);
    void onDemandPointChanged(const QUuid &uuid);
    void onLinkChanged(InfrastructureEntity entity_type, const QUuid &uuid);
    void onEntityLocateRequested(InfrastructureEntity entity_type, const QUuid &uuid);

public slots:
    void onMarkerSelectedDeleteRequested();
    void onRectangleSelect(const QRect &rect, RectangleSelectMode mode);

signals:
    void signalEntityMarkerSelected(bool status);
    void signalVisualStateChanged(quint64 revision);
};

#endif // MAP_CANVAS_ENTITIES_H
