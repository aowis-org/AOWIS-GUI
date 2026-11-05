# Projects graph — performance measurement

The Projects revision DAG uses the same internal layout and the same
`ProjectGraphBgfxScene` for this benchmark as it does in the GUI. **Benchmark
before adding more optimization machinery.** Step 4 fixes the quadratic layout
ordering updates and culls offscreen edges; no graph navigation behavior changes.

## Run

Build only the benchmark (plus the small integration smoke test):

```bash
cmake --build build-linux-tests --target aowis-project-graph-performance-benchmark -j 8
ctest --test-dir build-linux-tests --output-on-failure -R '^aowis-project-graph-performance-smoke$'
```

The executable emits CSV, suitable for comparing builds or importing into a
spreadsheet. This is a **CPU-only** test; it runs Qt offscreen, does not create
native GPU surfaces, and does not measure GPU frame time or FPS.

```bash
QT_QPA_PLATFORM=offscreen \
  ./build-linux-tests/aowis-project-graph-performance-benchmark \
  --sizes=1000,10000 --repeats=3 \
  | tee /tmp/aowis-project-graph-benchmark.csv
```

The 100,000-revision stress case is **not necessary for routine validation**.
If it is ever required, use a hard wall-clock limit and a single scene sample:

```bash
QT_QPA_PLATFORM=offscreen timeout 30s \
  ./build-linux-tests/aowis-project-graph-performance-benchmark \
  --stress --repeats=1 \
  | tee /tmp/aowis-project-graph-stress.csv
```

The benchmark reports size-specific progress to stderr and refuses a 100k
run when the preceding 10k layout required more than 2.5 seconds. The timeout
is still needed because a single large layout cannot be interrupted internally.
Do **not** leave a high-cost stress test unbounded and unattended.

Run on the same machine, Qt build, and compiler configuration when comparing
results. A RelWithDebInfo or Release build is representative; Debug timings
are not. If a stress test stalls or exhausts memory, record that result
rather than assuming it passed.

## Fixture and measured stages

Synthetic data has two database roots, eight separate projects, one editable
HEAD per project, and the requested number of revisions split among those
projects. It includes normal ancestry, periodic side-branch forks and merges.
IDs, titles and branch labels are deterministic. **It does not claim to
simulate every possible giant DAG or database response.** In particular,
one project with 100k commits might behave differently from eight projects
with 12.5k commits each.

| CSV field | Measurement |
| --- | --- |
| `revisions`, `nodes` | Requested immutable revisions, actual total including structural cards and HEADs |
| `generate_ms` | Synthetic fixture construction (not a database operation) |
| `layout_ms` | `ProjectGraphPlacement::place` on all graph nodes, including layered DAG routing |
| `scene_median_ms`, `scene_max_ms` | `ProjectGraphBgfxScene::build` (CPU staging) across shifted viewports |
| `visible_cards`, `culled_cards` | Counts returned by the production scene builder, from the last sample |
| `ancestry_edges` | Ancestry routes processed by production scene builder |
| `vertices`, `indices`, `overflow` | Last emitted HUD draw-list sizes; whether a sample exceeded its vertex budget |
| `after_generate_rss_kib`, `after_layout_rss_kib`, `peak_rss_kib` | Linux process resident sizes; `-1` if unavailable |

The scene sample includes Qt text rasterization/atlas updates as needed, card
culling and curve tessellation. It **does not** include bgfx command submission,
swapchain presentation, database fetches, QWidget event dispatch, or actual
input/picking. A frame may be slower than scene staging and GPU upload of a
large draw list may exceed platform buffer budgets. No GPU performance
conclusions should be drawn from the CPU test alone.

## Decision rules

1. **First** examine layout cost separately from repeated scene preparation.
   Layout should run only when the graph topology changes; ordinary panning
   should not trigger it.
2. If `scene_median_ms` or `scene_max_ms` grows with the *total* graph size
   despite a small visible set, consider viewport culling of edges and
   reducing full-graph hash/route work per navigation frame.
3. If `overflow=1`, the renderer's transient staging budget is exceeded;
   this requires a correctness/performance fix, not merely a lower FPS target.
4. If the CPU benchmark remains within budget, use native Vulkan and OpenGL
   profiling on a realistic large DVMDB project before implementing caches,
   persistent GPU buffers, LOD, or a spatial index.
5. Do not claim 60 FPS from the offscreen CPU result. Measure actual viewport
   frame pacing, responsiveness, memory, and initial load on the target GPU.

The benchmark is **measurement infrastructure**. It should not become an
always-running or 100k-node CTest gate. The smoke CTest uses a 64-revision
fixture to detect broken wiring quickly.

## Scaling findings from 2026-11-05

Before the layout correction, an actual 10k-revision dataset took approximately
10.8–10.9 **seconds** to lay out and 180–196 **milliseconds** per CPU scene
preparation, despite only ten visible cards. The 100k run was interrupted
because it saturated one CPU core without timely completion. This is a real
CPU bottleneck, not evidence that the bgfx GPU drawing backend is slow.

The layered layout was performing an all-rank `updateOrder()` after sorting
*each* individual rank; for long linear histories this was quadratic. It now
updates only the affected rank and skips crossing-reduction sweeps when the
current crossing count is already zero (the global optimum).

The GPU scene formerly tessellated ancestry/structural links outside the
viewport even though it culled their cards. It now conservatively rejects
out-of-view edge control-point hulls before emitting draw geometry, while
retaining routes which cross the viewport with both endpoints outside.

These corrections should be measured before attempting spatial indexing,
persistent geometry, or parallel layout. In particular, the scene builder
still performs an O(N) node lookup and route scan per frame; further work
may be needed if the post-fix 10k measurements remain too high.
