#pragma once

#include <array>
#include <cstddef>

// The map workspace's mode contract is independent of Qt, GPU ownership and
// the concrete tool panels. The integer values are the tab/page indices;
// changing the order is an intentional UI migration, not a renderer change.
namespace MapWorkspace
{
enum class Domain
{
    Distribution,
    Drainage,
    Management
};

enum class Activity
{
    Monitor,
    Editor,
    Manage
};

struct ModeInfo
{
    const char *title;
    const char *icon_resource;
    Domain domain;
    Activity activity;
    // Drainage has blank tool panels; management owns tile/terrain tools.
    bool tools_implemented;
};

inline constexpr std::array<ModeInfo, 5> Modes{{
    {"Distribution Monitor", ":/icon/water_distribution_monitor.png",
     Domain::Distribution, Activity::Monitor, true},
    {"Distribution Editor", ":/icon/water_distribution_editor.png",
     Domain::Distribution, Activity::Editor, true},
    {"Drainage Monitor", ":/icon/water_drainage_monitor.png",
     Domain::Drainage, Activity::Monitor, false},
    {"Drainage Editor", ":/icon/water_drainage_editor.png",
     Domain::Drainage, Activity::Editor, false},
    {"Map Management", ":/icon/map_config.png",
     Domain::Management, Activity::Manage, true}
}};

inline constexpr int DistributionMonitor = 0;
inline constexpr int DistributionEditor = 1;
inline constexpr int DrainageMonitor = 2;
inline constexpr int DrainageEditor = 3;
inline constexpr int MapManagement = 4;

constexpr const ModeInfo *modeInfo(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= Modes.size())
        return nullptr;
    return &Modes[static_cast<std::size_t>(index)];
}

// A drainage mode reuses the map/camera but does not render or pick any
// distribution entities. No geometry snapshot is cleared on mode changes.
constexpr bool rendersDistributionNetwork(int index)
{
    const ModeInfo *mode = modeInfo(index);
    return mode != nullptr && mode->domain == Domain::Distribution;
}

constexpr bool isImplemented(int index)
{
    const ModeInfo *mode = modeInfo(index);
    return mode != nullptr && mode->tools_implemented;
}

// Both distribution workspaces use flat entity icons, never 3D models.
constexpr bool useGlobe3dIconModels(int, bool = false)
{
    return false;
}

static_assert(Modes.size() == 5);
static_assert(Modes[DistributionMonitor].activity == Activity::Monitor);
static_assert(Modes[DistributionEditor].activity == Activity::Editor);
static_assert(Modes[DrainageMonitor].domain == Domain::Drainage);
static_assert(Modes[DrainageEditor].domain == Domain::Drainage);
static_assert(Modes[MapManagement].domain == Domain::Management);
static_assert(rendersDistributionNetwork(DistributionMonitor));
static_assert(rendersDistributionNetwork(DistributionEditor));
static_assert(!rendersDistributionNetwork(DrainageMonitor));
static_assert(!rendersDistributionNetwork(DrainageEditor));
static_assert(!rendersDistributionNetwork(MapManagement));
}
