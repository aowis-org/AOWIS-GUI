#include "map/bgfx/bgfx_device_manager.h"

#include <utility>

BgfxDeviceManager::BgfxDeviceManager(Initialize initialize, Shutdown shutdown)
    : initialize_api(std::move(initialize)), shutdown_api(std::move(shutdown))
{
    if (!this->initialize_api)
        this->initialize_api = [](const bgfx::Init &settings) { return bgfx::init(settings); };
    if (!this->shutdown_api)
        this->shutdown_api = []() { bgfx::shutdown(); };
}

BgfxDeviceManager &BgfxDeviceManager::shared()
{
    static BgfxDeviceManager manager;
    return manager;
}

bool BgfxDeviceManager::prepareFor(const void *requester)
{
    if (requester == nullptr)
        return false;
    if (this->device_owner == nullptr || this->device_owner == requester)
        return true;
    if (!this->owner_visibility || !this->owner_retire || this->owner_visibility())
        return false;

    // Copy before invocation: the callback shuts down and clears the stored
    // callback, which must not destroy the std::function during its own call.
    const Retire retire = this->owner_retire;
    retire();
    return this->device_owner == nullptr;
}

bool BgfxDeviceManager::initialize(const void *requester, const bgfx::Init &settings,
                                   Visibility visibility, Retire retire)
{
    if (requester == nullptr || this->device_owner != nullptr || !visibility || !retire)
        return false;
    if (!this->initialize_api(settings))
        return false;

    this->device_owner = requester;
    this->owner_visibility = std::move(visibility);
    this->owner_retire = std::move(retire);
    return true;
}

bool BgfxDeviceManager::shutdown(const void *requester)
{
    if (!owns(requester))
        return false;
    this->shutdown_api();
    this->device_owner = nullptr;
    this->owner_visibility = {};
    this->owner_retire = {};
    return true;
}

bool BgfxDeviceManager::owns(const void *requester) const
{
    return requester != nullptr && this->device_owner == requester;
}

bool BgfxDeviceManager::hasOwner() const
{
    return this->device_owner != nullptr;
}
