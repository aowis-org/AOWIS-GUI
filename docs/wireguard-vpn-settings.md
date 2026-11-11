# WireGuard VPN settings

**Settings → WireGuard VPN** is the native AOWIS GUI's central place for
managing reusable WireGuard configuration drafts.

## Current behavior

- The profile list supports **New**, **Import .conf**, **Edit**, **Save changes**,
  **Revert** and **Delete**. The right pane has **Configuration**, **Used by**
  and **Status** tabs. Both profile names and the complete INI-like WireGuard
  configuration are editable. An imported file is copied into the editor; its
  path is not used as an identifier or retained by this page.
- The plain-text `.conf` editor highlights section headers, recognized interface
  and peer directives, values, comments, and malformed lines. Unrecognized
  directives are differentiated without rewriting or rejecting their contents.
  Highlighting adapts to light/dark Qt palettes and works on desktop and Qt
  WebAssembly (although the VPN settings page itself is disabled on WASM).
- Configurations are syntax-checked for sections and required interface/peer
  keys. This is **not** a cryptographic validation or a network test. The GUI
  can represent multiple peers; current AOWIS-NETWORK probe support may have
  stricter limitations when it is integrated.
- Profiles have a stable generated ID. Service references and runtime snapshots
  are associated with this ID, not the profile name.
- **Used by** describes saved *service configurations* referencing a profile,
  not connections or sockets. The live **Status** tab represents independently
  supplied tunnel state, active consumers, traffic, peers and handshakes.
  Nothing is represented as online or assigned until supplied by a real adapter.
- Deleting a profile with known service references is refused. Renaming does
  not change its ID. The editor warns about unsaved changes.
- On **WebAssembly**, the Settings navigation entry remains visible but disabled,
  with a tooltip explaining that the browser cannot supply the native UDP/TUN
  networking capabilities required by the embedded WireGuard client.

## Boundaries of this GUI-only implementation

**This page is still session-local and is not yet persisted**; it is moved
onto AOWIS-CONFIG next. The page displays a persistent notice to that effect.

**DVMDB connections already use stored profiles.** Projects → Connection →
WireGuard selects a profile from the GUI's AOWIS-CONFIG store
(`aowis/gui/wireguard/<uuidv7>.json`, mode 0600) or the direct connection;
**Import WireGuard profile…** copies a `.conf` file's text into that store
(the path is not kept). A connection references its profile by ID
(`transport.profile_id` of its `connections` record). AOWIS-NETWORK's
`parseWireGuardConf()` checks the file on import, refusing what the embedded
engine cannot run (it supports exactly one interface address and one peer),
and again whenever a tunnel starts. A connection whose profile is missing or
unusable stays closed; it never falls back to direct HTTP.

The widget provides `setServiceReferences(profile_id, services)` and
`setTunnelStatus(profile_id, snapshot)` to support central service discovery and
WireGuard runtime monitoring when those components are connected. A profile
without snapshots explicitly displays **Service usage unavailable** and
**Status unavailable** rather than fabricated zeroes or a fake `Stopped` state.

The editor necessarily displays private keys while the user edits them. The
page does not copy them to the clipboard, log them, or save them to QSettings.

## Testing

`aowis-wireguard-vpn-settings-tests` checks the editor, session-local CRUD,
renaming via stable IDs, snapshot views, deletion blocking for known consumers,
the absence of invented runtime status, syntax formatting, and palette changes.
