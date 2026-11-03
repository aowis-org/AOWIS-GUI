# DVMDB integration: implementation contract and roadmap

> **Current implementation checkpoint — 2026-11-02.** The older HC/VC
> milestone rows below are a delivery/history plan, **not a live feature
> inventory**. Since they were written, GUI HEAD/Revision Inspector routing,
> multiple unborn branches, project/branch metadata updates, Local Commit,
> saved-draft network summaries, selected revision/HEAD export and first
> import to an untouched unborn private draft have been added. Claims below
> that these are “not integrated”, “not present”, or “not started” are
> superseded by the [current workspace guide](projects-workspace.md) and
> [quickstart](dvmdb-quickstart.md). Outstanding acceptance is still broad:
> root metadata capture, import replacement/merge, **non-blocking** hydraulic
> diagnostics and lossless invalid-model handling, cross-platform restart/recovery,
> Fetch and Publish. Do not infer test success
> from feature implementation alone.


This is the authoritative **GUI-side handoff** for the Projects workspace,
private hydraulic editing, and local version control. Read it with
[Projects workspace](projects-workspace.md),
[hydraulic editing](hydraulic-edit-transactions.md),
[Save/recovery](personal-working-copy-save.md), and
[network loading/import decisions](network-open-import-replacement.md).
The DVMDB repository owns the corresponding storage and HTTP contracts; the
MODEL repository owns canonical model serialization and edit transactions.

**Baseline snapshot: 2026-11-01 (historical, superseded by the 2026-11-02 checkpoint above).** “Implemented” means
present in this source, **not** that all deployment modes have passed acceptance.
Tests reported in previous implementation threads are identified separately.
The rejected HC-02.9 import-blocking proposal was **not applied** and is **not**
part of this contract. Do not infer product decisions from an unmerged patch.

## 1. Fixed architecture and terminology

| Object | Meaning |
|---|---|
| Database/repository | Persistent DVMDB identity and role (`private`, `shared`, or `unconfigured`); an HTTP or Standalone embedded source |
| Project | UUIDv7; contains branches and network history |
| Branch | Stable UUIDv7 plus user label; repository-specific nullable **committed** HEAD |
| Revision (Commit) | Immutable UUIDv7 model snapshot with real ancestry; SHA-256/Merkle data verify content, not identity |
| Personal DVMDB | Exactly one selected profile marked **Use as my personal working database**; owns private drafts and local commits |
| Working copy / draft | Private, editable UUIDv7 state based on a specific revision (or an unborn branch); saved independently of committed HEAD |
| Save | Persist edits to that working copy; **never** create an immutable revision |
| Commit locally | Implemented HEAD Inspector operation creating an immutable revision and conditionally advancing the **local** HEAD; full acceptance pending |
| Publish | Future explicit transfer/update of **shared** HEAD, independently checked against its expected value |
| Load network | Reconstruct/verify DVMDB candidate and install it in GUI memory; no edit event by itself |

New projects have an **unborn** `main` (HEAD `null`): no artificial first
revision. Branch creation from a committed revision records the real fork point
without creating a commit or a false ancestry edge. Branch labels are not
unique identities; repository ID + project ID + branch ID qualify operations.
A working copy is not a revision and must not appear as one in the DAG.

All AOWIS persistent identities are UUIDs, with UUIDv7 in the current
project/branch/revision/draft lifecycle. Never substitute a SHA-256 digest as
an object identity. No pre-first-release database migration or compatibility
layer is required. Authentication/authorization remain a **separate deferred
security effort**, not a hidden prerequisite for these VC milestones.

## 2. What the GUI actually implements

