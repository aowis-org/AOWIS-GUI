# Personal DVMDB designation and configuration

This page documents the **current GUI source behavior**, rather than an
older plan with a separate manual “Personal Repository” settings form.
AOWIS identifies database repositories by their canonical UUIDv7; a
profile name and endpoint are presentation/configuration, not identity.

## 1. Select the personal database in Projects

Open **Projects**, select a configured server/database, and use the
**Server Inspector → Use as my personal working database** checkbox.
The current GUI selects one designated personal source at a time. The
working-copy identity and transport must remain server-scoped so future
per-project designation of multiple personal working servers does not
require redefining what “personal” means. The GUI verifies
repository identity and role. Selecting a reachable `unconfigured`
repository may explicitly configure its role as `private`; a `shared`
repository cannot silently become a personal writable repository.
A source with unverified/pending identity cannot be selected.

A newly initialized DVMDB may still report `unconfigured`. Project and
branch **metadata** can be created there before choosing a repository role;
this does not automatically set `shared` and does not make the database a
personal editing target. If the repository is later designated personal,
the Inspector explicitly configures it as `private`. Working-copy edits and
local Commits still require that private designation. Metadata writes to an
already-private repository that is not selected as personal are blocked by
the GUI's repository-role preflight.

The sidebar shows a **♛** for the selected database. The corresponding
Projects graph server card becomes **gold**, retaining its normal left-hand
server icon and displaying a crown to the right of the server name.
Unchecking visibility of a source in the navigator affects display, not the
repository's persisted data or its role.

## 2. Deployment modes

| GUI mode | Personal draft target | Transport |
|---|---|---|
| Desktop/basic GUI | Explicitly selected private HTTP DVMDB server profile | DVMDB HTTP/JSON |
| Standalone | Selected local DVMDB profile with durable absolute directory | Native DVMDB service embedded in GUI process, accessed over dynamic localhost HTTP |
| WebAssembly | A verified reachable private HTTP DVMDB endpoint | HTTP/JSON; browser memory is not a durable database |

In Standalone, the implementation in `embedded_dvmdb_host.cpp` creates one
long-lived native DVMDB service per local data root and serves normal DVMDB
HTTP routes on `127.0.0.1` using a runtime-selected port. The GUI resolves
that endpoint from the local profile when needed; it is not stored in the
profile. There is **no separate DVMDB executable**, and the current
transport is **not** a direct C++ call path from editor to database.

Older documentation described a second automatically selected personal
storage directory distinct from the navigable local database. That is **not
the current profile-based selection model**. Do not reintroduce it through
settings work or mistake it for a required project checkout step.

## 3. Where configuration lives

The current implementation resolves the selected personal target from the
stored **ProjectServerProfile** list; `resolvePersonalRepositoryTarget()`
chooses the profile marked personal and verifies its canonical identity.
The GUI should not require users to manually type a repository UUID or
maintain a second independent personal-endpoint setting.

Shared sources may still be listed for browsing and branch checkout.
Selecting a project/branch on a shared source **never** makes that shared
repository the private draft target. Checkout must explicitly transfer the
chosen branch into the configured personal DVMDB, retaining the logical
project/branch UUIDs and immutable history, while local and remote committed
HEAD refs remain independent.

**Open network** performs checkout, draft creation/reopen, network loading and
editing automatically. There is no general-purpose **Personal working
copies…** inspector dialog. If a branch has multiple saved private drafts,
the Open workflow presents a chooser so the user can verify and select the
draft to view or edit. A straightforward whole-project **Clone to personal
database** action has **not** been implemented; it must not be described as
a current GUI button.

## 4. Session and recovery contract

Each writable draft is tied to its intended personal repository UUIDv7,
working-copy UUIDv7, pinned base, writer lease/fencing token and expected
persisted generation. A second session cannot override the same draft
without an authoritative writer transition. A missing personal repository
must leave saved/queued work recoverable, not fall back to a shared source.

The GUI's durable pending-event queue is separate from DVMDB's stored
candidate. Entries are retired only after authoritative Save confirmation.
A separate, checksummed client-side History navigation intent preserves the
original operation UUID, source MODEL, journal and cursor before a navigation
POST. On restart, the client reconciles it using SHA-256 **and** precise
operation/cursor identity without a full-model GET; a normal open still
loads the model once. The intent's storage must persist across GUI restart
(including browser storage if running under WASM). Save does not create a
Commit or move the branch's committed HEAD. Full platform acceptance is
still outstanding; see [History recovery](history-recovery.md).

Repository role designation is **not authentication**. External writable
HTTP endpoints and the Standalone loopback listener still require an
appropriate deployment environment. Security architecture is deferred
outside this implementation roadmap.

See [quickstart](dvmdb-quickstart.md),
[Save/recovery](personal-working-copy-save.md), and
[roadmap](dvmdb-integration-roadmap.md).
