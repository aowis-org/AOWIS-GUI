# Map HUD localization

The language button at the right of the main toolbar shows the current
language code. Choose English, Français, العربية, Deutsch or Español.
The GUI restores the selection at startup from the native GUI INI or
browser local storage. English is the default and supplies the fallback
text for untranslated messages. Arabic selects right-to-left widget layout.
The language preference is independent of the unit profile and numeric locale.

The shipped catalogues cover the main navigation, selected toolbar and
Projects controls, Settings categories, map modes and map HUD strings.
Navigation tooltips resolve on hover. Map labels and controls update in
place when Qt delivers a language change. Additional inspector forms,
editor tools and dialogs still contain English text.

## Responsibility and catalogue contexts

| Source / API | Owns presentation strings | Translation context |
| --- | --- | --- |
| `MapBgfxHud` (Render, `map_bgfx_hud.*`) | Renderer HUD chrome: downloads, activity/plurals, cancel, view toggles, tooltips, compass, joystick, underground controls and legend palette menu | `MapBgfxHud` |
| `MapBgfxHudTextCache` (Render) | **No strings**: renders caller-supplied Unicode, caches by text + font and updates only changed atlas regions | None |
| `MapBgfxGlobeHudController::fillHudState()` | GUI: camera/tooltips and unit-dependent slider text | `MapBgfxGlobeHudController` |
| `mapBgfxLegendContent()` | GUI: legend categories and model-driven metric, palette, unit labels | `MapBgfxLegendContent` (category prefixes) |
| GUI metric descriptors, units, palette descriptors, scale provider | GUI: presentation data passed into renderer | Their GUI-local contexts, to be audited during multilingual rollout |

Renderer source strings use `Q_DECLARE_TR_FUNCTIONS(MapBgfxHud)` (no `QObject`
required), plus `QT_TRANSLATE_NOOP` for compass abbreviations. **The
translation context stays `MapBgfxHud`**, wherever the file lives. GUI-originated
strings are translated before passing them to the renderer; do not import any
GUI `LanguageManager`, `MapModel`, hydraulic model, or unit-profile dependency
into the renderer. The `translations/` directory contains separate `aowis_render_*.ts` and
`aowis_gui_*.ts` Qt Linguist sources. Both corresponding QM resources are
embedded for native and WebAssembly builds. The GUI CMake build regenerates adjacent `.qm` catalogues from changed `.ts`
files when Qt LinguistTools (`lrelease`) is installed. The compiled `.qm` files
are Qt resources used at runtime on Linux and WebAssembly. If the host Qt kit
lacks `lrelease`, CMake warns and leaves the last shipped `.qm` in place; install
LinguistTools and rebuild before testing new translation entries. Always
commit both `.ts` and regenerated `.qm` files when preparing a release.

## Runtime language-change contract

1. The GUI installs both matching catalogues through `QTranslator` and sets
   the application layout direction from the selected language.
   Qt sends `QEvent::LanguageChange` to widgets.
2. `MapBgfxSurface::changeEvent(LanguageChange)` forwards to `MapBgfxMapView::handleHostChange()`, which notifies the `MapBgfxHud`
   (`languageChanged()`), requests a redraw and emits
   `localizedPresentationRefreshRequested()`. It never reinitializes bgfx,
   reloads map data, resets the camera, or closes a popup. A font change also
   updates the HUD font, rather than merely scheduling a redraw.
3. `MapMonitorContainer` responds to the signal by refreshing the *display*
   legend via `refreshBgfxLegend()`, not re-resolving hydraulic symbology.
   That refresh is skipped in editor/basemap modes, which have no monitor
   legend; the normal monitor activation path regenerates it when returning.
4. The next actual HUD frame calls `tr()` again, measures the new text,
   rebuilds layout/hitboxes and requests only new text runs from the atlas.
   Globe slider state is regenerated each frame from `fillHudState()`.
   Existing cached English runs may remain until ordinary atlas eviction.
5. For GPU-on-demand surfaces, the surface redraw request ensures a frame
   even if neither the map nor camera changes. Tooltips restart their delay
   without losing a pressed control or open legend popup.

