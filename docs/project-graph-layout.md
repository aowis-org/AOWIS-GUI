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

The interactive Projects revision graph uses the QWidget/QPainter presentation
path by default. Its existing graph layout, input events, first-load fit,
inertial pan/zoom, selection and inspector signals are independent of the
presentation backend.

A GPU-ready graph staging class, `ProjectGraphBgfxScene`, now converts the
same typed nodes and ancestry/structural/HEAD routes into the shared
`MapBgfxHudCanvas` triangle and text-atlas representation. It requires no
second bgfx instance, graphics device, native window or new shaders. The
`aowis-project-graph-bgfx-scene` CPU test checks this representation in a
headless Qt build. An **opt-in Linux native GPU bridge** (`ProjectGraphBgfxViewport`) now embeds
a QWindow inside the existing Projects graph. It reserves a secondary native
swapchain and a unique view ID on the process-global bgfx device, uses the
shared HUD shader/atlas to draw the scene, and submits through the existing
frame coordinator. Events are forwarded to the unchanged graph widget. This
path can be exercised with:

```bash
AOWIS_PROJECTS_BGFX_GRAPH=1 ./build-linux-tests/aowis-server-gui
```

**Step 3B limitations:** this is deliberately a live GPU integration probe,
not yet the default Projects renderer. **Open Maps once to initialize bgfx,
then open Projects.** If the device is not initialized, the GUI remains on
QPainter and will never attempt a second `bgfx::init()`. If the backend does
not support secondary native swapchains, the shader initialization fails or
a GPU target cannot be allocated, the graph remains on QPainter. On leaving
the Projects view, the native target and its GPU resources are released. The
graph's node/edge data are rebuilt from CPU on each navigation redraw.

The remaining step-3 work is moving device startup out of the map viewport so
the graph can initialize bgfx even when Projects is opened first, and proving
fully reliable focus, tab-switch and shutdown behavior on Linux and WASM.
The transient draw list does **not** yet provide the persistent buffers,
spatial-index culling or multi-scale LOD required for very large revision
graphs; those are step-4 performance work.
