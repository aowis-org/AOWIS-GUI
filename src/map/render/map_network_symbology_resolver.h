#ifndef MAP_NETWORK_SYMBOLOGY_RESOLVER_H
#define MAP_NETWORK_SYMBOLOGY_RESOLVER_H

// The GUI side of the resolved network symbology: fills Render's
// MapNetworkRenderSymbology from the hydraulic data and the current results.

#include "map/render/map_network_render_symbology.h"
#include "network/network_symbology.h"

#include <QHash>
#include <QUuid>
#include <QVector>
#include <QtGlobal>

class HydraulicData;
struct NetworkRenderSnapshot;

// Resolves the symbology inputs of the current time step into dense
// per-entity values. Keeps the uuid -> render id mapping of the snapshot and,
// per value source, the render ids in the order the source lists them: the
// result lists of every time step have the same order, so a step costs one
// uuid comparison per entity instead of a hash lookup (and never builds a
// hash table).
class MapNetworkSymbologyResolver
{
public:
    MapNetworkRenderSymbology resolve(const HydraulicData &hydraulic_data,
                                      const NetworkSymbologySettings &settings,
                                      const NetworkSymbologyRanges &ranges);

private:
    // Render ids of one visited source, by position in the visit.
    struct OrderCache
    {
        QVector<QUuid> uuids;
        QVector<quint32> render_ids;
    };
    enum Source
    {
        NodeValueSource,
        LinkValueSource,
        HeatmapValueSource,
        FlowSource,
        SourceCount
    };

    void synchronizeSnapshot(const NetworkRenderSnapshot &snapshot);
    // Render id of the entity visited at position index of a source (0: not
    // rendered). The cache entry is replaced when the order changed.
    quint32 renderIdAt(OrderCache *cache, qsizetype index, const QUuid &uuid,
                       const QHash<QUuid, quint32> &render_ids) const;
    // Visitor for the network_symbology_values.h sources: stores every
    // visited value at its render id in a dense vector (sized to the render
    // id range by the caller); finish() drops a stale tail of the order cache.
    struct ValueWriter
    {
        const MapNetworkSymbologyResolver *resolver = nullptr;
        OrderCache *cache = nullptr;
        const QHash<QUuid, quint32> *render_ids = nullptr;
        QVector<double> *values = nullptr;
        qsizetype index = 0;

        void operator()(const QUuid &uuid, double value);
        void finish();
    };
    ValueWriter valueWriter(Source source, const QHash<QUuid, quint32> &render_ids,
                            QVector<double> *values);

    quint64 snapshot_geometry_revision = 0;
    bool snapshot_known = false;
    qsizetype node_count = 0;
    qsizetype link_count = 0;
    QHash<QUuid, quint32> node_render_ids;
    QHash<QUuid, quint32> link_render_ids;
    OrderCache order_caches[SourceCount];
};

#endif // MAP_NETWORK_SYMBOLOGY_RESOLVER_H
