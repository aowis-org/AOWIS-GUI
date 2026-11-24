#ifndef MAP_INHERITED_PROVIDER_CLIENT_H
#define MAP_INHERITED_PROVIDER_CLIENT_H

#include "services/map_server_rest_configuration.h"

#include <QObject>
#include <QNetworkAccessManager>
#include <QStringList>
#include <QVector>

// Available-provider view and policy editor. Remote builds use REST;
// Standalone uses the embedded MAP library adapter directly.
#ifdef AOWIS_STANDALONE
class StandaloneMapProviderCatalog;
#endif
struct MapInheritedProviderEntry
{
    QString id;
    QString name;
    QString url;
    QStringList upstreams;
    int minimum_zoom = 0;
    int maximum_zoom = 19;
    bool enabled = true;
};

struct MapProviderRouteEntry
{
    QString id;
    QString name;
    QStringList upstreams; // Servers advertising this source.
    QStringList selected_upstreams; // Effective priority and subset.
    bool inherited = false;
    QString acquisition_mode = QStringLiteral("automatic");
    bool direct_fetch_allowed = false;
};

class MapInheritedProviderClient final : public QObject
{
    Q_OBJECT
public:
    explicit MapInheritedProviderClient(QObject *parent = nullptr);
    void setServer(const MapServerRestConfiguration &server);
#ifdef AOWIS_STANDALONE
    void setEmbeddedCatalog(StandaloneMapProviderCatalog &catalog);
#endif
    void refresh();
    void setEnabled(const QString &id, bool enabled);
    void setUpstreamRoute(const QString &id, const QStringList &upstream_names);
    void setAcquisitionMode(const QString &id, const QString &mode);
    const QVector<MapProviderRouteEntry> &routes() const { return this->route_entries; }
    const QVector<MapInheritedProviderEntry> &providers() const { return this->entries; }
    bool loaded() const { return this->has_catalog; }
    int localProviderCount() const { return this->local_provider_count; }
    int locallyDefinedWithUpstreams() const { return this->local_with_upstreams; }
    bool canEdit() const;
    bool updating() const { return this->change_pending; }

signals:
    void catalogChanged();
    void updateFinished(bool success, const QString &message);
    void errorOccurred(const QString &message);

private:
#ifdef AOWIS_STANDALONE
    void readEmbeddedCatalog();
    void completeEmbeddedUpdate(bool success, const QString &error);
    StandaloneMapProviderCatalog *embedded = nullptr;
    QMetaObject::Connection embedded_connection;
    QMetaObject::Connection embedded_local_connection;
#endif
    MapServerRestConfiguration map_server;
    QNetworkAccessManager network;
    QVector<MapInheritedProviderEntry> entries;
    QVector<MapProviderRouteEntry> route_entries;
    quint64 request_generation = 0;
    bool has_catalog = false;
    int local_provider_count = 0;
    int local_with_upstreams = 0;
    bool change_pending = false;
};

#endif // MAP_INHERITED_PROVIDER_CLIENT_H
