#include "config/gui_configuration.h"

#include "config/gui_config_store.h"
#include "config/gui_preferences.h"
#include "projects/dvmdb_identity.h"

#include <QByteArray>
#include <QDebug>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QString>
#include <QStringList>

#include <cmath>

#include <QSettings>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtGlobal>
#endif

// Preferences live in AOWIS-CONFIG (config/gui_preferences.h), groups:
//   gui             examples_builtin_enable (native only; WebAssembly reads
//                   globalThis.aowisGuiConfiguration of the hosting page)
//   symbology       {node,link,heatmap}_palette (name), *_palette_flipped
//   shortcuts       <config key> -> portable key sequence ("" = unassigned)
//   map_navigation  scroll_zoom_sensitivity, mouse_3d_pan_sensitivity,
//                   orbit_3d_sensitivity
//   map_performance terrain_detail_reduction, antialiasing_samples
//   (others: interface -> config/gui_language.cpp, units ->
//   config/unit_profile_storage.cpp, map_provider_fallback ->
//   map/core/gui_map_provider_catalog.cpp, dvmdb -> commit author identity)
// Missing keys use the advertised defaults; nothing is written at startup.
// The former INI files and browser localStorage items are deleted, not
// migrated (pre-production data); only the commit author identity is
// imported.

