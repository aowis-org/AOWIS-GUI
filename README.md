# AOWIS-SERVER-GUI (*AOWIS Controller*)

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

**Current limits (source review 2026-11-04):** MODEL capture and private
Save transport cover all 18 hydraulic collections and persistent root
metadata, including mixed root/entity transactions. Complete mutation-path,
external HTTP/Standalone restart and browser/WASM acceptance remain open.
General import replacement, whole-project Clone/Fetch, Publish and Merge UI
flows are unfinished. HEAD/Revision detail Tags/Description still share
branch-metadata persistence rather than independent revision annotations.

## History and Undo/Redo

The toolbar's **Open revision history in Projects** action opens a left
**History dock** without leaving Distribution Editor. For a verified private
working-copy session, the dock offers **Undo**, **Redo**, and direct selection
of saved actions, archived Commit actions and eligible Commit checkpoints.
Original MODEL actions are retained in DVMDB after Commit and expandable in
pages of 50. Commit markers are checkpoints, **not Undo steps**: navigation
reconstructs a private draft at a chosen action boundary; immutable revisions
and the committed branch HEAD remain unchanged.

DVMDB is authoritative for the cursor, leases, expected generation and
navigation eligibility. After accepted navigation the editor reloads the
saved canonical network before resuming edit capture. The current adapter
supports one existing hydraulic network based on a committed revision;
first/unborn-draft action history and multi-network navigation are not yet
supported, although DVMDB can still store multiple networks per revision.
The dock currently obtains Commit markers from the Projects preview, so an
incomplete ancestry preview can also limit which older actions are shown.
**End-to-end GUI/server restart and cross-Commit acceptance remains pending.**
See [Using History](docs/history.md), the
[History contract](docs/history-contract.md),
[remaining History work](docs/history-roadmap.md), and
[verification status](docs/verification-status.md).

Start with [documentation index](docs/README.md),
[DVMDB quickstart](docs/dvmdb-quickstart.md),
[Projects workspace](docs/projects-workspace.md),
[graph layout](docs/project-graph-layout.md),
[Save/recovery](docs/personal-working-copy-save.md),
[network import semantics](docs/network-open-import-replacement.md),
the [remaining integration roadmap](docs/dvmdb-integration-roadmap.md),
and the [History roadmap](docs/history-roadmap.md).

The Projects revision graph is drawn with **QWidget/QPainter**; it does not
create a second bgfx device next to the map renderer.

## Hydraulic Distribution Editor

The **Distribution Editor** works with one hydraulic network in both the 2D map
and the 3D terrain globe. Entity placement, snapping, pipe bends, attachments,
selection and movement use the shared editing workflow. In 3D, terrain-relative
node edits update terrain height while preserving offsets; absolute elevations
are preserved. Through-terrain picking remains available. The 3D **Delete
Selected** control is currently disabled.

See the [Distribution Editor guide](docs/distribution-editor.md) for controls,
terrain and elevation behavior, editing limitations and relevant tests.

## Git submodules

Clone the repository with its dependencies, or initialize them after cloning:

```bash
git clone https://github.com/aowis-org/AOWIS-SERVER-GUI.git
cd AOWIS-SERVER-GUI
./git_submodule_init.sh
```

`git_submodule_init.sh` initializes all submodules: AOWIS-SERVER-EPANET,
AOWIS-SERVER-MODEL and GeographicLib (recursively), and the bgfx rendering
libraries `bgfx`, `bx` and `bimg`. It checks out the exact revisions recorded
by the AOWIS-SERVER-GUI repository; it does not update them to their latest
upstream commits.

The bgfx libraries are fetched **shallow** (only the pinned commit) and checked
out **sparse** (`tools/bgfx_sparse_checkout.sh`): only the headers and sources
AOWIS builds, ~19 MB instead of ~270 MB. AOWIS builds them with its own small
CMake file (`cmake/aowis_bgfx.cmake`), not with `bgfx.cmake`.
`./git_submodule_init.sh --shader-tools` additionally checks out the bgfx shader
compiler sources, which are only needed to regenerate the committed shaders
(see `src/map/bgfx/README.md`, "bgfx build").

