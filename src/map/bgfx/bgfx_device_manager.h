#ifndef AOWIS_BGFX_DEVICE_MANAGER_H
#define AOWIS_BGFX_DEVICE_MANAGER_H

#include <bgfx/bgfx.h>
#include "map/bgfx/bgfx_frame_coordinator.h"
#include "map/bgfx/bgfx_native_target_registry.h"

#include <functional>

// Process-wide bgfx device ownership. The current map UI still owns one
// visible device lease; a hidden map may relinquish it as before.
// This manager also owns the generation-scoped native target registry, which
// prepares for additional swapchains without activating a second map yet.
// All clients must share one bgfx submission thread.
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
    BgfxNativeTargetRegistry &targets() { return this->native_targets; }
    const BgfxNativeTargetRegistry &targets() const { return this->native_targets; }
    BgfxFrameCoordinator &frames() { return this->frame_coordinator; }
    const BgfxFrameCoordinator &frames() const { return this->frame_coordinator; }

private:
    Initialize initialize_api;
    Shutdown shutdown_api;
    BgfxFrameCoordinator frame_coordinator;
    BgfxNativeTargetRegistry native_targets;
    const void *device_owner = nullptr;
    Visibility owner_visibility;
    Retire owner_retire;
};

#endif // AOWIS_BGFX_DEVICE_MANAGER_H
