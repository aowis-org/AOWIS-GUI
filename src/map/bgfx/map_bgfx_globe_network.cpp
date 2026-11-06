#include "map/bgfx/map_bgfx_globe_network.h"

#include "map/bgfx/map_bgfx_backend.h"
#include "map/bgfx/map_bgfx_globe_model_images.h"
#include "map/core/map_model.h"
#include "map/render/map_globe_icon_draw_order.h"
#include "map/render/map_globe_camera.h"
#include "map/render/map_globe_picking.h"
#include "map/render/map_globe_render_frame.h"
#include "map/render/map_globe_reservoir_model.h"
#include "map/render/map_globe_tank_model.h"
#include "network/network_symbology_rendering.h"

#include <QByteArray>
#include <QDebug>
#include <QImage>
#include <QLineF>
#include <QMatrix4x4>
#include <QSet>
#include <QVector3D>
#include <QVector4D>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace
{
constexpr uint64_t kAlbedoSamplerFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
// The six corners of an impostor quad (MapGlobeJunctionImpostorVertex).
const float kImpostorCorners[6][2] = {
    {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};

template <typename Handle>
void destroyHandle(Handle *handle)
{
    if (bgfx::isValid(*handle))
    {
        bgfx::destroy(*handle);
        *handle = BGFX_INVALID_HANDLE;
    }
}

// Bytes after the last attribute (render id, entity type, padding).
template <typename Vertex>
uint8_t tailBytes(std::size_t last_attribute_end)
{
    return uint8_t(sizeof(Vertex) - last_attribute_end);
}

// RGBA8 texture with its complete mip chain (premultiplied, smooth
// scaling), as the 2D icon atlas: the models are seen at every distance.
bgfx::TextureHandle mipmappedTexture(const QImage &source, uint64_t flags)
{
    if (source.isNull())
        return BGFX_INVALID_HANDLE;
    QImage level = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const int width = level.width();
    const int height = level.height();
    QByteArray pixels;
    int level_width = width;
    int level_height = height;
    while (true)
    {
        for (int row = 0; row < level.height(); ++row)
            pixels.append(reinterpret_cast<const char *>(level.constScanLine(row)), level.width() * 4);
        if (level_width == 1 && level_height == 1)
            break;
        level_width = std::max(1, level_width / 2);
        level_height = std::max(1, level_height / 2);
        level = level.scaled(level_width, level_height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    }
    return bgfx::createTexture2D(uint16_t(width), uint16_t(height), true, 1, bgfx::TextureFormat::RGBA8,
                                 flags, bgfx::copy(pixels.constData(), uint32_t(pixels.size())));
}

bool sameSymbologySizes(const MapNetworkRenderSymbology &a, const MapNetworkRenderSymbology &b)
{
    return a.link_thickness_unit == b.link_thickness_unit && a.link_thickness_px == b.link_thickness_px
        && a.link_thickness_m == b.link_thickness_m;
}
}

MapBgfxGlobeNetwork::MapBgfxGlobeNetwork() = default;

MapBgfxGlobeNetwork::~MapBgfxGlobeNetwork() = default;

bool MapBgfxGlobeNetwork::initialize()
{
    // Vertex layouts: the scene's structs uploaded unchanged; the trailing
    // render id / entity type are skipped.
    this->link_layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord2, 1, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord3, 3, bgfx::AttribType::Float)
        .skip(tailBytes<MapNetworkLinkVertex>(offsetof(MapNetworkLinkVertex, width_direction_z) + sizeof(float)))
        .end();
    this->node_layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord2, 1, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord4, 1, bgfx::AttribType::Float)
        .skip(tailBytes<MapNetworkNodeVertex>(offsetof(MapNetworkNodeVertex, metric_billboard) + sizeof(float)))
        .end();
    this->icon_layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord5, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .skip(tailBytes<MapNetworkIconVertex>(offsetof(MapNetworkIconVertex, alpha) + sizeof(float)))
        .end();
    this->junction_layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord2, 1, bgfx::AttribType::Float)
        .end();
    // Tank and reservoir model vertices share one layout.
    static_assert(sizeof(MapGlobeTankModelVertex) == sizeof(MapGlobeReservoirModelVertex),
                  "Tank and reservoir model vertices must share one layout");
    this->model_layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord5, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord2, 1, bgfx::AttribType::Float)
        .skip(tailBytes<MapGlobeTankModelVertex>(offsetof(MapGlobeTankModelVertex, selected) + sizeof(float)))
        .end();
    if (this->link_layout.getStride() != sizeof(MapNetworkLinkVertex)
        || this->node_layout.getStride() != sizeof(MapNetworkNodeVertex)
        || this->icon_layout.getStride() != sizeof(MapNetworkIconVertex)
        || this->junction_layout.getStride() != sizeof(JunctionVertex)
        || this->model_layout.getStride() != sizeof(MapGlobeTankModelVertex))
    {
        qWarning() << "bgfx globe network: vertex layouts do not match the scene's vertex structs";
        return false;
    }

    this->link_program = MapBgfxBackend::loadProgram("vs_globe_link", "fs_globe_link");
    this->link_xray_program = MapBgfxBackend::loadProgram("vs_globe_link", "fs_globe_link_xray");
    this->flow_program = MapBgfxBackend::loadProgram("vs_globe_flow", "fs_globe_link");
    this->node_program = MapBgfxBackend::loadProgram("vs_globe_node", "fs_globe_node");
    this->junction_program = MapBgfxBackend::loadProgram("vs_globe_junction", "fs_globe_junction");
    this->junction_xray_program = MapBgfxBackend::loadProgram("vs_globe_junction", "fs_globe_junction_xray");
    this->icon_program = MapBgfxBackend::loadProgram("vs_globe_icon", "fs_globe_icon");
    this->model_program = MapBgfxBackend::loadProgram("vs_globe_model", "fs_globe_model");
    this->view_projection_uniform = bgfx::createUniform("u_net_view_proj", bgfx::UniformType::Mat4);
    this->viewport_uniform = bgfx::createUniform("u_net_viewport", bgfx::UniformType::Vec4);
    this->params_uniform = bgfx::createUniform("u_net_params", bgfx::UniformType::Vec4);
    this->camera_right_uniform = bgfx::createUniform("u_net_camera_right", bgfx::UniformType::Vec4);
    this->camera_up_uniform = bgfx::createUniform("u_net_camera_up", bgfx::UniformType::Vec4);
    this->camera_eye_uniform = bgfx::createUniform("u_net_camera_eye", bgfx::UniformType::Vec4);
    this->icon_sampler = bgfx::createUniform("s_net_icons", bgfx::UniformType::Sampler);
    this->albedo_sampler = bgfx::createUniform("s_net_albedo", bgfx::UniformType::Sampler);
    this->tank_texture = mipmappedTexture(mapGlobeTankAlbedoImage(), kAlbedoSamplerFlags);
    this->reservoir_texture = mipmappedTexture(mapGlobeReservoirAlbedoImage(), kAlbedoSamplerFlags);
    // Everything goes up again on the (new) device.
    markDirty(AllStreams);
    if (!ready())
    {
        qWarning() << "bgfx globe network unavailable: programs" << bgfx::isValid(this->link_program)
                   << bgfx::isValid(this->link_xray_program) << bgfx::isValid(this->flow_program)
                   << bgfx::isValid(this->node_program) << bgfx::isValid(this->junction_program)
                   << bgfx::isValid(this->junction_xray_program) << bgfx::isValid(this->icon_program)
                   << bgfx::isValid(this->model_program);
        release();
        return false;
    }
    return true;
}

