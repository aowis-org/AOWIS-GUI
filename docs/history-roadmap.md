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
   their markers, jump to an archived action while staying in Distribution Editor, then
   return to the draft (capture remains fenced until cursor verification).
4. Undo past one or more Commits, make a **new** edit without leaving
   Distribution Editor, verify the former Redo suffix is no longer available,
   and Commit only eligible actions. The old immutable revisions
   and original Commit archives must remain unchanged, while the active
   action timeline follows the new logical fork.
5. Restart the GUI **and** DVMDB during each relevant cursor state, reopen
   the same personal repository/branch/draft and verify exact model bytes,
   action IDs, journal position and selected network.

Keep automated Qt/HTTP tests separate from this manual integration evidence.
A passing component test is not a substitute for the workflow above.

## 2. Concurrency and uncertain-result recovery (critical)

Verify the implemented bounded immediate same-operation-ID transport retries,
GUI-session long-outage reconciliation with lease rotation/backoff, and
server-side retained-source delta regeneration under dropped responses,
server restart and interrupted connections. Also exercise stale draft generations, expired leases, changed committed HEAD, Save in
flight, dropped network reload, incomplete/corrupt Commit archives, server
disconnect and switching scope during an archive page/POST. The GUI must
never claim successful navigation before authoritative acknowledgement and
canonical sparse-delta verification; it must never resume capture against unverified
state. Check a suspended active journal's Redo frames survive restart and
navigation into/out of archived History.

**Still to implement:** durable on-device pending History navigation intents
so an uncertain navigation can reconcile after the GUI itself restarts,
without first downloading a complete network. The current open-session
reconnect is in-memory and therefore cannot provide that guarantee.

## 3. Independent branch-history paging acceptance (important)

The History dock now reads committed first-parent checkpoints independently
from the Projects graph preview: **10 Commits per DVMDB page**, newest-first
on the wire and oldest-first in the dock. **Load earlier Commits** follows the
server's exclusive revision cursor. The GUI validates first-parent continuity,
rejects inconsistent or duplicate pages, and keeps already loaded pages on
older-page failure. **Refresh committed History** restarts at the current
branch tip. Archived MODEL descriptions remain separately paged in groups of
50; checkpoint-only Commits never acquire fabricated actions.

Accept long first-parent chains across many pages, branch-tip advancement
while pages are loaded, refresh/retry after disconnect, merge first-parent
ordering, private repository changes and delayed responses from a previous
scope. Confirm the visible cursor and scroll position do not jump unexpectedly.
These are **acceptance tests**, not another paging implementation milestone.

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
