#include "config/gui_configuration.h"

#include <QByteArray>
#include <QDebug>
#include <QKeyEvent>
#include <QString>

#include <cmath>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QtGlobal>
#endif

namespace
{
double validatedNavigationSensitivity(double value, double fallback)
{
    if (!std::isfinite(value) || value < 0.1 || value > 1.5)
        return fallback;
    return value;
}

// Mirrors 0..MapGlobeTerrainMaximumDetailReduction (map_globe_terrain_levels.h);
// hardcoded rather than including the map renderer here for two bounds.
int validatedTerrainDetailReduction(int value, int fallback)
{
    if (value < 0 || value > 3)
        return fallback;
    return value;
}

bool isValidSymbologyPaletteValue(int value)
{
    switch (static_cast<NetworkSymbologyPalette>(value))
    {
        case NetworkSymbologyPalette::Viridis:
        case NetworkSymbologyPalette::Plasma:
        case NetworkSymbologyPalette::Inferno:
        case NetworkSymbologyPalette::Turbo:
        case NetworkSymbologyPalette::CoolWarm:
        case NetworkSymbologyPalette::Cividis:
        case NetworkSymbologyPalette::Magma:
        case NetworkSymbologyPalette::Batlow:
        case NetworkSymbologyPalette::RedBlue:
            return true;
    }

    return false;
}

QKeySequence parseShortcutKeySequence(const QString &shortcut)
{
    QString portable_text = shortcut.trimmed();
    if (portable_text.startsWith(QStringLiteral("Win+"), Qt::CaseInsensitive))
        portable_text.replace(0, 4, QStringLiteral("Meta+"));

    return QKeySequence::fromString(portable_text, QKeySequence::PortableText);
}

#ifndef __EMSCRIPTEN__
QString symbologyPaletteConfigName(NetworkSymbologyPalette palette)
{
    switch (palette)
    {
        case NetworkSymbologyPalette::Viridis:
            return QStringLiteral("viridis");
        case NetworkSymbologyPalette::Plasma:
            return QStringLiteral("plasma");
        case NetworkSymbologyPalette::Inferno:
            return QStringLiteral("inferno");
        case NetworkSymbologyPalette::Turbo:
            return QStringLiteral("turbo");
        case NetworkSymbologyPalette::CoolWarm:
            return QStringLiteral("cool_warm");
        case NetworkSymbologyPalette::Cividis:
            return QStringLiteral("cividis");
        case NetworkSymbologyPalette::Magma:
            return QStringLiteral("magma");
        case NetworkSymbologyPalette::Batlow:
            return QStringLiteral("batlow");
        case NetworkSymbologyPalette::RedBlue:
            return QStringLiteral("red_blue");
    }

    return QStringLiteral("viridis");
}

NetworkSymbologyPalette parseSymbologyPaletteSetting(
    const QString &value, NetworkSymbologyPalette default_palette)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("viridis"))
        return NetworkSymbologyPalette::Viridis;
    if (normalized == QStringLiteral("plasma"))
        return NetworkSymbologyPalette::Plasma;
    if (normalized == QStringLiteral("inferno"))
        return NetworkSymbologyPalette::Inferno;
    if (normalized == QStringLiteral("turbo"))
        return NetworkSymbologyPalette::Turbo;
    if (normalized == QStringLiteral("cool_warm") || normalized == QStringLiteral("cool/warm"))
        return NetworkSymbologyPalette::CoolWarm;
    if (normalized == QStringLiteral("cividis"))
        return NetworkSymbologyPalette::Cividis;
    if (normalized == QStringLiteral("magma"))
        return NetworkSymbologyPalette::Magma;
    if (normalized == QStringLiteral("batlow"))
        return NetworkSymbologyPalette::Batlow;
    if (normalized == QStringLiteral("red_blue") || normalized == QStringLiteral("red/blue"))
        return NetworkSymbologyPalette::RedBlue;

    return default_palette;
}

void ensureSettingDefault(QSettings &settings, const QString &key, const QString &value)
{
    if (!settings.contains(key))
        settings.setValue(key, value);
}

void migrateSetting(QSettings &settings, const QString &legacy_key, const QString &current_key)
{
    if (!settings.contains(current_key) && settings.contains(legacy_key))
        settings.setValue(current_key, settings.value(legacy_key));
    settings.remove(legacy_key);
}