bool MapBgfxGlobeNetwork::ready() const
{
    return bgfx::isValid(this->link_program) && bgfx::isValid(this->link_xray_program)
        && bgfx::isValid(this->flow_program) && bgfx::isValid(this->node_program)
        && bgfx::isValid(this->junction_program) && bgfx::isValid(this->junction_xray_program)
        && bgfx::isValid(this->icon_program) && bgfx::isValid(this->model_program)
        && bgfx::isValid(this->view_projection_uniform) && bgfx::isValid(this->viewport_uniform)
        && bgfx::isValid(this->params_uniform) && bgfx::isValid(this->camera_right_uniform)
        && bgfx::isValid(this->camera_up_uniform) && bgfx::isValid(this->camera_eye_uniform)
        && bgfx::isValid(this->icon_sampler) && bgfx::isValid(this->albedo_sampler)
        && bgfx::isValid(this->tank_texture) && bgfx::isValid(this->reservoir_texture);
}

void MapBgfxGlobeNetwork::release()
{
    this->gpu_buffers.release();
    this->icon_order_dirty = true;
    destroyHandle(&this->reservoir_texture);
    destroyHandle(&this->tank_texture);
    destroyHandle(&this->albedo_sampler);
    destroyHandle(&this->icon_sampler);
    destroyHandle(&this->camera_eye_uniform);
    destroyHandle(&this->camera_up_uniform);
    destroyHandle(&this->camera_right_uniform);
    destroyHandle(&this->params_uniform);
    destroyHandle(&this->viewport_uniform);
    destroyHandle(&this->view_projection_uniform);
    destroyHandle(&this->model_program);
    destroyHandle(&this->icon_program);
    destroyHandle(&this->junction_xray_program);
    destroyHandle(&this->junction_program);
    destroyHandle(&this->node_program);
    destroyHandle(&this->flow_program);
    destroyHandle(&this->link_xray_program);
    destroyHandle(&this->link_program);
    markDirty(AllStreams);
}

// --- Inputs ---------------------------------------------------------------

void MapBgfxGlobeNetwork::setNetworkSnapshot(const NetworkRenderSnapshot &snapshot)
{
    this->snapshot = snapshot;
    this->snapshot_pending = true;
}

void MapBgfxGlobeNetwork::setHiddenEntityUuids(const QSet<QUuid> &uuids)
{
    if (this->hidden_entity_uuids == uuids)
        return;
    this->hidden_entity_uuids = uuids;
    this->hidden_pending = true;
}

void MapBgfxGlobeNetwork::setSymbology(const MapNetworkRenderSymbology &symbology)
{
    this->symbology = symbology;
    this->symbology_pending = true;
}

void MapBgfxGlobeNetwork::setSelectedEntity(InfrastructureEntity entity_type, const QUuid &uuid)
{
    if (this->selected_entity_type == entity_type && this->selected_entity_uuid == uuid)
        return;
    this->selected_entity_type = entity_type;
    this->selected_entity_uuid = uuid;
    this->selection_pending = true;
}

