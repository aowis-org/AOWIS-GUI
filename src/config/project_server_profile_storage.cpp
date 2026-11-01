#include "config/project_server_profile_storage.h"

#include "config/gui_configuration.h"
#include "projects/dvmdb_identity.h"
#include "projects/embedded_dvmdb_host.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <QDir>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstdlib>

EM_JS(char *, aowisLoadProjectServers, (),
{
    try
    {
        const value = globalThis.localStorage
            ? globalThis.localStorage.getItem("aowis.servers.profiles") : null;
        return stringToNewUTF8(value === null ? "[]" : value);
    }
    catch (error)
    {
        return stringToNewUTF8("[]");
    }
});

EM_JS(int, aowisSaveProjectServers, (const char *json),
{
    try
    {
        if (!globalThis.localStorage)
            return 0;
        globalThis.localStorage.setItem("aowis.servers.profiles", UTF8ToString(json));
        return 1;
    }
    catch (error)
    {
        return 0;
    }
});
#else
#include <QSettings>
#endif

namespace
{
const QString servers_key = QStringLiteral("servers/profiles");

// These two IDs belong exclusively to the in-memory demonstration explorer.
// All actual configured DVMDB connections use full UUIDv7 identifiers.
bool validProfileId(const QString &id)
{
    return DvmdbIdentity::isUuidV7(id)
        || id == QStringLiteral("preview-local")
        || id == QStringLiteral("preview-remote");
}

QJsonObject serializeProfile(const ProjectServerProfile &profile)
{
    QJsonArray tags;
    for (const QString &tag : profile.tags)
        tags.append(tag);

    return QJsonObject{
        {QStringLiteral("id"), profile.id},
        {QStringLiteral("pending"), profile.pending},
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("endpoint"), profile.endpoint},
        {QStringLiteral("description"), profile.description},
        {QStringLiteral("tags"), tags},
        {QStringLiteral("auth_mode"), profile.auth_mode},
        {QStringLiteral("connection_type"), profile.connection_type},
        {QStringLiteral("local_path"), profile.local_path},
        {QStringLiteral("visible"), profile.visible},
        {QStringLiteral("personal"), profile.personal}
    };
}
}

QList<ProjectServerProfile> loadProjectServerProfiles()
{
    QByteArray json;
#ifdef __EMSCRIPTEN__
    char *stored = aowisLoadProjectServers();
    if (stored != nullptr)
    {
        json = QByteArray(stored);
        std::free(stored);
    }
#else
    // Also creates the per-user configuration on first launch, before the
    // first Projects widget tries to read or write its server list.
    (void)guiConfiguration();
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    json = settings.value(servers_key, QStringLiteral("[]")).toString().toUtf8();
    if (settings.status() != QSettings::NoError)
        qWarning() << "Unable to load project server profiles from" << settings.fileName();
#endif

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
    {
        qWarning() << "Invalid project server profiles configuration:"
                   << (error.error == QJsonParseError::NoError
                        ? QStringLiteral("expected a JSON array") : error.errorString());
        return {};
    }

    QList<ProjectServerProfile> profiles;
    QSet<QString> seen_ids;
    QSet<QString> seen_names;
    bool personal_found = false;
    for (const QJsonValue &value : document.array())
    {
        if (!value.isObject()) continue;
        const QJsonObject record = value.toObject();
        ProjectServerProfile profile;
        profile.id = record.value(QStringLiteral("id")).toString().trimmed();
        profile.pending = record.value(QStringLiteral("pending")).toBool(false);
        profile.name = record.value(QStringLiteral("name")).toString().trimmed();
        if (!validProfileId(profile.id) || profile.name.isEmpty()
            || seen_ids.contains(profile.id)
            || seen_names.contains(profile.name.toCaseFolded()))
            continue;
        profile.endpoint = record.value(QStringLiteral("endpoint")).toString().trimmed();
        profile.connection_type = record.value(QStringLiteral("connection_type")).toString();
        if (profile.connection_type != QStringLiteral("local"))
            profile.connection_type = QStringLiteral("remote");
        profile.local_path = record.value(QStringLiteral("local_path")).toString().trimmed();
        profile.visible = record.value(QStringLiteral("visible")).toBool(true);
        profile.personal = record.value(QStringLiteral("personal")).toBool(false)
            && !profile.pending && !personal_found;
        if (profile.personal)
        {
            personal_found = true;
        }
        profile.description = record.value(QStringLiteral("description")).toString();
        for (const QJsonValue &tag : record.value(QStringLiteral("tags")).toArray())
            if (tag.isString()) profile.tags.append(tag.toString());
        profile.auth_mode = record.value(QStringLiteral("auth_mode")).toString();
        if (profile.auth_mode != QStringLiteral("API token")
            && profile.auth_mode != QStringLiteral("User login"))
            profile.auth_mode = QStringLiteral("None");
        seen_ids.insert(profile.id);
        seen_names.insert(profile.name.toCaseFolded());
        profiles.append(profile);
    }
    return profiles;
}