| Area | Source-level capability | Still missing / not accepted |
|---|---|---|
| Projects and history | Multi-source DVMDB reading; repository identity, project/branch/commit display; unborn branches; inspector and graph | Complete multi-repository/large-history acceptance |
| Personal database | Select in Server Inspector; profile verification/role handling; ♛ in navigator and golden, crowned graph card | Private repository is a functional designation, not an authentication mechanism |
| Remote → personal | Advanced **Personal working copies…** dialog on a database-backed **branch**; committed bundle checkout and unborn branch checkout paths | One-click **whole-project clone** with all branches; fully verified Standalone cross-server workflow |
| Personal draft | Open/create/reopen, network initialization, verified load, fenced writer, durable queued event Save | All persistent `NetworkHydraulic` fields and operational failure acceptance |
| Distribution Editor | **HEAD card → Overview → Open network**, or advanced draft editing dialog; captured edits queue and Save | Uncaptured mutation paths, network-root metadata, general import replacement/merge |
| Entity capture | All **18** persisted collections supported by the generic MODEL transaction encoder and draft event adapter | 18/18 is **not** proof that every widget/mutation path autosaves |
| Editing surfaces | Transaction-safe libraries (8 collections), rule controls and map labels, with navigation pages and widget tests | Final mutation-path audit and runtime persisted equality |
| Standalone | Native DVMDB engine and cpp-httplib routes hosted **inside the GUI process** over ephemeral `127.0.0.1` HTTP | Full Standalone edit/restart acceptance; listener isolation is not authentication |
| Local Commit | Native + HTTP Commit/status, GUI HEAD Commit button, expected local HEAD and idempotent operation ID | Lossless invalid-model support and cross-platform failure/restart acceptance |
| Fetch, Publish, merge | Backend transport/graph and merge primitives exist | Complete GUI workflows, conflict handling and acceptance |

**Verified by reports in this conversation:** focused HC-01 transaction test;
HC-02.2 rule/label cascade; HC-02.3 pipe rejoin; HC-02.5 library CRUD;
HC-02.8 controls/labels widget (after the CMake link fix). Each report was
**1/1** for its selected test, not a full-suite or end-to-end qualification.
HC-02.4 and HC-02.7 implementation is in the uploaded source, but focused
passing results were not supplied here. HC-02.6's widget test initially
failed; a test-dialog fix was delivered, but no subsequent passing result was
reported here. Do not upgrade those to “verified” without evidence.

A real external-HTTP DVMDB **create → edit → GUI restart → reopen** workflow
has been reported working for a tested hydraulic network. This is valuable
acceptance evidence for those operations, not a claim that all 18 entity kinds,
all root fields, and all failure modes have been exercised. The corresponding
Standalone end-to-end scenario is not yet confirmed.

## 2A. Ordered cross-repository implementation queue

These **INT-00 through INT-12** IDs match the authoritative execution queue
in `AOWIS-SERVER-DVMDB/docs/roadmap.md` (section C1). Take one ID per
implementation thread, attach fresh GUI + DVMDB snapshots, and update both
status tables on completion. The table distinguishes code already present
from missing integration/acceptance. **Commit/Publish/Merge never require a
hydraulically valid model.** Validation reports are non-mutating diagnostics;
lossless storage and repository invariants remain strict.

| ID | Priority | GUI deliverable and current starting point | Acceptance / handoff |
|---|---|---|---|
| **INT-00** | P0 | Align GUI, MODEL and DVMDB semantics: hydraulically invalid but representable work is savable, committable and mergeable; audit existing strict validation | Invalid model can be reopened unchanged; invalid record bytes still fail |
| **INT-01** | P0 | Pin `ProjectSourceWriter` metadata mutations to expected repository UUID/fingerprint (private draft client already checks identity) | Wrong/replaced endpoint and uncertain PATCH cannot silently edit wrong repo |
| **INT-02** | P0 | Persist Commit operation UUIDv7 + immutable body/target **before** POST; on restart reconcile `/local-commits/{id}` (or exact same-ID replay when provably not recorded) before another Commit | Lost POST/status responses, GUI exit and retry yield one revision |
| **INT-03** | P0 | Follow `GET /working-copies` `nextCursor` (currently only first page); filter by `projectId`, bound loops; scale catalogue/graph reads | 101/250+ drafts, 100+ projects and many branches not silently hidden |
| **INT-04** | P1 | GUI can open/display/edit/export a representable model with dangling hydraulic refs; diagnostics separate from version-control buttons | Broken hydraulic model round-trips without auto-repair or disabled Commit |
| **INT-05** | P1 | Finish 18/18 **mutation-path** audit, persistent root metadata (HC-03..05), queued autosave, undo/redo | Canonical full-model equality after GUI and DVMDB restart over HTTP and Standalone |
| **INT-06** | P1 | Preserve first import; design and implement explicit new, replace and selected-entity import paths, with dirty-queue protection | Stale/ambiguous/error imports never discard an existing network or queue |
| **INT-07** | P1 | Complete HEAD/branch/Revision inspector workflow, Commit status, unborn UX, selected network open/export and local history | One real create→edit→save→Commit→branch→reopen session; Inspector tests green |
| **INT-08** | P2 | Whole-project Clone, Fetch, pinned source and visible remote tracking; retain current branch-scoped checkout | All branches/UUIDs/2-parent history/unborn heads replicated; drafts untouched |
| **INT-09** | P2 | Explicit Publish action with expected remote HEAD, persisted operation ID, progress and authoritative result recovery | Concurrent remote CAS, timeouts and stale Publish preserve local work |
| **INT-10** | P2 | Compare, conflict preview, manual field/object choices, explicit fast-forward/merge and two-parent history | Can choose invalid but representable outcome; no silent draft overwrite |
| **INT-11** | P3 | Drive desktop HTTP, embedded Standalone and supported WASM flows with repeatable tests | Recorded exact round-trip canonical model, HEAD and UUID assertions |
| **INT-12** | P3 | Warn on journal (4,096 events) / model quotas; address large history UI, blocked queue recovery, UI hangs | No false autosave success; measurable soak and recovery results |

