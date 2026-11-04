# GUI ↔ DVMDB — remaining integration work

**Scope:** only work still unimplemented or missing reproducible acceptance
in the **2026-11-04 GUI snapshot**. Current behavior is documented in
[Projects](projects-workspace.md), [private Save](personal-working-copy-save.md),
[MODEL transactions](hydraulic-edit-transactions.md),
[network import/export](network-open-import-replacement.md), and
[History](history-contract.md). See [verification status](verification-status.md)
for the distinction between source presence and observed test results.

## Priorities and completion criteria

| Priority | Outstanding deliverable | Acceptance gate |
|---|---|---|
| **1** | Real hydraulic editing and durable Save | All 18 collections and root fields round-trip through GUI → real DVMDB HTTP → GUI/DVMDB restart; exact canonical equality, correct 2D/3D mutation capture and safe pending-queue recovery |
| **2** | History end-to-end qualification and remaining usability | Real Undo/Redo → cross-Commit jump → return to live draft → new edit/Redo invalidation → restart, with unchanged immutable revisions; full scope in the [History roadmap](history-roadmap.md) |
| **3** | Local checkout/Commit lifecycle and recovery | Unborn HEAD, saved draft, Commit, branch, reopened edit, lease conflicts and lost acknowledgements all resolve deterministically without wrong-target writes |
| **4** | Network import and replacement | Explicit new/replace/selective import semantics with identity, queue, history and UUID/reference safeguards; no accidental overwrite of edited networks |
| **5** | Distributed operations | Whole-project Clone/Fetch and remote tracking; explicit Publish with expected remote HEAD, idempotent recovery, merge/reconciliation and conflict UI |
| **6** | Release/platform qualification | Native Linux, embedded Standalone and supported WASM/browser behavior with crash/powercut/ENOSPC, large models and operational limits documented |

## Hydraulic editing and saving

Already implemented: canonical MODEL before/after transactions, membership
intents for all 18 hydraulic collections, persistent network-root fields,
mixed root/entity Save, private draft queue, lease/fencing and generation
checks, server-owned manifest chunks, saved-draft reload and local Commit.

Still required: run full GUI sessions against a **separate** DVMDB process and
embedded Standalone, restart both client and database, compare exact
`ModelCodec<NetworkHydraulic>` images, and exercise controls, libraries,
quality/MSX, map labels, terrain-relative placement, movements, cascaded
deletes and bulk edits. Confirm error handling for failed network loads,
misdirected repository profiles, stale generations, interrupted Saves,
rejected oversized transactions and lost HTTP replies. The implementation
currently bounds individual Save batches; a logical edit must never be
silently split into partially accepted states.

Hydraulic solvability is **not** a Save/Commit prerequisite. Canonical codecs,
entity UUID/reference identity, valid typed values and native-record integrity
still apply.

## History integration

The History dock and DVMDB's durable action architecture **already exist**:
indexed local-Commit action archives, verified historical reconstruction,
atomic native cursors, fenced HTTP navigation, active-draft Redo invalidation,
Commit-boundary handoff, and GUI adjacent/direct navigation. Do **not** put
those completed implementation stages back on the future roadmap.

Outstanding: prove the GUI + daemon behavior with real editor events and
restarts, independently page the full branch ancestry, qualify uncertain
navigation results, support unborn first drafts and add shortcuts. See
[History roadmap](history-roadmap.md) for the exact remaining tests.

## Local repository lifecycle

Exercise project creation, the independent unborn `main` HEAD, additional
unborn branches, branch-from-revision, remote-to-personal checkout, saved
network opening, Commit intent recovery, and branch/HEAD inspector summaries.
Reconcile unresolved operations by **the original operation UUID**, not a new
request. Test switching projects while queue events exist, writer contention,
large project/draft lists and a failed repository health/identity check.

The backend and GUI must use the same MODEL fingerprint. A mismatched
fingerprint must not be papered over by deleting the private edit queue.

## Network operations

First import into an **untouched, empty unborn private draft** is present.
General replace-existing-network and selected entity/library imports remain
unfinished. Define explicit target network identity, repository/project/
branch/draft identity, writer lease and expected generation, local pending
queue handling, UUID collisions, dependent references, undo checkpoint and
idempotent recovery. `HydraulicData::replaceNetworkHydraulic()` is an
in-memory operation, **not** evidence of a persisted replacement.

## Clone, Fetch, Publish and Merge

The existing branch-level checkout, immutable revision transport and DVMDB
primitives do not amount to complete end-user distributed version control.
Whole-project Clone/Fetch must preserve all branches and ancestry and track
remote state without replacing a personal working draft. Publish must use
expected remote HEAD and operation identity, not just branch-name matching.
Divergent ancestry needs an explicit merge/conflict workflow, including a
pinned common base and preservation of uncommitted work.

## Release sign-off

Capture the exact repository snapshots, MODEL fingerprint, platform,
commands, passing/failing/skipped results and reproduction steps for each
gate. A native unit test, a Qt widget regression and a real external-server
restart are distinct evidence. Do not claim completion from source presence.
See [verification status](verification-status.md).

**Deliberately outside this roadmap:** speculative vertical-datum metadata,
pre-release legacy migration mechanisms, and authentication/TLS work.
Security remains a separate deployment concern: do not expose an unauthenticated
writable DVMDB on an untrusted network.
