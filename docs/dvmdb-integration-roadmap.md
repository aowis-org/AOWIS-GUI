# AOWIS-SERVER-GUI / DVMDB version-control implementation roadmap

**Canonical implementation and thread-handoff plan. This is a specification,
not a statement that all described functionality already exists.**

Use this alongside [Projects workspace](projects-workspace.md),
[Git-style history graph](project-graph-layout.md),
[working-repository configuration](working-repository-configuration.md),
and [DVMDB quickstart](dvmdb-quickstart.md). The matching backend plan is
`AOWIS-SERVER-DVMDB/docs/roadmap.md`; both use the **same VC-xx milestone
IDs** and **DG-xx decision gates**. New implementation threads should be able
to work from these documents and the latest uploaded repository snapshots
without reopening the entire design discussion.

## A. The fixed user-visible model

- **Database/source:** an embedded DVMDB or a configured HTTP DVMDB. Shared
  project sources are distinct from the **user's private working repo**.
- **Project:** one UUIDv7 model-history container, initially `main` with
  **no committed HEAD and no revision node** (an unborn branch).
- **Branch:** UUIDv7 name/reference to a latest **committed immutable**
  revision or explicit null. It advances on Commit in a personal DVMDB or
  Publish in the shared DVMDB. Branch creation does not itself create a commit.
- **Revision/commit:** immutable UUIDv7 identity, complete Merkle-protected
  model state, SHA-256 record hash, zero/one/two real parent revisions.
- **Working copy:** a **separate private editable** UUIDv7 model state, saved
  durably in the user's working DVMDB; never called a committed revision.
- **Save draft:** persists only the private working copy. **Commit locally:**
  creates an immutable commit and conditionally advances only the local
  branch HEAD. **Publish:** conditionally advances the separately identified
  shared branch after verified history transfer. **Fetch:** reads remote
  history/tracking without altering private drafts or silently moving local
  HEADs. **Reconcile:** explicit user action on stale histories.
- **Graph:** one Git-style committed revision DAG. Revision-parent edges
  only. Branch names/HEAD status decorate the referenced immutable commits;
  layout lanes are not ownership. Unborn branches and empty projects are
  visible; private drafts do not appear as committed revision cards.
- **Branch provenance:** source branch UUID and precise immutable fork UUID
  are displayed in the inspector, not an additional graph ancestry edge.
- **Security boundary:** authentication, authorization, permissions, credentials,
  auditing and multi-user security design are **not part of this version-control
  implementation roadmap**. Record ideas only in
  [deferred security considerations](deferred-security-considerations.md).
  An HTTP repository URL or UUID identifies a target, not an authenticated user.

No code patch may substitute the earlier fabricated empty revision, a
writable shared HEAD, two ancestry parents for a branch reference, or a
branch card as a fake committed revision. UUIDv7 is identity; SHA-256 is
integrity. Names can include spaces and supported UTF-8.

## B. Approved implementation decisions — DG-01 through DG-06

**These decisions are closed and coordinated with DVMDB's roadmap.** Future
threads should implement and verify them, not choose alternative semantics.
Exact persistent/wire encodings and operational tuning belong in their
milestones. Do not invent unsupported behavior while the backend is still
being implemented.

