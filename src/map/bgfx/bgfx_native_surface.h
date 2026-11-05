#ifndef AOWIS_BGFX_NATIVE_SURFACE_H
#define AOWIS_BGFX_NATIVE_SURFACE_H

#include <bgfx/bgfx.h>

#include <cstdint>

// Platform-independent description of an already-created native window.
// The hosting toolkit owns the window/display: bgfx must release its native
// framebuffer BEFORE the toolkit destroys or replaces either handle.
// Values are device pixels, not Qt logical/widget dimensions. This type has
// no dependency on Qt, QWidget, QWindow, or a specific window system.
struct BgfxNativeSurface final
{
    void *window = nullptr;
    void *display = nullptr;
    bgfx::NativeWindowHandleType::Enum handle_type = bgfx::NativeWindowHandleType::Default;
    std::uint16_t width = 0;
    std::uint16_t height = 0;

    bool valid() const
    {
        return this->window != nullptr && this->width != 0 && this->height != 0
            && (this->handle_type != bgfx::NativeWindowHandleType::Wayland
                || this->display != nullptr);
    }

    bool sameWindow(const BgfxNativeSurface &other) const
    {
        return this->window == other.window && this->display == other.display
            && this->handle_type == other.handle_type;
    }

    bgfx::SwapChain swapChain() const
    {
        bgfx::SwapChain chain{};
        chain.nwh = this->window;
        chain.ndt = this->display;
        chain.width = this->width;
        chain.height = this->height;
        return chain;
    }

    void configurePrimary(bgfx::Init *settings) const
    {
        if (settings == nullptr)
            return;
        settings->swapChain = this->swapChain();
        settings->platformData.type = this->handle_type;
    }
};

#endif // AOWIS_BGFX_NATIVE_SURFACE_H