bool saveProjectServerProfiles(const QList<ProjectServerProfile> &profiles)
{
    // A single personal database is an invariant of the server list.
    bool seen_personal = false;
    QSet<QString> identities;
    for (const ProjectServerProfile &profile : profiles)
    {
        if (!validProfileId(profile.id) || identities.contains(profile.id)) return false;
        identities.insert(profile.id);
        if (!profile.personal) continue;
        if (seen_personal || profile.pending) return false;
        seen_personal = true;
    }
    QJsonArray records;
    for (const ProjectServerProfile &profile : profiles)
        records.append(serializeProfile(profile));
    const QByteArray json = QJsonDocument(records).toJson(QJsonDocument::Compact);

#ifdef __EMSCRIPTEN__
    // Browsers cannot modify the webroot INI file. Store per-browser profiles
    // in localStorage instead; the native build writes the shared GUI INI.
    const bool saved = aowisSaveProjectServers(json.constData()) != 0;
#else
    (void)guiConfiguration();
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(servers_key, QString::fromUtf8(json));
    // The old Projects section is no longer a server configuration source.
    settings.remove(QStringLiteral("projects/servers"));
    settings.sync();
    const bool saved = settings.status() == QSettings::NoError;
#endif
    if (!saved)
        qWarning() << "Unable to save project server profiles to GUI configuration";
    return saved;
}

QString defaultLocalDvmdbPath()
{
    // Keep standalone's embedded database separate from the DVMDB HTTP
    // daemon's default root. Never silently point two processes at one store.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("aowis-standalone-dvmdb"));
}

bool ensureDefaultLocalDvmdbProfile(QList<ProjectServerProfile> &profiles)
{
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    const QString data_root = QDir::cleanPath(defaultLocalDvmdbPath());
    for (const ProjectServerProfile &profile : profiles)
        if (profile.connection_type == QStringLiteral("local")
            && QDir::cleanPath(profile.local_path) == data_root)
            return true;

    ProjectServerProfile local;
    // Embedded repositories are verified in-process, without a separate daemon.
    QString repository_id;
    QString error;
    const QString endpoint = embeddedDvmdbEndpoint(data_root, false, &repository_id, &error);
    local.id = endpoint.isEmpty() || !DvmdbIdentity::isUuidV7(repository_id)
        ? DvmdbIdentity::newUuidV7() : repository_id;
    local.pending = endpoint.isEmpty() || !DvmdbIdentity::isUuidV7(repository_id);
    local.name = QStringLiteral("Local database");
    int suffix = 2;
    bool name_used = true;
    while (name_used)
    {
        name_used = false;
        for (const ProjectServerProfile &profile : profiles)
            if (profile.name.compare(local.name, Qt::CaseInsensitive) == 0)
            {
                local.name = QStringLiteral("Local database (%1)").arg(suffix++);
                name_used = true;
                break;
            }
    }
    local.connection_type = QStringLiteral("local");
    local.auth_mode = QStringLiteral("None");
    local.local_path = data_root;
    profiles.prepend(local);
    if (!saveProjectServerProfiles(profiles))
        qWarning() << "Unable to persist default local DVMDB profile";
    return true;
#else
    Q_UNUSED(profiles);
    return false;
#endif
}