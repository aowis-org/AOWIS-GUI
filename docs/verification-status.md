# GUI implementation and verification status

**Source inspected:** uploaded `gui(20261105-191917).zip` (5 November 2026).
The renderer audit below reflects that source, including the internal static
library, native-surface abstraction, focus-loss cancellation, and D24S8
swapchain fix.
This page distinguishes functionality visible in source from tests whose
execution has actually been reported. It is **not** a release sign-off.

## Implemented in this GUI snapshot

- Projects/Servers inspector, personal working database selection, project,
  branch, HEAD and immutable revision navigation; branch/revision deletion
  paths through HTTP; private autosave and local Commit.
- Canonical hydraulic MODEL transactions for 18 entity collections and
  root metadata, with durable pending Save queue and fenced writer.
- History dock with independent paginated first-parent Commit markers,
  separately paginated archived MODEL actions, Undo/Redo and eligible direct
  jumps, plus verified editing forks from archived positions.
- Routine History navigation applies typed **sparse** before/after changes,
  membership moves and SHA-256 verification without a full-network GET.
- Same-operation-UUID HTTP retries, long-outage reconciliation, hash and
  exact cursor checks, writer-lease renewal, and last-operation sparse replay.
- `PersonalHistoryNavigationIntentStore` persists the navigation's source
  snapshot, journal, IDs and coordinate before POST. On GUI restart the
  session can reconcile that *pending* operation without first hydrating the
  complete network; malformed or conflicting intents fail closed.
- Navigation remains one-unambiguous-network only; DVMDB multi-network
  storage is unaffected. Unprovable unborn membership deletions are refused.

## Evidence and outstanding acceptance

| Evidence | Status | Boundary |
|---|---|---|
| GUI and DVMDB sparse/reconciliation patches | User reported their tests green before the restart-intent patch | Does not verify the newer crash/restart-intent change |
| DVMDB HTTP History deletion endpoints | User reported proceeding without problems after the fix | Not equivalent to an independently recorded full destructive-path test report |
| GUI `testHistoryRestartAfterLostReply` in `personal_working_copy_client_test.cpp` | Test code is present for server-completed, never-published, modified-destination, invalid-intent and mismatched-cursor scenarios | Test execution for the uploaded snapshot has **not been supplied in this conversation** |
| Native C++/Qt compilation of this documentation snapshot | Not run in this sandbox (Qt6 development dependencies unavailable) | Source check is not build/run verification |
| Real GUI crash + separate DVMDB server, powercut/ENOSPC, Standalone, WASM persistent intent root | Outstanding acceptance | Must be exercised separately before release guarantees |

The restart-safe implementation milestone can be closed at **source-feature
completion**. Operational acceptance still requires real process termination
at the specified boundaries. This must not be confused with a remaining
requirement to redesign Undo/Redo.

## Focused GUI regression commands

Run from a configured `AOWIS-SERVER-GUI` build tree:

```bash
cmake --build build-linux-tests \
  --target aowis-personal-working-copy-client-tests \
           aowis-history-dock-tests \
           aowis-server-gui -j 8
ctest --test-dir build-linux-tests --output-on-failure \
  -R '^(aowis-personal-working-copy-client|aowis-history-dock)$'
```

For DVMDB's server-side behavior, rebuild and test the actual server binary
before running GUI live integration, because a stale server can lack newly
added HTTP routes:

```bash
cmake --build build-linux-tests \
  --target aowis-server-dvmdb aowis-dvmdb-http-tests -j 8
ctest --test-dir build-linux-tests --output-on-failure \
  -R '^aowis-dvmdb-http-tests$'
```

See the [remaining History acceptance](history-roadmap.md) and
[upcoming GUI–DVMDB capabilities](dvmdb-integration-roadmap.md).

## Renderer implementation and reported verification (5 November 2026)

See [Rendering architecture](rendering-architecture.md) for the operative
contracts, [Renderer library readiness](renderer-extraction-readiness.md) for
the actual source/link boundary, and [graph performance](project-graph-performance.md)
for reproducible CPU benchmarks. This is **not a release sign-off**.

