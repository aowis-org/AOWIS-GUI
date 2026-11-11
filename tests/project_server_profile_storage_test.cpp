// Configured DVMDB connections stored in the GUI's AOWIS-CONFIG store
// (collection "connections"): order, stable connection identity across
// verification, no duplicates from stale copies, deletion, protection of
// records this build does not understand, and isolation of the store.
#include "config/gui_config_store.h"
#include "config/project_server_profile_storage.h"
#include "projects/dvmdb_identity.h"

#include <aowis/config/object_store.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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

ProjectServerProfile remoteProfile(const QString &name)
{
    ProjectServerProfile profile;
    profile.id = DvmdbIdentity::newUuidV7();
    profile.name = name;
    profile.endpoint = QStringLiteral("http://dvmdb.example.invalid:8755/");
    profile.auth_mode = QStringLiteral("None");
    return profile;
}

int storedCount(const aowis::config::ObjectCollection &collection)
{
    const aowis::config::Result<aowis::config::ObjectListing> listing = collection.list();
    return listing ? static_cast<int>(listing.value().objects.size()) : -1;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary configuration root unavailable")) return 1;
    qputenv("XDG_CONFIG_HOME", root.path().toUtf8());
    qputenv("XDG_DATA_HOME", root.path().toUtf8());
    QCoreApplication app(argc, argv);

    QString error;
    const aowis::config::OwnerStore *store = guiConfigStore(&error);
    if (!require(store != nullptr, "GUI configuration store did not open")) return 1;
    if (!require(QFileInfo(QDir(root.path()).filePath(QStringLiteral("aowis/gui"))).isDir(),
                 "GUI store is not under XDG_CONFIG_HOME/aowis/gui")) return 1;
    const aowis::config::Result<aowis::config::ObjectCollection> connections = store->collection("connections");
    if (!require(connections.hasValue(), "connections collection unavailable")) return 1;

    if (!require(loadProjectServerProfiles().isEmpty(), "new store is not empty")) return 1;

    // Save assigns connection ids and keeps list order.
    ProjectServerProfile alpha = remoteProfile(QStringLiteral("Alpha"));
    alpha.tags = {QStringLiteral("north"), QStringLiteral("pumps")};
    alpha.wireguard_profile_id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6399");
    ProjectServerProfile beta = remoteProfile(QStringLiteral("Beta"));
    beta.connection_type = QStringLiteral("local");
    beta.local_path = QStringLiteral("/srv/dvmdb");
    beta.personal = true;
    QList<ProjectServerProfile> servers{alpha, beta};
    if (!require(saveProjectServerProfiles(servers), "initial save failed")) return 1;
    if (!require(!servers.at(0).connection_id.isEmpty() && !servers.at(1).connection_id.isEmpty()
                     && servers.at(0).connection_id != servers.at(1).connection_id,
                 "connection ids not written back")) return 1;
    QList<ProjectServerProfile> loaded = loadProjectServerProfiles();
    if (!require(loaded.size() == 2 && loaded.at(0).name == QStringLiteral("Alpha")
                     && loaded.at(1).name == QStringLiteral("Beta"), "order or contents lost")) return 1;
    if (!require(loaded.at(0).connection_id == servers.at(0).connection_id
                     && loaded.at(0).id == alpha.id && loaded.at(0).tags == alpha.tags
                     && loaded.at(0).wireguard_profile_id == alpha.wireguard_profile_id
                     && loaded.at(1).personal && loaded.at(1).local_path == beta.local_path
                     && loaded.at(1).connection_type == QStringLiteral("local"),
                 "fields not round-tripped")) return 1;

    // Reordering is persisted.
    servers = {servers.at(1), servers.at(0)};
    if (!require(saveProjectServerProfiles(servers), "reorder save failed")) return 1;
    loaded = loadProjectServerProfiles();
    if (!require(loaded.size() == 2 && loaded.at(0).name == QStringLiteral("Beta"), "order not persisted")) return 1;

    // A pending connection keeps its connection identity when verification
    // replaces its temporary id by the real repository id.
    ProjectServerProfile pending = remoteProfile(QStringLiteral("Gamma"));
    pending.pending = true;
    servers.append(pending);
    if (!require(saveProjectServerProfiles(servers), "pending save failed")) return 1;
    const QString gamma_connection = servers.last().connection_id;
    servers.last().id = DvmdbIdentity::newUuidV7();
    servers.last().pending = false;
    if (!require(saveProjectServerProfiles(servers), "promotion save failed")) return 1;
    loaded = loadProjectServerProfiles();
    if (!require(loaded.size() == 3 && loaded.at(2).connection_id == gamma_connection
                     && loaded.at(2).id == servers.last().id && !loaded.at(2).pending,
                 "verification changed the connection identity")) return 1;
    if (!require(storedCount(connections.value()) == 3, "promotion created an extra record")) return 1;

    // Stale copies without connection ids (const overload) never duplicate.
    QList<ProjectServerProfile> stale = loadProjectServerProfiles();
    for (ProjectServerProfile &profile : stale) profile.connection_id.clear();
    stale[0].description = QStringLiteral("edited from a stale copy");
    if (!require(saveProjectServerProfiles(static_cast<const QList<ProjectServerProfile> &>(stale)),
                 "stale save failed")) return 1;
    if (!require(storedCount(connections.value()) == 3, "stale copy duplicated records")) return 1;
    if (!require(loadProjectServerProfiles().at(0).description == QStringLiteral("edited from a stale copy"),
                 "stale copy edit lost")) return 1;

    // Invariants are rejected before anything is written.
    QList<ProjectServerProfile> twice{servers.at(0), servers.at(0)};
    if (!require(!saveProjectServerProfiles(twice), "duplicate repository ids accepted")) return 1;
    QList<ProjectServerProfile> two_personal = loadProjectServerProfiles();
    two_personal[1].personal = true;
    two_personal[0].personal = true;
    if (!require(!saveProjectServerProfiles(two_personal), "two personal databases accepted")) return 1;
    if (!require(storedCount(connections.value()) == 3, "rejected save wrote records")) return 1;

    // Records of a newer connection_version or another service are neither
    // shown nor deleted by a save of this build.
    const aowis::config::Result<aowis::config::StoredObject> newer = connections.value().create(
        R"({"connection_version":2,"service_type":"dvmdb","name":"From a newer GUI"})");
    const aowis::config::Result<aowis::config::StoredObject> other = connections.value().create(
        R"({"connection_version":1,"service_type":"map","name":"Map server"})");
    if (!require(newer && other, "cannot plant foreign records")) return 1;
    if (!require(loadProjectServerProfiles().size() == 3, "foreign records shown")) return 1;

    // Removal deletes exactly the dropped connection.
    QList<ProjectServerProfile> fewer = loadProjectServerProfiles();
    fewer.removeAt(1);
    if (!require(saveProjectServerProfiles(fewer), "removal save failed")) return 1;
    if (!require(loadProjectServerProfiles().size() == 2, "connection not removed")) return 1;
    if (!require(connections.value().read(newer.value().info.id).hasValue()
                     && connections.value().read(other.value().info.id).hasValue(),
                 "save deleted records it does not own")) return 1;
    if (!require(storedCount(connections.value()) == 4, "unexpected record count after removal")) return 1;

    // A transport this build does not know never degrades to direct HTTP.
    const aowis::config::Result<aowis::config::StoredObject> legacy = connections.value().create(
        R"({"connection_version":1,"service_type":"dvmdb","position":9,"name":"Legacy tunnel",)"
        R"("repository_id":"019a47b1-2040-7af9-8d1c-2670badb6390","connection_type":"remote",)"
        R"("endpoint":"http://10.0.0.1:8755","transport":{"type":"wireguard-file","path":"/tmp/x.conf"}})");
    if (!require(legacy.hasValue(), "cannot plant unknown transport")) return 1;
    bool fails_closed = false;
    for (const ProjectServerProfile &profile : loadProjectServerProfiles())
        if (profile.name == QStringLiteral("Legacy tunnel"))
            fails_closed = !profile.wireguard_profile_id.isEmpty()
                && !DvmdbIdentity::isUuidV7(profile.wireguard_profile_id);
    if (!require(fails_closed, "unknown transport became a direct connection")) return 1;

    std::printf("project server profile storage tests passed\n");
    return 0;
}
