#include "config/project_server_profile_storage.h"

#include "config/gui_config_store.h"
#include "projects/dvmdb_identity.h"
#include "projects/embedded_dvmdb_host.h"

#include <aowis/config/object_store.h>

#include <QDebug>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Storage of configured DVMDB connections in the GUI's AOWIS-CONFIG store,
// collection "connections", one record per connection. Replaces the former
// JSON array in the GUI INI ("servers/profiles") and the browser's
// "aowis.servers.profiles" localStorage key; those are no longer read
// (pre-production data, no migration).
//
// Record payload (connection_version 1):
//   connection_version, service_type "dvmdb", position (list order), name,
//   description, tags, auth_mode, connection_type "remote"|"local",
//   endpoint, local_path, repository_id (ProjectServerProfile::id: verified
//   repository UUIDv7, or a temporary UUIDv7 while pending), pending,
//   visible, personal, transport.
// transport is {"type":"direct"} or, transitionally until WireGuard profiles
// move into AOWIS-CONFIG, {"type":"wireguard-file","path":...}.

namespace
{
const char *const connections_collection = "connections";
constexpr int connection_version = 1;

// These two IDs belong exclusively to the in-memory demonstration explorer.
// All actual configured DVMDB connections use full UUIDv7 identifiers.
bool validProfileId(const QString &id)
{
    return DvmdbIdentity::isUuidV7(id)
        || id == QStringLiteral("preview-local")
        || id == QStringLiteral("preview-remote");
}

QString errorText(const aowis::config::Error &error)
{
    return QString::fromStdString(error.message);
}

const aowis::config::ObjectCollection *connections(QString *error)
{
    // Opened (verified and created) once; failures are retried on the next
    // call because they can be transient (lock held by another instance).
    static std::mutex guard;
    static std::optional<aowis::config::ObjectCollection> collection;
    const std::lock_guard<std::mutex> locked(guard);
    if (collection.has_value()) return &*collection;
    const aowis::config::OwnerStore *store = guiConfigStore(error);
    if (store == nullptr) return nullptr;
    aowis::config::Result<aowis::config::ObjectCollection> opened = store->collection(connections_collection);
    if (!opened)
    {
        if (error != nullptr) *error = errorText(opened.error());
        return nullptr;
    }
    collection.emplace(std::move(opened.value()));
    return &*collection;
}

std::string serializeProfile(const ProjectServerProfile &profile, int position)
{
    QJsonArray tags;
    for (const QString &tag : profile.tags)
        tags.append(tag);
    QJsonObject transport{{QStringLiteral("type"), QStringLiteral("direct")}};
    if (profile.connection_type == QStringLiteral("remote") && !profile.wireguard_profile_path.isEmpty())
        transport = QJsonObject{
            {QStringLiteral("type"), QStringLiteral("wireguard-file")},
            {QStringLiteral("path"), profile.wireguard_profile_path}};
    const QJsonObject record{
        {QStringLiteral("connection_version"), connection_version},
        {QStringLiteral("service_type"), QStringLiteral("dvmdb")},
        {QStringLiteral("position"), position},
        {QStringLiteral("repository_id"), profile.id},
        {QStringLiteral("pending"), profile.pending},
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("endpoint"), profile.endpoint},
        {QStringLiteral("description"), profile.description},
        {QStringLiteral("tags"), tags},
        {QStringLiteral("auth_mode"), profile.auth_mode},
        {QStringLiteral("connection_type"), profile.connection_type},
        {QStringLiteral("local_path"), profile.local_path},
        {QStringLiteral("transport"), transport},
        {QStringLiteral("visible"), profile.visible},
        {QStringLiteral("personal"), profile.personal}};
    // QJsonObject keeps keys sorted, so unchanged profiles serialize to
    // byte-identical payloads and saving them writes nothing.
    return QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString();
}

QJsonObject payloadObject(const aowis::config::StoredObject &record)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(record.json)).object();
}