### Immediate P0 slice (INT-01 → INT-03)

1. `src/projects/project_source_writer.cpp`: bind every project/branch
   mutating request to the selected repository identity; DVMDB must check
   the expected identity atomically, not just trust a preliminary GET.
   Test remote replacement with matching project UUID, missing endpoint,
   wrong role and lost metadata response.
2. `src/projects/personal_working_copy_client.cpp`: `commitSavedDraft()`
   currently generates an operation ID locally, then loses it if both POST
   and status GET are inconclusive. Save a durable intent including exact
   target/body/digest before dispatch; resume by *same* ID on next startup.
   Show `Pending / Confirmed / Rejected / Unreachable` in the HEAD Inspector;
   block a competing new operation while unresolved.
3. The same client `list()` currently reads one draft page, although DVMDB
   exposes `nextCursor`, `after`, `projectId`. Traverse all pages and detect
   missing/inconsistent cursors. Extend large-project and history tests
   without papering over incomplete reads with empty results.

**INT-02 implementation checkpoint (2026-11-03):** The GUI now durably stores
one immutable Commit intent per repository/project/branch, including UUIDv7,
full POST body, expected draft generation and HEAD, target endpoint, and SHA-256
request checksum. QSaveFile plus directory sync protects the record before
any POST. The HEAD inspector offers **Reconcile Commit** and automatically
probes previously pending records when the HEAD is loaded. A verified missing
ledger result permits only an exact same-UUID/body replay. A confirmed
succeeded/rejected outcome is persisted before UI acknowledgement, while
unknown/pending status blocks new operations. Added a local record-corruption
regression plus HTTP fault scenarios for lost POST/status replies, restart,
exact replay and server-side rejection. **Compiled Qt and real-process crash
acceptance remain outstanding**; browser persistence also requires a
separate platform-specific durability check.

### Finish local persistence and usable editing (INT-04 → INT-07)

* **Validity policy:** keep canonical codec/record validation and DB graph
  invariants, but do not make hydraulic connectivity, missing model references
  or solver feasibility a Save/Commit/Publish/Merge precondition. Audit
  serializer/GUI load paths that currently refuse such a model. Offer an
  independent validation command and report errors/warnings by draft
  generation or immutable revision UUID. Never silently fix or omit an entity.
* **Mutation capture:** inventory every edit surface for all 18 collections
  and persistent `NetworkHydraulic` root fields (time, quality, report,
  options, network identity, MSX, map backdrop). Distinguish UI-only state
  from persisted state. Ensure direct setters and undo/redo issue canonical
  before/after transactions. Keep lease/fencing/generation checks.
* **Import:** keep narrow first-import into untouched unborn draft. Decide
  separately how `Replace working network` safeguards current queued events,
  draft snapshot and UUIDs; selected-entity import must resolve collisions
  explicitly. No silent implicit replacement via `replaceNetworkHydraulic()`.
* **UX:** HEAD card is editable workflow state, Revision is immutable,
  branch metadata and revision display annotation are different. A Commit
  moves only personal HEAD. Show local/remote HEAD, draft dirty/pending and
  operation identity. The previously failing `aowis-projects-inspector-dock`
  suite must pass against actual tab ownership; avoid weakening real checks.

### Collaboration and synchronization (INT-08 → INT-10)

* **Clone/Fetch:** full project graph, branches and unborn states. Pin shared
  repository UUID, snapshot and MODEL fingerprint; preserve local HEAD and
  working copies, and show tracking details. Never confuse absent branch
  with existing null HEAD or reconstruct relationships by branch names.
* **Publish:** transfer immutable objects first, advance remote HEAD only
  by server-side expected-HEAD CAS, handle first-branch provisioning using
  the existing UUID. Persist/reconcile operation IDs exactly as for Commit.
