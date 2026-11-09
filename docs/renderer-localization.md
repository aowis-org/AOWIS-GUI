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
3. `MapMonitorContainer` also handles its own `LanguageChange` by
   refreshing the *display* legend via `refreshHudLegendText()` from retained
   symbology ranges, independently of the Render callback. No hydraulic
   symbology re-resolution or data fetch occurs. In editor/basemap modes the
   monitor legend is not active; normal monitor activation regenerates it.
   `MapBgfxSurface` requests a frame on language or layout-direction changes
   even when the camera is stationary, without reinitializing bgfx.
4. The next actual HUD frame calls `tr()` again, measures the new text,
   rebuilds layout/hitboxes and requests only new text runs from the atlas.
   Globe slider state is regenerated each frame from `fillHudState()`.
   Existing cached English runs may remain until ordinary atlas eviction.
5. For GPU-on-demand surfaces, the surface redraw request ensures a frame
   even if neither the map nor camera changes. Tooltips restart their delay
   without losing a pressed control or open legend popup.

Keep language names/localization in the GUI toolbar, not in GPU/device code.
Numeric scientific input and Model persistence must stay language independent.
Render now takes `MapBgfxSurface::layoutDirection()` through the host
contract on every HUD frame. RTL anchors the view actions to the right, the
joystick and zoom controls to the left, the legend bottom-left and globe
controls bottom-right, and reverses toolbar action order and legend popup
text/scrollbar alignment. Tooltip lines are bounded to the viewport.
The controls and their hit regions are rebuilt together, so an already-open
legend popup survives RTL/LTR switching. Qt `QPainter` provides Arabic
shaping for cached text runs. Physical Arabic font/fallback and browser-canvas
acceptance are still necessary; headless geometry tests do not prove glyph
quality on every system.
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
translator cannot be installed. Arabic sets Qt's RTL layout direction. The
custom `TopControlBarContent` widget places its three groups in mirrored
physical positions on `LayoutDirectionChange` as well as on layout and
language changes. This uses the *same* QWidget and existing children; it
neither clears edit controls nor recreates the toolbar. bgfx HUD mirroring,
Arabic text shaping and clipping still require platform acceptance.
User-entered text and external DVMDB/backend messages must not be translated
by guessing at their contents; the UI localizes only its own presentation.

### Top toolbar RTL and live-switch acceptance

`TopControlBarContent` has manually positioned left, centre and right
`QHBoxLayout` groups; automatic Qt layout mirroring cannot reposition those
groups. Its `placeGroups()` now mirrors physical anchors in RTL (Arabic) and
returns to ordinary LTR positioning immediately when another language is
selected, preserving each group's widget identity and internal data. Existing
center-overlap avoidance is applied using the physical leading/trailing
positions in either direction; no toolbar or child widget is reconstructed.

The headless `aowis-top-control-bar-language` integration test instantiates
**the actual content widget** and the real `GuiLanguage` manager with compiled
catalogues. It checks EN → FR → AR → DE → ES → EN → AR → EN, RTL/LTR
anchors and non-overlap, live label translations, retained QLineEdit draft and
cursor selection, retained combo enum identity, and no edit/selection signals.
It is an actual Qt widget test (not a geometry mock), but does not exercise
the complete MainWindow or hydraulic Entity Inspector. Those end-to-end native
and WebAssembly acceptance tests remain necessary, particularly while a draft
is open in a real editor and with a map/HUD on screen.

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

## Hydraulic Entity Inspector: Pipe and Pump input presentations

The Pump Inspector keeps its EPANET simple-control table and per-row editor
widgets intact when switching languages. Its headers, enabled-control tooltip,
trigger/action options, trigger-node type prefixes, missing-reference labels,
fixed-speed/efficiency choice labels and rule-count summary are display-only
translations. The node's UUID remains stored separately from its translated
caption; the rule count uses Qt plural forms, including Arabic's six forms.
The shared `EntityInspectorText::addFormattedChoice()` helper stores the
untranslated interpolation value independently of the choice's numeric/UUID
identity and uses a signal blocker when refreshing existing combo-box rows.

Pipe geometry, material, roughness formula and chemical-reaction tooltips are
translated from the current configuration, not from cached English display
strings. A language change refreshes those tooltips without deriving new
material coefficients, changing headloss settings, refreshing numeric editors
or emitting hydraulic edit transactions. The active editing mode and canonical
pipe roughness values remain unchanged.

`aowis-gui-language` now verifies real catalogue strings and formatted pump
choices through EN/FR/AR/DE/ES/EN while preserving combo selection and raw
entity identifiers. **Still required before full localization acceptance:**
a full instantiated Pipe/Pump Inspector test, numeric editor suffix review,
manual Arabic RTL/HUD checks, and Linux/WebAssembly runtime confirmation. The
no-restart acceptance criterion is unchanged.

