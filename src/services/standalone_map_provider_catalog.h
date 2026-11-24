#ifndef STANDALONE_MAP_PROVIDER_CATALOG_H
#define STANDALONE_MAP_PROVIDER_CATALOG_H

#include <aowis/map/map_provider.h>
#include <aowis/map/provider_catalog.h>
#include <aowis/map/settings_repository.h>
#include <memory>

#include <QObject>
#include <QVector>

// In-process owner of the embedded MAP provider catalog and cascade policy.
// All persistence and upstream discovery use MAP library APIs. GUI thread only.
class StandaloneMapProviderCatalog final : public QObject
{
    Q_OBJECT
public:
    static StandaloneMapProviderCatalog &instance();
    QVector<Aowis::Map::AvailableMapProvider> availableEntries() const;
    bool saveProviders(const QVector<Aowis::Map::MapProvider> &providers, QString *error);
    bool setInheritedEnabled(const QString &id, bool enabled, QString *error);
    bool setUpstreamRoute(const QString &id, const QStringList &names, QString *error);
    bool setAcquisitionMode(const QString &id, Aowis::Map::MapProviderAcquisitionMode mode,
                            QString *error);
    void refreshAvailable();
    QString errorMessage() const;
    const QVector<Aowis::Map::MapProvider> &providers() const { return this->map_providers; }
    QVector<Aowis::Map::MapProvider> availableProviders() const;
    QHash<QString, QSet<QString>> inheritedUpstreams() const;
    QHash<QString, QStringList> upstreamRoutes() const { return this->upstream_routes; }
    QHash<QString, Aowis::Map::MapProviderAcquisitionMode> acquisitionModes() const
    { return this->acquisition_modes; }
    bool isCatalogValid() const { return this->catalog_valid; }
    // The cascade the embedded map server applies: the stored record, with
    // upstreams and/or direct fetching removed while another MAP record is
    // unreadable (fail closed), and the tunnel proxy of every WireGuard
    // upstream whose tunnel runs (MapUpstreamTunnels).
    Aowis::Map::MapCascadeConfig cascade() const { return this->applied_cascade; }
    // Re-reads the stored cascade record and applies it live: provider
    // discovery is reconfigured and cascadeChanged() is emitted when the
    // effective cascade differs. An unreadable record keeps the last working
    // configuration and returns false with the reason.
    bool reloadCascade(QString *error);

signals:
    void providersChanged();
    void localProvidersChanged();
    // cascade() changed: reloadCascade(), or a WireGuard upstream's tunnel
    // started.
    void cascadeChanged();

private:
    explicit StandaloneMapProviderCatalog(QObject *parent);
    bool checkWritable(QString *error) const;

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

    void applyResolvedCascade();

    Aowis::Map::MapCascadeConfig effective_cascade; // Stored, restricted.
    Aowis::Map::MapCascadeConfig applied_cascade;   // With tunnel proxies.
    bool catalog_valid = false;
    bool providers_valid = false;
    bool policy_records_valid = true;
    bool upstreams_blocked = false;
    bool direct_fetch_blocked = false;
    QString catalog_error;
};

#endif
