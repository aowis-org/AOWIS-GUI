# Interface localization and live language switching

AOWIS-GUI supports **English, French, Arabic, German and Spanish** on native
Qt Widgets and WebAssembly. Select the language in the main toolbar. Switching
languages changes the existing interface **in place**; it does not recreate the
main window, map, editor, renderer or DVMDB session. Arabic uses right-to-left
(RTL) interface layout; map coordinates and navigation semantics remain the same.
AOWIS-GUI-Mobile has its own Qt Quick HUD and is **not** a consumer of the
renderer HUD described here.

This document describes the current GUI/Render contract and how to maintain it.
It is not a per-patch implementation history.

## Language ownership and persistence

`src/config/gui_language.{h,cpp}` (`GuiLanguage`) is the GUI language authority.
Its five language codes are `en`, `fr`, `ar`, `de` and `es`. English is the
source language and fallback. The chosen language is stored in the native GUI
INI (`interface/language`) or browser `localStorage`
(`aowis.gui.language`). The selection is independent of engineering-unit
profiles, numeric formatting and persisted hydraulic MODEL values.

On selection the GUI loads **both** translation domains:

| Catalogue | Presentation owner | Stable Qt context examples |
| --- | --- | --- |
| `translations/aowis_gui_{fr,ar,de,es}.ts` | MainWindow, Projects, editor, inspectors, Settings, simulation dialogs and GUI-supplied map labels | `MainWindow`, `ProjectsWidget`, `ProjectSourceReader`, `ProjectSourceWriter`, `EntityInspectorPresentation` |
| `translations/aowis_render_{fr,ar,de,es}.ts` | bgfx map HUD text and tooltips supplied by Render | `MapBgfxHud` |

`.ts` sources are compiled into `.qm` resources for the application, including
WebAssembly. CMake regenerates `.qm` with Qt LinguistTools (`lrelease`); when
shipping a catalogue change, rebuild and commit the corresponding generated
`.qm` files. The GUI installs both domains through `QTranslator`. It does not
load GUI translation internals from Render or put MODEL/domain state into the
renderer library. If the requested catalogue cannot be loaded, language
activation fails instead of knowingly installing only a partial language.

Qt translation contexts are **not inferred from filenames**. QObject-derived
classes without `Q_OBJECT` can inherit `QObject::tr()`; use an explicit
`Q_DECLARE_TR_FUNCTIONS(ClassContext)` (with the following `public:` restored,
if the public API follows) or a deliberate `QCoreApplication::translate()`
context. `ProjectSourceReader` and `ProjectSourceWriter` use their dedicated
contexts; the writer's free-function validation uses an explicit context.

## Live-update contract

Changing language must modify **presentation only**. Application code updates
current widget captions in response to `QEvent::LanguageChange` (and physical
placement in response to `QEvent::LayoutDirectionChange`). A component may
regenerate text from retained model/results data, but must not use language
switching to:

- reset the map camera or view mode, restart bgfx, reload DEM/imagery or
  re-fetch hydraulic data;
- close an editor or dialog, change an entity/branch/revision selection,
  change enum or UUID identities, or lose an unsaved numeric/text draft;
- create a hydraulic edit, enqueue autosave, commit, publish, retry HTTP or
  restart an in-flight DVMDB operation;
- reinterpret backend error messages, user-defined names or raw identifiers as
  interface strings.

Presentation examples implemented in the GUI:

| Surface | In-place behaviour |
| --- | --- |
| Main toolbar / navigation / footer | Existing actions, simulation state, counts, tooltips and custom toolbar groups update; RTL mirrors their physical placement |
| Projects | Existing tree captions and revision-graph labels refresh without redoing graph layout, camera or selection; unchanged built-in demonstration descriptions are retranslatable, user edits remain data |
| Project source and private working-copy dialogs | GUI-generated status/validation text uses dedicated contexts; retained progress state can retranslate while the request continues; server messages remain unchanged |
| History, Libraries, Controls, Entity Inspector | Current labels, result summaries, selectors and open form captions update without changing in-progress edits, collection identity or current history cursor |
| Map Monitor and editor | Translated legends, renderer status, tooltips, map controls, confirmation dialogs and context menus update from retained state |
| Hydraulic entity tables | Column labels and categorical display values are translated at render time; canonical values, sorting, filters and selection are retained |
| Unit-profile menus | Built-in names/descriptions and menu actions update while open; profile IDs, custom names and numeric editors do not change |

