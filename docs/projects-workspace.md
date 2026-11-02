# Projects workspace — current user interface

The **Projects** tab unifies DVMDB databases, project containers, named
branches, committed revision history and working **HEAD** cards. Its inspector
is selected by the object clicked in the navigator or graph. For a practical
workflow see [quickstart](dvmdb-quickstart.md); for graph semantics see
[graph layout](project-graph-layout.md).

## Workspace structure

| Area | Current responsibility |
|---|---|
| Servers (left) | Configured local/remote databases, connection status, per-source visibility, and ♛ personal database designation |
| Projects & Branches (left) | Project and named-branch navigation; selecting a *branch row* opens its branch-management inspector |
| Revisions (left) | Immutable committed revisions; absent for a branch with no commits |
| Graph (center) | Server and project cards, **immutable revision cards** and **writable HEAD cards**; no independent branch cards |
| Inspector (right) | Context-sensitive Overview and management tabs for Database, Project, Branch, HEAD or Revision |

The personal database has a crown in the navigator and a gold server card in
the graph. A visibility checkbox is a graph/list filter, **not** a
connect/disconnect control. DVMDB connection and repository identity are
configured in the Database Inspector; its health indicator must not confuse a
reachable server with an unreadable project catalogue.

Every project, branch, revision and private draft uses UUIDv7 identity.
Branch names are labels and may change. A revision's SHA-256 is integrity
metadata, never an object ID.

## Graph cards and click targets

A newly created project automatically has `main` with **no committed
revision**. Its graph starts as `Server → Project → HEAD (Branch: main)`.
Creating another independent unborn branch from the **Project Inspector**
adds another HEAD card directly beneath the project, without creating a
revision. A branch created **from an immutable revision** has a new HEAD
linked to its selected fork revision. There is no separate branch card.

After committing HEAD, an immutable revision card is added. The next writable
HEAD inherits the committed network through that revision. A revision shared
by several branches is one immutable object, not duplicate history.
Revision-to-revision edges encode only true parentage; the HEAD-to-base or
Project-to-unborn-HEAD links are structural, not extra commits.

- Click a **HEAD card** → HEAD/Revision Inspector in HEAD mode.
- Click an **immutable Revision card** → the same Inspector in Revision mode.
- Click a **Branch in the left tree** → Branch Inspector; this is distinct
  from clicking its HEAD card.
- Click a Project or Database card → its respective inspector.

The graph starts top-down; Fit is available explicitly. Camera movement,
selection and source filtering never change saved history.

## Inspectors

**Project Inspector:** Overview has editable **Name** (persisted project
rename), Tags/Description display, **Create branch**, and **Import network**
where that project-level action is available. Branches tab lists branches
and offers another **Create branch** button at the bottom. The branch dialog
asks for **name, tags, description**, then creates an independent unborn HEAD.
No source revision selector is needed when invoked from a Project.

**HEAD Inspector:** `Overview | HEAD | Branch`. Overview shows selected
branch, saved network inventory (junctions, pipes, reservoirs, tanks, pumps,
valves), **Open network**, **Import network**, **Export network**. The **HEAD**
tab contains the editable two-line-height commit message, draft-specific
tags and description fields, state, disabled **Create branch** with a
commit-first tooltip, **Commit**, and the **Why commit a revision?** explanation.
The **Branch** tab edits name, tags and description of the owning branch;
branch renaming requires confirmation. There is **no Revisions tab** on HEAD.

**Revision Inspector:** `Overview | Revision | Branch | Ancestry`.
Overview summarizes the pinned immutable network. **Open network** offers
read-only Distribution Monitor or creating a new branch to edit the
selected revision. Revision tab offers an editable **display message**,
metadata fields and **Create branch**; immutable contents and record SHA-256
are never rewritten. Branch tab edits branch metadata. Ancestry displays
actual parent revisions.

**Current metadata limitation:** Tags and Description shown on the
HEAD/Revision details and Branch tabs currently share the branch-metadata
save path. They are **not yet separately persisted per-HEAD/per-revision
annotations**. A distinct revision/HEAD metadata store remains future work.
Do not present the duplicate editors as independent persisted fields.

**Database Inspector:** `Overview | Connection | Projects`; server status and
connection settings belong here. Keep repository UUID identity distinct from
the editable database display name.

## Open, Save, Commit, Publish

The normal editing entry is **HEAD → Overview → Open network**. A confirmation
explains that the in-memory editor network will be replaced; supported edits
are automatically captured and saved to the private DVMDB draft. The editor
requires a verified personal repository and fenced writer lease. Uncaptured
or inconsistent edits block the session rather than pretending to save.

