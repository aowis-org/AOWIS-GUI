// The map-server connection stored in the GUI's AOWIS-CONFIG "connections"
// collection: defaults without a record, round trip, unchanged saves write
// nothing, private file permissions, coexistence with DVMDB connections,
// fail-closed transports, and the removal of the former INI section.
#include "config/gui_config_store.h"
#include "config/gui_configuration.h"
#include "config/map_server_client_configuration.h"
#include "config/project_server_profile_storage.h"
#include "projects/dvmdb_identity.h"

#include <aowis/config/object_store.h>

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

QList<aowis::config::StoredObject> mapServerRecords(const aowis::config::ObjectCollection &collection)
{
    QList<aowis::config::StoredObject> records;
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection.readAll();
    if (!snapshot) return records;
    for (const aowis::config::StoredObject &stored : snapshot.value().objects)
    {
        const QJsonObject record = QJsonDocument::fromJson(QByteArray::fromStdString(stored.json)).object();
        if (record.value(QStringLiteral("service_type")).toString() == QStringLiteral("map-server"))
            records.append(stored);
    }
    return records;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary configuration root unavailable")) return 1;
    qputenv("XDG_CONFIG_HOME", root.path().toUtf8());
    qputenv("XDG_DATA_HOME", root.path().toUtf8());
    QGuiApplication app(argc, argv); // guiConfiguration() parses key sequences

    // A pre-AOWIS-CONFIG INI with keys: the section is removed, not migrated.
    const QString ini_path = guiConfigurationFilePath();
    if (!require(QDir().mkpath(QFileInfo(ini_path).absolutePath()), "INI directory unavailable")) return 1;
    {
        QSettings legacy(ini_path, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("map_server/base_url"), QStringLiteral("http://legacy.invalid"));
        legacy.setValue(QStringLiteral("map_server/api_key"), QStringLiteral("legacy-secret"));
        legacy.setValue(QStringLiteral("units/selected_profile_id"), QStringLiteral("unrelated"));
    }
    static_cast<void>(guiConfiguration());
    if (!require(!QFile::exists(ini_path), "former INI with API keys not deleted")) return 1;

    QString error;
    const aowis::config::OwnerStore *store = guiConfigStore(&error);
    if (!require(store != nullptr, "GUI configuration store did not open")) return 1;
    const aowis::config::Result<aowis::config::ObjectCollection> connections = store->collection("connections");
    if (!require(connections.hasValue(), "connections collection unavailable")) return 1;

    // No record: defaults, nothing written by reading.
    MapServerClientConfiguration loaded = readMapServerClientConfiguration();
    if (!require(loaded.base_url == QStringLiteral("http://aowis-server-map.localhost:80")
                     && loaded.api_key.isEmpty() && loaded.delete_api_key.isEmpty(),
                 "defaults wrong")) return 1;
    if (!require(mapServerRecords(connections.value()).isEmpty(), "reading created a record")) return 1;

    // A DVMDB connection saved first must survive and stay separate.
    ProjectServerProfile dvmdb;
    dvmdb.id = DvmdbIdentity::newUuidV7();
    dvmdb.name = QStringLiteral("Alpha");
    dvmdb.endpoint = QStringLiteral("http://dvmdb.example.invalid:8755/");
    dvmdb.auth_mode = QStringLiteral("None");
    QList<ProjectServerProfile> servers{dvmdb};
    if (!require(saveProjectServerProfiles(servers), "DVMDB save failed")) return 1;

    MapServerClientConfiguration configured;
    configured.base_url = QStringLiteral("  https://maps.example.invalid/  ");
    configured.api_key = QByteArrayLiteral("read-key");
    configured.delete_api_key = QByteArrayLiteral("delete-key");
    if (!require(saveMapServerClientConfiguration(configured), "map server save failed")) return 1;
    QList<aowis::config::StoredObject> records = mapServerRecords(connections.value());
    if (!require(records.size() == 1, "expected exactly one map-server record")) return 1;
    loaded = readMapServerClientConfiguration();
    if (!require(loaded.base_url == QStringLiteral("https://maps.example.invalid/")
                     && loaded.api_key == QByteArrayLiteral("read-key")
                     && loaded.delete_api_key == QByteArrayLiteral("delete-key"),
                 "round trip lost or did not trim values")) return 1;

    const QString record_path = QDir(root.path()).filePath(
        QStringLiteral("aowis/gui/connections/%1.json").arg(QString::fromStdString(records.at(0).info.id.value())));
#ifdef Q_OS_UNIX
    if (!require(QFileInfo(record_path).permissions()
                     == (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser),
                 "map-server record is not private (0600)")) return 1;
#else
    if (!require(QFileInfo::exists(record_path), "map-server record file missing")) return 1;
#endif

    // An unchanged save writes nothing; a changed one updates in place.
    const std::string revision = records.at(0).info.revision;
    if (!require(saveMapServerClientConfiguration(configured), "unchanged save failed")) return 1;
    records = mapServerRecords(connections.value());
    if (!require(records.size() == 1 && records.at(0).info.revision == revision,
                 "unchanged save rewrote the record")) return 1;
    configured.api_key = QByteArrayLiteral("rotated-key");
    if (!require(saveMapServerClientConfiguration(configured), "changed save failed")) return 1;
    records = mapServerRecords(connections.value());
    if (!require(records.size() == 1 && records.at(0).info.revision != revision
                     && readMapServerClientConfiguration().api_key == QByteArrayLiteral("rotated-key"),
                 "changed save did not update the same record")) return 1;

    // DVMDB loads and saves neither show nor delete the map-server record.
    const QList<ProjectServerProfile> profiles = loadProjectServerProfiles();
    if (!require(profiles.size() == 1 && profiles.at(0).name == QStringLiteral("Alpha"),
                 "map-server record leaked into DVMDB connections")) return 1;
    QList<ProjectServerProfile> none;
    if (!require(saveProjectServerProfiles(none), "DVMDB clear failed")) return 1;
    if (!require(mapServerRecords(connections.value()).size() == 1, "DVMDB save deleted the map-server record"))
        return 1;

    // A WireGuard transport round-trips: saved, loaded and counted as a
    // reference to the profile (the deletion guard reads the same record).
    const QString tunnel_profile = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6399");
    configured.wireguard_profile_id = tunnel_profile;
    if (!require(saveMapServerClientConfiguration(configured), "WireGuard save failed")) return 1;
    records = mapServerRecords(connections.value());
    const QJsonObject saved_transport = QJsonDocument::fromJson(QByteArray::fromStdString(records.at(0).json))
        .object().value(QStringLiteral("transport")).toObject();
    const MapServerClientConfiguration tunnelled_loaded = readMapServerClientConfiguration();
    if (!require(records.size() == 1
                     && saved_transport.value(QStringLiteral("type")).toString() == QStringLiteral("wireguard")
                     && saved_transport.value(QStringLiteral("profile_id")).toString() == tunnel_profile
                     && tunnelled_loaded.wireguard_profile_id == tunnel_profile
                     && tunnelled_loaded.base_url == configured.base_url,
                 "WireGuard transport did not round-trip")) return 1;
    configured.wireguard_profile_id.clear();
    if (!require(saveMapServerClientConfiguration(configured)
                     && readMapServerClientConfiguration().wireguard_profile_id.isEmpty(),
                 "switching back to direct failed")) return 1;
    records = mapServerRecords(connections.value());

    // Transports this build cannot honor fail closed instead of going direct:
    // an unknown type, and a WireGuard transport without a profile.
    const QList<QJsonObject> unusable = {
        QJsonObject{{QStringLiteral("type"), QStringLiteral("carrier-pigeon")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("wireguard")}}};
    aowis::config::Result<aowis::config::StoredObject> updated = connections.value().read(records.at(0).info.id);
    for (const QJsonObject &transport : unusable)
    {
        QJsonObject planted = QJsonDocument::fromJson(QByteArray::fromStdString(updated.value().json)).object();
        planted.insert(QStringLiteral("transport"), transport);
        updated = connections.value().update(updated.value().info.id,
            QJsonDocument(planted).toJson(QJsonDocument::Compact).toStdString(), updated.value().info.revision);
        if (!require(updated.hasValue(), "planting an unusable transport failed")) return 1;
        const MapServerClientConfiguration closed = readMapServerClientConfiguration();
        if (!require(closed.base_url.isEmpty() && closed.wireguard_profile_id.isEmpty(),
                     "unusable transport did not fail closed")) return 1;
    }

    // A newer connection_version is ignored by reads and left alone by saves.
    if (!require(connections.value().erase(updated.value().info.id, updated.value().info.revision).hasValue(),
                 "erase failed")) return 1;
    const aowis::config::Result<aowis::config::StoredObject> newer = connections.value().create(
        "{\"connection_version\":2,\"service_type\":\"map-server\",\"base_url\":\"http://newer.invalid\"}");
    if (!require(newer.hasValue(), "planting a newer record failed")) return 1;
    if (!require(readMapServerClientConfiguration().base_url == QStringLiteral("http://aowis-server-map.localhost:80"),
                 "newer record was interpreted")) return 1;
    if (!require(saveMapServerClientConfiguration(configured), "save next to a newer record failed")) return 1;
    const aowis::config::Result<aowis::config::StoredObject> kept = connections.value().read(newer.value().info.id);
    if (!require(kept.hasValue() && kept.value().info.revision == newer.value().info.revision,
                 "newer record was modified")) return 1;

    std::fprintf(stdout, "map server client configuration tests passed\n");
    return 0;
}
