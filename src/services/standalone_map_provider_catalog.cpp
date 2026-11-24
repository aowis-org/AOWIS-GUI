#include "services/standalone_map_provider_catalog.h"

#include "services/map_upstream_tunnels.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>

namespace
{
bool fail(QString *error, const QString &message)
{
    if (error != nullptr) *error = message;
    return false;
}
}

StandaloneMapProviderCatalog &StandaloneMapProviderCatalog::instance()
{
    static StandaloneMapProviderCatalog *catalog =
        new StandaloneMapProviderCatalog(QCoreApplication::instance());
    return *catalog;
}

StandaloneMapProviderCatalog::StandaloneMapProviderCatalog(QObject *parent)
    : QObject(parent)
{
    this->repository = Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(
        &this->catalog_error);
    this->catalog_valid = this->repository != nullptr &&
        this->repository->readProviders(&this->map_providers,
                                        &this->provider_revision, &this->catalog_error);
    if (!this->catalog_valid)
    {
        // Do not silently restore fetching defaults on an unreadable catalog.
        qCritical().noquote() << "Invalid embedded MAP provider catalog:" << this->catalog_error;
        this->map_providers = Aowis::Map::defaultMapProviders();
        for (Aowis::Map::MapProvider &provider : this->map_providers)
            provider.remote_fetch_enabled = false;
    }
    const bool providers_valid = this->catalog_valid;
    Aowis::Map::MapCascadeConfig cascade;
    cascade.allow_direct_fetch = false;
    if (this->repository != nullptr)
    {
        QString error;
        if (!this->repository->readCascade(&cascade, nullptr, &error))
        {
            qCritical().noquote() << "Invalid MAP cascade settings:" << error;
            this->catalog_valid = false;
            this->catalog_error = error;
            cascade = Aowis::Map::MapCascadeConfig();
            cascade.allow_direct_fetch = false;
        }
        if (!this->repository->readDisabledInheritedProviders(
                &this->disabled_inherited, &this->inherited_revision, &error))
        {
            qCritical().noquote() << "Invalid inherited MAP overrides:" << error;
            this->policy_records_valid = false;
            this->catalog_error = error;
            // Do not accidentally re-enable inherited sources when the local
            // persisted deny-list is unreadable. Keep local providers usable.
            this->upstreams_blocked = true;
            this->disabled_inherited.clear();
        }
        if (!this->repository->readProviderAcquisitionModes(
                &this->acquisition_modes, &this->acquisition_modes_revision, &error))
        {
            qCritical().noquote() << "Invalid MAP provider acquisition modes:" << error;
            this->policy_records_valid = false;
            this->catalog_error = error;
            // Fail closed on corrupt mode policy.
            this->upstreams_blocked = true;
            this->direct_fetch_blocked = true;
            this->acquisition_modes.clear();
        }
        if (!this->repository->readProviderUpstreamRoutes(
                &this->upstream_routes, &this->upstream_routes_revision, &error))
        {
            qCritical().noquote() << "Invalid MAP provider routes:" << error;
            this->policy_records_valid = false;
            this->catalog_error = error;
            this->upstreams_blocked = true; // Cannot safely apply corrupt routing policy.
            this->upstream_routes.clear();
        }
    }
    this->providers_valid = providers_valid;
    this->catalog_valid = this->catalog_valid && this->policy_records_valid;
    cascade = this->restrictedCascade(cascade);
    this->effective_cascade = cascade;
    // WireGuard upstreams get their tunnel's proxy once it runs; until then
    // MAP skips them (never direct).
    MapUpstreamTunnels &tunnels = MapUpstreamTunnels::instance();
    tunnels.setCascade(cascade);
    this->applied_cascade = tunnels.resolved();
    this->available_catalog.reset(new Aowis::Map::ProviderCatalog(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("maptiles")), this));
    this->available_catalog->configure(this->map_providers, this->applied_cascade);
    connect(&tunnels, &MapUpstreamTunnels::resolvedChanged,
            this, &StandaloneMapProviderCatalog::applyResolvedCascade);
    this->available_catalog->setDisabledInheritedProviders(this->disabled_inherited);
    this->available_catalog->setUpstreamRoutes(this->upstream_routes);
    this->available_catalog->setAcquisitionModes(this->acquisition_modes);
    connect(this->available_catalog.get(), &Aowis::Map::ProviderCatalog::catalogChanged,
            this, &StandaloneMapProviderCatalog::providersChanged);
    this->available_catalog->refresh();
}

