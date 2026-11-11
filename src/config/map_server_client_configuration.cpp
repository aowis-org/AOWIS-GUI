#include "config/map_server_client_configuration.h"

#include <QDebug>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstdlib>
#else
#include "config/gui_config_store.h"

#include <aowis/config/object_store.h>

#include <QJsonDocument>
#include <QJsonObject>

#include <optional>
#include <string>
#endif

#ifdef __EMSCRIPTEN__
EM_JS(char *, aowisMapServerBaseUrl, (),
{
    const configuration = globalThis.aowisMapServerConfiguration || {};
    const value = typeof configuration.baseUrl === "string" ? configuration.baseUrl : "";
    return stringToNewUTF8(value);
});

EM_JS(char *, aowisMapServerApiKey, (),
{
    const configuration = globalThis.aowisMapServerConfiguration || {};
    const value = typeof configuration.apiKey === "string" ? configuration.apiKey : "";
    return stringToNewUTF8(value);
});

EM_JS(char *, aowisMapServerDeleteApiKey, (),
{
    const configuration = globalThis.aowisMapServerConfiguration || {};
    const value = typeof configuration.deleteApiKey === "string" ? configuration.deleteApiKey : "";
    return stringToNewUTF8(value);
});

#endif

namespace
{
#ifdef __EMSCRIPTEN__

QString takeWasmString(char *value)
{
    if (value == nullptr)
        return QString();

    const QString result = QString::fromUtf8(value);
    std::free(value);
    return result;
}

MapServerClientConfiguration loadConfiguration()
{
    MapServerClientConfiguration configuration;
    configuration.base_url = takeWasmString(aowisMapServerBaseUrl()).trimmed();
    configuration.api_key = takeWasmString(aowisMapServerApiKey()).trimmed().toUtf8();
    configuration.delete_api_key = takeWasmString(aowisMapServerDeleteApiKey()).trimmed().toUtf8();

    if (configuration.base_url.isEmpty())
        qCritical() << "Missing map_server/base_url in webroot/aowis-server-gui.ini";
    else
        qInfo() << "Loaded map server client configuration from webroot/aowis-server-gui.ini:"
                << configuration.base_url;
    return configuration;
}
#else
// Record payload (connection_version 1), sorted keys like every
// QJsonObject: api_key, base_url, connection_version, delete_api_key, name,
// position, service_type "map-server", transport {"type":"direct"} or
// {"type":"wireguard","profile_id":...}.
const char *const connections_collection = "connections";
constexpr int connection_version = 1;
QString defaultBaseUrl()
{
    return QStringLiteral("http://aowis-server-map.localhost:80");
}

QString errorText(const aowis::config::Error &error)
{
    return QString::fromStdString(error.message);
}

bool mapServerRecord(const QJsonObject &record)
{
    return record.value(QStringLiteral("connection_version")).toInt(0) == connection_version
        && record.value(QStringLiteral("service_type")).toString() == QStringLiteral("map-server");
}

QJsonObject payloadObject(const aowis::config::StoredObject &stored)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(stored.json)).object();
}

// The GUI uses one map server: the map-server record with the lowest
// position (then creation order, as readAll() returns it). Further
// map-server records, e.g. written by a newer build, are left alone.
const aowis::config::StoredObject *selectedRecord(const aowis::config::ObjectSnapshot &snapshot)
{
    const aowis::config::StoredObject *selected = nullptr;
    int selected_position = 0;
    for (const aowis::config::StoredObject &stored : snapshot.objects)
    {
        const QJsonObject record = payloadObject(stored);
        if (!mapServerRecord(record)) continue;
        const int position = record.value(QStringLiteral("position")).toInt(0);
        if (selected == nullptr || position < selected_position)
        {
            selected = &stored;
            selected_position = position;
        }
    }
    return selected;
}

std::string serializeConfiguration(const MapServerClientConfiguration &configuration)
{
    const QJsonObject record{
        {QStringLiteral("connection_version"), connection_version},
        {QStringLiteral("service_type"), QStringLiteral("map-server")},
        {QStringLiteral("position"), 0},
        {QStringLiteral("name"), QStringLiteral("Map server")},
        {QStringLiteral("base_url"), configuration.base_url.trimmed()},
        {QStringLiteral("api_key"), QString::fromUtf8(configuration.api_key).trimmed()},
        {QStringLiteral("delete_api_key"), QString::fromUtf8(configuration.delete_api_key).trimmed()},
        {QStringLiteral("transport"), configuration.wireguard_profile_id.trimmed().isEmpty()
            ? QJsonObject{{QStringLiteral("type"), QStringLiteral("direct")}}
            : QJsonObject{{QStringLiteral("type"), QStringLiteral("wireguard")},
                          {QStringLiteral("profile_id"), configuration.wireguard_profile_id.trimmed()}}}};
    return QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString();
}

