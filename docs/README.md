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

**Current boundary (2026-11-01):** External HTTP DVMDB has successfully
saved and reopened a tested GUI-edited hydraulic network. The GUI now
supports canonical entity capture for all 18 collections, durable queued
Save, library CRUD/UI and rule/label CRUD/UI. **Full `NetworkHydraulic`
autosave is not yet complete:** root metadata transactions and final
import/mutation-path design remain open, and Standalone end-to-end
acceptance has not been reported. Local Commit and Publish are separate
future milestones. The HC-02.9 blanket import-block patch was **not
applied** and is not an approved policy.

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
