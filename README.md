# AOWIS-GUI (*AOWIS Controller*)

[EPANET](https://github.com/OpenWaterAnalytics/EPANET) based Controller for Water Networks.

This is the GUI for the AOWIS-SERVER infrastructure.

This software is written in C++ with Qt and can be compiled for all major platforms, including WebAssembly.

## Projects workspace and DVMDB

The **Projects** workspace connects to actual DVMDB repositories and shows
server/project cards, immutable revision cards and **writable HEAD cards**
for branches. There are no independent branch cards. A new project creates
an unborn `main` HEAD without an initial synthetic commit; the Project
Inspector can create additional independent unborn branches. Shared
revisions appear only once in the graph.

The **Database Inspector** configures connections and designates one verified
♛ **personal working database** (gold graph card). Open the chosen HEAD
card, then **Overview → Open network** to load its saved private working
copy in Distribution Editor. Captured changes autosave to the private DVMDB
draft under a fenced session; **Save is not Commit**. The HEAD tab offers
**Commit** with a required message to create an immutable revision without
automatically publishing it elsewhere.

Network Overview also provides the selected saved-network summary, **Import
network** (only an untouched, empty unborn private draft) and **Export
network** (exact selected draft or immutable revision). Opening an immutable
revision offers read-only Monitor or creation of a branch for editing. There
is no Inspector **Delete network** button. Standalone embeds the DVMDB HTTP
service inside the GUI process.

**Current scope (5 November 2026):** Canonical MODEL capture and private
Save cover all 18 hydraulic collections and persistent root metadata.
A new, `unconfigured` DVMDB may accept project metadata writes without
silently becoming `shared`; direct editing requires a designated private
personal working server. Whole-project checkout, Fetch/Publish and merge
reconciliation are incomplete GUI workflows. The distribution editor's full
mutation matrix, live server restart and browser/Standalone acceptance remain
separate verification gates.

## History and Undo/Redo

The toolbar's **Open revision history in Projects** action opens a left
**History dock** without switching editor mode. It follows the selected
branch's first-parent Commit ancestry independently of the Projects graph
preview; original MODEL actions are expandable in bounded pages. Undo, Redo
and eligible direct jumps select an authoritative action boundary. A Commit
marker is a checkpoint, **not** another Undo step. History never rewrites
immutable revisions or the committed branch HEAD.

DVMDB verifies the selected model and returns a **sparse MODEL delta** with
canonical SHA-256 digests. The GUI checks entity before-images, membership
moves and the resulting digest before replacing its cached editor model. A
normal navigation does **not** download a complete `NetworkHydraulic`.

If a navigation acknowledgement is lost, the GUI retries the **same**
operation UUID, reconciles the precise generation/cursor and SHA-256, and can
replay the pinned delta after a long outage. The GUI now also persists a
checksummed, bounded navigation intent **before** the POST. If it restarts
while that operation is uncertain, it can resume without first downloading
the whole model. A normal fresh open without a pending intent still loads
the authoritative model once. The remote personal REST server must be
reachable for server-side editing; recovery does not create an offline
local authority.

History navigation currently targets one unambiguous hydraulic network,
including eligible actions after initializing an unborn first draft. Exact
unborn deletion ordering can remain unsupported. Multi-network **storage**
is not restricted. Automated crash/restart test code exists; real process-
kill, storage-failure and WebAssembly persistence acceptance remain open.

See [Using History](docs/history.md),
[History interaction](docs/history-contract.md),
[History recovery internals](docs/history-recovery.md), and
[verification status](docs/verification-status.md).

Start with [documentation index](docs/README.md),
[Projects workspace](docs/projects-workspace.md),
[personal working server configuration](docs/working-repository-configuration.md),
[Save/Commit recovery](docs/personal-working-copy-save.md),
[remaining integration work](docs/dvmdb-integration-roadmap.md), and
[History acceptance](docs/history-roadmap.md).

## Hydraulic Distribution Editor

The **Distribution Editor** works with one hydraulic network in both the 2D map
and the 3D terrain globe. Entity placement, snapping, pipe bends, attachments,
selection and movement use the shared editing workflow. In 3D, terrain-relative
node edits update terrain height while preserving offsets; absolute elevations
are preserved. Through-terrain picking remains available. The 3D **Delete
Selected** control is currently disabled.

See the [Distribution Editor guide](docs/distribution-editor.md) for controls,
terrain and elevation behavior, editing limitations and relevant tests.

## Map and rendering

The map (2D and the 3D terrain globe, imagery, terrain, network, labels, HUD)
is drawn by **AOWIS-GUI-Render**, the AOWIS map engine, pinned as a Git
submodule at `external/AOWIS-GUI-Render` and shared with AOWIS-GUI-Mobile.
The GUI hosts it in one persistent Qt Widgets surface (`MapBgfxSurface`)
for all Maps modes and supplies the hydraulic network, its symbology, the
editor overlay, settings and the map server connection (REST, or the
in-process server of the Standalone build). The map uses bgfx on every
platform (WebGL 2 in the browser) and has no CPU fallback.

The native Linux Projects revision graph also uses bgfx by default on X11 and
Wayland, sharing the one process-wide bgfx device with the map; QPainter is
its fallback (`AOWIS_PROJECTS_BGFX_GRAPH=0`). Two extra read-only 2D map
windows can be opened for diagnostics with `AOWIS_BGFX_THREE_VIEW_PROBE=1`.

See [Render integration](docs/render-integration.md),
[Rendering architecture](docs/rendering-architecture.md), the
[Maps workspace](docs/map-workspace-architecture.md), the
[map host guide](src/map/bgfx/README.md) and Render's own `README.md` and
`docs/`.

After updating the Render gitlink, build and test with the strict gate:

```bash
./git_submodule_update.sh --render && \
./tools/verify_render_checkout.sh && \
./compile_linux_tests.sh
```

`./compile_linux_tests.sh` builds and links the **complete application and
all test executables before CTest**; tests never start after a failed build.

## Git submodules

Clone the repository with its dependencies, or initialize them after cloning:

```bash
git clone https://github.com/aowis-org/AOWIS-GUI.git
cd AOWIS-GUI
./git_submodule_init.sh
```

`git_submodule_init.sh` initializes all submodules at the revisions this
repository records: AOWIS-GUI-Render, AOWIS-NETWORK, AOWIS-CONFIG,
AOWIS-SERVER-EPANET, AOWIS-SERVER-MODEL and GeographicLib (recursively), and
the bgfx libraries `bgfx`, `bx` and `bimg`. It does not update them to newer
upstream commits. `./git_submodule_init.sh --render` initializes only Render;
`--network` and `--config` likewise initialize only AOWIS-NETWORK or
AOWIS-CONFIG.

The bgfx libraries are fetched **shallow** (only the pinned commit) and checked
out **sparse** (`tools/bgfx_sparse_checkout.sh`): only what AOWIS builds,
~19 MB instead of ~270 MB. Render builds them from these checkouts with its
own small CMake file (`cmake/aowis_bgfx.cmake`), not with `bgfx.cmake`, so
the process contains exactly one bgfx. Keep their revisions equal to the ones
Render pins: Render's committed shaders are compiled for them.
`./git_submodule_init.sh --shader-tools` also checks out the shader compiler
sources, needed only to regenerate those shaders.

To **update** submodules to their remote-tracking branches:

```bash
./git_submodule_update.sh                 # MODEL, EPANET, Render, NETWORK and CONFIG
./git_submodule_update.sh --render        # Render only (plus its pinned nested submodules)
./git_submodule_update.sh --config        # AOWIS-CONFIG only (likewise --network)
./git_submodule_update.sh --model         # likewise --epanet, --epanet-deps, --geographiclib
./git_submodule_update.sh --bgfx          # bgfx, bx and bimg only
./git_submodule_update.sh --all           # everything
./git_submodule_update.sh --help          # usage
```

After `--bgfx`, run `./compile_linux.sh --shader-tools` and commit the
regenerated shaders in AOWIS-GUI-Render together with Render's new bgfx
gitlinks, then update the GUI's Render gitlink.

**bgfx submodule setup:** ZIP files cannot contain Git submodule gitlinks.
If the bgfx entries exist in `.gitmodules` but are not yet registered in Git,
`git_submodule_init.sh` or `git_submodule_update.sh --bgfx` registers them.
Review and commit the new gitlinks with `.gitmodules`.

## Build

**Compiler requirement:** C++20 (a compiler with C++20 support, for example GCC 12 or newer on Debian 12+). The GUI, its tests, and the AOWIS Model/EPANET components use C++20. CMake enforces this requirement across native and WebAssembly builds.

### Linux
Run the script `compile_linux.sh`

Qt's private Gui headers are required (Debian/Ubuntu: `qt6-base-private-dev`): the map's native window capture (Render's `BgfxQtNativeSurface`) and the Wayland orbit pointer lock read the native window and display handles through them.

Find the result in the folder `build-linux`

### WebAssembly
The WebAssembly compiler is fully self-contained in an AOWIS-owned Docker image. It does not depend on a prebuilt third-party Qt/Emscripten image.

`compile_wasm.sh` uses the pinned versions from `tools/qt-emscripten/toolchain_versions.sh`. On the first build it automatically runs `prepare_wasm_docker.sh`, which starts from Ubuntu, installs the matching Emscripten SDK, builds same-version Qt host tools, and builds Qt for WebAssembly with thread support (`-feature-thread`). The image also performs a small Qt Widgets/Network/QThread smoke build before it is accepted.

The default toolchain is Qt 6.10.2 with Emscripten 4.0.7. Re-run `prepare_wasm_docker.sh` manually whenever the Docker definition or pinned toolchain versions change. Building the image compiles Qt from source and is therefore a one-time heavyweight operation; normal `compile_wasm.sh` runs reuse the local Docker image.

Your system user needs to be allowed to run docker, or run the script as root.

Adding your system user to the group `docker`, and log out and in again should be enough:
```
usermod -aG docker [USERNAME]
```

Find the result in the folder `build-wasm`.

The WebAssembly build is a `Release` build. Two switches are taken from the environment of `compile_wasm.sh`:

- `AOWIS_WASM_SIMD` (default `ON`): compiles the GUI with WebAssembly SIMD (`-msimd128`), so the map's vertex generation runs vectorized. Every browser with WebGL 2 supports it (Chrome/Edge 91+, Firefox 89+, Safari 16.4+); `AOWIS_WASM_SIMD=OFF ./compile_wasm.sh` builds a module for browsers without it.
- `AOWIS_WASM_FUNCTION_NAMES` (default `OFF`): keeps function names in the module for readable browser stack traces (a few MB larger download).

The WebAssembly executable starts with 256 MiB of linear memory (the `QT_WASM_INITIAL_MEMORY` target property in `CMakeLists.txt`). This accommodates Qt, bgfx and the embedded resources at link time; Qt's default 50 MB is too small for this application. Memory growth remains enabled by the Qt/Emscripten toolchain so large projects can use more memory at runtime. This setting affects only the WebAssembly target and does not require rebuilding the Docker toolchain image.

To run the result, you need to have a web server set up. Threaded WebAssembly additionally requires a secure context and the `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` response headers. The generated webroot includes an `.htaccess` with these headers for Apache deployments that allow overrides.

### Windows / macOS
Usually as a Desktop-Client, you might want to prefer [AOWIS-SERVER-Standalone](https://github.com/aowis-org/AOWIS-SERVER-Standalone).


## GUI configuration

Configured DVMDB connections (Projects → servers), the map server
connection (Settings → Map: base URL and API keys), custom unit profiles
(collection `unit-profiles`, see [docs/units.md](docs/units.md)) and WireGuard profiles
(Settings → WireGuard VPN, see [docs/wireguard-vpn-settings.md](docs/wireguard-vpn-settings.md))
are stored with [AOWIS-CONFIG](https://github.com/aowis-org/AOWIS-CONFIG) in
the GUI's owner root, one record per object in the `connections` and
`wireguard` collections. WireGuard and map-server records contain secrets
(private keys, API keys); they are private to your user but not encrypted:

```text
Linux:   ${XDG_CONFIG_HOME:-~/.config}/aowis/gui/connections/<uuidv7>.json
Windows: %LOCALAPPDATA%\aowis\gui\connections\<uuidv7>.json
Browser: localStorage keys aowis-config:/aowis/gui/connections/<uuidv7>.json
```

Connection records carry a `service_type`: `dvmdb` (one per configured
database) or `map-server` (at most one; without it the GUI uses
`http://aowis-server-map.localhost:80` and no keys). The map server only
supports `{"type":"direct"}` transport; any other transport disables it
rather than connecting directly. In WebAssembly builds the map server comes
from the deployment's `aowis-server-gui.ini` (see below) and nothing is
stored in the browser.

The former `servers/profiles` INI key and the browser's
`aowis.servers.profiles` item are no longer read; the former native
`[map_server]` INI section is deleted on startup without migration
(re-enter the map server connection once in Settings → Map). In the browser only one
tab per origin can change connections at a time (the others show a save
error until that tab is closed).

Preferences are records of the `preferences` collection of the same store,
one per group, natively and in the browser:

| Group | Keys |
| --- | --- |
| `interface` | `language` |
| `shortcuts` | one per shortcut (`sidebar_toggle`, `fullscreen`, …); `""` unassigns; missing → default |
| `symbology` | `node_palette`, `link_palette`, `heatmap_palette` (names such as `viridis`), `*_palette_flipped` |
| `map_navigation` | `scroll_zoom_sensitivity`, `mouse_3d_pan_sensitivity`, `orbit_3d_sensitivity` (0.1–1.5) |
| `map_performance` | `terrain_detail_reduction` (0–3) |
| `gui` | `examples_builtin_enable` (native; WebAssembly takes it from the deployment) |

Change them in Settings; nothing is written until a value is changed, and
invalid values fall back to the defaults. The former INI groups `[gui]`,
`[shortcuts]`, `[symbology]`, `[map_navigation]`, `[map_performance]` and
`[interface]` and the browser's `aowis.symbology.*`, `aowis.map_*` and
`aowis.gui.language` items are deleted on startup, not migrated.

Until they move as well, the unit-profile selection, map provider fallbacks
and the commit author identity still use the former INI file:

```text
Windows: %APPDATA%\aowis-server-gui\aowis-server-gui.ini
Linux:   ~/.local/share/aowis-server-gui/aowis-server-gui.ini
macOS:   ~/Library/Application Support/aowis-server-gui/aowis-server-gui.ini
```

Built-in examples are enabled unless `examples_builtin_enable` is `false`.
Example Library actions do not create persisted DVMDB revisions.

### DVMDB database profiles

The Projects **Databases** section stores named profiles as `dvmdb`
records of the `connections` collection (see above), in the browser too;
never in the deployed read-only INI file.

Use the GUI inspector to create or edit profiles rather than hand-editing
their records. The stored fields include repository UUIDv7 (or a temporary
UUIDv7 for an unverified connection), name,
`connection_type` (`remote` or `local`), remote endpoint or local data
directory, a descriptive authentication mode and the visibility flag.
**Authentication choices do not implement authentication.** The current
DVMDB HTTP service has no authentication or TLS, and the GUI rejects
non-`None` modes when connecting. Do not embed passwords in URLs.

In supported native Standalone builds, the default **Local database** profile
is provisioned automatically with its own per-user DVMDB directory
(`aowis-standalone-dvmdb`). It is separate from the HTTP daemon's
`aowis-dvmdb` root; do not configure two live processes to use the same
root. GUI-only and WebAssembly builds do not provide embedded native DVMDB.
For practical connection instructions and limits, see
[the DVMDB quickstart](docs/dvmdb-quickstart.md).

bgfx is the only map renderer (2D and 3D globe, desktop and WebAssembly), so there is no renderer setting any more. The application removes an old `map_desktop_renderer` line from the native configuration file; an old `map_wasm_renderer` line in a WebAssembly deployment is ignored. If bgfx cannot start, Map Monitor shows a status message.

WebAssembly builds render through WebGL 2. If the GPU renderer cannot start, Map Monitor shows a status message, as on the desktop. Map Editor renders with bgfx as well, as on the desktop.

The Maps page layout, renderer ownership boundaries and opt-in migration
regression diagnostics are documented in
[`docs/map-workspace-architecture.md`](docs/map-workspace-architecture.md).

The bgfx map renders on demand: a frame is drawn only when the camera, the viewport, the content or an animation changes, so an idle map costs no CPU or GPU time in the browser. While the map is only panned, the editor's icons and the map labels are not rebuilt for every frame: they are built once with a margin of about a fifth of the screen around the visible area and then moved with the map, and built again only when the pan leaves that margin (zooming, resizing and editing rebuild them as before). Map tiles are decoded by the browser's own native image decoders (`createImageBitmap` on a dedicated worker, pixels written straight into the GPU's pixel format) and handed to WebGL without a further copy, into a few large atlas textures, so all visible map tiles are drawn with a single draw call; browsers without `OffscreenCanvas` in workers, and the first tiles after start, use Qt's decoders on worker threads instead. The browser canvas uses multisampling only below a device pixel ratio of 1.75 (on HiDPI screens the pixels are small enough without it, and there are four times as many) and requests the high-performance GPU on systems with two. `compile_wasm.sh` preserves an existing `build-wasm-dist/aowis-server-gui.ini`.

WebAssembly builds include `aowis-server-gui.ini` beside the generated `index.html`. Set `examples_builtin_enable`, `base_url`, `api_key`, and `delete_api_key` there and reload the page. The browser first loads the configuration from the same deployment directory as `index.html`, so subdirectory deployments work correctly; `/aowis-server-gui.ini` at the origin root remains a compatibility fallback. Existing administrator-edited files in `build-wasm` and `build-wasm-dist` are preserved across rebuilds. The normal API key is used for tile requests; the delete API key is used only for tile-cache deletion requests.

## Map providers and personal fallback chains

The REST map server owns the raster provider catalog: stable UUIDs, display
names, URL templates, enabled state, zoom ranges and download policies. In
**Settings → Map Settings → 2D Map Providers**, edit those properties directly;
AOWIS submits server-provider edits automatically shortly after a change. The
**Add provider** and **Remove selected** buttons edit the catalog; **Reload from
server** retrieves its current state (discarding any unsent table edits). The
server must be reachable before provider editing becomes available. Failed
submissions retain the edits in the table and show an error, so they can be
corrected and retried. There is no provider Save button.

**My fallback** is independent of the server catalog. Each user's fallback
selection takes effect immediately and is saved locally in the native GUI's
`aowis-server-gui.ini`, under `[map_provider_fallback]` by provider UUID.
Selecting a fallback never changes the server's provider definitions. A new
provider's fallback is saved once its UUID has been accepted by the server.
WASM builds do not currently persist these personal fallback preferences.

Fallbacks **cascade**: if A supports zoom 0–17, B 0–18 and C 0–20, choosing
A → B and B → C uses A through 17, B at 18 and C at 19–20. Every hop must
extend the previous maximum zoom without leaving a gap. The fallback dropdown
only offers enabled providers that meet that condition. This strictly
increasing zoom limit also prevents circular fallback chains. If there is no
valid next provider, the current highest-resolution tiles are smoothly enlarged
instead of requesting unsupported higher-zoom tiles from upstream.

The map layer picker displays all enabled sources announced by
`GET /map/v1/providers`; F1–F4 still select the four built-in sources when
available. Startup uses built-in provider entries until discovery succeeds.
Map server **connection** fields are saved when editing finishes; a GUI restart
is required before the changed connection takes effect. Map control and
performance settings save as you adjust them. The server stores catalog edits
in its own persistent state, leaving its administrator-owned `/etc` INI
untouched. For deployment ownership and HTTP API details, see the map-server
README. The provider-editing API currently has no authentication.

## Applying patch ZIPs and refreshing project snapshots

GUI patches use the `aowis-server-gui-*.zip` filename convention and contain
new or changed files under a top-level folder named like this checkout
(`AOWIS-SERVER-GUI/` for a default clone, `AOWIS-GUI/` for a checkout in a
folder of that name). Files listed in `<folder>/.aowis-patch-delete.txt` are
removed. Apply a patch from this
repository with:

```bash
./tools/apply_patch.sh /path/to/aowis-server-gui-change.zip
```

Applying replaces the included files, refreshes their timestamps for
CMake/Ninja, and automatically runs the ZIP snapshot scripts for whichever
repositories the patch touched (`gui.zip`, `model.zip` and/or `epanet.zip`).
Snapshots are created next to the GUI checkout. Patch application does **not**
compile or test the application, and a snapshot error does not roll back
already-extracted files.

**See [Patch ZIP workflow and snapshot regeneration](docs/apply-patches.md)**
for detailed path rules, making ZIP patches, submodule caveats, excluded
assets, failure recovery, and the standalone test command.
