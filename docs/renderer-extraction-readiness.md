# Shared bgfx renderer: current integration

AOWIS-GUI-Render is now a standalone source repository. The GUI links its
`AOWIS::RenderCore` target, which in turn links `AOWIS::RenderDevice`. The
renderer owns the device/frame/swapchain classes, HUD texture cache, 2D tile
atlas/uploader and all committed shader binaries. It also provides the
minimal bgfx/bx/bimg unity build when the host has not supplied a `bgfx` target.

The GUI retains the Qt Widgets/native-window adapter, map controller, data
repositories, input handling, editor, three-dimensional scene preparation and
Projects DAG view. Those are *not* standalone library APIs yet. Moving the
entire `MapBgfxSurface` into Render today would import GUI lifecycle and
network dependencies rather than produce a Mobile-ready render interface.

## Integration

- A checkout of `AOWIS-GUI-Render` can be a pinned submodule at
  `external/AOWIS-GUI-Render`, or a sibling of `AOWIS-SERVER-GUI` during local
  development. `AOWIS_RENDER_SOURCE_DIR` overrides either path.
- The GUI provides the existing `external/{bx,bimg,bgfx}` vendor sources using
  `AOWIS_RENDER_VENDOR_ROOT` (no second bgfx runtime).
- `AOWIS_RENDER_BUILD_MAP_CORE=ON` enables map GPU code, Qt resources, and
  shader family embedding; `AOWIS::RenderCore` is the executable's dependency.
- `AOWIS_BGFX_RENDERERS` retains its OS-specific defaults and override.
- Shader regeneration uses `-DAOWIS_BGFX_SHADER_TOOLS=ON`; no shaderc needed
  for normal Linux/Windows/macOS/WebAssembly builds.

## Existing acceptance contracts

- The renderer static-link test must open a committed shader resource via
  `MapBgfxBackend::initializeEmbeddedShaders()` without linking GUI sources.
- Device owner retirement, generation safety, view reservation and secondary
  swapchain lifecycle have GPU-free tests in the GUI test suite.
- Qt `QWindow` captures native handles; Render owns them only as borrowed
  descriptors. Primary and secondary window depth must stay D24S8. Removing
  it causes terrain LOD skirt overdraw during orbit/resize.
- Native and WASM Map/Projects HUD resources keep their original resource path
  `:/aowis/map/bgfx/<family>/<shader>.bin`.
- Localization of HUD labels remains a rendering-input boundary: the GUI owns
  dynamic text changes; the same `MapBgfxHud` context can be translated.

## Qualification still required on real devices

Reconfigure GUI from a clean build dir, build and execute the five device/view
lifecycle tests plus `aowis-render-core-link`, HUD/atlas/graph smoke tests,
then test Linux Vulkan and OpenGL, Android OpenGLES, WebAssembly WebGL 2,
Map↔Projects switching, resize, focus loss and scenegraph/context recreation.
These GPU/platform checks cannot be established by source inspection alone.

Next extraction: move the GPU-neutral 2D/3D scene-frame model and map draw
encoding into Render, expose explicit host callbacks, and have Mobile draw the
same production network/terrain/HUD. Keep Mobile's tested QtQuick EGL context
owner and Android lifecycle controls in Mobile.
