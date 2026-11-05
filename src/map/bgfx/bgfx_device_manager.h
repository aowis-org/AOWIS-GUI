#ifndef AOWIS_BGFX_DEVICE_MANAGER_H
#define AOWIS_BGFX_DEVICE_MANAGER_H

#include <bgfx/bgfx.h>

#include <functional>

// Process-wide bgfx device ownership. Only one initialized device/surface is
// supported at this stage. A hidden surface may relinquish the device for a
// newly shown surface, preserving the existing Maps tab behavior.
//
// This class must be used from the UI/render thread. View allocation, frame
// submission, and multiple simultaneous surfaces are intentionally NOT part
// of this first lifecycle extraction.
class BgfxDeviceManager final
{
public:
    using Visibility = std::function<bool()>;
    using Retire = std::function<void()>;
    using Initialize = std::function<bool(const bgfx::Init &)>;
    using Shutdown = std::function<void()>;

    // Injectable API functions permit GPU-free ownership/lifecycle tests.
    explicit BgfxDeviceManager(Initialize initialize = {}, Shutdown shutdown = {});

    BgfxDeviceManager(const BgfxDeviceManager &) = delete;
    BgfxDeviceManager &operator=(const BgfxDeviceManager &) = delete;

    static BgfxDeviceManager &shared();

    // If another surface owns bgfx, reject takeover while it is visible;
    // otherwise ask it to release its GPU resources and the device first.
    bool prepareFor(const void *requester);
    bool initialize(const void *requester, const bgfx::Init &settings,
                    Visibility visibility, Retire retire);
    bool shutdown(const void *requester);

    bool owns(const void *requester) const;
    bool hasOwner() const;

private:
    Initialize initialize_api;
    Shutdown shutdown_api;
    const void *device_owner = nullptr;
    Visibility owner_visibility;
    Retire owner_retire;
};

#endif // AOWIS_BGFX_DEVICE_MANAGER_H