| Operation | Meaning | Status in current GUI |
|---|---|---|
| Open HEAD | Load saved private working state and open Distribution Editor | Implemented; requires selected usable private draft |
| Open Revision | Read-only Monitor, or create a branch based on that immutable revision for full editing | Implemented entry point; end-to-end platform coverage remains to be verified |
| Save / autosave | Durably record supported edited entities in private draft; no immutable revision | Implemented for captured operations; root-metadata coverage remains limited |
| Commit | Require a nonblank commit message, commit saved modified draft, advance only personal committed HEAD | Implemented via DVMDB Local Commit API; still requires valid saved draft/lease |
| Publish | Conditionally advance shared history, separately from personal Commit | **Not implemented as complete GUI workflow** |
| Fetch/Clone | Transfer a specific remote branch into personal working repository | Advanced branch-level checkout exists; whole-project workflow not complete |
| Merge/reconcile | Resolve divergent committed histories without losing drafts | Complete end-to-end UI not available |

After Commit the saved draft overlay can be empty, but the network still
exists in its committed **base** revision. The HEAD loader and summary must
read the full saved draft view, not just pending changed records.

## Network actions, restrictions and unresolved work

**HEAD Overview summaries** use `GET /working-copies/{draftId}/networks/summary`
for a selected saved private draft (including its committed base).
**Revision Overview summaries** use the exact committed revision. A zero
network count, a network with zero entities and an unavailable summary are
**different states**; do not silently equate them.

- **Open network:** opens the chosen HEAD/editor or revision viewer/branching
  dialog, never whichever unrelated model is already in memory.
- **Import network:** targeted EPANET `.inp` / `.ejsdb` bulk import currently
  supports an **untouched, empty, unborn private HEAD** only. The server
  rejects overwrite of an already populated draft. A HEAD manually emptied
  later is **not** automatically eligible for this first-import path.
- **Export network:** exports the chosen exact saved HEAD generation or exact
  committed revision, not the currently open editor network. Current flow
  expects exactly one selected network with a verified identity.
- There is **no Delete network button** in the Inspector. If users remove
  all entities they do so in the network editor using its normal selection
  and deletion operations; that does not itself guarantee the network root
  has ceased to exist.

A failing summary or a reported unavailable database is not evidence of data
loss. Check `/health`, `/repository`, `/ready`, `/projects`, the selected
repository UUID, and the saved draft/committed revision separately.

## Opening networks from the main toolbar

The toolbar's project and branch dropdowns are **network selectors**, not
revision previews. The branch dropdown always has a **[none]** placeholder:

- A project with exactly one branch preselects that branch and offers the one
  **Open hydraulic network** confirmation.
- A project with multiple branches selects **[none]**; selecting a real branch
  offers the same confirmation. No separate branch-selection dialog appears.
- **[none]** is a GUI choice, never an unborn DVMDB branch. Choosing it has no
  database side effects. Choosing the already-open branch does not reload it.
- The same confirmation is used by the writable HEAD / Project Inspector.
  For a remote branch it states that checkout into the ♛ personal database
  will happen first. Cancel changes neither the network nor DVMDB.
- After confirmation, checkout, draft discovery, initialization and verified
  load run asynchronously with status shown in the toolbar/footer, not in a
  flashing secondary dialog. A stale modified draft can still require an
  extra explicit safety decision; this is not an ordinary branch prompt.
- When switching away from a private editor, the previous fenced session is
  closed and its locally queued events remain durable. The new network is
  installed only after its own verified session is ready. A failed switch
  must not replace the in-memory model.

The toolbar status area now indicates **No network**, **Opening…**,
**Saving · N pending**, **Autosaved**, **Read-only**, or **Open failed**.
It is not a manual Save control. The future Commit action remains separate
from draft autosave. In the graph, a personal server keeps its left-side
server cylinder and displays **♛ before its name** on a golden card.

## Known acceptance gaps

The complete `NetworkHydraulic` model contains 18 persisted entity
collections. Generic entity capture does not prove that every GUI mutation
path or root setting autosaves. The remaining work includes full root
metadata, import into a deliberately emptied existing draft, standalone and
WASM acceptance, user-friendly recovery for stale/conflicting drafts, and
Fetch/Publish. See [Save/recovery](personal-working-copy-save.md),
[network import](network-open-import-replacement.md), and the
[integration roadmap](dvmdb-integration-roadmap.md).