QString loadShortcutSetting(QSettings &settings, const QString &key, const QString &default_value)
{
    const QString value = settings.value(key, default_value).toString().trimmed();
    if (value.isEmpty())
        return QString();
    if (!parseShortcutKeySequence(value).isEmpty())
        return value;

    qWarning() << "Invalid shortcut" << key << "=" << value
               << "- using advertised default" << default_value;
    return default_value;
}
#endif

#ifdef __EMSCRIPTEN__
EM_JS(int, aowisBuiltinExamplesEnabled, (),
{
    const configuration = globalThis.aowisGuiConfiguration || {};
    return configuration.examplesBuiltinEnable === false ? 0 : 1;
});

EM_JS(int, aowisSymbologyPalettePreference, (const char *key, int default_value),
{
    try
    {
        const raw = globalThis.localStorage
            ? globalThis.localStorage.getItem(UTF8ToString(key))
            : null;
        if (raw === null)
            return default_value;
        const parsed = Number.parseInt(raw, 10);
        return Number.isFinite(parsed) ? parsed : default_value;
    }
    catch (error)
    {
        return default_value;
    }
});

EM_JS(int, aowisSymbologyPaletteFlippedPreference, (const char *key, int default_value),
{
    try
    {
        const raw = globalThis.localStorage
            ? globalThis.localStorage.getItem(UTF8ToString(key))
            : null;
        if (raw === null)
            return default_value;
        return raw === "true" || raw === "1" ? 1 : 0;
    }
    catch (error)
    {
        return default_value;
    }
});

EM_JS(int, aowisSaveSymbologyPalettePreference,
      (const char *palette_key, int palette, const char *flipped_key, int flipped),
{
    try
    {
        if (!globalThis.localStorage)
            return 0;
        globalThis.localStorage.setItem(UTF8ToString(palette_key), String(palette));
        globalThis.localStorage.setItem(UTF8ToString(flipped_key), flipped ? "true" : "false");
        return 1;
    }
    catch (error)
    {
        return 0;
    }
});

EM_JS(double, aowisMapPerformanceDoublePreference, (const char *key, double default_value),
{
    try
    {
        const raw = globalThis.localStorage
            ? globalThis.localStorage.getItem(UTF8ToString(key))
            : null;
        if (raw === null)
            return default_value;
        const parsed = Number.parseFloat(raw);
        return Number.isFinite(parsed) ? parsed : default_value;
    }
    catch (error)
    {
        return default_value;
    }
});

EM_JS(int, aowisMapPerformanceIntPreference, (const char *key, int default_value),
{
    try
    {
        const raw = globalThis.localStorage
            ? globalThis.localStorage.getItem(UTF8ToString(key))
            : null;
        if (raw === null)
            return default_value;
        const parsed = Number.parseInt(raw, 10);
        return Number.isFinite(parsed) ? parsed : default_value;
    }
    catch (error)
    {
        return default_value;
    }
});

EM_JS(int, aowisSaveMapPerformancePreference, (int terrain_detail_reduction),
{
    try
    {
        if (!globalThis.localStorage)
            return 0;
        globalThis.localStorage.setItem(
            "aowis.map_performance.terrain_detail_reduction",
            String(terrain_detail_reduction));
        // The QRhi renderer's texture-array batching switch is gone, and the
        // distance-based terrain LOD knobs with it (replaced by the detail
        // reduction).
        globalThis.localStorage.removeItem("aowis.map_performance.array_batching_enabled");
        globalThis.localStorage.removeItem("aowis.map_performance.terrain_lod_target_cell_size_px");
        globalThis.localStorage.removeItem("aowis.map_performance.terrain_max_detail_zoom");
        globalThis.localStorage.removeItem("aowis.map_performance.terrain_full_detail_zoom");
        return 1;
    }
    catch (error)
    {
        return 0;
    }
});

EM_JS(int, aowisSaveMapNavigationPreference,
      (double scroll_zoom_sensitivity, double mouse_3d_pan_sensitivity,
       double orbit_3d_sensitivity),
{
    try
    {
        if (!globalThis.localStorage)
            return 0;
        globalThis.localStorage.setItem(
            "aowis.map_navigation.scroll_zoom_sensitivity",
            String(scroll_zoom_sensitivity));
        globalThis.localStorage.setItem(
            "aowis.map_navigation.mouse_3d_pan_sensitivity",
            String(mouse_3d_pan_sensitivity));
        globalThis.localStorage.setItem(
            "aowis.map_navigation.orbit_3d_sensitivity",
            String(orbit_3d_sensitivity));
        globalThis.localStorage.removeItem("aowis.map_navigation.mouse_3d_sensitivity");
        globalThis.localStorage.removeItem("aowis.map_navigation.scroll_zoom_2d_sensitivity");
        globalThis.localStorage.removeItem("aowis.map_navigation.scroll_zoom_3d_sensitivity");
        return 1;
    }
    catch (error)
    {
        return 0;
    }
});

