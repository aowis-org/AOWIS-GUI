# GUI implementation and verification status

**Source inspected:** uploaded `gui(20261105-105231).zip` (5 November 2026).
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

See [Rendering architecture](rendering-architecture.md) for device/frame/target
contracts, platform defaults, command lines and future boundaries, and
[graph performance](project-graph-performance.md) for reproducible CPU numbers.
These renderer results are separate from the History/DVMDB acceptance table
above.

- On the supplied Linux build, Projects initialized bgfx as the primary
  Vulkan target and also attached as a secondary target after Maps.
- The 10k-revision CPU graph benchmark completed with ~403 ms layout, ~91 ms
  one-time BVH construction and ~3.5 ms median per-frame scene staging; the
  graph/scene/smoke CTest selection passed 3/3.
- The user observed three independent, simultaneously visible 2D maps, with
  independent pan/zoom and clean diagnostic-window attach/detach. The forced
  OpenGL run explicitly reported `bgfx / OpenGL 4.3`; Vulkan was also exercised.
- A complete three-view CTest run, real GPU frame pacing/latency data, and
  native multi-window qualification on Windows or WASM have **not** been
  reported. Linux diagnostics do not establish these other platform results.
- `AOWIS-RENDER` extraction is **deferred**, not present in this source tree.
