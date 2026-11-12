#include "map/core/gui_map_provider_catalog.h"
#include "map/core/gui_available_map_provider_directory.h"
#include "services/map_inherited_provider_client.h"

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
GuiAvailableMapProviderDirectory &guiAvailableMapProviderDirectory()
{
    // Keep the editable catalog and the renderer's available-provider view
    // independent. Both obtain their data exclusively through MAP APIs.
    MapProviderCatalog &local = mapProviderCatalog();
    static MapInheritedProviderClient inherited;
    static GuiAvailableMapProviderDirectory directory(local, inherited);
    MapInheritedProviderClient *client = &inherited;
    static bool configured = false;
    if (!configured)
    {
        configured = true;
#ifdef AOWIS_STANDALONE
        StandaloneMapProviderCatalogBridge &bridge =
            StandaloneMapProviderCatalogBridge::instance();
        inherited.setServer(bridge.catalogServer());
        QObject::connect(&bridge, &StandaloneMapProviderCatalogBridge::providersChanged,
                         &directory, [client]() { client->refresh(); });
        inherited.refresh();
#else
        whenGuiMapServerReachable(&directory,
            [client](const MapServerRestConfiguration &server)
        {
            client->setServer(server);
            client->refresh();
        });
#endif
        QTimer *refresh_timer = new QTimer(&directory);
        refresh_timer->setInterval(15 * 60 * 1000);
        QObject::connect(refresh_timer, &QTimer::timeout,
                         &directory, &GuiAvailableMapProviderDirectory::refresh);
        refresh_timer->start();
    }
    return directory;
}
