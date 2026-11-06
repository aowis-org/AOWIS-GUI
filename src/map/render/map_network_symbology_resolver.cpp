#include "map/render/map_network_symbology_resolver.h"

#include "network/hydraulic_data.h"
#include "network/network_render_snapshot.h"
#include "network/network_symbology_values.h"

#include <algorithm>
#include <cmath>
#include <limits>

void MapNetworkSymbologyResolver::synchronizeSnapshot(const NetworkRenderSnapshot &snapshot)
{
    if (this->snapshot_known && this->snapshot_geometry_revision == snapshot.geometry_revision
        && this->node_count == snapshot.nodes.size() && this->link_count == snapshot.links.size())
        return;
    this->snapshot_known = true;
    this->snapshot_geometry_revision = snapshot.geometry_revision;
    this->node_count = snapshot.nodes.size();
    this->link_count = snapshot.links.size();
    // Built once per network geometry, then only looked up when a source's
    // order differs from the cached one.
    this->node_render_ids.clear();
    this->node_render_ids.reserve(snapshot.nodes.size());
    for (const NetworkRenderNode &node : snapshot.nodes)
        this->node_render_ids.insert(node.uuid, node.render_id);
    this->link_render_ids.clear();
    this->link_render_ids.reserve(snapshot.links.size());
    for (const NetworkRenderLink &link : snapshot.links)
        this->link_render_ids.insert(link.uuid, link.render_id);
    for (OrderCache &cache : this->order_caches)
    {
        cache.uuids.clear();
        cache.render_ids.clear();
    }
}

quint32 MapNetworkSymbologyResolver::renderIdAt(OrderCache *cache, qsizetype index, const QUuid &uuid,
                                                const QHash<QUuid, quint32> &render_ids) const
{
    // The common case of every time step: same position, same entity.
    if (index < cache->uuids.size() && cache->uuids.constData()[index] == uuid)
        return cache->render_ids.constData()[index];
    const quint32 render_id = render_ids.value(uuid, 0);
    if (index < cache->uuids.size())
    {
        cache->uuids[index] = uuid;
        cache->render_ids[index] = render_id;
    }
    else
    {
        cache->uuids.append(uuid);
        cache->render_ids.append(render_id);
    }
    return render_id;
}

void MapNetworkSymbologyResolver::ValueWriter::operator()(const QUuid &uuid, double value)
{
    const quint32 render_id = this->resolver->renderIdAt(this->cache, this->index, uuid, *this->render_ids);
    ++this->index;
    if (render_id != 0 && qsizetype(render_id) < this->values->size())
        this->values->data()[qsizetype(render_id)] = value;
}

void MapNetworkSymbologyResolver::ValueWriter::finish()
{
    // A shorter source than last time: forget the stale tail.
    this->cache->uuids.resize(std::min(this->cache->uuids.size(), this->index));
    this->cache->render_ids.resize(std::min(this->cache->render_ids.size(), this->index));
}

MapNetworkSymbologyResolver::ValueWriter MapNetworkSymbologyResolver::valueWriter(
    Source source, const QHash<QUuid, quint32> &render_ids, QVector<double> *values)
{
    ValueWriter writer;
    writer.resolver = this;
    writer.cache = &this->order_caches[source];
    writer.render_ids = &render_ids;
    writer.values = values;
    return writer;
}

