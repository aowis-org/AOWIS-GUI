# GUI documentation

AOWIS-SERVER-GUI is the Qt controller, hydraulic editor and Projects
workspace for AOWIS. The following documents distinguish **implemented
source**, **reported test evidence**, **unfinished milestones** and
**unresolved design choices**.

## Start here — Projects and DVMDB

| What you need | Document |
|---|---|
| Create a private project and edit/save a network | [DVMDB quickstart](dvmdb-quickstart.md) |
| Find the server/project/branch controls and understand Commit vs Save | [Projects workspace](projects-workspace.md) |
| Understand the selected ♛ personal database and Standalone transport | [Personal working repository](working-repository-configuration.md) |
| Follow current implementation status and HC/VC delivery order | [Integration roadmap](dvmdb-integration-roadmap.md) |
| Understand all 18 entity collections and remaining capture gaps | [Hydraulic edit transactions](hydraulic-edit-transactions.md) |
| Understand durable queues, fencing and lost-acknowledgement recovery | [Save/recovery contract](personal-working-copy-save.md) |
| Decide how network load, import, replacement and merge should interact | [Network import decision record — **OPEN**](network-open-import-replacement.md) |
| Review immutable history/layout and UUID rules | [Project graph layout](project-graph-layout.md), [UUIDv7 contract](dvmdb-uuidv7-gui-readiness.md) |

**Current boundary (2026-11-02):** External HTTP DVMDB has successfully
saved and reopened a tested GUI-edited hydraulic network. The GUI now
supports canonical entity capture for all 18 collections, durable queued
Save, library CRUD/UI, rule/label CRUD/UI and local Commit from the HEAD
Inspector. **Full `NetworkHydraulic` autosave is not yet complete:** root
metadata transactions and the final import/replace/merge design remain
open, and Standalone end-to-end acceptance has not been reported. Import is
refused while a private draft is being edited (interim rule, 2026-11-02).
The 2026-11-02 editing fixes (stale drafts, pipe split order, imported
UUIDv5 entities, per-operation bounds, Commit lease) are delivered but not
yet built or tested. Publish is a separate future milestone.

## Other GUI systems

- [Map workspace architecture](map-workspace-architecture.md) — map and
  render ownership.
- [Numeric editor transactions](numeric-editor-transactions.md),
  [numeric formatting](numeric-formatting.md), and
  [unit presentation coverage](unit-gui-coverage-matrix.md) — numeric UI.
- [Apply patches](apply-patches.md) — source/patch ZIP tooling.

Security is deferred to [separate considerations](deferred-security-considerations.md).
AOWIS is pre-first-release: no legacy database format migrations or
compatibility scaffolding are prerequisites for these changes.