void MapBgfxGlobeNetwork::setSimulationErrorEntities(
    const QHash<QUuid, InfrastructureEntity> &error_entities, const QSet<QUuid> &stale_entity_uuids)
{
    if (this->simulation_error_entities == error_entities
        && this->simulation_stale_entity_uuids == stale_entity_uuids)
        return;
    this->simulation_error_entities = error_entities;
    this->simulation_stale_entity_uuids = stale_entity_uuids;
    this->errors_pending = true;
}

void MapBgfxGlobeNetwork::setUse3dIconModels(bool enabled)
{
    if (this->use_3d_icon_models == enabled)
        return;
    this->use_3d_icon_models = enabled;
    this->icon_models_pending = true;
}

void MapBgfxGlobeNetwork::setNodeDeclutteringEnabled(bool enabled)
{
    if (this->node_decluttering_enabled == enabled)
        return;
    this->node_decluttering_enabled = enabled;
    this->declutter_pending = true;
}

void MapBgfxGlobeNetwork::setGroundOffsetM(double offset_m)
{
    this->ground_offset_m = offset_m;
    this->ground_offset_pending = true;
}

void MapBgfxGlobeNetwork::setVerticalExaggeration(double exaggeration)
{
    this->vertical_exaggeration = exaggeration;
    this->exaggeration_pending = true;
}

void MapBgfxGlobeNetwork::setTerrainElevationResolver(MapGlobeNetworkScene::TerrainElevationResolver resolver)
{
    this->network_scene.setTerrainElevationResolver(std::move(resolver));
}

void MapBgfxGlobeNetwork::setSurfaceHeightResolver(MapGlobeNetworkScene::SurfaceHeightResolver resolver)
{
    this->network_scene.setSurfaceHeightResolver(std::move(resolver));
}

void MapBgfxGlobeNetwork::setUndergroundMode(MapUndergroundMode mode)
{
    if (this->underground_mode == mode)
        return;
    this->underground_mode = mode;
    this->underground_pending = true;
}

bool MapBgfxGlobeNetwork::refreshUndergroundXRay()
{
    synchronize();
    if (!this->network_scene.refreshUndergroundXRayGeometry())
        return false;
    markDirty(UndergroundStream);
    return true;
}

void MapBgfxGlobeNetwork::synchronize()
{
    // Placement inputs first: each rebuilds the whole network geometry.
    if (this->exaggeration_pending)
    {
        this->exaggeration_pending = false;
        if (this->network_scene.setVerticalExaggeration(this->vertical_exaggeration))
            markDirty(AllStreams);
    }
    if (this->ground_offset_pending)
    {
        this->ground_offset_pending = false;
        if (this->network_scene.setGroundOffsetM(this->ground_offset_m))
            markDirty(AllStreams);
    }
    if (this->declutter_pending)
    {
        this->declutter_pending = false;
        if (this->network_scene.setNodeDeclutteringEnabled(this->node_decluttering_enabled))
            markDirty(AllStreams);
    }
    if (this->hidden_pending)
    {
        this->hidden_pending = false;
        if (this->network_scene.setHiddenEntityUuids(this->hidden_entity_uuids))
            markDirty(AllStreams);
    }
    if (this->snapshot_pending)
    {
        this->snapshot_pending = false;
        const quint64 revision = this->network_scene.geometryRevision();
        this->network_scene.setNetworkSnapshot(this->snapshot);
        if (this->network_scene.geometryRevision() != revision)
        {
            markDirty(AllStreams);
            this->has_attachments = !this->snapshot.demand_point_attachments.isEmpty();
            this->entity_uuids.clear();
            for (const NetworkRenderNode &node : this->snapshot.nodes)
                this->entity_uuids.insert(entityKey(node.entity_type, node.render_id), node.uuid);
            for (const NetworkRenderLink &link : this->snapshot.links)
                this->entity_uuids.insert(entityKey(link.entity_type, link.render_id), link.uuid);
        }
        // Only the geometry is needed here: the copy's lists are released.
        this->snapshot = NetworkRenderSnapshot();
    }
    if (this->symbology_pending)
    {
        this->symbology_pending = false;
        // Colours never reach the vertex streams (the shaders read them from
        // MapBgfxEntityStyle): only sizes and toggles change streams, as the
        // scene rebuilds them (MapGlobeNetworkScene::setSymbology()).
        const MapNetworkRenderSymbology &previous = this->applied_symbology;
        const MapNetworkRenderSymbology &next = this->symbology;
        const bool first = !this->symbology_applied;
        const bool junctions_changed = first || previous.node_size_unit != next.node_size_unit
            || (next.node_size_unit == NetworkSymbologySizeUnit::Meters && previous.node_size_m != next.node_size_m);
        const bool thickness_changed = first || !sameSymbologySizes(previous, next);
        const bool icons_changed = first || previous.icon_size_unit != next.icon_size_unit
            || previous.icon_size_px != next.icon_size_px || previous.icon_size_m != next.icon_size_m
            || previous.show_icons != next.show_icons;
        const bool flow_changed = first || previous.show_flow_direction != next.show_flow_direction
            || previous.flow_direction_size_px != next.flow_direction_size_px
            || previous.flow_directions.isEmpty() != next.flow_directions.isEmpty() || thickness_changed;
        const bool highlights_changed = first || thickness_changed || previous.show_junctions != next.show_junctions;
        this->network_scene.setSymbology(next);
        this->applied_symbology = next;
        this->symbology_applied = true;
        if (junctions_changed)
            markDirty(JunctionStream | UndergroundStream);
        if (highlights_changed)
            markDirty(HighlightStream);
        // With demand point attachments the scene rebuilds the geometry.
        if (thickness_changed && this->has_attachments)
            markDirty(AllStreams);
        if (icons_changed)
            markDirty(IconStream);
        if (flow_changed)
            markDirty(FlowStream);
    }
    if (this->selection_pending)
    {
        this->selection_pending = false;
        this->network_scene.setSelectedEntity(this->selected_entity_type, this->selected_entity_uuid);
        // Junction and icon tints are entity flags (MapBgfxEntityStyle); the
        // outlines and the tank / reservoir models are geometry.
        markDirty(HighlightStream | IconStream);
    }
    if (this->errors_pending)
    {
        this->errors_pending = false;
        this->network_scene.setSimulationErrorEntities(this->simulation_error_entities,
                                                       this->simulation_stale_entity_uuids);
        markDirty(HighlightStream);
    }
    if (this->icon_models_pending)
    {
        this->icon_models_pending = false;
        if (this->network_scene.setUse3dIconModels(this->use_3d_icon_models))
            markDirty(IconStream);
    }
    if (this->underground_pending)
    {
        this->underground_pending = false;
        if (this->network_scene.setUndergroundXRayEnabled(this->underground_mode == MapUndergroundMode::XRay))
            markDirty(UndergroundStream | JunctionStream);
    }
}

