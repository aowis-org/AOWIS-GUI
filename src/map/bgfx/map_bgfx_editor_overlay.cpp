#include "map/bgfx/map_bgfx_editor_overlay.h"
#include "map/editor/map_canvas_projection.h"

#include "network/infrastructure_entity_traits.h"
#include "geo/geo_wgs84_ellipsoid.h"
#include "map/render/map_render_diagnostics.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace
{
// Glow colours of the selection frames (as the former QPainter editor).
constexpr quint32 rgba(int red, int green, int blue, int alpha)
{
    return (quint32(alpha & 0xff) << 24) | (quint32(red & 0xff) << 16)
        | (quint32(green & 0xff) << 8) | quint32(blue & 0xff);
}

// The tile-cache selection's colours: map tiles in the editor's "borg"
// green, terrain tiles in the same scheme in earth brown.
struct TileSelectionPalette
{
    // Diagonal fill gradient (top-left, middle, bottom-right).
    quint32 fill[3];
    // Frame strokes: wide glow, inner glow, crisp line.
    quint32 glow_wide;
    quint32 glow_inner;
    quint32 line;
    quint32 grid;
};

TileSelectionPalette tileSelectionPalette(MapEditorTileSelectionKind kind)
{
    if (kind == MapEditorTileSelectionKind::Terrain)
    {
        return TileSelectionPalette{
            {rgba(204, 146, 86, 64), rgba(156, 102, 54, 76), rgba(104, 64, 30, 88)},
            rgba(184, 118, 58, 64), rgba(206, 140, 74, 132), rgba(240, 202, 150, 235),
            rgba(222, 168, 108, 115)};
    }
    return TileSelectionPalette{
        {rgba(92, 255, 82, 54), rgba(32, 224, 58, 66), rgba(8, 132, 38, 76)},
        rgba(60, 255, 78, 54), rgba(92, 255, 96, 120), rgba(155, 255, 145, 230),
        rgba(104, 255, 104, 105)};
}

// Colour of the selection's diagonal fill gradient at t (0 = top-left,
// 1 = bottom-right).
quint32 tileSelectionFillAt(const TileSelectionPalette &palette, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t <= 0.5
        ? MapBgfxHudCanvas::interpolateArgb(palette.fill[0], palette.fill[1], t / 0.5)
        : MapBgfxHudCanvas::interpolateArgb(palette.fill[1], palette.fill[2], (t - 0.5) / 0.5);
}

bool isPointEntity(InfrastructureEntity entity)
{
    return InfrastructureEntityTraits::isHydraulicPointEntity(entity);
}

bool isDeviceLink(InfrastructureEntity entity)
{
    return InfrastructureEntityTraits::isHydraulicDeviceLink(entity);
}

double normalizeLongitude(double longitude)
{
    double result = std::fmod(longitude + 180.0, 360.0);
    if (result < 0.0)
        result += 360.0;
    return result - 180.0;
}

// The former QPainter editor's deviceLinkCenterCoordinate().
CoordinateWGS84 deviceLinkCenter(const QList<CoordinateWGS84> &vertices)
{
    if (vertices.size() > 2)
        return vertices.at(1);
    CoordinateWGS84 center;
    if (vertices.size() < 2)
        return center;
    const CoordinateWGS84 &start = vertices.first();
    const CoordinateWGS84 &end = vertices.last();
    center.latitude_deg = (start.latitude_deg + end.latitude_deg) / 2.0;
    const double longitude_delta = normalizeLongitude(end.longitude_deg - start.longitude_deg);
    center.longitude_deg = normalizeLongitude(start.longitude_deg + longitude_delta / 2.0);
    return center;
}

double deviceLinkElevationM(const NetworkRenderLink &link)
{
    if (link.vertices_wgs84.size() > 2 && link.elevations_m.size() > 1)
        return link.elevations_m.at(1);
    if (link.elevations_m.size() >= 2)
        return 0.5 * (link.elevations_m.first() + link.elevations_m.last());
    return std::numeric_limits<double>::quiet_NaN();
}

MapBgfx2d::WorldPoint world(const CoordinateWGS84 &coordinate)
{
    return MapBgfx2d::worldFromWgs84(coordinate.longitude_deg, coordinate.latitude_deg);
}

// Liang-Barsky: the part [t0, t1] of a -> b inside the rect; false when
// none.
bool clipSegment(const MapBgfxHudPoint &a, const MapBgfxHudPoint &b, double left, double top,
                 double right, double bottom, double *t0, double *t1)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x - left, right - a.x, a.y - top, bottom - a.y};
    double start = 0.0;
    double end = 1.0;
    for (int side = 0; side < 4; ++side)
    {
        if (p[side] == 0.0)
        {
            if (q[side] < 0.0)
                return false;
            continue;
        }
        const double t = q[side] / p[side];
        if (p[side] < 0.0)
            start = std::max(start, t);
        else
            end = std::min(end, t);
        if (start > end)
            return false;
    }
    *t0 = start;
    *t1 = end;
    return true;
}

bool finite(const CoordinateWGS84 &coordinate)
{
    return std::isfinite(coordinate.longitude_deg) && std::isfinite(coordinate.latitude_deg);
}

MapBgfxHudPoint midpoint(const MapBgfxHudPoint &a, const MapBgfxHudPoint &b)
{
    return MapBgfxHudPoint{0.5 * (a.x + b.x), 0.5 * (a.y + b.y)};
}

MapBgfxEditorOverlay::Highlight diagnosticHighlight(bool selected, bool stale)
{
    if (selected)
        return stale ? MapBgfxEditorOverlay::Highlight::SelectedStale
                     : MapBgfxEditorOverlay::Highlight::SelectedError;
    return stale ? MapBgfxEditorOverlay::Highlight::Stale
                 : MapBgfxEditorOverlay::Highlight::Error;
}
}

void MapBgfxEditorOverlay::synchronizeGeometry(const NetworkRenderSnapshot &snapshot)
{
    if (this->geometry.valid && this->geometry.geometry_revision == snapshot.geometry_revision
        && this->geometry.node_world.size() == snapshot.nodes.size()
        && this->geometry.link_world.size() == snapshot.links.size())
    {
        this->geometry.snapshot = &snapshot;
        return;
    }
    Geometry built;
    built.valid = true;
    built.geometry_revision = snapshot.geometry_revision;
    built.snapshot = &snapshot;
    built.node_world.reserve(snapshot.nodes.size());
    for (qsizetype index = 0; index < snapshot.nodes.size(); ++index)
    {
        const NetworkRenderNode &node = snapshot.nodes.at(index);
        built.node_world.append(world(node.coordinate_wgs84));
        built.node_index.insert(node.uuid, int(index));
    }
    built.link_world.reserve(snapshot.links.size());
    built.link_bounds.reserve(snapshot.links.size());
    built.device_center_world.reserve(snapshot.links.size());
    for (qsizetype index = 0; index < snapshot.links.size(); ++index)
    {
        const NetworkRenderLink &link = snapshot.links.at(index);
        QVector<MapBgfx2d::WorldPoint> points;
        points.reserve(link.vertices_wgs84.size());
        for (const CoordinateWGS84 &vertex : link.vertices_wgs84)
            points.append(world(vertex));
        Geometry::Bounds bounds;
        if (!points.isEmpty())
        {
            double previous = points.first().x;
            bounds.min_x = bounds.max_x = previous;
            bounds.min_y = bounds.max_y = points.first().y;
            for (const MapBgfx2d::WorldPoint &point : points)
            {
                const double x = MapBgfx2d::wrapNearest(point.x, previous);
                previous = x;
                bounds.min_x = std::min(bounds.min_x, x);
                bounds.max_x = std::max(bounds.max_x, x);
                bounds.min_y = std::min(bounds.min_y, point.y);
                bounds.max_y = std::max(bounds.max_y, point.y);
            }
        }
        built.link_bounds.append(bounds);
        built.link_world.append(points);
        built.device_center_world.append(world(deviceLinkCenter(link.vertices_wgs84)));
        built.link_index.insert(link.uuid, int(index));
    }
    this->geometry = std::move(built);
}

void MapBgfxEditorOverlay::build(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *cache,
                                 const Inputs &inputs)
{
    canvas->clear();
    if (inputs.snapshot == nullptr || inputs.visual_state == nullptr
        || inputs.viewport_state == nullptr || !(inputs.camera.world_pixels > 0.0))
        return;
    this->canvas = canvas;
    this->cache = cache;
    this->inputs = &inputs;
    if (!inputs.tile_only)
        synchronizeGeometry(*inputs.snapshot);

    // Membership tests per entity: sets instead of the visual state's lists
    // (a full selection would make every icon an O(n) scan).
    const MapEditorVisualState &state = *inputs.visual_state;
    this->selected_markers = QSet<QUuid>(state.selected_marker_uuids.cbegin(),
                                         state.selected_marker_uuids.cend());
    this->selected_pipes = QSet<QUuid>(state.selected_pipe_uuids.cbegin(),
                                       state.selected_pipe_uuids.cend());
    this->moving_markers.clear();
    this->moving_links.clear();
    if (state.move.active)
    {
        for (const MapEditorDynamicMarkerVisualState &marker : state.move.markers)
            this->moving_markers.insert(marker.uuid);
        for (const MapEditorDynamicLinkVisualState &link : state.move.links)
            this->moving_links.insert(link.uuid);
    }

    cache->beginFrame();
    buildOnce();
    if (cache->evictedSinceFrameStart())
    {
        // The atlas was cleared mid-build: rebuild against the refilled atlas.
        cache->beginFrame();
        canvas->clear();
        buildOnce();
    }
    this->canvas = nullptr;
    this->cache = nullptr;
    this->inputs = nullptr;
}

void MapBgfxEditorOverlay::buildOnce()
{
    this->canvas->setWhiteTexel(this->cache->whiteU(), this->cache->whiteV());
    this->offset_x = 0.0;
    this->offset_y = 0.0;
    const MapEditorVisualState &state = *this->inputs->visual_state;

    // The former QPainter editor's paintRenderSurfaceOverlay(), same order.
    tileSelection();
    rectangleSelection();
    if (this->inputs->tile_only)
        return;

    if (state.move.active && this->inputs->full_network_move)
    {
        // The GPU network is already shifted; draw the icons and vertices
        // once at the same translation, selected ones highlighted.
        this->offset_x = this->inputs->full_network_move_translation.x();
        this->offset_y = this->inputs->full_network_move_translation.y();
        staticDetails(true);
        this->offset_x = 0.0;
        this->offset_y = 0.0;
        return;
    }

    staticDetails(false);
    if (state.move.active)
    {
        movingNetwork();
        return;
    }
    selectedPipes();
    pipePlacement();
    deviceLinkPlacement();
    selectedMarkersAndDeviceLinks();
    simulationErrors();
    floatingPlacement();
}

// --- Projection and pens ----------------------------------------------------------

MapBgfxHudPoint MapBgfxEditorOverlay::screen(const MapBgfx2d::WorldPoint &point) const
{
    const MapBgfx2d::ScreenPoint projected = MapBgfx2d::screenFromWorld(this->inputs->camera, point);
    return MapBgfxHudPoint{projected.x + this->offset_x, projected.y + this->offset_y};
}