GuiConfiguration loadConfiguration()
{
    GuiConfiguration configuration;
    configuration.examples_builtin_enable = aowisBuiltinExamplesEnabled() != 0;
    const int node_palette_value = aowisSymbologyPalettePreference(
        "aowis.symbology.node_palette",
        static_cast<int>(NetworkSymbologyDefaultNodePalette));
    const int link_palette_value = aowisSymbologyPalettePreference(
        "aowis.symbology.link_palette",
        static_cast<int>(NetworkSymbologyDefaultLinkPalette));
    const int heatmap_palette_value = aowisSymbologyPalettePreference(
        "aowis.symbology.heatmap_palette",
        static_cast<int>(NetworkSymbologyDefaultHeatmapPalette));
    configuration.symbology_palettes.node_palette = isValidSymbologyPaletteValue(node_palette_value)
        ? static_cast<NetworkSymbologyPalette>(node_palette_value)
        : NetworkSymbologyDefaultNodePalette;
    configuration.symbology_palettes.node_palette_flipped =
        aowisSymbologyPaletteFlippedPreference(
            "aowis.symbology.node_palette_flipped", 0) != 0;
    configuration.symbology_palettes.link_palette = isValidSymbologyPaletteValue(link_palette_value)
        ? static_cast<NetworkSymbologyPalette>(link_palette_value)
        : NetworkSymbologyDefaultLinkPalette;
    configuration.symbology_palettes.link_palette_flipped =
        aowisSymbologyPaletteFlippedPreference(
            "aowis.symbology.link_palette_flipped", 0) != 0;
    configuration.symbology_palettes.heatmap_palette = isValidSymbologyPaletteValue(heatmap_palette_value)
        ? static_cast<NetworkSymbologyPalette>(heatmap_palette_value)
        : NetworkSymbologyDefaultHeatmapPalette;
    configuration.symbology_palettes.heatmap_palette_flipped =
        aowisSymbologyPaletteFlippedPreference(
            "aowis.symbology.heatmap_palette_flipped", 0) != 0;
    const GuiMapNavigationConfiguration default_map_navigation;
    configuration.map_navigation.scroll_zoom_sensitivity = validatedNavigationSensitivity(
        aowisMapPerformanceDoublePreference(
            "aowis.map_navigation.scroll_zoom_sensitivity",
            default_map_navigation.scroll_zoom_sensitivity),
        default_map_navigation.scroll_zoom_sensitivity);
    configuration.map_navigation.mouse_3d_pan_sensitivity = validatedNavigationSensitivity(
        aowisMapPerformanceDoublePreference(
            "aowis.map_navigation.mouse_3d_pan_sensitivity",
            default_map_navigation.mouse_3d_pan_sensitivity),
        default_map_navigation.mouse_3d_pan_sensitivity);
    configuration.map_navigation.orbit_3d_sensitivity = validatedNavigationSensitivity(
        aowisMapPerformanceDoublePreference(
            "aowis.map_navigation.orbit_3d_sensitivity",
            default_map_navigation.orbit_3d_sensitivity),
        default_map_navigation.orbit_3d_sensitivity);
    const GuiMapPerformanceConfiguration default_map_performance;
    configuration.map_performance.terrain_detail_reduction = validatedTerrainDetailReduction(
        aowisMapPerformanceIntPreference(
            "aowis.map_performance.terrain_detail_reduction",
            default_map_performance.terrain_detail_reduction),
        default_map_performance.terrain_detail_reduction);
    qInfo() << "Loaded GUI configuration from webroot/aowis-server-gui.ini: examples_builtin_enable ="
            << configuration.examples_builtin_enable;
    return configuration;
}
#else
#ifdef Q_OS_WIN
QString legacyNativeConfigurationPath()
{
    QString data_directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data_directory.isEmpty())
        data_directory = QDir::home().filePath(QStringLiteral("AppData/Local"));

    return QDir(data_directory).filePath(
        QStringLiteral("aowis-server-gui/aowis-server-gui.ini"));
}
#endif