## Hydraulic Entity Inspector: Valve configuration presentation

Valve type names (PRV, PSV, FCV, PBV, TCV, GPV and PCV) are translated
`LiveUiText` combo choices. Their numeric type values remain in `Qt::UserRole`;
changing the language updates the existing item captions with signals blocked,
without changing which valve type is selected or emitting a model edit.

The curve selector distinguishes an application-owned **Select curve** prompt
and a missing-curve caption from user-defined curve names. A missing curve's
UUID is retained separately from the localized caption, even when the list is
repopulated after a hydraulic model change. Existing curve names are never
translated. The active valve setting tooltip is updated by
`refreshValveSettingTooltips()` on `LanguageChange`, rather than by re-running
`onValveTypeChanged()`. This is critical: changing hydraulic types can cancel
pending numeric drafts, while language changes must leave them intact.

`aowis-gui-language` verifies live valve-type/curve-choice captions using all
five actual installed catalogues, plus stable enum values, retained UUIDs and
zero selection-change signals. An instantiated live Valve Inspector test (and
full Arabic RTL/native/WebAssembly acceptance) remains necessary before
claiming complete localization coverage across the editor.

## Hydraulic Entity Inspector: Tank geometry and volume curves

The Tank Inspector's geometry-mode choices already use `LiveUiText`. Its
volume-curve selector now separates UI-owned **Select curve** and **Missing
curve** captions from user-defined curve names and stored UUIDs. A language
change refreshes only those two caption kinds on the existing combo-box rows
with signals blocked; it neither repopulates the curve list nor emits an edit.
The current selection and the curve-signature cache are left intact.

Geometry-dependent editable/derived tooltips retain an untranslated source key
on each numeric editor. `LanguageChange` translates that key in place and
never calls the geometry-mode change handler, re-derives geometry, refreshes
canonical-unit editors or changes the draft being typed. The two water-quality
tooltips (mixing compartment fraction and bulk-reaction coefficient units) use
similar live text bindings. Tank volume computations remain unchanged and
independent of GUI language.

`aowis-gui-language` tests the real compiled EN/FR/AR/DE/ES catalogues with
retained tank curve UUIDs, a user-defined curve name, an existing selected
missing-curve row, and an unmodified numeric editor. A fully instantiated Tank
Inspector integration test and Arabic RTL/native/WebAssembly manual acceptance
are still required before claiming comprehensive localization coverage.

## Real Entity Inspector live-language acceptance

The `aowis-entity-inspector-live-language` CTest launches the actual linked
GUI executable in a special **test-only** headless mode. This occurs before
constructing `MainWindow`, a rendering device, or any map/network service.
It instantiates the production `EntityInspectorTank` with an in-memory
`HydraulicData` model and real compiled GUI/Render translation catalogues.

Across repeated EN → FR → AR → DE → ES switches (and back to English), it
checks the same widget objects, retained text draft and selection, tab, curve
UUID and user-defined name, hydraulic values, translated tooltips and controls,
RTL/LTR application direction, and **zero** model-edit or selection-change
signals. It deliberately tests both visible and hidden inspector text.

Run with `ctest --test-dir build-linux-tests --output-on-failure -R
'^aowis-entity-inspector-live-language$'`. This is a native Linux, offscreen
test; it does not establish full WebAssembly or renderer HUD acceptance.

## Map controls, footer and provider settings

The native footer, map navigation sidebar, icon-size tooltip and map-provider
settings are GUI-owned presentation surfaces. They use the stable
`FooterStatusBar`, `MapNavigationWidget`, `MapSymbologySlider` and
`MapSettingsWidget` contexts in the GUI catalogues. Qt `LanguageChange` updates
these existing widgets in place, without reinitializing the map, resetting
symbology sliders, changing checkbox state, or saving settings.

The footer retains its last displayed WGS84/UTM samples so a language change
updates coordinate captions even while the map is stationary. The zoom spin box
keeps an uncommitted typed value untouched; its translated prefix may be
applied when editing finishes. Coordinate values and unit profiles are not
translated or converted. The navigation sidebar updates its provider shortcut
hints and icon-size slider description without calling view-mode handlers,
which can emit hydraulic-display change signals.

Map-provider settings retain text being edited in the table, existing fallback
selections, provider IDs and pending saves. Only localized caption text,
format hints and locally generated status messages are regenerated. Server
error payloads are retained verbatim. Provider display names, URLs and UUIDs
are data, not translation keys. The table itself is never reconstructed merely
because the language changed.

### Coverage inventory

