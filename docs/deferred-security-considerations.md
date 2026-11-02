# Deferred AOWIS security considerations (GUI)

**Reference notes only — outside the DVMDB integration and Projects graph
roadmap.** The [current Projects/DVMDB implementation roadmap](dvmdb-integration-roadmap.md)
ends with functional integration and qualification. No VC milestone, acceptance
criterion, test gate or implementation schedule is assigned to security here.
This document records future ideas so they are not lost; **do not implement them
as part of the current repository/graph work** without a new, explicit product
decision. The backend's corresponding independent notes are at
`AOWIS-SERVER-DVMDB/docs/security-permissions-design.md`.

## What the current work means (and does not mean)

- **Personal** and **shared** DVMDB roles describe repository purpose and
  target selection. They are not permission roles or a security boundary.
- A repository UUIDv7 is a persistent identifier, not a login credential or
  proof of ownership, user identity or endpoint authenticity.
- A checked shared database profile must never become a private draft target
  by accident. This is a functional data-integrity invariant independent of
  any eventual access-control architecture.
- Session leases, fencing tokens, expected generations and idempotency keys
  prevent accidental concurrent overwrites or duplicate operations; none of
  them authorizes a user.
- The Commit author is one UUIDv7 per GUI installation plus the operating
  system account name (since 2026-11-02). It labels revisions consistently;
  it is not a verified user identity.
- Standalone's embedded loopback listener (`embedded_dvmdb_host.cpp`)
  accepts any request: no credential, no `Host`/`Origin` check, and DVMDB
  ignores `Content-Type`. Any local process, and a web page in the user's
  browser (simple cross-origin POSTs; DNS rebinding for reads), can reach
  the private repository. Candidate fix for the security concept: a
  per-process secret checked in a pre-routing handler plus `Host`/`Origin`
  checks (review of 2026-11-02).
- Existing GUI config and project/graph features must not claim to secure an
  HTTP DVMDB. For current development, keep unauthenticated writable services
  within controlled deployments; do not expose them to untrusted/public clients.

## Potential future GUI security design questions

1. **Who is the user?** Account/device/session identity, multiple devices,
   account recovery and how a personal repository is associated with a user.
2. **What may they do?** Read/edit/commit/publish permissions for projects,
   drafts, branches, history, replication, maintenance and administration;
   distinct personal and shared repository policies.
3. **How do clients authenticate?** Desktop, Standalone and browser/web login,
   service-to-service identity, token lifecycle, logout, rotation and revocation.
4. **Where are secrets stored?** Never treat an ordinary INI setting as a secret
   vault. Consider OS keyrings, secure stores, browser-specific limitations,
   and safe logging/error display when requirements are defined.
5. **How are remotes trusted?** HTTPS/TLS policy, endpoint verification,
   redirect behavior, wrong-repository detection and credential forwarding.
   A UUID comparison by itself does not authenticate a remote.
6. **What can others see?** Prevent cross-user draft/history disclosure,
   information leaks via graph metadata, activity status and error details.
7. **How is security explained?** Account/permission controls, sharing UX,
   clear denied-action messages and safe recovery without implying that
   "private" means cryptographically or administratively protected.
8. **How is it verified?** Threat modeling, negative authorization tests,
   audit and incident/revocation stories, cross-user and multi-tenant testing
   in all supported client modes.

## Explicit non-goals for the active DVMDB/GUI roadmap

No login UI, credential storage, ACL editor, permission-aware graph filtering,
authenticated publishing, audit UI, OAuth/TLS work or penetration-test release
gate is introduced by VC-00–VC-11. These may require a separate AOWIS-wide
design and new roadmap **only when explicitly requested**. Do not block graph,
private working-copy, commit, fetch, publish or merge implementation on an
unapproved security subsystem.