quint64 MapBgfxGlobeNetwork::entityKey(InfrastructureEntity entity_type, quint32 render_id)
{
    return (quint64(entity_type) << 32) | quint64(render_id);
}

void MapBgfxGlobeNetwork::markDirty(quint32 streams)
{
    this->dirty_streams |= streams;
    if (streams & IconStream)
        this->model_vertices_stale = true;
}

void MapBgfxGlobeNetwork::ensureModelVertices()
{
    if (!this->model_vertices_stale)
        return;
    this->model_vertices_stale = false;
    this->tank_model_vertices = mapGlobeBuildTankModelVertices(this->network_scene.tankInstances());
    this->reservoir_model_vertices = mapGlobeBuildReservoirModelVertices(this->network_scene.reservoirInstances());
}

// --- GPU ------------------------------------------------------------------

bool MapBgfxGlobeNetwork::junctionDrawn(quint32 render_id) const
{
    const QUuid uuid = this->entity_uuids.value(entityKey(InfrastructureEntity::Junction, render_id));
    return this->applied_symbology.show_junctions && !this->hidden_entity_uuids.contains(uuid);
}

QVector<MapBgfxGlobeNetwork::JunctionVertex> MapBgfxGlobeNetwork::junctionVertices(
    const QVector<MapGlobeJunctionInstance> &instances) const
{
    // Colour, selection, diagnostics and visibility are the junction's
    // entity data (vs_globe_junction.sc): the vertices only reference it.
    QVector<JunctionVertex> vertices;
    vertices.reserve(instances.size() * 6);
    for (const MapGlobeJunctionInstance &instance : instances)
    {
        for (const float (&corner)[2] : kImpostorCorners)
        {
            JunctionVertex vertex;
            vertex.x = instance.center_x;
            vertex.y = instance.center_y;
            vertex.z = instance.center_z;
            vertex.corner_x = corner[0];
            vertex.corner_y = corner[1];
            mapNetworkSetEntityReference(&vertex, instance.render_id, false);
            vertex.radius = instance.radius_world;
            vertices.append(vertex);
        }
    }
    return vertices;
}

namespace
{
// The GUI still owns scene vertex types and layouts; Render owns the GPU
// buffer allocations, updates and teardown.
template <typename Vertex>
quint64 uploadVertices(MapBgfxGlobeBufferPool &pool, MapBgfxGlobeStream stream,
                       const bgfx::VertexLayout &layout, const QVector<Vertex> &vertices)
{
    return pool.upload(stream, layout, vertices.constData(),
                       std::size_t(vertices.size()), sizeof(Vertex));
}
}

void MapBgfxGlobeNetwork::uploadStreams(FrameResult *result)
{
    const MapGlobeNetworkScene &scene = this->network_scene;
    quint64 bytes = 0;
    if (this->dirty_streams & GeometryStream)
    {
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::Link, this->link_layout,
                                scene.linkVertices());
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::Node, this->node_layout,
                                scene.nodeVertices());
    }
    if (this->dirty_streams & HighlightStream)
    {
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::SelectedLink, this->link_layout,
                                scene.selectedLinkVertices());
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::SelectedNode, this->node_layout,
                                scene.selectedNodeVertices());
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::DiagnosticLink, this->link_layout,
                                scene.diagnosticLinkVertices());
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::DiagnosticNode, this->node_layout,
                                scene.diagnosticNodeVertices());
    }
    if (this->dirty_streams & FlowStream)
    {
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::Flow, this->link_layout,
                                scene.flowDirectionVertices());
    }
    if (this->dirty_streams & IconStream)
    {
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::Icon, this->icon_layout,
                                scene.iconVertices());
        this->icon_order_dirty = true;
        // The models are built per instance in their local frames (kept for
        // picking).
        ensureModelVertices();
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::Tank, this->model_layout,
                                this->tank_model_vertices);
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::Reservoir, this->model_layout,
                                this->reservoir_model_vertices);
    }
    if (this->dirty_streams & UndergroundStream)
    {
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::UndergroundLink, this->link_layout,
                                scene.undergroundLinkVertices());
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::UndergroundJunction,
                                this->junction_layout,
                                junctionVertices(scene.undergroundJunctionInstances()));
    }
    if (this->dirty_streams & JunctionStream)
    {
        bytes += uploadVertices(this->gpu_buffers, MapBgfxGlobeStream::Junction, this->junction_layout,
                                junctionVertices(scene.junctionInstances()));
    }
    this->dirty_streams = 0;
    result->upload_bytes += bytes;
}

