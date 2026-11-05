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

// Native Projects bgfx viewport (Linux X11/Wayland builds only).
// The existing QWidget owns camera, navigation, layout, picking and events.
// This adapter owns only an embedded QWindow's secondary swapchain and GPU
// draw resources. The first visible client owns the process-global bgfx
// device: Projects can bootstrap the default window, while a Projects view
// opened after Maps uses a secondary swapchain. Retiring a hidden owner
// leaves the next visible client free to initialize the next device generation.
// QPainter remains the fallback if the native GPU path is unavailable.
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
    bool initializePrimary();
    void shutdownOwnedDevice();
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
