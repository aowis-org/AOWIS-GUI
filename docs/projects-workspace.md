# Projects workspace — interaction contract

**Normative product design, not a declaration that every action is already
wired in the GUI.** Real DVMDB browsing and graph inspection are available;
the complete checked-out `HydraulicData` autosave and local Commit workflows
are pending integrated acceptance. See the
[current integration status](dvmdb-integration-roadmap.md#current-implementation-state-and-immediate-delivery-priority).

The Projects workspace follows the
[DVMDB version-control model](../../AOWIS-SERVER-DVMDB/docs/version-control-semantics.md)
and [graph presentation](project-graph-layout.md). A database connection,
project, branch, committed revision and private working copy are **different
objects** with different lifecycles and actions.

## Workspace responsibilities

| Region | Responsibility |
|---|---|
| Database navigator | List saved embedded/REST source profiles and their availability and visibility |
| Project / branch navigator | Navigate projects and branches within the selected visible databases |
| History canvas | Show one Git-style immutable commit DAG with branch HEAD labels |
| Right inspector | Show selected identity, context-specific information tabs and permitted actions |
| Model editor | Edit one private working copy in the user's configured personal DVMDB; never edit a committed revision |

The database selector remains visible even when a source has no projects.
Unchecking a database filters it from the canvas and project tree; it is not
an implicit disconnect/delete operation. Multiple visible databases remain
source-qualified so equal human-readable names never collide.

The inspector uses the shared `Sizes::SidebarRightWidth` sizing convention,
not a half-window layout. Optional tab pages are hidden when inactive, and
selection of another object changes the inspector without a modal duplicate.

## Working repository versus published database sources

Each user has one configured **personal working DVMDB** for durable drafts,
local commits and tracking of shared branches. Standalone uses its built-in
native DVMDB; basic desktop and web clients require an explicit private
HTTP DVMDB repository. A visible/selected shared project source is **not**
the working repository. The GUI must distinguish Save draft, Commit locally,
and Publish/Push to shared HEAD. See
[working-repository configuration](working-repository-configuration.md).

## Creation and opening

### Database/source profile

Creating a database chooses embedded/REST and connection information. For
reachable DVMDB endpoints, the GUI verifies `GET /dvmdb/v1/repository` before
saving, and uses its persistent repository UUIDv7 as the database identity in
the configuration, navigator and graph (not a separate generated profile ID).
If the endpoint is unreachable, the user may explicitly save it as **pending**.
This temporary entry has a local UUIDv7 plus `pending: true` and cannot be a
personal working database. The existing health monitor retries discovery;
a successful complete catalogue read atomically promotes the pending entry to
the actual repository UUIDv7 and rekeys selection, graph and source sessions.
A verified offline database keeps its UUID; a different repository at its
endpoint is rejected. If another configured entry already has the discovered
UUID, no duplicate database is created. The server name and endpoint remain
GUI settings, not database identity. Removing the entry does not delete data.
Standalone discovers embedded DVMDB identity from the native service.

The configuration schema is pre-release and intentionally does not migrate
obsolete truncated `s-...` profile IDs or `local-default` IDs. Re-add those
connections once using the repository-based identity model.

### Project

`Create project` allocates a project UUIDv7 and its default `main` branch
UUIDv7. The branch is **unborn**: no committed HEAD revision and no synthetic
R0. Show the project and branch immediately with `No commits yet`. To edit,
start a private working copy from the project's empty model state. A user
must explicitly commit to create the first immutable revision.

### Branch

`Create branch` requires an explicit **committed source revision**.
An unborn `main` must receive its first successful commit before any branch
can be created from it. If a source branch is selected, display its
committed HEAD and retain both `originBranchId` and exact `forkRevisionId`.
The form must not silently pick a remembered branch or a different root.

If the user has uncommitted edits, offer `Commit first` or `Branch from last
commit (without my edits)`; do not silently finalize, copy or discard private
changes. Branch creation from a committed revision does **not** create an
extra revision. Branch labels may include spaces and valid UTF-8, with
validation aligned to DVMDB rather than an ASCII-only GUI regex.

## Working-copy editor

A session checks out a **separate private working copy** from a fixed base
revision UUIDv7 (or `null` for the unborn initial branch). Each working copy
has its own UUIDv7, owner/session and target branch. Different users may
check out the same branch and base concurrently. Only their own working
copies are editable; the shared committed HEAD is always immutable.

The editor must display the target branch, its **current committed HEAD**,
its **pinned working-copy base**, dirty state and validation errors. It must
preserve uncommitted changes through safe navigation and service errors.
Opening a historical revision for inspection must not move a branch HEAD;
editing historical content requires an explicit checkout/fork workflow.

### Commit

`Commit locally` submits the whole validated model state or a deterministic
change set to the user's **personal DVMDB**, with exact expected **local**
branch HEAD/null, target local branch and an idempotency key. Success creates
one immutable revision UUIDv7 and advances only its local branch HEAD.

`Publish` transmits verified immutable commits to the shared project
repository and conditionally advances the **shared** branch HEAD. An
explicitly null expected remote HEAD is required on first publication.
A successful local commit is never undone by rejected or timed-out Publish.

If the local HEAD changed before Commit or the shared HEAD changed before
Publish, show which **local or shared branch advanced**, preserving both
private drafts and all successful local commits. Offer compare/reconcile,
explicit fast-forward or three-way merge resolution. Never automatically reset another
user's work, force-publish, or blindly retry a timed-out write. A HEAD
mismatch is an optimistic-concurrency collision and might or might not be
an overlapping model-data conflict. Validate the complete hydraulic network
before publishing a reconciled commit.

## Inspector tabs and actions

| Selection | Contextual tabs | Principal actions |
|---|---|---|
| Database | Overview, Connection, Projects | Create project, refresh |
| Project | Overview, Branches, Networks | Start working copy, create branch |
| Branch | Overview, Revisions, Working copies | Checkout, create branch from committed HEAD, compare/merge |
| Committed revision | Overview, Parents/History, Snapshot | Inspect, compare, branch from this finalized revision |
| Private working copy | Overview, Model, Changes/Validation | Edit, commit, reconcile, discard with confirmation |

Only show tabs with implemented, meaningful content. Creation forms live
**inside** the inspector, with validation, Cancel and explicit source context;
they are not permanent Creation tabs or separate modal inspectors. Dangerous
operations require appropriate confirmation and permissions.

The `Created from` field displays source **branch UUIDv7** and exact immutable
**fork revision UUIDv7**, not just the source's current moving HEAD. It is
metadata rather than a second edge in history. The revision inspector shows
full revision UUID, ordered parent IDs and SHA-256 verification metadata.

## Graph and terminology

The canvas draws a **Git-style single committed revision DAG with branch
labels and optional visual lanes**, not branch-owned duplicate histories; see
[graph rules](project-graph-layout.md). It must not draw a branch as a
revision leaf, treat an editable working copy as a committed revision, or
connect the same branch with both a HEAD edge and a creation-origin edge.
Several branches can reference one committed revision. Their future commits,
not their creation events, create a true history fork.

User-facing labels:

- **Branch:** stable named development line.
- **HEAD:** latest **committed** revision of that branch, or `No commits yet`.
- **Revision / Commit:** immutable finalized whole-model snapshot.
- **Working copy:** editable state owned by the user/session, not a revision.
- **Save draft:** persist uncommitted edits in the personal working DVMDB.
- **Commit:** create one immutable commit and advance a **local** branch HEAD.
- **Publish:** conditionally advance the shared repository's branch HEAD
  after verified history transfer.
- **Branch from here:** create a branch from this committed snapshot, without
  creating another snapshot.

## Native and REST parity

Both backends must supply real UUIDv7 project, branch, revision and
working-copy IDs; immutable SHA-256 metadata; nullable unborn HEADs;
branch provenance; ordered revision ancestry; consistent paginated graphs;
and explicit optimistic conflict semantics. Never invent UUIDs in the GUI,
assume a revision exists when a branch is unborn, or quietly fall back to
a mismatched identity format.

All model/session operations must use a source-qualified project ID.
Asynchronous responses must be tied to the request generation and selection;
late results may not overwrite newer data. Source outages leave drafts
recoverable and persisted catalogues visibly stale, not silently writable.

## Acceptance

- Empty newly created project displays `main` but no revision card.
- User A and user B independently check out and edit `main` at R7.
- A commits locally and publishes R8; B may commit locally from R7, but
  B's stale Publish is rejected without losing local commits or drafts.
- Creating a feature branch from R7 keeps that exact fork point and does
  not create a new revision.
- Committing independently to `main` and `feature` creates the actual fork
  in the revision graph; no arbitrary dashed origin edge is drawn.
- Graph selection, filter, refresh, inspector tabs and camera manipulation
  never move a committed HEAD or change private drafts.

This is the required workspace behavior; implementation and tests must be
brought into conformity with it before checkout/commit is released.
