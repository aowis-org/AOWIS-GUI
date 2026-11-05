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

On native Linux using Qt `xcb` (X11) or `wayland`, **bgfx is the default
Projects graph presenter**. `ProjectRevisionGraph` (QWidget) still owns the
actual graph model, internal layered layout, camera, inertia, zoom, hit-testing,
selection and Inspector signals. `ProjectGraphBgfxViewport` attaches a native
child window and renders `ProjectGraphBgfxScene` GPU geometry and text through
the shared bgfx device and frame coordinator. Text is rasterized at a zoom-
and device-pixel-ratio-appropriate resolution, rather than magnifying 1× glyphs.

The GPU scene uses a **retained graph-space BVH**, built from nodes, fold
markers, structural/ancestry/HEAD routes when the displayed graph changes.
Ordinary pan/zoom and selection reuse that index. Viewport queries conservatively
include crossing curves even when both endpoints are offscreen; per-frame GPU
geometry contains only relevant visible cards and routes. The original drawing
order and graph semantics remain unchanged. No Graphviz dependency is required.

If Projects opens first, its native window may initialize the default bgfx
device. If Maps opens first, Projects can attach to the already-running device
through a secondary native swapchain and an allocated view ID. Hiding Projects
when it owns the device releases it so Maps can take ownership. It must not
initialize another independent bgfx instance. See the
[rendering architecture](rendering-architecture.md) for lifecycle details.

**QPainter fallback:** `AOWIS_PROJECTS_BGFX_GRAPH=0` explicitly disables the
GPU presenter. Qt `offscreen` and `minimal`, non-Linux platforms and failed
native swapchains/HUD shaders use QPainter; this is not a fallback for the
Maps renderer. Test both paths with:

```bash
./build-linux-tests/aowis-server-gui
AOWIS_PROJECTS_BGFX_GRAPH=0 ./build-linux-tests/aowis-server-gui
```

The selection policy, graph geometry and interactive behavior have focused
regressions: `aowis-project-graph-render-policy`,
`aowis-project-graph-bgfx-scene` and `aowis-project-revision-graph`.

## Performance verification

The [CPU performance benchmark](project-graph-performance.md) uses the same
layered-DAG placement and retained GPU scene builder as the GUI. At 10,000
synthetic revisions the most recent reported median scene preparation was
about **3.5 ms** with ten visible cards and 15 spatial-index candidates;
layout and index construction are measured separately. The benchmark does
**not** measure native bgfx frame time or FPS.
