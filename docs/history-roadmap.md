# History — remaining work and acceptance

This roadmap contains **future tasks only**. For implemented behavior see
[History](history-contract.md), [Projects](projects-workspace.md), and
[private Save](personal-working-copy-save.md). For evidence and the latest
unverified Qt/HTTP gates see [verification status](verification-status.md).

The existing implementation already has a left History dock, saved and
pending actions, paginated permanent Commit archives, Undo/Redo buttons,
direct action/eligible checkpoint selection, fenced HTTP cursor navigation,
MODEL-verified reconstruction, durable cursor recovery, retained Redo across
Commit boundaries and atomic Redo invalidation after a new edit. All of
those are **implemented code**, not new roadmap milestones. Immutable
Commit snapshots and the branch's committed HEAD remain unchanged by Undo.

## 1. Real editor + DVMDB acceptance (critical)

Prove with a real single-network editor session and separate DVMDB daemon:

1. Open a committed network; make representative property, structural,
   library, controls, map-label and root-metadata edits in 2D and 3D.
2. Confirm autosaved MODEL events and exact canonical network equality;
   Undo and Redo several active-draft actions.
3. Create two genuine local Commits, navigate backward and forward over
   their markers, jump to an archived action and return to the draft.
4. Undo, make a **new** edit, verify the former Redo suffix is no longer
   available, and Commit only eligible actions. The old immutable revisions
   must remain unchanged.
5. Restart the GUI **and** DVMDB during each relevant cursor state, reopen
   the same personal repository/branch/draft and verify exact model bytes,
   action IDs, journal position and selected network.

Keep automated Qt/HTTP tests separate from this manual integration evidence.
A passing component test is not a substitute for the workflow above.

## 2. Concurrency and uncertain-result recovery (critical)

Exercise lost navigation acknowledgements and original-operation-ID retry,
stale draft generations, expired leases, changed committed HEAD, Save in
flight, dropped network reload, incomplete/corrupt Commit archives, server
disconnect and switching scope during an archive page/POST. The GUI must
never claim successful navigation before authoritative acknowledgement and
canonical network reload; it must never resume capture against unverified
state. Check a suspended active journal's Redo frames survive restart and
navigation into/out of archived History.

## 3. Full first-parent branch timeline (important)

The dock currently gets visible Commit markers from the Projects preview.
Implement independent, watermark-pinned paging of **all** first-parent
Commit ancestry from DVMDB. Detect incomplete pages, expose snapshot-only
checkpoints without inventing MODEL actions, retain scroll/cursor across
updates and load archived action metadata in bounded pages of 50.

## 4. Direct-jump acceptance and ergonomics (important)

Test clicking a saved action, an archived action (including pages beyond
50) and an eligible Commit checkpoint whose total actions exceed the
loaded page. Verify exact action boundaries, retries, stale generation,
branch switching, empty/failed page responses and unchanged selection on
failure. Check pending actions and checkpoint-only revisions remain
non-navigable.

Add context-sensitive keyboard shortcuts for History Undo/Redo without
capturing text-field Undo/Redo or conflicting with map and Qt controls.
Expose accurate unavailable reasons in the dock.

## 5. First/unborn working-copy history (important)

Qualify the implemented initial unborn-draft path in the real GUI: the
already initialized/imported network is a seed (not an Undo step), followed
by journaled creations and property changes. Verify Undo of the first tank,
Redo, new edits after Undo, restart recovery, imported-network seeds and
refusal of unsupported deletions when original membership ordering cannot
be proved. Extend ordering provenance before enabling those deletions.

## 6. Platform qualification

Repeat the accepted workflow in Standalone with embedded DVMDB and in the
supported WASM/browser client. Record actual build configuration and test
results; avoid assuming Linux desktop success extends to other platforms.

**Not a prerequisite:** multi-network History navigation. DVMDB may store
multiple independent `NetworkHydraulic` objects in a revision. We are
leaving that native capability intact and limiting this History acceptance
to one existing network. No solver feasibility requirement is introduced.
