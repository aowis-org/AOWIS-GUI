# History navigation: persistence, sparse transfer and recovery

This is the implemented **GUI-side** contract for a single editable hydraulic
network. DVMDB remains the authority for journal actions, archived MODEL
frames, the committed branch HEAD, working-copy candidate and History cursor.
For user-facing controls, see [History](history.md); for Save/Commit boundaries,
see [private working-copy persistence](personal-working-copy-save.md).

## What is recorded where

| Data | Owner | Purpose |
|---|---|---|
| Immutable revision and its canonical snapshot | DVMDB | Permanent committed model and revision ancestry |
| Local Commit MODEL action archive | DVMDB | Original actions across Commit markers; not an extra Undo stack |
| Active MODEL journal and selected History cursor | DVMDB working copy | Draft Undo/Redo and persisted selected position |
| Candidate overlay and navigation receipt | DVMDB working copy | Atomic destination, last operation UUID and pinned source for sparse replay |
| Pending unsaved MODEL events | `PersonalEditQueue` on GUI client | Crash-safe Save submission, not the authoritative Undo timeline |
| Pending History navigation intent | `PersonalHistoryNavigationIntentStore` on GUI client | Recovery of an uncertain Undo/Redo/jump after client termination |
| Cached current `NetworkHydraulic` | GUI session | Rendering and before-image verification |

The personal working repository can be an **embedded server** or a **remote
REST DVMDB**. “Personal” grants an editing role; it does not guarantee local
availability. The client must not redirect its write to a different repository
when the designated server is unreachable.

## Normal Undo, Redo and direct jump

1. The editor has a saved, verified, single-network draft, an up-to-date cursor,
   a current writer lease and **no pending Save events**.
2. `PersonalDraftEditSession` creates a new navigation operation UUIDv7 and
   writes a durable local intent **before** sending the request.
3. `POST /working-copies/{draftId}/history/navigate` carries the expected
   generation, base revision, writer fencing, network identity and exact
   destination coordinate. DVMDB reconstructs and verifies the destination,
   then atomically changes its private candidate and cursor.
4. The response contains a sparse `historyDelta`: canonical changed MODEL
   images, collection membership/order moves, and `fromSha256`/`toSha256`.
   The GUI verifies the current model hash and before-images, applies the
   changes to a temporary model, then checks the destination hash before
   publishing it to the editor.
5. The GUI acknowledges/removes the local intent and fetches a fresh
   authoritative History cursor. It does **not** reopen the draft, reload the
   original journal or GET a complete `NetworkHydraulic` for this operation.

A bulk edit or distant jump may legitimately produce a large sparse delta;
only changed data should travel. Replacing the GUI's current model after
validation is an in-memory update, **not** a network re-download. DVMDB may
still reconstruct full canonical states internally to verify navigation.

A Commit marker is a checkpoint, not an Undo step. Navigation across it does
not reset or mutate the immutable revision/committed HEAD. An edit from an
older verified position starts a new logical action path; the abandoned Redo
suffix is no longer active, but old committed revisions remain immutable.

## Recovery after a lost acknowledgement (GUI still open)

Two immediate transport retries use the **same** navigation UUID and expected
source generation. If the result remains uncertain, editing stays paused.
The session then retries reconciliation with backoff (1, 2, 4, then 8 seconds;
8-second cap). No new Undo operation is constructed.

On reconnect it reads the small, read-only
`GET /working-copies/{draftId}/history/reconcile?networkId={networkId}`
receipt, which identifies the repository's actual draft session, generation,
base, current model SHA-256, journal count, cursor and last navigation UUID.
**A matching SHA-256 is not enough:** distinct History coordinates can contain
identical model bytes.

- **Original request already published; correct UUID/generation/cursor and
  model digest matches:** renew the original writer session's lease and
  acknowledge without transferring a MODEL delta.
