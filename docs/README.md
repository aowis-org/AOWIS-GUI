# AOWIS GUI documentation

This index describes the **implemented product** in the GUI source snapshot
of **2026-11-04**, and separates implementation from runtime acceptance.
It is not a record of past implementation patches. Remaining work is in
the two forward-looking roadmaps.

## Start here

| Guide | What it describes |
|---|---|
| [DVMDB quickstart](dvmdb-quickstart.md) | Configure a private database, create/open HEAD, edit, Save and Commit |
| [Projects workspace](projects-workspace.md) | Database/project/branch/HEAD/revision navigation, graph and Inspectors |
| [Distribution Editor](distribution-editor.md) | Shared 2D/3D hydraulic editor, placement, terrain and controls |
| [Using History](history.md) | Conceptual timeline, Undo/Redo, Commit markers, direct jumps and redo branching |
| [History interaction contract](history-contract.md) | Exact navigation eligibility, archive paging and editor synchronization |
| [Autosave and recovery](personal-working-copy-save.md) | MODEL event queue, writer lease, draft persistence, uncertain requests and Commit intent |
| [Network open/import/export](network-open-import-replacement.md) | Current network initialization/import/export boundaries |
| [Verification status](verification-status.md) | What the source implements, what has been observed passing and what remains unverified |

**Save** persists the private working copy; **Commit** creates a new immutable
revision and advances the personal branch; **Publish** to another database is a
different, unfinished workflow. **Undo/Redo** navigates network states in the
private working copy, including eligible original actions across Commit
checkpoints. It does **not** rewrite committed revisions or roll back HEAD.

The current GUI wires adjacent Undo/Redo and direct jumps to DVMDB's verified
History HTTP operations, expands archived actions in pages of 50 and reloads
the canonical model after success. First/unborn-draft and multi-network History
navigation are deferred; DVMDB storage remains capable of multiple networks.
End-to-end acceptance is **not** implied by this implementation statement.

## Reference documentation

- [Hydraulic MODEL transactions](hydraulic-edit-transactions.md): all 18
  collections, root metadata, canonical before/after actions and membership.
- [Personal database configuration](working-repository-configuration.md),
  [UUIDv7 and revision identities](dvmdb-uuidv7-gui-readiness.md), and
  [Projects graph layout](project-graph-layout.md).
- [Map workspace architecture](map-workspace-architecture.md): bgfx 2D/3D
  rendering and editor ownership.
- [Numeric editor behavior](numeric-editor-transactions.md),
  [numeric formatting](numeric-formatting.md), and
  [unit-system user guide](units.md).
- [Applying patch ZIPs](apply-patches.md) and
  [deferred security considerations](deferred-security-considerations.md).

Detailed unit-system audits under `docs/unit-*` are supporting technical
references from earlier source reviews, **not** active delivery roadmaps or
proof of acceptance of the current snapshot.

## Remaining work

- [History roadmap](history-roadmap.md): GUI/DVMDB acceptance, complete
  first-parent timeline paging, initial unborn-draft actions, keyboard
  shortcuts, and Standalone/WASM qualification.
- [GUI ↔ DVMDB integration roadmap](dvmdb-integration-roadmap.md): full hydraulic
  Save acceptance, local workflow/recovery, import/replacement,
  Clone/Fetch/Publish/Merge, and release qualification.

Before the first release, transient development repositories are not a reason
to introduce legacy-data migration paths. Security engineering is a separate
deferred workstream; the personal repository role is **not authorization**.
