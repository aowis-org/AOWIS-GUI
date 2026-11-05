# AOWIS GUI documentation

These guides describe the **current GUI source as of 5 November 2026**. They
separate implemented functionality from end-to-end acceptance. The product
is not described in terms of historical development stages.

## Product and architecture

| Guide | Scope |
|---|---|
| [Projects workspace](projects-workspace.md) | Servers, project/branch/HEAD/revision navigation, Inspector, graph and deletion |
| [Personal working databases](working-repository-configuration.md) | Server designation, embedded/REST targets and checkout boundary |
| [DVMDB quickstart](dvmdb-quickstart.md) | Project creation, open/import/export, autosave and Commit |
| [Distribution Editor](distribution-editor.md) | Shared 2D/3D hydraulic editing, terrain, UI and limitations |
| [Hydraulic MODEL transactions](hydraulic-edit-transactions.md) | Canonical edits, root metadata and 18 collections |
| [Rendering architecture](rendering-architecture.md) | Process-wide bgfx device, view IDs, native targets, frame coordination, Projects graph, three-map diagnostics and tests |
| [Map workspace architecture](map-workspace-architecture.md) | One shared map across five modes, controllers and native surface ownership |
| [Projects graph layout](project-graph-layout.md) | Cards, ancestry, picking, GPU fallback and BVH culling |
| [Projects graph performance](project-graph-performance.md) | CPU benchmark method, current scaling results and limits |
| [Units](units.md) | User-visible measurement and formatting behavior |

## History and persistence

| Guide | Scope |
|---|---|
| [Using History](history.md) | Action timeline, Undo/Redo, archived Commit actions and new-edit forks |
| [History interaction contract](history-contract.md) | Cursor/navigation eligibility, paging and editor update rules |
| [History transport and restart recovery](history-recovery.md) | Sparse MODEL deltas, SHA-256, same-UUID retry, durable intents and failure handling |
| [Private Save, Commit and recovery](personal-working-copy-save.md) | Autosave queue, working-copy lease/generation, local Commit intent and recovery |
| [Network import/export boundaries](network-open-import-replacement.md) | Supported initialization and replacement limits |

**Save** updates a private working copy; **Commit** creates an immutable
revision; **Publish** to another server is a separate workflow that is not
implemented as a complete GUI experience. Undo/Redo selects authoritative
MODEL action boundaries, including eligible archived Commit actions. For a
normal navigation or a recoverable lost response it exchanges verified
**sparse changed-entity data**, not a complete model download. Restarting
with a pending durable History intent also uses sparse recovery.

Personal working DVMDB describes an **editing role**, not necessarily a local
machine. Standalone generally uses an embedded server, while a remote REST
server may be designated personal (notably for WebAssembly). Other servers
remain browsing/checkout sources. The present GUI chooses one personal
server at a time; future per-project personal-server routing is a design
extension, not current UI behavior.

## Verification and next work

- [Verification status](verification-status.md): evidence versus source
  implementation and exact targeted regression commands.
- [History acceptance](history-roadmap.md): only remaining integration,
  crash/power-loss and platform qualification; no new History subsystem planned.
- [GUI–DVMDB integration roadmap](dvmdb-integration-roadmap.md): next product
  capabilities, led by complete project checkout, Fetch and Publish.
- [Applying patch archives](apply-patches.md) and
  [deferred security concerns](deferred-security-considerations.md).

The detailed `unit-*` audits are supporting technical records, **not** current
implementation roadmaps. Pre-release legacy migrations are not a product
requirement. A personal-repository role is a workflow guard, **not user
authorization or transport security**.
