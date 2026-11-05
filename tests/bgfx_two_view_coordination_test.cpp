#include "map/bgfx/bgfx_frame_coordinator.h"
#include "map/bgfx/bgfx_native_target_registry.h"

#include <cstdint>
#include <cstdio>
#include <vector>

int main()
{
    int primary = 1;
    int secondary = 2;
    int native_window = 3;
    int frames = 0;
    int first_draws = 0;
    int second_draws = 0;
    int binds = 0;
    int destroys = 0;
    std::uint16_t next_framebuffer = 1;

    BgfxFrameCoordinator coordinator([&frames]() { ++frames; });
    BgfxNativeTargetRegistry targets(
        [&next_framebuffer, &native_window](void *window, std::uint16_t width,
                                            std::uint16_t height)
        {
            if (window != &native_window || width == 0 || height == 0)
                return bgfx::FrameBufferHandle{bgfx::kInvalidHandle};
            return bgfx::FrameBufferHandle{next_framebuffer++};
        },
        [&binds](std::uint16_t, bgfx::FrameBufferHandle) { ++binds; },
        [&destroys](bgfx::FrameBufferHandle) { ++destroys; });

    const BgfxFrameCoordinator::ViewRange map = coordinator.reserve(&primary, 5);
    const BgfxFrameCoordinator::ViewRange probe = coordinator.reserve(&secondary, 1);
    if (!map.valid() || map.first != 0 || !probe.valid() || probe.first != 5)
        return 1;
    if (!targets.attachDefault(&primary, map.first)
        || !targets.attachWindow(&secondary, probe.first, &native_window, 480, 320, true))
        return 2;
    if (!coordinator.renderAndPresent({
        {&primary, [&first_draws]() { ++first_draws; }},
        {&secondary, [&second_draws]() { ++second_draws; }}}))
        return 3;
    if (frames != 1 || first_draws != 1 || second_draws != 1 || targets.count() != 2)
        return 4;
    if (!targets.resizeWindow(&secondary, 600, 400) || destroys != 1)
        return 5;
    if (!coordinator.renderAndPresent({
        {&primary, [&first_draws]() { ++first_draws; }},
        {&secondary, [&second_draws]() { ++second_draws; }}}))
        return 6;
    if (frames != 2 || first_draws != 2 || second_draws != 2)
        return 7;

    // Retiring the secondary target must preserve the main map's rendering.
    if (!targets.release(&secondary) || !coordinator.release(&secondary)
        || !coordinator.present(&primary) || frames != 3 || destroys != 2)
        return 8;
    if (!targets.release(&primary) || !coordinator.release(&primary)
        || targets.count() != 0 || destroys != 2 || binds != 5)
        return 9;
    std::puts("bgfx two-view presentation, resize and detach coordination passed");
    return 0;
}