| Gate | Before GUI milestone | Required GUI behavior |
|---|---|---|
| **DG-01: repository-qualified branch refs** | VC-03, VC-06 | The same logical project/branch retains its UUIDv7 across repositories; each repository has an independent HEAD under `(repositoryId, projectId, branchId)`. Display local HEAD and shared/tracking HEAD separately. New logical branches get new UUIDv7s, preserved on first remote provisioning. Match by UUID plus repository, never by label alone. |
| **DG-02: draft/session concurrency** | VC-04 | Create a distinct UUIDv7 working copy per session by default. On deliberate reopen, acquire exclusive expiring writer lease, fencing token and persisted generation; each Save supplies all three. Show read-only/lease-expired/takeover states, and never discard a saved draft on crash. |
| **DG-03: uncertain Commit/Publish** | VC-05, VC-07 | Generate and persist a UUIDv7 operation ID and payload digest for each action; on timeout/disconnect, query its authoritative pending/succeeded/rejected status and retry only with identical ID/payload. Never silently issue a second Commit/Publish. |
| **DG-04: pre-release format policy** | All stages | There is **no backward-compatibility or migration requirement** before AOWIS's first release. New formats may replace obsolete experimental formats cleanly; reject unsupported records explicitly instead of supporting transitional readers or retaining legacy layers. Do not silently corrupt data, rewrite a committed history or disguise a failing test fixture. Fresh test data may be initialized explicitly. |
| **DG-05: initial reconciliation** | VC-08 | Offer explicit Fetch, fast-forward and three-way merge of committed history. Provide manual conflict choices, persisted unresolved merge draft and whole hydraulic-model validation. No automatic rebase, force Push, silent conflict resolution or automatic Publish in initial UI. |
| **DG-06: first remote branch** | VC-06, VC-07 | Require explicit shared-repository/project target. Provision locally created branch remotely under the **same UUIDv7** with idempotency and collision checks. Treat missing branch separately from existing branch with null HEAD; first Publish from null uses an explicit expected-null precondition. |

**Operational parameters:** lease duration, draft quotas/recovery windows,
operation-status retention, conflict-report payloads and exact endpoint/config
names must be documented and tested in the appropriate milestone. The agreed
lifecycle is not open for renegotiation. Security design has **no VC milestone
and is not an acceptance dependency** for the database/graph implementation.
Any operational exposure risks are described separately in the
[security backlog](deferred-security-considerations.md).

**Projects personal database designation:** In the Projects Server Inspector,
selecting an HTTP database as personal discovers its UUIDv7 and checks its
repository role. If still `unconfigured`, the GUI explicitly configures it as
`private` using `POST /repository/role` with the discovered identity, then
stores the personal designation and crown in its server profile. Existing
`shared` repositories are never silently changed, and a remote whose identity
changes is never accepted. No UUID needs manual entry. In Standalone, a
personal local DVMDB profile now configures and pins the embedded repository
using its process-owned DVMDB service; it needs no external server process.

## Current implementation state and immediate delivery priority

This section records the **source-level checkpoint**, not a claim that the full
user workflow already works. Documentation and unit tests never substitute for
an end-to-end checkout/edit/reopen/commit test against a real personal DVMDB.

| Area | Available in the checked source | Not yet accepted end to end |
|---|---|---|
| Project and branch history (VC-01/02) | Unborn project creation, nullable committed HEAD, immutable branch fork provenance, native/REST readers, corresponding GUI graph/inspector tests | Real multiclient history/graph acceptance across all deployment modes |
| Personal repository identity and checkout (VC-03/04) | Persistent repository UUID/roles, tracking metadata, private working-copy CRUD and fenced generation-checked saves; committed REST bundle transfer and identity-preserving unborn root checkout to a private clone target; Standalone process-owned local DVMDB adapter | Standalone end-to-end acceptance and remaining deployment modes |
| Hydraulic editing (VC-04) | MODEL-owned canonical codec and reversible transactions; HC-01 generic capture now covers all 18 entity collections with one shared capture/adapter registry and per-collection replay regressions; durable local queue, typed record/membership adapter, lost-acknowledgement recovery and private-draft network initialization | HC-02 audit of actual GUI mutation paths, HC-03/04 network-root metadata, full HTTP/Standalone restart acceptance and platform validation |
| Local Commit (VC-05) | Immutable revision and model persistence primitives | Working-copy-to-revision CAS, recoverable operation ID/status, usable Projects-tab Commit |
| Collaboration (VC-06–08) | History transfer, import, revision DAG and domain merge primitives | Unified user-facing Fetch/Publish/reconciliation acceptance using independent private repositories |
| Presentation and qualification (VC-09–11) | Graph and inspector regression coverage; extensive DVMDB durability/sync tests | Integrated GUI/Standalone/browser scenarios and complete release evidence |

