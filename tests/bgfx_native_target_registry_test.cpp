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


BgfxNativeSurface testSurface(void *window, std::uint16_t width, std::uint16_t height)
{
    BgfxNativeSurface surface;
    surface.window = window;
    surface.width = width;
    surface.height = height;
    return surface;
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
        [&](const BgfxNativeSurface &surface)
        {
            check(surface.window == &first_window || surface.window == &second_window,
                  "secondary native window identity preserved");
            check(surface.width != 0 && surface.height != 0, "nonzero framebuffer dimensions");
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
    check(!registry.attachWindow(&second, 5, testSurface(&first_window, 640, 480), true),
          "secondary target requires a primary device target");
    check(registry.attachDefault(&primary, 0), "default framebuffer attached to view zero");
    check(registry.isDefault(&primary) && registry.count() == 1,
          "primary target registered without owned framebuffer");
    check(registry.onlyClient(&primary),
          "primary alone owns all native targets before secondary attachment");
    check(!registry.attachDefault(&third, 1), "second default framebuffer rejected");
    check(!registry.attachDefault(&primary, 0), "duplicate primary rejected");
    check(!registry.attachWindow(&second, 0, testSurface(&first_window, 640, 480), true),
          "view collision rejected");
    check(!registry.attachWindow(&second, 5, testSurface(&first_window, 640, 480), false),
          "secondary rejected when bgfx lacks swap-chain capability");
    check(!registry.attachWindow(&second, 5, testSurface(nullptr, 640, 480), true),
          "null native window rejected");
    check(!registry.attachWindow(&second, 5, testSurface(&first_window, 0, 480), true),
          "zero-size target rejected");
    fail_create = true;
    check(!registry.attachWindow(&second, 5, testSurface(&first_window, 640, 480), true),
          "failed framebuffer creation leaves no target registered");
    check(registry.count() == 1, "failed secondary attach does not change target count");

    // Native-surface contract: display + handle type survive both the
    // primary Init conversion and the secondary framebuffer descriptor.
    int wayland_display = 7;
    BgfxNativeSurface wayland = testSurface(&first_window, 640, 480);
    wayland.handle_type = bgfx::NativeWindowHandleType::Wayland;
    check(!wayland.valid(), "Wayland without display must be refused");
    wayland.display = &wayland_display;
    check(wayland.valid(), "Wayland surface with display is valid");
    const bgfx::SwapChain chain = wayland.swapChain();
    check(chain.nwh == &first_window && chain.ndt == &wayland_display
          && chain.width == 640 && chain.height == 480,
          "native swapchain retains both window and display");
    check(chain.formatDepthStencil == bgfx::TextureFormat::D24S8,
          "secondary swapchain owns a depth attachment for globe geometry");
    check(chain.formatColor == bgfx::TextureFormat::BGRA8 && chain.numBackBuffers == 2,
          "secondary swapchain has explicit default color and buffering");
    const bgfx::SwapChain resized = BgfxNativeSurface::resizeSwapChain(800, 500);
    check(resized.width == 800 && resized.height == 500
          && resized.formatDepthStencil == bgfx::TextureFormat::D24S8,
          "primary resize must not discard the depth attachment");
    check(resized.nwh == nullptr && resized.ndt == nullptr,
          "primary resize does not replace its native window handles");
    bgfx::Init init_settings{};
    check(init_settings.swapChain.formatDepthStencil == bgfx::TextureFormat::D24S8,
          "bgfx Init starts with a D24S8 default depth attachment");
    wayland.configurePrimary(&init_settings);
    check(init_settings.platformData.type == bgfx::NativeWindowHandleType::Wayland
          && init_settings.swapChain.ndt == &wayland_display,
          "primary initialization retains Wayland handle type and display");
    check(init_settings.swapChain.formatDepthStencil == bgfx::TextureFormat::D24S8
          && init_settings.swapChain.formatColor == bgfx::TextureFormat::BGRA8
          && init_settings.swapChain.numBackBuffers == 2,
          "primary initialization preserves default color, depth and buffering");

    fail_create = false;
    check(registry.attachWindow(&second, 5, testSurface(&first_window, 640, 480), true),
          "secondary native framebuffer attached to own view");
    check(!registry.isDefault(&second) && registry.count() == 2,
          "secondary registered as owned target");
    check(bindings.back().view == 5 && bindings.back().framebuffer == 10,
          "second view bound to first created framebuffer");
    check(!registry.attachWindow(&third, 10, testSurface(&first_window, 640, 480), true),
          "same native window cannot be registered twice");
    // A replaced wl_surface (or different native display) must never be
    // resized through an existing swapchain that still owns the old handle.
    BgfxNativeSurface replaced = testSurface(&second_window, 800, 500);
    check(!registry.resizeWindow(&second, replaced),
          "different window handle requires release and reattach");
    BgfxNativeSurface other_display = testSurface(&first_window, 800, 500);
    other_display.display = &wayland_display;
    check(!registry.resizeWindow(&second, other_display),
          "different display requires release and reattach");
    check(create_count == 2, "identity mismatch never allocates a replacement");
    check(!registry.resizeWindow(&primary, testSurface(&first_window, 320, 240)),
          "primary resize delegated to bgfx reset instead");
    check(registry.resizeWindow(&second, testSurface(&first_window, 640, 480)) && create_count == 2,
          "no-op secondary resize avoids new GPU allocation");
    fail_create = true;
    check(!registry.resizeWindow(&second, testSurface(&first_window, 800, 500)),
          "failed secondary resize reported");
    check(destroyed.empty() && bindings.back().framebuffer == 10,
          "failed secondary resize preserves original framebuffer and binding");
    fail_create = false;
    check(registry.resizeWindow(&second, testSurface(&first_window, 800, 500)), "secondary framebuffer resized");
    check(bindings.back().framebuffer == 11 && destroyed.size() == 1
          && destroyed[0] == 10, "new framebuffer bound before old one destroyed");
    check(registry.attachWindow(&third, 10, testSurface(&second_window, 500, 300), true),
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

    int fourth = 8;
    std::vector<BgfxNativeSurface> received;
    BgfxNativeTargetRegistry with_display(
        [&received](const BgfxNativeSurface &native)
        {
            received.push_back(native);
            return bgfx::FrameBufferHandle{std::uint16_t(received.size() + 20)};
        },
        [](std::uint16_t, bgfx::FrameBufferHandle) {},
        [](bgfx::FrameBufferHandle) {});
    check(with_display.attachDefault(&primary, 0), "display test default target");
    check(with_display.attachWindow(&fourth, 5, wayland, true),
          "Wayland secondary surface attaches with display");
    wayland.width = 800;
    check(with_display.resizeWindow(&fourth, wayland),
          "Wayland secondary surface resizes without losing identity");
    check(received.size() == 2 && received[0].display == &wayland_display
          && received[1].display == &wayland_display
          && received[0].handle_type == bgfx::NativeWindowHandleType::Wayland
          && received[1].window == &first_window,
          "full native descriptor propagated to framebuffer creation and resize");
    check(with_display.release(&fourth) && with_display.release(&primary),
          "Wayland display target retired before toolkit surface destruction");

    registry.reset();
    check(destroyed.size() == 3, "reset is idempotent");

    if (failures == 0)
        std::puts("bgfx native render-target lifecycle tests passed");
    return failures == 0 ? 0 : 1;
}
