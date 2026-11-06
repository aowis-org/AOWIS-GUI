#ifndef MAP_PROVIDER_CATALOG_H
#define MAP_PROVIDER_CATALOG_H

#include "map/core/map_imagery_provider_directory.h"

#include <QObject>
#include <QNetworkAccessManager>
#include <QHash>
#include <QString>
#include <QVector>

struct MapProviderEntry
{
    QString id;
    QString name;
    QString url;
    int minimum_zoom = 0;
    int maximum_zoom = 19;
    bool enabled = true;
    bool remote_fetch_enabled = true;
    QString coverage;
};

// Server owns provider definitions; the GUI owns each user's fallback choices.
// It is also the MapModel's (Render) view of the providers: main_window passes
// mapProviderCatalog() to the model and forwards catalogChanged() /
// fallbackChanged() to MapModel::providerCatalogChanged() / providerFallbackChanged().
class MapProviderCatalog : public QObject, public MapImageryProviderDirectory
{
    Q_OBJECT
public:
    explicit MapProviderCatalog(QObject *parent = nullptr);
    const QVector<MapProviderEntry> &providers() const;
    bool serverLoaded() const;
    const MapProviderEntry *provider(const QString &id) const;
    QString fallback(const QString &provider_id) const;
    bool setFallback(const QString &provider_id, const QString &fallback_id);
    QString sourceForZoom(const QString &selected_id, int zoom) const override;
    int maximumSourceZoom(const QString &selected_id) const override;
    bool isProviderEnabled(const QString &provider_id) const override;
    QString firstEnabledProviderId() const override;
    bool sourceZoomRange(const QString &source, int *minimum_zoom,
                         int *maximum_zoom) const override;
    QString builtinProviderId(MapProvider provider) const override;
    void refresh();
    void saveProviders(const QVector<MapProviderEntry> &entries);
    static QString builtinId(int provider_enum);
signals:
    void catalogChanged();
    void fallbackChanged();
    void errorOccurred(const QString &message);
    void providersSaved();
private:
    QVector<MapProviderEntry> entries;
    QHash<QString, QString> fallback_cache;
    bool server_loaded = false;
    QNetworkAccessManager network_manager;
};

MapProviderCatalog &mapProviderCatalog();

#endif
