#include "services/standalone_map_provider_catalog.h"
#include "map/core/gui_local_map_provider_catalog.h"
#include "map/core/gui_available_map_provider_directory.h"
#include "services/map_inherited_provider_client.h"
#include <aowis/map/settings_repository.h>
#include <aowis/map/provider_catalog.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>
#include <functional>
#include <memory>

namespace
{
bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

constexpr const char *InheritedProviderId = "15c2d930-1e4a-4b76-9675-31ea802989fa";

bool waitUntil(const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 4000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
        QThread::msleep(1);
    }
    return ready();
}

// Real MAP's ProviderCatalog performs authenticated HTTP discovery. Exercise
// that pipeline rather than populating the Standalone catalog with fake rows.
class TestUpstream final : public QObject
{
public:
    TestUpstream(const QByteArray &key, const QByteArray &catalog, QObject *parent = nullptr)
        : QObject(parent), expected_key(key), catalog_payload(catalog), listener(this)
    {
        QObject::connect(&this->listener, &QTcpServer::newConnection, this, [this]()
        {
            while (this->listener.hasPendingConnections())
            {
                QTcpSocket *socket = this->listener.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                    [this, socket, buffered = QByteArray()]() mutable
                {
                    buffered += socket->readAll();
                    const int end = buffered.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray headers = buffered.left(end);
                    ++this->requests;
                    const bool authorized = headers.toLower().contains(
                        QByteArray("x-api-key: ") + this->expected_key.toLower());
                    const bool requested_catalog = headers.startsWith(
                        "GET /map/v1/providers/available ");
                    if (authorized && requested_catalog) ++this->authorized_requests;
                    const QByteArray payload = authorized && requested_catalog ?
                        this->catalog_payload : QByteArray("{}");
                    const QByteArray status = authorized && requested_catalog ?
                        QByteArray("200 OK") : QByteArray("403 Forbidden");
                    socket->write(QByteArray("HTTP/1.1 ") + status +
                        "\r\nContent-Type: application/json\r\nConnection: close\r\n"
                        "Content-Length: " + QByteArray::number(payload.size()) +
                        "\r\n\r\n" + payload);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected,
                                 socket, &QObject::deleteLater);
            }
        });
    }

    bool start() { return this->listener.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(this->listener.serverPort()));
    }
    void stop() { this->listener.close(); }

    int requests = 0;
    int authorized_requests = 0;

private:
    QByteArray expected_key;
    QByteArray catalog_payload;
    QTcpServer listener;
};