`python3 tools/audit_gui_localization.py` reports missing explicitly marked
translation keys and suspicious hardcoded UI literal setters. Use
`--details` to inspect file locations; add `--check-context FooterStatusBar`
(or another named context) to fail if that context has missing catalogue keys.
The scanner is conservative: it cannot understand every dynamic string or
infer translation ownership for every use of `tr()`. It is **not** a proof of
application-wide completeness. Qt `lupdate`/`lrelease`, runtime switching,
and native plus WebAssembly layout acceptance are still required. New GUI
strings must be supplied in all four translated catalogues; completed TS
entries alone do not prove that persistent UI updates on `LanguageChange`.

## Explicit-key coverage and presentation-only refresh

The GUI's `tools/audit_gui_localization.py --check-all` checks that every
translation key recognized by the source scanner exists in all four GUI
catalogues. This includes the map-monitor size tooltips, Projects graph and
revision captions, the top toolbar's headloss descriptions, and the
`LiveUiText` contexts used by editor controls. Run this audit whenever
adding or changing translated source strings, then rebuild the `.qm` files
from the `.ts` files using the configured Qt build.

The top toolbar retains simulation-run **state and warning/error counts**
separately from the tooltip text. Live switching rebuilds the tooltip from
that state and retranslates the timeline navigation controls without
starting or stopping simulations, changing the timepoint or restarting
playback. Error and warning counts have proper singular/plural presentation.

Demand-point descriptions and meter-tag placeholders, pipe-allocation
explanations, emitter tooltips and the table filter titles are now updated on
existing widgets. In particular, table numeric filters update *tooltips only*
on language changes, never their ranges or in-progress editor contents.
Open map tile/terrain deletion confirmations and map entity deletion
confirmations retranslate in place without losing their destructive-button
connections. Terrain deletion progress and completion text is regenerated
from retained counts/errors: language changes never schedule a new deletion.

**Audit limitations:** the explicit-key audit does not yet find every literal
used as an `addAction` argument, a programmatically composed status message,
or an external/backend message. `--check-all` passing is an important
catalogue gate, **not** proof that all application-owned strings are already
localized or that all custom WebAssembly and bgfx UI has completed RTL
acceptance. Continue the literal-string inventory and actual native/WASM
language-switch tests before declaring multilingual support complete.

## Persistent map startup status and broader Qt context inventory

`MapMonitorContainer` retains a typed renderer-status reason (`Starting`,
`Failed`, `Stalled`) and the original backend diagnostics separately from the
visible label. On `LanguageChange` it only translates the current status; it
does **not** recreate the renderer, restart the watchdog, discard diagnostics,
or change the map or editing state. The three messages have a dedicated
`MapMonitorContainer` translation context.

The four GUI catalogues also cover previously missing MainWindow private
working-draft/History confirmations and the simulation diagnostics widget's
rich-text labels. Existing `SimulationDiagnosticsWidget::changeEvent()`
already refreshes retained simulation results in place while restoring list
selection and scroll positions; this additional coverage makes that refresh
actually use the selected language instead of falling back to English.

The source audit now recognizes unqualified `tr()` calls from `MainWindow`,
`MapMonitorContainer`, and `SimulationDiagnosticsWidget` as well as its
previously mapped classes; unlike `HistoryDock::tr()` it does not mistake a
**qualified** call for a method of the surrounding class. It reports counts of
**unmapped** `tr()` calls separately. These are not necessarily untranslated,
but `--check-all` cannot claim coverage for them until their owning class and
Qt translation context have been verified. Remaining large interfaces include
Projects, private working-copy workflows and simulation management. The
presence of complete `.ts` catalogs is not equivalent to all strings or
all live-switch paths being audited.

## Current translation-context coverage and private working copies

`tools/audit_gui_localization.py --check-all` validates source strings for
**verified** Qt translation contexts. In addition to explicit `QT_TRANSLATE_NOOP`
and `QCoreApplication::translate`, this includes the `Q_OBJECT`-backed
`HistoryDock`, `PersonalWorkingCopyDialog`, `ProjectRevisionGraph` and
`SimulationStatisticsDialog`, plus the Projects inspector helper classes that
explicitly declare `Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)`. Their C++ file
locations do not determine their Qt translation context.

The personal working-copy dialog retains the source key for a running progress
message (and its optional count argument). `LanguageChange` retranslates the
existing label and forwards the localized presentation text to the status
subscriber without restarting checkout, HTTP requests, leases or draft writes.
External/backend errors remain intact. The standard Close button handles its
own language event after Qt's `QDialogButtonBox` retranslation. The network
summary and draft selection remain based on retained data, not a refresh call.

