#pragma once

#include "map/bgfx/map_bgfx_hud_canvas.h"
#include "map/bgfx/map_bgfx_hud_gpu.h"
#include "map/bgfx/map_bgfx_hud_text_cache.h"

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QTimer>

#include <cstdint>

class ProjectRevisionGraph;
class QWindow;
class QWidget;

// EXPERIMENTAL embedded Projects bgfx viewport (Linux native builds only).
// The existing QWidget owns camera, navigation, layout, picking and events.
// This adapter owns only an embedded QWindow's secondary swapchain and GPU
// draw resources. It NEVER calls bgfx::init/shutdown or bgfx::reset.
//
// Step 3B limitation: the existing Maps viewport still owns device startup.
// Until the next ownership refactor, this GPU view is enabled only when that
// device has already initialized. Otherwise the graph stays QPainter-based.
class ProjectGraphBgfxViewport final : public QObject
{
public:
    explicit ProjectGraphBgfxViewport(ProjectRevisionGraph *graph);
    ~ProjectGraphBgfxViewport() override;

    bool active() const { return this->attached; }
    void onShown();
    void onHidden();
    void resize();
    void requestRedraw();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void checkAvailability();
    void tryAttach();
    void detach();
    void render();
    QSize targetPixels() const;

    ProjectRevisionGraph *graph = nullptr;
    QPointer<QWindow> window;
    QPointer<QWidget> container;
    QTimer retry_timer;
    QTimer redraw_timer;
    MapBgfxHudCanvas canvas;
    MapBgfxHudTextCache atlas;
    MapBgfxHudGpu gpu;
    QSize allocated_pixels;
    std::uint16_t view_id = 0;
    bool attached = false;
    bool gpu_initialized = false;
    bool unavailable = false;
};