Aowis::Map::AvailableMapProvider providerById(const StandaloneMapProviderCatalog &catalog,
                                                const QString &id)
{
    for (const Aowis::Map::AvailableMapProvider &entry : catalog.availableEntries())
        if (entry.provider.id == id) return entry;
    return {};
}

}

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary root")) return 1;
    qputenv("XDG_CONFIG_HOME", root.path().toUtf8());
    qputenv("XDG_DATA_HOME", root.path().toUtf8());
    QCoreApplication application(argc, argv);
    // Standalone must respect its MAP-owned administrator acquisition policy.
    QString setup_error;
    std::unique_ptr<Aowis::Map::SettingsRepository> setup_repository =
        Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&setup_error);
    if (!expect(setup_repository != nullptr, "Standalone MAP settings repository")) return 1;
    // Create two actual upstream HTTP endpoints advertising an identical
    // provider that is absent from Standalone's editable local catalog.
    Aowis::Map::MapProvider inherited_provider = Aowis::Map::defaultMapProviders().first();
    const QString inherited_id = QString::fromLatin1(InheritedProviderId);
    inherited_provider.id = inherited_id;
    inherited_provider.name = QStringLiteral("Remote project imagery");
    inherited_provider.url_template = QStringLiteral(
        "https://tiles.example.org/project/{z}/{x}/{y}.png");
    const QByteArray upstream_catalog = Aowis::Map::encodeProviderCatalog(
        QVector<Aowis::Map::MapProvider>{inherited_provider});
    TestUpstream primary("primary-read", upstream_catalog);
    TestUpstream backup("backup-read", upstream_catalog);
    if (!expect(primary.start() && backup.start(), "upstream HTTP listeners")) return 1;

    Aowis::Map::MapCascadeConfig cascade;
    cascade.allow_direct_fetch = false;
    cascade.upstreams.append({QStringLiteral("primary"), primary.url(), QByteArray("primary-read")});
    cascade.upstreams.append({QStringLiteral("backup"), backup.url(), QByteArray("backup-read")});
    if (!expect(setup_repository->saveCascade(cascade, {}, nullptr, &setup_error),
                "persist ordered upstreams and direct-fetch prohibition")) return 1;
    QVector<Aowis::Map::MapProvider> initial;
    QString initial_revision;
    if (!expect(setup_repository->readProviders(&initial, &initial_revision, &setup_error) &&
                !initial.isEmpty(), "read initial providers")) return 1;
    initial.first().allow_lossy_transfer = false;
    if (!expect(setup_repository->saveProviders(initial, initial_revision, nullptr, &setup_error),
                "persist lossless policy")) return 1;
    StandaloneMapProviderCatalog &bridge = StandaloneMapProviderCatalog::instance();
    if (!expect(waitUntil([&bridge, &inherited_id]()
    {
        // Wait for both independent HTTP responses, not just the first one.
        const QSet<QString> sources = bridge.inheritedUpstreams().value(inherited_id);
        return sources.contains(QStringLiteral("primary")) &&
               sources.contains(QStringLiteral("backup"));
    }), "Standalone MAP discovers provider from both upstreams")) return 1;
    if (!expect(primary.authorized_requests > 0 && backup.authorized_requests > 0,
                "upstream credentials forwarded for both catalogs")) return 1;
    using Mode = Aowis::Map::MapProviderAcquisitionMode;
    const qsizetype local_count = Aowis::Map::defaultMapProviders().size();
    GuiLocalMapProviderCatalog local(bridge);
    MapInheritedProviderClient inherited;
    inherited.setEmbeddedCatalog(bridge);
    GuiAvailableMapProviderDirectory directory(local, inherited);
    if (!expect(local.serverLoaded() && inherited.loaded() && inherited.canEdit() &&
                local.providers().size() == local_count &&
                directory.providers().size() == local_count + 1 &&
                directory.isInherited(inherited_id) && directory.isProviderEnabled(inherited_id) &&
                inherited.providers().first().upstreams ==
                    QStringList({QStringLiteral("primary"), QStringLiteral("backup")}),
                "typed local and inherited catalogs are available")) return 1;
    QString error;
    const QString first_id = local.providers().first().id;
    if (!expect(!bridge.setInheritedEnabled(first_id, false, &error) && !error.isEmpty(),
                "local provider cannot be changed as inherited")) return 1;
    if (!expect(!bridge.setUpstreamRoute(first_id, {QStringLiteral("nonexistent")}, &error),
                "reject non-advertising upstream")) return 1;
    if (!expect(!bridge.setUpstreamRoute(inherited_id,
                    {QStringLiteral("backup"), QStringLiteral("backup")}, &error),
                "reject duplicate upstream")) return 1;

    // GUI edits persist through MAP's typed API and immediately update the
    // renderer directory; they require no local socket or HTTP request.
    int changed = 0;
    int saved = 0;
    int rejected = 0;
    QObject::connect(&bridge, &StandaloneMapProviderCatalog::providersChanged,
                     &application, [&changed]() { ++changed; });
    QObject::connect(&local, &GuiLocalMapProviderCatalog::providersSaved,
                     &application, [&saved]() { ++saved; });
    QObject::connect(&local, &GuiLocalMapProviderCatalog::errorOccurred,
                     &application, [&rejected](const QString &) { ++rejected; });
    QVector<MapProviderEntry> proposed = local.providers();
    proposed.first().name = QStringLiteral("Edited embedded provider");
    local.saveProviders(proposed);
    if (!expect(saved == 1 && changed == 1 && directory.provider(first_id) != nullptr &&
                directory.provider(first_id)->name == proposed.first().name,
                "direct GUI edit persists and updates the renderer")) return 1;

    // Fallback choices use the existing shared Render zoom resolver.
    QString fallback_source;
    QString fallback_target;
    local.setFallbackStore({}, [&fallback_source, &fallback_target](const QString &id, const QString &target)
    {
        fallback_source = id;
        fallback_target = target;
        return true;
    });
    const QString osm = MapProviderCatalog::builtinId(2);
    const QString topo = MapProviderCatalog::builtinId(3);
    if (!expect(local.setFallback(topo, osm) && fallback_source == topo && fallback_target == osm &&
                directory.sourceForZoom(topo, 18) == osm && directory.maximumSourceZoom(topo) == 19 &&
                !local.setFallback(osm, topo), "local fallback retains shared Render semantics")) return 1;

    inherited.setAcquisitionMode(first_id, QStringLiteral("cascade"));
    if (!expect(waitUntil([&inherited]() { return !inherited.updating(); }) &&
                providerById(bridge, first_id).acquisition_mode == Mode::Cascade,
                "typed acquisition update visible")) return 1;
    if (!expect(!bridge.setAcquisitionMode(first_id, Mode::Direct, &error) && !error.isEmpty() &&
                !bridge.setAcquisitionMode(first_id, static_cast<Mode>(-1), &error),
                "global policy denies direct and invalid modes")) return 1;
    std::unique_ptr<Aowis::Map::SettingsRepository> repository =
        Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&error);
    QHash<QString, Mode> stored_modes;
    QVector<Aowis::Map::MapProvider> stored;
    if (!expect(repository != nullptr && repository->readProviderAcquisitionModes(
                    &stored_modes, nullptr, &error) && stored_modes.value(first_id) == Mode::Cascade &&
                repository->readProviders(&stored, nullptr, &error) &&
                stored.first().name == proposed.first().name && !stored.first().allow_lossy_transfer,
                "typed edit preserves MAP-owned fields and persists acquisition policy")) return 1;
    const int before_invalid = changed;
    proposed.first().url = QStringLiteral("file:///etc/passwd");
    local.saveProviders(proposed);
    QVector<Aowis::Map::MapProvider> persisted_after_invalid;
    if (!expect(rejected == 1 && saved == 1 && changed == before_invalid &&
                local.providers().first().url == stored.first().url_template &&
                repository->readProviders(&persisted_after_invalid, nullptr, &error) &&
                Aowis::Map::encodeProviderCatalog(persisted_after_invalid) ==
                    Aowis::Map::encodeProviderCatalog(stored), "invalid edit rejected atomically")) return 1;

    inherited.setEnabled(inherited_id, false);
    if (!expect(waitUntil([&inherited]() { return !inherited.updating(); }) &&
                !directory.isProviderEnabled(inherited_id), "inherited disable updates renderer")) return 1;
    QSet<QString> disabled_ids;
    if (!expect(repository->readDisabledInheritedProviders(&disabled_ids, nullptr, &error) &&
                disabled_ids.contains(inherited_id), "inherited disable persists")) return 1;
    inherited.setEnabled(inherited_id, true);
    if (!expect(waitUntil([&inherited]() { return !inherited.updating(); }) &&
                directory.isProviderEnabled(inherited_id) &&
                bridge.inheritedUpstreams().value(inherited_id).contains(QStringLiteral("backup")),
                "inherited re-enable restores sources")) return 1;
    inherited.setUpstreamRoute(inherited_id, {QStringLiteral("backup")});
    if (!expect(waitUntil([&inherited]() { return !inherited.updating(); }) &&
                providerById(bridge, inherited_id).selected_upstreams == QStringList{QStringLiteral("backup")},
                "typed routing preserves selected subset")) return 1;
    QHash<QString, QStringList> restored_routes;
    if (!expect(repository->readProviderUpstreamRoutes(&restored_routes, nullptr, &error) &&
                restored_routes.value(inherited_id) == QStringList{QStringLiteral("backup")},
                "route survives repository reopen")) return 1;
    if (!expect(!bridge.setAcquisitionMode(inherited_id, Mode::Direct, &error) &&
                providerById(bridge, inherited_id).acquisition_mode == Mode::Cascade,
                "inherited sources cannot use direct mode")) return 1;

    primary.stop();
    backup.stop();
    bridge.refreshAvailable();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    if (!expect(directory.isProviderEnabled(inherited_id),
                "disconnected upstream retains discovered provider")) return 1;
    // Local editing remains possible while upstreams are down.
    proposed = local.providers();
    proposed.first().name = QStringLiteral("Offline edit");
    local.saveProviders(proposed);
    if (!expect(saved == 2 && local.providers().first().name == proposed.first().name,
                "local editing remains available offline")) return 1;
    // A concurrent MAP repository writer must not be silently overwritten.
    QString revision;
    if (!expect(repository->readProviders(&stored, &revision, &error), "read current revision")) return 1;
    QVector<Aowis::Map::MapProvider> external = stored;
    external.first().name = QStringLiteral("Concurrent edit");
    if (!expect(repository->saveProviders(external, revision, nullptr, &error), "external write")) return 1;
    proposed.first().name = QStringLiteral("Stale edit");
    local.saveProviders(proposed);
    if (!expect(rejected == 2 && saved == 2 && local.providers().first().name == stored.first().name &&
                repository->readProviders(&persisted_after_invalid, nullptr, &error) &&
                persisted_after_invalid.first().name == external.first().name,
                "revision conflict leaves in-memory catalog and persisted data intact")) return 1;
    Aowis::Map::ProviderCatalog restarted(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("maptiles")));
    restarted.configure(stored, cascade);
    bool recovered = false;
    for (const Aowis::Map::AvailableMapProvider &entry : restarted.available())
        if (entry.inherited && entry.provider.id == inherited_id &&
            entry.upstream_names == QStringList({QStringLiteral("primary"),
                                                 QStringLiteral("backup")}))
            recovered = true;
    if (!expect(recovered, "new MAP catalog restores upstream snapshot offline")) return 1;

    // Live reconfiguration (Map settings): replacing the stopped upstreams by
    // a new one applies without recreating the catalog. Discovery uses the
    // new upstream and its key; the removed ones are no longer sources.
    TestUpstream replacement("replacement-read", upstream_catalog);
    if (!expect(replacement.start(), "replacement upstream listener")) return 1;
    Aowis::Map::MapCascadeConfig stored_cascade;
    QString cascade_revision;
    if (!expect(setup_repository->readCascade(&stored_cascade, &cascade_revision, &setup_error),
                "read stored cascade revision")) return 1;
    Aowis::Map::MapCascadeConfig replaced = stored_cascade;
    replaced.upstreams.clear();
    replaced.upstreams.append({QStringLiteral("replacement"), replacement.url(),
                               QByteArray("replacement-read")});
    if (!expect(setup_repository->saveCascade(replaced, cascade_revision, nullptr, &setup_error),
                "persist replaced upstream")) return 1;
    int cascade_changes = 0;
    QObject::connect(&bridge, &StandaloneMapProviderCatalog::cascadeChanged,
                     &bridge, [&cascade_changes]() { ++cascade_changes; });
    QString reload_error;
    if (!expect(bridge.reloadCascade(&reload_error) && cascade_changes == 1 &&
                bridge.cascade().upstreams.size() == 1 &&
                bridge.cascade().upstreams.first().name == QStringLiteral("replacement") &&
                !bridge.cascade().allow_direct_fetch,
                "saved cascade not applied live")) return 1;
    if (!expect(waitUntil([&bridge, &inherited_id]()
    {
        return bridge.inheritedUpstreams().value(inherited_id) ==
               QSet<QString>{QStringLiteral("replacement")};
    }) && replacement.authorized_requests > 0,
                "live cascade did not rediscover through the new upstream")) return 1;
    if (!expect(bridge.reloadCascade(&reload_error) && cascade_changes == 1,
                "unchanged cascade reload notified consumers")) return 1;
    return 0;
}
