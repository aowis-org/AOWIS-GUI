# Projects graph — performance measurement

The Projects revision DAG uses the same internal layout and the same
`ProjectGraphBgfxScene` for this benchmark as it does in the GUI. **Benchmark
before adding more optimization machinery.** The layout avoids quadratic
ordering work, and the GPU scene retains a spatial index across camera moves.
Navigation, layout semantics and drawing order are unchanged.

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
| `index_build_ms` | One-time spatial-index construction on a graph/layout replacement; excluded from pan-frame scene timings |
| `scene_median_ms`, `scene_max_ms` | Reused-index `ProjectGraphBgfxScene::build` (CPU staging) across shifted viewports |
| `visible_cards`, `culled_cards` | Counts returned by the production scene builder, from the last sample |
| `ancestry_edges` | Total ancestry routes in the indexed graph (not all emitted every frame) |
| `indexed_candidates` | Number of cards, folds, and routes passing the spatial query on the last scene pass (before exact view clipping) |
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
2. If `scene_median_ms` or `scene_max_ms` grows with total graph size
   despite a small visible set, compare `indexed_candidates` and graph
   topology. The BVH conservatively retains long routes crossing a viewport,
   so pathological giant edge bounds can still cause genuine extra work.
3. If `overflow=1`, the renderer's transient staging budget is exceeded;
   this requires a correctness/performance fix, not merely a lower FPS target.
4. If the CPU benchmark remains within budget, use native Vulkan and OpenGL
   profiling on a realistic large DVMDB project before introducing persistent
   GPU buffers, new levels of detail, or parallel layout.
5. Do not claim 60 FPS from the offscreen CPU result. Measure actual viewport
   frame pacing, responsiveness, memory, and initial load on the target GPU.

The benchmark is **measurement infrastructure**. It should not become an
always-running or 100k-node CTest gate. The smoke CTest uses a 64-revision
fixture to detect broken wiring quickly.

## Measured CPU performance (5 November 2026)

The current renderer uses layered-DAG ordering without all-rank updates for
every individual rank, conservative offscreen link culling, and a retained
BVH for graph-space nodes and curves. A camera movement reuses the index;
only displayed-graph/layout changes rebuild it. The original painter order is
preserved, and viewport-crossing ancestry routes remain eligible even when
neither endpoint is visible.

On the user's Linux GUI test build, using the benchmark's synthetic fixture,
three scene passes and ten visible cards:

| Revisions | Layout | One-time index | Median scene | Index candidates | GPU vertices | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 43.4 ms | 8.0 ms | 4.1 ms | 15 | 4,524 | 65,036 KiB |
| 10,000 | 403.2 ms | 91.5 ms | **3.5 ms** | 15 | 4,524 | 123,688 KiB |

The 10k layout and index-build costs are **not per-pan-frame costs**. Since
the 10k frame visits only 15 candidate objects and generates the same small
draw list as the 1k frame, no additional spatial-index or persistent-buffer
framework is currently justified by these tests. Layout/build responsiveness
on even larger or unusually dense graphs can be evaluated if real project
workloads make it relevant. A previous unbounded 100k run stalled and was
terminated; it must not be used as a routine test.

These results come from CPU-side layout, culling, atlas and draw-list staging.
They do **not** establish 60 FPS, GPU draw performance, full native interaction
latency or multi-platform release acceptance. Profile native GPU frame pacing
only when a realistic user-visible problem warrants it.
