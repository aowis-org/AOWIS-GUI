# Private draft autosave, Commit and recovery

This page describes the current GUI client, durable queue, and DVMDB
working-copy contract. Saving is **not** committing. Committing locally is
**not** publishing. A MODEL event journal is part of the durable History
architecture, but is not the *entire* historical timeline. The user-facing
History dock navigates the current draft and eligible permanently archived
Commit actions through DVMDB; see [History](history-contract.md).

## Repository, draft and lease identity

The user designates one verified ♛ personal DVMDB in the Projects Database
Inspector. A private working copy is scoped to repository, project, branch,
working-copy UUIDv7 and a pinned committed base (or unborn null). Every
writer session has a session UUID, lease/fencing token and expected saved
generation. Repository UUID/role/MODEL fingerprint checks prevent accessing
an incorrectly configured target. A shared browsing source is never a
fallback writable draft.

Desktop uses HTTP DVMDB. Standalone embeds native DVMDB with the **same HTTP
routes** on a runtime-selected localhost port and a persistent configured
data root. Browser access to a remote HTTP service is supported where the
client can reach it; browser memory is not durable DVMDB storage.

## Load and autosave

- Select a project/branch and create or reopen the private draft. An unborn
  branch may have **no commit yet** but already contain a saved network.
- Load the exact saved candidate (committed base plus draft overlay), then
  reconcile the local durable edit queue; install the verified model in
  `HydraulicData` and enable capture only after the writer session is ready.
- Record a MODEL before/after transaction, persist it to the local
  `PersonalEditQueue`, then send the oldest unacknowledged event to
  `POST /working-copies/{draft}/save-events` with generation and fencing.
- The request carries typed changes, matching create/delete membership
  intents, and the MODEL event bytes. Root metadata can be part of the same
  event as typed entity changes. DVMDB constructs the native root/chunks.
- Retire a queue item only after DVMDB acknowledges the exact event and
  updated generation, or a status/recovery read proves it was already saved.
  Network canonical equality is checked against the staged candidate.
- Lease expiry, inconsistent journal, wrong repository, malformed event,
  wrong MODEL fingerprint or disk failure blocks new saves; pending data
  remains available for recovery. A blocked session must not show Autosaved.

The GUI's pending queue is local transport state, **not the full edit
history**: acknowledged entries are removed. DVMDB stores accepted events
in a bounded draft journal and archives their original MODEL frames against
an immutable revision on local Commit. The active journal is cleared/rebased
after Commit, but the indexed permanent Commit action archive is retained.
See [History](history-contract.md) and
[working-copy details in DVMDB](../../AOWIS-SERVER-DVMDB/docs/working-copies.md).

## Complete discovery and uncertain operations

`PersonalWorkingCopyClient::list()` follows DVMDB's `nextCursor` pages and
uses server-side `projectId` filtering where the GUI knows the selected
project. Malformed/out-of-order cursor results fail the list instead of
pretending a first-page subset is complete. Catalogue reads are bounded.

Project/branch **metadata writes** are preflighted against the selected
repository UUIDv7, expected role and MODEL fingerprint. They also carry
an expected-repository header checked by DVMDB during the mutation. This
is a correctness guard, not authentication. A lost metadata acknowledgement
still needs an authoritative refresh.

## Local Commit and intent recovery

The HEAD Inspector accepts a nonblank message and submits a **saved,
modified** private draft. DVMDB creates an immutable revision with the
correct expected local HEAD, atomically advances **only the local branch**,
rebases the draft onto that new revision and clears its active event journal.
The original revision record and model remain immutable.

Before the POST, the GUI writes a branch-scoped `PersonalCommitIntentStore`
record containing the operation UUIDv7, exact request body/digest, repository,
project, branch, endpoint, expected base/generation and state. After a lost
reply or restart the GUI uses `GET /local-commits/{operationId}` and, where
the server definitively has no record, resends **the same exact ID/body**.
A second Commit on that branch is blocked while an unresolved intent exists.
The HEAD Inspector can show **Reconcile Commit** instead of creating a new
operation. Recorded terminal outcomes require acknowledgement before the
next new Commit. Local intent persistence is separate from the native
server's idempotent Commit ledger.

## History cursor, Redo and draft recovery

For a verified, unambiguous hydraulic network the GUI reads the authoritative
`GET /working-copies/{draftId}/history/cursor`. Fenced
`POST /working-copies/{draftId}/history/navigate` selects an adjacent Undo,
Redo or exact eligible action boundary, including actions archived across
Commits. DVMDB verifies and atomically publishes the private candidate and
cursor, returning canonical sparse changed-entity images, collection moves,
and source/destination SHA-256 digests. The GUI applies/validates these in a
temporary model before publishing to its editor. **Ordinary navigation does
not reopen the draft or GET a full network.**

Before the POST the GUI durably writes a bounded `PersonalHistoryNavigationIntent`
scoped to the repository/project/draft/network, original UUID, session, base,
source generation, source/target cursor, complete canonical source model and
active journal frames. Only one pending navigation is permitted for a draft.
The record exists to recover an **uncertain** HTTP operation, not as a second
permanent Undo history.

A lost response is retried with the **same operation UUID**. After immediate
retries, temporary connectivity failures pause editing. Reconnection uses a
small SHA-256/generation/cursor/last-operation receipt; if the operation
already completed and the verified network matches, no delta is needed. If
it completed but the GUI holds old bytes, it obtains the original sparse
delta. If it was never published and the source still matches, it retries
its original UUID. The writer lease is reacquired for the same session.
Definitive conflicts block editing.

After **GUI restart** the session reads the pending intent *before* ordinary
network hydration. It opens the local queue around the persisted source
model and enters that same verification/reconciliation flow. Valid restart
recovery requires no initial full-model GET. Corrupt, wrong-scope, oversized
or irreconcilable intents are preserved/blocked, not silently dropped.
A normal fresh open **without** such an intent still downloads the model
once. Native durability uses `QSaveFile` and filesystem synchronization;
WebAssembly restart durability requires persistent browser-backed storage.
See [History transport/recovery](history-recovery.md) for state transitions,
limits and tests.

Undo may retain unapplied journal frames for Redo. A new ordinary edit after
Undo atomically invalidates its forward suffix. A new MODEL Save from a
verified archived position starts a logical fork while preserving all
immutable revisions and their action archives. An ordinary native Save
cannot bypass the archived-history fork guard. Commit markers are not
Undo steps and History navigation does not rewrite the branch's committed
HEAD.

An unborn initial draft can navigate journaled creations and updates against
its initialized seed; initialization itself is not an Undo step. Unsupported
unborn deletions without proven original membership ordering fail closed.
History navigation remains one-network only; DVMDB native storage can hold
multiple networks independently.

## Current limits and required acceptance

A DVMDB draft currently has a 4,096-event/8-MiB journal limit, with per-Save
record/event limits and a bounded candidate size. Permanent indexed Commit
archives are **separate** from this draft-journal limit; old snapshots without
original actions remain checkpoint-only. The GUI must not silently continue
editing after the server rejects an oversized Save. The remaining
[roadmap](dvmdb-integration-roadmap.md) includes actual external HTTP and
embedded Standalone restart/ENOSPC/lost-acknowledgement tests, the complete
mutation-path audit and safe bulk-event handling. See the
[History roadmap](history-roadmap.md) for end-to-end navigation acceptance.

The GUI and server must be built against the same tracked MODEL sources;
the advertised content fingerprint must match. A fingerprint mismatch is a
reason to rebuild the components, **not** to delete pending edit queues.
