# History interaction contract — GUI

**Status: HIST-00 architecture, 2026-11-03. Navigation not implemented.**
This document specifies the behavior of the already implemented **read-only
History dock** once HIST-01–04 make its actions safe. The authoritative
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
  original per-action history was never retained. A checkpoint may remain a
  jump target even though individual missing actions cannot be replayed.
- A merge checkpoint with a link to its other parent(s), not a fabricated
  linear ordering of concurrent actions from both branches.

## Undo, Redo and jump

**Undo** selects the immediately preceding reconstructable user-action
state. **Redo** selects the next action on the active redo path. Neither
consumes a Commit marker step. **Jump** selects a known action-state cursor
or an exact immutable checkpoint; it is a single requested, atomic history
transition, not a visible loop of unconfirmed Undo clicks.

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
Unborn branches and branches with only snapshot checkpoints must not
pretend to have ordinary Undo steps. Revision and branch metadata edits
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
  eligible target. Direct row jumps and keyboard shortcuts are not yet wired.
  An unavailable position must not issue a write.
- **Future direct jumps:** a click on a Commit marker should select its
  exact snapshot, and a click on an action should select the state **after**
  that action. Arbitrary row selection is currently disabled; only adjacent
  server-provided Undo/Redo coordinates are executable.
- Load long histories by pages at a stable history watermark, preserving
  scroll position and selected cursor. Never truncate silently to the
  draft's current journal capacity or show a first page as the entire past.
- Show meaningful action labels, but never use a label alone as identity.
  Before/after images, action IDs and revision IDs remain authoritative.
- When navigation is rejected, keep the unchanged model and cursor; expose
  the actual conflict/recovery reason rather than reporting success.

## Integration with the live editor

After an accepted History navigation, `PersonalDraftEditSession` reopens
DVMDB and rehydrates the authoritative network. `MainWindow` disables
logical edit capture before navigation, replaces the canonical network and
resumes capture only after the stage has been reset and the network has
passed its consistency check. This avoids recording a synthetic second
MODEL action. Full 2D/3D, selection and inspector acceptance remains open.
Regular editor mutations still create normal MODEL frames through
`HydraulicEditTransactionCapture` and the private queue.
Network-root metadata and 18 collections participate in the same atomic
user action where necessary. Invalid hydraulic *solvability* alone does not
block navigation; canonical codec, reference identity and membership guards
still apply.

A session closing, switching branches, timing out or losing its fence must
reconcile pending history operations against the original draft. The GUI
must not accidentally apply an old result to whatever network happened to
be opened next. Reopen reloads the authoritative cursor and reconstructs
state only from the matching repository/project/branch/draft/network scope.

## Current acceptance boundary

DVMDB retains permanent indexed local-Commit MODEL action archives, verifies
canonical reconstruction and provides fenced HTTP cursor navigation. The GUI
wires adjacent Undo/Redo to that controller for eligible private drafts, but
**the current tests have not yet proved the complete GUI + server + restart
workflow**. The visual list does not yet expand permanent archived action
frames, and direct jumps, shortcuts and unborn-base navigation remain
outstanding. See the [History roadmap](history-roadmap.md).