namespace
{
#ifndef __EMSCRIPTEN__
const QString group_gui = QStringLiteral("gui");
#endif
const QString group_symbology = QStringLiteral("symbology");
const QString group_map_navigation = QStringLiteral("map_navigation");
const QString group_map_performance = QStringLiteral("map_performance");

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

int validatedAntialiasingSamples(int value, int fallback)
{
    // Same set as bgfxAntialiasingSamplesValid() (render library); the
    // configuration stays free of renderer headers.
    if (value == 0 || value == 2 || value == 4 || value == 8 || value == 16)
        return value;
    return fallback;
}

QKeySequence parseShortcutKeySequence(const QString &shortcut)
{
    QString portable_text = shortcut.trimmed();
    if (portable_text.startsWith(QStringLiteral("Win+"), Qt::CaseInsensitive))
        portable_text.replace(0, 4, QStringLiteral("Meta+"));

    return QKeySequence::fromString(portable_text, QKeySequence::PortableText);
}

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

struct ShortcutField
{
    const char *key;
    QString GuiShortcutConfiguration::*member;
};

// Keys match GuiShortcutDefinition::config_key (config/shortcut_registry.cpp).
const ShortcutField shortcut_fields[] = {
    {"sidebar_toggle", &GuiShortcutConfiguration::sidebar_toggle},
    {"fullscreen", &GuiShortcutConfiguration::fullscreen},
    {"map_monitor_fullscreen", &GuiShortcutConfiguration::map_monitor_fullscreen},
    {"simulation_run", &GuiShortcutConfiguration::simulation_run},
    {"simulation_run_alternate", &GuiShortcutConfiguration::simulation_run_alternate},
    {"map_zoom_in", &GuiShortcutConfiguration::map_zoom_in},
    {"map_zoom_out", &GuiShortcutConfiguration::map_zoom_out},
    {"map_pan_up", &GuiShortcutConfiguration::map_pan_up},
    {"map_pan_down", &GuiShortcutConfiguration::map_pan_down},
    {"map_pan_left", &GuiShortcutConfiguration::map_pan_left},
    {"map_pan_right", &GuiShortcutConfiguration::map_pan_right},
    {"map_provider_arcgis_sat", &GuiShortcutConfiguration::map_provider_arcgis_sat},
    {"map_provider_openstreetmap", &GuiShortcutConfiguration::map_provider_openstreetmap},
    {"map_provider_opentopomap", &GuiShortcutConfiguration::map_provider_opentopomap},
    {"map_provider_cycloosm", &GuiShortcutConfiguration::map_provider_cycloosm},
    {"map_editor_select", &GuiShortcutConfiguration::map_editor_select},
    {"map_editor_delete", &GuiShortcutConfiguration::map_editor_delete},
    {"map_editor_add_pipe", &GuiShortcutConfiguration::map_editor_add_pipe},
    {"map_editor_add_junction", &GuiShortcutConfiguration::map_editor_add_junction},
    {"map_editor_add_valve", &GuiShortcutConfiguration::map_editor_add_valve},
    {"map_editor_add_demand_point", &GuiShortcutConfiguration::map_editor_add_demand_point},
    {"map_editor_add_pump", &GuiShortcutConfiguration::map_editor_add_pump},
    {"map_editor_add_tank", &GuiShortcutConfiguration::map_editor_add_tank},
    {"map_editor_add_power_source", &GuiShortcutConfiguration::map_editor_add_power_source},
    {"map_editor_add_reservoir", &GuiShortcutConfiguration::map_editor_add_reservoir},
    {"map_editor_add_note", &GuiShortcutConfiguration::map_editor_add_note},
};

// A stored "" deliberately unassigns the shortcut; anything unparsable falls
// back to the advertised default.
QString loadShortcut(const QJsonObject &stored, const QString &key, const QString &default_value)
{
    const QJsonValue value = stored.value(key);
    if (!value.isString())
        return default_value;
    const QString shortcut = value.toString().trimmed();
    if (shortcut.isEmpty())
        return QString();
    // Unknown key names parse to Qt::Key_unknown, not to an empty sequence.
    const QKeySequence sequence = parseShortcutKeySequence(shortcut);
    bool known = !sequence.isEmpty();
    for (int index = 0; index < sequence.count(); ++index)
        known = known && sequence[index].key() != Qt::Key_unknown;
    if (known)
        return shortcut;
    qWarning() << "Invalid stored shortcut" << key << "- using advertised default" << default_value;
    return default_value;
}

double loadDouble(const QJsonObject &stored, const QString &key, double default_value)
{
    const QJsonValue value = stored.value(key);
    return value.isDouble() ? validatedNavigationSensitivity(value.toDouble(), default_value) : default_value;
}

#ifdef __EMSCRIPTEN__
EM_JS(int, aowisBuiltinExamplesEnabled, (),
{
    const configuration = globalThis.aowisGuiConfiguration || {};
    return configuration.examplesBuiltinEnable === false ? 0 : 1;
});

// Former per-preference localStorage items ("aowis.symbology.*",
// "aowis.map_navigation.*", "aowis.map_performance.*").
EM_JS(int, aowisRemoveLegacyPreferenceItems, (),
{
    try
    {
        if (!globalThis.localStorage)
            return 0;
        const prefixes = ["aowis.symbology.", "aowis.map_navigation.", "aowis.map_performance."];
        const stale = [];
        for (let index = 0; index < globalThis.localStorage.length; ++index)
        {
            const key = globalThis.localStorage.key(index);
            if (key !== null && prefixes.some((prefix) => key.startsWith(prefix)))
                stale.push(key);
        }
        for (const key of stale)
            globalThis.localStorage.removeItem(key);
        return stale.length;
    }
    catch (error)
    {
        return 0;
    }
});
#endif

// The commit author identity is the one value carried over from the former
// storage: a new one would make this installation's past commits look like
// another person's (config/gui_preferences.h, group "dvmdb").
void importLegacyCommitAuthorId(const QString &legacy_id)
{
    if (!DvmdbIdentity::isUuidV7(legacy_id))
        return;
    if (!GuiPreferences::value(QStringLiteral("dvmdb"), QStringLiteral("commit_author_id")).isUndefined())
        return;
    if (GuiPreferences::update(QStringLiteral("dvmdb"),
                               QJsonObject{{QStringLiteral("commit_author_id"), legacy_id}}))
        qInfo() << "Imported the commit author identity into the GUI configuration store";
}

#ifdef __EMSCRIPTEN__
void retireLegacyStorage()
{
    if (aowisRemoveLegacyPreferenceItems() > 0)
        qWarning() << "Removed former GUI preferences from browser localStorage";
    // Former browser QSettings of the commit author identity.
    QSettings settings(QStringLiteral("AOWIS"), QStringLiteral("AOWIS-SERVER-GUI"));
    const QString legacy_id = settings.value(QStringLiteral("dvmdb/commit_author_id")).toString();
    if (legacy_id.isEmpty())
        return;
    importLegacyCommitAuthorId(legacy_id);
    settings.remove(QStringLiteral("dvmdb/commit_author_id"));
}
#else
QString legacyIniPath(const QString &base_directory)
{
    return QDir(base_directory).filePath(QStringLiteral("aowis-server-gui/aowis-server-gui.ini"));
}

// The former GUI INI files (Windows also the older AppData\Local copy).
QStringList legacyIniPaths()
{
    QStringList paths;
#ifdef Q_OS_WIN
    QString roaming = qEnvironmentVariable("APPDATA");
    if (roaming.isEmpty())
        roaming = QDir::home().filePath(QStringLiteral("AppData/Roaming"));
    paths.append(legacyIniPath(roaming));
#endif
    QString data_directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data_directory.isEmpty())
        data_directory = QDir::home().filePath(QStringLiteral(".local/share"));
    const QString data_path = legacyIniPath(data_directory);
    if (!paths.contains(data_path))
        paths.append(data_path);
    return paths;
}

