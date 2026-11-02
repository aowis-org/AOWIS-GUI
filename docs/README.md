# GUI documentation

AOWIS-SERVER-GUI is the Qt-based hydraulic editor, map/monitor workspace and
DVMDB Projects client. This index separates **current source behavior** from
**unfinished acceptance or planned work**; a code path's presence does not
by itself demonstrate successful end-to-end execution on Linux or WASM.

## Projects and DVMDB

| Start with | Purpose |
|---|---|
| [DVMDB quickstart](dvmdb-quickstart.md) | Configure the personal database; create project/HEAD, open, import, export and Commit |
| [Projects workspace](projects-workspace.md) | Current database/project/HEAD/revision inspector tabs and buttons |
| [Project graph layout](project-graph-layout.md) | Separate HEAD cards, immutable shared revisions and ancestry edges |
| [Personal working repository](working-repository-configuration.md) | ♛ private source, embedded Standalone and repository identity |
| [Private autosave and recovery](personal-working-copy-save.md) | Durable event queue, session fencing and generation checks |
| [Network import and replacement](network-open-import-replacement.md) | Implemented safe unborn first import vs unsupported replacement/merge |
| [Hydraulic edit transactions](hydraulic-edit-transactions.md) | 18 persisted entity collections and gaps in mutation/root metadata capture |
| [Integration roadmap](dvmdb-integration-roadmap.md) | Remaining GUI/DVMDB integration and acceptance gates |

**Status snapshot (source review 2026-11-02):** HEAD/Revision Inspectors,
network summary, selected-network opening/export, safe first import to an
untouched unborn draft, persistent branch metadata and Local Commit are
implemented in source. Complete root-metadata autosave, import into an
existing working network, Fetch/Publish, and full Standalone/WASM acceptance
remain open. In the current GUI the duplicate Tags/Description editors on
HEAD/Revision details and Branch share a branch-metadata save path, **not**
independent persisted version annotations.

## Other systems

- [Map workspace architecture](map-workspace-architecture.md)
- [Numeric editor transactions](numeric-editor-transactions.md),
  [numeric formatting](numeric-formatting.md) and
  [unit presentation coverage](unit-gui-coverage-matrix.md)
- [Patch workflow](apply-patches.md)

Security is deferred to [separate considerations](deferred-security-considerations.md).
Before the first release, do not add obsolete database-format migration or
compatibility scaffolding merely to preserve development data.