QString nativeConfigurationPath()
{
#ifdef Q_OS_WIN
    QString data_directory = qEnvironmentVariable("APPDATA");
    if (data_directory.isEmpty())
        data_directory = QDir::home().filePath(QStringLiteral("AppData/Roaming"));
#else
    QString data_directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data_directory.isEmpty())
        data_directory = QDir::home().filePath(QStringLiteral(".local/share"));
#endif

    return QDir(data_directory).filePath(
        QStringLiteral("aowis-server-gui/aowis-server-gui.ini"));
}

bool createDefaultConfiguration(const QString &path)
{
    const QFileInfo file_info(path);
    if (!QDir().mkpath(file_info.absolutePath()))
    {
        qWarning() << "Failed to create GUI configuration directory:"
                   << file_info.absolutePath();
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        qWarning() << "Failed to create GUI configuration:" << path
                   << file.errorString();
        return false;
    }

    static const QByteArray contents(
        "; AOWIS Server GUI configuration\n"
        "; Restart the application after changing this file.\n"
        "\n"
        "[gui]\n"
        "examples_builtin_enable=true\n"
        "\n"
        "[shortcuts]\n"
        "sidebar_toggle=Win+Tab\n"
        "fullscreen=F11\n"
        "map_monitor_fullscreen=Ctrl+F11\n"
        "simulation_run=Ctrl+R\n"
        "simulation_run_alternate=Shift+Enter\n"
        "map_zoom_in=E\n"
        "map_zoom_out=Q\n"
        "map_pan_up=W\n"
        "map_pan_down=S\n"
        "map_pan_left=A\n"
        "map_pan_right=D\n"
        "map_provider_arcgis_sat=F1\n"
        "map_provider_openstreetmap=F2\n"
        "map_provider_opentopomap=F3\n"
        "map_provider_cycloosm=F4\n"
        "map_editor_select=Esc\n"
        "map_editor_delete=Del\n"
        "map_editor_add_pipe=1\n"
        "map_editor_add_junction=2\n"
        "map_editor_add_valve=3\n"
        "map_editor_add_demand_point=4\n"
        "map_editor_add_pump=5\n"
        "map_editor_add_tank=6\n"
        "map_editor_add_power_source=7\n"
        "map_editor_add_reservoir=8\n"
        "map_editor_add_note=9\n"
        "\n"
        "[map_navigation]\n"
        "scroll_zoom_sensitivity=0.5\n"
        "mouse_3d_pan_sensitivity=0.8\n"
        "orbit_3d_sensitivity=0.8\n"
        "\n"
        "[map_performance]\n"
        "terrain_detail_reduction=1\n");

    if (file.write(contents) != contents.size())
    {
        qWarning() << "Failed to write GUI configuration:" << path
                   << file.errorString();
        file.cancelWriting();
        return false;
    }

    if (!file.commit())
    {
        qWarning() << "Failed to commit GUI configuration:" << path
                   << file.errorString();
        return false;
    }

    qInfo() << "Created default GUI configuration:" << path;
    return true;
}

