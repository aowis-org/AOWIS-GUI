#ifndef AOWIS_BGFX_QT_NATIVE_SURFACE_H
#define AOWIS_BGFX_QT_NATIVE_SURFACE_H

#include "map/bgfx/bgfx_native_surface.h"

class QWindow;

// Qt hosting adapter. This is the sole native handle discovery path for
// desktop Maps, Projects and diagnostic bgfx windows. Keep this Qt-dependent
// bridge outside the eventual renderer library's device/target classes.
class BgfxQtNativeSurface final
{
public:
    // Capture only while Qt owns a live native window. On Wayland, callers
    // should retry after Expose if the wl_surface is not available yet.
    static BgfxNativeSurface capture(QWindow *window);
};

#endif // AOWIS_BGFX_QT_NATIVE_SURFACE_H
