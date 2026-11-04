# Projects and private editing — verification guide

This page lists the **current product acceptance scenarios**. It is not an
implementation-stage history or a claim that every scenario has passed.

1. Create a private project: it has a real project UUID, an unborn `main`
   branch and no immutable revision. Its editable HEAD appears in Projects.
2. Open a private network and edit every supported hydraulic collection,
   root metadata, controls, libraries and map labels. Check that the active
   writer's staged state matches `ModelCodec<NetworkHydraulic>` exactly.
3. Shut down and restart the GUI **and** DVMDB, reopen that same draft, and
   compare all canonical model bytes. Repeat against embedded Standalone.
4. Lose an edit Save reply; verify authoritative journal and local pending
   queue reconcile without duplicate events or a false Autosaved indicator.
5. Commit an intentionally unsupplied but referentially consistent network;
   verify exact immutable revision, local HEAD movement and unchanged shared
   HEAD. Recover a lost Commit reply with its original operation ID.
6. Verify project/draft discovery past 100 entries and wrong repository
   role/UUID/fingerprint refusals.
7. Verify an immutable Revision opens read-only or through explicit branch
   creation; no revision content is rewritten.
8. On an eligible committed-base private draft, exercise direct History
   action jumps, archived Commit markers, Undo/Redo and retained Redo across
   Commit boundaries. Verify immutable revision and committed HEAD stability.
   Repeat across GUI/DVMDB restart and lost navigation acknowledgement;
   distinguish implemented code from accepted end-to-end behavior.
9. Verify genuinely **unfinished** features (unborn first-draft History,
   general replacement, complete Publish/Merge workflows) are not falsely
   exposed as working.

See [product documentation](README.md), [remaining integration](dvmdb-integration-roadmap.md),
and [History roadmap](history-roadmap.md). Keep actual commands and observed
pass/fail outcomes in test reports, not inferred from source presence.
