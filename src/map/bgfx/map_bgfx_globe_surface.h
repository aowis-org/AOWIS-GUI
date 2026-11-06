#ifndef MAP_BGFX_GLOBE_SURFACE_H
#define MAP_BGFX_GLOBE_SURFACE_H

#include "map/bgfx/map_bgfx_2d_imagery_gpu.h"
#include "map/bgfx/map_bgfx_globe_tile_index_pool.h"
#include "map/render/map_globe_camera.h"
#include "map/render/map_globe_heatmap_scene.h"
#include "map/render/map_globe_surface_preparation.h"
#include "map/render/map_terrain_mesh_scheduler.h"

#include <QColor>
#include <QHash>
#include <QSize>
#include <QString>
#include <QVector>

#include <array>

#include <bgfx/bgfx.h>

class MapModel;
class MapTerrainRepository;
class MapTileRepository;

// The globe's terrain surface on bgfx (roadmap Phase 3, slice 1): the
// WGS84 ellipsoid with DEM relief and basemap imagery, the polar caps and
// the wireframe debug view, drawn into the map view before the HUD.
//
// All CPU work is the backend-neutral MapGlobeSurfacePreparation the QRhi
// globe uses as well: quadtree leaf selection with horizon and view-cone
// culling, ellipsoid and cap geometry relative to the sticky render origin,
// DEM requests at the DEM zoom of each tile (MapGlobeTerrainLevels, every
// mesh at its DEM's full resolution), asynchronous terrain meshes (applied
// as vertex-range patches) and the wireframe edges. This class only keeps
// that geometry on the GPU and draws it:
//
// - window and cap vertices in dynamic buffers: uploaded completely when
//   the window or the caps are rebuilt, terrain meshes as patches of the
//   tile's vertex range; window tiles share one static index buffer per
//   cell count (Render's MapBgfxGlobeTileIndexPool), the caps have their own;
// - imagery from the 2D imagery atlas (MapBgfx2dImageryGpu, shared: a tile
//   is uploaded once for both views). Each tile is one indexed draw (its
//   cell count's topology from its first vertex) with its atlas rectangle
//   as a uniform: its own cell, or the
//   covering part of its nearest resident ancestor while its own imagery is
//   missing (map_bgfx_globe_imagery.h; no CPU compositing), or a solid
//   colour texel (nothing loaded yet, polar caps);
// - depth test and write (LessEqual) like the QRhi pipeline; the camera's
//   canonical OpenGL clip space is corrected to [0, 1] depth for backends
//   without homogeneous depth.
//
// Imagery requests go to the shared MapTileRepository in one batch per
// window rebuild; uploads into the atlas share the caller's per-frame budget.
//
// The heatmap (slice 3b) lies on the terrain like the QRhi globe's: per
// window tile, the stamps of MapGlobeHeatmapScene (radial gradients around
// the network nodes, in the tile's own 256 x 256 pixel space) are baked on
// the GPU into a cell of a heatmap atlas page (2048 x 2048 render target,
// 64 cells, up to 4 pages created as needed), in bake views that run before
// the map view (bgfx::setViewOrder). A tile with content is drawn by the
// heatmap variant of the surface program, which composites its cell over
// the imagery. Cells are kept per tile and scene revision (LRU), baked
// within a per-frame budget; tiles without stamps need no cell.
//
// The sky: before the surface, one screen triangle adds the light of the
// atmosphere along each view ray onto the space colour (fs_globe_sky.sc: a
// spherical exponential atmosphere, integrated per pixel). Blue from close
// to the ground, darker with altitude, black above the atmosphere except for
// the thin bright band along the globe's limb; the surface covers it below
// the horizon. Drawn with the map tiles (not in the wireframe-only view).
class MapBgfxGlobeSurface
{
public:
    explicit MapBgfxGlobeSurface(MapModel *map_model);
    ~MapBgfxGlobeSurface();

    // GPU objects (programs, uniforms, solid-colour textures). false: the
    // globe is unavailable on this device (the 2D map is unaffected).
    bool initialize();
    void release();
    bool ready() const;

