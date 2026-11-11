#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// A locally configured data source. Old endpoint-only records are REST sources.
// Authentication modes are descriptive only; credentials and connection state
// are not part of the stored profile.
struct ProjectServerProfile
{
    // Identity of this configured connection (AOWIS-CONFIG record UUIDv7).
    // Distinct from the repository identity below: it survives verification
    // (pending -> real repository id) and endpoint changes. Empty until saved.
    QString connection_id;
    QString id; // Actual DVMDB repository UUIDv7, or temporary UUIDv7 while pending
    bool pending = false; // Unverified connection; never treated as a repository identity
    QString name;
    QString endpoint;
    QString description;
    QStringList tags;
    QString auth_mode;
    QString connection_type = QStringLiteral("remote"); // "remote" or "local"
    QString local_path;
    bool visible = true; // include this source in the navigator and graph
    bool personal = false; // one personal working database, selected in Projects Inspector
    // ID of a stored WireGuard profile (config/wireguard_profile_storage.h).
    // Empty selects direct HTTP for catalogue reads and metadata writes. A
    // non-empty ID that does not name a usable profile fails closed.
    QString wireguard_profile_id;
};

// Configured connections are records of the GUI's AOWIS-CONFIG
// "connections" collection; list order is persisted. Records that cannot be
// read, or that belong to a newer connection_version or another service type,
// are skipped with a warning and never modified or deleted by a save.
QList<ProjectServerProfile> loadProjectServerProfiles();
// Replaces the stored list with profiles in one owner session: changed
// records are updated, new ones created, missing ones deleted. The non-const
// overload writes the connection_id of newly created records back into
// profiles, so later saves keep updating the same records.
bool saveProjectServerProfiles(QList<ProjectServerProfile> &profiles);
bool saveProjectServerProfiles(const QList<ProjectServerProfile> &profiles);
QString defaultLocalDvmdbPath();
// The embedded source exists only in builds with the native DVMDB backend.
// Does not open/create the database; that belongs to the repository adapter.
bool ensureDefaultLocalDvmdbProfile(QList<ProjectServerProfile> &profiles);