// Everything the GUI kept in its INI now lives in AOWIS-CONFIG. The files are
// deleted (pre-production data, not migrated), except for the commit author
// identity, which is imported first.
void retireLegacyStorage()
{
    for (const QString &path : legacyIniPaths())
    {
        if (!QFile::exists(path))
            continue;
        {
            QSettings settings(path, QSettings::IniFormat);
            importLegacyCommitAuthorId(settings.value(QStringLiteral("dvmdb/commit_author_id")).toString());
        }
        if (QFile::remove(path))
        {
            QDir().rmdir(QFileInfo(path).absolutePath()); // only if now empty
            qWarning() << "Removed the former GUI configuration file" << path
                       << "- settings are now stored in" << guiConfigStoreLocation();
        }
        else
        {
            qWarning() << "Could not remove the former GUI configuration file" << path;
        }
    }
}
#endif

GuiConfiguration loadConfiguration()
{
    retireLegacyStorage();

    GuiConfiguration configuration;
#ifdef __EMSCRIPTEN__
    configuration.examples_builtin_enable = aowisBuiltinExamplesEnabled() != 0;
#else
    configuration.examples_builtin_enable =
        GuiPreferences::value(group_gui, QStringLiteral("examples_builtin_enable")).toBool(true);
#endif

    const QJsonObject symbology = GuiPreferences::group(group_symbology);
    GuiSymbologyPaletteConfiguration &palettes = configuration.symbology_palettes;
    palettes.node_palette = parseSymbologyPaletteSetting(
        symbology.value(QStringLiteral("node_palette")).toString(), NetworkSymbologyDefaultNodePalette);
    palettes.node_palette_flipped = symbology.value(QStringLiteral("node_palette_flipped")).toBool(false);
    palettes.link_palette = parseSymbologyPaletteSetting(
        symbology.value(QStringLiteral("link_palette")).toString(), NetworkSymbologyDefaultLinkPalette);
    palettes.link_palette_flipped = symbology.value(QStringLiteral("link_palette_flipped")).toBool(false);
    palettes.heatmap_palette = parseSymbologyPaletteSetting(
        symbology.value(QStringLiteral("heatmap_palette")).toString(), NetworkSymbologyDefaultHeatmapPalette);
    palettes.heatmap_palette_flipped = symbology.value(QStringLiteral("heatmap_palette_flipped")).toBool(false);

    const QJsonObject shortcuts = GuiPreferences::group(guiShortcutPreferenceGroup());
    const GuiShortcutConfiguration advertised_shortcuts;
    for (const ShortcutField &field : shortcut_fields)
        configuration.shortcuts.*field.member = loadShortcut(
            shortcuts, QString::fromLatin1(field.key), advertised_shortcuts.*field.member);

    const QJsonObject navigation = GuiPreferences::group(group_map_navigation);
    const GuiMapNavigationConfiguration default_map_navigation;
    configuration.map_navigation.scroll_zoom_sensitivity = loadDouble(
        navigation, QStringLiteral("scroll_zoom_sensitivity"), default_map_navigation.scroll_zoom_sensitivity);
    configuration.map_navigation.mouse_3d_pan_sensitivity = loadDouble(
        navigation, QStringLiteral("mouse_3d_pan_sensitivity"), default_map_navigation.mouse_3d_pan_sensitivity);
    configuration.map_navigation.orbit_3d_sensitivity = loadDouble(
        navigation, QStringLiteral("orbit_3d_sensitivity"), default_map_navigation.orbit_3d_sensitivity);

    const GuiMapPerformanceConfiguration default_map_performance;
    const QJsonValue detail_reduction =
        GuiPreferences::value(group_map_performance, QStringLiteral("terrain_detail_reduction"));
    configuration.map_performance.terrain_detail_reduction = validatedTerrainDetailReduction(
        detail_reduction.isDouble() ? detail_reduction.toInt(default_map_performance.terrain_detail_reduction)
                                    : default_map_performance.terrain_detail_reduction,
        default_map_performance.terrain_detail_reduction);
    const QJsonValue antialiasing_samples =
        GuiPreferences::value(group_map_performance, QStringLiteral("antialiasing_samples"));
    configuration.map_performance.antialiasing_samples = validatedAntialiasingSamples(
        antialiasing_samples.isDouble() ? antialiasing_samples.toInt(default_map_performance.antialiasing_samples)
                                        : default_map_performance.antialiasing_samples,
        default_map_performance.antialiasing_samples);

    qInfo() << "Loaded GUI preferences: examples_builtin_enable =" << configuration.examples_builtin_enable;
    return configuration;
}

