#include "map/bgfx/bgfx_native_target_registry.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace
{
int failures = 0;
void check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct Binding
{
    std::uint16_t view = 0;
    std::uint16_t framebuffer = 0;
};
}

int main()
{
    int primary = 1;
    int second = 2;
    int third = 3;
    int first_window = 4;
    int second_window = 5;
    std::uint16_t next_framebuffer = 10;
    bool fail_create = false;
    int create_count = 0;
    std::vector<std::uint16_t> destroyed;
    std::vector<Binding> bindings;

    BgfxNativeTargetRegistry registry(
        [&](void *window, std::uint16_t width, std::uint16_t height)
        {
            check(window == &first_window || window == &second_window,
                  "secondary native window identity preserved");
            check(width != 0 && height != 0, "nonzero framebuffer dimensions");
            ++create_count;
            if (fail_create)
                return bgfx::FrameBufferHandle{bgfx::kInvalidHandle};
            return bgfx::FrameBufferHandle{next_framebuffer++};
        },
        [&](std::uint16_t view, bgfx::FrameBufferHandle handle)
        {
            bindings.push_back({view, handle.idx});
        },
        [&](bgfx::FrameBufferHandle handle)
        {
            destroyed.push_back(handle.idx);
        });

    check(!registry.attachDefault(nullptr, 0), "null primary owner refused");
    check(!registry.attachWindow(&second, 5, &first_window, 640, 480, true),
          "secondary target requires a primary device target");
    check(registry.attachDefault(&primary, 0), "default framebuffer attached to view zero");
    check(registry.isDefault(&primary) && registry.count() == 1,
          "primary target registered without owned framebuffer");
    check(registry.onlyClient(&primary),
          "primary alone owns all native targets before secondary attachment");
    check(!registry.attachDefault(&third, 1), "second default framebuffer rejected");
    check(!registry.attachDefault(&primary, 0), "duplicate primary rejected");
    check(!registry.attachWindow(&second, 0, &first_window, 640, 480, true),
          "view collision rejected");
    check(!registry.attachWindow(&second, 5, &first_window, 640, 480, false),
          "secondary rejected when bgfx lacks swap-chain capability");
    check(!registry.attachWindow(&second, 5, nullptr, 640, 480, true),
          "null native window rejected");
    check(!registry.attachWindow(&second, 5, &first_window, 0, 480, true),
          "zero-size target rejected");
    fail_create = true;
    check(!registry.attachWindow(&second, 5, &first_window, 640, 480, true),
          "failed framebuffer creation leaves no target registered");
    check(registry.count() == 1, "failed secondary attach does not change target count");

    fail_create = false;
    check(registry.attachWindow(&second, 5, &first_window, 640, 480, true),
          "secondary native framebuffer attached to own view");
    check(!registry.isDefault(&second) && registry.count() == 2,
          "secondary registered as owned target");
    check(bindings.back().view == 5 && bindings.back().framebuffer == 10,
          "second view bound to first created framebuffer");
    check(!registry.attachWindow(&third, 10, &first_window, 640, 480, true),
          "same native window cannot be registered twice");
    check(!registry.resizeWindow(&primary, 320, 240),
          "primary resize delegated to bgfx reset instead");
    check(registry.resizeWindow(&second, 640, 480) && create_count == 2,
          "no-op secondary resize avoids new GPU allocation");
    fail_create = true;
    check(!registry.resizeWindow(&second, 800, 500),
          "failed secondary resize reported");
    check(destroyed.empty() && bindings.back().framebuffer == 10,
          "failed secondary resize preserves original framebuffer and binding");
    fail_create = false;
    check(registry.resizeWindow(&second, 800, 500), "secondary framebuffer resized");
    check(bindings.back().framebuffer == 11 && destroyed.size() == 1
          && destroyed[0] == 10, "new framebuffer bound before old one destroyed");
    check(registry.attachWindow(&third, 10, &second_window, 500, 300, true),
          "third independent target registered");
    check(registry.count() == 3, "all three targets live simultaneously");
    check(!registry.onlyClient(&primary),
          "foreign native targets block primary-only shutdown");
    check(registry.release(&second), "secondary target released");
    check(bindings.back().view == 5 && bindings.back().framebuffer == bgfx::kInvalidHandle,
          "view unbound on release");
    check(destroyed.size() == 2 && destroyed[1] == 11,
          "secondary framebuffer destroyed once after unbinding");
    check(!registry.release(&second), "duplicate release no-op");
    check(registry.release(&primary), "primary detached");
    check(destroyed.size() == 2, "default swapchain never destroyed by registry");
    registry.reset();
    check(registry.count() == 0 && destroyed.size() == 3 && destroyed[2] == 12,
          "reset destroys remaining secondary framebuffer");
    check(!registry.contains(&third), "reset removed all target bindings");
    registry.reset();
    check(destroyed.size() == 3, "reset is idempotent");

    if (failures == 0)
        std::puts("bgfx native render-target lifecycle tests passed");
    return failures == 0 ? 0 : 1;
}
