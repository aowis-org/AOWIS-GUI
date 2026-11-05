# History interaction contract — GUI

**Status: current GUI History behavior (2026-11-05 implementation).**
This document describes the implemented History dock, its authoritative
navigation boundaries and the remaining user-facing limitations. Implemented
code is not a claim of complete GUI/server restart acceptance. The authoritative
persistence/recovery design is in the
[DVMDB history contract](../../AOWIS-SERVER-DVMDB/docs/history-contract.md).
The [History roadmap](history-roadmap.md) lists remaining work. This page
specifies both the currently wired adjacent navigation and the intended
complete timeline. See [Projects workspace](projects-workspace.md) for
observable GUI behavior.

## Scope and ordering

The dock follows the **active project, branch, draft and network**. It must
not conflate a read-only selected revision with a writable private draft.
Commit markers are loaded from the real first-parent ancestry; actions are
loaded from the authoritative DVMDB action archive, with the local pending
queue displayed separately as unconfirmed actions. No original pre-Commit
MODEL actions are inferred from immutable revision snapshots.

A meaningful user action is one history step: one completed node drag,
property edit, pipe split, cascaded delete or grouped library change. A
MODEL event may touch many entities and root metadata in that same action.
Live mouse movement and every intermediate text keystroke must not create
hundreds of user-facing steps. Backend action IDs are UUIDv7 and remain
stable even if branch display names or action labels change.

The History dock presents (chronologically):

- A meaningful model action with its description, saved/pending/rejected
  status and original action identity (not a raw manifest/chunk write).
- A visually distinct **Commit marker** with message, revision UUID and
  author/time when known. It is a checkpoint, **not** an Undo step.
- A visible current-state cursor; redo-eligible actions lie after it on the
  active path. A new edit after Undo invalidates ordinary Redo on that
  suffix while leaving committed revisions and the audit intact.
- **Snapshot-only** gaps where exact immutable revisions exist but the
  original per-action history was never retained. These checkpoints are
  displayed, but direct jumps to them are not currently offered by the
  verified MODEL-action navigation API.
- A merge checkpoint with a link to its other parent(s), not a fabricated
  linear ordering of concurrent actions from both branches.

## Undo, Redo and jump

**Undo** selects the immediately preceding reconstructable user-action
state. **Redo** selects the next action on the active redo path. Neither
consumes a Commit marker step. **Jump** selects a known action-state cursor or an eligible archived Commit
action-tip checkpoint; it is a single requested, atomic history transition,
not a visible loop of unconfirmed Undo clicks. Snapshot-only immutable
checkpoints currently remain read-only.

Crossing a Commit must never mutate or remove that immutable revision or
move the branch's committed HEAD. Instead the **private candidate** receives
fenced, journaled changes that reproduce the requested historical contents.
The branch's actual committed HEAD remains the base for any future Commit;
creating a branch whose ancestry starts at an old revision is a separate
operation. A committed checkpoint may be displayed in the middle of a
continuous editing session without interrupting navigation.

Successful Undo/Redo/jump updates the authoritative history cursor, editor
network, selection/inspector data, status and local timeline **together**.
Before success, keep the request pending/blocked in the dock; do not move the
visible cursor just because HTTP was sent. A lost acknowledgement is queried
and retried only with the same operation UUID. An optimistic preview must
not be mistaken for durable acceptance.

If no personal editing session or no live writer lease is available, show
history **read-only**; provide contextual information rather than enabling
navigation. A dirty queued Save must be reconciled before navigating.
An unborn branch with no journal has no Undo steps. Once a network has
been initialized and original MODEL actions have been saved, the verified
single-network draft cursor may navigate those actions without inventing
a Commit. An unborn deletion with unprovable membership order remains
ineligible; checkpoint-only revisions must not invent MODEL actions. Revision and branch metadata edits
are informational until a separate metadata-history contract is designed.

## Dock behavior and controls

- The existing **Open revision history in Projects** toolbar button opens or
  focuses the dock on the **left**, without switching the current editor
  away from Distribution. The separate Projects graph remains the full DAG.
- The dock is movable/closable and follows map fullscreen behavior. It
  refreshes when the active draft, branch, project, or saved generation
  changes, but must not silently switch scope during pending navigation.
- **Undo/Redo buttons are wired today** to DVMDB's server-resolved adjacent
  action coordinates. They are enabled only for a ready private writer, no
  pending Save, a verified generation/HEAD cursor, a valid lease and an
  eligible target. Exact row jumps use the same fenced publication path.
  Keyboard shortcuts are not yet wired.
  An unavailable position must not issue a write.