    void setTileRepository(MapTileRepository *tile_repository);
    void setTerrainRepository(MapTerrainRepository *terrain_repository);
    void notifyTerrainTileAvailable(const QString &key);
    // A failed terrain tile became retryable, or the provider changed.
    void requestAgain();
    // Map provider switch: the window's imagery keys follow the new provider
    // and its tiles are requested again (the textures were cleared).
    void providerChanged();
    // Selects the visible window now (outside a frame) and requests its DEM
    // tiles: terrain starts loading for a freshly fitted network before the
    // next frame. logical_viewport as FrameInputs::logical_viewport.
    void requestTerrainForCurrentView(const QSize &logical_viewport);
    // Entering the globe: the next frame selects the window from scratch.
    void invalidateTerrainView();
    // The vertical exaggeration changed: every terrain mesh is rebuilt.
    void invalidateTerrain();
    void setWireframeVisible(bool visible);
    void setMapVisible(bool visible);
    // Imagery drawn on the terrain (the HUD's "map" toggle).
    bool mapVisible() const { return this->map_visible; }
    // Hillshading of the terrain (the HUD's "hillshade" toggle).
    void setHillshadeVisible(bool visible);
    bool hillshadeVisible() const { return this->hillshade_visible; }

    // The network heatmap on the terrain: markers (lon/lat, ramp colour),
    // radius in metres and solid-centre fraction; no markers: no heatmap.
    // Re-bakes only when this changes anything (scene revision).
    void setHeatmapOverlay(const QVector<MapGlobeHeatmapMarker> &markers, double radius_m,
                           double solid_fraction);

    // One bake view per heatmap atlas page, owned by the parent map's
    // contiguous view reservation. No global view 0..4 assumption.
    static constexpr int HeatmapPageCount = 4;
    void setHeatmapBakeFirstView(bgfx::ViewId first) { this->heatmap_bake_first_view = first; }

    struct FrameInputs
    {
        QSize logical_viewport;
        QColor background;
        // 0..1: imagery faded towards the background ("slide opacity").
        double background_opacity = 0.0;
        // Atlas uploads this frame may still make (shared with 2D budget).
        int upload_budget = 0;
        // 0..1: heatmap opacity (symbology); 0 skips the heatmap.
        double heatmap_opacity = 0.0;
    };
    struct FrameResult
    {
        int uploads = 0;
        // Tiles waiting for the upload budget or terrain meshes still
        // being built: keep presenting frames.
        bool pending = false;
        int tiles_drawn = 0;
        // Heatmap cells baked this frame / tiles drawn with a heatmap.
        int heatmap_bakes = 0;
        int heatmap_tiles = 0;
    };
    // Prepares (selection, terrain, imagery) and draws the surface into view
    // with camera (viewport and origin already synchronized by the caller).
    FrameResult render(bgfx::ViewId view, const MapGlobeCamera &camera,
                       MapBgfx2dImageryGpu *imagery, const FrameInputs &inputs);

    bool hasPendingTerrainMeshes() const;
    // Changes whenever the drawn terrain geometry changes (window rebuilt,
    // terrain meshes applied, render origin rebased): what is draped on it
    // is placed again.
    quint64 surfaceRevision() const { return this->surface_revision; }
    void terrainMeshProgress(int *completed, int *total, bool *active) const;
    const MapGlobeSurfacePreparation &preparation() const { return this->surface_preparation; }

