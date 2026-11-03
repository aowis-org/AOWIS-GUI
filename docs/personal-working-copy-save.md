# Private draft autosave, Commit and recovery

This page describes the current GUI client, durable queue, and DVMDB
working-copy contract. Saving is **not** committing. Committing locally is
**not** publishing. A MODEL event journal is **not** a user-facing Undo/Redo
system; that future subsystem is in [History](history-roadmap.md).

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
in a bounded draft journal, then clears/rebases that journal after a
successful Commit. See [working-copy details in DVMDB](../../AOWIS-SERVER-DVMDB/docs/working-copies.md).

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

## Current limits and required acceptance

A DVMDB draft currently has a 4,096-event/8-MiB journal limit, with per-Save
record/event limits and a bounded candidate size. The GUI must not silently
continue editing after the server rejects an oversized Save. The remaining
[roadmap](dvmdb-integration-roadmap.md) includes actual external HTTP and
embedded Standalone restart/ENOSPC/lost-acknowledgement tests, the complete
mutation-path audit and safe bulk-event handling. History/Undo/Redo requires
**additional persisted action history**, not merely raising this journal
limit; see [History](history-roadmap.md).

The GUI and server must be built against the same tracked MODEL sources;
the advertised content fingerprint must match. A fingerprint mismatch is a
reason to rebuild the components, **not** to delete pending edit queues.