void MapBgfxGlobeNetwork::updateIconDrawOrder(const QVector3D &eye, FrameResult *result)
{
    if (!this->icon_order_dirty && this->icon_order_eye == eye)
        return;
    this->icon_order_eye = eye;
    this->icon_order_dirty = false;
    const QVector<quint32> indices = mapGlobeIconDrawOrder(this->network_scene.iconVertices(), eye);
    result->upload_bytes += this->gpu_buffers.uploadIconOrder(indices.constData(),
                                                              std::size_t(indices.size()));
}

MapBgfxGlobeNetwork::FrameResult MapBgfxGlobeNetwork::render(bgfx::ViewId view, const MapGlobeCamera &camera,
                                                             const QSize &logical_viewport,
                                                             bgfx::TextureHandle icon_atlas,
                                                             const MapBgfxEntityStyle &entity_style)
{
    FrameResult result;
    if (!ready() || !entity_style.ready() || !logical_viewport.isValid())
        return result;
    synchronize();
    // An empty scene adopts the camera's origin once; networks keep their
    // own geometry anchor (camera rebases never touch the buffers).
    if (this->network_scene.setRenderOriginEcef(camera.renderOriginEcef()))
        markDirty(AllStreams);
    if (!this->network_scene.hasGeometry())
        return result;
    // Terrain LOD/window changes: bends only rebuild the network if their
    // sampled heights actually changed. Otherwise demand points update in
    // place as before. Never rebuild every frame just because tiles streamed.
    if (this->draped_surface_revision != this->terrain_surface_revision)
    {
        this->draped_surface_revision = this->terrain_surface_revision;
        if (this->network_scene.refreshDrapedPipeBendHeights())
            markDirty(AllStreams);
        else if (this->network_scene.refreshDemandPointHeights())
            markDirty(GeometryStream | IconStream | HighlightStream);
    }
    // Pixel-sized chevrons (and pixel-sized 3D models) in metres at the
    // orbit distance: rebuilt only when that scale changes.
    const double reference_depth_m = std::max(1.0, camera.orbitDistanceM());
    const double tan_half_fov = std::tan(qDegreesToRadians(MapModel::GlobeFieldOfViewDeg / 2.0));
    if (this->network_scene.setFlowDirectionPixelsPerMeter(
            double(logical_viewport.height()) / (2.0 * reference_depth_m * tan_half_fov)))
        markDirty(FlowStream | IconStream);
    if (this->dirty_streams != 0)
        uploadStreams(&result);

    const MapGlobeRenderFrame frame = mapGlobeBuildRenderFrame(
        camera, this->network_scene, this->applied_symbology, logical_viewport, logical_viewport);
    // Canonical OpenGL clip space; [0, 1] depth where bgfx has no
    // homogeneous depth: z' = (z + w) / 2. NDC depth -> window depth for the
    // impostors' gl_FragDepth accordingly.
    QMatrix4x4 view_projection = frame.canonical_view_projection;
    const bgfx::Caps *caps = bgfx::getCaps();
    const bool homogeneous_depth = caps == nullptr || caps->homogeneousDepth;
    if (!homogeneous_depth)
    {
        const QMatrix4x4 depth_correction(1.0f, 0.0f, 0.0f, 0.0f,
                                          0.0f, 1.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, 0.5f, 0.5f,
                                          0.0f, 0.0f, 0.0f, 1.0f);
        view_projection = depth_correction * view_projection;
    }
    MapBgfxGlobeGpu::FrameUniforms uniforms;
    std::copy_n(view_projection.constData(), 16, uniforms.view_projection);
    const MapGlobeRenderResources &resources = frame.resources;
    uniforms.viewport[0] = float(logical_viewport.width());
    uniforms.viewport[1] = float(logical_viewport.height());
    uniforms.viewport[2] = resources.link_thickness_unit == NetworkSymbologySizeUnit::Meters
        ? -float(resources.link_thickness_m * 0.5) : float(resources.link_thickness_px) * 0.5f;
    uniforms.viewport[3] = resources.node_size_unit == NetworkSymbologySizeUnit::Meters
        ? -float(resources.node_size_m * 0.5) : float(resources.node_size_px) * 0.5f;
    uniforms.params[0] = float(frame.orbit_distance_m);
    uniforms.params[1] = resources.icon_size_unit == NetworkSymbologySizeUnit::Meters
        ? -float(resources.icon_size_m) : float(resources.icon_size_px);
    uniforms.params[2] = homogeneous_depth ? 0.5f : 1.0f;
    uniforms.params[3] = homogeneous_depth ? 0.5f : 0.0f;
    updateIconDrawOrder(frame.camera_basis.eye, &result);
    const QVector3D *basis[3] = {&frame.camera_basis.right, &frame.camera_basis.up, &frame.camera_basis.eye};
    float *targets[3] = {uniforms.camera_right, uniforms.camera_up, uniforms.camera_eye};
    for (int index = 0; index < 3; ++index)
    {
        targets[index][0] = basis[index]->x();
        targets[index][1] = basis[index]->y();
        targets[index][2] = basis[index]->z();
        targets[index][3] = 0.0f;
    }

    // The GUI owns model adaptation and GPU resource lifetimes; the shared
    // Render library owns the entire globe-network draw order and bgfx
    // submission. Only lightweight handles and uniform values cross the ABI.
    MapBgfxGlobeGpu::Input draw_input;
    draw_input.view = view;
    // Use named stream/program slots rather than positional copies: adding a
    // new GPU pass cannot silently shift an existing handle to the wrong
    // shader or vertex layout.
    this->gpu_buffers.populateInput(&draw_input);
    draw_input.setProgram(MapBgfxGlobeProgram::Link, this->link_program);
    draw_input.setProgram(MapBgfxGlobeProgram::LinkXRay, this->link_xray_program);
    draw_input.setProgram(MapBgfxGlobeProgram::Flow, this->flow_program);
    draw_input.setProgram(MapBgfxGlobeProgram::Node, this->node_program);
    draw_input.setProgram(MapBgfxGlobeProgram::Junction, this->junction_program);
    draw_input.setProgram(MapBgfxGlobeProgram::JunctionXRay, this->junction_xray_program);
    draw_input.setProgram(MapBgfxGlobeProgram::Icon, this->icon_program);
    draw_input.setProgram(MapBgfxGlobeProgram::Model, this->model_program);
    draw_input.uniforms = {
        this->view_projection_uniform, this->viewport_uniform, this->params_uniform,
        this->camera_right_uniform, this->camera_up_uniform, this->camera_eye_uniform
    };
    draw_input.icon_atlas = icon_atlas;
    draw_input.tank_texture = this->tank_texture;
    draw_input.reservoir_texture = this->reservoir_texture;
    draw_input.icon_sampler = this->icon_sampler;
    draw_input.albedo_sampler = this->albedo_sampler;
    draw_input.show_junctions = resources.show_junctions;
    switch (this->underground_mode)
    {
    case MapUndergroundMode::Solid:
        draw_input.underground = MapBgfxGlobeUndergroundMode::Solid;
        break;
    case MapUndergroundMode::XRay:
        draw_input.underground = MapBgfxGlobeUndergroundMode::XRay;
        break;
    case MapUndergroundMode::Hide:
        draw_input.underground = MapBgfxGlobeUndergroundMode::Hide;
        break;
    }
    result.draw_calls += int(MapBgfxGlobeGpu::draw(draw_input, uniforms, entity_style));
    return result;
}