To **update** submodules to their remote-tracking branches, use:

```bash
./git_submodule_update.sh          # AOWIS submodules and GeographicLib only
./git_submodule_update.sh --bgfx   # bgfx, bx and bimg only
./git_submodule_update.sh --all    # Both groups
./git_submodule_update.sh --help   # Usage
```

The committed bgfx shader binaries (`src/map/bgfx/shaders/bin`) must match the
bgfx revision. After `--bgfx`, run `./compile_linux.sh --shader-tools` and commit
the regenerated shaders together with the new `external/bgfx`, `external/bx`
and `external/bimg` gitlinks.

**First-time setup of the bgfx patch:** ZIP files cannot contain Git submodule
gitlinks. If the bgfx-group entries exist in `.gitmodules` but have not yet
been registered in Git, either `git_submodule_init.sh` or
`git_submodule_update.sh --bgfx` will register them automatically. Review and
commit the new gitlinks alongside `.gitmodules`, so subsequent clones can
initialize all dependencies normally.

## Build

**Compiler requirement:** C++20 (a compiler with C++20 support, for example GCC 12 or newer on Debian 12+). The GUI, its tests, and the AOWIS Model/EPANET components use C++20. CMake enforces this requirement across native and WebAssembly builds.

### Linux
Run the script `compile_linux.sh`

Qt's private Gui headers are required (Debian/Ubuntu: `qt6-base-private-dev`): the bgfx map and the Wayland orbit pointer lock read the native window and display handles through them.

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

Native builds create the configuration file automatically on first startup. The default location is platform-specific:

```text
Windows: %APPDATA%\aowis-server-gui\aowis-server-gui.ini
Linux:   ~/.local/share/aowis-server-gui/aowis-server-gui.ini
macOS:   ~/Library/Application Support/aowis-server-gui/aowis-server-gui.ini
```

On Windows, `%APPDATA%` normally resolves to `C:\Users\<user>\AppData\Roaming`. Existing configurations from the previous `AppData\Local` location are migrated automatically when the roaming file does not exist. On Linux, `XDG_DATA_HOME` is respected when it is set. The default file contents are:

```ini
[gui]
examples_builtin_enable=true

[shortcuts]
sidebar_toggle=Win+Tab
fullscreen=F11
simulation_run=Ctrl+R
simulation_run_alternate=Shift+Enter
map_zoom_in=E
map_zoom_out=Q
map_pan_up=W
map_pan_down=S
map_pan_left=A
map_pan_right=D
map_provider_arcgis_sat=F1
map_provider_openstreetmap=F2
map_provider_opentopomap=F3
map_provider_cycloosm=F4
map_editor_select=Esc
map_editor_delete=Del
map_editor_add_pipe=1
map_editor_add_junction=2
map_editor_add_valve=3
map_editor_add_demand_point=4
map_editor_add_pump=5
map_editor_add_tank=6
map_editor_add_power_source=7
map_editor_add_reservoir=8
map_editor_add_note=9

[map_server]
base_url=http://aowis-server-map.localhost:80
api_key=
delete_api_key=
```

Restart the application after editing the file. Missing native `[shortcuts]` entries are added automatically using the advertised UI shortcuts.

Set `examples_builtin_enable=false` to hide bundled example entries. If
the option is missing, built-in examples are enabled. Example Library
actions do not create persisted DVMDB revisions.

### DVMDB database profiles

The Projects **Databases** section stores named profiles in the native
INI under `[servers]` as `profiles` (serialized JSON). In the WebAssembly
build, profiles are stored in browser `localStorage` under
`aowis.servers.profiles`, not in the deployed read-only INI file.

Use the GUI inspector to create or edit profiles rather than hand-editing
their JSON. The stored fields include repository UUIDv7 (or a temporary
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
new or changed files under `AOWIS-SERVER-GUI/`. Apply a patch from this
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