* **Merge:** fetch and preview against pinned base/local/remote revisions;
  show field/object conflict choices, allow deliberate invalid networks,
  commit two ordered parents locally. Manual conflicts block only until
  the user's explicit choice, not until a hydraulic solver succeeds.
* A GUI must never silently rebase, force push, discard a dirty draft, or
  make a shared database the fallback editing target.

### Acceptance and scale (INT-11 → INT-12)

Test a **real** create→edit→save→close→reopen→Commit→branch→Fetch→Publish→
conflict→Merge→Publish sequence; also a deliberately invalid model; run it
against external HTTP DVMDB and persistent embedded Standalone. Browser/WASM
features get separately marked supported, unsupported or not yet accepted.
Assert complete canonical `NetworkHydraulic` bytes (18 collections + root),
UUIDs, both merge parents, local and shared HEADs and absence of discarded
queued edits. Inject server/GUI process death, lost replies, stale lease,
wrong identity, disk full and crossing the journal cap. For every test,
record exact commands/platform, result and known gaps. First-release security
and migration compatibility are out of scope.

**Status discipline:** `Implemented` is not `accepted`. Do not mark an INT
item done until both repositories' gates and the user-visible workflow have
passing evidence; never claim a GUI Qt/Standalone test was run if only native
DVMDB tests were executed. Deliver repository-specific, changed-files-only
patch ZIPs with fresh timestamps, and one copy/paste apply command and Git
commit message per repository.

## 3. Normal user workflows (as currently exposed)

### Mark the personal database

Open **Projects**, select an actual database/server entry, then in its
**Server Inspector** select **Use as my personal working database**. Exactly
one configured source may be designated. The GUI verifies its identity and
private role (configuring an `unconfigured` repository as private as part of
the explicit selection); it must not silently convert a `shared` repository.
The navigator marks it ♛; its server **graph card** is gold with the normal
server icon at left and a crown beside the name.

Standalone uses a selected local DVMDB data-directory **profile** and an
embedded process-owned DVMDB service; it does not launch a DVMDB server
executable. The GUI talks to that embedded service using the same HTTP/JSON
routes on a dynamically bound loopback port. The port is runtime-only and is
not saved in the profile. The standalone local path must be persistent and
absolute. Ordinary desktop GUI uses a reachable personal HTTP profile.

### Create and edit a project

Open **Projects** → select the personal server → create a project. A new
project starts with one unborn `main`. From the selected **HEAD card → Overview**, use
**Open network**; with multiple branches, select the intended branch first.
The GUI creates/reuses a draft, initializes an empty network if appropriate,
loads the verified candidate into `HydraulicData`, binds the fenced editing
session and opens **Distribution Editor**. Captured edits are queued locally
then submitted to the personal DVMDB. Save does not Commit.

For an existing database-backed branch, the branch inspector's
**Personal working copies…** opens advanced checkout and draft controls. It
is **not** a project-level Clone control. It can transfer a remote committed
or unborn branch to the personal repository, then open a private draft. The
**View in Distribution Monitor** action loads a verified read-only preview;
**Edit in Distribution Editor** starts a separate writable session.

### Shared Open Network workflow (November 2026)

The main toolbar project/branch selectors and writable HEAD / Project Inspector
use one confirmation **before** any checkout or draft creation. Single-branch
projects propose that branch; multi-branch projects default to **[none]** and
wait for an explicit dropdown choice. The old intermediate working-copy dialog
is now an invisible asynchronous preparation controller, while the advanced
**Personal working copies…** inspector operation remains visible and separate.
The toolbar status reflects the editing session (opening, autosaved, pending,
read-only, failed). Personal database names always put **♛ before the name**.
Switching safely stops the previous fenced editor, retains its durable queue,
then verifies the new working network. A conflicting modified draft may require
an additional explicit safety decision; no silent rebase is permitted.

### Network import is not the same as load

The current selected-HEAD first-import path can persist converted EPANET/EPANET-JS data into an untouched empty unborn private draft. General import paths can still replace the in-memory model.
The general `HydraulicData::replaceNetworkHydraulic()` resets edit capture.
That function is also used when loading a verified DVMDB draft. **There is an approved narrow first-import path into an untouched empty
unborn private draft**. That does not approve replacing an already saved
network. The old HC-02.9 blanket import ban was rejected. Do not interpret
`replaceNetworkHydraulic()` as a persistent replacement API. The options and decision gates are tracked in
[network loading/import decisions](network-open-import-replacement.md).