**Project-to-editor entry point (HTTP personal DVMDB):** For a new database-backed
project on the selected personal server, **Projects → Project Inspector → Edit
network** directly opens the project's single branch, creates or reuses its
private draft, initializes the first empty hydraulic network if needed, and
loads it into Distribution Editor with the fenced autosave session. This is
working-copy persistence, **not** an implicit Commit or an advance of the
branch HEAD. For a multi-branch project, select the exact branch first. If
more than one draft exists, the GUI requires explicit selection via the
advanced Personal working copies action to avoid choosing the wrong edits.
Cross-server checkout still requires a verifiable HTTP source, including the
process-owned internal endpoint for local Standalone databases. The Standalone
adapter connects local profiles and draft editing to the same validated DVMDB
routes in-process; it does not launch the external DVMDB executable or persist
an ephemeral endpoint. Full Standalone GUI/restart acceptance is still required.

**GUI VC-04 network preview:** The personal working-copy dialog can now
initialize the first network through a fenced lease and server-owned draft
initializer. After a generation-matched canonical hydrate it offers **View in
Distribution Monitor**. MainWindow loads the verified `NetworkHydraulic` into
the shared model after explicit confirmation, with logical edit capture disabled.
This path is intentionally read-only and never activates draft capture. The
separate **Edit in Distribution Editor** path opens the fenced session and
enables captured transactions only after verified hydration. A preview is not
an editing session, and local Commit is not yet available.

**VC-04 local event staging (implemented and editor-bound for captured edits):**
`PersonalDraftEditStage` opens the exclusive durable `PersonalEditQueue` only
against a verified authoritative candidate. It replays pending MODEL events in
order to reconstruct the local visible model, prepares the oldest event against
the **last confirmed server candidate**, and accepts acknowledgements only for
the exact oldest transaction. A matching authoritative journal-tail event can
be retired on restart after a lost acknowledgement, without replaying it twice.
Invalid replay, uncertain disk persistence and contradictory acknowledgements
block further staging while retaining queue records for investigation. The
hydraulic transaction test covers multi-event replay, a creation followed by a
property update, restart after the first Save and out-of-order failure.

**VC-04 fenced HTTP writer controller (implemented and editor-bound):**
`PersonalDraftEditSession` now reopens the authoritative draft and hydrated
candidate, acquires a writer lease, checks its generation, recovers the local
queue and serially flushes MODEL events with atomic membership intents.
Lease renewal rotates the DVMDB fencing token **only between Save requests**;
unknown outcomes leave queued data intact and block subsequent operations.
HTTP-fixture regressions exercise lost-acknowledgement recovery and replay of
an event after a rejected Save and process-like session restart.

The Projects draft dialog now offers a separately confirmed **Edit in
Distribution Editor** action. `MainWindow` binds `HydraulicData` to the
hydrated/replayed session state, stages captured events before HTTP Save, and
checks the live canonical model against the staged network after edits and on a
periodic guard. Capture stops on a failed staging, Save, or lease check; the
queue is preserved. This guarded slice still requires runtime acceptance with
an actual DVMDB, including restart, structural changes and network outages.
Do not reconstruct an old event from the current editor network.

**Read-only draft hydration:** The DVMDB `GET /working-copies/:draft/networks/:network`
endpoint reconstitutes the saved candidate over the pinned immutable base,
validating manifest chunks, canonical entity payloads and typed dependencies.
`PersonalWorkingCopyClient::loadDraftNetwork()` decodes and verifies the exact
MODEL payload and generation. The draft inspector previews an unambiguous
candidate-root network. No editor Save is implied: connecting the snapshot to
the separate fenced editing action is required for durable transaction capture.

