# UUIDv7 version-control integration contract

AOWIS/DVMDB uses UUIDv7 for the persistent identities of projects,
branches, immutable committed revisions and private working copies.
SHA-256 verifies immutable revision records and complete Merkle snapshots;
it is never an alternative revision identity.

## Model boundaries

| Object | Display and interaction |
|---|---|
| Project | UUIDv7; owns named branches and revision history |
| Branch | Stable UUIDv7, UTF-8 label, nullable committed HEAD, origin branch/fork revision metadata |
| Committed revision | Immutable UUIDv7, ordered parents, canonical SHA-256, snapshot identity |
| Working copy | User/session-private UUIDv7, mutable model state, pinned base revision UUID/null |

A new project creates an unborn `main` and **no committed revision**.
Additional independent unborn branches can be added to the same project.
The GUI renders one working HEAD **card** for each branch; it is not a
committed revision or a revision DAG vertex.
Creating a branch from committed history does **not** create a revision.
Creating a new working copy does **not** create or advance a branch HEAD.
Only a successful expected-HEAD commit creates a new immutable revision.

## Native and REST reader/writer requirements

- Read paginated complete history with a stable snapshot of branch HEADs,
  two-parent merge closure, record hashes and branch-origin metadata.
- Treat `headRevisionId: null` as an unborn branch, not a missing object.
- Preserve full UUIDv7 IDs in model, selections, API calls and persistence;
  shortened labels are display-only. Names may contain valid UTF-8 spaces.
- Checkout always returns an independent working-copy UUIDv7, owner/session,
  base revision/null and target branch UUIDv7.
- Commit with an exact expected branch HEAD/null and idempotency key;
  return a conflict if HEAD advanced. Retain private work on conflict.
- Never silently replace a current user's draft with a stale catalogue read.
- Branch provenance is inspector metadata, not a second revision-ancestry
  graph edge.

## Verification

The complete acceptance suite must cover empty project, first commit, independent
branching, shared immutable ancestry, multi-user optimistic conflicts,
three-way merge, pagination, reload, replication and native/REST equivalence.
All resulting graph cards and inspector actions follow
[Projects workspace](projects-workspace.md) and
[version-control rules](../../AOWIS-SERVER-DVMDB/docs/version-control-semantics.md).
