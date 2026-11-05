#ifndef MAP_CANVAS_WIDGET_H
#define MAP_CANVAS_WIDGET_H

#include <QPointer>
#include <QPointF>
#include <QWidget>

#include "map/editor/map_editor_visual_state.h"
#include "map/core/map_model.h"
#include "map/core/map_widget.h"
#include "map/editor/map_canvas_entities.h"

#include "network/hydraulic_data.h"

class MapEditorController;
class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

// The map editor's input widget: the editor controller first, then MapWidget
// navigation, for pointer and keys. It paints nothing - the bgfx surface
// draws the map, the network and every editor overlay from visualState()
// and viewportRenderState() (roadmap Phase 2; the QPainter editor is gone).
// On desktop it lies under the bgfx native window, which forwards the
// pointer to it; in the browser it lies on top of the Qt stack, under the
// bgfx canvas (pointer-events:none).
class MapCanvasWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MapCanvasWidget(MapModel *map_model, MapWidget *map, HydraulicData *hydraulic_data,
                             QWidget *parent = nullptr);

    void setEditorController(MapEditorController *editor_controller);
    MapCanvasEntities *mapCanvasEntities() const;
    int backgroundOpacity() const;
    MapEditorVisualState visualState() const;
    MapEditorViewportRenderState viewportRenderState() const;
    // Whatever the editor shows changed: signalRenderUpdateRequested().
    void requestRenderUpdate();
    void setSharedEditorActive(bool active);
    // Drop unfinished pointer gestures when input focus is lost, including
    // when the native bgfx window receives focus events instead of QWidget.
    void cancelActiveInput();

signals:
    // Whatever the editor shows changed (visual state, selection frames,
    // camera); the bgfx overlay rebuilds its cached state from this.
    void signalRenderUpdateRequested();
    // Only the camera moved (continuous zoom, pan): of the visual state just
    // the pointer's map coordinate changes (placement.mouse_coordinate_wgs84).
    void signalCameraChanged();

public slots:
    void setBackgroundOpacity(int opacity);
    void setIconSizePercent(int size_percent);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void applyControllerState();

    MapModel *map_model = nullptr;
    MapWidget *map = nullptr;
    HydraulicData *hydraulic_data = nullptr;
    MapCanvasEntities *map_canvas_entities = nullptr;
    QPointer<MapEditorController> editor_controller;
    QPointF last_pointer_position;
    bool last_pointer_position_valid = false;
    bool shared_editor_active = false;

    // 0 = transparent, 100 = fully system background
    int map_background_opacity = 0;
};

#endif // MAP_CANVAS_WIDGET_H
