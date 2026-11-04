# Implementation and verification status

**Source baseline:** GUI snapshot `gui(20261104-150843).zip` (4 November
2026). This page records implementation separately from **observed** test
results. It is not a release certificate.

## What exists in source

- Projects navigator and revision DAG; personal ♛ database profiles; editable
  HEAD and immutable Revision inspectors; private draft Save and local Commit.
- Hydraulic MODEL capture for all 18 collections and persistent root fields;
  queued HTTP Save and guarded canonical editor reload.
- History dock with DVMDB-backed adjacent Undo/Redo, archived action page
  expansion, direct jumps to eligible saved/archived actions and Commit
  checkpoints. DVMDB owns the durable history cursor and immutable archives.
- Client-side protections for pending Saves, stale cursor/scope and uncertain
  navigation, with explicit refusal of unsupported unborn-base/multi-network
  History requests. Multiple-network **storage** is unchanged.

## Reported tests and acceptance boundary

| Evidence | Most recent reported observation | What it does **not** establish |
|---|---|---|
| DVMDB local Commit, working-copy, project-service and offline-backup suites (4 Nov 2026) | **4/4 passed** in the latest six-suite HDB-03g run | Does not prove GUI navigation or HTTP integration |
| DVMDB hydraulic-network suite (same run) | **Passed** | Does not prove full editor/HTTP restart recovery |
| DVMDB HTTP suite (same run) | **Failed** in the new HDB-03g retained-Redo assertion; a test-only response-field fix was supplied afterward, with no reported successful rerun | A corrected test source is not a passing HTTP run |
| GUI History dock, personal working-copy client, and full GUI after direct-jump changes | **No confirmed Qt build/test results supplied for this snapshot** | Source presence and ZIP validation do not prove runtime acceptance |
| Real GUI + external DVMDB, Standalone and browser/WASM workflows | **Not yet accepted end to end** | Native tests do not cover renderer/editor synchronization or platform integration |

The intentionally logged `content checksum mismatch during read` in the
DVMDB HTTP suite occurs in a corruption-response test; it is **not** by
itself evidence of a new integrity defect. The latest HTTP suite's actual
failure assertion still needs a passing rerun.

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

On DVMDB, rerun at least its HTTP and hydraulic-network suites **after**
applying the latest HDB-03g test-assertion correction. Report the observed
results, exact revisions and test environment before changing an acceptance
status.

Further work and required evidence: [History roadmap](history-roadmap.md),
[GUI ↔ DVMDB integration roadmap](dvmdb-integration-roadmap.md).
