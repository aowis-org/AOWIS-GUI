#include "map/core/gui_map_provider_catalog.h"

#include "config/gui_preferences.h"
#include "services/map_server_transport_factory.h"

#include <QHash>
#include <QJsonObject>
#include <QTimer>

namespace
{
void configureCatalog(MapProviderCatalog *catalog)
{
    catalog->setServer(guiMapServerRestConfiguration());
    // Personal fallbacks: GUI preferences group "map_provider_fallback",
    // provider UUID -> fallback provider UUID (natively and in browsers).
    const QString group = QStringLiteral("map_provider_fallback");
    QHash<QString, QString> fallbacks;
    const QJsonObject stored = GuiPreferences::group(group);
    for (auto entry = stored.constBegin(); entry != stored.constEnd(); ++entry)
        if (entry.value().isString() && !entry.value().toString().isEmpty())
            fallbacks.insert(entry.key(), entry.value().toString());
    catalog->setFallbackStore(fallbacks, [group](const QString &provider_id, const QString &fallback_id)
    {
        if (fallback_id.isEmpty())
            return GuiPreferences::update(group, QJsonObject(), {provider_id});
        return GuiPreferences::update(group, QJsonObject{{provider_id, fallback_id}});
    });
#ifndef AOWIS_STANDALONE
    QTimer::singleShot(0, catalog, [catalog]() { catalog->refresh(); });
#endif
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
