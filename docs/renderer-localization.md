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
embedded for native and WebAssembly builds. After editing a TS file in Qt
Linguist, rebuild its adjacent QM using `lrelease file.ts -qm file.qm` and
commit both files.

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
