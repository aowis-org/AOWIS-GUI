# WireGuard VPN settings

**Settings → WireGuard VPN** manages the GUI's reusable WireGuard profiles.
Profiles are stored by [AOWIS-CONFIG](https://github.com/aowis-org/AOWIS-CONFIG)
in the GUI's owner root, one record per profile:

```text
Linux:   ${XDG_CONFIG_HOME:-~/.config}/aowis/gui/wireguard/<uuidv7>.json   (0600, directory 0700)
Windows: %LOCALAPPDATA%\aowis\gui\wireguard\   (needs the AOWIS-CONFIG Windows backend; not yet implemented)
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
- Validation: on builds with the embedded engine (Linux,
  `AOWIS_GUI_WIREGUARD_PROBE`) the editor runs AOWIS-NETWORK's
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
- **Status** observes `WireGuardRuntimeStatus`, a process-local registry
  updated by `DvmdbConnection` as the AOWIS-NETWORK embedded engine starts,
  runs, fails or stops. It updates immediately when connections change, even
  when Settings is already open. Multiple connections using one profile are
  counted as separate active tunnel instances; they are not pooled yet.
- **Running** means the embedded engine and local SOCKS5 proxy are running;
  it does **not** prove a WireGuard peer handshake. Once an actual DVMDB HTTP
  response arrives through the tunnel the tab reports that a response was received;
  that is evidence of past reachability, not a continuous connectivity check.
  **Not started**, **Starting**, **Stopped**, and **Error** are separate states,
  and the most recent tunnel startup error is shown without keys.
- **Traffic counters and peer handshake timestamps** come from the live
  AOWIS-NETWORK engine when a tunnel is running. Unavailable readings are
  displayed as unavailable, never as invented zeros. `Active tunnels` means
  *live embedded tunnel instances*, not configured service references.
- On **WebAssembly** the navigation entry stays visible but disabled, with a
  tooltip: browsers cannot run the embedded UDP/TUN WireGuard client.

## Connections

Projects → Connection → WireGuard selects a stored profile or the direct
connection, and **Import WireGuard profile…** stores a `.conf` there
directly. A connection whose profile is missing or unusable stays closed and
never falls back to direct HTTP; the selector shows it as **Missing profile**.

## Testing

`aowis-wireguard-vpn-settings-tests` (isolated `XDG_CONFIG_HOME`) checks
highlighting, storing and reopening profiles, rename by stable ID, parser
validation and warnings, revert, "Used by" from connection records, refused
deletion while referenced, storage deletion, draft revert, damaged-record
removal, and live status transitions (starting/running/HTTP confirmed/stopped/
error/retry), including multiple consumers of one profile.

## Live peer telemetry

The Status tab polls the current profile's running embedded WireGuard tunnels
through `WireGuardClient::peerStats()` every two seconds **while the Status tab
is visible**. The monitor retains only counters, handshake timestamps, and
peer endpoints; it never receives keys or the device's raw IPC output.

Received/sent byte totals are cumulative over the lifetime of the active
tunnel instance(s), so they reset after a restart. Displayed rates are
calculated from successive samples (the first sample has no rate yet). The
latest handshake is a historical event, not a continuous online signal; a
successful DVMDB HTTP response is tracked separately. Multiple live tunnels
using the same saved profile have their counters summed.

The GUI requires the updated AOWIS-NETWORK submodule with the peer-stats ABI.
Pin the NETWORK commit in GUI after landing its standalone patch.
