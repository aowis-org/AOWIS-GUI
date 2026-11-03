# AOWIS GUI documentation

This index describes the **current product**, as implemented in the source
snapshot of 2026-11-03. Future behavior appears in roadmaps, not as if it were
already available. An existing test or API is not by itself an accepted
end-to-end workflow.

## Hydraulic editor and map

- [Distribution Editor](distribution-editor.md) — 2D and 3D editing, nodes,
  links, demand points, terrain/elevation behavior and current controls.
- [Hydraulic editing and MODEL transactions](hydraulic-edit-transactions.md) —
  captured actions, all 18 entity collections, network-root metadata, and
  current model-reference rules.
- [Map workspace architecture](map-workspace-architecture.md) and
  [graph layout](project-graph-layout.md) — renderers, shared editing view,
  and separate Projects revision graph.

## Projects, repositories, and saving

| Guide | Current behavior |
|---|---|
| [DVMDB quickstart](dvmdb-quickstart.md) | Configure a private database, create a project, edit a HEAD, Commit |
| [Projects workspace](projects-workspace.md) | Tree, graph, HEAD/Revision/Branch Inspectors, toolbar navigation |
| [Personal working database](working-repository-configuration.md) | Repository role, ♛ profile selection, embedded Standalone |
| [Autosave and recovery](personal-working-copy-save.md) | Lease/fencing, MODEL events, durable local queue, Commit intent |
| [Network open/import/export](network-open-import-replacement.md) | Supported first import, selected-network loading and export |
| [Version-control identities](dvmdb-uuidv7-gui-readiness.md) | UUIDv7, branch HEAD vs GUI HEAD, immutable revisions |

**Save** updates the private draft; **Commit** creates an immutable revision and
advances only the local branch; **Publish** is a different, not-yet-complete
workflow. The current GUI has **no general Undo/Redo or navigable action-history
dock**. The History toolbar item currently navigates to the Projects revision
tree; it does not undo edits or open a history panel.

## Work that is not yet implemented or accepted

- [Remaining GUI ↔ DVMDB integration roadmap](dvmdb-integration-roadmap.md)
  — full external HTTP/Standalone acceptance, import/replacement, local
  workflow hardening, whole-project Clone/Fetch, Publish and Merge.
- [History and Undo/Redo roadmap](history-roadmap.md) — planned left History
  dock, durable action navigation, and Undo/Redo **across Commit markers**;
  none of this should be described as working today.
- [DVMDB backend roadmap](../../AOWIS-SERVER-DVMDB/docs/roadmap.md) — the
  server-side work and acceptance dependencies.

Other maintained guides cover [numeric editing](numeric-editor-transactions.md),
[formatting](numeric-formatting.md), [units](unit-gui-coverage-matrix.md),
[patch application](apply-patches.md), and
[deferred security](deferred-security-considerations.md).

Before the first release, no legacy-format migration or backwards-compatibility
work is required merely to preserve transient development data. Security
architecture is a separate deferred effort; a repository role is not an
access-control mechanism.