// Records this build understands. Anything else (a newer connection_version,
// another service type) is neither shown nor ever modified or deleted.
bool ownRecord(const QJsonObject &record)
{
    return record.value(QStringLiteral("connection_version")).toInt(0) == connection_version
        && record.value(QStringLiteral("service_type")).toString() == QStringLiteral("dvmdb");
}

struct LoadedProfile
{
    ProjectServerProfile profile;
    int position = 0;
};
}

QList<ProjectServerProfile> loadProjectServerProfiles()
{
    QString error;
    const aowis::config::ObjectCollection *collection = connections(&error);
    if (collection == nullptr)
    {
        qWarning() << "Unable to load configured connections:" << error;
        return {};
    }
    aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection->readAll();
    if (!snapshot)
    {
        qWarning() << "Unable to load configured connections:" << errorText(snapshot.error());
        return {};
    }
    for (const aowis::config::UnreadableObject &damaged : snapshot.value().unreadable)
        qWarning() << "Skipping unreadable connection record" << QString::fromStdString(damaged.filename)
                   << errorText(damaged.error);

    std::vector<LoadedProfile> loaded;
    for (const aowis::config::StoredObject &stored : snapshot.value().objects)
    {
        const QJsonObject record = payloadObject(stored);
        if (!ownRecord(record))
        {
            qWarning() << "Skipping connection record of an unsupported version or service"
                       << QString::fromStdString(stored.info.id.value());
            continue;
        }
        LoadedProfile entry;
        ProjectServerProfile &profile = entry.profile;
        entry.position = record.value(QStringLiteral("position")).toInt(0);
        profile.connection_id = QString::fromStdString(stored.info.id.value());
        profile.id = record.value(QStringLiteral("repository_id")).toString().trimmed();
        profile.pending = record.value(QStringLiteral("pending")).toBool(false);
        profile.name = record.value(QStringLiteral("name")).toString().trimmed();
        profile.endpoint = record.value(QStringLiteral("endpoint")).toString().trimmed();
        profile.connection_type = record.value(QStringLiteral("connection_type")).toString();
        if (profile.connection_type != QStringLiteral("local"))
            profile.connection_type = QStringLiteral("remote");
        profile.local_path = record.value(QStringLiteral("local_path")).toString().trimmed();
        const QJsonObject transport = record.value(QStringLiteral("transport")).toObject();
        if (profile.connection_type == QStringLiteral("remote")
            && transport.value(QStringLiteral("type")).toString() == QStringLiteral("wireguard-file"))
            profile.wireguard_profile_path = transport.value(QStringLiteral("path")).toString();
        profile.visible = record.value(QStringLiteral("visible")).toBool(true);
        profile.personal = record.value(QStringLiteral("personal")).toBool(false);
        profile.description = record.value(QStringLiteral("description")).toString();
        for (const QJsonValue &tag : record.value(QStringLiteral("tags")).toArray())
            if (tag.isString()) profile.tags.append(tag.toString());
        profile.auth_mode = record.value(QStringLiteral("auth_mode")).toString();
        if (profile.auth_mode != QStringLiteral("API token")
            && profile.auth_mode != QStringLiteral("User login"))
            profile.auth_mode = QStringLiteral("None");
        loaded.push_back(entry);
    }
    // Saved list order; records of equal position (e.g. written by another
    // instance) fall back to creation order, which readAll() already gives.
    std::stable_sort(loaded.begin(), loaded.end(), [](const LoadedProfile &left, const LoadedProfile &right)
    {
        return left.position < right.position;
    });

    QList<ProjectServerProfile> profiles;
    QSet<QString> seen_ids;
    QSet<QString> seen_names;
    bool personal_found = false;
    for (LoadedProfile &entry : loaded)
    {
        ProjectServerProfile &profile = entry.profile;
        if (!validProfileId(profile.id) || profile.name.isEmpty()
            || seen_ids.contains(profile.id)
            || seen_names.contains(profile.name.toCaseFolded()))
        {
            qWarning() << "Skipping invalid or duplicate connection record" << profile.connection_id;
            continue;
        }
        profile.personal = profile.personal && !profile.pending && !personal_found;
        if (profile.personal) personal_found = true;
        seen_ids.insert(profile.id);
        seen_names.insert(profile.name.toCaseFolded());
        profiles.append(profile);
    }
    return profiles;
}