GuiConfiguration loadConfiguration()
{
    const QString path = nativeConfigurationPath();
    if (!QFile::exists(path))
    {
#ifdef Q_OS_WIN
        const QString legacy_path = legacyNativeConfigurationPath();
        if (legacy_path != path && QFile::exists(legacy_path))
        {
            const QFileInfo file_info(path);
            if (QDir().mkpath(file_info.absolutePath()) && QFile::copy(legacy_path, path))
                qInfo() << "Migrated GUI configuration from" << legacy_path << "to" << path;
            else
                qWarning() << "Failed to migrate GUI configuration from" << legacy_path
                           << "to" << path;
        }
#endif

        if (!QFile::exists(path))
            createDefaultConfiguration(path);
    }

    QSettings settings(path, QSettings::IniFormat);

    const GuiShortcutConfiguration advertised_shortcuts;
    migrateSetting(settings, QStringLiteral("shortcuts/map_editor_add_customer_point"),
                   QStringLiteral("shortcuts/map_editor_add_demand_point"));
    // bgfx is the only map renderer: the renderer choice of older versions
    // is dropped from the file.
    settings.remove(QStringLiteral("gui/map_desktop_renderer"));
    // The map server connection (base URL and API keys) is stored in
    // AOWIS-CONFIG (config/map_server_client_configuration.h). The former
    // [map_server] group is deleted, not migrated, so no API key stays in
    // this file (on Windows it lives in the roaming profile).
    if (settings.childGroups().contains(QStringLiteral("map_server")))
    {
        settings.remove(QStringLiteral("map_server"));
        qWarning() << "Removed the former [map_server] section from" << path
                   << "- re-enter the map server connection in Settings -> Map";
    }
    ensureSettingDefault(settings, QStringLiteral("shortcuts/sidebar_toggle"), advertised_shortcuts.sidebar_toggle);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/fullscreen"), advertised_shortcuts.fullscreen);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_monitor_fullscreen"), advertised_shortcuts.map_monitor_fullscreen);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/simulation_run"), advertised_shortcuts.simulation_run);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/simulation_run_alternate"), advertised_shortcuts.simulation_run_alternate);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_zoom_in"), advertised_shortcuts.map_zoom_in);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_zoom_out"), advertised_shortcuts.map_zoom_out);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_pan_up"), advertised_shortcuts.map_pan_up);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_pan_down"), advertised_shortcuts.map_pan_down);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_pan_left"), advertised_shortcuts.map_pan_left);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_pan_right"), advertised_shortcuts.map_pan_right);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_provider_arcgis_sat"), advertised_shortcuts.map_provider_arcgis_sat);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_provider_openstreetmap"), advertised_shortcuts.map_provider_openstreetmap);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_provider_opentopomap"), advertised_shortcuts.map_provider_opentopomap);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_provider_cycloosm"), advertised_shortcuts.map_provider_cycloosm);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_select"), advertised_shortcuts.map_editor_select);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_delete"), advertised_shortcuts.map_editor_delete);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_pipe"), advertised_shortcuts.map_editor_add_pipe);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_junction"), advertised_shortcuts.map_editor_add_junction);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_valve"), advertised_shortcuts.map_editor_add_valve);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_demand_point"), advertised_shortcuts.map_editor_add_demand_point);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_pump"), advertised_shortcuts.map_editor_add_pump);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_tank"), advertised_shortcuts.map_editor_add_tank);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_power_source"), advertised_shortcuts.map_editor_add_power_source);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_reservoir"), advertised_shortcuts.map_editor_add_reservoir);
    ensureSettingDefault(settings, QStringLiteral("shortcuts/map_editor_add_note"), advertised_shortcuts.map_editor_add_note);
    const GuiMapNavigationConfiguration advertised_map_navigation;
    ensureSettingDefault(settings, QStringLiteral("map_navigation/scroll_zoom_sensitivity"),
                         QString::number(advertised_map_navigation.scroll_zoom_sensitivity));
    ensureSettingDefault(settings, QStringLiteral("map_navigation/mouse_3d_pan_sensitivity"),
                         QString::number(advertised_map_navigation.mouse_3d_pan_sensitivity));
    ensureSettingDefault(settings, QStringLiteral("map_navigation/orbit_3d_sensitivity"),
                         QString::number(advertised_map_navigation.orbit_3d_sensitivity));
    const GuiMapPerformanceConfiguration advertised_map_performance;
    ensureSettingDefault(settings, QStringLiteral("map_performance/terrain_detail_reduction"),
                         QString::number(advertised_map_performance.terrain_detail_reduction));
    // The distance-based terrain LOD knobs, replaced by the detail reduction.
    settings.remove(QStringLiteral("map_performance/terrain_lod_target_cell_size_px"));
    settings.remove(QStringLiteral("map_performance/terrain_max_detail_zoom"));
    settings.remove(QStringLiteral("map_performance/terrain_full_detail_zoom"));
    settings.sync();

    GuiConfiguration configuration;
    configuration.examples_builtin_enable = settings.value(
        QStringLiteral("gui/examples_builtin_enable"), true).toBool();
    configuration.symbology_palettes.node_palette = parseSymbologyPaletteSetting(
        settings.value(QStringLiteral("symbology/node_palette"),
                       symbologyPaletteConfigName(NetworkSymbologyDefaultNodePalette)).toString(),
        NetworkSymbologyDefaultNodePalette);
    configuration.symbology_palettes.node_palette_flipped = settings.value(
        QStringLiteral("symbology/node_palette_flipped"), false).toBool();
    configuration.symbology_palettes.link_palette = parseSymbologyPaletteSetting(
        settings.value(QStringLiteral("symbology/link_palette"),
                       symbologyPaletteConfigName(NetworkSymbologyDefaultLinkPalette)).toString(),
        NetworkSymbologyDefaultLinkPalette);
    configuration.symbology_palettes.link_palette_flipped = settings.value(
        QStringLiteral("symbology/link_palette_flipped"), false).toBool();
    configuration.symbology_palettes.heatmap_palette = parseSymbologyPaletteSetting(
        settings.value(QStringLiteral("symbology/heatmap_palette"),
                       symbologyPaletteConfigName(NetworkSymbologyDefaultHeatmapPalette)).toString(),
        NetworkSymbologyDefaultHeatmapPalette);
    configuration.symbology_palettes.heatmap_palette_flipped = settings.value(
        QStringLiteral("symbology/heatmap_palette_flipped"), false).toBool();

    configuration.shortcuts.sidebar_toggle = loadShortcutSetting(settings, QStringLiteral("shortcuts/sidebar_toggle"), advertised_shortcuts.sidebar_toggle);
    configuration.shortcuts.fullscreen = loadShortcutSetting(settings, QStringLiteral("shortcuts/fullscreen"), advertised_shortcuts.fullscreen);
    configuration.shortcuts.map_monitor_fullscreen = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_monitor_fullscreen"), advertised_shortcuts.map_monitor_fullscreen);
    configuration.shortcuts.simulation_run = loadShortcutSetting(settings, QStringLiteral("shortcuts/simulation_run"), advertised_shortcuts.simulation_run);
    configuration.shortcuts.simulation_run_alternate = loadShortcutSetting(settings, QStringLiteral("shortcuts/simulation_run_alternate"), advertised_shortcuts.simulation_run_alternate);
    configuration.shortcuts.map_zoom_in = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_zoom_in"), advertised_shortcuts.map_zoom_in);
    configuration.shortcuts.map_zoom_out = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_zoom_out"), advertised_shortcuts.map_zoom_out);
    configuration.shortcuts.map_pan_up = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_pan_up"), advertised_shortcuts.map_pan_up);
    configuration.shortcuts.map_pan_down = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_pan_down"), advertised_shortcuts.map_pan_down);
    configuration.shortcuts.map_pan_left = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_pan_left"), advertised_shortcuts.map_pan_left);
    configuration.shortcuts.map_pan_right = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_pan_right"), advertised_shortcuts.map_pan_right);
    configuration.shortcuts.map_provider_arcgis_sat = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_provider_arcgis_sat"), advertised_shortcuts.map_provider_arcgis_sat);
    configuration.shortcuts.map_provider_openstreetmap = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_provider_openstreetmap"), advertised_shortcuts.map_provider_openstreetmap);
    configuration.shortcuts.map_provider_opentopomap = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_provider_opentopomap"), advertised_shortcuts.map_provider_opentopomap);
    configuration.shortcuts.map_provider_cycloosm = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_provider_cycloosm"), advertised_shortcuts.map_provider_cycloosm);
    configuration.shortcuts.map_editor_select = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_select"), advertised_shortcuts.map_editor_select);
    configuration.shortcuts.map_editor_delete = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_delete"), advertised_shortcuts.map_editor_delete);
    configuration.shortcuts.map_editor_add_pipe = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_pipe"), advertised_shortcuts.map_editor_add_pipe);
    configuration.shortcuts.map_editor_add_junction = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_junction"), advertised_shortcuts.map_editor_add_junction);
    configuration.shortcuts.map_editor_add_valve = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_valve"), advertised_shortcuts.map_editor_add_valve);
    configuration.shortcuts.map_editor_add_demand_point = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_demand_point"), advertised_shortcuts.map_editor_add_demand_point);
    configuration.shortcuts.map_editor_add_pump = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_pump"), advertised_shortcuts.map_editor_add_pump);
    configuration.shortcuts.map_editor_add_tank = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_tank"), advertised_shortcuts.map_editor_add_tank);
    configuration.shortcuts.map_editor_add_power_source = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_power_source"), advertised_shortcuts.map_editor_add_power_source);
    configuration.shortcuts.map_editor_add_reservoir = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_reservoir"), advertised_shortcuts.map_editor_add_reservoir);
    configuration.shortcuts.map_editor_add_note = loadShortcutSetting(settings, QStringLiteral("shortcuts/map_editor_add_note"), advertised_shortcuts.map_editor_add_note);

    const GuiMapNavigationConfiguration default_map_navigation;
    bool scroll_zoom_sensitivity_valid = false;
    const double loaded_scroll_zoom_sensitivity = settings.value(
        QStringLiteral("map_navigation/scroll_zoom_sensitivity")).toDouble(
            &scroll_zoom_sensitivity_valid);
    configuration.map_navigation.scroll_zoom_sensitivity =
        (scroll_zoom_sensitivity_valid && std::isfinite(loaded_scroll_zoom_sensitivity)
         && loaded_scroll_zoom_sensitivity >= 0.1 && loaded_scroll_zoom_sensitivity <= 1.5)
            ? loaded_scroll_zoom_sensitivity
            : default_map_navigation.scroll_zoom_sensitivity;

    bool mouse_3d_pan_sensitivity_valid = false;
    const double loaded_mouse_3d_pan_sensitivity = settings.value(
        QStringLiteral("map_navigation/mouse_3d_pan_sensitivity")).toDouble(
            &mouse_3d_pan_sensitivity_valid);
    configuration.map_navigation.mouse_3d_pan_sensitivity =
        (mouse_3d_pan_sensitivity_valid && std::isfinite(loaded_mouse_3d_pan_sensitivity)
         && loaded_mouse_3d_pan_sensitivity >= 0.1 && loaded_mouse_3d_pan_sensitivity <= 1.5)
            ? loaded_mouse_3d_pan_sensitivity
            : default_map_navigation.mouse_3d_pan_sensitivity;

    bool orbit_3d_sensitivity_valid = false;
    const double loaded_orbit_3d_sensitivity = settings.value(
        QStringLiteral("map_navigation/orbit_3d_sensitivity")).toDouble(
            &orbit_3d_sensitivity_valid);
    configuration.map_navigation.orbit_3d_sensitivity =
        (orbit_3d_sensitivity_valid && std::isfinite(loaded_orbit_3d_sensitivity)
         && loaded_orbit_3d_sensitivity >= 0.1 && loaded_orbit_3d_sensitivity <= 1.5)
            ? loaded_orbit_3d_sensitivity
            : default_map_navigation.orbit_3d_sensitivity;

    const GuiMapPerformanceConfiguration default_map_performance;
    bool detail_reduction_valid = false;
    const int loaded_detail_reduction = settings.value(
        QStringLiteral("map_performance/terrain_detail_reduction")).toInt(&detail_reduction_valid);
    configuration.map_performance.terrain_detail_reduction = validatedTerrainDetailReduction(
        detail_reduction_valid ? loaded_detail_reduction : default_map_performance.terrain_detail_reduction,
        default_map_performance.terrain_detail_reduction);

    if (settings.status() != QSettings::NoError)
        qWarning() << "Failed to read GUI configuration:" << path;
    else
        qInfo() << "Loaded GUI configuration:" << path
                << "examples_builtin_enable =" << configuration.examples_builtin_enable;

    return configuration;
}
#endif