MapBgfxHudPoint MapBgfxEditorOverlay::screen(const CoordinateWGS84 &coordinate) const
{
    return screen(world(coordinate));
}

QVector<MapBgfxHudPoint> MapBgfxEditorOverlay::screenPolyline(
    const QVector<MapBgfx2d::WorldPoint> &points) const
{
    QVector<MapBgfxHudPoint> result;
    if (points.isEmpty())
        return result;
    const MapBgfx2d::Camera &camera = this->inputs->camera;
    // Continuous along the link, then the copy whose middle is nearest to
    // the camera.
    QVector<double> xs;
    xs.reserve(points.size());
    double previous = points.first().x;
    for (const MapBgfx2d::WorldPoint &point : points)
    {
        const double x = MapBgfx2d::wrapNearest(point.x, previous);
        xs.append(x);
        previous = x;
    }
    const double middle = 0.5 * (xs.first() + xs.last());
    const double shift = std::round(camera.center.x - middle);
    result.reserve(points.size());
    for (qsizetype index = 0; index < points.size(); ++index)
    {
        result.append(MapBgfxHudPoint{
            camera.viewport_width * 0.5 + (xs.at(index) + shift - camera.center.x)
                * camera.world_pixels + this->offset_x,
            camera.viewport_height * 0.5 + (points.at(index).y - camera.center.y)
                * camera.world_pixels + this->offset_y});
    }
    return result;
}

QVector<MapBgfxHudPoint> MapBgfxEditorOverlay::screenPolyline(
    const QList<CoordinateWGS84> &coordinates) const
{
    QVector<MapBgfx2d::WorldPoint> points;
    points.reserve(coordinates.size());
    for (const CoordinateWGS84 &coordinate : coordinates)
        points.append(world(coordinate));
    return screenPolyline(points);
}

void MapBgfxEditorOverlay::polyline(const QVector<MapBgfxHudPoint> &points, double width,
                                    quint32 argb)
{
    if (points.size() < 2)
        return;
    for (qsizetype index = 1; index < points.size(); ++index)
    {
        const MapBgfxHudPoint &a = points.at(index - 1);
        const MapBgfxHudPoint &b = points.at(index);
        this->canvas->line(a.x, a.y, b.x, b.y, width, argb);
    }
    // Round caps and joins (Qt::RoundCap / Qt::RoundJoin).
    for (const MapBgfxHudPoint &point : points)
        this->canvas->fillCircle(point.x, point.y, 0.5 * width, argb);
}

void MapBgfxEditorOverlay::dashedPolyline(const QVector<MapBgfxHudPoint> &points, double width,
                                          double dash, double gap, double offset, quint32 argb)
{
    if (points.size() < 2 || !(dash > 0.0) || !(gap >= 0.0))
        return;
    // Position inside the pattern carried over segment boundaries.
    double phase = std::max(0.0, offset);
    for (qsizetype index = 1; index < points.size(); ++index)
    {
        const MapBgfxHudPoint &a = points.at(index - 1);
        const MapBgfxHudPoint &b = points.at(index);
        dashedSegment(a, b, width, dash, gap, phase, argb);
        phase += std::hypot(b.x - a.x, b.y - a.y);
    }
}

void MapBgfxEditorOverlay::synchronizeSpriteMemo()
{
    // A cleared atlas (full, or a new device pixel ratio) invalidates every
    // memoized sprite.
    if (this->sprite_memo_valid && this->sprite_memo_generation == this->cache->generation())
        return;
    this->dash_strips.clear();
    this->discs.clear();
    this->sprite_memo_generation = this->cache->generation();
    this->sprite_memo_valid = true;
}