- **Archived actions:** each Commit has a separate disclosure row for
  expanding its original archived MODEL actions, fetched from the **private**
  DVMDB in bounded pages of 50.
  The actions are placed chronologically *before* their Commit checkpoint,
  with their descriptions, stable action UUIDs and entity edit counts. A
  **Load more** row requests the next page; unavailable pages show an error
  rather than silently hiding older actions. Expanded pages are cached only
  for the same private draft and branch. No original actions are fabricated
  for checkpoint-only revisions.
- **Direct jumps:** clicking a saved draft action or an original archived
  action selects its exact numeric action boundary. Clicking a Commit
  checkpoint selects the archive tip, using DVMDB’s validated total action
  count rather than the number of rows loaded. On the first click, the GUI
  fetches at most the first archive page to establish that total, then sends
  one fenced navigation POST. A snapshot-only checkpoint with no archived
  MODEL actions remains read-only. Disclosure, paging, pending actions and
  the current-state marker never navigate. The DVMDB backend re-verifies
  archive coverage and rejects ineligible revisions; the GUI never grants
  eligibility from labels or from a partial local replay.
- **Commit ancestry paging:** while a private draft is open, fetch the
  actual first-parent Commit history from DVMDB independently of the Projects
  graph preview, 10 Commits per page. A **Load earlier Commits** row follows
  DVMDB's exclusive revision cursor, while **Refresh committed History**
  rereads the branch tip. The GUI validates first-parent continuity,
  de-duplicates revision UUIDs, and keeps already loaded pages when an older
  request fails. The dock displays oldest-to-newest even though DVMDB's
  transport pages arrive newest-first. On a missing/unavailable private draft,
  the Projects preview remains a read-only fallback and reports incomplete
  ancestry rather than silently treating it as exhaustive. Archived MODEL
  actions are paginated separately, 50 at a time.
- Preserve scroll position and selected action cursor across updates. Never
  truncate silently to the current draft journal or graph-preview capacity.
- Show meaningful action labels, but never use a label alone as identity.
  Before/after images, action IDs and revision IDs remain authoritative.
- When navigation is rejected, keep the unchanged model and cursor; expose
  the actual conflict/recovery reason rather than reporting success.

## Integration with the live editor

After an accepted History navigation, `PersonalDraftEditSession` applies a
verified sparse `historyDelta` to a temporary canonical network. It checks
entity before-images, collection membership and final SHA-256 before
publishing that network to the existing editor. Routine Undo/Redo or eligible
direct jumps do **not** reopen the draft or issue a full-model GET. New
logical capture is enabled only after the authoritative cursor is refreshed
and its position is writable.

DVMDB is the authority for the selected historical state. Its immutable
Commit archives, active MODEL journal and working-copy cursor are not
replaced with a second GUI Undo stack. A new edit from an archived position
starts a valid logical path and discards forward Redo; immutable revisions
and the committed branch HEAD remain unchanged. Editing does not force a
switch into Distribution Monitor.

When navigation acknowledgement is uncertain, the GUI repeats the *same*
UUID and source generation (bounded immediate retries). On a prolonged
outage, the session pauses editing and reads the small
`/history/reconcile?networkId=<uuid>` receipt once reachable, validates
operation identity, generation, exact History position and network SHA-256,
and reacquires the **same writer session's** lease. A matching digest by
itself is insufficient: two different actions can produce identical model
bytes. The GUI either confirms the matching destination without a MODEL
transfer, replays the original pinned sparse delta, or retries an original
unpublished operation without changing its UUID.

For a **GUI crash/restart**, `PersonalHistoryNavigationIntentStore` preserves
the source network, SHA-256, journal frames, scoped IDs, source and target
coordinates, and original operation UUID before the POST. `start()` checks
for that record before ordinary network hydration. A valid intent permits
hash/cursor reconciliation and sparse replay **without an initial full-model
GET**. Invalid, mismatched or inaccessible records fail closed. The record
is cleared only after the authoritative result is verified and acknowledged.
A normal open with no uncertain intent continues to load the network
once. Neither case provides fully offline editing when the personal working
server is remote and unreachable.

The rendered network may be replaced in memory after verification; this is
not a network re-download. Selection/inspector synchronization and complete
2D/3D end-to-end acceptance remain distinct test requirements.

The full state machine and platform caveats are documented in
[History navigation recovery](history-recovery.md).

## Current acceptance boundary

The GUI source implements sparse History navigation, crash-durable pending
navigation intents, long-outage reconnect and verified forked editing. Its
component tests cover lost replies, retries, restart and corrupt/mismatched
intents. End-to-end process-kill, independent DVMDB server, Standalone and
WebAssembly browser storage acceptance still need to be recorded. Keyboard
shortcuts, limited unborn deletion ordering and multi-network History
navigation remain separate usability/scope decisions.

See [verification status](verification-status.md) and
[future History acceptance](history-roadmap.md).