GuiConfiguration &mutableGuiConfiguration()
{
    static GuiConfiguration configuration = loadConfiguration();
    return configuration;
}
}

QKeySequence guiShortcutKeySequence(const QString &shortcut)
{
    return parseShortcutKeySequence(shortcut);
}

bool guiShortcutMatches(const QKeyEvent *event, const QString &shortcut,
                        Qt::KeyboardModifiers allowed_extra_modifiers)
{
    if (event == nullptr)
        return false;

    const QKeySequence sequence = parseShortcutKeySequence(shortcut);
    if (sequence.count() != 1)
        return false;

    const QKeyCombination combination = sequence[0];
    if (event->key() != combination.key())
        return false;

    const Qt::KeyboardModifiers expected_modifiers = combination.keyboardModifiers();
    const Qt::KeyboardModifiers actual_modifiers = event->modifiers();
    if ((actual_modifiers & expected_modifiers) != expected_modifiers)
        return false;

    return (actual_modifiers & ~(expected_modifiers | allowed_extra_modifiers)) == Qt::NoModifier;
}

QString guiConfigurationFilePath()
{
#ifdef __EMSCRIPTEN__
    return QStringLiteral("webroot/aowis-server-gui.ini");
#else
    return nativeConfigurationPath();
#endif
}

