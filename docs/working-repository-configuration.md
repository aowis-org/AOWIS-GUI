# Personal DVMDB designation and configuration

This page documents the **current GUI source behavior**, rather than an
older plan with a separate manual “Personal Repository” settings form.
AOWIS identifies database repositories by their canonical UUIDv7; a
profile name and endpoint are presentation/configuration, not identity.

## 1. Select the personal database in Projects

Open **Projects**, select a configured server/database, and use the
**Server Inspector → Use as my personal working database** checkbox.
There is one designated personal source at a time. The GUI verifies
repository identity and role. Selecting a reachable `unconfigured`
repository may explicitly configure its role as `private`; a `shared`
repository cannot silently become a personal writable repository.
A source with unverified/pending identity cannot be selected.

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

The advanced branch inspector action **Personal working copies…** handles
branch-level checkout, draft creation/reopen and editing. A straightforward
whole-project **Clone to personal database** action has **not** been
implemented; it must not be described as a current GUI button.

## 4. Session and recovery contract

Each writable draft is tied to its intended personal repository UUIDv7,
working-copy UUIDv7, pinned base, writer lease/fencing token and expected
persisted generation. A second session cannot override the same draft
without an authoritative writer transition. A missing personal repository
must leave saved/queued work recoverable, not fall back to a shared source.

The GUI's durable pending-event queue is separate from the DVMDB's stored
candidate. A queue entry is retired only after authoritative confirmation,
including loss-of-acknowledgement recovery. Save does not create a Commit or
change the branch's committed HEAD. Standalone acceptance of the full
edit/restart workflow is still outstanding.

Repository role designation is **not authentication**. External writable
HTTP endpoints and the Standalone loopback listener still require an
appropriate deployment environment. Security architecture is deferred
outside this implementation roadmap.

See [quickstart](dvmdb-quickstart.md),
[Save/recovery](personal-working-copy-save.md), and
[roadmap](dvmdb-integration-roadmap.md).
