# Hydraulic network load, first import, replacement and merge

**Current contract reviewed 2026-11-02.** Earlier design notes described
all private-draft imports as undecided. A **narrow safe first-import path is
now implemented**; replacement and merge remain separate unfinished work.

## Different operations

| Operation | Current behavior |
|---|---|
| Open selected HEAD | Reads its verified saved private draft (base plus overlay); edits use fenced autosave |
| Open immutable revision | Reads the exact committed snapshot in Monitor; offers creation of a new branch for writable editing |
| First import into untouched unborn HEAD | Supported: selected private draft, EPANET `.inp`/`.ejsdb`, backend fenced import, no prior network |
| Export selected HEAD | Reads one verified saved draft network at an exact generation, not the global editor |
| Export selected revision | Reads exactly the pinned committed revision |
| Replace an already saved network | **Not implemented** as an atomic draft operation |
| Merge imported entities into a network | **Not implemented**; needs topology, identity, and reference/conflict policy |

The **Import network** action is disabled unless the GUI can establish an
eligible untouched unborn draft. DVMDB also enforces this independently and
returns a conflict if the target is populated or stale. An unborn branch is
not automatically empty, and a HEAD based on a commit may be empty without
being an untouched unborn draft. Do not enable first import merely because
its six entity counts are zero.

A network with no entities may still have a persistent network root and
other hydraulic configuration. Selecting all elements and deleting them in
the editor does **not** necessarily remove that root. **No explicit Delete
network button belongs in the Inspector.**

## Persistence boundary

`HydraulicData::replaceNetworkHydraulic()` replaces the in-memory model and
resets capture; it is useful for verified loading, but by itself it is not a
transaction-safe persistent replacement. A private editor session has an
exact project/branch/draft identity, durable queued events, writer lease,
fencing token and generation. Do not mix data from a different editor's
model or silently discard a pending queue.

The first-import route is a separate server-side transaction; it rejects a
populated draft before applying anything. It does not implement arbitrary
replacement, selective entity import, or merging.

## Still to design and test

1. Explicit replacement intent and target identification, if replacement is
   ever offered in the editor; never invent a bulk Delete action.
2. Network-root identity and handling when a model has zero elements but
   still stores hydraulic metadata or libraries.
3. Transactional replacement/merge with exact old generation and lease,
   pending queue handling, UUID/reference/topology validation, and recovery
   after a dropped acknowledgement.
4. Importer parity across desktop HTTP, embedded Standalone and WASM, where
   applicable; precise errors when source format is unsupported.
5. Repeated imports and stale-generation errors must leave existing network
   data unchanged. Export must be tied to the selected saved network, not
   current in-memory editor selection.

See [Projects workspace](projects-workspace.md), [quickstart](dvmdb-quickstart.md)
and [private Save](personal-working-copy-save.md).
