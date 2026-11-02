# Projects workspace — visible controls and version-control contract

The **Projects** tab brings together database servers, projects, branches,
revision history and a context-sensitive Inspector. It is the entry point
for editing hydraulic networks stored in DVMDB. The product's Git-like
object model is specified here; where a control is not yet implemented it is
explicitly identified as a future action.

## 1. Screen layout

| Area | What it shows/does |
|---|---|
| Left — Servers | Configured local/remote DVMDB databases; a visibility checkbox filters that source from the displayed workspace; the **♛** marks the personal working database |
| Left — Projects & Branches | Actual projects and their named branches; expand a project to select a **branch** and reveal branch-only Inspector actions |
| Left — Revisions | Committed revisions for the current source/context; an unborn branch has no revision |
| Center — graph | Server/project/branch presentation and committed revision ancestry; each server has a left-hand server icon; the personal server's card is **gold with a crown** beside its name |
| Right — Inspector | Selection-specific identity, project/branch/revision information, creation/edit actions and connection configuration |

The Inspector is shown for relevant selections/actions. Selecting a server,
project, branch or revision changes its content. Branch actions should not
be expected when the **project row** is selected. Hiding a database from
the graph is not disconnecting or deleting it.

All project/branch/revision identities are UUIDv7, not labels. A repository
ID qualifies a branch reference: two repositories may have the same
logical project and branch UUIDs but different committed HEADs.

## 2. Configure and identify databases

The **Server Inspector** includes **Use as my personal working database**.
Exactly one source can be designated at a time. For a reachable source the
GUI verifies repository identity and role; an unconfigured repository can
be explicitly configured as private, but a shared repository cannot be
silently repurposed. This selected source owns private drafts and future
local commits, regardless of which shared source is being browsed.

Standalone has a native DVMDB service embedded in its own process, accessed
through temporary loopback HTTP routes. Local profiles select persistent
absolute storage directories; the runtime endpoint is not persisted.
Desktop/basic GUI connects to configured HTTP repositories. Selecting a
server is **not** the same as selecting a working copy.

## 3. Projects, branches and revisions

A newly created project has a UUIDv7 and an unborn `main` branch. Unborn
means `headRevisionId` is null; **no R0 or empty synthetic Commit** is
created. Branch creation from an immutable committed revision records
origin branch and exact fork revision without adding an extra revision.
Branch UUIDs and names are distinct; names may include UTF-8/spaces subject
to the backend's validation.

A **revision** is an immutable whole-model snapshot with genuine parent
revisions; SHA-256 is an integrity check, not the revision ID. One committed
revision may be referenced by several branches. The graph must not invent
branch-creation ancestry or present a writable draft as an immutable
revision. See [graph layout](project-graph-layout.md).

## 4. Current editing workflow

1. Mark one database personal in its **Server Inspector** (♛).
2. Create/select a project **on the personal database**. If it has one
   branch, **Project Inspector → Edit network** can open that branch. With
   multiple branches, select the intended branch before editing.
3. The GUI opens/creates a private working copy and its network, verifies
   the candidate, replays durable local pending edits, obtains a fenced
   writer session and opens **Distribution Editor**. If the branch's draft
   is older than the branch HEAD, an unmodified draft is replaced by one at
   the current HEAD; a draft with edits opens only after confirmation
   (**Edit draft anyway**).
4. `HydraulicData` captures supported logical entity changes; the local
   queue and DVMDB Save persist them without moving the committed HEAD.
5. Stopping and reopening the session is distinct from Commit. An actual
   external HTTP GUI-restart recovery was reported for tested edits; full
   coverage remains incomplete.
6. **HEAD Inspector → Commit** with a commit message creates the immutable
   revision from the saved draft. Stop editing first; Commit is refused
   while edits are still queued on this device.

For branch-level checkout/working-copy administration, select an actual
**database-backed branch** in Projects & Branches and use the **Personal
working copies…** Inspector button. The advanced dialog offers one-branch
**Checkout branch privately**, draft operations, read-only **View in
Distribution Monitor** and writable **Edit in Distribution Editor**.
This is **not** an existing one-click whole-project clone UI.

The narrow left application navigation bar also has **Hydraulic Libraries**
and **Controls & Map Labels** pages. They edit the current shared in-memory
`HydraulicData` using transaction-safe APIs. Only a properly bound private
draft session makes supported edits eligible for durable Save.

## 5. Save, Commit, Publish — different operations

| Action | Effect | GUI availability |
|---|---|---|
| **Save draft** | Persist changes in the private working copy; no revision or committed HEAD movement | Available for captured entity edits in a fenced session; incomplete metadata coverage |
| **Commit locally** | Create immutable revision UUIDv7 from validated private state, CAS-update **personal** branch HEAD | **HEAD Inspector → Commit**; operation UUID reconciled after a lost reply, not yet persisted across restarts |
| **Fetch/Clone** | Transfer/present verified remote history and tracking | Advanced **branch-level** checkout exists; whole-project Clone absent |
| **Publish** | Transfer local commits and CAS-update **shared** branch HEAD | **Not yet complete in GUI** |
| **Reconcile/merge** | Explicitly compare divergent histories and resolve model conflicts | Planned; no silent force-push/rebase |

A timeout after Commit/Publish must be reconciled by the **same persisted
operation UUID and digest**, not by issuing a second unrelated action. A
local branch HEAD and a shared branch HEAD are independent even when their
branch UUID is identical.

## 6. Open network versus import

Loading an authoritative DVMDB network into `HydraulicData` is a read,
not a user edit. Importing EPANET/EPANET-JS data as new project, replacing
an existing working network and merging entities are **distinct potential
operations**. The GUI currently has whole-model replacement code that
resets capture; it is not an accepted persistent replacement operation.

**Interim rule (2026-11-02):** while a private draft session is open,
EPANET/EPANET-JS import and the built-in examples are refused; stop private
editing first. Import into a selected empty private HEAD is unaffected.
The final replace/merge behavior is still undecided: target identity,
transaction atomicity, pending queue handling and conflict/recovery
semantics are tracked in
[network import decisions](network-open-import-replacement.md).

## 7. Acceptance and known limits

- Personal source identity/role and exact target must be checked; no
  fallback to the selected shared source if personal DVMDB is unreachable.
- Captured entity transaction and membership updates must be validated as
  one durable unit, with lease fencing and expected generation.
- Failed/uncertain Save retains pending queue records; wrong-target and
  stale-session errors block instead of discarding local work.
- `NetworkHydraulic` **network-root metadata** still needs HC-03/04/05.
  Generic 18/18 entity capture is not complete GUI coverage.
- Whole-model import/replace/merge needs the HC-02.9/06 design decision;
  import is refused during a private session until then.
- One editor operation may change at most 128 entities; larger ones are
  refused and the session blocks. A draft holds at most 4096 events and
  20 000 changed records (8 MiB each) until Commit, without a warning yet.
- Real end-to-end equality after DVMDB and GUI restart must be proven for
  **both** external HTTP and embedded Standalone (HC-07).
- Full-project Clone, Publish and reconciliation remain separate future
  work; local Commit exists but has not passed acceptance.

See [quickstart](dvmdb-quickstart.md),
[working repository](working-repository-configuration.md),
[hydraulic transactions](hydraulic-edit-transactions.md), and
[roadmap](dvmdb-integration-roadmap.md).
