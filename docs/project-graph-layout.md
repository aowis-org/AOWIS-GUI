# Projects graph — HEAD cards and revision ancestry

This describes the current Projects graph and its deliberate separation of
**writable HEAD cards** from **immutable revisions**. The native DVMDB
`headRevisionId` is the branch's *committed* tip (null for unborn), while the
GUI HEAD card represents the branch's working tip. The HEAD card is not an
extra immutable revision record. See [Projects workspace](projects-workspace.md)
and the DVMDB [version-control contract](../../AOWIS-SERVER-DVMDB/docs/version-control-semantics.md).

## Card hierarchy

```text
Server
└── Project: Water Network
    ├── HEAD                        Branch: main (unborn)
    └── HEAD                        Branch: experiment (unborn)
```

There are **no separate branch cards**. A user can create multiple independent
unborn branches directly in a project, each with its own HEAD. Such HEADs
connect structurally to the Project card and display `No commits yet`.

Once revisions are committed:

```text
Server → Project → Revision R1 (Branch: main)
                       ├── HEAD (Branch: main)
                       └── HEAD (Branch: experiment)
```

The HEADs reference the selected immutable base; creating a branch at R1
does not produce another R1. When independent subsequent revisions are
committed, real ancestry edges express their divergence. Immutable R1
remains **one** card even if several branches inherit it. The GUI can show
`Branches: main, experiment` for a shared revision when useful. Never label a
revision `Branch: Unknown` merely because a single historical owner was not
included in a partial graph response; use actual referencing branches or an
explicit unreferenced status.

## Edge semantics

- **Revision → Revision:** only a real immutable parent/child relationship
  from the DVMDB revision record. Merges have up to two ordered parents.
- **Revision → HEAD:** the branch's editable tip, based on that committed
  revision. Not a new commit or immutable ancestry edge.
- **Project → unborn HEAD:** organizational connection where no committed
  base exists.
- **Server → Project:** organization, not model ancestry.

Never invent a branch-creation ancestry revision or duplicate immutable
shared ancestors. Branch names are mutable display metadata; UUIDv7s are
stable identities. The revision SHA-256 is an integrity digest, not a key.

## Selection and Inspector routing

| Click | Destination |
|---|---|
| Server / Project card | Database / Project Inspector |
| Writable HEAD card | HEAD Inspector (`Overview | HEAD | Branch`) |
| Immutable revision card | Revision Inspector (`Overview | Revision | Branch | Ancestry`) |
| Named branch in left tree | Branch-management Inspector, not HEAD |

Clicking HEAD never opens the old Branch Inspector accidentally. Selecting a
revision opens its historical snapshot without making it editable. To edit
that snapshot, create a branch explicitly from the selected revision.

## Layout and camera

The renderer uses the internal layered layout (`src/tabs/project_graph_placement.h`
and related graph code), not Graphviz. Server cards sit at the same top
rank; projects and their history follow top-down. The two presentation
filters show the full graph or the editable HEAD tips as appropriate.
Fit happens for initial display or on explicit user request; later camera
pan/zoom and dock changes should not unexpectedly reset the user's view.
Revision and HEAD hit-testing must use the transformed viewport geometry,
including after keyboard panning and a delayed page resize.

## Acceptance examples

1. Brand-new project: its default `main` has a selectable HEAD, no immutable
   revision, and the graph shows a structural Project → HEAD connection.
2. Add an independent unborn branch: two distinct HEAD cards, different
   branch UUIDs and no invented revision.
3. Commit `main`: one immutable root revision, main's new editable HEAD
   connected to it; reopened draft can read the committed base network.
4. Branch from the root revision: a second HEAD points to the **same** root,
   without duplicating it.
5. Divergence/merge: ancestry follows only the real parent IDs; branch
   provenance does not create fictitious DAG edges.
6. Clicking HEAD, revision, project and tree branch opens the correct
   Inspector after pan, resize, filtering, and refresh.

## Rendering implementation

The **native Linux Projects graph uses bgfx by default** on the tested Qt
`xcb` (X11) and `wayland` platforms. The existing QWidget continues to own
the graph's layout, camera, inertial navigation, zoom, transformed picking,
selection and Inspector signals; only its presentation is delegated to
`ProjectGraphBgfxViewport`. `ProjectGraphBgfxScene` prepares the same nodes,
structural/ancestry/HEAD routes, fold markers and text atlas as GPU draw data.
Text atlas rasterization follows zoom and device pixel ratio to avoid scaling
up low-resolution glyphs on zoom-in.

**Device startup is independent of Maps:** opening Projects first initializes
bgfx against the graph's native window (default swapchain, view 0). Opening
Maps first allows the graph to use an independent secondary swapchain and
view ID from the shared bgfx device. Hiding a graph that owns the device
releases it so Maps can become the new owner. A single bgfx device exists in
the process, and the established Maps workspace retains its one shared map
surface and five-mode behavior. This is not yet three independent maps.

**Fallback:** `AOWIS_PROJECTS_BGFX_GRAPH=0` starts the GUI with the historical
QPainter graph. The Qt `offscreen`/`minimal` platforms (including headless
CTest) and non-Linux platforms continue to use QPainter until their native
bgfx surface adapters are verified. Unsupported swapchains, framebuffer
allocation failures, or unavailable HUD shaders also fall back to QPainter.
For manual comparison, use:

```bash
./build-linux-tests/aowis-server-gui
AOWIS_PROJECTS_BGFX_GRAPH=0 ./build-linux-tests/aowis-server-gui
```

The `aowis-project-graph-render-policy` regression test verifies these
platform and fallback defaults independently of Qt/GPU; the existing
`aowis-project-revision-graph` and `aowis-project-graph-bgfx-scene` tests
continue to validate graph behavior and GPU-ready scene geometry.

**Remaining work:** Verify that the default GPU path survives real Vulkan and
OpenGL tab switching, resized docks, selection, keyboard/mouse navigation and
shutdown. The transient CPU draw list is still rebuilt on navigation redraws;
spatial indexing, persistent GPU geometry, batching and multi-scale LOD are
reserved for the separate large-graph performance phase (Step 4). The
multi-map demonstration (Step 5) and standalone renderer-library extraction
(later, explicitly outside this batch) are not part of this change.

## Performance verification

The independent [Projects graph performance benchmark](project-graph-performance.md)
measures the existing layered DAG placement, CPU-side bgfx scene preparation,
visibility/culling and transient draw-list sizes at 1k/10k and optionally
100k revisions. The benchmark does not initialize bgfx or claim GPU frame
rates. Profile before introducing any additional graph optimization.