bool saveProjectServerProfiles(QList<ProjectServerProfile> &profiles)
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

    QString error;
    const aowis::config::ObjectCollection *collection = connections(&error);
    if (collection == nullptr)
    {
        qWarning() << "Unable to save configured connections:" << error;
        return false;
    }
    // One owner session: no other GUI instance can interleave between
    // reading the current records and replacing them.
    aowis::config::Result<aowis::config::OwnerSession> session = collection->store().lock();
    if (!session)
    {
        qWarning() << "Unable to save configured connections:" << errorText(session.error());
        return false;
    }
    aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection->readAll(session.value());
    if (!snapshot)
    {
        qWarning() << "Unable to save configured connections:" << errorText(snapshot.error());
        return false;
    }

    // Existing records by connection id, plus a repository-id fallback for
    // profiles whose connection_id was not written back to the caller.
    QHash<QString, const aowis::config::StoredObject *> by_connection;
    QHash<QString, QString> connection_by_repository;
    for (const aowis::config::StoredObject &stored : snapshot.value().objects)
    {
        const QJsonObject record = payloadObject(stored);
        if (!ownRecord(record)) continue;
        const QString connection_id = QString::fromStdString(stored.info.id.value());
        by_connection.insert(connection_id, &stored);
        const QString repository_id = record.value(QStringLiteral("repository_id")).toString();
        if (!repository_id.isEmpty() && !connection_by_repository.contains(repository_id))
            connection_by_repository.insert(repository_id, connection_id);
    }

    QSet<QString> kept;
    bool saved = true;
    for (int index = 0; index < profiles.size() && saved; ++index)
    {
        ProjectServerProfile &profile = profiles[index];
        QString target = profile.connection_id;
        if (target.isEmpty() || !by_connection.contains(target) || kept.contains(target))
            target = connection_by_repository.value(profile.id);
        if (kept.contains(target)) target.clear();
        const std::string payload = serializeProfile(profile, index);
        if (!target.isEmpty() && by_connection.contains(target))
        {
            const aowis::config::StoredObject *stored = by_connection.value(target);
            kept.insert(target);
            profile.connection_id = target;
            if (stored->json == payload) continue;
            const aowis::config::Result<aowis::config::StoredObject> updated =
                collection->update(session.value(), stored->info.id, payload, stored->info.revision);
            if (!updated)
            {
                qWarning() << "Unable to update connection" << target << errorText(updated.error());
                saved = false;
            }
            continue;
        }
        const aowis::config::Result<aowis::config::StoredObject> created =
            collection->create(session.value(), payload);
        if (!created)
        {
            qWarning() << "Unable to create connection" << errorText(created.error());
            saved = false;
            continue;
        }
        profile.connection_id = QString::fromStdString(created.value().info.id.value());
        kept.insert(profile.connection_id);
    }
    // Delete only after every kept/new record is written, so a failure never
    // leaves fewer connections than before. Unreadable and foreign records
    // (by_connection holds only own records) are left alone.
    for (const aowis::config::StoredObject &stored : snapshot.value().objects)
    {
        if (!saved) break;
        const QString connection_id = QString::fromStdString(stored.info.id.value());
        if (kept.contains(connection_id) || !by_connection.contains(connection_id)) continue;
        const aowis::config::Result<bool> erased = collection->erase(session.value(), stored.info.id, stored.info.revision);
        if (!erased)
        {
            qWarning() << "Unable to delete connection" << connection_id << errorText(erased.error());
            saved = false;
        }
    }
    if (!saved)
        qWarning() << "Unable to save configured connections";
    return saved;
}

bool saveProjectServerProfiles(const QList<ProjectServerProfile> &profiles)
{
    QList<ProjectServerProfile> copy = profiles;
    return saveProjectServerProfiles(copy);
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