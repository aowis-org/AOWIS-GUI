# DVMDB quickstart — project, HEAD, editor and Commit

This is the current shortest workflow for the **Projects** tab. For exact
UI controls see [Projects workspace](projects-workspace.md); for uncertain
writes, leases and recovery see [private Save](personal-working-copy-save.md).

## 1. Start and verify your DVMDB server

From the **AOWIS-SERVER-DVMDB** repository:

```bash
./compile_linux.sh             # build server/dependencies, then start the server
# or: ./compile_linux.sh --build-only  # build without starting it
```

From a second terminal, for the default local endpoint:

```bash
curl -fsS http://127.0.0.1:8755/dvmdb/v1/health
curl -fsS http://127.0.0.1:8755/dvmdb/v1/repository
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects
```

A 200 `/health` means the process responds; it does **not** prove the
catalogue or repository identity matches your GUI profile. If `/projects`
returns an unexpected empty list, **check the storage root and repository
UUID before creating anything**, rather than assuming saved data was lost.

Standalone embeds DVMDB in the GUI process and exposes its routes on a
runtime-only loopback port; it does not require a separate server binary.

### Connecting from the WebAssembly GUI

The browser version creates **remote HTTP connections only**. The `New database`
form verifies `GET <endpoint>/dvmdb/v1/repository` asynchronously before
saving a verified profile. If the request cannot be completed, it offers an
explicit **pending/unverified** profile instead. A pending connection
must not be treated as an authenticated or verified repository until a
subsequent successful identity check.

Browser requests to a DVMDB address with a different host, scheme or port
require a suitable **CORS policy on the DVMDB response**. The GUI's Apache
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` headers enable threaded WASM,
but do **not** grant permission to fetch from the DVMDB origin. The DVMDB
service (or a correctly configured reverse proxy) must allow the GUI's origin,
and any browser-required preflight requests. Use the browser Network/Console
tabs to distinguish HTTP failure from a CORS rejection. A URL working with
`curl` is not sufficient evidence that a browser can access it.

The WASM GUI stores configured database profiles in the current **browser
origin's** `localStorage`, not in the native GUI INI file. Profiles on two
different GUI hostnames are separate even when both serve the same build.
Database creation confirmations are nonblocking: a rejected connection check
should show an error or pending-choice dialog, not terminate the WASM app.

## 2. Select your personal database

1. In **Projects**, configure the local/remote database in its
   **Database Inspector → Connection** tab.
2. Confirm its UUID and availability, and designate the intended private
   source **Use as my personal working database**.
3. The selected personal server displays a ♛ in the navigator and a gold
   crowned card in the graph.

Shared browsing sources and the personal editing source are **not**
interchangeable. An offline or mismatched personal repository must not be
silently substituted with another configured server.

## 3. Create a project and HEAD

Create a project in the selected personal database. DVMDB creates an unborn
`main` branch automatically; the graph shows **Project → HEAD (Branch: main)**
with no committed revision.

The **Project Inspector → Overview** and **Branches** tab both expose
**Create branch**. Enter branch **name**, optional comma-separated **tags**
and **description**. From the Project Inspector this creates another
independent unborn branch and a HEAD card, not a synthetic revision.

Choose the HEAD card corresponding to the branch you intend to edit. The
Branch tab lets you rename its owning branch (with confirmation) and edit
its tags/description.

## 4. Open, import and edit

Open the chosen **HEAD → Overview**. The six network counters summarize the
**saved private working copy**, including its committed base, rather than
just the branch's last committed revision. `No commits yet` is not the same
as `No network`: an unborn HEAD can already contain a saved model.

- **Open network:** replaces the current in-memory editor model with the
  selected verified draft, acquires a fenced writer lease and opens
  Distribution Editor. Supported model changes autosave to DVMDB.
- **Import network:** accepts EPANET `.inp` or `.ejsdb` into an **untouched,
  empty unborn private HEAD**. It is disabled for populated HEADs and does
  not replace a previously saved network. Importing into a manually emptied
  existing HEAD is not yet implemented.
- **Export network:** exports the selected saved HEAD's exact generation
  when one network is present; it does not export whichever model happens
  to be open in another editor session.

There is **no Delete network action** on the Inspector. Deleting entities
is an editor operation subject to model-reference checks and private draft
autosave. For an eligible saved private draft, open the toolbar's **History**
dock to Undo/Redo, expand original actions archived under a Commit or click
an action/eligible checkpoint to jump to its state. Commit markers do not
consume Undo steps or change committed HEAD. The editor applies a verified sparse MODEL delta after navigation, not a
full-network re-download; pending Saves or stale sessions block navigation. See [Using History](history.md) and the
[technical History contract](history-contract.md).

## 5. Commit an immutable revision

On **HEAD → HEAD**, enter a **nonblank commit message** describing your work.
Choose **Commit** once the private draft is saved and modified. Local Commit
creates an immutable revision and advances only the personal branch. The
working HEAD remains available, based on the new revision. Reopen it from
the graph; the network can be inherited entirely from the committed base
even when the draft overlay has no pending changes.

Committing preserves a milestone for future comparison or branching; it is
not an independently stored backup and does not automatically Publish to
another server. The HEAD tab explains the distinction in **Why commit a
revision?**.

To open an immutable **Revision** network, use Revision → Overview →
**Open network** and choose either read-only Distribution Monitor or
**Create branch and open for full editing**. Editing the revision directly
is forbidden; making a branch does not rewrite the original snapshot.

## 6. Limits and recovery

- Generic capture includes all **18 hydraulic entity collections** and
  network-root metadata, including mixed root/entity events. Complete
  cross-process HTTP/Standalone and every UI mutation-path acceptance remain open.
- Unsupported/uncaptured changes must block rather than appear saved.
- The current safe imported-network path is first import to untouched unborn
  HEAD; replacement and entity merge are separate unfinished workflows.
- A draft Save does not Commit. A local Commit does not Publish.
- On uncertain History responses, the GUI keeps the same operation UUID,
  reconciles SHA-256 **and** cursor, and restores a durable pending intent
  after GUI restart. Do not issue an independent second Undo.
- Standalone and browser/WASM full end-to-end acceptance remain separate
  validation tasks.
- The **History** toolbar action opens the left History dock without leaving
  the editor. For a ready private session with no pending Save, DVMDB provides
  adjacent Undo/Redo targets and verifies direct jumps to saved and archived
  actions or eligible Commit checkpoints. Archived actions expand in pages of
  50; Commit ancestry is paginated independently. The selected state is
  published atomically and applied as a verified sparse delta. Initialized
  unborn first drafts are supported for eligible action history; unprovable
  membership deletion order, multi-network History navigation and keyboard
  shortcuts remain out of scope. See the
  [History documentation](history.md) and [recovery details](history-recovery.md).

See [network import semantics](network-open-import-replacement.md),
[hydraulic transactions](hydraulic-edit-transactions.md), and the
[integration roadmap](dvmdb-integration-roadmap.md).