| Area | Implemented in current source | Evidence actually reported | Still to qualify |
| --- | --- | --- | --- |
| Shared bgfx device, views and native targets | Device generations, client-retirement barrier, view reservations and swapchain registry | Mock-C++ infrastructure tests passed during preparation; user reported lifecycle changes fine | Complete post-split targeted CTest run, shutdown/restart under real native windows |
| Native-surface boundary | Qt adapter to Qt-independent window/display/type/pixels descriptor | Mock-target tests passed during preparation | Real X11 and Wayland native-surface replacement and resize stress |
| Internal renderer library | `aowis-render-core` static target (`AOWIS::RenderCore` alias), GPU helpers, embedded shader resources, executable-independent link test | CMake shader configuration and patch application validated while preparing change | User's full Qt build, `aowis-render-core-link` CTest and shader loading with both backends after the split |
| 3D terrain depth | Primary Init preserves D24S8; secondary target and resize request D24S8 | User saw vertical skirt stripes after native-surface/library work, then confirmed the fix resolved them | Full native Vulkan/OpenGL globe pan, orbit, resize, LOD transition and return-from-hide regression |
| Map focus loss | Navigation, pointer grabs and editor drags cancelled on FocusOut/WindowDeactivate | Focus policy test code and patch checks were supplied; no focused native test log provided | Native X11/Wayland Alt+Tab during pan, orbit, joystick and entity drag |
| Projects GPU graph | Native Linux bgfx default; headless/unsupported QPainter; retained BVH, zoom-aware text | Projects-first and Maps-first Vulkan attachment logged; project graph/scene/performance smoke selection passed 3/3 | Real GPU frame pacing and platform acceptance |
| Graph CPU staging | Retained spatial index used during pan/zoom | 10k: ~403 ms layout, ~91 ms one-time BVH build, ~3.5 ms median scene staging, 15 candidates | No further optimization needed on current evidence; GPU FPS not measured |
| Three independent 2D maps | Read-only Linux diagnostic, independent cameras, shared device/geometry, view IDs | Three visible independently pannable/zoomable maps on Vulkan; forced OpenGL run confirmed `bgfx / OpenGL 4.3`; attach/detach logged | Complete new native replay following final fixes; non-Linux and QtQuick multi-view unsupported/unverified |

### Focused renderer regression

Run in the configured GUI repository. These tests are named in the current
`CMakeLists.txt`; keep them focused rather than rebuilding all unrelated
submodules for a documentation check.

```bash
cmake --build build-linux-tests --target \
  aowis-render-core-link-tests \
  aowis-server-gui \
  aowis-bgfx-device-manager-tests \
  aowis-bgfx-frame-coordinator-tests \
  aowis-bgfx-native-target-registry-tests \
  aowis-bgfx-three-view-camera-tests \
  aowis-map-input-focus-policy-tests \
  aowis-project-graph-render-policy-tests \
  aowis-project-graph-bgfx-scene-tests \
  aowis-project-revision-graph-tests -j 8

ctest --test-dir build-linux-tests --output-on-failure \
  -R '^(aowis-render-core-link|aowis-bgfx-device-manager|aowis-bgfx-frame-coordinator|aowis-bgfx-native-target-registry|aowis-bgfx-three-view-camera|aowis-map-input-focus-policy|aowis-project-graph-render-policy|aowis-project-graph-bgfx-scene|aowis-project-revision-graph)$'
```

### Native GPU acceptance

On Linux, use the same geographical location and tile cache for the two
backends. Disable Projects bgfx for the forced backend runs, because the
**first bgfx owner determines the actual renderer**:

```bash
AOWIS_PROJECTS_BGFX_GRAPH=0 AOWIS_BGFX_RENDERER=vulkan \
  AOWIS_BGFX_THREE_VIEW_PROBE=1 ./build-linux-tests/aowis-server-gui
AOWIS_PROJECTS_BGFX_GRAPH=0 AOWIS_BGFX_RENDERER=opengl \
  AOWIS_BGFX_THREE_VIEW_PROBE=1 ./build-linux-tests/aowis-server-gui
```

In each run, verify the backend in `Map Monitor renderer`, three-map
independent navigation/resize/close, the 3D globe's depth-tested tile skirts,
Map ↔ Projects handoff, and normal application shutdown. Without diagnostic
windows, also check key/button focus loss during map or editor interaction.
The optional three-map windows are **2D only**: test 3D terrain in the main map,
not in those two windows.

A real GPU screenshot/interactive check is required for terrain depth; the
headless mocks verify descriptor configuration, not actual z-buffer contents.
No Windows, WebAssembly, QtQuick or mobile GPU acceptance is implied.
