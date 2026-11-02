#include "tabs/map_workspace_mode.h"

#include <cstdio>
#include <cstring>

namespace
{
bool expect(bool valid, const char *description)
{
    if (!valid)
        std::fprintf(stderr, "map_workspace_contract: %s\n", description);
    return valid;
}
}

int main()
{
    using namespace MapWorkspace;

    if (!expect(modeInfo(-1) == nullptr && modeInfo(5) == nullptr,
                "invalid tab indices must be rejected"))
        return 1;

    const char *expected_titles[] = {
        "Distribution Monitor", "Distribution Editor",
        "Drainage Monitor", "Drainage Editor", "Map Management"
    };
    const Domain expected_domains[] = {
        Domain::Distribution, Domain::Distribution,
        Domain::Drainage, Domain::Drainage, Domain::Management
    };
    const Activity expected_activities[] = {
        Activity::Monitor, Activity::Editor,
        Activity::Monitor, Activity::Editor, Activity::Manage
    };
    for (int index = 0; index < static_cast<int>(Modes.size()); ++index)
    {
        const ModeInfo *info = modeInfo(index);
        if (!expect(info != nullptr, "missing mode")
            || !expect(std::strcmp(info->title, expected_titles[index]) == 0,
                       "mode label/order changed")
            || !expect(info->domain == expected_domains[index], "incorrect map domain")
            || !expect(info->activity == expected_activities[index], "incorrect map activity")
            || !expect(info->icon_resource[0] == ':', "missing Qt icon resource")
            || !expect(isImplemented(index) == (index < 2 || index == MapManagement),
                       "management implements tools but drainage remains blank"))
            return 1;
    }
    if (!expect(!isImplemented(-1) && !isImplemented(5),
                "invalid modes cannot own tools"))
        return 1;

    // Stage 7: drainage has a viewport but never exposes distribution
    // network rendering, selection or simulation overlays.
    for (int index = 0; index < static_cast<int>(Modes.size()); ++index)
    {
        if (!expect(rendersDistributionNetwork(index) == (index < 2),
                    "only distribution modes may show network"))
            return 1;
    }
    if (!expect(!rendersDistributionNetwork(-1) && !rendersDistributionNetwork(5),
                "invalid modes must never enable the distribution network"))
        return 1;

    // Both distribution 3D views use the same flat entity icons, regardless
    // of the old monitor icon-model preference.
    if (!expect(!useGlobe3dIconModels(DistributionEditor, true),
                "editor must use its original flat globe icons")
        || !expect(!useGlobe3dIconModels(DistributionMonitor, true),
                   "monitor must not render 3D icon models")
        || !expect(!useGlobe3dIconModels(DistributionMonitor, false),
                   "monitor must use flat icons even when models are disabled")
        || !expect(!useGlobe3dIconModels(DrainageMonitor, true)
                       && !useGlobe3dIconModels(DrainageEditor, true)
                       && !useGlobe3dIconModels(MapManagement, true),
                   "basemap-only modes must not inherit distribution icon models"))
        return 1;

    std::puts("Map workspace mode/ownership contract passed");
    return 0;
}
