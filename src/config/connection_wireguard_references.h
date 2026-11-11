#pragma once

#include <aowis/config/object_store.h>

#include <QString>

#include <cstddef>

// Number of configured connection records whose transport references the
// WireGuard profile, counted inside the caller's owner session: the deletion
// guard passed to WireGuardProfiles::erase()/eraseUnreadable(). Counts
// records of every version and service type, and fails if any connection
// record cannot be read, so it never under-counts.
//
// Precondition: the connections collection was opened before the session was
// taken (any loadProjectServerProfiles() call does that). Opening it under a
// held session would wait for that very session and fail with lock_timeout.
//
// Implemented in project_server_profile_storage.cpp, next to the connection
// record format it reads.
aowis::config::Result<std::size_t> countConnectionsUsingWireGuardProfile(
    const aowis::config::OwnerSession &session, const QString &profile_id);
