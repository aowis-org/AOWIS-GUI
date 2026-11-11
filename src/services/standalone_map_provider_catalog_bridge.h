#ifndef STANDALONE_MAP_PROVIDER_CATALOG_BRIDGE_H
#define STANDALONE_MAP_PROVIDER_CATALOG_BRIDGE_H

#include "services/map_server_rest_configuration.h"

#include <aowis/map/map_provider.h>

#include <QObject>
#include <QTcpServer>
#include <QVector>

// The renderer's provider catalog speaks GET/PUT /map/v1/providers. Standalone
// has no public MAP HTTP server, so expose that *one* catalog endpoint on an
// ephemeral authenticated loopback port, backed by AOWIS-CONFIG and MapTiles.
class StandaloneMapProviderCatalogBridge final : public QObject
{
    Q_OBJECT
public:
    static StandaloneMapProviderCatalogBridge &instance();
    MapServerRestConfiguration catalogServer() const;
    const QVector<Aowis::Map::MapProvider> &providers() const { return this->map_providers; }

signals:
    void providersChanged();

private:
    explicit StandaloneMapProviderCatalogBridge(QObject *parent);
    void acceptConnections();
    void respond(class QTcpSocket *socket, const QByteArray &request);
    QByteArray catalogJson() const;
    bool storeCatalog(const QByteArray &body, QString *error);

    QTcpServer listener;
    QByteArray access_token;
    QVector<Aowis::Map::MapProvider> map_providers;
};

#endif