**Incomplete inventory remains visible:** `--check-all` does not certify any
file whose ordinary `tr()` calls have an unverified context. The remaining
project source reader/writer classes and the personal repository settings
widget do not declare their own Qt translation context via `Q_OBJECT` or
`Q_DECLARE_TR_FUNCTIONS`: inherited `tr()` is not automatically translated in
the C++ subclass's name. Simply inserting translations under guessed class
names would not work. `SimulationManager` and `ProjectsWidget` also retain
unmapped ordinary `tr()` calls requiring the next catalogue inventory pass.
Use `--require-all-contexts` to enforce complete context mapping when that
backlog has been resolved. This is distinct from the app-wide zero-restart
acceptance requirement, which also needs native and WebAssembly verification.

## DVMDB project source error contexts

`ProjectSourceReader` and `ProjectSourceWriter` are QObject subclasses without
`Q_OBJECT`. They explicitly declare `Q_DECLARE_TR_FUNCTIONS` so their
validation, pagination and DVMDB write error messages resolve under their own
Qt contexts rather than silently falling back to `QObject`. The writer's
free-function profile validation uses `QCoreApplication::translate()` with
its explicit `ProjectSourceWriter` context. The errors are generated when an
operation fails, in the currently selected language; switching language does
not restart any in-flight HTTP request or write. Already delivered backend
error strings are not reinterpreted or retransmitted.

The translation audit maps these two verified contexts, with the four GUI
TS catalogues containing every detected key. Its remaining unmapped ordinary
`tr()` calls are in `ProjectsWidget`, `SimulationManager` and the legacy
`PersonalRepositorySettingsWidget`; they remain on the completion roadmap.

`SimulationManager` owns the translation context declared by its `Q_OBJECT`
macro. Its import/export prompts and errors now have four-language catalogue
coverage. Dialogs created after a language change use the currently installed
translator; an already-open native file picker or message box is not recreated
by the language change. These ephemeral dialogs still need targeted live
retranslation acceptance where changing language while a modal is open matters.

The legacy `PersonalRepositorySettingsWidget` also has an explicit Qt
translation context and a `LanguageChange` refresh path. Captions, button
labels, form fields and its retained verification status update in place; the
existing URL/UUID text and an in-flight verification request are preserved.
Its original verification detail is retained as data, never retransmitted.

At this stage, the remaining unmapped ordinary `tr()` calls in the audit
are confined to `ProjectsWidget`; translation-key coverage and live-reflow
of that larger UI remain separate acceptance work.

### Projects workspace translation coverage

`ProjectsWidget` has `Q_OBJECT`, so its unqualified `tr()` calls use the
`ProjectsWidget` context. The source audit maps the complete widget file to
that verified context and `--require-all-contexts` rejects unchecked calls.
Project action/error text, destructive-operation confirmations and demonstration
captions are translated into French, Arabic, German and Spanish, including
Qt plural categories for queued draft edits.

The two built-in demonstration projects are not real DVMDB data. On an active
language change, untouched example names, descriptions, commit messages and
author captions are retranslated from stable source strings; user-modified
preview fields are preserved. Existing tree items and graph node titles update
in place, while the graph retains its selection, zoom, camera and node layout.
The inspector only replaces sample text still matching its previous generated
translation, without triggering a project save. User-entered drafts and real
repository metadata are never translated.

Use `python3 tools/audit_gui_localization.py --check-all --require-all-contexts`
to enforce the mapped source inventory. Passing this check proves coverage of
recognized translation calls, **not** exhaustive localization of dynamic
backend messages or completion of Arabic HUD/WebAssembly usability testing.

### Hydraulic table cell and popup localization

The hydraulic entity tables keep user-entered IDs, names, tags, canonical
quantities, filter bounds and sort values separate from application-owned
categorical display strings. A language change repaints existing table cells and
headers: Yes/No, asset roles, input modes, valve/pump statuses and similar
categorical values are translated at display time from their original source
text. The table model is not reset, rows are not rebuilt, and no hydraulic edit
or server operation is triggered by the language change. Header tooltips also
use the current translation. Newly created context menus are translated, and
already open pipe-vertex menus and confirmation dialogs update their captions
without closing or accepting an operation. These paths work with both native
Qt menus and the WebAssembly popup implementation.

### Unit-profile menus and zero-restart switching

The toolbar, Settings unit selector and the new-profile source menu share
`UnitProfileMenu`. Built-in profile descriptions and menu sections use the
`UnitProfileMenu` Qt catalogue context; custom profile names remain user data.
Language changes retitle the existing native `QAction` instances without
rebuilding the menu, modifying the selected profile ID or refreshing numerical
unit editors. WebAssembly popup buttons receive individual language-change
bindings, including submenu triggers.

The Settings profile selector refreshes its *display labels* in place. Active
create/rename/delete workflows preserve their selected source, original UUID
and unsaved name draft; confirmations update their captions while open. A
source-level zero-missing-key audit is necessary but is not proof of Arabic
HUD visual layout or full WebAssembly runtime acceptance.