    // Clear colour of the globe view ("deep space", as the QRhi globe).
    static constexpr quint32 SpaceColorRgba = 0x05050dffu;

private:
    // Per window tile: which atlas rectangle it samples this frame, and its
    // heatmap cell (page < 0: no heatmap).
    struct TileImagery
    {
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        float uniform[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        int heatmap_page = -1;
        float heatmap_uniform[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    };
    // One heatmap atlas page: render target texture and its frame buffer.
    struct HeatmapPage
    {
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        bgfx::FrameBufferHandle frame_buffer = BGFX_INVALID_HANDLE;
    };
    // A tile's heatmap: the scene revision it shows, its cell (if it has
    // content) and the stamp layout cache of MapGlobeHeatmapScene.
    struct HeatmapCell
    {
        quint64 revision = 0;
        int page = -1;
        int slot = -1;
        quint64 last_used_frame = 0;
        MapGlobeHeatmapTileLayoutCache layout_cache;
    };
    // One stamp-quad corner of the bake (vs_globe_heatmap_bake).
    struct HeatmapBakeVertex
    {
        float corner_x = 0.0f;
        float corner_y = 0.0f;
        float center_x = 0.0f;
        float center_y = 0.0f;
        float radius = 0.0f;
        float solid_fraction = 0.0f;
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        float alpha = 1.0f;
        float cell_x = 0.0f;
        float cell_y = 0.0f;
    };

    void uploadWindowGeometry();
    void uploadCapGeometry();
    void uploadWireframe();
    void requestImagery();
    // Resolves every window tile's imagery; uploads within the budget.
    void resolveImagery(MapBgfx2dImageryGpu *imagery, FrameResult *result, int upload_budget);
    // Assigns heatmap cells to the window tiles, bakes stale ones within the
    // budget (bake views) and sets TileImagery::heatmap_*.
    void resolveHeatmap(double opacity, FrameResult *result);
    // A free cell (page, slot), evicting the least recently used cell not
    // used this frame; creates pages as needed. false: all cells in use.
    bool allocateHeatmapCell(HeatmapCell *cell);
    void releaseHeatmap();
    // The sky triangle (see above); before everything else in the view.
    void drawSky(bgfx::ViewId view, const MapGlobeCameraBasis &basis,
                 const GeoWgs84Ellipsoid::EcefPositionD &render_origin, const QSize &logical_viewport);
    void releaseSky();
    void destroyBuffers();

    MapModel *map_model = nullptr;
    MapTileRepository *tile_repository = nullptr;
    MapGlobeSurfacePreparation surface_preparation;

    bgfx::VertexLayout surface_layout;
    bgfx::VertexLayout wireframe_layout;
    bgfx::ProgramHandle surface_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle wireframe_program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle view_projection_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle tile_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle background_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle tile_sampler = BGFX_INVALID_HANDLE;
    // 1x1 solid textures: imagery not loaded yet, polar caps.
    bgfx::TextureHandle missing_texture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle cap_texture = BGFX_INVALID_HANDLE;

    bgfx::DynamicVertexBufferHandle window_vertices = BGFX_INVALID_HANDLE;
    // GPU ownership and reuse of per-resolution tile/skirt topology.
    MapBgfxGlobeTileIndexPool tile_index_pool;
    bgfx::DynamicVertexBufferHandle cap_vertices = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle cap_indices = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle wireframe_vertices = BGFX_INVALID_HANDLE;
    bool window_upload_pending = true;
    bool cap_upload_pending = true;
    bool wireframe_upload_pending = true;
    bool window_tiles_requested = false;
    bool map_visible = true;
    bool hillshade_visible = true;
    // Hillshading (vs/fs_globe_surface*): the render origin (normals and the
    // local light frame), the light in east/north/up, x strength y the flat
    // ground's lambert (normalisation).
    bgfx::UniformHandle origin_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle light_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shading_uniform = BGFX_INVALID_HANDLE;
    quint64 surface_revision = 1;
    QVector<TileImagery> window_imagery;

    // Heatmap (slice 3b).
    MapGlobeHeatmapScene heatmap_scene;
    bgfx::ProgramHandle heatmap_surface_program = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle heatmap_bake_program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle heatmap_tile_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle heatmap_style_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle heatmap_bake_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle heatmap_sampler = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout heatmap_bake_layout;
    bgfx::ViewId heatmap_bake_first_view = 1;
    HeatmapPage heatmap_pages[HeatmapPageCount];
    // Cells by tile key (zoom, virtual x, y); slot owners per page.
    QHash<quint64, HeatmapCell> heatmap_cells;
    QVector<quint64> heatmap_slot_owners[HeatmapPageCount];
    quint64 heatmap_frame = 0;

    // Sky (optional: without its program the globe draws as before).
    bgfx::ProgramHandle sky_program = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout sky_layout;
    bgfx::VertexBufferHandle sky_triangle = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sky_forward_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sky_right_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sky_up_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sky_vertical_uniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sky_params_uniform = BGFX_INVALID_HANDLE;
};

#endif // MAP_BGFX_GLOBE_SURFACE_H