const GuiConfiguration &guiConfiguration()
{
    return mutableGuiConfiguration();
}

bool saveGuiNodeSymbologyPalette(NetworkSymbologyPalette palette, bool flipped)
{
#ifdef __EMSCRIPTEN__
    const bool saved = aowisSaveSymbologyPalettePreference(
        "aowis.symbology.node_palette", static_cast<int>(palette),
        "aowis.symbology.node_palette_flipped", flipped ? 1 : 0) != 0;
#else
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("symbology/node_palette"), symbologyPaletteConfigName(palette));
    settings.setValue(QStringLiteral("symbology/node_palette_flipped"), flipped);
    settings.sync();
    const bool saved = settings.status() == QSettings::NoError;
#endif
    if (saved)
    {
        mutableGuiConfiguration().symbology_palettes.node_palette = palette;
        mutableGuiConfiguration().symbology_palettes.node_palette_flipped = flipped;
    }
    return saved;
}

bool saveGuiLinkSymbologyPalette(NetworkSymbologyPalette palette, bool flipped)
{
#ifdef __EMSCRIPTEN__
    const bool saved = aowisSaveSymbologyPalettePreference(
        "aowis.symbology.link_palette", static_cast<int>(palette),
        "aowis.symbology.link_palette_flipped", flipped ? 1 : 0) != 0;
#else
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("symbology/link_palette"), symbologyPaletteConfigName(palette));
    settings.setValue(QStringLiteral("symbology/link_palette_flipped"), flipped);
    settings.sync();
    const bool saved = settings.status() == QSettings::NoError;
