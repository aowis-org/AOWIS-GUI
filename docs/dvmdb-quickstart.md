# DVMDB quickstart — browse, edit and save a private network

This is the shortest current workflow for the AOWIS **Projects** GUI.
It distinguishes what is available today from features still planned.
For the exact lifecycle and failure rules see the
[Save/recovery contract](personal-working-copy-save.md).

## 1. Make a DVMDB source available

**Separate DVMDB server:** start the DVMDB HTTP executable using that
repository's build/run script. Its usual development endpoint is
`http://127.0.0.1:8755` (subject to server configuration). Verify:

```bash
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready
curl -fsS http://127.0.0.1:8755/dvmdb/v1/repository
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects
```

**Standalone:** its DVMDB engine is built into the same process as the GUI.
Select a local source with a persistent absolute DVMDB data directory. It
uses an internal dynamically bound `127.0.0.1` HTTP endpoint and does not
require starting a separate server executable.

## 2. Add and designate your personal database

1. Open **Projects**.
2. Add/select a database/server in the left-side server section and use
   its **Server Inspector** to verify source identity and configuration.
3. Select **Use as my personal working database** on the repository that
   should hold your private drafts. If necessary, the GUI verifies/configures
   its private role. Do not choose a shared server merely because it is the
   currently selected project source.
4. Check for **♛** in the navigator and the **gold, crowned server card** in
   the history graph.

The user does **not** need to type a repository UUID in a separate personal
settings page. A mismatching or unverified repository identity is an error.

## 3. Create, edit and reopen a network

1. Select the personal server in Projects and create a new project.
   New `main` is **unborn**; there is no artificial first Commit.
2. Select the project; in the **Project Inspector**, choose **Edit network**.
   If it has multiple branches, select the intended branch first.
3. The GUI creates/reuses a private working draft and initializes an empty
   hydraulic network when appropriate. It opens **Distribution Editor**
   after verifying the saved candidate and obtaining a writer lease.
4. Make supported changes (for example, add two junctions and a pipe).
   They are captured and durably queued before DVMDB Save. A status message
   can confirm the private draft was saved; this does **not** create R1.
5. Stop private editing, reopen the same project/draft and verify the
   network. A real external HTTP DVMDB GUI-restart/reopen test has already
   succeeded for tested edits. Complete-network and Standalone acceptance
   are still pending.

Advanced controls live in the **branch Inspector**, not in the project
Inspector: select the database-backed **branch** (expand the project in the
tree) and use **Personal working copies…**. That dialog offers **Checkout
branch privately**, draft creation/reopen, **View in Distribution Monitor**
(read-only), and **Edit in Distribution Editor** (fenced editing).

The branch checkout transfers one selected remote branch, not every branch
of a project. A whole-project Clone button and local Commit are not present.

## 4. Hydraulic Libraries, controls and labels

The narrow vertical application navigation bar includes **Hydraulic
Libraries**, followed by **Controls & Map Labels**. Those pages call the
transaction-safe `HydraulicData` editing APIs. Their edits participate in
the MODEL transaction system when a writable personal draft is bound.
The generic capture engine covers all 18 entity collections, but complete
mutation-path and network-root autosave acceptance is not yet finished.

## 5. What not to assume

- **Save draft** does not create a revision; **Commit locally** remains a
  future GUI workflow. **Publish** is a separate later workflow.
- Updating root network settings such as simulation headloss formula does
  **not** yet have complete durable metadata transaction support.
- EPANET and EPANET-JS import are **not yet defined as safe replace/merge
  operations for an active private draft**. The proposed HC-02.9 import ban
  was **rejected and not applied**. See
  [network open/import decisions](network-open-import-replacement.md).
- An HTTP response timeout may occur **after** DVMDB accepted a Save. Do
  not discard pending queue records or retry as a fresh event without
  authoritative journal verification.
- Standalone's internal HTTP server is still a loopback listener, not a
  security boundary.

For detailed status: [roadmap](dvmdb-integration-roadmap.md),
[Projects UI](projects-workspace.md), and
[personal database configuration](working-repository-configuration.md).