For Qt controls that do not automatically retranslate programmatically
assigned captions, use their existing explicit language-event binding rather
than reconstructing the widget. When text comes from an enum or operation
state, retain that state and translate at display time; do not reverse-map a
previously translated string. For already-open modal confirmations, update the
captions but preserve their button connections and pending operation.

## Render HUD, Arabic layout and hit testing

The GUI hosts `MapBgfxMapView` through `MapBgfxSurface`.
`MapBgfxSurface::changeEvent()` forwards host language/layout events to Render
and requests a frame even with a stationary camera. It provides the widget's
current `layoutDirection()` to Render on each HUD frame. The renderer
retranslates its own HUD chrome; GUI-owned text, including legend metrics and
engineering units, is translated **before** it reaches the renderer.
`MapMonitorContainer` can regenerate its display legend from retained
symbology ranges independently of an asynchronous Render refresh callback.

Render's `MapBgfxHud` builds text layouts and clickable regions together.
Its Unicode text cache takes caller-supplied strings and font information;
it does not own translation keys. On Arabic/RTL changes, it mirrors the
**interface**, not the world:

- view actions are right-anchored; joystick/zoom controls left-anchored;
- the legend moves to the left and its popup labels, alignment and scrollbar
  follow RTL; globe controls and popups use mirrored screen placement;
- text and tooltips are viewport-bounded; actions and their hitboxes move
  together; an already-open legend popup remains open across RTL/LTR changes.

Map geometry, globe azimuth, camera orbit, physical compass direction, pipe
coordinates and hydraulic values are not mirrored. Rendering an Arabic text
run is supported via Qt text shaping/font selection; glyph fallback and
clipping still depend on the fonts and browser/runtime actually used.

## Automated verification

Use the normal build gates in **both** repositories when changing the Render
HUD, then the GUI. Run the GUI test suite again after updating the Render
gitlink:

```bash
# AOWIS-GUI-Render checkout
./compile_linux_tests.sh
ctest --test-dir build-linux-tests --output-on-failure -R '^aowis-render-bgfx-hud$'

# AOWIS-GUI checkout, with that Render revision checked out
./compile_linux_tests.sh
python3 tools/audit_gui_localization.py --check-all --require-all-contexts
```

The dedicated Render HUD test checks synthetic live translation, Unicode text
cache behaviour, RTL control/legend positions, hit targets and popup
preservation. GUI tests cover actual Qt widgets, compiled language catalogues,
RTL top-toolbar placement, inspector/Projects state and unit-profile switching.
These do not substitute for visual inspection of native GPU and browser text.

At the current source snapshot (9 November 2026), the strict GUI source audit
finds **1,728** recognized translation keys, **zero** missing in each translated
language, **zero** unmapped ordinary `tr()` calls and **zero** suspicious simple
literal-widget setters. Each GUI `.ts` catalogue contains **1,942** entries.
`--details` shows source locations when investigating a new string.

The audit checks source patterns and verified contexts, not arbitrary runtime
string construction, text supplied by plugins/backends, glyph shaping, or
whether a widget actually responds to a language change. A zero-missing audit
is therefore a **source-inventory gate**, not a complete visual/usability
certification. For dated build evidence, see [Verification status](verification-status.md).

## Native and WebAssembly acceptance

The desktop and WebAssembly builds have been reported as working after the
RTL integration. Before a release, visually check a real 2D map, 3D globe,
Projects selection, an open editor with an unsaved draft, a simulation panel
and an open HUD legend while switching EN → FR → AR → DE → ES → EN.

In particular, verify Arabic font fallback, text clipping/overlap, popup and
scrollbar placement, all clickable hit targets, and return to LTR at small
and large viewport sizes. Preserve camera, network state, selected entity,
current timeline and numeric drafts. Also verify the browser's loaded `.qm`
resources and saved language after reload. These are **release checks**; the
reported build success alone does not establish every interaction above.

## Adding or changing interface text

1. Identify the owning component and Qt translation context. Mark static
   literals with `tr()` or an explicit context, never by translating a user
   value or backend response.
2. Add all four language entries to the correct `aowis_gui_*` or
   `aowis_render_*` catalogues, preserving `%1`, `%n`, markup and RTL plural
   forms. Translate operations from their retained state so live switching
   does not replay them.
3. Update the **existing** widget on `LanguageChange`; keep stable IDs and
   current editing state. When Render HUD geometry changes, update layout
   and hit testing in the same frame.
4. Run the source audit and both build/test gates as applicable. Regenerate
   and commit `.qm` resources. Check native and browser layouts when text
   wrapping, RTL or custom-rendered HUD placement changes.

AOWIS-GUI-Mobile is outside this HUD contract; it renders its own Qt Quick HUD.
