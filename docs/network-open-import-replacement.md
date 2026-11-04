# Open, import and export hydraulic networks

This page describes the operations exposed in the current GUI source. Unimplemented
replacement and selective import belong in the
[remaining integration roadmap](dvmdb-integration-roadmap.md).

| Action | Current behavior |
|---|---|
| **Open HEAD** | Resolve exact project/branch/private draft, load committed base plus saved overlay, then open fenced Distribution Editor |
| **Open immutable Revision** | Read pinned snapshot in Distribution Monitor or explicitly create a branch for editing; never modify a revision in place |
| **First import** | Import EPANET `.inp` or `.ejsdb` into an **untouched, empty unborn private HEAD**, using a fenced server operation |
| **Export HEAD** | Export the selected saved draft network and verified generation, not an unrelated in-memory editor model |
| **Export Revision** | Export the explicitly selected immutable revision/network |

An unborn HEAD is not necessarily empty. A saved network may exist without
any immutable Commit. A network containing zero entities may still have a
root, settings and libraries. Deleting the last visible object is **not**
equivalent to deleting the network root. The Project/HEAD Inspector does not
have a **Delete network** button.

First import is a narrowly defined, server-enforced operation. The GUI
should enable it only when the selected private draft is eligible;
a populated, previously edited or stale draft must not be overwritten.
`HydraulicData::replaceNetworkHydraulic()` replaces **only the in-memory
network** and must not be used to imply a persistent replacement operation.

**Not implemented:** an atomic "Replace working network" operation, selected
entity/library import into an existing network, UUID/reference reconciliation,
or safe replacement of a dirty draft. These require explicit user intent,
lease/generation/queue checks, recoverable idempotent server operations and
integration with the implemented History navigation and its retained
MODEL actions and Redo state. See the
[roadmap](dvmdb-integration-roadmap.md) for design and acceptance.
