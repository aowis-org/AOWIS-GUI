#pragma once

#include <string_view>

// Projects graph presentation selection, independent of Qt/GPU so a headless
// unit test can protect the application default and its safe fallback.
//
// The embedded native bgfx child has been validated on Linux X11/Wayland.
// Until other platform adapters are proven, do not turn on their GPU path
// merely because bgfx itself supports the graphics API.
namespace ProjectGraphRenderPolicy
{
inline bool useBgfx(std::string_view platform, std::string_view setting)
{
    // Offscreen/minimal Qt environments must never construct a native GPU
    // child. This includes CI even when AOWIS_PROJECTS_BGFX_GRAPH=1.
    const bool native_window = platform == "xcb"
        || platform == "wayland" || platform.starts_with("wayland-");
    if (!native_window)
        return false;

    // Explicit rollback switch for troubleshooting or regressions. An absent
    // setting enables the production GPU graph on the validated platforms.
    if (setting == "0" || setting == "off" || setting == "false"
        || setting == "no")
        return false;
    return true;
}
}
