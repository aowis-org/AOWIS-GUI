# Private working-copy Save, recovery and session safety

This is the current **implementation contract** for `PersonalDraftEditSession`,
`PersonalDraftEditStage`, `PersonalEditQueue`,
`PersonalWorkingCopyClient`, `HydraulicDraftEventAdapter` and the
`MainWindow` edit binding. **Save draft is not Commit or Publish.**

## Targets and identity

A private working copy belongs to the user-selected **personal DVMDB**
repository. Each operation is tied to the verified repository UUIDv7,
project UUIDv7, branch UUIDv7, working-copy UUIDv7 and network UUIDv7.
The draft records its pinned committed base (or a null/unborn base), saved
generation and writer ownership. Never fall back to a selected shared
browsing repository because the personal target is unavailable.

A desktop GUI uses its configured private HTTP DVMDB source. Standalone
embeds the native DVMDB `ProjectService` and its HTTP routes **inside the
GUI process**, bound to an ephemeral `127.0.0.1` port; local profile paths
are durable and the dynamic endpoint is never persisted. Both routes use
the same checked client contract. “Embedded” is a deployment distinction,
not permission to skip DVMDB validation or fencing.

## Read and start a draft

1. Select the exact project and branch. A new project may have an unborn
   `main` with no immutable revision.
2. Create or reopen a private draft on the personal target. An empty unborn
   working copy can be initialized with a server-owned empty network root.
   **Edit network** first checks the branch's single draft against the
   branch's current HEAD in the personal repository (see *Stale drafts*).
3. Load the verified authoritative candidate, including draft overlay and
   saved generation.
4. Open the local edit queue (this takes its per-draft lock file) and replay
   pending local events onto the candidate. The oldest event is retired
   locally if it is already in the server journal tail, **or** if the
   candidate already holds exactly its result (its Save reached DVMDB, the
   acknowledgement was lost and a later Commit cleared the journal). Any
   other event that does not replay blocks the session; it is kept.
5. Acquire the fenced writer lease, verify the generation once more and
   bind the editor only to this session. The queue lock is taken **before**
   the lease: every writer of a draft leases with the draft's session UUID
   and each acquisition rotates the fencing token, so a second GUI window
   that leased first would fence out the running session. Now it fails on
   the lock without touching the lease. A read-only Monitor preview is
   **not** a writable editor session.
6. Install the verified candidate into `HydraulicData` and enable supported
   logical edit capture only after session readiness.

The GUI's normal entry is **Projects → Project Inspector → Edit network** for
an unambiguous personal project/branch. The branch inspector's **Personal
working copies…** dialog also offers **Edit in Distribution Editor**.

### Stale drafts

