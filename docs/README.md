# AOWIS GUI documentation

These guides describe the **current GUI source** (7 November 2026). They keep
implemented behaviour apart from end-to-end acceptance, and describe the
product as it is, not its development history.

## Projects, DVMDB and editing

| Guide | Scope |
|---|---|
| [Projects workspace](projects-workspace.md) | Servers, project/branch/HEAD/revision navigation, Inspector, graph and deletion |
| [Projects identities](dvmdb-uuidv7-gui-readiness.md) | UUID conventions and revision semantics used by the navigator and API clients |
| [Personal working databases](working-repository-configuration.md) | Server designation, embedded/REST targets and checkout boundary |
| [DVMDB quickstart](dvmdb-quickstart.md) | Project creation, open/import/export, autosave and Commit |
| [Distribution Editor](distribution-editor.md) | Shared 2D/3D hydraulic editing, terrain, UI and limitations |
| [Hydraulic MODEL transactions](hydraulic-edit-transactions.md) | Canonical edits, root metadata and 18 collections |
| [Network import/export boundaries](network-open-import-replacement.md) | Supported initialization and replacement limits |
| [Projects and editing acceptance](vc00-baseline.md) | Product acceptance scenarios for projects and private editing |

## History and persistence

| Guide | Scope |
|---|---|
| [Using History](history.md) | Action timeline, Undo/Redo, archived Commit actions and new-edit forks |
| [History interaction contract](history-contract.md) | Cursor/navigation eligibility, paging and editor update rules |
| [History transport and restart recovery](history-recovery.md) | Sparse MODEL deltas, SHA-256, same-UUID retry, durable intents and failure handling |
| [Private Save, Commit and recovery](personal-working-copy-save.md) | Autosave queue, working-copy lease/generation, local Commit intent and recovery |

**Save** updates a private working copy; **Commit** creates an immutable
revision; **Publish** to another server is a separate workflow that is not
yet a complete GUI experience. Undo/Redo selects authoritative MODEL action
boundaries, including eligible archived Commit actions. Navigation and
recovery exchange verified **sparse changed-entity data**, not a complete
model download.

Personal working DVMDB describes an **editing role**, not necessarily a local
machine. Standalone generally uses an embedded server; a remote REST server
may be designated personal (notably for WebAssembly). Other servers remain
browsing/checkout sources. The GUI chooses one personal server at a time.

## Map and rendering

The map engine is **AOWIS-GUI-Render** (`external/AOWIS-GUI-Render`), shared
with AOWIS-GUI-Mobile. Its own `README.md` and `docs/` describe the engine;
these guides describe how the GUI hosts it.

| Guide | Scope |
|---|---|
| [Render integration](render-integration.md) | What the GUI adds to each Render library, build, the Render-then-GUI change workflow |
| [Rendering architecture](rendering-architecture.md) | Maps, Projects graph and diagnostic windows on one bgfx device: view IDs, frames, ownership, depth, input loss |
| [Maps workspace](map-workspace-architecture.md) | One shared map across five modes, controllers, diagnostics counters and acceptance |
| [Map host guide](../src/map/bgfx/README.md) | `MapBgfxSurface`, input, editor overlay, 3D Editor icons and their measurement |
| [Map HUD localization](renderer-localization.md) | Translation contexts, GUI/Render split, live HUD text refresh |
| [Projects graph layout](project-graph-layout.md) | Cards, ancestry, picking, GPU fallback and BVH culling |
| [Projects graph performance](project-graph-performance.md) | CPU benchmark method, scaling results and limits |

## Units and numbers

| Guide | Scope |
|---|---|
| [Units](units.md) | User-visible measurement and formatting behaviour |
| [Numeric formatting](numeric-formatting.md) | Display precision policies and parsing |
| [Numeric editor transactions](numeric-editor-transactions.md) | How unit profile changes interact with edits in progress |
| [Table unit metadata](unit-table-metadata.md) | Column quantity, unit and filter declarations |

The unit-system audits and acceptance reports are kept as records in
[`records/units/`](records/units/); they are evidence, not roadmaps.

## Verification and next work

- [Verification status](verification-status.md): evidence versus source,
  the renderer regression matrix and focused test commands.
- [History acceptance](history-roadmap.md): remaining integration,
  crash/power-loss and platform qualification.
- [GUI–DVMDB integration roadmap](dvmdb-integration-roadmap.md): next product
  capabilities, led by complete project checkout, Fetch and Publish.
- [Applying patch archives](apply-patches.md) and
  [deferred security concerns](deferred-security-considerations.md).

Pre-release legacy migrations are not a product requirement. A
personal-repository role is a workflow guard, **not user authorization or
transport security**.
