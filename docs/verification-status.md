# GUI implementation and verification status

**Source basis:** general GUI/DVMDB product audit from 5 November 2026;
renderer ownership cross-checked against the 6 November 2026 GUI and Render
snapshots. The verification evidence below states which later builds were
actually reported, rather than treating the existence of test source as a
successful test run. This is **not** a release sign-off.

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

## Renderer implementation and reported verification (6 November 2026)

See [Rendering architecture](rendering-architecture.md),
[Shared renderer integration](renderer-extraction-readiness.md), and the
Render repository's `docs/map-engine-boundary.md` / `docs/roadmap.md`.

| Area | Current source state | Evidence reported | Acceptance still required |
| --- | --- | --- | --- |
| Standalone Render | Five CMake targets; production 2D network and globe GPU code, terrain buffers/topology and sky/heatmap GPU ownership | User reported **19/19** standalone Render tests passing after globe link extraction; subsequent extraction slices have focused or local/mock validations only | Full standalone production+test build after the latest GPU changes |
| GUI integration | Pinned Render gitlink, explicit checkout verifier, production link test, strict build-before-CTest gate | User reported complete GUI compile/link followed by **83/83** passing tests after globe link extraction; subsequent terrain GPU patches lack an equivalent user build log | Complete GUI build-gated suite on newest source/gateway revisions |
| Globe scene data | Render owns CPU link/flow/X-Ray algorithms and invalidation policy; GUI owns retained scene and terrain queries | Earlier GUI suite covers globe origin, draping and drawing boundaries; isolated Render tests added | Full runtime visual parity, editor picking and terrain-backed networks |
| Terrain, sky, heatmap | Render owns shared GPU handles/submission; GUI owns DEM/imagery data pipeline and tile residency decisions | Recording-bgfx mock tests and source-boundary checks for newer slices; not an independent native GPU sign-off | OpenGL/Vulkan terrain LOD, skirting, partial DEM updates, wireframe, caps, sky and heatmap |
| Device and native window | Shared device/client retirement and swapchain registry; Qt adapter stays in GUI | Focused lifecycle tests and earlier native smoke checks reported | Native X11/Wayland swapchain resize/reparent, shutdown/recreation, WebAssembly and Mobile |

The latest proposed common-engine extraction (DEM decode, quadtree/LOD,
terrain mesh scheduling, tile streaming, imagery residency and terrain
queries) is **future implementation work**, not a capability of the current
Render library. Provider configuration and server transport stay in the host.

### Focused renderer regression

For complete production acceptance use `./compile_linux_tests.sh`, which
builds the complete GUI and every test executable before invoking CTest.
The following is a **focused** renderer subset only; its build and CTest
commands must be chained with `&&` so old test binaries cannot run after
compilation failure.

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
  aowis-project-revision-graph-tests -j 8 && \
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
