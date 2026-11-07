#ifndef GUI_MAP_PROVIDER_CATALOG_H
#define GUI_MAP_PROVIDER_CATALOG_H

#include "map/core/map_provider_catalog.h"

// The GUI's one provider catalog (Render's MapProviderCatalog), configured
// with the map server of the GUI configuration and the user's fallback
// choices stored in the GUI configuration file. Refreshed once at startup
// (not in standalone builds).
MapProviderCatalog &mapProviderCatalog();

#endif // GUI_MAP_PROVIDER_CATALOG_H
