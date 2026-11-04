# History — remaining GUI and end-to-end work

This page lists only **outstanding** work. The shipped implementation is
covered in [Projects workspace](projects-workspace.md),
[History interaction contract](history-contract.md) and the
[DVMDB History contract](../../AOWIS-SERVER-DVMDB/docs/history-contract.md).

## Current implementation

The left **History** dock opens from the existing toolbar control without
leaving Distribution Editor. It shows first-parent immutable Commit markers,
acknowledged draft MODEL actions and local pending events. Opening a Commit
loads genuine archived MODEL descriptions from the private DVMDB in pages
of 50 actions; neither row clicks nor archive retrieval change model state. While a private
draft writer is ready and its queue is empty, the GUI reads the authoritative
DVMDB History cursor and enables only server-advertised adjacent Undo/Redo
coordinates. It never derives eligibility from visible labels.

The private session performs fenced, UUIDv7-identified HTTP navigation,
reopens the acknowledged draft, rehydrates the saved canonical network and
replaces the editor network with logical edit capture disabled. Only after
that complete verified reload does capture resume. On an uncertain result
it blocks editing and requires an authoritative reopen; it does not assume
the operation succeeded. New ordinary Saves after Undo branch the current
journal and invalidate the abandoned redo tail in DVMDB. Commit markers do
not move or rewrite immutable HEADs.

DVMDB currently supports this navigation for one **existing** hydraulic
network in a private draft whose base is a committed revision, and for
eligible archived action sequences. Unborn-base draft action navigation and
multi-network navigation are not implemented. This does **not** restrict
DVMDB's ability to store multiple networks per revision.

## Remaining acceptance and usability gates

1. **Prove the full external-server path:** GUI edit → autosave → Undo →
   Redo → Undo across two Commit markers → return to draft → new edit after
   Undo → close/reopen GUI and DVMDB, with exact canonical network equality,
   immutable HEAD unchanged, and no forgotten queued operations.
2. **Qualify failure recovery:** lost POST response, stale generation, expired
   lease, changed HEAD, disconnected server, corrupted archive and attempted
   navigation while a Save is in flight. All must fail closed and never
   leave editor capture active with unverified model state.
3. **Make the whole branch timeline complete:** the dock now expands archived
   action pages for Commits present in the Projects first-parent graph.
   Independently page older Commit ancestry from DVMDB when the Projects
   preview is incomplete, explicitly identify snapshot-only checkpoints,
   and retain a stable branch watermark during refresh. Never imply an
   incomplete ancestry is complete.
4. **Add direct history jumps:** selecting a specific verified action or
   checkpoint from the list must use one atomic server navigation request.
   Keep long-history page cursors stable and avoid mismatched branches.
5. **Add keyboard shortcuts:** expose context-sensitive Undo/Redo actions
   without stealing text-field Undo/Redo, map controls or native Qt editor
   shortcuts. Bind availability to the same authoritative cursor as the dock.
6. **Support normal newborn branches:** DVMDB must verify actions in an
   unborn-base first draft (network initialization/import included) before
   the GUI offers Undo for that history.
7. **Standalone and WASM acceptance:** repeat the verified network/history
   path with embedded DVMDB and Qt WebAssembly, then document results and
   remaining platform-specific limitations.

Multi-network navigation may be added later if the product needs it; it is
**not a prerequisite** for the single-network user workflow. No solver
convergence or connected-supply requirement is introduced by History; MODEL
record and UUID/reference integrity remain mandatory.
