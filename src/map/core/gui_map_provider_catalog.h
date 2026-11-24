#ifndef GUI_MAP_PROVIDER_CATALOG_H
#define GUI_MAP_PROVIDER_CATALOG_H

#include "map/core/gui_local_map_provider_catalog.h"

// Editable local provider catalog. MapModel and the navigation selector
// use guiAvailableMapProviderDirectory() for inherited providers. MAP owns
// provider definitions; GUI preferences store only per-user fallback choices.
GuiLocalMapProviderCatalog &mapProviderCatalog();

#endif // GUI_MAP_PROVIDER_CATALOG_H
