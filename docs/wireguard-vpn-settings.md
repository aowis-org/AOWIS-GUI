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
- **Status** shows only what a tunnel runtime reports via
  `setTunnelStatus(profile_id, snapshot)`; no adapter is connected yet, so it
  shows **Unavailable** rather than a fabricated state.
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
deletion while referenced, storage deletion, draft revert, and damaged-record
removal.