Keep language names/localization in the GUI toolbar, not in GPU/device code.
Numeric scientific input and Model persistence must stay language independent.
Qt Arabic shaping/bidirectional text is available in `QPainter` text runs,
but the HUD's manually placed controls, text clipping and popup geometry
still require explicit Arabic/RTL usability testing in the multilingual phase.
Do **not** mirror map coordinates, network geometry, camera rotation or compass
semantics when enabling RTL widget layout.

## Regression coverage

Render's `aowis-render-bgfx-hud` test (`tests/bgfx_hud_test.cpp`) is CPU-only and constructs a subclassed synthetic
`QTranslator` without `.qm` resources. Its language test checks:

- Stable renderer and GUI translation contexts, including `%n` plural counts.
- English → synthetic translation → English fallback in the same HUD.
- Longer labels generate new atlas runs without atlas reset, and redraw
  hitboxes remain clickable.
- Open legend popup survives a language-change notification.
- Qt text-cache accepts an Arabic Unicode text run without a GPU.

Run it in a Render checkout:

```bash
./compile_linux_tests.sh
ctest --test-dir build-linux-tests --output-on-failure -R '^aowis-render-bgfx-hud$'
```

This does **not** replace manual native/WebAssembly testing of
`LanguageChange` propagation to actual map surfaces, tooltip positioning,
Arabic shaping/fallback fonts and complete RTL interactions. Full-language
translations, unit-independent locale formatting and UI-wide lazy refresh
belong to the GUI multilingual phase.

## Persistent GUI presentation

The GUI language selector does not reconstruct the main window. Existing
widgets respond to Qt's language-change event, and stateful controls refresh
their presentation from model-independent source labels:

- `MainWindow` recalculates its personal-draft editing action label using the
  live session state and pending-count (no session close or undo mutation).
- `ProjectsWidget` calls `ProjectRevisionGraph::retranslateCaptions()` to
  update both canonical and visible graph node captions **without rerunning
  DAG layout, changing the camera, collapsing history, or fetching a database**.
  The revision tree updates existing item titles and tooltips in place.
- Open working-copy and simulation-results dialogs update their static
  labels, tabs and controls without closing; the working-copy dialog's
  verified network summary is regenerated from retained data without HTTP.
  Simulation summary rows refresh from the loaded timeline; the current tab,
  selected summary row and expansion state are retained. Diagnostics retain
  their selected index and scroll position.
- Map size sliders translate their tooltip descriptions from stable English
  source keys on `LanguageChange`; changing language never changes slider
  numeric values. Headloss and water-quality tooltips are refreshed without
  changing checked simulation options.

The translator loader rolls back any partially installed new catalogue if a
translator cannot be installed. Arabic sets Qt's RTL layout direction, but
custom HUD/toolbar mirroring and clipping still require runtime acceptance.
User-entered text and external DVMDB/backend messages must not be translated
by guessing at their contents; the UI localizes only its own presentation.

### Tests and residual coverage

`aowis-gui-language` exercises new non-English catalogue contexts alongside
existing persistent settings, inspector state and History dock tests.
`aowis-project-revision-graph` verifies repeated synthetic translation and
English restoration without moving the graph camera or selection. These are
headless tests: native Linux and WebAssembly manual verification is still
required, particularly for Arabic text shaping and the custom bgfx HUD.

This does **not** yet assert complete live-retranslation coverage of every
AOWIS screen. Some transient network-operation status/error messages remain
as displayed until superseded by the next operation (to avoid hiding backend
error details), and not all feature modules have complete translated catalogues.
The acceptance target remains full in-place switching without restart or
lost model/editor state; any untranslated application-owned text is a coverage
gap, not an acceptable restart workaround.

## Hydraulic Libraries: live presentation contract

The Hydraulic Libraries page has eight translated collection names, displayed
through a `QComboBox` whose **index is the collection identity**. On language
changes, its existing item texts are replaced with signals blocked: the active
collection, current UUID selection, list contents, and logical edit capture
remain untouched. Library entry IDs, UUIDs, and numerical source values are
never translated; the editor deliberately retains dot-decimal input.

