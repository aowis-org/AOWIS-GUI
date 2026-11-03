# GUI ↔ DVMDB — remaining integration roadmap

This document contains **only unfinished implementation and acceptance work**.
For features present in the current product, consult
[Projects](projects-workspace.md), [autosave](personal-working-copy-save.md),
[MODEL transactions](hydraulic-edit-transactions.md),
[network import](network-open-import-replacement.md), and the
[DVMDB product contract](../../AOWIS-SERVER-DVMDB/docs/version-control-semantics.md).
The independent [History roadmap](history-roadmap.md) covers the future
Undo/Redo subsystem, including navigation across Commit markers.

A path existing in source, a simulated HTTP fixture passing, and a real
application/daemon restart passing are **different levels of evidence**.
Record the exact platform, commands, results and failures before declaring
an acceptance gate complete. Latest snapshot reviewed: 2026-11-03.

## Work order

| Order | Remaining work | Required result |
|---|---|---|
| **1** | Finish hydraulic Save acceptance | All 18 collections and persistent root fields round-trip through real HTTP and Standalone, including server + GUI restart, interrupted Save and exact canonical equality |
| **2** | Agree on History architecture; implement initial History dock | One left dock opened by the existing History action; show actual data and make unsupported navigation visibly disabled |
| **3** | Build persistent History/Undo/Redo | Navigate edits and Commit checkpoints without modifying immutable revisions or silently losing work; see the dedicated roadmap |
| **4** | Complete network import/replacement | Explicit new/replace/selected-entity operations, atomicity, target identity, queued-edit protection and recovery |
| **5** | Qualify local editing lifecycle | Unborn HEAD → checkout → edit → Save → Commit → branch → reopen, including wrong-target/recovery and multiple sessions |
| **6** | Whole-project Clone and Fetch | Pinned repository identity, all branches/history/unborn HEADs, remote-tracking state; no local draft or HEAD changes |
| **7** | Publish and reconcile | Server-side expected remote HEAD/CAS, idempotent Publish status, explicit fast-forward/three-way Merge and conflict choices |
| **8** | Release qualification | Durability/soak, large models, journals, Linux desktop, embedded Standalone, supported browser/WASM, and deployment-specific limitations |

### 1 — Hydraulic persistence and capture acceptance

**Already in product (not new work):** MODEL before/after events, 18 typed
collections, network-root edit images, GUI local queue, lease/fencing and
generation checks, backend-owned membership/manifest chunks, mixed root/entity
Save support, native Commit and draft reload paths.

**Still to prove or finish:**

- Run the latest mixed root/entity GUI and DVMDB HTTP regressions and fix any
  failures. The current snapshots contain tests, but their presence does not
  mean a passing result was supplied for the latest changes.
- Exercise **actual** complete hydraulic editor sessions against a separate
  DVMDB process and the persistent embedded Standalone server. Assert full
  `ModelCodec<NetworkHydraulic>` equality across both GUI and DVMDB restart,
  including libraries, controls, labels, MSX and network-root settings.
- Inventory **actual UI mutation paths**, not just the generic 18-type
  serializer; cover moved nodes, cascade edits, bulk edits, libraries,
  numerical settings and placement in both 2D and 3D.
- Exercise dropped replies, interrupted queue flush, lease expiry, stale
  generation, target replacement and disk failure without false Autosaved
  status or event loss. Confirm mixed root/member edits commit atomically.
- Extend bounded batch handling: the current per-Save changed-entity limit
  can reject a large meaningful edit. Do not silently split one logical
  operation into partially committed states.

**Not part of this gate:** implementing Undo/Redo. MODEL events are reversible
in principle, but no operational history controller or cross-Commit cursor
exists. Those belong to the [History roadmap](history-roadmap.md).

### 2–3 — Action history and History dock

The proposed design is a **left History dock** opened by the existing toolbar
"Open revision history in Projects" action. It shows one continuous timeline
for the selected branch, meaningful editing actions, a current-state cursor,
Undo/Redo controls and visually distinctive immutable Commit markers.
The user must be able to move backward and forward across those markers
**without moving or rewriting a committed HEAD or revision**. Historical
state navigation changes only the private working candidate and is itself
durable. Details, staged UI delivery, and the unresolved durability/merge
questions are in [history-roadmap.md](history-roadmap.md). This subsystem
must be implemented before calling the editor's Undo/Redo support complete.

### 4 — Network creation, import and replacement

The **first import into an untouched, empty unborn private HEAD** already
exists. Outstanding operations are explicit "Create network", "Replace
working network", and selective entity/library import. Each must name its
repository, project, branch, draft, generation, lease and network. Decide
what happens to the current local event queue, whether a history checkpoint
is made, and how UUID collisions and dependent objects are reconciled.
Partial failure, timeout and user cancellation must leave the original
network and pending events recoverable. Never use in-memory
`replaceNetworkHydraulic()` as proof that DVMDB replaced a saved network.

### 5 — Local private editing acceptance

Verify inspector and toolbar handling of unborn branches, saved/uncommitted
HEADs, read-only historical revisions, local immutable Commit, branch
creation, repository-pinned metadata and durable Commit-intent recovery.
Cross-check network summaries against the **base revision plus draft overlay**.
Persisted operation UUIDs must be reconciled after a lost response; a new ID
must not be generated for an unresolved previous operation. Test more than
100 drafts per repository, branch switching and concurrent writer fencing.

### 6–7 — Distributed version control

The backend has verified bundles, provisioning, revision reads and merge
primitives. Complete **whole-project Clone**, **Fetch**, remote tracking,
**Publish** and the GUI conflict-resolution workflow are not implemented
end-to-end. Transfer immutable objects without moving local refs; Publish
advances the remote branch only with its expected HEAD and durable operation
identity. Do not infer correspondence from a mutable branch name. For merge,
use an explicit pinned common base, preserve the user's dirty draft, and
record two-parent revision ancestry without silent force updates.
Hydraulic solvability is not a Commit/Publish/Merge prerequisite; existing
canonical record, object-reference and repository-integrity rules remain.

### 8 — Capacity and acceptance

Bound all lists/graph loads without silently omitting results; expose quota
and journal-capacity conditions clearly. DVMDB currently limits draft
journals to 4,096 events / 8 MiB; continuous history requires its **own**
retention design instead of treating that journal as permanent Undo/Redo.
Re-run native integrity, ENOSPC/crash/power-cut, HTTP soak and GUI workflows
against exact same MODEL source fingerprint. Report actual pass/fail/skipped
results per platform, not inferred release readiness.

## Out of scope

No pre-first-release legacy migrations, speculative vertical-datum system,
or authorization/TLS implementation in the editor-integration roadmap.
Security is a separate deployment concern; do not expose unauthenticated
writable DVMDB services to untrusted networks.