A draft keeps the base revision it was created on. When the branch has
moved on since (another draft's Commit, a later checkout), opening "the
branch" must not silently show the older network.
`PersonalWorkingCopyDialog::verifyDirectDraft()` reads the branch HEAD in
the personal repository (`GET …/projects/{id}/branch-ids/{branchId}`):

- **Branch from another repository, unmodified draft** (not dirty, no queued
  local edits): newer source commits are pulled first with the branch
  checkout. If that fails (for example, the personal branch has local
  commits the source lacks), the personal HEAD is used as it is.
- **Unmodified draft on an older base:** it is deleted under a lease of a
  fresh session (`POST …/working-copies/{id}/discard`; a lost reply is
  resolved by reopening, "not found" confirms it) and a new draft is
  created at the current HEAD. A draft held by a running editor is not
  deleted.
- **Draft with edits on an older base:** not opened automatically. The
  dialog names both revisions and offers **Edit draft anyway**; its Commit
  will be refused while the branch has moved on.

## Canonical entity transactions and membership

MODEL owns canonical encoding of `ModelEditTransaction` and entity types.
`HydraulicDraftEventAdapter::prepareWithMembership()` replays the queued
transaction against the **previous staged candidate**, emitting typed DVMDB
records and create/delete membership intents. The 18 collection kinds and
ordinals are shared in `hydraulic_draft_entity_registry.h`.

The `/dvmdb/v1/working-copies/{draftId}/save-events` request contains the
encoded event, changed records, exact expected generation and writer
fencing information. For creates/deletes it also contains matching
`membership` entries (`networkId`, `entityId`, `kind`, `collectionIndex`,
`inserted`). DVMDB updates its server-owned membership atomically and
rejects missing/mismatched intents. Do not synthesize network root or
membership-chunk records in the GUI.

**Entity identities.** The transaction UUID is a UUIDv7 (the editor creates
it; DVMDB uses it to recognise a resent Save). Entity UUIDs are model keys
and may be any non-null UUID: EPANET imports carry deterministic UUIDv5
keys. MODEL's edit codec required UUIDv7 for entity keys until the
2026-11-02 MODEL patch (`aowis-server-model-entity-uuid-any-version.zip`),
so every edit of an imported entity failed at capture
("Entity edits must be valid, changed and have unique UUIDv7"). GUI and
DVMDB must both build the patched MODEL.

**Collection order.** The stage and DVMDB's membership chunks append a
created entity to the end of its collection and remove entities in order.
Every editor operation must do the same, because the live model and the
staged candidate are compared byte for byte. Pipe split inserted the new
pipe after the original and blocked the session after each split; it now
appends.

**Per-operation bounds.** One event is sent per Save, and `/save-events`
accepts at most 128 typed records, 128 membership intents and an 8 MiB
body. `PersonalDraftEditStage::append()` refuses an event beyond these
bounds (about 7 MiB, leaving room for JSON framing) **before** it is
queued: the session blocks with a message naming the size, nothing durable
refers to the event, and reopening the draft restores the saved network.
Before, such an event (for example moving a selection of ~45 junctions with
their pipes) was queued and failed on every reopen.

The **network root's persistent metadata is currently outside this edit
transport**. HC-03/HC-04/HC-05 must define and implement a logical metadata
transaction before root settings are described as autosaved.

## Ordering, acknowledgement and recovery

A captured edit first becomes a durable local queue entry. The session
submits events **oldest first**, with the expected saved generation and
valid lease fencing. It never reconstructs an old queued event from the
latest, possibly more advanced `HydraulicData` state.

When Save succeeds, verify authoritative event identity, accepted count and
new generation before retiring the queue item. If the response is dropped,
`saveEventsRecovering()` reopens authoritative DVMDB state and checks the
actual event bytes and records instead of guessing whether the POST failed.
The same recovery is necessary when the GUI terminates after server
acceptance but before local queue deletion.

Lease renewal is serialized between Saves because a renewed fencing token
invalidates the old one. Stale generation, stale lease, inconsistent journal,
invalid replay, wrong target or uncertain local disk persistence **blocks
new writes** and retains the queue for explicit recovery. Explicit stop
releases the active lease when possible; server expiry handles crashes.

`MainWindow` listens for captured logical edits, stages them through the
session and checks canonical equality between the live GUI network and the
staged candidate. Divergence blocks editing; a block does not prove the
unsupported change was safely saved. The implementation at this checkpoint
uses the visual revision as a comparison trigger; it is **not** an exhaustive
watch for root metadata modifications that do not announce a revision.
If edit capture is switched off while a session is open and not blocked
(a whole-model replacement does that), the check now blocks the session;
before, it skipped itself and later edits were neither saved nor flagged.

## Local Commit

**HEAD Inspector → Commit** (with a commit message) commits the branch's
single modified saved draft to the personal repository:

1. The draft must be dirty and the only modified draft of the branch.
2. Commit is refused while **edits of that draft are still queued on this
   device** (`PersonalEditQueue::pendingTransactionFileCount()` on
   `PersonalEditQueue::defaultRoot()`): they would be left out of the
   revision. Open the network so they are saved, stop editing, commit.
3. A lease under a fresh session UUID fences out other writers; an open
   editor session makes Commit fail with "Draft is being edited".
4. `POST /local-commits` with a new operation UUIDv7, expected generation
   and expected HEAD. A lost reply is reconciled with
   `GET /local-commits/{operationId}`, never by a second POST; a pending
   result names its operation UUID.
5. After success the lease is **released at once**; before, it stayed held
   for 120 s and reopening the draft failed with "draft held by another
   writer session". DVMDB rebases the draft onto the new revision.

The author is one UUIDv7 per installation, created on first Commit and kept
in the GUI ini (`dvmdb/commit_author_id`; browser storage on WASM), with the
operating-system account name (`USER`/`USERNAME`, else "AOWIS user"). This
is an interim identity until the security concept defines users.

## Proven and outstanding behavior

Source-level implementation includes event queue/replay, a fenced HTTP
writer, acknowledgement recovery, and actual editor binding for supported
captured edits. Focused unit and HTTP-fixture tests exist. A real external
HTTP DVMDB GUI restart test preserved a network created in the editor.

**Do not claim full persistence acceptance yet.** Outstanding evidence
includes all 18 entity editing paths, root metadata, multi-event interrupted
runs, wrong-repository failures, GUI+DVMDB process restarts, and equivalent
Standalone embedded operation. The 2026-11-02 fixes above (stale drafts,
queue-before-lease, already-applied recovery, per-operation bounds, split
order, Commit lease release and queue check, entity UUIDs) are delivered
but **not yet built or tested**.

Known limits that still block a session: a draft journal holds at most 4096
events / 8 MiB and a candidate at most 20 000 records / 8 MiB until a
Commit, with no warning before the limit; imports above 5 MiB canonical or
20 000 entities cannot enter a draft.

Import/replacement/merge semantics have **not** been agreed. Since
2026-11-02 an **interim rule** refuses import and built-in examples while a
private session is open; see
[network import decisions](network-open-import-replacement.md) before
changing model replacement semantics.

See [hydraulic edit transactions](hydraulic-edit-transactions.md) and
[implementation roadmap](dvmdb-integration-roadmap.md).