The modal Add/Edit library dialog updates its window title, labels, numerical
format hint, Save/Cancel controls and subsequent validation messages using
`HydraulicLibrariesWidget` translation keys. Because a language change is
received by the **already open dialog** inside `exec()`, it must not recreate
any input widget, accept/reject the dialog, or mutate the transaction. User
entered text and numerical drafts survive English ↔ any supported language.

`aowis-hydraulic-libraries-widget` exercises a synthetic translator against
both the existing collection selector and an open dialog, asserting no logical
edit, lost draft, or selection signal. `aowis-gui-language` separately checks
real catalogue entries for all five supported languages. Both tests must run
with Qt 6; a passing catalogue test alone does not demonstrate UI refresh.

## Hydraulic Controls and map labels: live presentation contract

The Controls & Map Labels page translates its two collection names without
repopulating the selector, changing its enum index, emitting selection signals
or changing the loaded network. Rule and map-label IDs remain user data.

The rule, premise, action and map-label modal editors use the stable GUI-owned
`HydraulicControlsWidget` translation context. They update window titles,
form labels, enum captions, reference prefixes, clause summaries and
Save/Cancel buttons when Qt delivers `LanguageChange` inside an already-running
modal `exec()` loop. Enum ordinals, UUID item data, partially entered numeric
values, cursor positions and unsaved rule actions are never recomputed or
replaced in response to the language event. Reference selectors update their
**display captions** while retaining the UUID used on save. Rule summary list
items are updated in place, keeping their current row.

Transient validation/deletion dialogs use the active GUI catalogue when opened.
Inputs continue to use unambiguous, dot-decimal storage values; changing GUI
language is not an engineering-unit or numeric-locale conversion.

`aowis-hydraulic-controls-widget` tests an English → synthetic translation →
English switch on the existing selector, an open map-label modal, and nested
rule/premise modals while preserving edits, selections and zero new autosave
transactions. `aowis-gui-language` checks representative translations from
the four compiled catalogues. These tests do not replace native or WebAssembly
verification of Arabic RTL, font shaping and window manager behaviour.

## Hydraulic Entity Inspector: simulation presentation

The Entity Inspector keeps its current entity, active tab, editing controls and
unsaved values during a language switch. UI-derived simulation captions,
pump/valve status words, diagnostic severity/context labels, energy-result
fallbacks, water-quality fallback messages, and the demand summary are
regenerated from the **existing in-memory** hydraulic results on Qt
`LanguageChange`. This refresh path does not reload a network, replace the
inspector, modify the MODEL, or generate an autosave transaction.

`EntityInspectorPresentation` is the stable GUI-owned translation context for
these strings. Free-form simulator/backend messages, entity names, UUIDs,
pattern names and numeric input values remain unmodified. Hydraulic-demand
summary counts use Qt numerus forms (including Arabic plural categories)
while number formatting still follows the independently configured unit
profile. Static tooltips, date 'not set' labels and endpoint actions also
refresh in place. The `aowis-gui-language` test verifies representative
compiled-catalogue simulation strings and a retained two-demand summary
across all five languages. This is not yet an integration test that creates a
fully populated live Inspector or a final Arabic RTL acceptance test.

## Hydraulic Entity Inspector: terrain and Source Trace presentation

The terrain lookup button, its in-progress label, the last returned elevation,
DEM metadata (dataset, source resolution, vertical datum) and calculation
help text now refresh from the **retained raw lookup result** on Qt
`LanguageChange`. Changing language does not request terrain data again,
change the canonical elevation, reset an active request, or commit an edit.
An already-open terrain-error dialog retranslates its application-owned title,
summary and action caption; free-form service-provided error details remain
untouched.

Source Trace's current-origin display, missing-node fallback, action tooltips
and confirmation dialog use `EntityInspectorPresentation`. The confirmation
stays open during a language switch and dynamically regenerates the node
names and button text; its accept/cancel roles and the source-origin UUID do
not change. The no-entity-specific water-quality descriptions also update in
place. Source-type combo entries were already backed by `LiveUiText` and keep
their original enum values and current selection.

The `aowis-gui-language` test covers the newly compiled catalogue entries
and retained terrain/UUID presentation through all five languages. A direct
GUI integration test for an open inspector with an in-flight DEM request and
live confirmation dialog is still required before declaring comprehensive
runtime-localization acceptance.