- **Already published, but GUI still holds the source model:** reacquire
  the lease and replay the *same* operation UUID to obtain the pinned sparse
  delta, then verify its destination digest against reconciliation.
- **Never published; source generation/cursor/digest still match:** send the
  original request under its unchanged UUID and original source generation.
- **Temporary unavailability:** retain the pending intent and continue
  non-editable recovery; do not substitute a different database.
- **Changed identity, cursor, generation, HEAD, journal or invalid delta:**
  fail closed rather than guess or silently fetch and overwrite the model.

DVMDB's durable previous-candidate retention allows the last navigation's
sparse result to be regenerated after lease rotation or server restart.

## Recovery after the GUI crashes or restarts

`PersonalHistoryNavigationIntentStore` writes
`<queue-root>/<repository-uuid>/<draft-uuid>.history-intent.json` using
`QSaveFile`, with an on-disk checksum and canonical MODEL encoding. On supported
Unix-native targets the implementation also syncs the file and containing
directory. The intent is scoped to the repository, project, draft, network,
writer session, base revision, original operation UUID and source generation.
It holds the **complete source network**, its canonical SHA-256, the source and
target History coordinates and the active journal frames. This is a bounded,
**local recovery checkpoint** (up to 32 MiB encoded), not another permanent
network copy kept for every Undo step. An intent already present prevents
starting another uncertain navigation.

On opening that same draft, `PersonalDraftEditSession::start()` checks for the
intent **before** the ordinary network hydration path. If it exists and
validates, the session opens its durable edit queue around the recorded source
network and enters the normal hash/cursor reconciliation state machine. It
never issues the full-model GET as a prerequisite to this recovery.

The intent is removed **only after** a confirmed navigation, verified model
state and successful durable intent acknowledgement. If it is corrupt,
wrongly scoped, oversized, or cannot be reconciled, editing is blocked; the
saved recovery information must not be silently discarded. A restart with
**no uncertain navigation** still uses the normal draft open/hydration path,
which can download the full model once.

## Durability and platform boundaries

- **DVMDB** is authoritative; the GUI does not invent an independent inverse
  transaction or silently perform local-only Undo.
- **Desktop/Standalone:** the queue root and intent store need durable,
  writable storage. Embedded DVMDB uses HTTP over loopback, not a special
  editor-only database code path.
- **WebAssembly:** the queue/intent root must actually reside on persistent
  browser-backed storage and be flushed appropriately. Process-restart
  recovery cannot be promised for an ephemeral in-memory filesystem.
- **Power failure, disk full, interrupted filesystem sync and browser eviction**
  require separate platform acceptance. The implementation provides safety
  guards; source inspection does not prove every failure mode in deployment.
- Normal History navigation **is not** whole-project Fetch/Publish and does
  not provide fully offline editing against an unreachable remote personal
  REST server. It provides reconnection/reconciliation, not a hidden server
  replacement.

## Relevant source and tests

- `src/projects/personal_draft_edit_session.{h,cpp}` — cursor permissions,
  navigation, sparse verification, retries, reconciliation and startup.
- `src/projects/personal_history_navigation_intent_store.{h,cpp}` — bounded,
  checksummed persistent pending navigation record.
- `src/projects/hydraulic_history_delta.*` — canonical model digest and
  typed sparse application across the hydraulic collections.
- `src/projects/personal_working_copy_client.*` — fenced HTTP navigation,
  read-only reconciliation and idempotent replay.
- `tests/personal_working_copy_client_test.cpp` — lost responses, automatic
  retry, same-operation recovery, client restart, unexecuted operations,
  mismatched cursors, corrupted intents, sparse replay and no full-network
  GET during supported navigation/recovery.

Source tests exercise the state machine with a stub HTTP server. A real
process-kill + independent DVMDB daemon, embedded Standalone and browser
persistence acceptance remain distinct checks. See
[verification status](verification-status.md) and the
[remaining History acceptance work](history-roadmap.md).
