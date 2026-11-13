#include "config/map_upstream_wireguard_references.h"

#ifdef AOWIS_STANDALONE

#include <aowis/map/map_cascade_config.h>
#include <aowis/map/settings_repository.h>

#include <QUrl>

#include <memory>

bool readMapUpstreamWireGuardReferences(QList<MapUpstreamWireGuardReference> *references,
                                        QString *error)
{
    references->clear();
    QString read_error;
    const std::unique_ptr<Aowis::Map::SettingsRepository> repository =
        Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&read_error);
    Aowis::Map::MapCascadeConfig cascade;
    if (repository == nullptr || !repository->readCascade(&cascade, nullptr, &read_error))
    {
        if (error != nullptr)
            *error = read_error.isEmpty() ? QStringLiteral("MAP cascade settings unavailable") : read_error;
        return false;
    }
    for (const Aowis::Map::MapCascadeUpstream &upstream : cascade.upstreams)
        if (Aowis::Map::requiresWireGuardTransport(upstream))
            references->append({upstream.name, upstream.url.toString(QUrl::FullyEncoded),
                                upstream.wireguard_profile_id});
    return true;
}

#endif // AOWIS_STANDALONE
