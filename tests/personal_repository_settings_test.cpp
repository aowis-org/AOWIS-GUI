#include "config/gui_configuration.h"
#include "config/personal_repository_storage.h"
#include "config/project_server_profile_storage.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QSettings>

#include <cstdio>

namespace
{
const QString known_id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6371");

bool check(bool value, const char *message)
{
    if (!value) std::fprintf(stderr, "FAILED: %s\n", message);
    return value;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!check(root.isValid(), "settings directory unavailable")) return 1;
    qputenv("XDG_DATA_HOME", root.path().toUtf8());
    QApplication app(argc, argv);

    ProjectServerProfile a;
    a.id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6372");
    a.name = QStringLiteral("Shared database");
    a.endpoint = QStringLiteral("https://source.example.invalid");
    ProjectServerProfile b;
    b.id = known_id;
    b.name = QStringLiteral("My working server");
    b.endpoint = QStringLiteral("https://private.example.invalid");
    // Saving the new server list removes the obsolete Projects-section key.
    {
        QSettings obsolete_settings(guiConfigurationFilePath(), QSettings::IniFormat);
        obsolete_settings.setValue(QStringLiteral("projects/servers"), QStringLiteral("[]"));
        obsolete_settings.sync();
    }
    if (!check(saveProjectServerProfiles({a, b}), "unable to save initial server profiles")) return 1;
    QSettings saved_settings(guiConfigurationFilePath(), QSettings::IniFormat);
    if (!check(saved_settings.contains(QStringLiteral("servers/profiles"))
               && !saved_settings.contains(QStringLiteral("projects/servers")),
               "database profiles must be stored in the servers section")) return 1;
    if (!check(resolvePersonalRepositoryTarget().kind == PersonalRepositoryTarget::Kind::Unavailable,
               "an unmarked source was used implicitly")) return 1;

    // The selected server's UUID is learned from /repository/tracking by the
    // inspector and pinned internally; there is no editable UUID field.
    b.personal = true;
    if (!check(saveProjectServerProfiles({a, b}), "unable to designate personal server")) return 1;
    QList<ProjectServerProfile> loaded = loadProjectServerProfiles();
    if (!check(loaded.size() == 2 && !loaded.at(0).personal && loaded.at(1).personal
               && loaded.at(1).id == known_id,
               "personal designation or pinned identity did not survive restart")) return 1;
    PersonalRepositoryTarget target = resolvePersonalRepositoryTarget();
    if (!check(target.kind == PersonalRepositoryTarget::Kind::Http
               && target.endpoint == b.endpoint && target.repository_id == known_id,
               "personal HTTP target was not derived from designated server")) return 1;
    if (!check(!saveProjectServerProfiles({b, b}), "duplicate personal servers accepted")) return 1;

    // Selecting another personal server atomically revokes the old choice.
    a.personal = true;
    b.personal = false;
    if (!check(saveProjectServerProfiles({a, b}), "unable to switch personal server")) return 1;
    target = resolvePersonalRepositoryTarget();
    if (!check(target.kind == PersonalRepositoryTarget::Kind::Http
               && target.endpoint == a.endpoint && target.repository_id == a.id,
               "switching personal server retained the old target")) return 1;
    a.personal = false;
    b.personal = true;

    // A pending connection may not be marked as personal.
    b.pending = true;
    if (!check(!saveProjectServerProfiles({a, b}),
               "pending HTTP repository was allowed to become personal")) return 1;
    b.personal = false;
    if (!check(saveProjectServerProfiles({a, b})
               && resolvePersonalRepositoryTarget().kind == PersonalRepositoryTarget::Kind::Unavailable,
               "unverified connection was treated as a personal repository")) return 1;
    b.pending = false;

    // Native personal selection uses the selected local profile, rather than
    // a separate undocumented data directory.
    ProjectServerProfile embedded;
    embedded.id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6373");
    embedded.name = QStringLiteral("Embedded local database");
    embedded.connection_type = QStringLiteral("local");
    embedded.local_path = QDir(root.path()).filePath(QStringLiteral("local-db"));
    embedded.personal = true;
    if (!check(saveProjectServerProfiles({a, embedded}), "unable to designate local server")) return 1;
    target = resolvePersonalRepositoryTarget();
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
    if (!check(target.kind == PersonalRepositoryTarget::Kind::Embedded
               && target.storage_path == embedded.local_path,
               "personal embedded target does not match selected server")) return 1;
#else
    if (!check(target.kind == PersonalRepositoryTarget::Kind::Unavailable,
               "embedded target was offered without embedded DVMDB")) return 1;
#endif
    embedded.personal = false;
    if (!check(saveProjectServerProfiles({a, embedded})
               && resolvePersonalRepositoryTarget().kind == PersonalRepositoryTarget::Kind::Unavailable,
               "removing designation did not disable private target")) return 1;

    std::puts("Projects personal-server persistence and target resolution passed");
    return 0;
}
