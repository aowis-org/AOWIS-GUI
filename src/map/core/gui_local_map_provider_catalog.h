#ifndef GUI_LOCAL_MAP_PROVIDER_CATALOG_H
#define GUI_LOCAL_MAP_PROVIDER_CATALOG_H

#include "map/core/map_provider_catalog.h"

#ifdef AOWIS_STANDALONE
class StandaloneMapProviderCatalog;

// Standalone uses a typed local adapter; remote builds retain Render's REST
// catalog. Both use the same provider entry type and fallback resolver.
class GuiLocalMapProviderCatalog final : public QObject, public MapImageryProviderDirectory
{
    Q_OBJECT
public:
    explicit GuiLocalMapProviderCatalog(StandaloneMapProviderCatalog &backend, QObject *parent = nullptr);
    void setFallbackStore(const QHash<QString, QString> &fallbacks, MapProviderCatalog::FallbackSave save);
    const QVector<MapProviderEntry> &providers() const { return this->entries; }
    bool serverLoaded() const;
    const MapProviderEntry *provider(const QString &id) const;
    QString fallback(const QString &id) const { return this->fallbacks.value(id); }
    bool setFallback(const QString &id, const QString &target_id);
    QString sourceForZoom(const QString &id, int zoom) const override;
    int maximumSourceZoom(const QString &id) const override;
    bool isProviderEnabled(const QString &id) const override;
    QString firstEnabledProviderId() const override;
    bool sourceZoomRange(const QString &id, int *minimum, int *maximum) const override;
    QString builtinProviderId(MapProvider provider) const override;
    void refresh();
    void saveProviders(const QVector<MapProviderEntry> &providers);
signals:
    void catalogChanged();
    void fallbackChanged();
    void errorOccurred(const QString &message);
    void providersSaved();
private:
    StandaloneMapProviderCatalog &backend;
    QVector<MapProviderEntry> entries;
    QHash<QString, QString> fallbacks;
    MapProviderCatalog::FallbackSave save_fallback;
};
#else
using GuiLocalMapProviderCatalog = MapProviderCatalog;
#endif
#endif
