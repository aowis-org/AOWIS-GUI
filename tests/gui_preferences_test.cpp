// GUI preferences in the GUI's AOWIS-CONFIG store (collection "preferences",
// one record per group): defaults, stored values and validation at startup,
// save-through to storage and cache, unchanged writes, merging with keys
// another instance stored, newer records left alone, and retirement of the
// former INI (deleted; only the commit author identity is imported).
#include "config/gui_config_store.h"
#include "config/gui_configuration.h"
#include "config/gui_preferences.h"

#include <aowis/config/object_store.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

const aowis::config::StoredObject *groupRecord(const aowis::config::ObjectSnapshot &snapshot, const QString &group)
{
    for (const aowis::config::StoredObject &stored : snapshot.objects)
    {
        const QJsonObject record = QJsonDocument::fromJson(QByteArray::fromStdString(stored.json)).object();
        if (record.value(QStringLiteral("preferences_version")).toInt() == 1
            && record.value(QStringLiteral("group")).toString() == group)
            return &stored;
    }
    return nullptr;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary configuration root unavailable")) return 1;
    qputenv("XDG_CONFIG_HOME", root.path().toUtf8());
    qputenv("XDG_DATA_HOME", root.path().toUtf8());
    QGuiApplication app(argc, argv); // key sequences need a GUI application

    // Former INI preferences are deleted, not migrated; other groups stay.
    const QString ini_path = guiConfigurationFilePath();
    if (!require(QDir().mkpath(QFileInfo(ini_path).absolutePath()), "INI directory unavailable")) return 1;
    {
        QSettings legacy(ini_path, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("shortcuts/fullscreen"), QStringLiteral("F9"));
        legacy.setValue(QStringLiteral("symbology/node_palette"), QStringLiteral("turbo"));
        legacy.setValue(QStringLiteral("interface/language"), QStringLiteral("de"));
        legacy.setValue(QStringLiteral("gui/examples_builtin_enable"), false);
        legacy.setValue(QStringLiteral("units/selected_profile_id"), QStringLiteral("dropped"));
        legacy.setValue(QStringLiteral("dvmdb/commit_author_id"),
                        QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6301"));
    }

    const aowis::config::OwnerStore *store = guiConfigStore();
    if (!require(store != nullptr, "GUI configuration store did not open")) return 1;
    const aowis::config::Result<aowis::config::ObjectCollection> preferences = store->collection("preferences");
    if (!require(preferences.hasValue(), "preferences collection unavailable")) return 1;

    // Stored before the first guiConfiguration(): read and validated once.
    if (!require(GuiPreferences::update(guiShortcutPreferenceGroup(), QJsonObject{
                     {QStringLiteral("sidebar_toggle"), QStringLiteral("Ctrl+B")},
                     {QStringLiteral("map_zoom_in"), QString()},
                     {QStringLiteral("map_zoom_out"), QStringLiteral("Ctrl+NoSuchKey")}}),
                 "storing shortcuts failed")) return 1;
    if (!require(GuiPreferences::update(QStringLiteral("symbology"), QJsonObject{
                     {QStringLiteral("link_palette"), QStringLiteral("magma")},
                     {QStringLiteral("link_palette_flipped"), true}}),
                 "storing symbology failed")) return 1;
    if (!require(GuiPreferences::update(QStringLiteral("map_navigation"), QJsonObject{
                     {QStringLiteral("scroll_zoom_sensitivity"), 1.2},
                     {QStringLiteral("orbit_3d_sensitivity"), 9.0}}),
                 "storing navigation failed")) return 1;
    if (!require(GuiPreferences::update(QStringLiteral("map_performance"), QJsonObject{
                     {QStringLiteral("terrain_detail_reduction"), 2}}),
                 "storing performance failed")) return 1;
    GuiPreferences::reload();

    // The first guiConfiguration() retires the former INI: the commit author
    // identity is imported, everything else is deleted with the file.
    const GuiConfiguration &configuration = guiConfiguration();
    if (!require(!QFile::exists(ini_path), "former INI file not deleted")) return 1;
    if (!require(GuiPreferences::value(QStringLiteral("dvmdb"), QStringLiteral("commit_author_id")).toString()
                     == QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6301"),
                 "commit author identity not imported")) return 1;
    if (!require(GuiPreferences::group(QStringLiteral("units")).isEmpty(), "other INI values were migrated"))
        return 1;
    const GuiShortcutConfiguration defaults;
    if (!require(configuration.examples_builtin_enable, "former INI value was migrated")) return 1;
    if (!require(configuration.shortcuts.fullscreen == defaults.fullscreen, "former INI shortcut was migrated"))
        return 1;
    if (!require(configuration.shortcuts.sidebar_toggle == QStringLiteral("Ctrl+B"), "stored shortcut lost"))
        return 1;
    if (!require(configuration.shortcuts.map_zoom_in.isEmpty(), "unassigned shortcut not kept empty")) return 1;
    if (!require(configuration.shortcuts.map_zoom_out == defaults.map_zoom_out, "invalid shortcut not defaulted"))
        return 1;
    if (!require(configuration.symbology_palettes.link_palette == NetworkSymbologyPalette::Magma
                     && configuration.symbology_palettes.link_palette_flipped
                     && configuration.symbology_palettes.node_palette == NetworkSymbologyDefaultNodePalette,
                 "symbology preferences wrong")) return 1;
    const GuiMapNavigationConfiguration navigation_defaults;
    if (!require(configuration.map_navigation.scroll_zoom_sensitivity == 1.2
                     && configuration.map_navigation.orbit_3d_sensitivity == navigation_defaults.orbit_3d_sensitivity
                     && configuration.map_navigation.mouse_3d_pan_sensitivity
                            == navigation_defaults.mouse_3d_pan_sensitivity,
                 "navigation preferences not validated")) return 1;
    if (!require(configuration.map_performance.terrain_detail_reduction == 2, "performance preference lost"))
        return 1;

    // Saves go through to storage and the cached configuration.
    if (!require(saveGuiNodeSymbologyPalette(NetworkSymbologyPalette::Batlow, true), "palette save failed"))
        return 1;
    if (!require(guiConfiguration().symbology_palettes.node_palette == NetworkSymbologyPalette::Batlow,
                 "cached configuration not updated")) return 1;
    GuiPreferences::reload();
    if (!require(GuiPreferences::value(QStringLiteral("symbology"), QStringLiteral("node_palette")).toString()
                     == QStringLiteral("batlow")
                     && GuiPreferences::value(QStringLiteral("symbology"), QStringLiteral("link_palette")).toString()
                            == QStringLiteral("magma"),
                 "palette not stored, or other keys of the group lost")) return 1;

    // An unchanged update writes nothing.
    const aowis::config::Result<aowis::config::ObjectSnapshot> before = preferences.value().readAll();
    const aowis::config::StoredObject *previous =
        before ? groupRecord(before.value(), QStringLiteral("symbology")) : nullptr;
    if (!require(previous != nullptr, "symbology record missing")) return 1;
    const std::string revision = previous->info.revision;
    if (!require(saveGuiNodeSymbologyPalette(NetworkSymbologyPalette::Batlow, true), "repeated save failed"))
        return 1;
    const aowis::config::Result<aowis::config::ObjectSnapshot> after = preferences.value().readAll();
    const aowis::config::StoredObject *symbology =
        after ? groupRecord(after.value(), QStringLiteral("symbology")) : nullptr;
    if (!require(symbology != nullptr && symbology->info.revision == revision, "unchanged save rewrote the record"))
        return 1;

    // Another instance adds a key to the group: a later update keeps it.
    QJsonObject record = QJsonDocument::fromJson(QByteArray::fromStdString(symbology->json)).object();
    QJsonObject values = record.value(QStringLiteral("values")).toObject();
    values.insert(QStringLiteral("heatmap_palette"), QStringLiteral("plasma"));
    record.insert(QStringLiteral("values"), values);
    if (!require(preferences.value().update(symbology->info.id,
                     QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString(), symbology->info.revision)
                     .hasValue(), "simulated second instance write failed")) return 1;
    if (!require(saveGuiLinkSymbologyPalette(NetworkSymbologyPalette::Cividis, false), "link save failed")) return 1;
    GuiPreferences::reload();
    if (!require(GuiPreferences::value(QStringLiteral("symbology"), QStringLiteral("heatmap_palette")).toString()
                     == QStringLiteral("plasma"), "update dropped a key stored by another instance")) return 1;

    // Removing keys; a newer record of a group is neither read nor changed.
    if (!require(GuiPreferences::update(guiShortcutPreferenceGroup(), QJsonObject(),
                                        {QStringLiteral("sidebar_toggle")}), "key removal failed")) return 1;
    GuiPreferences::reload();
    if (!require(GuiPreferences::value(guiShortcutPreferenceGroup(), QStringLiteral("sidebar_toggle")).isUndefined(),
                 "removed key still stored")) return 1;
    const aowis::config::Result<aowis::config::StoredObject> newer = preferences.value().create(
        "{\"preferences_version\":2,\"group\":\"interface\",\"values\":{\"language\":\"fr\"}}");
    if (!require(newer.hasValue(), "planting a newer record failed")) return 1;
    GuiPreferences::reload();
    if (!require(GuiPreferences::value(QStringLiteral("interface"), QStringLiteral("language")).isUndefined(),
                 "newer record was read")) return 1;
    if (!require(GuiPreferences::update(QStringLiteral("interface"),
                                        QJsonObject{{QStringLiteral("language"), QStringLiteral("es")}}),
                 "update next to a newer record failed")) return 1;
    const aowis::config::Result<aowis::config::StoredObject> kept = preferences.value().read(newer.value().info.id);
    if (!require(kept.hasValue() && kept.value().info.revision == newer.value().info.revision,
                 "newer record was modified")) return 1;

#ifdef Q_OS_UNIX
    const QFileInfo directory(QDir(root.path()).filePath(QStringLiteral("aowis/gui/preferences")));
    if (!require(directory.isDir()
                     && directory.permissions() == (QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                    | QFileDevice::ExeOwner | QFileDevice::ReadUser
                                                    | QFileDevice::WriteUser | QFileDevice::ExeUser),
                 "preferences collection is not private (0700)")) return 1;
#endif

    std::fprintf(stdout, "GUI preferences tests passed\n");
    return 0;
}