Aowis::Map::MapCascadeConfig StandaloneMapProviderCatalog::restrictedCascade(
    Aowis::Map::MapCascadeConfig cascade) const
{
    if (this->upstreams_blocked) cascade.upstreams.clear();
    if (this->direct_fetch_blocked) cascade.allow_direct_fetch = false;
    return cascade;
}

bool StandaloneMapProviderCatalog::reloadCascade(QString *error)
{
    Aowis::Map::MapCascadeConfig stored;
    QString read_error;
    if (this->repository == nullptr)
        read_error = this->catalog_error.isEmpty()
            ? QStringLiteral("MAP settings repository unavailable") : this->catalog_error;
    else if (this->repository->readCascade(&stored, nullptr, &read_error))
        read_error.clear();
    else if (read_error.isEmpty())
        read_error = QStringLiteral("Invalid MAP cascade settings");
    if (!read_error.isEmpty())
    {
        // Keep the last working configuration.
        if (error != nullptr) *error = read_error;
        return false;
    }
    // A repaired cascade record makes the catalog editable again unless
    // another MAP record is still unreadable.
    const bool was_valid = this->catalog_valid;
    this->catalog_valid = this->providers_valid && this->policy_records_valid;
    if (was_valid != this->catalog_valid) emit localProvidersChanged();
    const Aowis::Map::MapCascadeConfig effective = this->restrictedCascade(stored);
    if (effective == this->effective_cascade)
        return true;
    this->effective_cascade = effective;
    MapUpstreamTunnels::instance().setCascade(effective);
    this->applyResolvedCascade();
    return true;
}

void StandaloneMapProviderCatalog::applyResolvedCascade()
{
    const Aowis::Map::MapCascadeConfig resolved = MapUpstreamTunnels::instance().resolved();
    if (resolved == this->applied_cascade)
        return;
    this->applied_cascade = resolved;
    if (this->available_catalog != nullptr)
    {
        // Validated offline snapshots of the remaining upstreams are reused;
        // discovery refreshes in the background (providersChanged follows).
        this->available_catalog->configure(this->map_providers, resolved);
        this->available_catalog->refresh();
    }
    emit cascadeChanged();
}

QVector<Aowis::Map::AvailableMapProvider> StandaloneMapProviderCatalog::availableEntries() const
{
    return this->available_catalog->available();
}

void StandaloneMapProviderCatalog::refreshAvailable()
{
    this->available_catalog->refresh();
}

QString StandaloneMapProviderCatalog::errorMessage() const
{
    return this->catalog_error.isEmpty() ? QStringLiteral("MAP provider repository unavailable") :
                                         this->catalog_error;
}

bool StandaloneMapProviderCatalog::checkWritable(QString *error) const
{
    if (error != nullptr) error->clear();
    return (this->catalog_valid && this->repository != nullptr) || fail(error, this->errorMessage());
}

QVector<Aowis::Map::MapProvider> StandaloneMapProviderCatalog::availableProviders() const
{
    QVector<Aowis::Map::MapProvider> result;
    if (this->available_catalog == nullptr) return this->map_providers;
    for (const Aowis::Map::AvailableMapProvider &entry : this->available_catalog->available())
        result.append(entry.provider);
    return result;
}

QHash<QString, QSet<QString>> StandaloneMapProviderCatalog::inheritedUpstreams() const
{
    return this->available_catalog == nullptr ? QHash<QString, QSet<QString>>() :
        this->available_catalog->inheritedUpstreams();
}

