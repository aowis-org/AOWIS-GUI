#pragma once

#include "config/project_server_profile_storage.h"

#include <QList>
#include <QString>

// Why a new database connection would duplicate one that is already configured.
// Checked in this order; the first match wins.
enum class ProjectServerDuplicateKind
{
    None,
    Name,       // another connection already uses this display name (case-insensitive)
    LocalPath,  // another local connection uses the same data directory
    Endpoint,   // another remote connection uses the same endpoint through the same tunnel
    Repository, // another verified connection already reaches this repository UUIDv7
};

struct ProjectServerDuplicate
{
    ProjectServerDuplicateKind kind = ProjectServerDuplicateKind::None;
    qsizetype index = -1; // index of the conflicting connection in the searched list

    explicit operator bool() const { return this->kind != ProjectServerDuplicateKind::None; }
};

// Canonical form of a DVMDB endpoint for comparison: lower-case scheme and host,
// explicit default port, no trailing slashes and no trailing "/dvmdb/v1" API
// prefix (the GUI appends it itself). Unparseable input is returned trimmed.
QString normalizedProjectServerEndpoint(const QString &endpoint);

// Canonical form of a local data directory: the canonical path when it exists
// (resolving symbolic links), otherwise the cleaned path.
QString normalizedProjectServerLocalPath(const QString &path);

// Finds the first configured connection that candidate would duplicate. A
// pending candidate (unverified, temporary id) and pending existing entries
// never match by repository identity.
ProjectServerDuplicate findProjectServerDuplicate(const QList<ProjectServerProfile> &existing,
                                                  const ProjectServerProfile &candidate);
