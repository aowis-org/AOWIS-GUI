# Projects identities and revision semantics in the GUI

This is the current identity convention used by the Projects navigator,
inspectors, and DVMDB API clients; it is not a migration/readiness roadmap.

- The repository, project, branch, immutable revision and private working
  copy have distinct UUIDv7 identities. Branch names are mutable labels.
- Immutable revision records contain ordered parents, author, message,
  time, canonical snapshot roots and SHA-256 integrity information.
  SHA-256 is **not** the revision's logical ID.
- `headRevisionId: null` means an existing unborn branch, **not** a missing
  branch. A newly created project has unborn `main` and no fake first
  commit; independent unborn branches can coexist.
- DVMDB's committed branch **HEAD** is a nullable revision reference. The
  GUI's **HEAD card** is the editable working tip; it is **not** an immutable
  revision. Opening it creates or reopens a separate private draft.
- Shared immutable revisions can be reachable from several branches and
  are drawn only once in the Projects graph. The branch's fork provenance
  is metadata, not an invented ancestry edge.
- Checkout preserves logical UUIDs and immutable ancestry while creating a
  distinct working-copy UUIDv7. Personal and shared repositories have
  independent branch HEAD refs.
- Creation, rename, checkout and metadata writes use canonical identities,
  not names for cross-repository matching. The GUI verifies repository
  identity/role and MODEL fingerprint; pinned metadata writes carry the
  selected repository UUID.

For the actual UI see [Projects](projects-workspace.md); for storage/API
semantics see [DVMDB version control](../../AOWIS-SERVER-DVMDB/docs/version-control-semantics.md).
Whole-project Clone/Fetch, Publish and conflict-resolution GUI flows are
unfinished and described only in the [integration roadmap](dvmdb-integration-roadmap.md).