**Next actionable work (in order):** (1) done 2026-10-30: the GUI
event-save client sends `membership`, checks it locally and recovers a
dropped response; DVMDB rejects creation/removal without it. Also done
2026-10-30 (DG-01): every GUI write and checkout addresses branches by UUIDv7
(`fromBranchId`, draft `branchId`, `GET …/branch-ids/{id}`, bundle header
project/branch/tip check), and project, branch and draft creation carry a
client-chosen UUIDv7 that DVMDB answers idempotently, because Qt before 6.10
(Debian 13: 6.8) resends a POST whose connection dropped;
Also implemented: identity-preserving checkout of an unborn root branch
from shared REST into an empty personal clone target. This uses a null HEAD,
keeps the source branch UUIDv7, and never fabricates a revision. It is
independent of first-network creation and still requires the user's test run.
(2) invoke the new private-draft `/initialize-network` endpoint through
`PersonalWorkingCopyClient::initializeEmptyNetwork()` when opening an empty
unborn editor. This operation creates one server-owned empty network root
without a commit; the caller must retain the draft on any ambiguous outcome
and reopen to confirm its generation. The candidate hydration read is now implemented and has dedicated regression
tests; the Projects dialog now offers an explicit **Edit in Distribution Editor**
path that opens `PersonalDraftEditSession`, hydrates its replayed visible model,
enables MODEL capture and drains the durable queue through the fenced DVMDB
writer. A periodic canonical-model guard blocks the session if a mutation
escapes transaction capture. The editing session can be explicitly closed
without clearing queued events. This is a limited VC-04 vertical slice: it
requires real GUI acceptance over interrupted sessions, property mutations
and network structure edits, and it does not enable local Commit. (3) verify capture/queue/flush of supported edits,
including creation/deletion, over interrupted network and restart; (4) add
local working-copy Commit with expected local HEAD and idempotent status;
(5) integrate the Commit action and prove first unborn commit end to end.
Broader Fetch/Publish/Merge polish follows this usable local workflow. Do not
mark VC-04 complete until a real editor can reopen an automatically saved
model byte-for-byte after process restart.

**VC-04 structural restart regression (new, pending execution):** The
`aowis-hydraulic-edit-transaction` test now stages five real `HydraulicData`
operations: junction creation ×2, pipe creation, junction rename, and cascading
junction/pipe deletion. It checks membership and endpoint references; performs
a process-like close/reopen after two confirmed Saves; simulates a lost pipe
acknowledgement using the authoritative journal tail; drains remaining events;
and verifies canonical `NetworkHydraulic` byte equality and an empty queue
after a second reopen. This is a deterministic GUI-side staging regression,
not a substitute for real HTTP/GUI process-restart acceptance. Do not mark
VC-04 accepted until the latter is observed.

**Verification note:** Previously reported targeted GUI tests passed for
`aowis-personal-working-copy-client`, `aowis-personal-edit-queue` and
`aowis-hydraulic-edit-transaction`. DVMDB hydraulic membership HTTP code and
fixture are present in the supplied snapshot, but this document does **not**
claim its revised integration test passed unless that run is separately
recorded. There is no claimed end-to-end local Commit acceptance yet.

## C. Milestone order and concrete GUI work

### VC-00 — Current baseline, API inventory and acceptance matrix

**Dependency:** none. **Backend:** its VC-00 inventory. **GUI work:**

1. Inventory `src/projects/project_source_reader.*`,
   `project_source_writer.*`, `src/tabs/tab_projects_widget.*`,
   `src/tabs/project_revision_graph.*`, `src/graph/layered_dag_layout.h`,
   project-profile/config code, network import and the existing test suite.
2. Map every GUI assumption that all branch HEADs are non-null, a project
   always owns R0, a branch ID can be formed from its name, branch cards are
   revision nodes, or every source is also a writable working repository.
3. Agree on one native/REST DVMDB API revision and submodule baseline;
   audit Qt desktop, Standalone and browser/web build capabilities and
   network transport boundaries separately.
4. Capture reproducible current UI, graph and data-flow baselines. Create
   initially failing acceptance tests from section E below rather than
   assuming an earlier patch has already been applied.

**Exit:** source file/API matrix and DVMDB/GUI build prerequisites listed;
the no-compatibility pre-release format policy is applied; no UI operation is built
on unverified backend shapes.

