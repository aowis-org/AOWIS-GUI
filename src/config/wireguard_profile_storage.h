#pragma once

#include <aowis/config/wireguard_profiles.h>

#include <QByteArray>
#include <QList>
#include <QString>

#include <string>

// The GUI owner's imported WireGuard profiles, stored by AOWIS-CONFIG
// (collection "wireguard" of the store from guiConfigStore()). A profile is
// the exact original .conf text, including its private key, plus a name.
// Connections reference profiles by ID (ProjectServerProfile::
// wireguard_profile_id), never by file path.
//
// Storage does not interpret WireGuard syntax: callers validate with
// AOWIS-NETWORK's parseWireGuardConf() before importing and before starting
// a tunnel. All functions are thread-safe (tunnels start on worker threads).
// Messages never contain profile text or keys.

struct WireGuardProfileEntry
{
    QString id;   // UUIDv7
    QString name;
};

const aowis::config::WireGuardProfiles *guiWireGuardProfiles(QString *error = nullptr);

// Readable profiles, sorted by name (case-insensitive), then id.
QList<WireGuardProfileEntry> listWireGuardProfiles(QString *error = nullptr);

// Exact stored text of a profile. False (with error) when the id is not a
// stored profile, so a connection never silently falls back to direct.
bool readWireGuardProfileText(const QString &profile_id, std::string &text, QString *error);

// Stores text under a name that is unique among the profiles (a numbered
// suffix is appended if needed) and returns the new profile's id.
bool importWireGuardProfile(const QString &name, const QByteArray &text, QString *profile_id, QString *error);
