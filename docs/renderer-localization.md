# Renderer HUD localization boundary

**Current behavior.** The production bgfx map HUD is ready for Qt translation
catalogues without owning language selection. AOWIS-GUI has **not** implemented
the language selector, shipped translated catalogues, Arabic RTL layouts, or a
`LanguageManager`. Those belong to the later multilingual GUI implementation.
This contract must be preserved if the renderer is extracted into AOWIS-RENDER.

## Responsibility and catalogue contexts

| Source / API | Owns presentation strings | Translation context |
| --- | --- | --- |
| `MapBgfxHud` (`map_bgfx_hud.*`) | Renderer HUD chrome: downloads, activity/plurals, cancel, view toggles, tooltips, compass, joystick, underground controls and legend palette menu | `MapBgfxHud` |
| `MapBgfxHudTextCache` | **No strings**: renders caller-supplied Unicode, caches by text + font and updates only changed atlas regions | None |
| `MapBgfxGlobeHudController::fillHudState()` | GUI: camera/tooltips and unit-dependent slider text | `MapBgfxGlobeHudController` |
| `mapBgfxLegendContent()` | GUI: legend categories and model-driven metric, palette, unit labels | `MapBgfxLegendContent` (category prefixes) |
| GUI metric descriptors, units, palette descriptors, scale provider | GUI: presentation data passed into renderer | Their GUI-local contexts, to be audited during multilingual rollout |

Renderer source strings use `Q_DECLARE_TR_FUNCTIONS(MapBgfxHud)` (no `QObject`
required), plus `QT_TRANSLATE_NOOP` for compass abbreviations. **Do not change
`MapBgfxHud` as a translation context when moving files.** GUI-originated
strings are translated before passing them to the renderer; do not import any
GUI `LanguageManager`, `MapModel`, hydraulic model, or unit-profile dependency
into the renderer. Future catalogue split: `aowis-render_*.qm` for
renderer-owned messages and `aowis-gui_*.qm` for application-owned ones. Both
are installed/removed centrally by the GUI.

## Runtime language-change contract

1. The future GUI language manager installs the matching `QTranslator`s in
   `QCoreApplication` and changes application locale/RTL policy separately.
   Qt sends `QEvent::LanguageChange` to widgets.
2. `MapBgfxSurface::changeEvent(LanguageChange)` notifies the `MapBgfxHud`
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

`aowis-bgfx-hud` is CPU-only and constructs a subclassed synthetic
`QTranslator` without `.qm` resources. Its language test checks:

- Stable renderer and GUI translation contexts, including `%n` plural counts.
- English → synthetic translation → English fallback in the same HUD.
- Longer labels generate new atlas runs without atlas reset, and redraw
  hitboxes remain clickable.
- Open legend popup survives a language-change notification.
- Qt text-cache accepts an Arabic Unicode text run without a GPU.

Run (in a configured GUI tests build):

```bash
cmake --build build-linux-tests --target aowis-bgfx-hud-tests -j 8
ctest --test-dir build-linux-tests --output-on-failure -R '^aowis-bgfx-hud$'
```

This does **not** replace manual native/WebAssembly testing of
`LanguageChange` propagation to actual map surfaces, tooltip positioning,
Arabic shaping/fallback fonts and complete RTL interactions. Full-language
translations, unit-independent locale formatting and UI-wide lazy refresh
still belong to the GUI multilingual phase after renderer extraction.
