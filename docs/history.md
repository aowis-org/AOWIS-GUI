# Using History — Undo, Redo and Commit checkpoints

The **History** dock is opened by **Open revision history in Projects** in
the main toolbar. It is a left-side dock that can remain visible while
editing a hydraulic network. Unlike the Projects revision graph, it follows
the *editing actions* along the selected branch's first-parent timeline.

This page explains the user-facing concept and the controls present in the
2026-11-04 GUI source. For the exact navigation contract and guards see
[History interaction contract](history-contract.md); for work not yet
accepted see [History roadmap](history-roadmap.md).

## One timeline, two kinds of entries

Suppose you work on a distribution network:

```text
Initial network
  Add junction J1
  Add pipe P1
  ── Commit R1: "Initial network" ──
  Move junction J1
  Change pipe diameter
  ── Commit R2: "Adjusted layout" ──
  Add pump PU1
  Change pump properties      ◀ current position
```

- An **action** is one recorded MODEL transaction: one meaningful edit may
  update several objects together. It is one Undo step, not one step per
  entity or per intermediate mouse movement.
- A **Commit marker** identifies a permanent immutable revision. It is a
  checkpoint **between** actions, **not** an additional Undo step.
- The **current cursor** identifies the network state being displayed and
  persisted in your *private working copy*. Actions after the cursor are
  eligible for Redo only while their active path is retained.

Press **Undo** repeatedly and the cursor moves backward through actions,
including across Commit markers when DVMDB verifies those action archives.
Press **Redo** to move forward. The marker itself does not consume a button
press. History never erases an immutable revision or silently moves its
committed branch HEAD.

## Why Undo can cross a Commit

DVMDB retains two different things:

1. **Immutable revision snapshots**: the committed contents and ancestry of
   `R1`, `R2`, ...; once created they are never edited by History.
2. **Original MODEL actions**: a permanent archive linked to each eligible
   local Commit, plus an active journal for uncommitted edits.

To navigate to the state after **Add pipe P1**, the server verifies the
archived actions and reconstructs that historical network. It then builds a
**replacement candidate overlay relative to the current committed HEAD**,
publishes the overlay and History cursor atomically, and returns the
acknowledged state. R2 is **still the committed HEAD**. AOWIS has not reset
the branch, deleted a Commit or edited an immutable snapshot.

The GUI subsequently reloads the authoritative saved network before enabling
logical edit capture again. If that reload or the HTTP result is uncertain,
editing remains blocked until the actual draft state is reconciled.

## How to navigate in the dock

| Interaction | Result |
|---|---|
| **Undo** / **Redo** | Select the next server-verified adjacent action position, across eligible Commit markers |
| **Click a saved draft action** | Jump directly to the state after that draft action |
| **Click an archived action** | Jump directly to that position within the selected Commit's original actions |
| **Click an eligible Commit marker** | Jump to the network state at that Commit's final archived action boundary |
| **Click the Commit's disclosure row** | Expand/collapse original archived actions **without** navigating |
| **Load more** | Fetch another page of up to 50 archived actions; not a navigation command |
| **Pending action / current-state marker** | Informational; not a navigation target |

Original action descriptions, UUIDs and edit counts come from the private
DVMDB's indexed action archive. The archive is loaded in bounded pages; the
checkpoint coordinate uses the server-reported total, **not** the number of
rows currently visible. Commit snapshots that predate a complete archive are
shown as **checkpoint-only** and cannot be treated as verified action jumps.

## What if I Undo and start editing again?

For an active uncommitted draft:

```text
A → B → C → D
    ▲ current cursor after two Undo operations

New edit E:

A → B → E
        ▲ current cursor
```

The previous active Redo suffix `C → D` is invalidated when E is accepted.
The new edit and remaining applied prefix are saved atomically as the new
active journal path. **Immutable Commits and their permanent archives are
unaffected**; draft Redo is not a substitute for version-control branching.

When browsing an older **archived** action, the editor remains in its
selected **Distribution Editor** mode. Undo/Redo can continue across Commit
markers in either direction. On the first new accepted MODEL edit, DVMDB
atomically starts a forked action path at that coordinate and discards the
abandoned forward Redo steps. All immutable Commits, their original MODEL
action archives and their revision DAG ancestry remain untouched. The next
Commit records the fork provenance so later History reads follow the new
action path rather than replaying the abandoned suffix.

## When navigation is disabled

The authoritative DVMDB cursor and the private writer decide whether an
action is eligible. The GUI disables History navigation while it is saving,
the writer lease or generation is not verified, the draft scope changes, a
navigation response is unresolved, or the server cannot reconstruct the
target. The GUI does not execute an optimistic local-only Undo then hope
that it matches the persisted database.

The current executable navigation adapter supports **one unambiguous
hydraulic network**, including an initialized unborn-first-draft seed and
verified archived MODEL actions. Ambiguous original deletion ordering still
fails closed. **DVMDB may store multiple networks per revision**; History
does not restrict the storage model. The dock pages complete first-parent
Commit ancestry independently of the Projects preview. Keyboard
shortcuts are not wired; use the dock controls.

## What survives restart?

DVMDB persists the working-copy candidate, cursor and original action frames
under versioned working-copy metadata. The GUI also retains an unsent local
MODEL event queue. Restart must re-establish the correct repository, project,
branch, draft and writer lease; restore the server's authoritative cursor;
and reconcile any unacknowledged Save/navigation operation by its original
UUID rather than assuming success or creating another operation.

The persistence machinery is in source, but complete end-to-end
GUI + daemon restart acceptance remains open. The latest test evidence is
recorded in [verification status](verification-status.md).
