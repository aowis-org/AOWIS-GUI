#pragma once

#include "map/bgfx/map_bgfx_hud_canvas.h"
#include "map/bgfx/map_bgfx_hud_gpu.h"
#include "map/bgfx/map_bgfx_hud_text_cache.h"
#include "map/bgfx/map_bgfx_label_gpu.h"
#include "tabs/project_graph_bgfx_scene.h"

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
    void invalidateScene();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void checkAvailability();
    bool initializePrimary();
    void shutdownOwnedDevice();
    void retireFromDevice();
    void tryAttach();
    void detach();
    void render();
    QSize targetPixels() const;

    ProjectRevisionGraph *graph = nullptr;
    QPointer<QWindow> window;
    QPointer<QWidget> container;
    QTimer retry_timer;
    QTimer redraw_timer;
    // Shapes (cards, routes) with the HUD program; atlas holds only their
    // white texel, so the smallest atlas suffices (64 x 64 RGBA = 16 KiB
    // instead of 16 MiB at 2048). Text on top: glyph quads from the distance
    // atlas (fs_label_sdf).
    static constexpr int ShapeAtlasExtent = 64;
    MapBgfxHudCanvas canvas;
    MapBgfxHudTextCache atlas;
    MapBgfxHudGpu gpu;
    MapBgfxHudCanvas text_canvas;
    MapBgfxGlyphAtlas glyphs{1024};
    MapBgfxLabelGpu text_gpu;
    ProjectGraphBgfxScene::Index scene_index;
    bool scene_dirty = true;
    QSize allocated_pixels;
    // Swap chain flags (MSAA level) the owned back buffer was created with.
    std::uint32_t allocated_swap_chain_flags = 0;
    std::uint16_t view_id = 0;
    std::uint64_t device_generation = 0;
    bool attached = false;
    bool gpu_initialized = false;
    bool unavailable = false;
};
