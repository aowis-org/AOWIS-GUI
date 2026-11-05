#include "tabs/project_graph_render_policy.h"

#include <cstdio>
#include <string_view>

namespace
{
bool require(bool condition, const char *description)
{
    if (!condition)
        std::fprintf(stderr, "project_graph_render_policy: %s\n", description);
    return condition;
}
}

int main()
{
    using ProjectGraphRenderPolicy::useBgfx;

    // Both desktop Qt window systems covered by the two-view GPU probe use
    // bgfx by default; an empty environment variable is also the default.
    if (!require(useBgfx("xcb", ""), "X11 default must select bgfx")) return 1;
    if (!require(useBgfx("wayland", ""), "Wayland default must select bgfx")) return 1;
    if (!require(useBgfx("wayland-egl", ""), "Wayland variant must select bgfx")) return 1;
    if (!require(useBgfx("xcb", "1"), "explicit 1 must select bgfx")) return 1;
    if (!require(useBgfx("wayland", "on"), "explicit on must select bgfx")) return 1;

    // Keep an easy deterministic escape hatch for runtime regressions.
    for (const std::string_view setting : {"0", "off", "false", "no"})
    {
        if (!require(!useBgfx("xcb", setting), "X11 fallback switch failed")) return 1;
        if (!require(!useBgfx("wayland", setting), "Wayland fallback switch failed")) return 1;
    }

    // Never try to construct a native child window in headless CTest or on
    // not-yet-validated platforms, even with an explicit GPU request.
    for (const std::string_view platform : {"offscreen", "minimal", "vnc",
                                           "windows", "cocoa", "eglfs", ""})
    {
        if (!require(!useBgfx(platform, ""), "unvalidated platform got GPU default")) return 1;
        if (!require(!useBgfx(platform, "1"), "unvalidated platform accepted GPU override")) return 1;
    }
    return 0;
}
