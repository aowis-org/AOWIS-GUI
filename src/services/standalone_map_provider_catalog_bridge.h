#ifndef STANDALONE_MAP_PROVIDER_CATALOG_BRIDGE_H
#define STANDALONE_MAP_PROVIDER_CATALOG_BRIDGE_H

#include "services/map_server_rest_configuration.h"

#include <aowis/map/map_provider.h>
#include <aowis/map/provider_catalog.h>
#include <aowis/map/settings_repository.h>
#include <memory>

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
    QVector<Aowis::Map::MapProvider> availableProviders() const;
    QHash<QString, QSet<QString>> inheritedUpstreams() const;
    QHash<QString, QStringList> upstreamRoutes() const { return this->upstream_routes; }
    QHash<QString, Aowis::Map::MapProviderAcquisitionMode> acquisitionModes() const
    { return this->acquisition_modes; }
    bool isCatalogValid() const { return this->catalog_valid; }
    // The cascade the embedded map server applies: the stored record, with
    // upstreams and/or direct fetching removed while another MAP record is
    // unreadable (fail closed).
    Aowis::Map::MapCascadeConfig cascade() const { return this->effective_cascade; }
    // Re-reads the stored cascade record and applies it live: provider
    // discovery is reconfigured and cascadeChanged() is emitted when the
    // effective cascade differs. An unreadable record keeps the last working
    // configuration and returns false with the reason.
    bool reloadCascade(QString *error);

signals:
    void providersChanged();
    // The effective cascade changed (reloadCascade()).
    void cascadeChanged();

private:
    explicit StandaloneMapProviderCatalogBridge(QObject *parent);
    void acceptConnections();
    void respond(class QTcpSocket *socket, const QByteArray &request);
    QByteArray catalogJson() const;
    bool storeCatalog(const QByteArray &body, QString *error);

    QTcpServer listener;
    QByteArray access_token;
    QVector<Aowis::Map::MapProvider> map_providers;
    std::unique_ptr<Aowis::Map::SettingsRepository> repository;
    std::unique_ptr<Aowis::Map::ProviderCatalog> available_catalog;
    QSet<QString> disabled_inherited;
    QHash<QString, QStringList> upstream_routes;
    QHash<QString, Aowis::Map::MapProviderAcquisitionMode> acquisition_modes;
    QString acquisition_modes_revision;
    QString upstream_routes_revision;
    QString inherited_revision;
    QString provider_revision;
    Aowis::Map::MapCascadeConfig restrictedCascade(Aowis::Map::MapCascadeConfig cascade) const;

    Aowis::Map::MapCascadeConfig effective_cascade;
    bool catalog_valid = false;
    bool providers_valid = false;
    bool policy_records_valid = true;
    bool upstreams_blocked = false;
    bool direct_fetch_blocked = false;
    QString catalog_error;
};

#endif
