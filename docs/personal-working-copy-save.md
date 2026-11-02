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
3. Load the verified authoritative candidate, including draft overlay and
   saved generation. Replay any pending local events that are not already
   confirmed in the server journal.
4. Acquire the current fenced writer lease, verify generation and bind the
   editor only to this session. A read-only Monitor preview is **not** a
   writable editor session.
5. Install the verified candidate into `HydraulicData` and enable supported
   logical edit capture only after session readiness.

The GUI's normal entry is **Projects → HEAD card → Overview → Open
network** for the selected branch. The branch Inspector's **Personal
working copies…** dialog remains an advanced checkout/draft workflow.

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

## Proven and outstanding behavior

Source-level implementation includes event queue/replay, a fenced HTTP
writer, acknowledgement recovery, and actual editor binding for supported
captured edits. Focused unit and HTTP-fixture tests exist. A real external
HTTP DVMDB GUI restart test preserved a network created in the editor.

**Do not claim full persistence acceptance yet.** Outstanding evidence
includes all 18 entity editing paths, root metadata, multi-event interrupted
runs, wrong-repository failures, GUI+DVMDB process restarts, and equivalent
Standalone embedded operation. Local Commit (immutable revision, expected local HEAD and durable operation
status) is now exposed through the HEAD Inspector; non-blocking hydraulic
diagnostics, lossless invalid-model round trips and cross-platform recovery
acceptance are still separate milestones.

First import into an untouched empty unborn HEAD is implemented through a
fenced server-side operation. General replacement/merge remains unresolved;
see [network import](network-open-import-replacement.md).

See [hydraulic edit transactions](hydraulic-edit-transactions.md) and
[implementation roadmap](dvmdb-integration-roadmap.md).

### MODEL codec compatibility before editing

The GUI embeds a content fingerprint of its AOWIS-SERVER-MODEL checkout at
build time. DVMDB advertises its corresponding fingerprint through repository
identity and tracking responses. Opening a project catalogue checks the source
server; every private-draft operation checks the personal server again before
accessing or writing a draft. Checkout from a shared source checks its
fingerprint as well. Missing or mismatched fingerprints stop the operation
before queued MODEL transactions can be submitted.

If a mismatch is reported, rebuild both components from identical MODEL source
files (including any uncommitted changes), restart the GUI and server, and
retry. Do **not** delete the on-disk edit queue to address a mismatch. The
fingerprint proves source-content agreement for the tracked files, not database
integrity or the identity/authorization of the remote endpoint.