## 4. Hydraulic coverage milestones (HC)

The HC milestones are the remaining **VC-04 editor-persistence** work, not
separate version-control branches. Keep the evidence and outstanding gaps
explicit.

| Step | Scope | State at checkpoint |
|---|---|---|
| HC-01 | Generic before/after images for all 18 entity collections; shared capture/adapter registry, canonical replay | **Implemented, focused test passed** |
| HC-02.1 | Pump simple-control UUID capture and cascades | Implemented |
| HC-02.2 | Rule-control deletion and anchored map-label detachment | **Implemented, focused test passed** |
| HC-02.3 | Split/rejoin dependent-control capture | **Implemented, focused test passed** |
| HC-02.4 | Pattern/curve/material reference validation in property setters | Implemented, focused result not reported |
| HC-02.5 | Transaction-safe CRUD across eight library collections | **Implemented, focused test passed** |
| HC-02.6 | Hydraulic Libraries GUI editor | Implemented; widget retest not confirmed |
| HC-02.7 | Transaction-safe rule/label CRUD | Implemented; focused result not reported |
| HC-02.8 | Controls & Map Labels GUI editor | **Implemented, focused test passed** |
| HC-02.9 | Finish mutation-path audit; define load/import/replace/merge semantics | **Design open; proposed blocking patch NOT applied** |
| HC-03 | MODEL canonical network-root metadata transaction representation | **Partly integrated in source; acceptance incomplete** |
| HC-04 | DVMDB server-owned atomic metadata updates in draft Save | **Partly integrated in source; acceptance incomplete** |
| HC-05 | GUI settings mutation capture and root-metadata autosave | **Partly integrated in source; acceptance incomplete** |
| HC-06 | Agreed bulk import/replacement behavior and persistence | **Not started; dependent on decisions** |
| HC-07 | Complete `NetworkHydraulic` equality/restart/crash acceptance in HTTP and Standalone | **Partly integrated in source; acceptance incomplete** |

### HC-02.9 — Decision before code

Inventory each remaining persistent `HydraulicData` mutation and each
external model replacement path. Identify whether a change is (a) a verified
read/load, (b) an entity edit, (c) a network-root settings edit, or (d) an
explicit user-requested import/replacement/merge. **Do not implement a
unilateral import ban or silent replacement.** Agree on target-selection,
conflict, atomicity, UUID/reference, queued-edit and recovery semantics first.
This is a design gate, not an excuse to mark HC-02 complete prematurely.

### HC-03 — MODEL network metadata

Define a canonical representation of **persistent fields of
`NetworkHydraulic` excluding its 18 child collections**: network identity
and labels, times, hydraulic/quality/reaction/energy/reporting options,
multi-species configuration, and map backdrop. Reuse MODEL's transaction
identity, canonical before/after validation and deterministic encoding.
No GUI-specific second serializer; no implicit update of entity membership.

### HC-04 — DVMDB atomic network-root Save

DVMDB owns network-root/manifest data. Its draft Save currently disallows
client-supplied root/manifest records. Add a **logical metadata edit** that
reads/verifies the authoritative root, enforces lease fencing and expected
generation, updates only metadata, and journals the result atomically with
any related entity changes. A stale before-image is a conflict, not a
last-writer-wins overwrite. Do not loosen the server-owned root boundary.

### HC-05 — GUI settings wiring

Route all user-editable persistent network settings through the canonical
transaction mechanism. For example, `setSimulationHeadlossFormula()`
currently modifies root metadata directly without producing a MODEL edit.
Temporary simulation results, presentation state and view preferences are
not automatically part of the persisted hydraulic network.

### HC-06 — Bulk/import integration

Implement the **agreed** new-project / replace-working-network / merge
operations, not three implicit variants of `replaceNetworkHydraulic()`.
For substantial imports, decide whether a dedicated atomic DVMDB operation
is needed rather than an unbounded queue of individual entity edits. Preserve
private draft state on errors, timeouts, and navigation. No accepted design
exists yet for this milestone; see the decision record.

### HC-07 — Acceptance gate

A representative network with all 18 entity collections **and** all root
settings must survive GUI edit → queue → DVMDB save → GUI restart → DVMDB
restart → reopen with byte-identical canonical `NetworkHydraulic` content.
Include create/update/delete, reference cascades, collection order,
null/unborn base, duplicate UUID rejection, interrupted saves, lost replies,
stale leases and wrong-repository safeguards. Run the same user-visible
scenario over external HTTP and embedded Standalone. Check that no Save
advances a committed branch HEAD.

