#include "services/interface_server_map_standalone.h"

#include "services/standalone_map_provider_catalog_bridge.h"

#include <aowis/map/terrain_dataset.h>

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#ifdef Q_OS_WIN
#include <QPointer>
#include <QStandardPaths>
#endif
#include <QRunnable>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <limits>
#include <optional>

namespace
{
constexpr int TerrainRequestThreadCountMaximum = 4;
const QString TerrainDefaultDataset = QStringLiteral("copernicus-glo30");

QString terrainTileErrorMessage(const Aowis::Map::TerrainTileLookupResult &result)
{
    if (!result.error_message.isEmpty())
        return result.error_message;

    return QStringLiteral("Terrain tile request failed: %1")
        .arg(Aowis::Map::terrainTileLookupStatusId(result.status));
}

QString terrainElevationErrorMessage(const Aowis::Map::TerrainElevationLookupResult &result)
{
    if (!result.error_message.isEmpty())
        return result.error_message;

    return QStringLiteral("Terrain elevation request failed: %1")
        .arg(Aowis::Map::terrainElevationLookupStatusId(result.status));
}

QByteArray terrainElevationJson(const Aowis::Map::TerrainElevationLookupResult &result)
{
    if (result.status != Aowis::Map::TerrainElevationLookupStatus::Ready ||
        !result.sample.has_value())
    {
        return QByteArray();
    }

    const Aowis::Map::TerrainElevationSample &sample = result.sample.value();
    QJsonObject object;
    object.insert(QStringLiteral("status"), QStringLiteral("ready"));
    object.insert(QStringLiteral("elevation_m"), sample.elevation_m);
    object.insert(QStringLiteral("dataset"), sample.dataset);
    object.insert(QStringLiteral("nominal_resolution_m"), sample.nominal_resolution_m);
    object.insert(QStringLiteral("vertical_datum"),
                  Aowis::Map::terrainVerticalDatumId(sample.vertical_datum));
    object.insert(QStringLiteral("vertical_datum_name"),
                  Aowis::Map::terrainVerticalDatumDisplayName(sample.vertical_datum));

    const QString vertical_datum_authority =
        Aowis::Map::terrainVerticalDatumAuthorityCode(sample.vertical_datum);
    if (!vertical_datum_authority.isEmpty())
        object.insert(QStringLiteral("vertical_datum_authority"), vertical_datum_authority);

    object.insert(QStringLiteral("vertical_reference"),
                  Aowis::Map::terrainVerticalReferenceId(
                      Aowis::Map::terrainVerticalReference(sample.vertical_datum)));
    object.insert(QStringLiteral("source_vertical_datum"),
                  Aowis::Map::terrainVerticalDatumId(sample.source_vertical_datum));
    object.insert(QStringLiteral("origin"), Aowis::Map::terrainDataOriginId(sample.origin));

    if (result.tile_address.has_value())
    {
        const Aowis::Map::TerrainTileAddress &address = result.tile_address.value();
        QJsonObject tile;
        tile.insert(QStringLiteral("zoom"), address.zoom);
        tile.insert(QStringLiteral("x"), double(address.x));
        tile.insert(QStringLiteral("y"), double(address.y));
        object.insert(QStringLiteral("tile"), tile);
    }

    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
}

InterfaceServerMapStandalone::InterfaceServerMapStandalone(QObject *parent)
    : InterfaceServerMap(parent),
#ifdef Q_OS_WIN
      map_tiles(new MapTiles())
#else
      map_tiles(new MapTiles(this))
#endif
{
    // Configuration is owned by MAP and read once by the provider bridge,
    // which also fails closed (no upstreams, no direct downloads) while any
    // MAP-owned routing or inherited-provider record is corrupt.
    StandaloneMapProviderCatalogBridge &provider_bridge =
        StandaloneMapProviderCatalogBridge::instance();
    const Aowis::Map::MapCascadeConfig cascade = provider_bridge.cascade();
    // Configure before the Windows map downloader is moved to its own thread.
    this->map_tiles->setCascadeConfig(cascade);
    this->map_tiles->configureProviders(provider_bridge.availableProviders(), {}, {},
                                        provider_bridge.inheritedUpstreams(),
                                        provider_bridge.upstreamRoutes(),
                                        provider_bridge.acquisitionModes());
    connect(&provider_bridge, &StandaloneMapProviderCatalogBridge::providersChanged,
            this, [this, &provider_bridge]
    {
        const QVector<Aowis::Map::MapProvider> updated = provider_bridge.availableProviders();
        const QHash<QString, QSet<QString>> inherited = provider_bridge.inheritedUpstreams();
        const QHash<QString, QStringList> routes = provider_bridge.upstreamRoutes();
        const QHash<QString, Aowis::Map::MapProviderAcquisitionMode> modes =
            provider_bridge.acquisitionModes();
#ifdef Q_OS_WIN
        // MapTiles lives on its dedicated thread in Windows Standalone.
        MapTiles *tiles = this->map_tiles;
        QMetaObject::invokeMethod(tiles, [tiles, updated, inherited, routes, modes]
        {
            tiles->cancelUpstreamDownloads();
            tiles->configureProviders(updated, {}, {}, inherited, routes, modes);
        }, Qt::QueuedConnection);
#else
        this->map_tiles->cancelUpstreamDownloads();
        this->map_tiles->configureProviders(updated, {}, {}, inherited, routes, modes);
#endif
    });
    // Live cascade changes (Map settings): later requests use the new
    // upstreams, credentials, priority and options; downloads from the
    // previous upstream set are canceled (the repositories retry them).
    // Cached tiles stay valid. terrain_data is created below.
    connect(&provider_bridge, &StandaloneMapProviderCatalogBridge::cascadeChanged,
            this, [this, &provider_bridge]
    {
        const Aowis::Map::MapCascadeConfig updated = provider_bridge.cascade();
#ifdef Q_OS_WIN
        MapTiles *tiles = this->map_tiles;
        QMetaObject::invokeMethod(tiles, [tiles, updated]
        {
            tiles->setCascadeConfig(updated);
        }, Qt::QueuedConnection);
#else
        this->map_tiles->setCascadeConfig(updated);
#endif
        if (this->terrain_data != nullptr)
            this->terrain_data->setCascadeConfig(updated);
    });
    const QByteArray cascade_instance_id = this->map_tiles->cascadeInstanceId();
#ifdef Q_OS_WIN
    this->map_tiles->moveToThread(&this->map_tile_thread);
    connect(&this->map_tile_thread, &QThread::finished,
            this->map_tiles, &QObject::deleteLater);
    this->map_tile_thread.start();
#endif

    connect(this->map_tiles, &MapTiles::tileReady, this,
            [this](const QString &key, const QByteArray &data)
    {
        if (data.isEmpty())
        {
            qWarning() << "Downloaded tile is empty:" << key;
            emit signalTileFailed(key);
            return;
        }
        emit signalTileDataReceived(key, data);
    }, Qt::QueuedConnection);

    connect(this->map_tiles, &MapTiles::tileFailed, this,
            [this](const QString &key, MapTiles::TileFailureReason)
    {
        // MapTiles logs the detailed upstream error once. Do not emit a second
        // warning here for the same failed transfer.
        emit signalTileFailed(key);
    });

#ifdef Q_OS_WIN
    // The Windows standalone build performs terrain source download, GeoTIFF
    // decoding and normalized-cache writes in the same worker path. Keep the
    // first-fill path serialized on Windows: parallel cached terrain decoding
    // still happens later in MapTerrainRepository, while this avoids concurrent
    // filesystem/provider activity during the initial terrain population.
    const int terrain_thread_count = 1;
#else
    const int ideal_thread_count = qMax(1, QThread::idealThreadCount());
    const int terrain_thread_count =
        qMax(1, qMin(TerrainRequestThreadCountMaximum, ideal_thread_count / 2));
#endif
    this->terrain_request_pool.setMaxThreadCount(terrain_thread_count);
    this->terrain_request_pool.setExpiryTimeout(30000);

    Aowis::Map::TerrainData::Config terrain_config;
    terrain_config.enabled = true;
    terrain_config.remote_fetch_enabled = true;
    terrain_config.cascade = cascade;
#ifdef Q_OS_WIN
    // Terrain is disposable cache data. Do not put large Copernicus source and
    // normalized tile caches into Windows Roaming AppData. Qt's CacheLocation
    // resolves below AppData/Local and is the correct per-user storage class.
    terrain_config.cache_base_directory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
#endif
    this->terrain_data = new Aowis::Map::TerrainData(terrain_config, this);
    this->terrain_data->setCascadeInstanceId(cascade_instance_id);
    this->terrain_data_initialized =
        this->terrain_data->initialize(&this->terrain_initialization_error);
    if (!this->terrain_data_initialized)
    {
        qWarning().noquote() << QStringLiteral("Failed to initialize standalone terrain data: %1")
                                    .arg(this->terrain_initialization_error);
    }
}

InterfaceServerMapStandalone::~InterfaceServerMapStandalone()
{
    this->terrain_request_pool.clear();
    this->terrain_request_pool.waitForDone();

#ifdef Q_OS_WIN
    this->map_tile_thread.quit();
    this->map_tile_thread.wait();
    this->map_tiles = nullptr;
#endif
}

void InterfaceServerMapStandalone::requestTile(const QString &endpoint, const QString &key, int x, int y)
{
    QString endpoint_without_extension = endpoint;
    endpoint_without_extension.remove(".png");
    const QStringList parts = endpoint_without_extension.split("/", Qt::SkipEmptyParts);
    if (parts.size() < 2)
    {
        qWarning() << "Invalid endpoint:" << endpoint;
        emit signalTileFailed(key);
        return;
    }

    const QString provider = parts[0];
    bool zoom_valid = false;
    const int zoom = parts[1].toInt(&zoom_valid);
    if (!zoom_valid)
    {
        qWarning() << "Invalid tile zoom in endpoint:" << endpoint;
        emit signalTileFailed(key);
        return;
    }

#ifdef Q_OS_WIN
    MapTiles *map_tiles = this->map_tiles;
    if (map_tiles == nullptr || !this->map_tile_thread.isRunning())
    {
        emit signalTileFailed(key);
        return;
    }

    const QPointer<InterfaceServerMapStandalone> receiver(this);
    QMetaObject::invokeMethod(map_tiles,
        [receiver, map_tiles, provider, zoom, x, y, key, endpoint]
    {
        if (receiver.isNull())
            return;

        const MapTiles::TileRequestResult result =
            map_tiles->getTile(provider, zoom, x, y, key);

        QMetaObject::invokeMethod(receiver.data(),
            [receiver, endpoint, key, x, y, result]
        {
            if (receiver.isNull())
                return;

            receiver->finishTileRequest(endpoint, key, x, y, result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
#else
    const MapTiles::TileRequestResult result = this->map_tiles->getTile(provider, zoom, x, y, key);
    switch (result.status)
    {
    case MapTiles::TileRequestStatus::Ready:
        if (result.data.isEmpty())
        {
            qWarning() << "Cached tile is empty:" << key;
            emit signalTileFailed(key);
            return;
        }

        QTimer::singleShot(0, this, [this, key, data = result.data]
        {
            emit signalTileDataReceived(key, data);
        });
        return;

    case MapTiles::TileRequestStatus::Pending:
        return;

    case MapTiles::TileRequestStatus::InvalidRequest:
        qWarning() << "Invalid tile request:" << endpoint << x << y;
        emit signalTileFailed(key);
        return;

    case MapTiles::TileRequestStatus::ServerBusy:
        qWarning() << "Map tile downloader is busy:" << key;
        emit signalTileFailed(key);
        return;
    }
#endif
}

#ifdef Q_OS_WIN
void InterfaceServerMapStandalone::finishTileRequest(
    const QString &endpoint, const QString &key, int x, int y,
    const MapTiles::TileRequestResult &result)
{
    switch (result.status)
    {
    case MapTiles::TileRequestStatus::Ready:
        if (result.data.isEmpty())
        {
            qWarning() << "Cached tile is empty:" << key;
            emit signalTileFailed(key);
            return;
        }

        emit signalTileDataReceived(key, result.data);
        return;

    case MapTiles::TileRequestStatus::Pending:
        return;

    case MapTiles::TileRequestStatus::InvalidRequest:
        qWarning() << "Invalid tile request:" << endpoint << x << y;
        emit signalTileFailed(key);
        return;

    case MapTiles::TileRequestStatus::ServerBusy:
        qWarning() << "Map tile downloader is busy:" << key;
        emit signalTileFailed(key);
        return;
    }
}
#endif

void InterfaceServerMapStandalone::requestTerrainTile(const QString &endpoint, const QString &key)
{
    if (!this->terrain_data_initialized || this->terrain_data == nullptr)
    {
        const QString error = this->terrain_initialization_error.isEmpty()
            ? QStringLiteral("Standalone terrain data is not initialized")
            : this->terrain_initialization_error;
        emit signalTerrainTileFailed(key, error);
        return;
    }

    QString path = QUrl(endpoint).path();
    if (path.isEmpty())
        path = endpoint.section(QLatin1Char('?'), 0, 0);

    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() != 6 || parts.at(0) != QStringLiteral("terrain") ||
        parts.at(1) != QStringLiteral("v1") ||
        !parts.at(5).endsWith(QStringLiteral(".aowterrain")))
    {
        emit signalTerrainTileFailed(
            key, QStringLiteral("Invalid standalone terrain tile endpoint: %1").arg(endpoint));
        return;
    }

    const QString dataset = parts.at(2);
    bool zoom_valid = false;
    bool x_valid = false;
    bool y_valid = false;
    const int zoom = parts.at(3).toInt(&zoom_valid);
    const qulonglong x_value = parts.at(4).toULongLong(&x_valid);
    QString y_text = parts.at(5);
    y_text.chop(QStringLiteral(".aowterrain").size());
    const qulonglong y_value = y_text.toULongLong(&y_valid);

    if (!zoom_valid || !x_valid || !y_valid ||
        x_value > qulonglong(std::numeric_limits<quint32>::max()) ||
        y_value > qulonglong(std::numeric_limits<quint32>::max()))
    {
        emit signalTerrainTileFailed(
            key, QStringLiteral("Invalid standalone terrain tile XYZ address: %1").arg(endpoint));
        return;
    }

    Aowis::Map::TerrainTileAddress address;
    address.zoom = zoom;
    address.x = quint32(x_value);
    address.y = quint32(y_value);
    if (!Aowis::Map::isValidTerrainDatasetId(dataset) ||
        !Aowis::Map::isValidTerrainTileAddress(address))
    {
        emit signalTerrainTileFailed(
            key, QStringLiteral("Invalid standalone terrain dataset or XYZ address: %1").arg(endpoint));
        return;
    }

    Aowis::Map::TerrainData *terrain_data = this->terrain_data;
    this->terrain_request_pool.start(QRunnable::create([this, terrain_data, dataset, address, key]
    {
        const Aowis::Map::TerrainTileLookupResult result =
            terrain_data->terrainTile(dataset, address);
        QMetaObject::invokeMethod(this, [this, result, key]
        {
            if (result.status != Aowis::Map::TerrainTileLookupStatus::Ready || result.data.isEmpty())
            {
                emit signalTerrainTileFailed(key, terrainTileErrorMessage(result));
                return;
            }

            emit signalTerrainTileDataReceived(key, result.data);
        }, Qt::QueuedConnection);
    }));
}

void InterfaceServerMapStandalone::requestTerrainDatasets()
{
    if (!this->terrain_data_initialized || this->terrain_data == nullptr)
    {
        const QString error = this->terrain_initialization_error.isEmpty()
            ? QStringLiteral("Standalone terrain data is not initialized")
            : this->terrain_initialization_error;
        QTimer::singleShot(0, this, [this, error]
        {
            emit signalTerrainDatasetsFailed(error);
        });
        return;
    }

    // The same document the map server's GET /terrain/v1/datasets serves;
    // TerrainDefaultDataset plays the server's terrain/default_dataset.
    // Delivered from the event loop like a network reply.
    const QByteArray catalog = Aowis::Map::encodeTerrainDatasetCatalogJson(
        this->terrain_data->datasets(), TerrainDefaultDataset);
    QTimer::singleShot(0, this, [this, catalog]
    {
        emit signalTerrainDatasetsReceived(catalog);
    });
}

void InterfaceServerMapStandalone::requestTerrainElevation(const QString &endpoint)
{
    if (!this->terrain_data_initialized || this->terrain_data == nullptr)
    {
        const QString error = this->terrain_initialization_error.isEmpty()
            ? QStringLiteral("Standalone terrain data is not initialized")
            : this->terrain_initialization_error;
        emit signalTerrainElevationFailed(error);
        return;
    }

    if (this->terrain_elevation_pending)
    {
        emit signalTerrainElevationFailed(
            QStringLiteral("A terrain elevation request is already in progress"));
        return;
    }

    const QUrl url(endpoint);
    const QUrlQuery query(url);
    const QString latitude_text = query.queryItemValue(QStringLiteral("latitude"));
    const QString longitude_text = query.queryItemValue(QStringLiteral("longitude"));
    bool latitude_valid = false;
    bool longitude_valid = false;
    const double latitude_deg = latitude_text.toDouble(&latitude_valid);
    const double longitude_deg = longitude_text.toDouble(&longitude_valid);
    if (!latitude_valid || !longitude_valid || latitude_text.isEmpty() || longitude_text.isEmpty())
    {
        emit signalTerrainElevationFailed(
            QStringLiteral("Terrain elevation requests require numeric latitude and longitude query parameters"));
        return;
    }

    std::optional<Aowis::Map::TerrainVerticalDatum> requested_vertical_datum;
    const QString vertical_datum_text =
        query.queryItemValue(QStringLiteral("vertical_datum")).trimmed().toLower();
    if (!vertical_datum_text.isEmpty() && vertical_datum_text != QStringLiteral("native"))
    {
        const std::optional<Aowis::Map::TerrainVerticalDatum> parsed_vertical_datum =
            Aowis::Map::terrainVerticalDatumFromId(vertical_datum_text);
        if (!parsed_vertical_datum.has_value() ||
            !Aowis::Map::isRequestableTerrainVerticalDatum(parsed_vertical_datum.value()))
        {
            emit signalTerrainElevationFailed(
                QStringLiteral("vertical_datum must be native, wgs84-ellipsoid, egm96 or egm2008"));
            return;
        }
        requested_vertical_datum = parsed_vertical_datum.value();
    }

    this->terrain_elevation_pending = true;
    Aowis::Map::TerrainData *terrain_data = this->terrain_data;
    this->terrain_request_pool.start(QRunnable::create(
        [this, terrain_data, latitude_deg, longitude_deg, requested_vertical_datum]
    {
        const Aowis::Map::TerrainElevationLookupResult result =
            terrain_data->sampleElevation(
                TerrainDefaultDataset, latitude_deg, longitude_deg, requested_vertical_datum);
        QMetaObject::invokeMethod(this, [this, result]
        {
            this->terrain_elevation_pending = false;
            if (result.status != Aowis::Map::TerrainElevationLookupStatus::Ready ||
                !result.sample.has_value())
            {
                emit signalTerrainElevationFailed(terrainElevationErrorMessage(result));
                return;
            }

            const QByteArray data = terrainElevationJson(result);
            if (data.isEmpty())
            {
                emit signalTerrainElevationFailed(
                    QStringLiteral("Failed to serialize standalone terrain elevation response"));
                return;
            }

            emit signalTerrainElevationDataReceived(data);
        }, Qt::QueuedConnection);
    }));
}

void InterfaceServerMapStandalone::deleteTiles(quint64 request_id, const QString &provider, int zoom,
                                               int tile_x_min, int tile_x_max,
                                               int tile_y_min, int tile_y_max)
{
#ifdef Q_OS_WIN
    MapTiles *map_tiles = this->map_tiles;
    if (map_tiles == nullptr || !this->map_tile_thread.isRunning())
    {
        emit signalTileDeletionFailed(
            request_id, QStringLiteral("Standalone map tile worker is not running"));
        return;
    }

    const QPointer<InterfaceServerMapStandalone> receiver(this);
    QMetaObject::invokeMethod(map_tiles,
        [receiver, map_tiles, request_id, provider, zoom,
         tile_x_min, tile_x_max, tile_y_min, tile_y_max]
    {
        if (receiver.isNull())
            return;

        const int deleted_count = map_tiles->deleteTiles(
            provider, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max);

        QMetaObject::invokeMethod(receiver.data(),
            [receiver, request_id, deleted_count]
        {
            if (receiver.isNull())
                return;

            receiver->finishTileDeletion(request_id, deleted_count);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
#else
    const int deleted_count = this->map_tiles->deleteTiles(
        provider, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max);
    if (deleted_count == -1)
    {
        emit signalTileDeletionFailed(request_id, QStringLiteral("Invalid tile cache deletion request"));
        return;
    }
    if (deleted_count < -1)
    {
        emit signalTileDeletionFailed(request_id, QStringLiteral("Failed to delete one or more cached tiles"));
        return;
    }
    emit signalTilesDeleted(request_id);
#endif
}

#ifdef Q_OS_WIN
void InterfaceServerMapStandalone::finishTileDeletion(quint64 request_id, int deleted_count)
{
    if (deleted_count == -1)
    {
        emit signalTileDeletionFailed(
            request_id, QStringLiteral("Invalid tile cache deletion request"));
        return;
    }
    if (deleted_count < -1)
    {
        emit signalTileDeletionFailed(
            request_id, QStringLiteral("Failed to delete one or more cached tiles"));
        return;
    }
    emit signalTilesDeleted(request_id);
}
#endif

void InterfaceServerMapStandalone::deleteTerrainCache(quint64 request_id,
                                                      MapTerrainCacheDeletionScope scope,
                                                      const QString &dataset, int zoom,
                                                      int tile_x_min, int tile_x_max,
                                                      int tile_y_min, int tile_y_max)
{
    if (!this->terrain_data_initialized || this->terrain_data == nullptr)
    {
        const QString error = this->terrain_initialization_error.isEmpty()
            ? QStringLiteral("Standalone terrain data is not initialized")
            : this->terrain_initialization_error;
        QTimer::singleShot(0, this, [this, request_id, error]
        {
            emit signalTerrainCacheDeletionFailed(request_id, error);
        });
        return;
    }

    // File work (and a brief wait for a source file being downloaded) off
    // the GUI thread, on the pool that serves the terrain requests; the
    // answer arrives from the event loop like a network reply.
    Aowis::Map::TerrainData *terrain_data = this->terrain_data;
    this->terrain_request_pool.start(QRunnable::create(
        [this, terrain_data, request_id, scope, dataset, zoom,
         tile_x_min, tile_x_max, tile_y_min, tile_y_max]
    {
        const Aowis::Map::TerrainCacheDeletionResult result =
            scope == MapTerrainCacheDeletionScope::SourceData
                ? terrain_data->deleteSourceData(
                      dataset, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max)
                : terrain_data->deleteNormalizedTiles(
                      dataset, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max);
        QMetaObject::invokeMethod(this, [this, request_id, result]
        {
            if (result.status != Aowis::Map::TerrainCacheDeletionStatus::Deleted)
            {
                emit signalTerrainCacheDeletionFailed(
                    request_id, result.error_message.isEmpty()
                        ? QStringLiteral("Terrain cache deletion failed: %1")
                              .arg(Aowis::Map::terrainCacheDeletionStatusId(result.status))
                        : result.error_message);
                return;
            }
            emit signalTerrainCacheDeleted(request_id, result.deleted_count);
        }, Qt::QueuedConnection);
    }));
}

void InterfaceServerMapStandalone::requestMapTileUpstreamActivity()
{
    MapUpstreamActivity activity;
    if (this->map_tiles != nullptr)
    {
        const MapTiles::UpstreamActivity source = this->map_tiles->upstreamActivity();
        activity.active = source.active;
        activity.queued = source.queued;
    }
    emit signalMapTileUpstreamActivity(activity);
}

void InterfaceServerMapStandalone::requestTerrainUpstreamActivity()
{
    MapUpstreamActivity activity;
    if (this->terrain_data != nullptr)
    {
        const Aowis::Map::TerrainUpstreamActivity source = this->terrain_data->upstreamActivity();
        activity.active = source.active;
        activity.queued = source.queued;
    }
    emit signalTerrainUpstreamActivity(activity);
}

void InterfaceServerMapStandalone::cancelMapTileUpstreamDownloads()
{
    if (this->map_tiles == nullptr)
        return;
#ifdef Q_OS_WIN
    MapTiles *map_tiles = this->map_tiles;
    QMetaObject::invokeMethod(map_tiles, [map_tiles]
    {
        map_tiles->cancelUpstreamDownloads();
    }, Qt::QueuedConnection);
#else
    this->map_tiles->cancelUpstreamDownloads();
#endif
}

void InterfaceServerMapStandalone::cancelTerrainUpstreamDownloads()
{
    if (this->terrain_data != nullptr)
        this->terrain_data->cancelUpstreamDownloads();
}