// --- Picking (slice 4) -----------------------------------------------------

namespace
{
// Hit radii of the QRhi globe (larger than 2D: perspective views).
constexpr double GlobeMinimumNodeHitRadiusPx = 16.0;
constexpr double GlobeNodeHitPaddingPx = 9.0;
constexpr double GlobeMinimumLinkHitRadiusPx = 12.0;
constexpr double GlobeLinkHitPaddingPx = 8.0;
constexpr double GlobeMinimumIconHitRadiusPx = 18.0;
constexpr double GlobeIconHitPaddingPx = 10.0;

bool finitePoint(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

// Canonical clip space (NDC y up) to logical screen pixels (y down); NaN
// behind the camera.
QPointF projectToScreen(const QMatrix4x4 &view_projection, const QVector3D &position, const QSize &viewport)
{
    const QVector4D clip = view_projection * QVector4D(position, 1.0f);
    if (!(clip.w() > 1e-6f))
        return QPointF(qQNaN(), qQNaN());
    const double ndc_x = double(clip.x() / clip.w());
    const double ndc_y = double(clip.y() / clip.w());
    return QPointF((ndc_x + 1.0) * viewport.width() / 2.0, (1.0 - ndc_y) * viewport.height() / 2.0);
}

double pointSegmentDistance(const QPointF &point, const QPointF &start, const QPointF &end)
{
    const QPointF segment = end - start;
    const double length_squared = segment.x() * segment.x() + segment.y() * segment.y();
    if (length_squared <= 1e-12)
        return QLineF(point, start).length();
    const QPointF relative = point - start;
    const double ratio = qBound(0.0, (relative.x() * segment.x() + relative.y() * segment.y()) / length_squared, 1.0);
    return QLineF(point, QPointF(start.x() + segment.x() * ratio, start.y() + segment.y() * ratio)).length();
}

// Nearest model triangle along the ray (model vertices are triangle lists
// relative to the network origin); 0: none.
template <typename Vertex>
quint32 nearestModelHit(const QVector<Vertex> &vertices, const QVector3D &ray_origin,
                        const QVector3D &ray_direction)
{
    quint32 best_render_id = 0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (qsizetype index = 0; index + 2 < vertices.size(); index += 3)
    {
        const Vertex &a = vertices.at(index);
        const Vertex &b = vertices.at(index + 1);
        const Vertex &c = vertices.at(index + 2);
        if (a.render_id == 0 || a.render_id != b.render_id || a.render_id != c.render_id)
            continue;
        double distance = 0.0;
        if (!mapGlobeRayTriangleIntersectionDistance(
                ray_origin, ray_direction, QVector3D(a.position_x, a.position_y, a.position_z),
                QVector3D(b.position_x, b.position_y, b.position_z),
                QVector3D(c.position_x, c.position_y, c.position_z), &distance)
            || !mapGlobeUpdateNearestHitDistance(distance, &best_distance))
            continue;
        best_render_id = a.render_id;
    }
    return best_render_id;
}
}

MapRenderHit MapBgfxGlobeNetwork::entityHit(InfrastructureEntity entity_type, quint32 render_id) const
{
    MapRenderHit hit;
    const QUuid uuid = this->entity_uuids.value(entityKey(entity_type, render_id));
    if (uuid.isNull() || this->hidden_entity_uuids.contains(uuid))
        return hit;
    hit.render_id = render_id;
    hit.entity_type = entity_type;
    hit.uuid = uuid;
    return hit;
}

MapRenderHit MapBgfxGlobeNetwork::hitTest(const QPointF &screen_position, const MapGlobeCamera &camera,
                                          const QSize &logical_viewport)
{
    MapRenderHit no_hit;
    synchronize();
    if (!this->network_scene.hasGeometry() || !logical_viewport.isValid() || !finitePoint(screen_position))
        return no_hit;
    ensureModelVertices();
    const MapGlobeNetworkScene &scene = this->network_scene;
    const QSize viewport(std::max(1, logical_viewport.width()), std::max(1, logical_viewport.height()));
    const GeoWgs84Ellipsoid::EcefPositionD origin = scene.renderOriginEcef();
    MapGlobeCameraBasis basis;
    const QMatrix4x4 view_projection = camera.viewProjectionMatrixForOrigin(origin, &basis);

    // 3D tank / reservoir models: the screen ray against their triangles
    // (with 3D icons on, their billboard icons are gone).
    MapGlobeScreenRay ray;
    if (camera.screenRay(screen_position, &ray))
    {
        const QVector3D ray_origin(float(ray.origin_ecef.x - origin.x), float(ray.origin_ecef.y - origin.y),
                                   float(ray.origin_ecef.z - origin.z));
        const quint32 tank = nearestModelHit(this->tank_model_vertices, ray_origin, ray.direction);
        if (tank != 0)
        {
            const MapRenderHit hit = entityHit(InfrastructureEntity::Tank, tank);
            if (hit.isValid())
                return hit;
        }
        const quint32 reservoir = nearestModelHit(this->reservoir_model_vertices, ray_origin, ray.direction);
        if (reservoir != 0)
        {
            const MapRenderHit hit = entityHit(InfrastructureEntity::Reservoir, reservoir);
            if (hit.isValid())
                return hit;
        }
    }

    // Junction spheres (only drawn ones), by their projected radius.
    double best_junction_distance = std::numeric_limits<double>::infinity();
    quint32 best_junction = 0;
    for (const MapGlobeJunctionInstance &instance : scene.junctionInstances())
    {
        if (!junctionDrawn(instance.render_id))
            continue;
        const QVector3D center(instance.center_x, instance.center_y, instance.center_z);
        const QPointF projected = projectToScreen(view_projection, center, viewport);
        if (!finitePoint(projected))
            continue;
        double visual_radius_px = GlobeMinimumNodeHitRadiusPx;
        if (instance.radius_world > 0.0f)
        {
            const QPointF edge = projectToScreen(view_projection, center + basis.right * instance.radius_world,
                                                 viewport);
            if (finitePoint(edge))
                visual_radius_px = QLineF(projected, edge).length();
        }
        const double hit_radius = std::max(GlobeMinimumNodeHitRadiusPx, visual_radius_px + GlobeNodeHitPaddingPx);
        const double distance = QLineF(screen_position, projected).length();
        if (distance > hit_radius || distance >= best_junction_distance)
            continue;
        best_junction_distance = distance;
        best_junction = instance.render_id;
    }
    if (best_junction != 0)
    {
        const MapRenderHit hit = entityHit(InfrastructureEntity::Junction, best_junction);
        if (hit.isValid())
            return hit;
    }

    // Icons: the projected billboard rectangle (atlas aspect ratio, the
    // icon shader's depth-dependent size).
    const double tan_half_fov = std::tan(qDegreesToRadians(MapModel::GlobeFieldOfViewDeg / 2.0));
    double best_icon_distance = std::numeric_limits<double>::infinity();
    quint32 best_icon = 0;
    InfrastructureEntity best_icon_type = InfrastructureEntity::Unknown;
    const QVector<MapNetworkIconVertex> &icons = scene.iconVertices();
    for (qsizetype index = 0; index + 5 < icons.size(); index += 6)
    {
        const MapNetworkIconVertex &icon = icons.at(index);
        const QVector3D center(icon.center_x, icon.center_y, icon.center_z);
        const QPointF projected = projectToScreen(view_projection, center, viewport);
        if (!finitePoint(projected))
            continue;
        const double depth_m = std::abs(double((view_projection * QVector4D(center, 1.0f)).w()));
        if (!std::isfinite(depth_m) || depth_m <= 0.000001)
            continue;
        double icon_size_px = double(scene.iconSizePx());
        if (scene.iconSizeUnit() == NetworkSymbologySizeUnit::Meters)
            icon_size_px = scene.iconSizeM() * double(viewport.height()) / (2.0 * depth_m * tan_half_fov);
        else
            icon_size_px *= std::max(1.0, camera.orbitDistanceM()) / depth_m;
        const double half_width = std::max(GlobeMinimumIconHitRadiusPx,
                                           std::abs(double(icon.offset_x_ratio)) * icon_size_px
                                               + GlobeIconHitPaddingPx);
        const double half_height = std::max(GlobeMinimumIconHitRadiusPx,
                                            std::abs(double(icon.offset_y_ratio)) * icon_size_px
                                                + GlobeIconHitPaddingPx);
        if (std::abs(screen_position.x() - projected.x()) > half_width
            || std::abs(screen_position.y() - projected.y()) > half_height)
            continue;
        const double distance = QLineF(screen_position, projected).length();
        if (distance >= best_icon_distance)
            continue;
        best_icon_distance = distance;
        best_icon = icon.render_id;
        best_icon_type = icon.entity_type;
    }
    if (best_icon != 0)
    {
        const MapRenderHit hit = entityHit(best_icon_type, best_icon);
        if (hit.isValid())
            return hit;
    }

    // Generic nodes (not junctions; not tanks/reservoirs shown as models,
    // whose invisible centre points would be targets outside the model).
    QSet<quint32> modeled_tanks;
    for (const MapGlobeTankInstance &instance : scene.tankInstances())
        modeled_tanks.insert(instance.render_id);
    QSet<quint32> modeled_reservoirs;
    for (const MapGlobeReservoirInstance &instance : scene.reservoirInstances())
        modeled_reservoirs.insert(instance.render_id);
    double best_node_distance = std::numeric_limits<double>::infinity();
    quint32 best_node = 0;
    InfrastructureEntity best_node_type = InfrastructureEntity::Unknown;
    const QVector<MapNetworkNodeVertex> &nodes = scene.nodeVertices();
    for (qsizetype index = 0; index + 5 < nodes.size(); index += 6)
    {
        const MapNetworkNodeVertex &node = nodes.at(index);
        if (node.entity_type == InfrastructureEntity::Junction
            || (node.entity_type == InfrastructureEntity::Tank && modeled_tanks.contains(node.render_id))
            || (node.entity_type == InfrastructureEntity::Reservoir && modeled_reservoirs.contains(node.render_id)))
            continue;
        const QVector3D center(node.center_x, node.center_y, node.center_z);
        const QPointF projected = projectToScreen(view_projection, center, viewport);
        if (!finitePoint(projected))
            continue;
        double visual_radius_px = scene.nodeSizePx() * 0.5;
        if (scene.nodeSizeUnit() == NetworkSymbologySizeUnit::Meters)
        {
            const QPointF edge = projectToScreen(
                view_projection, center + basis.right * float(scene.nodeSizeM() * 0.5), viewport);
            if (finitePoint(edge))
                visual_radius_px = QLineF(projected, edge).length();
        }
        visual_radius_px = std::max(0.0, visual_radius_px + double(node.size_adjust_px));
        const double hit_radius = std::max(GlobeMinimumNodeHitRadiusPx, visual_radius_px + GlobeNodeHitPaddingPx);
        const double distance = QLineF(screen_position, projected).length();
        if (distance > hit_radius || distance >= best_node_distance)
            continue;
        best_node_distance = distance;
        best_node = node.render_id;
        best_node_type = node.entity_type;
    }
    if (best_node != 0)
    {
        const MapRenderHit hit = entityHit(best_node_type, best_node);
        if (hit.isValid())
            return hit;
    }

    // Pipes, pumps, valves: distance to each projected segment (six vertices
    // per segment share start and end).
    const double fixed_half_width_px = scene.linkThicknessPx() * 0.5;
    double best_link_distance = std::numeric_limits<double>::infinity();
    quint32 best_link = 0;
    InfrastructureEntity best_link_type = InfrastructureEntity::Unknown;
    const QVector<MapNetworkLinkVertex> &links = scene.linkVertices();
    for (qsizetype index = 0; index + 5 < links.size(); index += 6)
    {
        const MapNetworkLinkVertex &link = links.at(index);
        const QVector3D start(link.start_x, link.start_y, link.start_z);
        const QVector3D end(link.end_x, link.end_y, link.end_z);
        const QPointF start_screen = projectToScreen(view_projection, start, viewport);
        const QPointF end_screen = projectToScreen(view_projection, end, viewport);
        if (!finitePoint(start_screen) || !finitePoint(end_screen))
            continue;
        double half_width_px = fixed_half_width_px;
        if (scene.linkThicknessUnit() == NetworkSymbologySizeUnit::Meters)
        {
            QVector3D width_direction(link.width_direction_x, link.width_direction_y, link.width_direction_z);
            if (width_direction.lengthSquared() > 0.000001f)
                width_direction.normalize();
            else
                width_direction = basis.right;
            const QVector3D midpoint = (start + end) * 0.5f;
            const QPointF midpoint_screen = projectToScreen(view_projection, midpoint, viewport);
            const QPointF edge_screen = projectToScreen(
                view_projection, midpoint + width_direction * float(scene.linkThicknessM() * 0.5), viewport);
            if (finitePoint(midpoint_screen) && finitePoint(edge_screen))
                half_width_px = QLineF(midpoint_screen, edge_screen).length();
        }
        half_width_px = std::max(0.0, half_width_px + double(link.size_adjust_px));
        const double hit_radius = std::max(GlobeMinimumLinkHitRadiusPx, half_width_px + GlobeLinkHitPaddingPx);
        const double distance = pointSegmentDistance(screen_position, start_screen, end_screen);
        if (distance > hit_radius || distance >= best_link_distance)
            continue;
        best_link_distance = distance;
        best_link = link.render_id;
        best_link_type = link.entity_type;
    }
    if (best_link != 0)
        return entityHit(best_link_type, best_link);
    return no_hit;
}
