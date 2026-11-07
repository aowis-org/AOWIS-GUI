#include "map/core/gui_map_provider_catalog.h"

#include "config/gui_configuration.h"
#include "services/map_server_transport_factory.h"

#include <QHash>
#include <QSettings>
#include <QTimer>

namespace
{
void configureCatalog(MapProviderCatalog *catalog)
{
    catalog->setServer(guiMapServerRestConfiguration());
#ifndef __EMSCRIPTEN__
    QHash<QString, QString> fallbacks;
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("map_provider_fallback"));
    for (const QString &key : settings.childKeys())
        fallbacks.insert(key, settings.value(key).toString());
    settings.endGroup();
    catalog->setFallbackStore(fallbacks, [](const QString &provider_id, const QString &fallback_id)
    {
        QSettings store(guiConfigurationFilePath(), QSettings::IniFormat);
        if (fallback_id.isEmpty())
            store.remove(QStringLiteral("map_provider_fallback/") + provider_id);
        else
            store.setValue(QStringLiteral("map_provider_fallback/") + provider_id, fallback_id);
        store.sync();
        return store.status() == QSettings::NoError;
    });
#endif
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