GuiConfiguration &mutableGuiConfiguration()
{
    static GuiConfiguration configuration = loadConfiguration();
    return configuration;
}

bool savePalette(const char *name, NetworkSymbologyPalette palette, bool flipped)
{
    const QString key = QString::fromLatin1(name);
    return GuiPreferences::update(group_symbology, QJsonObject{
        {key + QStringLiteral("_palette"), symbologyPaletteConfigName(palette)},
        {key + QStringLiteral("_palette_flipped"), flipped}});
}
}

QString guiShortcutPreferenceGroup()
{
    return QStringLiteral("shortcuts");
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
    return legacyIniPaths().constFirst();
#endif
}

const GuiConfiguration &guiConfiguration()
{
    return mutableGuiConfiguration();
}

bool saveGuiNodeSymbologyPalette(NetworkSymbologyPalette palette, bool flipped)
{
    const bool saved = savePalette("node", palette, flipped);
    if (saved)
    {
        mutableGuiConfiguration().symbology_palettes.node_palette = palette;
        mutableGuiConfiguration().symbology_palettes.node_palette_flipped = flipped;
    }
    return saved;
}

bool saveGuiLinkSymbologyPalette(NetworkSymbologyPalette palette, bool flipped)
{
    const bool saved = savePalette("link", palette, flipped);
    if (saved)
    {
        mutableGuiConfiguration().symbology_palettes.link_palette = palette;
        mutableGuiConfiguration().symbology_palettes.link_palette_flipped = flipped;
    }
    return saved;
}

bool saveGuiHeatmapSymbologyPalette(NetworkSymbologyPalette palette, bool flipped)
{
    const bool saved = savePalette("heatmap", palette, flipped);
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
    const bool saved = GuiPreferences::update(group_map_navigation, QJsonObject{
        {QStringLiteral("scroll_zoom_sensitivity"), configuration.scroll_zoom_sensitivity},
        {QStringLiteral("mouse_3d_pan_sensitivity"), configuration.mouse_3d_pan_sensitivity},
        {QStringLiteral("orbit_3d_sensitivity"), configuration.orbit_3d_sensitivity}});
    if (saved)
        applyGuiMapNavigationConfiguration(configuration);
    return saved;
}

bool saveGuiMapPerformanceConfiguration(const GuiMapPerformanceConfiguration &configuration)
{
    const bool saved = GuiPreferences::update(group_map_performance, QJsonObject{
        {QStringLiteral("terrain_detail_reduction"), configuration.terrain_detail_reduction},
        {QStringLiteral("antialiasing_samples"), configuration.antialiasing_samples}});
    if (saved)
        mutableGuiConfiguration().map_performance = configuration;
    return saved;
}