## 5. Version-control milestones (VC)

| Milestone | Required outcome | Current integration state |
|---|---|---|
| VC-00 | Inventory APIs, source qualification and acceptance matrix | Baseline and extensive unit tests present |
| VC-01 | Unborn projects, nullable HEAD and exact identity | Implemented at source level |
| VC-02 | Branch creation from exact committed revision with fork provenance | Implemented at source level; integrated acceptance pending |
| VC-03 | Source and personal-repository identity/role handling | Implemented; deployment validation pending |
| **VC-04** | Private checkout, complete edit capture, durable Save, restart recovery | **In progress; HC-02.9 through HC-07 outstanding** |
| **VC-05** | Explicit local Commit: immutable revision, expected-local-HEAD CAS, idempotency/status | **Integrated in source; acceptance incomplete** |
| VC-06 | Complete Fetch/Clone and remote-tracking UI, including all branches | Branch-level checkout exists; full-project workflow pending |
| VC-07 | Verified Publish with expected shared HEAD, idempotency and recovery | GUI workflow pending |
| VC-08 | Explicit fast-forward / three-way merge with non-blocking hydraulic diagnostics | GUI workflow pending |
| VC-09 | Complete graph/inspector presentation for local and shared history | Partially implemented |
| VC-10 | Coherent editing lifecycle on desktop, Standalone and web | Incomplete acceptance |
| VC-11 | First-release integrity, recovery and workload qualification | Incomplete |

**Sequence:** first finish INT-00..03 correctness/recovery; complete VC-04/HC
coverage and VC-05 lossless-model/recovery acceptance (Commit API/button already
exist); then improve whole-project Clone,
Fetch/Publish/merge and cross-platform acceptance. Separate source-level
primitives from user-visible workflow completion in every status update.

## 6. Agreed version-control decision gates

These predate the HC import discussion and remain binding:

- **DG-01, repository-qualified branch refs:** one logical project/branch
  UUIDv7 may exist in multiple repositories with different local/shared
  committed HEADs. Never match by label alone.
- **DG-02, private draft concurrency:** default independent session drafts;
  deliberate reopening requires exclusive lease, fencing token and expected
  persisted generation. Preserve pending queue on failure.
- **DG-03, uncertain Commit/Publish:** persist UUIDv7 operation ID and digest;
  reconcile an ambiguous response against authoritative operation status
  before retrying with the *same* operation identity.
- **DG-04, pre-release policy:** no obsolete format/migration requirement.
  Reject incompatible data explicitly; do not silently corrupt it.
- **DG-05, initial reconciliation:** explicit Fetch, fast-forward and
  three-way merge; persisted manual conflicts, non-blocking hydraulic diagnostics.
  No automatic rebase or force Publish.
- **DG-06, first remote branch:** explicit remote target and same logical
  UUIDv7; distinguish missing branch from existing unborn branch; first
  publish checks expected-null HEAD.

**Separate open design gate — network import:** no approved decision yet for
new-project import, replacing an active working network, or merging imported
entities. These are not solved by DG-01 through DG-06.

## 7. Required evidence and handoff rules

A milestone may be marked **Verified** only with the actual build/test
command, result and relevant deployment. One focused CTest does not certify
all GUI operations; a successful GUI restart test does not certify
Standalone. Maintain a source snapshot and targeted test in each handoff.

Minimum acceptance stories:

1. New personal project → unborn main → Open network → two junctions and a
   pipe → autosave → GUI restart → exact recovered network; no Commit.
2. Library/pattern/curve/material edits, rule clauses and map-label edits,
   including failed reference changes and cascading removals, survive reload.
3. All root-level settings survive editing, both service restarts, and exact
   canonical replay (after HC-03/04/05).
4. Lost Save acknowledgement, lease expiry, stale generation, disk/network
   failure and wrong target cannot discard or duplicate an acknowledged edit.
5. Complete project/branch cloning retains identities and immutable ancestry;
   local Commit and later Publish move only their intended repository HEAD.
6. Standalone embedded and remote HTTP use the same accepted model content;
   no fabricated revisions and no silent fallback to another repository.
7. Import actions follow the **decided** policy, with recoverable failures and
   no untracked model replacement (after HC-02.9/06 decisions).

The GUI implementation must obey C++ repository style: no `auto` variables;
use `this->` for member variables, not member-function invocations. Keep
patch ZIPs scoped to changed files with their repository hierarchy and fresh
file timestamps. Tests must fail quickly rather than waiting on long hangs.
