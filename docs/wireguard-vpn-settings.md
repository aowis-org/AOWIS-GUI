# WireGuard VPN settings

**Settings → WireGuard VPN** manages the GUI's reusable WireGuard profiles.
Profiles are stored by [AOWIS-CONFIG](https://github.com/aowis-org/AOWIS-CONFIG)
in the GUI's owner root, one record per profile:

```text
Linux:   ${XDG_CONFIG_HOME:-~/.config}/aowis/gui/wireguard/<uuidv7>.json   (0600, directory 0700)
Windows: %LOCALAPPDATA%\aowis\gui\wireguard\<uuidv7>.json   (private DACL, AOWIS-CONFIG Windows backend)
```

A record holds the profile's name, description and the **exact** `.conf`
text, including its private key. Files are private to the user account but
**not encrypted**; treat copies and backups of the configuration as secret.

## Behavior

- **New** and **Import…** create unsaved drafts; **Save changes** stores them
  (import copies the file's text; the path is never kept). **Revert** discards
  edits or an unsaved draft. **Delete** removes the stored record.
- Names are unique (case-insensitive). Renaming keeps the profile's ID, so
  connections referencing it are unaffected.
- Validation: on builds with the embedded engine (Linux and Windows,
  `AOWIS_GUI_WIREGUARD_PROBE`; Windows links the engine archive prebuilt by
  Standalone's `compile_windows.sh`) the editor runs AOWIS-NETWORK's
  `parseWireGuardConf()`, the same parser that starts tunnels. Profiles it
  rejects cannot be saved; its first error is shown with its line. Ignored
  wg-quick directives (`PostUp`, `Table`, …) are warnings: saveable, never
  executed. Other builds only check text integrity (non-empty, ≤ 2 MiB, no NUL).
- The syntax highlighter is presentation only and adapts to light/dark
  palettes.
- **Used by** lists the configured connections (Projects → Connection →
  WireGuard) whose `transport.profile_id` references the profile. It refreshes
  whenever the page is shown. It counts configuration, not running tunnels.
- **Delete** is refused while any connection references the profile. The
  check runs inside the same AOWIS-CONFIG owner session as the delete, so no
  other AOWIS instance can assign the profile in between; if any connection
  record cannot be read, deletion is refused as well.
- Damaged profile records (unreadable, or not a WireGuard profile) are
  reported above the list; **Remove damaged…** deletes them after
  confirmation, except records written by a newer AOWIS version or still
  referenced by a connection.
- Tabs: **Status**, **Configuration**, **Used by**. The editor hides each
  `PrivateKey` value behind a mask by default; **Show/edit private key**
  reveals it. Hidden keys are restored for validation and saving, so other
  lines can be edited and saved with the key hidden; copying while hidden
  copies the mask. Every newly shown or saved profile starts hidden.
- **One tunnel per profile:** all connections using a profile share one
  embedded tunnel (two tunnels with the same key would make the peer roam
  between them). It starts with the first connection and stops when the last
  one closes; profile edits apply from the next start.
- **Status** observes `WireGuardRuntimeStatus`, a process-local registry
  updated by `DvmdbConnection`. **Used by now** lists the connections using
  the tunnel. **Running** means the engine and its local SOCKS5 proxy run;
  peer health comes from engine telemetry, polled once per second while the
  page is visible: last handshake, encrypted traffic and throughput, peer
  endpoint, keepalive, uptime and relayed connections. Hints flag a missing
  first handshake (after 15 s), a handshake older than 3 minutes with
  keepalive on, an idle tunnel without keepalive, and failed in-tunnel
  connections. The most recent start error is shown without keys.
- **Reconnect:** wireguard-go retries handshakes itself. A host-name
  endpoint is re-resolved after 135 s without a handshake (multi-address
  failover, or the new address if the name moved). A failed tunnel start is
  retried by Projects after 5, 10, 20, 40, then every 60 seconds.
- On **WebAssembly** the navigation entry stays visible but disabled, with a
  tooltip: browsers cannot run the embedded UDP/TUN WireGuard client.

## Connections

Projects → Connection → WireGuard selects a stored profile or the direct
connection, and **Import WireGuard profile…** stores a `.conf` there
directly. A connection whose profile is missing or unusable stays closed and
never falls back to direct HTTP; the selector shows it as **Missing profile**.

For a WireGuard connection, everything that talks to that server goes
through its tunnel: the catalogue and health checks, metadata writes,
private checkouts (identity, branch and bundle reads from the source), and,
when it is the personal server, every draft, lease, save and bundle import of
`PersonalWorkingCopyClient`. A checkout from a WireGuard source whose
connection is not open fails; nothing falls back to direct HTTP.

**Settings → Map → Map server → WireGuard** does the same for the map
server (record `service_type` "map-server", same `transport` format). Its
tunnel starts on first use and stays up while the connection uses that
profile. Changing any map-server field applies at once: switching profile or
to direct releases the old tunnel lease (the pool stops the tunnel when no
other connection uses it), and new requests use the new connection while
requests already in flight finish on the previous one. Tiles, terrain, elevation, the
provider catalog and the footer health check wait for it and then use its
SOCKS5 proxy on their own network managers; requests made while it starts
are queued, and a failed start fails them and is retried after 5, 10, 20, 40,
then every 60 seconds. It appears under **Used by** and **Used by now**, and
blocks deleting its profile. A profile ID that no longer exists, or a build
without the embedded engine, leaves the map server disabled — never direct.

## Testing

`aowis-wireguard-vpn-settings-tests` (isolated `XDG_CONFIG_HOME`) checks
highlighting, storing and reopening profiles, rename by stable ID, parser
validation and warnings, revert, "Used by" from connection records, refused
deletion while referenced, storage deletion, draft revert, damaged-record
removal, and live status transitions (starting/running/HTTP confirmed/stopped/
error/retry), multiple connections sharing one profile, sampled telemetry and
health hints, and the hidden private key (default hidden, editing other lines,
toggling with unsaved edits, changing the key).