MapBgfxEditorOverlay::DashStrip MapBgfxEditorOverlay::dashStrip(double width, double dash,
                                                               double gap)
{
    synchronizeSpriteMemo();
    for (const DashStrip &strip : this->dash_strips)
    {
        if (strip.width == width && strip.dash == dash && strip.gap == gap)
            return strip;
    }
    DashStrip strip;
    strip.width = width;
    strip.dash = dash;
    strip.gap = gap;
    const double period = dash + gap;
    strip.length = period * std::max(1.0, std::floor(DashStripLengthPx / period));
    const QSizeF logical(strip.length, width);
    const QString key = QStringLiteral("D|%1|%2|%3").arg(width).arg(dash).arg(gap);
    strip.sprite = this->cache->cachedImage(key, logical);
    if (!strip.sprite.valid)
    {
        // White where the pattern is on (sampled at texel centres on the
        // physical grid), transparent in the gaps; tinted when drawn.
        const double ratio = this->cache->devicePixelRatio();
        const int image_width = std::max(1, int(std::lround(strip.length * ratio)));
        const int image_height = std::max(1, int(std::lround(width * ratio)));
        QImage image(image_width, image_height, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        for (int x = 0; x < image_width; ++x)
        {
            const double position = (double(x) + 0.5) * strip.length / double(image_width);
            if (std::fmod(position, period) >= dash)
                continue;
            for (int y = 0; y < image_height; ++y)
                image.setPixel(x, y, 0xffffffffu);
        }
        strip.sprite = this->cache->image(key, image, logical);
        // Storing may have cleared the atlas: start the memo over in the new
        // generation (the build is then repeated, see build()).
        synchronizeSpriteMemo();
    }
    if (strip.sprite.valid)
        this->dash_strips.append(strip);
    return strip;
}

void MapBgfxEditorOverlay::dashedSegment(const MapBgfxHudPoint &a, const MapBgfxHudPoint &b,
                                         double width, double dash, double gap, double phase,
                                         quint32 argb)
{
    const double length = std::hypot(b.x - a.x, b.y - a.y);
    if (!(length > 0.0) || !(dash > 0.0) || !(gap >= 0.0) || !(width > 0.0))
        return;
    const double period = dash + gap;
    const double ux = (b.x - a.x) / length;
    const double uy = (b.y - a.y) / length;
    double pattern = std::fmod(std::max(0.0, phase), period);
    const DashStrip strip = dashStrip(width, dash, gap);
    if (!strip.sprite.valid || !(strip.length > period))
    {
        // No atlas room: one quad per dash.
        double position = 0.0;
        while (position < length)
        {
            const bool on = pattern < dash;
            const double step = std::min(length - position, on ? dash - pattern : period - pattern);
            if (on)
            {
                this->canvas->line(a.x + ux * position, a.y + uy * position,
                                   a.x + ux * (position + step), a.y + uy * (position + step),
                                   width, argb);
            }
            position += step;
            pattern = std::fmod(pattern + step, period);
        }
        return;
    }
    // One textured quad per strip length; u runs along the line from the
    // current pattern position, v across it.
    const double nx = -uy * 0.5 * width;
    const double ny = ux * 0.5 * width;
    const double chunk_limit = strip.length - period;
    const float du = strip.sprite.u1 - strip.sprite.u0;
    double position = 0.0;
    while (position < length)
    {
        const double chunk = std::min(length - position, chunk_limit);
        const double x0 = a.x + ux * position;
        const double y0 = a.y + uy * position;
        const double x1 = a.x + ux * (position + chunk);
        const double y1 = a.y + uy * (position + chunk);
        const float u_start = strip.sprite.u0 + float(pattern / strip.length) * du;
        const float u_end = strip.sprite.u0 + float((pattern + chunk) / strip.length) * du;
        const MapBgfxHudPoint corners[4] = {
            {x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
        const float u[4] = {u_start, u_end, u_end, u_start};
        const float v[4] = {strip.sprite.v0, strip.sprite.v0, strip.sprite.v1, strip.sprite.v1};
        this->canvas->spriteQuad(corners, u, v, argb);
        position += chunk;
        pattern = std::fmod(pattern + chunk, period);
    }
}

MapBgfxEditorOverlay::Disc MapBgfxEditorOverlay::discSprite(double radius, double pen,
                                                           quint32 fill_argb, quint32 ring_argb)
{
    synchronizeSpriteMemo();
    for (const Disc &disc : this->discs)
    {
        if (disc.radius == radius && disc.pen == pen && disc.fill == fill_argb
            && disc.ring == ring_argb)
            return disc;
    }
    Disc disc;
    disc.radius = radius;
    disc.pen = pen;
    disc.fill = fill_argb;
    disc.ring = ring_argb;
    // Ring outside edge plus one pixel for antialiasing on every side.
    disc.extent = std::ceil(2.0 * (radius + 0.5 * pen) + 2.0);
    const QSizeF logical(disc.extent, disc.extent);
    const QString key = QStringLiteral("C|%1|%2|%3|%4").arg(radius).arg(pen)
        .arg(qulonglong(fill_argb)).arg(qulonglong(ring_argb));
    disc.sprite = this->cache->cachedImage(key, logical);
    if (!disc.sprite.valid)
    {
        const double ratio = this->cache->devicePixelRatio();
        const int size = std::max(1, int(std::lround(disc.extent * ratio)));
        QImage image(size, size, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setBrush(QColor::fromRgba(fill_argb));
            painter.setPen(QPen(QColor::fromRgba(ring_argb), pen * ratio, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            const double center = 0.5 * double(size);
            painter.drawEllipse(QPointF(center, center), radius * ratio, radius * ratio);
        }
        disc.sprite = this->cache->image(key, image, logical);
        synchronizeSpriteMemo();
    }
    if (disc.sprite.valid)
        this->discs.append(disc);
    return disc;
}

void MapBgfxEditorOverlay::disc(double x, double y, double radius, double pen,
                                quint32 fill_argb, quint32 ring_argb)
{
    const Disc sprite = discSprite(radius, pen, fill_argb, ring_argb);
    if (!sprite.sprite.valid)
    {
        this->canvas->fillCircle(x, y, radius, fill_argb);
        this->canvas->strokeCircle(x, y, radius + 0.5 * pen, pen, ring_argb);
        return;
    }
    // Top-left on the physical pixel grid: the point-sampled sprite then
    // maps texel to pixel 1:1 (no shimmer while panning).
    const double ratio = this->cache->devicePixelRatio();
    const double left = std::round((x - 0.5 * sprite.extent) * ratio) / ratio;
    const double top = std::round((y - 0.5 * sprite.extent) * ratio) / ratio;
    this->canvas->sprite(MapBgfxHudRect{left, top, sprite.extent, sprite.extent}, sprite.sprite,
                         0xffffffffu);
}

void MapBgfxEditorOverlay::dashedCircle(double x, double y, double radius, double width,
                                        double dash, double gap, quint32 argb)
{
    if (!(radius > 0.0))
        return;
    const int segments = std::clamp(int(std::ceil(radius * 3.0)), 24, 96);
    QVector<MapBgfxHudPoint> points;
    points.reserve(segments + 1);
    for (int index = 0; index <= segments; ++index)
    {
        const double angle = 2.0 * MapBgfxHudCanvas::Pi * double(index) / double(segments);
        points.append(MapBgfxHudPoint{x + radius * std::cos(angle), y + radius * std::sin(angle)});
    }
    dashedPolyline(points, width, dash, gap, 0.0, argb);
}

void MapBgfxEditorOverlay::strokeRectCentered(const MapBgfxHudRect &rect, double radius,
                                              double width, quint32 argb)
{
    if (!(width > 0.0))
        return;
    const double half = 0.5 * width;
    this->canvas->strokeRoundedRect(MapBgfxHudRect{rect.x - half, rect.y - half,
                                                   rect.width + width, rect.height + width},
                                    radius > 0.0 ? radius + half : 0.0, width, argb);
}

bool MapBgfxEditorOverlay::onScreen(double x, double y, double width, double height) const
{
    return x + width >= 0.0 && y + height >= 0.0 && x <= this->inputs->viewport.width()
        && y <= this->inputs->viewport.height();
}

bool MapBgfxEditorOverlay::boundsOnScreen(const Geometry::Bounds &bounds, double margin_px) const
{
    // The copy nearest to the camera, like screenPolyline().
    const MapBgfx2d::Camera &camera = this->inputs->camera;
    const double shift = std::round(camera.center.x - 0.5 * (bounds.min_x + bounds.max_x));
    const double left = camera.viewport_width * 0.5
        + (bounds.min_x + shift - camera.center.x) * camera.world_pixels + this->offset_x;
    const double right = camera.viewport_width * 0.5
        + (bounds.max_x + shift - camera.center.x) * camera.world_pixels + this->offset_x;
    const double top = camera.viewport_height * 0.5
        + (bounds.min_y - camera.center.y) * camera.world_pixels + this->offset_y;
    const double bottom = camera.viewport_height * 0.5
        + (bounds.max_y - camera.center.y) * camera.world_pixels + this->offset_y;
    return onScreen(left - margin_px, top - margin_px, right - left + 2.0 * margin_px,
                    bottom - top + 2.0 * margin_px);
}

// --- Icons -------------------------------------------------------------------------

void MapBgfxEditorOverlay::drawIcon(double x, double y, const Icon &icon)
{
    if (!icon.sprite.valid)
        return;
    this->canvas->sprite(MapBgfxHudRect{x - icon.padding, y - icon.padding,
                                        icon.width + 2.0 * icon.padding,
                                        icon.height + 2.0 * icon.padding},
                         icon.sprite, 0xffffffffu);
}

void MapBgfxEditorOverlay::bottomAnchoredIcon(const MapBgfxHudPoint &anchor,
                                              InfrastructureEntity entity, const QString &path,
                                              int width, Highlight highlight, bool round_anchor)
{
    if (!this->icon_provider || width <= 0)
        return;
    // Cull before asking for the sprite: off-screen icons cost nothing.
    const double reach = 2.0 * double(width) + 24.0;
    if (!onScreen(anchor.x - reach, anchor.y - reach, 2.0 * reach, 2.0 * reach))
        return;
    const Icon icon = this->icon_provider(this->cache, entity, path, width, highlight);
    // MapEntityPixmapRenderer::bottomAnchoredRect() at the rounded anchor.
    const double x = round_anchor ? double(qRound(anchor.x)) : anchor.x;
    const double y = round_anchor ? double(qRound(anchor.y)) : anchor.y;
    drawIcon(x, y - icon.height, icon);
}

void MapBgfxEditorOverlay::centeredIcon(const MapBgfxHudPoint &center, InfrastructureEntity entity,
                                        const QString &path, int width, Highlight highlight)
{
    if (!this->icon_provider || width <= 0)
        return;
    const double reach = 2.0 * double(width) + 24.0;
    if (!onScreen(center.x - reach, center.y - reach, 2.0 * reach, 2.0 * reach))
        return;
    const Icon icon = this->icon_provider(this->cache, entity, path, width, highlight);
    drawIcon(center.x - icon.width / 2.0, center.y - icon.height / 2.0, icon);
}

const NetworkRenderNode *MapBgfxEditorOverlay::nodeByUuid(const QUuid &uuid) const
{
    if (uuid.isNull())
        return nullptr;
    const QHash<QUuid, int>::const_iterator found = this->geometry.node_index.constFind(uuid);
    if (found == this->geometry.node_index.cend())
        return nullptr;
    return &this->inputs->snapshot->nodes.at(found.value());
}

bool MapBgfxEditorOverlay::deviceLinkScreen(int link_index, MapBgfxHudPoint *start,
                                            MapBgfxHudPoint *center, MapBgfxHudPoint *end) const
{
    const QVector<MapBgfx2d::WorldPoint> &points = this->geometry.link_world.at(link_index);
    if (points.size() < 2)
        return false;
    // Start, centre, end as one continuous polyline.
    QVector<MapBgfx2d::WorldPoint> three;
    three.append(points.first());
    three.append(this->geometry.device_center_world.at(link_index));
    three.append(points.last());
    const QVector<MapBgfxHudPoint> projected = screenPolyline(three);
    *start = projected.at(0);
    *center = projected.at(1);
    *end = projected.at(2);
    return true;
}

// --- Selection frames --------------------------------------------------------------

void MapBgfxEditorOverlay::tileSelection()
{
    // The former QPainter editor's paintTileSelection(): the tile-cache range at its
    // integer zoom, placed by the continuous camera.
    const MapEditorViewportRenderState &viewport = *this->inputs->viewport_state;
    if (!viewport.tile_selection_visible || viewport.tile_x_max < viewport.tile_x_min
        || viewport.tile_y_max < viewport.tile_y_min || viewport.tile_selection_zoom < 0)
        return;
    const MapBgfx2d::Camera &camera = this->inputs->camera;
    const double tiles = std::exp2(double(viewport.tile_selection_zoom));
    double west = double(viewport.tile_x_min) / tiles;
    double east = double(viewport.tile_x_max + 1) / tiles;
    const double north = double(viewport.tile_y_min) / tiles;
    const double south = double(viewport.tile_y_max + 1) / tiles;
    // Nearest world copy to the camera (whole range shifted together).
    const double shift = std::round(camera.center.x - 0.5 * (west + east));
    west += shift;
    east += shift;
    const std::function<double(double)> screen_x = [&camera](double world_x)
    {
        return camera.viewport_width * 0.5 + (world_x - camera.center.x) * camera.world_pixels;
    };
    const std::function<double(double)> screen_y = [&camera](double world_y)
    {
        return camera.viewport_height * 0.5 + (world_y - camera.center.y) * camera.world_pixels;
    };
    const MapBgfxHudRect rect{screen_x(west), screen_y(north), screen_x(east) - screen_x(west),
                              screen_y(south) - screen_y(north)};
    if (!(rect.width > 0.0) || !(rect.height > 0.0)
        || !onScreen(rect.x, rect.y, rect.width, rect.height))
        return;

    // Diagonal fill gradient (top-left -> bottom-right), corner colours from
    // the gradient's projection.
    const TileSelectionPalette palette = tileSelectionPalette(viewport.tile_selection_kind);
    const double length_squared = rect.width * rect.width + rect.height * rect.height;
    const std::function<quint32(double, double)> gradient_at = [&](double dx, double dy)
    {
        return tileSelectionFillAt(palette, (dx * rect.width + dy * rect.height) / length_squared);
    };
    QVector<MapBgfxHudPoint> corners = {
        {rect.x, rect.y}, {rect.right(), rect.y}, {rect.right(), rect.bottom()}, {rect.x, rect.bottom()}};
    QVector<quint32> colors = {gradient_at(0.0, 0.0), gradient_at(rect.width, 0.0),
                               gradient_at(rect.width, rect.height), gradient_at(0.0, rect.height)};
    this->canvas->fillConvexPolygonArgb(corners, colors);

    strokeRectCentered(rect, 0.0, 12.0, palette.glow_wide);
    strokeRectCentered(rect, 0.0, 5.0, palette.glow_inner);
    strokeRectCentered(rect, 0.0, 1.5, palette.line);

    // Tile grid inside the range, only where visible.
    const quint32 grid = palette.grid;
    const double viewport_west = camera.center.x - camera.viewport_width * 0.5 / camera.world_pixels;
    const double viewport_east = camera.center.x + camera.viewport_width * 0.5 / camera.world_pixels;
    const int first_x = std::max(viewport.tile_x_min + 1,
                                 int(std::ceil((viewport_west - shift) * tiles)));
    const int last_x = std::min(viewport.tile_x_max, int(std::floor((viewport_east - shift) * tiles)));
    for (int tile_x = first_x; tile_x <= last_x; ++tile_x)
    {
        const double x = screen_x(double(tile_x) / tiles + shift);
        this->canvas->line(x, rect.y, x, rect.bottom(), 1.0, grid);
    }
    const double viewport_north = camera.center.y - camera.viewport_height * 0.5 / camera.world_pixels;
    const double viewport_south = camera.center.y + camera.viewport_height * 0.5 / camera.world_pixels;
    const int first_y = std::max(viewport.tile_y_min + 1, int(std::ceil(viewport_north * tiles)));
    const int last_y = std::min(viewport.tile_y_max, int(std::floor(viewport_south * tiles)));
    for (int tile_y = first_y; tile_y <= last_y; ++tile_y)
    {
        const double y = screen_y(double(tile_y) / tiles);
        this->canvas->line(rect.x, y, rect.right(), y, 1.0, grid);
    }
}

std::optional<InfrastructureEntityReference> MapBgfxEditorOverlay::hitTestGlobeIcon(
    const QPointF &position) const
{
    if (!std::isfinite(position.x()) || !std::isfinite(position.y()))
        return std::nullopt;
    // The last drawn icon wins, including when multiple entities overlap.
    for (qsizetype index = this->globe_icon_hit_areas.size(); index > 0; --index)
    {
        const GlobeIconHitArea &area = this->globe_icon_hit_areas.at(index - 1);
        if (area.bounds.contains(position))
            return area.entity;
    }
    return std::nullopt;
}

void MapBgfxEditorOverlay::buildGlobe(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *cache,
                                      const MapEditorViewportRenderState &viewport_state,
                                      const GlobeProjector &projector,
                                      const NetworkRenderSnapshot *snapshot, int icon_width,
                                      const QSizeF &viewport_size,
                                      const MapBgfxGlobeIconIndex::Frustum &frustum,
                                      const MapEditorVisualState *editor_state,
                                      const MapCanvasProjection *editor_projection,
                                      const GlobeElevatedProjector &elevated_projector)
{
    canvas->clear();
    this->globe_icon_hit_areas.clear();
    if (!projector || (!viewport_state.tile_selection_visible
                       && !viewport_state.rectangle_selection_visible
                       && (snapshot == nullptr || icon_width <= 0)))
        return;

    // The common icon and pen helpers use the viewport from Inputs. Keep
    // this frame-local: a render callback may capture a short-lived globe
    // camera projector and must NEVER escape this build.
    Inputs globe_inputs;
    globe_inputs.viewport = viewport_size;
    globe_inputs.snapshot = snapshot;
    globe_inputs.visual_state = editor_state;
    globe_inputs.viewport_state = &viewport_state;
    this->inputs = &globe_inputs;
    this->canvas = canvas;
    this->cache = cache;
    cache->beginFrame();
    if (snapshot != nullptr && icon_width > 0 && this->icon_provider)
        synchronizeGlobeIcons(*snapshot);
    const std::function<void()> build = [this, &viewport_state, &projector, snapshot, icon_width,
                                         &viewport_size, &frustum, editor_state,
                                         editor_projection, &elevated_projector]()
    {
        // The atlas can be reset and this frame rebuilt: never retain hit
        // areas from sprites discarded by the rebuild.
        this->globe_icon_hit_areas.clear();
        this->canvas->setWhiteTexel(this->cache->whiteU(), this->cache->whiteV());
        if (viewport_state.tile_selection_visible)
            globeTileSelection(viewport_state, projector);
        // Same screen-space blue selection frame as the 2D Editor. The
        // frame itself must not be terrain-occluded or require DEM data.
        if (viewport_state.rectangle_selection_visible)
            rectangleSelection();
        if (snapshot != nullptr && icon_width > 0 && this->icon_provider)
            globeNetworkIcons(*snapshot, projector, icon_width, viewport_size,
                              frustum, editor_state, elevated_projector);
        if (snapshot != nullptr && editor_state != nullptr && editor_projection != nullptr
            && !viewport_size.isEmpty())
            globeEditorVisuals(*snapshot, *editor_state, *editor_projection,
                               elevated_projector, viewport_size, icon_width);
    };
    build();
    // The atlas may evict while the first icon sizes are created. As with
    // build(), no draw list may refer to regions of a discarded generation.
    if (cache->evictedSinceFrameStart())
    {
        cache->beginFrame();
        canvas->clear();
        build();
    }
    this->canvas = nullptr;
    this->cache = nullptr;
    this->inputs = nullptr;
}

// Rendering-only bridge from the 2D editor's MapEditorVisualState to the
// drawn globe. There is no 3D placement tool, drag state or model mutation
// here: the 2D editor owns all of those when they are enabled in 3D later.
void MapBgfxEditorOverlay::globeEditorVisuals(
    const NetworkRenderSnapshot &snapshot, const MapEditorVisualState &state,
    const MapCanvasProjection &projection, const GlobeElevatedProjector &elevated_projector,
    const QSizeF &viewport_size, int icon_width)
{
    const QSize viewport(qMax(1, qRound(viewport_size.width())),
                         qMax(1, qRound(viewport_size.height())));
    const std::function<std::optional<MapBgfxHudPoint>(const CoordinateWGS84 &)> point =
        [&projection, &viewport, &state](const CoordinateWGS84 &coordinate)
        -> std::optional<MapBgfxHudPoint>
    {
        const std::optional<QPointF> projected = projection.screenFromCoordinate(
            coordinate, viewport, state.wrap_reference_longitude);
        if (!projected.has_value())
            return std::nullopt;
        return MapBgfxHudPoint{projected->x(), projected->y()};
    };
    const std::function<std::optional<MapBgfxHudPoint>(
        const CoordinateWGS84 &, double)> elevated_point =
        [&point, &elevated_projector](const CoordinateWGS84 &coordinate, double elevation_m)
        -> std::optional<MapBgfxHudPoint>
    {
        if (!elevated_projector || !std::isfinite(elevation_m))
            return point(coordinate); // tests/older callers without a height projector
        QPointF projected;
        if (!elevated_projector(coordinate.longitude_deg, coordinate.latitude_deg,
                                elevation_m, &projected, nullptr))
            return std::nullopt;
        return MapBgfxHudPoint{projected.x(), projected.y()};
    };
    const std::function<const NetworkRenderNode *(const QUuid &)> node =
        [&snapshot](const QUuid &uuid) -> const NetworkRenderNode *
    {
        for (const NetworkRenderNode &candidate : snapshot.nodes)
        {
            if (candidate.uuid == uuid)
                return &candidate;
        }
        return nullptr;
    };
    const std::function<std::optional<MapBgfxHudPoint>(const NetworkRenderNode &)> node_position =
        [&point, &elevated_point](const NetworkRenderNode &found)
        -> std::optional<MapBgfxHudPoint>
    {
        // Reservoir head is a hydraulic boundary value, not a physical
        // location. Demand points do not have an elevation in the Model.
        if (found.entity_type == InfrastructureEntity::Reservoir
            || found.entity_type == InfrastructureEntity::DemandPoint)
            return point(found.coordinate_wgs84);
        return elevated_point(found.coordinate_wgs84, found.elevation_m);
    };
    const std::function<std::optional<MapBgfxHudPoint>(const QUuid &)> node_point =
        [&node, &node_position](const QUuid &uuid) -> std::optional<MapBgfxHudPoint>
    {
        const NetworkRenderNode *found = node(uuid);
        return found != nullptr ? node_position(*found) : std::nullopt;
    };
    const std::function<QVector<MapBgfxHudPoint>(const QList<CoordinateWGS84> &)> line =
        [&point](const QList<CoordinateWGS84> &coordinates)
    {
        QVector<MapBgfxHudPoint> result;
        result.reserve(coordinates.size());
        for (const CoordinateWGS84 &coordinate : coordinates)
        {
            const std::optional<MapBgfxHudPoint> projected = point(coordinate);
            if (!projected.has_value())
                return QVector<MapBgfxHudPoint>(); // never connect across the horizon
            result.append(projected.value());
        }
        return result;
    };

    // Project existing pipe endpoints at hydraulic height, pipe bends on
    // the drawn terrain. Device links use their rendered network elevations.
    const std::function<QVector<MapBgfxHudPoint>(const NetworkRenderLink &)> network_line =
        [&point, &elevated_point](const NetworkRenderLink &link)
    {
        QVector<MapBgfxHudPoint> result;
        result.reserve(link.vertices_wgs84.size());
        for (qsizetype index = 0; index < link.vertices_wgs84.size(); ++index)
        {
            const CoordinateWGS84 &coordinate = link.vertices_wgs84.at(index);
            const bool bend = link.entity_type == InfrastructureEntity::Pipe
                && index > 0 && index + 1 < link.vertices_wgs84.size();
            const std::optional<MapBgfxHudPoint> projected =
                !bend && index < link.elevations_m.size()
                    ? elevated_point(coordinate, link.elevations_m.at(index))
                    : point(coordinate);
            if (!projected.has_value())
                return QVector<MapBgfxHudPoint>();
            result.append(projected.value());
        }
        return result;
    };

    // The 2D editor shows black bend dots for ALL pipes, not only selected
    // ones. Reuse the same visual-state selection colours on the globe.
    // Moving links are rendered by the shared dynamic preview below.
    for (const NetworkRenderLink &link : snapshot.links)
    {
        if (link.entity_type != InfrastructureEntity::Pipe
            || link.vertices_wgs84.size() < 3
            || state.selected_pipe_uuids.contains(link.uuid))
            continue;
        bool moving = false;
        if (state.move.active)
        {
            for (const MapEditorDynamicLinkVisualState &dynamic_link : state.move.links)
            {
                if (dynamic_link.uuid == link.uuid)
                {
                    moving = true;
                    break;
                }
            }
        }
        if (moving)
            continue;
        for (qsizetype index = 1; index + 1 < link.vertices_wgs84.size(); ++index)
        {
            const std::optional<MapBgfxHudPoint> bend = point(link.vertices_wgs84.at(index));
            if (bend.has_value() && onScreen(bend->x - PipeVertexRadiusPx,
                                              bend->y - PipeVertexRadiusPx,
                                              2.0 * PipeVertexRadiusPx,
                                              2.0 * PipeVertexRadiusPx))
                this->canvas->fillCircle(bend->x, bend->y, PipeVertexRadiusPx, BlackColor);
        }
    }

    // The same blue selected-link stroke and pipe-vertex dots as the flat view.
    for (const QUuid &uuid : state.selected_pipe_uuids)
    {
        bool moving = false;
        if (state.move.active)
        {
            for (const MapEditorDynamicLinkVisualState &link : state.move.links)
            {
                if (link.uuid == uuid)
                {
                    moving = true;
                    break;
                }
            }
        }
        if (moving)
            continue;
        for (const NetworkRenderLink &link : snapshot.links)
        {
            if (link.uuid != uuid || link.entity_type != InfrastructureEntity::Pipe)
                continue;
            const QVector<MapBgfxHudPoint> vertices = network_line(link);
            polyline(vertices, LineWidthPx, SelectedColor);
            for (qsizetype index = 1; index + 1 < vertices.size(); ++index)
                this->canvas->fillCircle(vertices.at(index).x, vertices.at(index).y,
                                         PipeVertexRadiusPx, SelectedColor);
            break;
        }
    }

    // Selected entities keep the identical icon assets and highlight style.
    for (const QUuid &uuid : state.selected_marker_uuids)
    {
        bool moving = false;
        if (state.move.active)
        {
            for (const MapEditorDynamicMarkerVisualState &marker : state.move.markers)
            {
                if (marker.uuid == uuid)
                {
                    moving = true;
                    break;
                }
            }
        }
        if (moving)
            continue; // the shared dynamic preview draws it at the new position
        const NetworkRenderNode *selected = node(uuid);
        if (selected != nullptr)
        {
            const std::optional<MapBgfxHudPoint> center = node_position(*selected);
            if (center.has_value() && isPointEntity(selected->entity_type))
                bottomAnchoredIcon(center.value(), selected->entity_type, QString(),
                                   icon_width, Highlight::Selected, true);
            continue;
        }
        for (const NetworkRenderLink &link : snapshot.links)
        {
            if (link.uuid != uuid || !isDeviceLink(link.entity_type)
                || link.vertices_wgs84.size() < 2)
                continue;
            const QVector<MapBgfxHudPoint> vertices = network_line(link);
            if (vertices.size() >= 2)
            {
                // Use the same geographic anchor as the ordinary (unselected)
                // device icon, so highlighting does not jump on a tilted globe.
                const std::optional<MapBgfxHudPoint> center_position =
                    elevated_point(deviceLinkCenter(link.vertices_wgs84),
                                   deviceLinkElevationM(link));
                if (!center_position.has_value())
                    continue;
                const MapBgfxHudPoint center = center_position.value();
                polyline(QVector<MapBgfxHudPoint>{vertices.first(), center, vertices.last()},
                         LineWidthPx, SelectedColor);
                centeredIcon(center, link.entity_type, QString(), icon_width,
                             Highlight::Selected);
            }
            break;
        }
    }

    // The SAME dynamic move preview that the 2D editor renders: a proposed
    // marker position and its connected links, before the hydraulic model is
    // committed. Globe input does not own a second movement state.
    if (state.move.active)
    {
        for (const MapEditorDynamicLinkVisualState &link : state.move.links)
        {
            const NetworkRenderLink *original = nullptr;
            for (const NetworkRenderLink &candidate : snapshot.links)
            {
                if (candidate.uuid == link.uuid)
                {
                    original = &candidate;
                    break;
                }
            }
            NetworkRenderLink moved_link;
            moved_link.entity_type = link.entity;
            moved_link.vertices_wgs84 = link.vertices_wgs84;
            if (original != nullptr)
                moved_link.elevations_m = original->elevations_m;
            const QVector<MapBgfxHudPoint> vertices = network_line(moved_link);
            if (vertices.size() != link.vertices_wgs84.size() || vertices.size() < 2)
                continue;
            if (link.entity == InfrastructureEntity::Pipe)
            {
                const quint32 color = state.selected_pipe_uuids.contains(link.uuid)
                    ? SelectedColor : BlackColor;
                polyline(vertices, LineWidthPx, color);
                for (qsizetype index = 1; index + 1 < vertices.size(); ++index)
                    this->canvas->fillCircle(vertices.at(index).x, vertices.at(index).y,
                                             PipeVertexRadiusPx, color);
            }
            else if (isDeviceLink(link.entity) && vertices.size() >= 3)
            {
                const bool selected = state.selected_marker_uuids.contains(link.uuid);
                polyline(vertices, LineWidthPx, selected ? SelectedColor : DeviceLinkColor);
                centeredIcon(vertices.at(1), link.entity, QString(), icon_width,
                             selected ? Highlight::Selected : Highlight::None);
            }
        }
        for (const MapEditorDynamicMarkerVisualState &marker : state.move.markers)
        {
            const NetworkRenderNode *original = node(marker.uuid);
            const std::optional<MapBgfxHudPoint> position =
                original != nullptr && original->entity_type != InfrastructureEntity::Reservoir
                    && original->entity_type != InfrastructureEntity::DemandPoint
                    ? elevated_point(marker.coordinate_wgs84, original->elevation_m)
                    : point(marker.coordinate_wgs84);
            if (!position.has_value() || !isPointEntity(marker.entity))
                continue;
            this->canvas->fillCircle(position->x, position->y, MarkerDotRadiusPx, BlackColor);
            bottomAnchoredIcon(position.value(), marker.entity, marker.pixmap_path,
                               icon_width,
                               state.selected_marker_uuids.contains(marker.uuid)
                                   ? Highlight::Selected : Highlight::None, false);
        }
    }

    // Demand-point attachment guides are the same shared visual state used
    // by the flat editor. In globe mode only their coordinate projection
    // differs; editing, attaching, and reattaching remain in MapCanvasEntities.
    const quint32 emphasized_color = rgba(0, 120, 190, 255);
    for (const MapEditorDemandPointAttachmentVisualState &attachment :
         state.demand_point_attachments)
    {
        if (!attachment.visible)
            continue;
        const std::optional<MapBgfxHudPoint> demand =
            point(attachment.demand_point_coordinate_wgs84);
        if (!demand.has_value())
            continue;
        if (attachment.selecting_target
            && onScreen(demand->x - 12.0, demand->y - 12.0, 24.0, 24.0))
        {
            dashedCircle(demand->x, demand->y, 10.0, 1.0, 6.0, 3.0,
                         emphasized_color);
        }
        if (!attachment.attachment_coordinate_valid)
            continue;
        const std::optional<MapBgfxHudPoint> target =
            point(attachment.attachment_coordinate_wgs84);
        if (!target.has_value())
            continue; // never draw an attachment across the horizon
        const bool emphasized = attachment.selected || attachment.selecting_target
                                || attachment.moving;
        const quint32 color = emphasized ? emphasized_color : rgba(70, 105, 125, 190);
        const double width = emphasized ? 1.5 : 1.0;
        const double min_x = std::min(demand->x, target->x);
        const double min_y = std::min(demand->y, target->y);
        if (!onScreen(min_x - 16.0, min_y - 16.0,
                      std::fabs(target->x - demand->x) + 32.0,
                      std::fabs(target->y - demand->y) + 32.0))
            continue;
        double start_fraction = 0.0;
        double end_fraction = 1.0;
        const double length = std::hypot(target->x - demand->x,
                                         target->y - demand->y);
        if (length > 0.0 && clipSegment(
                demand.value(), target.value(), -16.0, -16.0,
                viewport_size.width() + 16.0, viewport_size.height() + 16.0,
                &start_fraction, &end_fraction))
        {
            dashedSegment(MapBgfxHudPoint{
                              demand->x + (target->x - demand->x) * start_fraction,
                              demand->y + (target->y - demand->y) * start_fraction},
                          MapBgfxHudPoint{
                              demand->x + (target->x - demand->x) * end_fraction,
                              demand->y + (target->y - demand->y) * end_fraction},
                          width, 4.0 * width, 2.0 * width,
                          start_fraction * length, color);
        }
        const double radius = attachment.moving ? 7.0 : attachment.selected ? 6.0 : 4.5;
        const double pen = emphasized ? 2.0 : 1.5;
        if ((emphasized || length >= MinimumAttachmentHandleLinePx)
            && onScreen(target->x - radius - pen, target->y - radius - pen,
                        2.0 * (radius + pen), 2.0 * (radius + pen)))
        {
            disc(target->x, target->y, radius, pen,
                 rgba(255, 255, 255, emphasized ? 230 : 190), color);
        }
    }

    const MapEditorPlacementVisualState &placement = state.placement;
    if (!placement.creating)
        return;

    const NetworkRenderNode *target = node(placement.connection_target_uuid);
    const std::optional<MapBgfxHudPoint> target_point =
        target != nullptr ? node_position(*target) : std::nullopt;
    // The shared input state supplies one geographic pointer coordinate;
    // do not interpret a stale 2D screen point as a globe position.
    const std::optional<MapBgfxHudPoint> pointer =
        placement.mouse_coordinate_wgs84_valid
            ? point(placement.mouse_coordinate_wgs84)
            : std::nullopt;
    if (!target_point.has_value() && !pointer.has_value())
        return;
    const MapBgfxHudPoint end = target_point.has_value() ? target_point.value() : pointer.value();

    if (placement.entity == InfrastructureEntity::Pipe
        && !placement.pipe_start_node_uuid.isNull())
    {
        const std::optional<MapBgfxHudPoint> start = node_point(placement.pipe_start_node_uuid);
        if (start.has_value())
        {
            QVector<MapBgfxHudPoint> vertices;
            vertices.append(start.value());
            const QVector<MapBgfxHudPoint> bends = line(placement.pipe_intermediate_vertices);
            if (bends.size() == placement.pipe_intermediate_vertices.size())
            {
                for (const MapBgfxHudPoint &bend : bends)
                {
                    vertices.append(bend);
                    this->canvas->fillCircle(bend.x, bend.y, PipeVertexRadiusPx, PreviewColor);
                }
                vertices.append(end);
                polyline(vertices, LineWidthPx, PreviewColor);
            }
        }
    }
    else if (isDeviceLink(placement.entity)
             && !placement.device_link_start_node_uuid.isNull())
    {
        const std::optional<MapBgfxHudPoint> start =
            node_point(placement.device_link_start_node_uuid);
        if (start.has_value())
        {
            const MapBgfxHudPoint center = midpoint(start.value(), end);
            polyline(QVector<MapBgfxHudPoint>{start.value(), center, end},
                     LineWidthPx, PreviewColor);
            if (placement.floating_marker_visible)
                centeredIcon(center, placement.entity, QString(),
                             placement.floating_width, Highlight::None);
        }
    }
    else if (placement.floating_marker_visible
             && placement.entity != InfrastructureEntity::Unknown)
    {
        bottomAnchoredIcon(end, placement.entity, QString(),
                           placement.floating_width, Highlight::None, false);
    }

    if (target_point.has_value() && target != nullptr)
    {
        this->canvas->fillCircle(end.x, end.y, ConnectionTargetRadiusPx, PreviewColor);
        bottomAnchoredIcon(end, target->entity_type, QString(), icon_width,
                           Highlight::None, true);
    }
}

void MapBgfxEditorOverlay::synchronizeGlobeIcons(const NetworkRenderSnapshot &snapshot)
{
    if (this->globe_icons_snapshot == &snapshot
        && this->globe_icons_revision == snapshot.geometry_revision
        && this->globe_icons_node_count == snapshot.nodes.size()
        && this->globe_icons_link_count == snapshot.links.size())
        return;

    // Store only drawable icon anchors. The ECEF kd-tree is geometry state,
    // not a screen-space cache; its lifetime is independent of camera motion,
    // 2D/3D switching, terrain arrivals, and HUD icon-size changes.
    QVector<GlobeIconAnchor> anchors;
    std::vector<MapBgfxGlobeIconIndex::Position> positions;
    anchors.reserve(snapshot.nodes.size() + snapshot.links.size());
    positions.reserve(std::size_t(snapshot.nodes.size() + snapshot.links.size()));
    const std::function<void(const CoordinateWGS84 &, const QUuid &,
                             InfrastructureEntity, bool, double)> append =
        [&anchors, &positions](const CoordinateWGS84 &coordinate, const QUuid &uuid,
                               InfrastructureEntity entity, bool bottom, double elevation_m)
    {
        if (!finite(coordinate) || coordinate.latitude_deg < -90.0
            || coordinate.latitude_deg > 90.0)
            return;
        const GeoWgs84Ellipsoid::EcefPositionD ecef =
            GeoWgs84Ellipsoid::geodeticToEcefD(coordinate.longitude_deg,
                                                coordinate.latitude_deg, 0.0);
        anchors.append(GlobeIconAnchor{coordinate, uuid, entity, bottom, elevation_m});
        positions.push_back({ecef.x, ecef.y, ecef.z});
    };
    for (const NetworkRenderNode &node : snapshot.nodes)
    {
        if (node.entity_type != InfrastructureEntity::Junction
            && isPointEntity(node.entity_type))
            append(node.coordinate_wgs84, node.uuid, node.entity_type, true,
                   node.entity_type == InfrastructureEntity::Tank ? node.elevation_m
                       : std::numeric_limits<double>::quiet_NaN());
    }
    for (const NetworkRenderLink &link : snapshot.links)
    {
        if (isDeviceLink(link.entity_type) && link.vertices_wgs84.size() >= 2)
        {
            append(deviceLinkCenter(link.vertices_wgs84), link.uuid,
                   link.entity_type, false, deviceLinkElevationM(link));
        }
    }
    this->globe_icon_index.rebuild(positions);
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::GlobeEditorIconIndexBuild);
    this->globe_icon_anchors = std::move(anchors);
    this->globe_icons_revision = snapshot.geometry_revision;
    this->globe_icons_node_count = snapshot.nodes.size();
    this->globe_icons_link_count = snapshot.links.size();
    this->globe_icons_snapshot = &snapshot;
}

void MapBgfxEditorOverlay::globeNetworkIcons(const NetworkRenderSnapshot &snapshot,
                                             const GlobeProjector &projector, int icon_width,
                                             const QSizeF &viewport_size,
                                             const MapBgfxGlobeIconIndex::Frustum &frustum,
                                             const MapEditorVisualState *editor_state,
                                             const GlobeElevatedProjector &elevated_projector)
{
    // The camera frustum prunes kd-tree branches *before* the terrain-aware
    // projector would query an expensive drawn-surface height. A worst-case
    // elevation bound prevents a visible relief-draped sprite from being
    // culled merely because its sea-level anchor is outside the viewport.
    // The final projection and actual pixel cull remain authoritative.
    MapRenderDiagnostics::Scope icon_frame_scope(
        MapRenderDiagnostics::Counter::GlobeEditorIconFrame);
    (void)snapshot;
    struct VisibleIcon
    {
        QPointF point;
        InfrastructureEntity entity = InfrastructureEntity::Unknown;
        double distance_m = 0.0;
        bool bottom_anchored = false;
        std::size_t source_index = 0;
    };
    std::vector<std::size_t> candidates;
    {
        MapRenderDiagnostics::Scope index_query_scope(
            MapRenderDiagnostics::Counter::GlobeEditorIconIndexQuery);
        this->globe_icon_index.queryUnordered(frustum, &candidates);
    }
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::GlobeEditorIconAnchors,
                              quint64(this->globe_icon_anchors.size()));
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::GlobeEditorIconCandidates,
                              quint64(candidates.size()));
    QVector<VisibleIcon> icons;
    icons.reserve(qsizetype(candidates.size()));
    const double margin = 2.0 * double(icon_width) + 24.0;
    const std::function<bool(const QPointF &)> visible =
        [&viewport_size, margin](const QPointF &point)
    {
        return point.x() >= -margin && point.y() >= -margin
            && point.x() <= viewport_size.width() + margin
            && point.y() <= viewport_size.height() + margin;
    };
    {
        MapRenderDiagnostics::Scope projection_scope(
            MapRenderDiagnostics::Counter::GlobeEditorIconProjection);
        for (std::size_t index : candidates)
        {
            const GlobeIconAnchor &anchor = this->globe_icon_anchors.at(qsizetype(index));
            bool moving = false;
            if (editor_state != nullptr && editor_state->move.active)
            {
                for (const MapEditorDynamicMarkerVisualState &marker : editor_state->move.markers)
                {
                    if (marker.uuid == anchor.uuid)
                    {
                        moving = true;
                        break;
                    }
                }
            }
            if (moving)
                continue; // prevent a duplicate icon at its previous position
            QPointF point;
            double distance_m = 0.0;
            const bool projected = std::isfinite(anchor.elevation_m) && elevated_projector
                ? elevated_projector(anchor.coordinate.longitude_deg,
                                     anchor.coordinate.latitude_deg,
                                     anchor.elevation_m, &point, &distance_m)
                : projector(anchor.coordinate.longitude_deg, anchor.coordinate.latitude_deg,
                            &point, &distance_m);
            if (!projected || !visible(point))
                continue;
            icons.append({point, anchor.entity, distance_m, anchor.bottom_anchored, index});
        }
    }
    MapRenderDiagnostics::add(MapRenderDiagnostics::Counter::GlobeEditorIconVisible,
                              quint64(icons.size()));
    MapRenderDiagnostics::Scope drawing_scope(
        MapRenderDiagnostics::Counter::GlobeEditorIconDraw);
    // Sorting by distance with original snapshot index as a tie breaker
    // reproduces the previous stable-sort painter order even though the
    // spatial query now returns kd-tree traversal order.
    std::sort(icons.begin(), icons.end(), [](const VisibleIcon &a, const VisibleIcon &b)
    {
        return a.distance_m != b.distance_m ? a.distance_m > b.distance_m
                                             : a.source_index < b.source_index;
    });
    for (const VisibleIcon &entry : icons)
    {
        const Icon icon = this->icon_provider(this->cache, entry.entity,
                                              QString(), icon_width, Highlight::None);
        if (!(icon.width > 0.0) || !(icon.height > 0.0))
            continue;
        // Use the exact sprite anchor and actual pixmap dimensions, not
        // an arbitrary GPU picking radius. Exclude the decorative glow.
        const double x = entry.bottom_anchored ? double(qRound(entry.point.x()))
                                               : entry.point.x() - icon.width / 2.0;
        const double y = entry.bottom_anchored ? double(qRound(entry.point.y())) - icon.height
                                               : entry.point.y() - icon.height / 2.0;
        drawIcon(x, y, icon);
        const GlobeIconAnchor &anchor = this->globe_icon_anchors.at(
            qsizetype(entry.source_index));
        if (!anchor.uuid.isNull())
            this->globe_icon_hit_areas.append(
                GlobeIconHitArea{InfrastructureEntityReference{anchor.entity, anchor.uuid},
                                 QRectF(x, y, icon.width, icon.height)});
    }
}

void MapBgfxEditorOverlay::globeTileSelection(const MapEditorViewportRenderState &viewport,
                                              const GlobeProjector &projector)
{
    // The 2D tileSelection() on the globe: fill, glowing frame and tile grid
    // of the same tile range, laid on the drawn surface. Tile borders are
    // straight in Web Mercator, so a lattice regular in tile units (per axis
    // a whole number of cells per tile while the grid is drawn) puts every
    // tile border on lattice lines; each lattice point is projected once and
    // serves the fill, the frame and the grid.
    if (!viewport.tile_selection_visible || viewport.tile_x_max < viewport.tile_x_min
        || viewport.tile_y_max < viewport.tile_y_min || viewport.tile_selection_zoom < 0
        || viewport.tile_selection_zoom > 30)
        return;
    const qint64 tile_count = qint64(1) << viewport.tile_selection_zoom;
    const double tiles = double(tile_count);
    const qint64 width_tiles = std::min<qint64>(
        qint64(viewport.tile_x_max) - viewport.tile_x_min + 1, tile_count);
    const qint64 height_tiles = qint64(viewport.tile_y_max) - viewport.tile_y_min + 1;
    const double x_first = double(viewport.tile_x_min);
    const double y_first = double(viewport.tile_y_min);

    // Cells per tile while the tile grid fits the lattice, else a plain
    // lattice without grid lines on that axis (more tile borders than
    // cells: a solid wash at that zoom anyway).
    const std::function<int(qint64)> cells_per_tile = [](qint64 span_tiles) -> int
    {
        if (span_tiles > GlobeSelectionMaximumCells)
            return 0;
        return std::clamp(int(GlobeSelectionMaximumCells / span_tiles), 1,
                          GlobeSelectionMaximumCellsPerTile);
    };
    const int per_tile_x = cells_per_tile(width_tiles);
    const int per_tile_y = cells_per_tile(height_tiles);
    const int columns = per_tile_x > 0 ? int(width_tiles) * per_tile_x : GlobeSelectionMaximumCells;
    const int rows = per_tile_y > 0 ? int(height_tiles) * per_tile_y : GlobeSelectionMaximumCells;
    const double column_step = double(width_tiles) / double(columns);
    const double row_step = double(height_tiles) / double(rows);

    // Projected lattice, row-major; valid = in front of the camera and on
    // the visible side of the globe.
    const int lattice_width = columns + 1;
    QVector<MapBgfxHudPoint> lattice(qsizetype(lattice_width) * (rows + 1));
    QVector<bool> lattice_valid(lattice.size(), false);
    for (int row = 0; row <= rows; ++row)
    {
        const double tile_y = y_first + double(row) * row_step;
        const double mercator_y = MapBgfxHudCanvas::Pi * (1.0 - 2.0 * tile_y / tiles);
        const double latitude_deg = std::atan(std::sinh(mercator_y)) * 180.0 / MapBgfxHudCanvas::Pi;
        for (int column = 0; column <= columns; ++column)
        {
            const double tile_x = x_first + double(column) * column_step;
            const double longitude_deg = normalizeLongitude(tile_x / tiles * 360.0 - 180.0);
            QPointF projected;
            const qsizetype index = qsizetype(row) * lattice_width + column;
            if (projector(longitude_deg, latitude_deg, &projected, nullptr))
            {
                lattice[index] = MapBgfxHudPoint{projected.x(), projected.y()};
                lattice_valid[index] = true;
            }
        }
    }
    const std::function<qsizetype(int, int)> at = [lattice_width](int row, int column)
    {
        return qsizetype(row) * lattice_width + column;
    };

    // Fill: one quad per cell whose corners are all visible, coloured by the
    // diagonal gradient in tile space.
    const TileSelectionPalette palette = tileSelectionPalette(viewport.tile_selection_kind);
    for (int row = 0; row < rows; ++row)
    {
        for (int column = 0; column < columns; ++column)
        {
            const qsizetype corner_indices[4] = {at(row, column), at(row, column + 1),
                                                 at(row + 1, column + 1), at(row + 1, column)};
            bool visible = true;
            QVector<MapBgfxHudPoint> corners;
            QVector<quint32> colors;
            corners.reserve(4);
            colors.reserve(4);
            for (int corner = 0; corner < 4 && visible; ++corner)
            {
                const qsizetype index = corner_indices[corner];
                visible = lattice_valid.at(index);
                corners.append(lattice.at(index));
                const int corner_row = int(index / lattice_width);
                const int corner_column = int(index % lattice_width);
                colors.append(tileSelectionFillAt(
                    palette, 0.5 * (double(corner_column) / double(columns)
                                    + double(corner_row) / double(rows))));
            }
            if (visible)
                this->canvas->fillConvexPolygonArgb(corners, colors);
        }
    }

    // A lattice line as runs of visible points (a run breaks where the line
    // passes behind the horizon or the camera).
    const std::function<void(const QVector<qsizetype> &, double, quint32)> stroke =
        [this, &lattice, &lattice_valid](const QVector<qsizetype> &indices, double width,
                                         quint32 argb)
    {
        for (qsizetype index = 1; index < indices.size(); ++index)
        {
            const qsizetype from = indices.at(index - 1);
            const qsizetype to = indices.at(index);
            if (!lattice_valid.at(from) || !lattice_valid.at(to))
                continue;
            // Plain segments, no round joins: the glows are translucent and
            // overlapping join discs would bead along the densely sampled
            // frame.
            this->canvas->line(lattice.at(from).x, lattice.at(from).y,
                               lattice.at(to).x, lattice.at(to).y, width, argb);
        }
    };

    // Tile grid inside the range (thinner, under the frame).
    if (per_tile_x > 0)
    {
        for (int tile = 1; tile < int(width_tiles); ++tile)
        {
            QVector<qsizetype> indices;
            indices.reserve(rows + 1);
            for (int row = 0; row <= rows; ++row)
                indices.append(at(row, tile * per_tile_x));
            stroke(indices, 1.0, palette.grid);
        }
    }
    if (per_tile_y > 0)
    {
        for (int tile = 1; tile < int(height_tiles); ++tile)
        {
            QVector<qsizetype> indices;
            indices.reserve(columns + 1);
            for (int column = 0; column <= columns; ++column)
                indices.append(at(tile * per_tile_y, column));
            stroke(indices, 1.0, palette.grid);
        }
    }

    // The frame: the lattice's border, clockwise from the north-west corner.
    QVector<qsizetype> frame;
    frame.reserve(2 * (columns + rows) + 1);
    for (int column = 0; column <= columns; ++column)
        frame.append(at(0, column));
    for (int row = 1; row <= rows; ++row)
        frame.append(at(row, columns));
    for (int column = columns - 1; column >= 0; --column)
        frame.append(at(rows, column));
    for (int row = rows - 1; row >= 0; --row)
        frame.append(at(row, 0));
    stroke(frame, 12.0, palette.glow_wide);
    stroke(frame, 5.0, palette.glow_inner);
    stroke(frame, 1.5, palette.line);
}

void MapBgfxEditorOverlay::rectangleSelection()
{
    // The former QPainter editor's paintRectangleSelection().
    const MapEditorViewportRenderState &viewport = *this->inputs->viewport_state;
    if (!viewport.rectangle_selection_visible || viewport.rectangle_selection.isEmpty())
        return;
    const QRect &selection = viewport.rectangle_selection;
    const MapBgfxHudRect outer{selection.x() + 2.5, selection.y() + 2.5,
                               selection.width() - 5.0, selection.height() - 5.0};
    if (!(outer.width > 0.0) || !(outer.height > 0.0))
        return;
    const double radius = std::min(2.0, std::min(outer.width, outer.height) / 8.0);

    // Fill: vertical gradient.
    this->canvas->fillLinearGradient(
        outer, radius, true, outer.y, outer.bottom(), QVector<double>{0.0, 0.45, 1.0},
        QVector<quint32>{rgba(35, 151, 211, 24), rgba(0, 145, 215, 32), rgba(0, 65, 110, 38)});

    // Glows and the dark shadow under the steel frame.
    strokeRectCentered(outer, radius, 18.0, rgba(0, 149, 230, 52));
    strokeRectCentered(outer, radius, 9.0, rgba(23, 190, 255, 112));
    strokeRectCentered(outer, radius, 5.0, rgba(10, 15, 18, 205));

    // Steel frame: 3 px pen with a vertical gradient from the frame's top to
    // its bottom; drawn as four bands (top/bottom/left/right).
    const QVector<double> steel_positions = {0.0, 0.24, 0.52, 0.78, 1.0};
    const QVector<quint32> steel = {rgba(245, 250, 252, 245), rgba(129, 147, 153, 240),
                                    rgba(48, 61, 66, 245), rgba(177, 190, 194, 240),
                                    rgba(31, 42, 46, 245)};
    const double half = 1.5;
    const MapBgfxHudRect bands[4] = {
        {outer.x - half, outer.y - half, outer.width + 2.0 * half, 2.0 * half},
        {outer.x - half, outer.bottom() - half, outer.width + 2.0 * half, 2.0 * half},
        {outer.x - half, outer.y + half, 2.0 * half, outer.height - 2.0 * half},
        {outer.right() - half, outer.y + half, 2.0 * half, outer.height - 2.0 * half}};
    for (const MapBgfxHudRect &band : bands)
    {
        if (band.width > 0.0 && band.height > 0.0)
            this->canvas->fillLinearGradient(band, 0.0, true, outer.y, outer.bottom(),
                                             steel_positions, steel);
    }

    const MapBgfxHudRect edge{outer.x + 1.5, outer.y + 1.5, outer.width - 3.0, outer.height - 3.0};
    if (edge.width > 0.0 && edge.height > 0.0)
        strokeRectCentered(edge, std::max(0.0, radius - 1.0), 1.0, rgba(86, 215, 255, 180));

    // Dashed blue inner frame (Qt::DashLine: 4 on, 2 off in pen widths,
    // dash offset 1.5 pen widths).
    const MapBgfxHudRect frame{outer.x + 3.0, outer.y + 3.0, outer.width - 6.0, outer.height - 6.0};
    if (frame.width > 0.0 && frame.height > 0.0)
    {
        const QVector<MapBgfxHudPoint> loop = {
            {frame.x, frame.y}, {frame.right(), frame.y}, {frame.right(), frame.bottom()},
            {frame.x, frame.bottom()}, {frame.x, frame.y}};
        dashedPolyline(loop, 5.0, 20.0, 10.0, 7.5, rgba(36, 196, 255, 96));
        dashedPolyline(loop, 1.5, 6.0, 3.0, 2.25, rgba(102, 224, 255, 245));
    }
}

// --- Network details ------------------------------------------------------------------

void MapBgfxEditorOverlay::staticDetails(bool include_moving)
{
    // The former QPainter editor's paintRenderSurfaceStaticDetails(): the editor's
    // full-colour icons and the pipe vertex dots on top of the GPU network.
    const MapEditorVisualState &state = *this->inputs->visual_state;
    const NetworkRenderSnapshot &snapshot = *this->inputs->snapshot;
    const int width = state.entity_width;
    for (qsizetype index = 0; index < snapshot.nodes.size(); ++index)
    {
        const NetworkRenderNode &node = snapshot.nodes.at(index);
        if (!include_moving && this->moving_markers.contains(node.uuid))
            continue;
        const Highlight highlight = include_moving && this->selected_markers.contains(node.uuid)
            ? Highlight::Selected : Highlight::None;
        bottomAnchoredIcon(screen(this->geometry.node_world.at(index)), node.entity_type,
                           QString(), width, highlight, true);
    }
    for (qsizetype index = 0; index < snapshot.links.size(); ++index)
    {
        const NetworkRenderLink &link = snapshot.links.at(index);
        if (!include_moving && this->moving_links.contains(link.uuid))
            continue;
        if (link.entity_type == InfrastructureEntity::Pipe)
        {
            const QVector<MapBgfx2d::WorldPoint> &points = this->geometry.link_world.at(index);
            if (points.size() < 3)
                continue;
            // Cull before projecting (and allocating) the whole pipe.
            if (!boundsOnScreen(this->geometry.link_bounds.at(index), PipeVertexRadiusPx))
                continue;
            const quint32 color = include_moving && this->selected_pipes.contains(link.uuid)
                ? SelectedColor : BlackColor;
            const QVector<MapBgfxHudPoint> projected = screenPolyline(points);
            for (qsizetype vertex = 1; vertex + 1 < projected.size(); ++vertex)
            {
                const MapBgfxHudPoint &point = projected.at(vertex);
                if (onScreen(point.x - PipeVertexRadiusPx, point.y - PipeVertexRadiusPx,
                             2.0 * PipeVertexRadiusPx, 2.0 * PipeVertexRadiusPx))
                    this->canvas->fillCircle(point.x, point.y, PipeVertexRadiusPx, color);
            }
            continue;
        }
        if (!isDeviceLink(link.entity_type) || link.vertices_wgs84.size() < 2)
            continue;
        MapBgfxHudPoint start;
        MapBgfxHudPoint center;
        MapBgfxHudPoint end;
        if (!deviceLinkScreen(int(index), &start, &center, &end))
            continue;
        const Highlight highlight = include_moving && this->selected_markers.contains(link.uuid)
            ? Highlight::Selected : Highlight::None;
        centeredIcon(center, link.entity_type, QString(), width, highlight);
    }
    demandPointAttachments();
}

void MapBgfxEditorOverlay::movingNetwork()
{
    // The former QPainter editor's paintMovingNetwork(): the dragged entities, which
    // are hidden in the GPU network meanwhile.
    const MapEditorVisualState &state = *this->inputs->visual_state;
    for (const MapEditorDynamicLinkVisualState &link : state.move.links)
    {
        if (link.vertices_wgs84.size() < 2)
            continue;
        if (link.entity == InfrastructureEntity::Pipe)
        {
            const quint32 color = this->selected_pipes.contains(link.uuid) ? SelectedColor : BlackColor;
            const QVector<MapBgfxHudPoint> points = screenPolyline(link.vertices_wgs84);
            polyline(points, LineWidthPx, color);
            for (qsizetype vertex = 1; vertex + 1 < points.size(); ++vertex)
                this->canvas->fillCircle(points.at(vertex).x, points.at(vertex).y,
                                         PipeVertexRadiusPx, color);
            continue;
        }
        if (!isDeviceLink(link.entity) || link.vertices_wgs84.size() < 3)
            continue;
        const bool selected = this->selected_markers.contains(link.uuid);
        QList<CoordinateWGS84> three;
        three.append(link.vertices_wgs84.first());
        three.append(link.vertices_wgs84.at(1));
        three.append(link.vertices_wgs84.last());
        const QVector<MapBgfxHudPoint> points = screenPolyline(three);
        polyline(points, LineWidthPx, selected ? SelectedColor : DeviceLinkColor);
        centeredIcon(points.at(1), link.entity, QString(), state.entity_width,
                     selected ? Highlight::Selected : Highlight::None);
    }
    for (const MapEditorDynamicMarkerVisualState &marker : state.move.markers)
    {
        if (!isPointEntity(marker.entity))
            continue;
        const MapBgfxHudPoint point = screen(marker.coordinate_wgs84);
        this->canvas->fillCircle(point.x, point.y, MarkerDotRadiusPx, BlackColor);
        bottomAnchoredIcon(point, marker.entity, marker.pixmap_path, state.entity_width,
                           this->selected_markers.contains(marker.uuid) ? Highlight::Selected
                                                                        : Highlight::None,
                           true);
    }
}

void MapBgfxEditorOverlay::selectedPipes()
{
    // The former QPainter editor's paintSelectedPipes(): blue over the black GPU pipe.
    for (const QUuid &uuid : this->inputs->visual_state->selected_pipe_uuids)
    {
        const QHash<QUuid, int>::const_iterator found = this->geometry.link_index.constFind(uuid);
        if (found == this->geometry.link_index.cend())
            continue;
        const NetworkRenderLink &link = this->inputs->snapshot->links.at(found.value());
        const QVector<MapBgfx2d::WorldPoint> &points = this->geometry.link_world.at(found.value());
        if (link.entity_type != InfrastructureEntity::Pipe || points.size() < 2)
            continue;
        const QVector<MapBgfxHudPoint> projected = screenPolyline(points);
        polyline(projected, LineWidthPx, SelectedColor);
        for (qsizetype vertex = 1; vertex + 1 < projected.size(); ++vertex)
            this->canvas->fillCircle(projected.at(vertex).x, projected.at(vertex).y,
                                     PipeVertexRadiusPx, SelectedColor);
    }
}

void MapBgfxEditorOverlay::pipePlacement()
{
    // The former QPainter editor's paintPipePlacement(): start node, clicked
    // intermediate vertices, then the pointer or the snapped target node.
    const MapEditorPlacementVisualState &placement = this->inputs->visual_state->placement;
    if (!placement.creating || placement.entity != InfrastructureEntity::Pipe
        || placement.pipe_start_node_uuid.isNull())
        return;
    const NetworkRenderNode *start = nodeByUuid(placement.pipe_start_node_uuid);
    if (start == nullptr)
        return;
    QList<CoordinateWGS84> coordinates;
    coordinates.append(start->coordinate_wgs84);
    coordinates.append(placement.pipe_intermediate_vertices);
    QVector<MapBgfxHudPoint> points = screenPolyline(coordinates);
    const NetworkRenderNode *end = nodeByUuid(placement.connection_target_uuid);
    points.append(end != nullptr ? screen(end->coordinate_wgs84)
                                 : MapBgfxHudPoint{placement.mouse_position.x(),
                                                   placement.mouse_position.y()});
    polyline(points, LineWidthPx, PreviewColor);
}

void MapBgfxEditorOverlay::deviceLinkPlacement()
{
    // The former QPainter editor's paintDeviceLinkPlacement(): start -> middle -> end.
    const MapEditorPlacementVisualState &placement = this->inputs->visual_state->placement;
    if (!placement.creating || !isDeviceLink(placement.entity)
        || placement.device_link_start_node_uuid.isNull())
        return;
    const NetworkRenderNode *start_node = nodeByUuid(placement.device_link_start_node_uuid);
    if (start_node == nullptr)
        return;
    const MapBgfxHudPoint start = screen(start_node->coordinate_wgs84);
    const NetworkRenderNode *end_node = nodeByUuid(placement.connection_target_uuid);
    const MapBgfxHudPoint end = end_node != nullptr
        ? screen(end_node->coordinate_wgs84)
        : MapBgfxHudPoint{placement.mouse_position.x(), placement.mouse_position.y()};
    polyline(QVector<MapBgfxHudPoint>{start, midpoint(start, end), end}, LineWidthPx, PreviewColor);
}

void MapBgfxEditorOverlay::selectedMarkersAndDeviceLinks()
{
    // The former QPainter editor's paintSelectedMarkersAndDeviceLinks().
    const MapEditorVisualState &state = *this->inputs->visual_state;
    for (const QUuid &uuid : state.selected_marker_uuids)
    {
        const QHash<QUuid, int>::const_iterator found = this->geometry.link_index.constFind(uuid);
        if (found == this->geometry.link_index.cend())
            continue;
        const NetworkRenderLink &link = this->inputs->snapshot->links.at(found.value());
        if (!isDeviceLink(link.entity_type))
            continue;
        MapBgfxHudPoint start;
        MapBgfxHudPoint center;
        MapBgfxHudPoint end;
        if (!deviceLinkScreen(found.value(), &start, &center, &end))
            continue;
        polyline(QVector<MapBgfxHudPoint>{start, center, end}, LineWidthPx, SelectedColor);
        centeredIcon(center, link.entity_type, QString(), state.entity_width, Highlight::Selected);
    }
    for (const QUuid &uuid : state.selected_marker_uuids)
    {
        const QHash<QUuid, int>::const_iterator found = this->geometry.node_index.constFind(uuid);
        if (found == this->geometry.node_index.cend())
            continue;
        const NetworkRenderNode &node = this->inputs->snapshot->nodes.at(found.value());
        bottomAnchoredIcon(screen(this->geometry.node_world.at(found.value())), node.entity_type,
                           QString(), state.entity_width, Highlight::Selected, true);
    }
    const QUuid &target_uuid = state.placement.connection_target_uuid;
    const QHash<QUuid, int>::const_iterator target = this->geometry.node_index.constFind(target_uuid);
    if (!target_uuid.isNull() && target != this->geometry.node_index.cend())
    {
        const NetworkRenderNode &node = this->inputs->snapshot->nodes.at(target.value());
        const MapBgfxHudPoint point = screen(this->geometry.node_world.at(target.value()));
        this->canvas->fillCircle(point.x, point.y, ConnectionTargetRadiusPx, PreviewColor);
        bottomAnchoredIcon(point, node.entity_type, QString(), state.entity_width,
                           this->selected_markers.contains(target_uuid) ? Highlight::Selected
                                                                        : Highlight::None,
                           true);
    }
}

void MapBgfxEditorOverlay::simulationErrors()
{
    // The former QPainter editor's paintSimulationError(): red (current) or grey
    // (stale) lines and glows, a blue outline under selected links.
    const MapEditorVisualState &state = *this->inputs->visual_state;
    if (state.simulation_error_entities.isEmpty())
        return;
    for (QHash<QUuid, InfrastructureEntity>::const_iterator error =
             state.simulation_error_entities.cbegin();
         error != state.simulation_error_entities.cend(); ++error)
    {
        const QUuid &uuid = error.key();
        const InfrastructureEntity entity_type = error.value();
        const bool stale = state.simulation_stale_diagnostic_entity_uuids.contains(uuid);
        const quint32 color = stale ? StaleColor : ErrorColor;
        const QHash<QUuid, int>::const_iterator link_found = this->geometry.link_index.constFind(uuid);
        if (link_found != this->geometry.link_index.cend())
        {
            const NetworkRenderLink &link = this->inputs->snapshot->links.at(link_found.value());
            const QVector<MapBgfx2d::WorldPoint> &points = this->geometry.link_world.at(link_found.value());
            if (link.entity_type != entity_type || points.size() < 2)
                continue;
            const bool selected = link.entity_type == InfrastructureEntity::Pipe
                ? this->selected_pipes.contains(link.uuid)
                : this->selected_markers.contains(link.uuid);
            QVector<MapBgfxHudPoint> projected;
            MapBgfxHudPoint center;
            if (link.entity_type == InfrastructureEntity::Pipe)
            {
                projected = screenPolyline(points);
            }
            else if (isDeviceLink(link.entity_type))
            {
                MapBgfxHudPoint start;
                MapBgfxHudPoint end;
                if (!deviceLinkScreen(link_found.value(), &start, &center, &end))
                    continue;
                projected = QVector<MapBgfxHudPoint>{start, center, end};
            }
            else
            {
                continue;
            }
            if (selected)
                polyline(projected, SelectedOutlineWidthPx, SelectedColor);
            polyline(projected, LineWidthPx, color);
            if (isDeviceLink(link.entity_type))
                centeredIcon(center, link.entity_type, QString(), state.entity_width,
                             diagnosticHighlight(selected, stale));
            continue;
        }
        const QHash<QUuid, int>::const_iterator node_found = this->geometry.node_index.constFind(uuid);
        if (node_found == this->geometry.node_index.cend())
            continue;
        const NetworkRenderNode &node = this->inputs->snapshot->nodes.at(node_found.value());
        if (node.entity_type != entity_type)
            continue;
        bottomAnchoredIcon(screen(this->geometry.node_world.at(node_found.value())), node.entity_type,
                           QString(), state.entity_width,
                           diagnosticHighlight(this->selected_markers.contains(node.uuid), stale), true);
    }
}

void MapBgfxEditorOverlay::floatingPlacement()
{
    // The former QPainter editor's paintPlacement(): the marker under the pointer, or
    // a device link's icon midway to its pointer end.
    const MapEditorPlacementVisualState &placement = this->inputs->visual_state->placement;
    if (!placement.creating || !placement.floating_marker_visible
        || placement.entity == InfrastructureEntity::Unknown || placement.floating_width <= 0)
        return;
    const MapBgfxHudPoint mouse{placement.mouse_position.x(), placement.mouse_position.y()};
    if (isDeviceLink(placement.entity) && !placement.device_link_start_node_uuid.isNull())
    {
        const NetworkRenderNode *start_node = nodeByUuid(placement.device_link_start_node_uuid);
        if (start_node != nullptr)
        {
            const MapBgfxHudPoint start = screen(start_node->coordinate_wgs84);
            const NetworkRenderNode *end_node = nodeByUuid(placement.connection_target_uuid);
            const MapBgfxHudPoint end = end_node != nullptr ? screen(end_node->coordinate_wgs84) : mouse;
            centeredIcon(midpoint(start, end), placement.entity, QString(), placement.floating_width,
                         Highlight::None);
            return;
        }
    }
    bottomAnchoredIcon(mouse, placement.entity, QString(), placement.floating_width,
                       Highlight::None, false);
}

void MapBgfxEditorOverlay::demandPointAttachments()
{
    // The former QPainter editor's paintDemandPointAttachment(): dashed line from the
    // demand point to its network attachment with a round handle. Runs every
    // frame for every attachment, so: no allocations, culled against the
    // viewport, the dashes clipped to it (a long attachment at a high zoom
    // would otherwise emit thousands of off-screen dashes), and handles only
    // where the line is long enough to show them (MinimumAttachmentHandleLinePx).
    const MapEditorVisualState &state = *this->inputs->visual_state;
    const MapBgfx2d::Camera &camera = this->inputs->camera;
    const double viewport_width = this->inputs->viewport.width();
    const double viewport_height = this->inputs->viewport.height();
    // Handle and selection-circle reach around the line.
    constexpr double Margin = 16.0;
    const quint32 emphasized_color = rgba(0, 120, 190, 255);
    for (const MapEditorDemandPointAttachmentVisualState &attachment : state.demand_point_attachments)
    {
        if (!attachment.visible || !finite(attachment.demand_point_coordinate_wgs84))
            continue;
        if (!attachment.attachment_coordinate_valid)
        {
            if (attachment.selecting_target)
            {
                const MapBgfxHudPoint demand = screen(attachment.demand_point_coordinate_wgs84);
                if (onScreen(demand.x - Margin, demand.y - Margin, 2.0 * Margin, 2.0 * Margin))
                    dashedCircle(demand.x, demand.y, 11.0, 1.5, 6.0, 3.0, emphasized_color);
            }
            continue;
        }
        // Both ends as one continuous segment at the copy nearest to the
        // camera (screenPolyline() rule for the pair).
        MapBgfx2d::ScreenPoint start;
        MapBgfx2d::ScreenPoint end;
        MapBgfx2d::screenSegment(camera, world(attachment.demand_point_coordinate_wgs84),
                                 world(attachment.attachment_coordinate_wgs84), &start, &end);
        const MapBgfxHudPoint demand{start.x + this->offset_x, start.y + this->offset_y};
        const MapBgfxHudPoint network{end.x + this->offset_x, end.y + this->offset_y};
        const double min_x = std::min(demand.x, network.x);
        const double min_y = std::min(demand.y, network.y);
        if (!onScreen(min_x - Margin, min_y - Margin,
                      std::fabs(network.x - demand.x) + 2.0 * Margin,
                      std::fabs(network.y - demand.y) + 2.0 * Margin))
            continue;

        const bool emphasized = attachment.selected || attachment.selecting_target || attachment.moving;
        const quint32 line_color = emphasized ? emphasized_color : rgba(70, 105, 125, 190);
        const double line_width = emphasized ? 1.5 : 1.0;
        const double length = std::hypot(network.x - demand.x, network.y - demand.y);
        double t0 = 0.0;
        double t1 = 1.0;
        if (length > 0.0
            && clipSegment(demand, network, -Margin, -Margin, viewport_width + Margin,
                           viewport_height + Margin, &t0, &t1))
        {
            // The pattern continues from where the clipped part starts.
            dashedSegment(MapBgfxHudPoint{demand.x + (network.x - demand.x) * t0,
                                          demand.y + (network.y - demand.y) * t0},
                          MapBgfxHudPoint{demand.x + (network.x - demand.x) * t1,
                                          demand.y + (network.y - demand.y) * t1},
                          line_width, 4.0 * line_width, 2.0 * line_width, t0 * length,
                          line_color);
        }

        const double radius = attachment.moving ? 7.0 : attachment.selected ? 6.0 : 4.5;
        const double pen = emphasized ? 2.0 : 1.5;
        if ((emphasized || length >= MinimumAttachmentHandleLinePx)
            && onScreen(network.x - radius - pen, network.y - radius - pen,
                        2.0 * (radius + pen), 2.0 * (radius + pen)))
        {
            disc(network.x, network.y, radius, pen, rgba(255, 255, 255, emphasized ? 230 : 190),
                 line_color);
        }
        if (attachment.selecting_target
            && onScreen(demand.x - Margin, demand.y - Margin, 2.0 * Margin, 2.0 * Margin))
            dashedCircle(demand.x, demand.y, 10.0, 1.0, 1.0, 2.0, emphasized_color);
    }
}