bool StandaloneMapProviderCatalog::saveProviders(
    const QVector<Aowis::Map::MapProvider> &providers, QString *error)
{
    if (!this->checkWritable(error)) return false;
    // Match MAP's canonical representation. MAP validates and persists the
    // typed record atomically; no REST payload is constructed by this adapter.
    QVector<Aowis::Map::MapProvider> normalized = providers;
    for (Aowis::Map::MapProvider &provider : normalized)
    {
        provider.id = provider.id.trimmed().toLower();
        provider.name = provider.name.trimmed();
        provider.url_template = provider.url_template.trimmed();
        provider.coverage = provider.coverage.trimmed();
    }
    QString revision;
    if (!this->repository->saveProviders(normalized, this->provider_revision, &revision, error))
        return false;
    this->provider_revision = revision;
    this->map_providers = normalized;
    this->available_catalog->setLocalProviders(normalized);
    emit localProvidersChanged();
    return true;
}

bool StandaloneMapProviderCatalog::setInheritedEnabled(const QString &id, bool enabled, QString *error)
{
    if (!this->checkWritable(error)) return false;
    bool found = false;
    for (const Aowis::Map::AvailableMapProvider &entry : this->availableEntries())
        if (entry.inherited && entry.provider.id == id) found = true;
    if (!found) return fail(error, QStringLiteral("Provider is not inherited"));
    QSet<QString> disabled = this->disabled_inherited;
    if (enabled) disabled.remove(id);
    else disabled.insert(id);
    if (disabled == this->disabled_inherited) return true;
    QString revision;
    if (!this->repository->saveDisabledInheritedProviders(
            disabled, this->inherited_revision, &revision, error)) return false;
    this->inherited_revision = revision;
    this->disabled_inherited = disabled;
    this->available_catalog->setDisabledInheritedProviders(disabled);
    return true;
}

bool StandaloneMapProviderCatalog::setUpstreamRoute(
    const QString &id, const QStringList &names, QString *error)
{
    if (!this->checkWritable(error)) return false;
    QStringList advertised;
    bool found = false;
    for (const Aowis::Map::AvailableMapProvider &entry : this->availableEntries())
        if (entry.provider.id == id)
        {
            advertised = entry.upstream_names;
            found = true;
            break;
        }
    if (!found) return fail(error, QStringLiteral("Unknown provider"));
    QSet<QString> seen;
    for (const QString &name : names)
    {
        if (!advertised.contains(name) || seen.contains(name))
            return fail(error, QStringLiteral("Unknown or duplicate upstream"));
        seen.insert(name);
    }
    QHash<QString, QStringList> updated = this->upstream_routes;
    if (names.isEmpty()) updated.remove(id);
    else updated.insert(id, names);
    if (updated == this->upstream_routes) return true;
    QString revision;
    if (!this->repository->saveProviderUpstreamRoutes(
            updated, this->upstream_routes_revision, &revision, error)) return false;
    this->upstream_routes_revision = revision;
    this->upstream_routes = updated;
    this->available_catalog->setUpstreamRoutes(updated);
    return true;
}

bool StandaloneMapProviderCatalog::setAcquisitionMode(
    const QString &id, Aowis::Map::MapProviderAcquisitionMode mode, QString *error)
{
    using Mode = Aowis::Map::MapProviderAcquisitionMode;
    if (!this->checkWritable(error)) return false;
    if (mode != Mode::Automatic && mode != Mode::Cascade && mode != Mode::Direct)
        return fail(error, QStringLiteral("Invalid acquisition mode"));
    bool found = false;
    bool inherited = false;
    bool direct_allowed = false;
    for (const Aowis::Map::AvailableMapProvider &entry : this->availableEntries())
        if (entry.provider.id == id)
        {
            found = true;
            inherited = entry.inherited;
            direct_allowed = entry.direct_fetch_allowed;
            break;
        }
    if (!found) return fail(error, QStringLiteral("Unknown provider"));
    if (mode == Mode::Direct && !direct_allowed)
        return fail(error, QStringLiteral("Direct acquisition forbidden"));
    QHash<QString, Mode> updated = this->acquisition_modes;
    const Mode default_mode = inherited ? Mode::Cascade : Mode::Automatic;
    if (mode == default_mode) updated.remove(id);
    else updated.insert(id, mode);
    if (updated == this->acquisition_modes) return true;
    QString revision;
    if (!this->repository->saveProviderAcquisitionModes(
            updated, this->acquisition_modes_revision, &revision, error)) return false;
    this->acquisition_modes_revision = revision;
    this->acquisition_modes = updated;
    this->available_catalog->setAcquisitionModes(updated);
    return true;
}
