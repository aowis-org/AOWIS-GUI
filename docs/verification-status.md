# GUI implementation and verification status

**Source basis:** GUI/DVMDB product audit of 5 November 2026; renderer
section updated to the 7 November 2026 GUI and Render sources. The verification evidence below states which later builds were
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
| GUI `testHistoryRestartAfterLostReply` in `personal_working_copy_client_test.cpp` | Test code is present for server-completed, never-published, modified-destination, invalid-intent and mismatched-cursor scenarios | Part of the GUI suite (77/77 reported on 7 November 2026); no run against a real crashed GUI and server is recorded |
| Native C++/Qt build of the current source | User reported a complete build and 77/77 tests on 7 November 2026 | A build on Linux only |
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

## Renderer implementation and reported verification

See [Rendering architecture](rendering-architecture.md),
[Render integration](render-integration.md) and Render's
`docs/roadmap.md`.

| Area | Current source state | Evidence reported | Acceptance still required |
| --- | --- | --- | --- |
| Render library | Complete map engine: 2D and globe drawing, imagery and terrain streaming, terrain pipeline, HUD, labels, picking, REST client (`AOWIS::RenderMapRest`), host-context mode for Qt Quick | Standalone build-gated suite run by the user during the extraction; no count recorded here for the latest Render revision | Fresh `./compile_linux_tests.sh` in Render on the pinned revision |
| GUI integration | `MapBgfxSurface` hosts `MapBgfxMapView`; GUI keeps transports, settings, network snapshot and symbology, editor overlay | User reported **77/77** GUI tests after the REST client and provider catalog moved to Render (7 November 2026) | Re-run after every Render gitlink update |
| Globe and terrain | Quadtree, DEM meshes, sparse updates, sky, heatmap, network on the globe, all in Render | Recording-bgfx mock tests and boundary checks; earlier native smoke checks | OpenGL/Vulkan terrain LOD and skirts, partial DEM updates, wireframe, caps, sky and heatmap |
| Device and native windows | One device, client retirement, swapchain registry, Qt native-surface adapter in Render | Focused lifecycle tests; three-map and Projects handoff observed natively on Vulkan and OpenGL 4.3 | X11/Wayland resize and reparent, shutdown and recreation, Windows, WebAssembly |
| AOWIS-GUI-Mobile | Same engine in Qt Quick's EGL context (`HostContext`) | Linux and Android builds in progress | Android device acceptance |

### Focused renderer regression

For complete acceptance use `./compile_linux_tests.sh`, which builds the
whole GUI and every test before CTest. The following is a focused subset; its
build and CTest commands are chained with `&&` so old test binaries never run
after a failed build.

```bash
cmake --build build-linux-tests --target \
  aowis-render-core-link-tests \
  aowis-server-gui \
  aowis-bgfx-device-manager-tests \
  aowis-bgfx-frame-coordinator-tests \
  aowis-bgfx-native-target-registry-tests \
  aowis-bgfx-three-view-camera-tests \
  aowis-project-graph-render-policy-tests \
  aowis-project-graph-bgfx-scene-tests \
  aowis-project-revision-graph-tests -j 8 && \
ctest --test-dir build-linux-tests --output-on-failure \
  -R '^(aowis-render-core-link|aowis-bgfx-device-manager|aowis-bgfx-frame-coordinator|aowis-bgfx-native-target-registry|aowis-bgfx-three-view-camera|aowis-project-graph-render-policy|aowis-project-graph-bgfx-scene|aowis-project-revision-graph)$'
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
No Windows, WebAssembly or Mobile GPU acceptance is implied.
