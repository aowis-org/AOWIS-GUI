# GUI–DVMDB — next product work

This roadmap records **unfinished product capabilities and required
acceptance**, not historical patch stages. Current operation is documented in
[Projects](projects-workspace.md), [private working copies](personal-working-copy-save.md),
[History](history-contract.md) and [History recovery](history-recovery.md).

## 1. Complete project checkout to a personal working DVMDB

AOWIS currently designates one personal working server in the GUI. That
server can be embedded or reachable by REST; it is **not necessarily local**.
Projects on other repositories are browsable, but must be checked out to
the designated personal working server before direct edits. Complete the
user-facing **whole-project checkout** workflow:

- Select a source project and a target personal DVMDB, with explicit
  repository identities and supported MODEL fingerprint.
- Transfer/verify required immutable revisions, branches, shared ancestry
  and branch HEADs, preserving UUIDs without overwriting target drafts.
- Track source/target identities and the provenance needed for future Fetch
  and Publish; use bounded/resumable transfer where possible.
- Open the selected editable personal HEAD/draft when checkout succeeds;
  show missing dependencies and conflicts without destructive fallback.
- Maintain the abstraction that can later select **different personal
  working servers for different projects** without assuming one embedded
  local server in the transport or persistence format.

Existing branch-level checkout and native bundle primitives are useful
building blocks; they do **not** equal complete GUI project checkout.

## 2. Fetch, Publish and conflicts

Implement incremental Fetch of immutable revisions and remote tracking
without overwriting active personal drafts. Publish should use expected
remote HEAD/generation, idempotent operation identity and explicit divergent
ancestry handling. Build merge/conflict UI around native revision/merge
primitives only after safe transfer and reconciliation are accepted.

Connectivity failures must not silently redirect writes or create duplicate
remote revisions. The transport should minimize unchanged MODEL transfers,
consistent with sparse History navigation.

## 3. Complete editing, import and lifecycle acceptance

Run the full 18-collection/root-metadata GUI mutation matrix against a
**separate live DVMDB process** and embedded Standalone; test Save, Commit,
branch/fork, HEAD deletion and revision truncation through actual HTTP.
Check expected-HEAD/role/identity guards, lost Save/Commit acknowledgements,
writer contention, queue restart, malicious/malformed response, bulk action
limits and disk failures. Invalid hydraulic *solvability* must not alone
prevent Save or Commit.

General network replacement and selective import need explicit source/target
identity, UUID-reference, pending queue and History semantics. The current
first-import path is for an untouched, empty unborn personal HEAD.

## 4. Release/platform qualification

Build and execute focused then full regression suites on native Linux,
Standalone and supported WebAssembly/browser deployments. Test large
networks, bad bandwidth/high latency, server outages, process restarts,
power loss and storage exhaustion. Record exact versions, commands, outcomes
and remaining failure cases. Unit tests and source review alone are **not**
release acceptance; see [verification status](verification-status.md) and
[History acceptance](history-roadmap.md).

Authentication, authorization, transport security and deployable trust
boundaries are a distinct security workstream. A personal role designation
is not a security boundary. No speculative pre-release migration or elaborate
vertical-datum layer is required for the current product.
