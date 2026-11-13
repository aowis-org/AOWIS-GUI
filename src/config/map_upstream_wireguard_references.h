#ifndef MAP_UPSTREAM_WIREGUARD_REFERENCES_H
#define MAP_UPSTREAM_WIREGUARD_REFERENCES_H

// Standalone: the embedded MAP engine's cascading upstreams that reach their
// MAP server through a central WireGuard profile. MAP's cascade record lives
// in MAP's own AOWIS-CONFIG owner store, next to (not inside) the GUI's
// connections, so the profile deletion guard reads it separately.
#ifdef AOWIS_STANDALONE

#include <QList>
#include <QString>

struct MapUpstreamWireGuardReference
{
    QString name;       // Upstream name (MAP's identifier).
    QString url;
    QString profile_id;
};

// False with error when MAP's cascade record cannot be read: the deletion
// guard then refuses, since an unreadable record might reference the profile.
bool readMapUpstreamWireGuardReferences(QList<MapUpstreamWireGuardReference> *references,
                                        QString *error);

#endif // AOWIS_STANDALONE

#endif // MAP_UPSTREAM_WIREGUARD_REFERENCES_H
