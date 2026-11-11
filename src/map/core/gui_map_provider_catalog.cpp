#include "map/core/gui_map_provider_catalog.h"

#include "config/gui_preferences.h"
#include "services/map_server_transport_factory.h"
#ifdef AOWIS_STANDALONE
#include "services/standalone_map_provider_catalog_bridge.h"
#endif

#include <QHash>
#include <QJsonObject>
#include <QTimer>

namespace
{
void configureCatalog(MapProviderCatalog *catalog)
{
#ifdef AOWIS_STANDALONE
    // Standalone's provider editor uses a private, loopback-only catalog
    // endpoint. Never use the unrelated remote MAP connection for local edits.
    catalog->setServer(StandaloneMapProviderCatalogBridge::instance().catalogServer());
    catalog->refresh();
#else
    // A WireGuard map-server connection may only become reachable later.
    whenGuiMapServerReachable(catalog, [catalog](const MapServerRestConfiguration &server)
    {
        catalog->setServer(server);
        catalog->refresh();
    });
#endif
    // Personal fallbacks: GUI preferences group "map_provider_fallback",
    // provider UUID -> fallback provider UUID (natively and in browsers).
    const QString group = QStringLiteral("map_provider_fallback");
    QHash<QString, QString> fallbacks;
    const QJsonObject stored = GuiPreferences::group(group);
    for (QJsonObject::const_iterator entry = stored.constBegin(); entry != stored.constEnd(); ++entry)
        if (entry.value().isString() && !entry.value().toString().isEmpty())
            fallbacks.insert(entry.key(), entry.value().toString());
    catalog->setFallbackStore(fallbacks, [group](const QString &provider_id, const QString &fallback_id)
    {
        if (fallback_id.isEmpty())
            return GuiPreferences::update(group, QJsonObject(), {provider_id});
        return GuiPreferences::update(group, QJsonObject{{provider_id, fallback_id}});
    });
}
}

MapProviderCatalog &mapProviderCatalog()
{
    static MapProviderCatalog catalog;
    static bool configured = false;
    if (!configured)
    {
        configured = true;
        configureCatalog(&catalog);
    }
    return catalog;
}
