# Implementation and verification status

**Source baseline:** GUI snapshot `gui(20261104-161516).zip` (4 November
2026), with the independent branch History pagination patch. This page records implementation separately from **observed** test
results. It is not a release certificate.

## What exists in source

- Projects navigator and revision DAG; personal ♛ database profiles; editable
  HEAD and immutable Revision inspectors; private draft Save and local Commit.
- Hydraulic MODEL capture for all 18 collections and persistent root fields;
  queued HTTP Save and guarded canonical editor reload.
- History dock with DVMDB-backed adjacent Undo/Redo, archived action page
  expansion, direct jumps to eligible saved/archived actions and Commit
  checkpoints, and independently paged first-parent Commit ancestry.
  DVMDB owns the durable history cursor and immutable archives.
- Client-side protections for pending Saves, stale cursor/scope and uncertain
  navigation, with deliberate refusal of unsupported multi-network History
  requests. First initialized unborn drafts now have a verified single-network
  replay path, subject to the remaining membership-order limitations.
  Multiple-network **storage** is unchanged.

## Reported tests and acceptance boundary

| Evidence | Most recent reported observation | What it does **not** establish |
|---|---|---|
| GUI full `compile_linux_tests.sh` suite (4 Nov 2026) | **70/70 passed**, including `aowis-history-dock` and personal working-copy client, after fixing the aggregate test target | Does not prove real editor + external DVMDB navigation or GUI/server restart recovery |
| DVMDB hydraulic and HTTP regression (4 Nov 2026, HDB-03k) | User reported **tests passed** after the new unborn-tank test compilation fix | Does not establish complete GUI workflow, multi-network replay or all platform configurations |
| GUI independent branch ancestry paging | Added source and focused mock/dock tests; **not yet compiled or executed on user's Qt6 setup** | Source checks cannot stand in for actual Qt tests |
| Real GUI + external DVMDB, Standalone and browser/WASM workflows | **Not yet accepted end to end** | Unit tests do not cover every editor synchronization and platform condition |

The intentionally logged `content checksum mismatch during read` in the
DVMDB HTTP suite occurs in a corruption-response test; it is **not** by
itself evidence of a new integrity defect. Earlier HTTP regressions were corrected and HDB-03k tests were reported
passing; this page does not infer a whole-system acceptance from that result.

## Suggested focused GUI regression

From an already configured `AOWIS-SERVER-GUI/build-linux-tests`:

```bash
cmake --build build-linux-tests \
  --target aowis-history-dock-tests \
           aowis-personal-working-copy-client-tests \
           aowis-hydraulic-edit-transaction-tests \
           aowis-server-gui -j 8
ctest --test-dir build-linux-tests --output-on-failure \
  -R '^(aowis-history-dock|aowis-personal-working-copy-client|aowis-hydraulic-edit-transaction)$'
```

On DVMDB, retain HTTP and hydraulic-network regression coverage for the
unborn first-draft workflow. Report exact revisions, environment and live
editor restart results before changing an end-to-end acceptance status.

Further work and required evidence: [History roadmap](history-roadmap.md),
[GUI ↔ DVMDB integration roadmap](dvmdb-integration-roadmap.md).