#endif
    if (saved)
    {
        mutableGuiConfiguration().symbology_palettes.link_palette = palette;
        mutableGuiConfiguration().symbology_palettes.link_palette_flipped = flipped;
    }
    return saved;
}

bool saveGuiHeatmapSymbologyPalette(NetworkSymbologyPalette palette, bool flipped)
{
#ifdef __EMSCRIPTEN__
    const bool saved = aowisSaveSymbologyPalettePreference(
        "aowis.symbology.heatmap_palette", static_cast<int>(palette),
        "aowis.symbology.heatmap_palette_flipped", flipped ? 1 : 0) != 0;
#else
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("symbology/heatmap_palette"), symbologyPaletteConfigName(palette));
    settings.setValue(QStringLiteral("symbology/heatmap_palette_flipped"), flipped);
    settings.sync();
    const bool saved = settings.status() == QSettings::NoError;
#endif
    if (saved)
    {
        mutableGuiConfiguration().symbology_palettes.heatmap_palette = palette;
        mutableGuiConfiguration().symbology_palettes.heatmap_palette_flipped = flipped;
    }
    return saved;
}

void applyGuiMapNavigationConfiguration(const GuiMapNavigationConfiguration &configuration)
{
    mutableGuiConfiguration().map_navigation = configuration;
}

bool saveGuiMapNavigationConfiguration(const GuiMapNavigationConfiguration &configuration)
{
#ifdef __EMSCRIPTEN__
    const bool saved = aowisSaveMapNavigationPreference(
        configuration.scroll_zoom_sensitivity,
        configuration.mouse_3d_pan_sensitivity,
        configuration.orbit_3d_sensitivity) != 0;
#else
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("map_navigation/scroll_zoom_sensitivity"),
                      configuration.scroll_zoom_sensitivity);
    settings.setValue(QStringLiteral("map_navigation/mouse_3d_pan_sensitivity"),
                      configuration.mouse_3d_pan_sensitivity);
    settings.setValue(QStringLiteral("map_navigation/orbit_3d_sensitivity"),
                      configuration.orbit_3d_sensitivity);
    settings.remove(QStringLiteral("map_navigation/mouse_3d_sensitivity"));
    settings.remove(QStringLiteral("map_navigation/scroll_zoom_2d_sensitivity"));
    settings.remove(QStringLiteral("map_navigation/scroll_zoom_3d_sensitivity"));
    settings.sync();
    const bool saved = settings.status() == QSettings::NoError;
#endif
    if (saved)
        applyGuiMapNavigationConfiguration(configuration);
    return saved;
}

bool saveGuiMapPerformanceConfiguration(const GuiMapPerformanceConfiguration &configuration)
{
#ifdef __EMSCRIPTEN__
    const bool saved = aowisSaveMapPerformancePreference(
        configuration.terrain_detail_reduction) != 0;
#else
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("map_performance/terrain_detail_reduction"),
                      configuration.terrain_detail_reduction);
    settings.remove(QStringLiteral("map_performance/terrain_lod_target_cell_size_px"));
    settings.remove(QStringLiteral("map_performance/terrain_max_detail_zoom"));
    settings.remove(QStringLiteral("map_performance/terrain_full_detail_zoom"));
    // The QRhi renderer's texture-array batching switch is gone.
    settings.remove(QStringLiteral("map_performance/array_batching_enabled"));
    settings.sync();
    const bool saved = settings.status() == QSettings::NoError;
#endif
    if (saved)
        mutableGuiConfiguration().map_performance = configuration;
    return saved;
}
