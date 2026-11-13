#ifndef WIREGUARD_PROFILE_IMPORT_H
#define WIREGUARD_PROFILE_IMPORT_H

// Importing a WireGuard profile file into the central profile store
// (Settings -> WireGuard VPN), shared by every connection editor (DVMDB
// sources, the map server, MAP cascading upstreams). Builds with the
// embedded engine only (AOWIS_GUI_WIREGUARD_PROBE).
#ifdef AOWIS_GUI_WIREGUARD_PROBE

#include <QString>

#include <functional>

class QWidget;

// Asks for a WireGuard .conf file, validates it with AOWIS-NETWORK's parser
// and stores its text centrally (the path is not kept). imported(profile_id)
// is called once the new profile is stored; a profile the embedded engine
// cannot run is never stored and a warning is shown instead.
void importWireGuardProfileFromFile(QWidget *parent, std::function<void(const QString &)> imported);

#endif // AOWIS_GUI_WIREGUARD_PROBE

#endif // WIREGUARD_PROFILE_IMPORT_H
