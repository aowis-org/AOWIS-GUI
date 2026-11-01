# Loading, importing and replacing a hydraulic network

**Design record — OPEN, not approved for implementation.** This distinction
was raised during HC-02.9. An earlier proposed patch that refused all
imports while a private draft was open was **rejected and not applied**.
Do not implement the rejected restriction, or an automatic replacement,
without resolving the decisions below.

## Terms: operations with different meaning

| Operation | Intent | Current support |
|---|---|---|
| **Load a DVMDB network** | Read an authoritative, verified working-copy candidate into `HydraulicData` for viewing or editing | Implemented: preview and fenced private editing paths |
| **Import as a new project** | Create a separate target project containing data converted from EPANET or EPANET-JS | External import/conversion exists; fully persisted new-project import workflow not accepted |
| **Replace the working network** | User explicitly chooses to make imported content the new state of an existing private draft | Not defined or safely persisted as an atomic user operation |
| **Merge imported entities** | Bring selected external objects into the currently edited network while retaining its other objects | Not defined; UUID/reference reconciliation required |

“Hydration” in old code/docs means **loading/reconstructing** a persisted
network. It is not an additional editing action. Prefer **Load network** in
user-facing copy. Verified load itself must not emit edit transactions.

## Source boundary that prompted this decision

`HydraulicData::replaceNetworkHydraulic()` currently replaces the entire
in-memory network and disables logical edit capture. The GUI uses it both
for verified DVMDB loading and for imported/replaced models. Consequently,
it is **not** a general-purpose, transaction-safe operation for editing a
DVMDB working copy. The fact that a call exists does not establish a
correct persistence policy.

A private editing session uses a verified candidate, a local durable event
queue, fenced writer lease and exact saved generation. Any import operation
must respect that session's ownership and its pending or uncertain writes.
The accepted solution cannot silently reset capture, discard the local queue,
change target repositories or claim an imported network was saved when only
the in-memory model changed.

## Decisions required before HC-02.9 / HC-06 implementation

1. **Entry points and targets.** Which GUI actions create a new project,
   replace the selected draft, or merge into it? What happens when no private
   working copy is open, or multiple branches/drafts are available?
2. **Replacement atomicity.** Is replacement one explicit, server-side
   generation-checked operation, or a bounded logical event group? Define
   durable rollback/recovery and authoritative verification after timeouts.
3. **Queued and uncommitted edits.** Should a replacement require a clean
   local queue, be sequenced after queued changes, or explicitly offer to
   preserve/cancel the operation? **Never silently discard** pending edits.
4. **Identity.** Which network UUID is retained or regenerated on replacement?
   What is the policy for colliding entity UUIDs and for missing cross-entity
   references? UUIDv7 is the identity scheme; labels alone cannot resolve
   collisions.
5. **Merge behavior.** Define entity selection, conflicts, topology/geometry,
   coordinate and unit conversion, dependencies, ordering and references.
   Do not call a whole-network replacement a merge.
6. **Working-copy history and recovery.** Does a replacement leave a
   reversible checkpoint or explicitly save a new durable candidate? How
   do restart, dropped reply and lease loss recover without duplicate writes?
7. **Cross-deployment parity.** The same operation must work against personal
   external HTTP DVMDB and the embedded Standalone repository. Specify
   which source/target repository each action modifies.

## Candidate user experience — proposal only

A clear long-term UI would offer **Import as new project**, **Replace this
working network**, and **Merge into this network** as distinct operations.
Importing into a new project should not unexpectedly replace the current
editor. Replacement should always identify its target draft and describe
what changes; merge should expose collision/conflict choices.

These are **proposals, not settled defaults**. Until decisions are made, the
safe development rule is to keep existing import and editing behavior
explicitly documented as *not fully integrated* with durable draft Save,
without inventing a blanket restriction or describing it as complete.

## Implementation/acceptance sequence after the decision

1. Record chosen semantics and failure outcomes here.
2. Separate verified **Load** from user-requested **Edit/Import** at the
   API boundary; never reuse an implicit capture-disabling assignment for
   a persistent edit.
3. Implement validated MODEL/DVMDB transaction or dedicated bulk operation
   as needed; preserve fencing, generation checks and durable queue safety.
4. Connect the explicit GUI actions and surface their outcomes accurately.
5. Verify normal success plus restart, pending queue, lost acknowledgement,
   stale lease, mismatched target, UUID collision and failed reference checks.

See [roadmap](dvmdb-integration-roadmap.md) and
[Save/recovery contract](personal-working-copy-save.md).