std::optional<aowis::config::ObjectCollection> openConnections(QString *error)
{
    const aowis::config::OwnerStore *store = guiConfigStore(error);
    if (store == nullptr) return std::nullopt;
    aowis::config::Result<aowis::config::ObjectCollection> opened = store->collection(connections_collection);
    if (!opened)
    {
        if (error != nullptr) *error = errorText(opened.error());
        return std::nullopt;
    }
    return std::move(opened.value());
}

MapServerClientConfiguration loadConfiguration()
{
    MapServerClientConfiguration configuration;
    configuration.base_url = defaultBaseUrl();

    QString error;
    const std::optional<aowis::config::ObjectCollection> collection = openConnections(&error);
    if (!collection.has_value())
    {
        qWarning() << "Unable to load the map server connection, using defaults:" << error;
        return configuration;
    }
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection->readAll();
    if (!snapshot)
    {
        qWarning() << "Unable to load the map server connection, using defaults:" << errorText(snapshot.error());
        return configuration;
    }
    const aowis::config::StoredObject *stored = selectedRecord(snapshot.value());
    if (stored == nullptr)
    {
        qInfo() << "No stored map server connection; using" << configuration.base_url;
        return configuration;
    }
    const QJsonObject record = payloadObject(*stored);
    configuration.base_url = record.value(QStringLiteral("base_url")).toString().trimmed();
    configuration.api_key = record.value(QStringLiteral("api_key")).toString().trimmed().toUtf8();
    configuration.delete_api_key = record.value(QStringLiteral("delete_api_key")).toString().trimmed().toUtf8();
    const QJsonObject transport_object = record.value(QStringLiteral("transport")).toObject();
    const QString transport = transport_object.value(QStringLiteral("type")).toString(QStringLiteral("direct"));
    if (transport == QStringLiteral("wireguard"))
        configuration.wireguard_profile_id = transport_object.value(QStringLiteral("profile_id")).toString().trimmed();
    if ((transport != QStringLiteral("direct") && transport != QStringLiteral("wireguard"))
        || (transport == QStringLiteral("wireguard") && configuration.wireguard_profile_id.isEmpty()))
    {
        // Never fall back to a direct connection for a record that asks for
        // something else (e.g. a transport of a newer build).
        qCritical() << "Map server connection uses an unsupported transport; the map server is disabled";
        configuration.base_url.clear();
        configuration.wireguard_profile_id.clear();
        return configuration;
    }
    if (configuration.base_url.isEmpty())
        qCritical() << "Stored map server connection has no base URL";
    else
        qInfo() << "Loaded map server connection:" << configuration.base_url;
    return configuration;
}

bool storeConfiguration(const MapServerClientConfiguration &configuration)
{
    QString error;
    const std::optional<aowis::config::ObjectCollection> collection = openConnections(&error);
    if (!collection.has_value())
    {
        qWarning() << "Unable to save the map server connection:" << error;
        return false;
    }
    // One owner session: another GUI instance cannot create a second record
    // between the lookup and the write.
    aowis::config::Result<aowis::config::OwnerSession> session = collection->store().lock();
    if (!session)
    {
        qWarning() << "Unable to save the map server connection:" << errorText(session.error());
        return false;
    }
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection->readAll(session.value());
    if (!snapshot)
    {
        qWarning() << "Unable to save the map server connection:" << errorText(snapshot.error());
        return false;
    }
    const std::string payload = serializeConfiguration(configuration);
    const aowis::config::StoredObject *stored = selectedRecord(snapshot.value());
    if (stored != nullptr)
    {
        if (stored->json == payload) return true;
        const aowis::config::Result<aowis::config::StoredObject> updated =
            collection->update(session.value(), stored->info.id, payload, stored->info.revision);
        if (!updated)
        {
            qWarning() << "Unable to save the map server connection:" << errorText(updated.error());
            return false;
        }
        return true;
    }
    const aowis::config::Result<aowis::config::StoredObject> created = collection->create(session.value(), payload);
    if (!created)
    {
        qWarning() << "Unable to save the map server connection:" << errorText(created.error());
        return false;
    }
    return true;
}
#endif
}

const MapServerClientConfiguration &mapServerClientConfiguration()
{
    static const MapServerClientConfiguration configuration = loadConfiguration();
    return configuration;
}

MapServerClientConfiguration readMapServerClientConfiguration()
{
    return loadConfiguration();
}

bool saveMapServerClientConfiguration(const MapServerClientConfiguration &configuration)
{
#ifdef __EMSCRIPTEN__
    Q_UNUSED(configuration);
    // In WASM builds this configuration comes from
    // globalThis.aowisMapServerConfiguration, set by the hosting page
    // before the app loads -- it isn't something the in-app settings UI can
    // meaningfully override, and API keys are never stored in the browser.
    qWarning() << "Map server configuration cannot be changed from within "
                  "the WASM build; it is set by the hosting page.";
    return false;
#else
    return storeConfiguration(configuration);
#endif
}