MapNetworkRenderSymbology MapNetworkSymbologyResolver::resolve(
    const HydraulicData &hydraulic_data,
    const NetworkSymbologySettings &settings,
    const NetworkSymbologyRanges &ranges)
{
    const NetworkSymbologySettings bounded_settings = settings.bounded();
    const NetworkRenderSnapshot &snapshot = hydraulic_data.networkRenderSnapshot();
    const NetworkHydraulic &network_hydraulic = hydraulic_data.networkHydraulic();
    synchronizeSnapshot(snapshot);

    MapNetworkRenderSymbology result;
    result.node_size_unit = bounded_settings.node_size_unit;
    result.show_junctions = bounded_settings.show_junctions;
    result.node_size_px = bounded_settings.node_size_px;
    result.node_size_m = bounded_settings.node_size_m;
    result.icon_size_unit = bounded_settings.icon_size_unit;
    result.icon_size_px = bounded_settings.icon_size_px;
    result.icon_size_m = bounded_settings.icon_size_m;
    result.show_icons = true;
    result.visual_node = bounded_settings.visual_node;
    result.node_palette = bounded_settings.node_palette;
    result.node_palette_flipped = bounded_settings.node_palette_flipped;
    result.visual_link = bounded_settings.visual_link;
    result.link_palette = bounded_settings.link_palette;
    result.link_palette_flipped = bounded_settings.link_palette_flipped;
    result.link_thickness_unit = bounded_settings.link_thickness_unit;
    result.link_thickness_px = bounded_settings.link_thickness_px;
    result.link_thickness_m = bounded_settings.link_thickness_m;
    result.show_flow_direction = bounded_settings.show_flow_direction;
    result.flow_direction_size_px = bounded_settings.flow_direction_size_px;
    result.visual_heatmap = bounded_settings.visual_heatmap;
    result.heatmap_opacity = bounded_settings.heatmap_opacity;
    result.heatmap_radius_unit = bounded_settings.heatmap_radius_unit;
    result.heatmap_radius_m = bounded_settings.heatmap_radius_m;
    result.heatmap_radius_px = bounded_settings.heatmap_radius_px;
    result.heatmap_solid_center_percent = bounded_settings.heatmap_solid_center_percent;
    result.heatmap_palette = bounded_settings.heatmap_palette;
    result.heatmap_palette_flipped = bounded_settings.heatmap_palette_flipped;
    result.ranges = ranges;

    const WaterQualitySimulationResult *quality_result =
        hydraulic_data.currentWaterQualitySimulationResult(WaterQualityAnalysisType::WaterAge);
    const HydraulicSimulationResult *hydraulic_result = hydraulic_data.currentSimulationResult();
    const double no_value = std::numeric_limits<double>::quiet_NaN();
    const qsizetype node_slots = snapshot.nodes.size() + 1;
    const qsizetype link_slots = snapshot.links.size() + 1;

    // One dense fill per metric: every visited (uuid, value) lands at its
    // render id; entities the source does not list keep NaN.
    if (result.visual_node != VisualNode::None)
    {
        result.node_values.fill(no_value, node_slots);
        ValueWriter writer = valueWriter(NodeValueSource, this->node_render_ids, &result.node_values);
        const VisualNode visual = result.visual_node;
        if (visual == VisualNode::WaterAge)
        {
            if (quality_result != nullptr)
                visitWaterAgeNodeSymbologyValues(*quality_result, writer);
        }
        else if (nodeVisualUsesHydraulicSimulationResult(visual))
        {
            if (hydraulic_result != nullptr)
                visitHydraulicNodeSymbologyValues(*hydraulic_result, visual, writer);
        }
        else
        {
            visitNetworkNodeSymbologyValues(network_hydraulic, visual, writer);
        }
        writer.finish();
    }

    if (result.visual_link != VisualLink::None)
    {
        result.link_values.fill(no_value, link_slots);
        ValueWriter writer = valueWriter(LinkValueSource, this->link_render_ids, &result.link_values);
        const VisualLink visual = result.visual_link;
        if (visual == VisualLink::WaterAge)
        {
            if (quality_result != nullptr)
                visitWaterAgeLinkSymbologyValues(*quality_result, writer);
        }
        else if (linkVisualUsesHydraulicSimulationResult(visual))
        {
            if (hydraulic_result != nullptr)
                visitHydraulicLinkSymbologyValues(*hydraulic_result, visual, writer);
        }
        else
        {
            visitNetworkLinkSymbologyValues(network_hydraulic, visual, writer);
        }
        writer.finish();
    }

    if (result.visual_heatmap != VisualHeatmap::None)
    {
        result.heatmap_values.fill(no_value, node_slots);
        ValueWriter writer = valueWriter(HeatmapValueSource, this->node_render_ids, &result.heatmap_values);
        const VisualHeatmap visual = result.visual_heatmap;
        const VisualNode node_visual = networkHeatmapNodeVisual(visual);
        if (visual == VisualHeatmap::WaterAge)
        {
            if (quality_result != nullptr)
                visitWaterAgeNodeSymbologyValues(*quality_result, writer);
        }
        else if (heatmapVisualUsesHydraulicSimulationResult(visual))
        {
            if (hydraulic_result != nullptr)
                visitHydraulicNodeSymbologyValues(*hydraulic_result, node_visual, writer);
        }
        else
        {
            visitNetworkNodeSymbologyValues(network_hydraulic, node_visual, writer);
        }
        writer.finish();
    }

    // Flow directions only exist with a hydraulic result and the arrows on.
    if (result.show_flow_direction && hydraulic_result != nullptr)
    {
        QVector<double> directions(link_slots, 0.0);
        ValueWriter writer = valueWriter(FlowSource, this->link_render_ids, &directions);
        visitHydraulicLinkFlowDirections(*hydraulic_result, [&writer](const QUuid &uuid, qint8 direction)
        {
            writer(uuid, double(direction));
        });
        writer.finish();
        result.flow_directions.resize(link_slots);
        for (qsizetype index = 0; index < link_slots; ++index)
            result.flow_directions[index] = qint8(directions[index]);
    }

    return result;
}