The [current implementation status](#current-implementation-state-and-immediate-delivery-priority)
supersedes the initial source audit. Nullable committed HEAD readers and
the graph have targeted tests; the remaining gate is integrated editor Save,
reopen and local Commit, not a repeat of the initial nullable-HEAD work.

### VC-01 — Unborn projects and null HEAD display

**Needs:** backend VC-01. **GUI work:**

1. Update native and REST project-source readers to preserve a real UUIDv7
   branch with `headRevisionId: null`, not a fake all-zero or empty-revision
   UUID; reject malformed graph pages, changing pinned HEADs and missing
   parents. Graph pages for an empty project are valid and complete.
2. Show selected database and project nodes even with zero revisions. Show
   unborn `main` with **No commits yet** and a disabled branch-from-revision
   action until the first real commit exists.
3. Preserve an empty graph after refresh, reconnect and source switching.
   Navigation, selection and inspector updates never auto-create revisions.
4. Test native/REST parity, creation via inspector, zero-history graph,
   multiple databases and offline/error states.

**Exit:** first project has zero immutable revision cards in all modes;
round-trip reload is accurate and the creation UI does not invent R0.

### VC-02 — Creating branches from exact immutable revisions

**Needs:** backend VC-02. **GUI work:**

1. Use full project/branch/revision UUIDs as action targets and selection
   keys. Source names are labels, not identifiers. Prefer selecting an exact
   committed revision (possibly reached from another branch's history).
2. In the compact tabbed inspector, create a branch with validated name,
   selected source branch UUID (when relevant), and exact committed fork
   revision UUID. Show source identity/hash before confirmation.
3. Do not create a branch from an unborn HEAD. When a draft contains edits,
   make the choice explicit: Commit locally first, or fork from the latest
   committed base **without** draft changes; never silently commit drafts.
4. Refresh from the authoritative backend and select the resulting new
   branch UUID. Its creation does not add a revision vertex or DAG edge.

**Exit:** correct provenance after rename/restart, no extra commit on branch
creation, UTF-8 labels with spaces, native/REST parity.

### VC-03 — Projects server designation and tracking identity

**Needs:** backend VC-03, DG-01 and DG-06. **GUI contract:**

1. The Projects **Server Inspector** is the only configuration surface for
   choosing a personal working database. An existing server can be marked
   **Use as my personal working database**, and exactly one profile may be
   marked at a time. The Servers navigator shows a crown (♛) for it.
2. No manual UUID entry or separate Personal Repository settings page exists.
   For REST servers, designation obtains `repositoryId` from
   `GET /dvmdb/v1/repository/tracking`, requires UUIDv7 and `role=private`,
   then pins that identity within the selected server profile. All draft
   requests recheck UUID and role and fail closed on mismatch.
3. For local Standalone profiles, designation records the selected embedded
   store path; it does not create an additional hidden personal directory.
   It configures an unconfigured repository to private and pins its UUIDv7
   internally. An application-owned ProjectService holds the catalogue lock;
   its bounded local-only loopback routes reuse the HTTP GUI draft writer.
   Autosave activates only when **Edit network** opens a verified draft,
   never merely because a personal database is selected.
4. Branch tracking and local/remote commit identities remain distinct.
   A missing, disconnected or mismatched personal server must never silently
   fall back to whichever source is being viewed.
5. Keep credentials and accounts out of this phase. Verify designation,
   persistence, reconnect handling and single-personal-server invariants.

**Exit:** a single obvious personal-server choice in Projects, with pinned
backend identity and no user-entered UUIDs. Editing only becomes available
when the selected backend actually supports its draft lifecycle.

### VC-04 — Private checkout, draft editing and crash recovery

**Needs:** backend VC-04, DG-02. **GUI work:**

1. Checkout a pinned revision UUID or explicit unborn `main` into a
   dedicated UUIDv7 working copy **inside the personal DVMDB**.
2. Wire EPANET/network/model editing to this draft through the private
   native/REST APIs. The committed graph remains read-only.
3. Show draft base, local branch, dirty state, save status, validation errors
   and personal repository location. Autosave needs debouncing, version
   checking and reliable last-save acknowledgement; it never commits.
4. Support opening/recovering existing drafts after restart or HTTP
   reconnect, continuing one draft deliberately, and discarding only after
   explicit confirmation. Preserve edits across graph navigation and tab
   switches.
5. Test two simultaneous GUI sessions, lost connectivity, interrupted
   autosave, stale draft-generation rejection and reopening by UUID.

**Exit:** persistent editing works without moving any committed HEAD; draft
recovery is verified in Standalone and HTTP modes.

### VC-05 — Local Commit, validation and safe retry UX

**Needs:** backend VC-05 and DG-03. **GUI work:**

1. Expose **Commit locally** separately from **Save draft** and **Publish**.
   Display the candidate snapshot, project, exact target *local* branch,
   expected local HEAD UUID/null and validation results.
2. Send persisted draft UUID and a stable idempotency/operation UUID. Track
   progress and authoritative operation status through cancel/timeout/
   restart; don't interpret HTTP disconnect as rejected Commit.
3. On successful Commit show immutable revision UUID/SHA-256, advance
   local HEAD only and offer continuing work in a newly based working copy.
4. On stale local HEAD, display the new local HEAD and preserve the draft;
   offer explicit reconciliation rather than automatically overwriting.
5. Test first zero-parent local commit, two drafts racing to commit, 409
   conflict, duplicate submission and post-response crash.

**Exit:** local success never claims the shared branch advanced and a
failed/unknown result never loses drafts.

### VC-06 — Fetch/clone and remote-tracking presentation

**Needs:** backend VC-06 and DG-01/DG-06. **GUI work:**

1. Add **Fetch** and project checkout/clone into the personal DVMDB,
   presenting the exact shared repository, project and branch identities.
2. Show separate *local HEAD*, *fetched remote-tracking HEAD* and *current
   shared HEAD* when available. Explain when a tracking value is stale.
3. Fetch must be non-destructive to local branches and active private
   drafts. Show pending object verification, progress, and resumable/error
   outcomes; validate pinned DAG generations before graph publication.
4. Support unborn shared projects, Unicode branch names, multiple shared
   servers, shared/non-leaf tips and two-parent histories.

**Exit:** two separate users can fetch the same shared revision with
independent local refs, unchanged drafts and accurate tracking indicators.

### VC-07 — Publish/Push and stale-remote recovery

**Needs:** backend VC-07, DG-03 and DG-06. **GUI work:**

1. Show **Publish** only when a valid local commit/branch, explicit shared
   destination and expected shared HEAD UUID/null have been resolved.
2. Include idempotency/operation ID, destination project/branch UUID,
   verified immutable history and exact expected shared HEAD; no guesses
   from labels, no auto force push and no direct writing into the editor.
3. Present progress separately for transferring objects and atomic remote
   ref publication. On timeout query authoritative status before retrying.
4. On 409 stale/non-fast-forward, retain local commits and private drafts,
   show the current remote HEAD, and offer Fetch / Compare / Reconcile;
   don't falsely label a successful local Commit as failed.
5. Test two editors pushing from R7, first publication from an unborn
   branch with expected null, duplicate operations and interrupted network.

**Exit:** one safe remote Publish, no lost commits on failed Publish, and
comprehensible local-vs-remote status across reconnect.

### VC-08 — Reconciliation, hydraulic validation and merge UX

**Needs:** backend VC-08 and DG-05. **GUI work:**

1. Present base, local committed tip, shared fetched tip and candidate
   three-way differences. Distinguish stale HEAD from actual entity/model
   conflicts; do not assume non-overlapping edits are hydraulically valid.
2. Support explicit user-approved fast-forward/merge and manual conflict
   resolution, with whole-model validation and no lost uncommitted draft.
3. A merge creates one genuine two-parent immutable revision **locally**;
   Publish still requires a separate conditional remote update. Avoid
   automatic rebase/replay in the initial scope.
4. Test safe cancellation, persisted unresolved conflict draft, retry after
   another remote advance, correct parent ordering and domain validation.

**Exit:** Bob can reconcile after Alice's Publish and publish a validated
successor, without overwriting either user's earlier committed history.

### VC-09 — Final Git-style revision graph and inspectors

**Needs:** backend VC-01/VC-02/VC-09 pinned graph contract. **GUI work:**

1. Render **one node per committed revision UUID**, with only actual ordered
   revision-parent edges. Do not draw branch-origin blue/dashed ancestry
   edges or branch-reference cards as descendants of a revision.
2. Decorate tips with **branch names and HEAD markers**, not fake revision
   vertices. If many refs share one revision, use a compact, selectable
   overflow list/inspector linked to the same revision—not a crowded card.
3. Preserve source/project organization and user filtering, but ensure
   containment links are visually distinct from ancestry. Never refit the
   graph automatically on selection or inspector tab changes.
4. Show zero-revision projects, single-commit shared HEADs, non-leaf HEADs,
   genuine divergence and two-parent merge. No UUIDv7 chronological
   heuristics for commit ancestry.
5. Keep the inspector at `Sizes::SidebarRightWidth`, with separate related
   tabs for branch provenance, commit integrity and working-copy status.
6. Test actual Qt item geometry/click targets and no overlays/orphaned
   widgets, not just Qt-independent DAG coordinates.

**Exit:** native and REST graph snapshots render the same genuine commit
DAG and all pointers remain correct after full refresh and pagination.

### VC-10 — Integrated editor lifecycle across GUI variants

**Needs:** VC-04 through VC-09 in DVMDB and GUI. **GUI work:**

1. Join network discovery/import, checkout, model editor, draft autosave,
   Commit, Fetch, Publish and conflict recovery into one persistent workflow.
2. Confirm the correct private working DVMDB in Standalone; for basic GUI
   and browser/web, require a per-user HTTP endpoint and explicit connection
   state. Shared project servers stay independently browsable.
3. Exercise cancellation, navigation, app shutdown, reconnect and draft
   recovery during each operation. Preserve selection and camera without
   sacrificing data safety.
4. Verify native/REST parity through real service integration, not only
   synthetic REST fixtures. Unsupported action paths must be disabled with
   explanatory feedback, not silently substituted with preview behavior.

**Exit:** end-to-end tests cover private editing and local committing
alongside shared publication and conflict recovery for all supported modes.

### VC-11 — Data integrity and first-release qualification

**Needs:** backend VC-11. **GUI work:**

1. Reject unsupported experimental repository formats with a clear error;
   do not implement legacy readers or migration scaffolding. Never silently
   change UUIDs or manufacture fork points.
2. Test all source roles, graph import, bundle transfer, draft restore,
   empty unborn repositories, database restart, slow/failed HTTP, Unicode
   names and corrupt histories in the actual target environments.
3. Run `./compile_linux_tests.sh` and scoped project tests (including
   `aowis-project-source-reader`, `aowis-project-source-writer`,
   `aowis-projects-inspector-dock`, `aowis-projects-multi-source`, and
   graph-layout tests). Test Standalone embedded build and browser build
   separately; record tests missing/unbuilt, not just ctest totals.
4. Capture benchmark/resource and manual UI evidence, including actual
   inspector/graph rendering, narrow-dock sizing and branch-label overflow.

**Exit:** all scenarios in section E pass with evidence; no unsupported
backend contract is hidden behind GUI fallback code.

**Security is deliberately outside VC-00 through VC-11.** No authentication,
permission or deployment-hardening milestone follows VC-11 in this roadmap.
Possible future AOWIS-wide work is documented solely in
[deferred security considerations](deferred-security-considerations.md) and
DVMDB's `docs/security-permissions-design.md`; those documents are not
prerequisites for completing the database/graph integration.

## D. Shared milestone matrix and dependency rules

| Milestone | Backend dependency | GUI result |
|---|---|---|
| VC-00 | Inventory and baseline | Source, graph, config and Qt regression gap list |
| VC-01 | Nullable HEADs/unborn lifecycle | Unborn `main`, zero revisions, valid graph |
| VC-02 | Persistent fork metadata/branch API | Explicit branch-from-revision editor |
| VC-03 | Repository identity/tracking | Safe personal DVMDB config and connection |
| VC-04 | Durable draft CRUD and concurrency | Checkout, autosave and recovery |
| VC-05 | Local Commit CAS/status | Commit locally and stale-local recovery |
| VC-06 | Fetch/clone and tracking refs | Show local vs remote HEADs, Fetch action |
| VC-07 | Publish atomic shared ref CAS | Publish and stale-remote recovery |
| VC-08 | Merge/compare/domain validation | Explicit conflict and merge workflow |
| VC-09 | Pinned full revision DAG | True Git-style revision graph and inspector |
| VC-10 | Native/REST complete parity | Integrated user workflow in all GUI modes |
| VC-11 | Format/fault/replica qualification | Real Qt/Standalone/browser acceptance |

**Sequencing:** A backend API stage can be implemented before its matching
GUI stage; the GUI may prepare read-only UI in parallel. **Do not enable**
mutating controls until the native and REST contract actually exists.
If an endpoint is intentionally not available in a particular mode, show a
clear unsupported/read-only state, not a substitute request to a shared repo.

## E. Mandatory GUI acceptance stories

1. Create a project on a shared source: project and unborn `main` appear,
   revision count is zero, the graph has no synthetic R0.
2. Standalone checks out empty `main` into its persistent embedded DVMDB;
   remote GUI checks out into an explicitly configured private HTTP DVMDB.
3. Save a draft, switch tabs/servers, close and restart: saved draft UUID and
   edits are recovered, while the published branch HEAD remains unchanged.
4. Commit locally from unborn `main`: exactly one immutable zero-parent R0
   appears in personal history; shared `main` still has null HEAD.
5. First Publish conditionally updates shared `main` from null to R0. HTTP
   timeout/retry reports authoritative status without duplicating a commit.
6. Create `feature` from R0: source UUID and exact R0 are recorded, no extra
   revision node; independent R1/F1 commits create actual divergence.
7. Two users with different private DVMDBs commit locally from shared R7;
   only one can publish from expected R7; the other retains all local commits
   and saved drafts, fetches and resolves explicitly.
8. Real two-parent merge validates hydraulic meaning, creates one committed
   merge revision locally and updates only the selected shared target after
   successful Publish.
9. Shared/non-leaf branch refs, long UTF-8 names and several branch HEADs at
   one revision remain selectable and readable without a fake ancestry edge
   or a widened/obscured inspector.
10. Missing/misconfigured private HTTP working repo disables editing but
    preserves published history; no silent fallback into shared server.
11. Wrong repo/project/branch UUID, changed pagination snapshot, corrupted
    parent hash, wrong expected HEAD and repeated idempotency key each have
    correct non-destructive errors and status.
12. All variants expose data-consistent state after recovery, replication,
    compaction and backend upgrades; no old empty-root assumptions leak in.
13. Cloned logical branch UUID remains identical in personal and shared
    repositories while their HEADs differ; selecting a shared branch never
    silently targets the private HEAD of another repository.
14. Reopening one draft in multiple sessions exposes read-only and writer
    lease/fencing states; stale generation Save is rejected after takeover.
15. Lost Commit/Publish response is recovered by original operation UUID,
    with pending/succeeded/rejected display and no duplicate write.
16. A newly created project has an unborn `main`; its first committed
    revision is created only by an explicit local Commit.
17. New local branch is provisioned explicitly at the shared repository
    with its own stable branch UUID; absence is distinct from null HEAD.
18. Initial conflict workflow is explicit validated three-way merge, not
    automatic rebase, force-push or silent resolution.

## F. Thread handoff and status discipline

**For the next thread:** supply the **latest full DVMDB and GUI source ZIPs**,
refer to `AOWIS-SERVER-DVMDB/docs/roadmap.md`, this document and the relevant
VC-xx stage. Do not assume an earlier patch was applied merely because it
was previously delivered. Use one named milestone or independently testable
substep per patch.

1. Review the exact source and relevant API/test fixtures in both archives.
2. Follow the **approved** DG-01–DG-06 decisions. Specify any necessary
   wire/storage representations and operational parameters consistently in
   both docs before implementation; record acceptance evidence, not a new
   competing architectural default.
3. Implement backend storage and native/REST semantics first; then GUI
   reader/writer, editor/inspector and graph changes with real integration
   tests. Do not mix unrelated UI redesign into a backend lifecycle patch.
4. Run Qt-independent and Qt tests where available; record what cannot run,
   including CI/Standalone/web platform gaps. A ZIP/application success is
   not a GUI functionality test.
5. Produce ZIP patches of **changed/added files only** under the correct
   repository root hierarchy, with fresh timestamps; include a one-line
   `./tools/apply_patch.sh ~/Downloads/<patch>.zip` command and a one-line
   commit message for each affected repository.
6. Mark each milestone `Not started`, `In progress`, `Blocked` or `Verified`
   **only with matching test evidence**. Avoid claiming features work from
   documentation alone.

**Next task:** Complete the current VC-04 private draft editing path as
specified in the status section above. Then implement VC-05 local Commit,
without restarting VC-00 or adding unrelated features.
