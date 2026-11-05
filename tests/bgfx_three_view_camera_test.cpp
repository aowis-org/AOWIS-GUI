#include "map/bgfx/map_bgfx_three_view_camera.h"
#include "map/bgfx/bgfx_frame_coordinator.h"
#include "map/bgfx/bgfx_native_target_registry.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
bool near(double first, double second, double tolerance = 1e-8)
{
    return std::abs(first - second) <= tolerance;
}
}

BgfxNativeSurface testSurface(void *window, std::uint16_t width, std::uint16_t height)
{
    BgfxNativeSurface surface;
    surface.window = window;
    surface.width = width;
    surface.height = height;
    return surface;
}

int main()
{
    MapBgfxThreeViewCamera first;
    MapBgfxThreeViewCamera second;
    first.reset(8.0, 50.0, 12.0);
    second.reset(8.0, 50.0, 12.0);
    const MapBgfx2d::Camera original = second.camera(480, 320);
    first.pan(160.0, -70.0);
    if (near(first.centerWorld().x, second.centerWorld().x)
        || !near(second.centerWorld().y, original.center.y))
        return 1;

    const MapBgfx2d::Camera before = first.camera(480, 320);
    const MapBgfx2d::ScreenPoint anchor{120, 90};
    const double geographic_x = before.center.x
        + (anchor.x - before.viewport_width * 0.5) / before.world_pixels;
    const double geographic_y = before.center.y
        + (anchor.y - before.viewport_height * 0.5) / before.world_pixels;
    first.zoomAround(4, anchor.x, anchor.y, 480, 320);
    const MapBgfx2d::Camera after = first.camera(480, 320);
    if (!near(geographic_x, after.center.x
            + (anchor.x - after.viewport_width * 0.5) / after.world_pixels)
        || !near(geographic_y, after.center.y
            + (anchor.y - after.viewport_height * 0.5) / after.world_pixels)
        || !near(after.world_pixels / before.world_pixels, 2.0)
        || !near(second.zoom(), 12.0))
        return 2;

    int presented = 0;
    BgfxFrameCoordinator coordinator([&presented]() { ++presented; });
    int primary = 1;
    int secondary_a = 2;
    int secondary_b = 3;
    int native_a = 4;
    int native_b = 5;
    int destroys = 0;
    std::uint16_t next_handle = 1;
    BgfxNativeTargetRegistry targets(
        [&next_handle, &native_a, &native_b](const BgfxNativeSurface &surface)
        {
            if ((surface.window != &native_a && surface.window != &native_b)
                || surface.width == 0 || surface.height == 0)
                return bgfx::FrameBufferHandle{bgfx::kInvalidHandle};
            return bgfx::FrameBufferHandle{next_handle++};
        },
        [](std::uint16_t, bgfx::FrameBufferHandle) {},
        [&destroys](bgfx::FrameBufferHandle) { ++destroys; });
    const BgfxFrameCoordinator::ViewRange main_range = coordinator.reserve(&primary, 5);
    const BgfxFrameCoordinator::ViewRange range_a = coordinator.reserve(&secondary_a, 1);
    const BgfxFrameCoordinator::ViewRange range_b = coordinator.reserve(&secondary_b, 1);
    if (main_range.first != 0 || range_a.first != 5 || range_b.first != 6)
        return 3;
    if (!targets.attachDefault(&primary, main_range.first)
        || !targets.attachWindow(&secondary_a, range_a.first, testSurface(&native_a, 480, 320), true)
        || !targets.attachWindow(&secondary_b, range_b.first, testSurface(&native_b, 480, 320), true))
        return 4;
    int draws = 0;
    const std::vector<BgfxFrameCoordinator::RenderWork> work = {
        {&primary, [&draws]() { ++draws; }},
        {&secondary_a, [&draws]() { ++draws; }},
        {&secondary_b, [&draws]() { ++draws; }}
    };
    if (!coordinator.renderAndPresent(work) || presented != 1 || draws != 3)
        return 5;
    if (!targets.resizeWindow(&secondary_a, testSurface(&native_a, 600, 420)) || destroys != 1)
        return 6;
    if (!targets.release(&secondary_a) || !coordinator.release(&secondary_a)
        || !coordinator.renderAndPresent({{&primary, {}}, {&secondary_b, {}}})
        || presented != 2 || targets.count() != 2)
        return 7;
    if (!targets.release(&secondary_b) || !coordinator.release(&secondary_b)
        || !targets.release(&primary) || !coordinator.release(&primary)
        || destroys != 3 || !coordinator.reserve(&secondary_a, 1).valid())
        return 8;
    std::puts("three independent cameras and bgfx native-target coordination passed");
    return 0;
}
