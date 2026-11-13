#ifndef GUI_CONFIGURATION_H
#define GUI_CONFIGURATION_H

#include <QKeySequence>
#include <QString>

#include "network/network_symbology.h"

class QKeyEvent;

struct GuiShortcutConfiguration
{
    QString sidebar_toggle = QStringLiteral("Win+Tab");
    QString fullscreen = QStringLiteral("F11");
    QString map_monitor_fullscreen = QStringLiteral("Ctrl+F11");
    QString simulation_run = QStringLiteral("Ctrl+R");
    QString simulation_run_alternate = QStringLiteral("Shift+Enter");

    QString map_zoom_in = QStringLiteral("E");
    QString map_zoom_out = QStringLiteral("Q");
    QString map_pan_up = QStringLiteral("W");
    QString map_pan_down = QStringLiteral("S");
    QString map_pan_left = QStringLiteral("A");
    QString map_pan_right = QStringLiteral("D");
    QString map_provider_arcgis_sat = QStringLiteral("F1");
    QString map_provider_openstreetmap = QStringLiteral("F2");
    QString map_provider_opentopomap = QStringLiteral("F3");
    QString map_provider_cycloosm = QStringLiteral("F4");

    QString map_editor_select = QStringLiteral("Esc");
    QString map_editor_delete = QStringLiteral("Del");
    QString map_editor_add_pipe = QStringLiteral("1");
    QString map_editor_add_junction = QStringLiteral("2");
    QString map_editor_add_valve = QStringLiteral("3");
    QString map_editor_add_demand_point = QStringLiteral("4");
    QString map_editor_add_pump = QStringLiteral("5");
    QString map_editor_add_tank = QStringLiteral("6");
    QString map_editor_add_power_source = QStringLiteral("7");
    QString map_editor_add_reservoir = QStringLiteral("8");
    QString map_editor_add_note = QStringLiteral("9");
};

struct GuiSymbologyPaletteConfiguration
{
    NetworkSymbologyPalette node_palette = NetworkSymbologyDefaultNodePalette;
    bool node_palette_flipped = false;
    NetworkSymbologyPalette link_palette = NetworkSymbologyDefaultLinkPalette;
    bool link_palette_flipped = false;
    NetworkSymbologyPalette heatmap_palette = NetworkSymbologyDefaultHeatmapPalette;
    bool heatmap_palette_flipped = false;
};

struct GuiMapNavigationConfiguration
{
    // Multipliers applied to the built-in map navigation response.
    double scroll_zoom_sensitivity = 0.5;
    double mouse_3d_pan_sensitivity = 0.8;
    double orbit_3d_sensitivity = 0.8;
};

struct GuiMapPerformanceConfiguration
{
    // DEM levels the globe terrain stays below the imagery
    // (MapGlobeTerrainLevels). 0 draws every elevation sample the terrain
    // source provides at the imagery's level of detail: one mesh cell per DEM
    // cell, about 4 px on screen. Each further level halves the terrain
    // resolution (about 8, 16, 32 px per cell) with a quarter of the
    // triangles. Close to the ground, past the source's native detail, every
    // setting shows the full source resolution. Range 0 to 3
    // (MapGlobeTerrainMaximumDetailReduction).
    int terrain_detail_reduction = 1;
    // Multisample antialiasing of the bgfx map and project graph back buffer
    // (desktop; WebAssembly: the browser antialiases the canvas). Samples:
    // 0 = off, 2, 4, 8, 16 (bgfx_antialiasing.h); the GPU's highest
    // supported level below the request is used.
    int antialiasing_samples = 4;
};

struct GuiConfiguration
{
    bool examples_builtin_enable = true;
    GuiSymbologyPaletteConfiguration symbology_palettes;
    GuiShortcutConfiguration shortcuts;
    GuiMapNavigationConfiguration map_navigation;
    GuiMapPerformanceConfiguration map_performance;
};

// The former native GUI INI, deleted at startup (tests use it to plant old
// files); in WebAssembly the deployment's read-only webroot INI.
QString guiConfigurationFilePath();
// Preferences, read once from AOWIS-CONFIG and cached; the save functions
// below update both. The first call retires the former INI storage.
const GuiConfiguration &guiConfiguration();
// The GuiPreferences group of keyboard shortcuts (keys: config_key).
QString guiShortcutPreferenceGroup();
QKeySequence guiShortcutKeySequence(const QString &shortcut);
bool guiShortcutMatches(const QKeyEvent *event, const QString &shortcut,
                        Qt::KeyboardModifiers allowed_extra_modifiers = Qt::NoModifier);
bool saveGuiNodeSymbologyPalette(NetworkSymbologyPalette palette, bool flipped);
bool saveGuiLinkSymbologyPalette(NetworkSymbologyPalette palette, bool flipped);
bool saveGuiHeatmapSymbologyPalette(NetworkSymbologyPalette palette, bool flipped);
void applyGuiMapNavigationConfiguration(const GuiMapNavigationConfiguration &configuration);
bool saveGuiMapNavigationConfiguration(const GuiMapNavigationConfiguration &configuration);
bool saveGuiMapPerformanceConfiguration(const GuiMapPerformanceConfiguration &configuration);

#endif // GUI_CONFIGURATION_H
