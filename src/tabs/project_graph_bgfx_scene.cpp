#include "tabs/project_graph_bgfx_scene.h"

#include "projects/dvmdb_identity.h"
#include "tabs/project_revision_card_geometry.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QHash>
#include <QLineF>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double project_height = 68.0;
constexpr double server_width = 252.0;
constexpr double server_height = 76.0;
constexpr double branch_width = 196.0;
constexpr double branch_height = 70.0;
constexpr double fold_height = 58.0;

quint32 tinted(const QColor &color, int alpha)
{
    QColor result = color;
    result.setAlpha(alpha);
    return result.rgba();
}

MapBgfxHudRect hudRect(const QRectF &rect)
{
    return {rect.left(), rect.top(), rect.width(), rect.height()};
}

QRectF cardBounds(const ProjectGraphNode &node)
{
    if (node.kind == ProjectGraphNode::Kind::Revision)
        return ProjectRevisionCardGeometry::bounds(node.position, 0);
    const double width = node.kind == ProjectGraphNode::Kind::Server ? server_width
        : node.kind == ProjectGraphNode::Kind::Branch ? branch_width
        : ProjectRevisionCardGeometry::width;
    const double height = node.kind == ProjectGraphNode::Kind::Server ? server_height
        : node.kind == ProjectGraphNode::Kind::Branch ? branch_height : project_height;
    return {node.position.x() - width * 0.5, node.position.y() - height * 0.5,
            width, height};
}

QPointF screenPoint(const QPointF &point, const ProjectGraphBgfxScene::View &view)
{
    return view.offset + point * view.zoom;
}

QRectF screenRect(const QRectF &rect, const ProjectGraphBgfxScene::View &view)
{
    return {screenPoint(rect.topLeft(), view), rect.size() * view.zoom};
}

void line(MapBgfxHudCanvas *canvas, const QPointF &begin, const QPointF &end,
          double width, quint32 color)
{
    canvas->line(begin.x(), begin.y(), end.x(), end.y(), width, color);
}

QPointF cubicPoint(const QPointF &a, const QPointF &b, const QPointF &c,
                   const QPointF &d, double t)
{
    const double s = 1.0 - t;
    return a * (s * s * s) + b * (3.0 * s * s * t)
        + c * (3.0 * s * t * t) + d * (t * t * t);
}

// Sample the same Bezier route that the current QWidget painter uses.
// Screen-space subdivision limits visual differences between the two paths.
void cubic(MapBgfxHudCanvas *canvas, const ProjectGraphBgfxScene::View &view,
           const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d,
           double width, quint32 color, bool dashed)
{
    const double span = QLineF(screenPoint(a, view), screenPoint(b, view)).length()
        + QLineF(screenPoint(b, view), screenPoint(c, view)).length()
        + QLineF(screenPoint(c, view), screenPoint(d, view)).length();
    const int segments = std::clamp(int(std::ceil(span / 9.0)), 4, 96);
    QPointF previous = screenPoint(a, view);
    for (int index = 1; index <= segments; ++index)
    {
        const QPointF next = screenPoint(cubicPoint(a, b, c, d,
            double(index) / double(segments)), view);
        if (!dashed || (index % 4) < 2)
            line(canvas, previous, next, width, color);
        previous = next;
    }
}

void route(MapBgfxHudCanvas *canvas, const ProjectGraphBgfxScene::View &view,
           const QPointF &begin, const QPointF &end,
           const QVector<QPointF> &control_points, double bend,
           double width, quint32 color, bool dashed = false)
{
    // A cubic Bezier stays inside the convex hull of its control points.
    // Reject wholly offscreen routes BEFORE tessellation: on long histories,
    // otherwise hundreds of thousands of invisible line segments are added
    // to the transient GPU draw list on every pan or zoom frame.
    double min_x = std::min(begin.x(), end.x());
    double min_y = std::min(begin.y(), end.y());
    double max_x = std::max(begin.x(), end.x());
    double max_y = std::max(begin.y(), end.y());
    const bool valid_route = control_points.size() >= 4
        && (control_points.size() - 1) % 3 == 0;
    if (valid_route)
    {
        for (const QPointF &point : control_points)
        {
            min_x = std::min(min_x, point.x());
            min_y = std::min(min_y, point.y());
            max_x = std::max(max_x, point.x());
            max_y = std::max(max_y, point.y());
        }
    }
    else if (std::fabs(begin.x() - end.x()) >= 0.01)
    {
        // The fallback cubic's control points extend vertically by bend.
        min_y -= std::fabs(bend);
        max_y += std::fabs(bend);
    }
    const QPointF upper_left = screenPoint(QPointF(min_x, min_y), view);
    const QPointF lower_right = screenPoint(QPointF(max_x, max_y), view);
    const QRectF projected(upper_left, lower_right);
    const double margin = std::max(12.0, width + 3.0);
    if (!projected.adjusted(-margin, -margin, margin, margin)
             .intersects(view.viewport))
        return;
    if (valid_route)
    {
        for (int i = 1; i + 2 < control_points.size(); i += 3)
            cubic(canvas, view, control_points.at(i - 1), control_points.at(i),
                  control_points.at(i + 1), control_points.at(i + 2),
                  width, color, dashed);
        return;
    }
    if (std::fabs(begin.x() - end.x()) < 0.01)
    {
        line(canvas, screenPoint(begin, view), screenPoint(end, view), width, color);
        return;
    }
    const double sign = view.top_down ? 1.0 : -1.0;
    cubic(canvas, view, begin, begin + QPointF(0, sign * bend),
          end - QPointF(0, sign * bend), end, width, color, dashed);
}

// HUD text is a *raster* sprite, unlike the cards and edges. Enlarging the
// same atlas entry by the camera zoom magnifies individual glyph pixels.
// Rasterize instead at a scale at least as large as the screen-space scale.
// Quarter-step buckets reuse entries throughout inertial zoom animations.
// At zoom <= 1 the existing atlas glyphs are only minified.
double textRasterScale(double zoom)
{
    return std::max(1.0, std::ceil(zoom * 4.0 - 1e-7) / 4.0);
}

QFont textRasterFont(const QFont &font, double scale)
{
    if (scale <= 1.0)
        return font;
    QFont scaled = font;
    if (font.pixelSize() > 0)
        scaled.setPixelSize(std::max(1, int(std::ceil(double(font.pixelSize()) * scale))));
    else if (font.pointSizeF() > 0.0)
        scaled.setPointSizeF(font.pointSizeF() * scale);
    return scaled;
}

void text(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *atlas,
          const QString &value, const QFont &font, const QRectF &rect,
          double zoom, quint32 color, ProjectGraphBgfxScene::Stats *stats)
{
    if (value.isEmpty() || rect.width() <= 0.0 || rect.height() <= 0.0)
        return;
    // Eliding uses graph-space metrics, exactly as before: changing raster
    // resolution must not alter the name/branch text that fits on a card.
    const QFontMetricsF metrics(font);
    const QString display = metrics.elidedText(value, Qt::ElideRight,
                                               int(rect.width() / zoom));
    const double raster_scale = textRasterScale(zoom);
    const MapBgfxHudSprite sprite = atlas->text(display, textRasterFont(font, raster_scale));
    if (!sprite.valid)
        return;
    // The font was enlarged before rasterization. Undo that extra size when
    // placing its atlas quad so the text retains its original graph size.
    // This factor never exceeds one: no blurry magnification at high zoom.
    const double draw_scale = zoom / raster_scale;
    const double rendered_height = sprite.height * draw_scale;
    const double y = rect.top() + (rect.height() - rendered_height) * 0.5;
    canvas->sprite({rect.left() - sprite.origin_x * draw_scale,
                    y - sprite.origin_y * draw_scale,
                    sprite.width * draw_scale, rendered_height}, sprite, color);
    ++stats->text_runs;
}

void rectCard(MapBgfxHudCanvas *canvas, const QRectF &rect, double radius,
              double stroke, quint32 fill, quint32 border)
{
    canvas->fillRoundedRect(hudRect(rect), radius, fill);
    canvas->strokeRoundedRect(hudRect(rect), radius, stroke, border);
}

void databaseIcon(MapBgfxHudCanvas *canvas, const QRectF &box, quint32 color,
                  double zoom)
{
    canvas->strokeRoundedRect(hudRect(box), 5.0 * zoom, 1.7 * zoom, color);
    // Cylinder top and the shallow front seam, without platform icons or
    // an extra GPU shader. The same line canvas is used for ancestry routes.
    const int steps = 24;
    for (int arc = 0; arc < 2; ++arc)
    {
        const double cy = box.top() + (arc == 0 ? 4.0 : 13.0) * zoom;
        const double radius_x = box.width() * 0.5;
        const double radius_y = 4.0 * zoom;
        const int first = 0;
        const int last = arc == 0 ? steps * 2 : steps;
        for (int i = first; i < last; ++i)
        {
            const double first_angle = double(i) * MapBgfxHudCanvas::Pi / double(steps);
            const double second_angle = double(i + 1) * MapBgfxHudCanvas::Pi / double(steps);
            const QPointF a(box.center().x() + radius_x * std::cos(first_angle),
                            cy + radius_y * std::sin(first_angle));
            const QPointF b(box.center().x() + radius_x * std::cos(second_angle),
                            cy + radius_y * std::sin(second_angle));
            line(canvas, a, b, 1.7 * zoom, color);
        }
    }
}
}

void ProjectGraphBgfxScene::Index::clear()
{
    this->nodes.clear();
    this->folds.clear();
    this->edges.clear();
    this->items.clear();
    this->order.clear();
    this->branches.clear();
    this->root = -1;
    this->structural_edges = 0;
    this->ancestry_edges = 0;
    this->head_edges = 0;
}

void ProjectGraphBgfxScene::Index::appendEdge(
    const QPointF &begin, const QPointF &end, const QVector<QPointF> &controls,
    double bend, bool dashed, Type type)
{
    double min_x = std::min(begin.x(), end.x());
    double min_y = std::min(begin.y(), end.y());
    double max_x = std::max(begin.x(), end.x());
    double max_y = std::max(begin.y(), end.y());
    const bool valid_route = controls.size() >= 4
        && (controls.size() - 1) % 3 == 0;
    if (valid_route)
    {
        // Bezier curves stay within their control-point convex hull. Keep
        // long edges whose endpoints are offscreen but whose route crosses
        // the current viewport.
        for (const QPointF &point : controls)
        {
            min_x = std::min(min_x, point.x());
            min_y = std::min(min_y, point.y());
            max_x = std::max(max_x, point.x());
            max_y = std::max(max_y, point.y());
        }
    }
    else if (std::fabs(begin.x() - end.x()) >= 0.01)
    {
        min_y -= std::fabs(bend);
        max_y += std::fabs(bend);
    }
    // A vertical link can have zero width. Give every index entry a
    // non-empty rectangle so QRectF intersection cannot reject it.
    const QRectF bounds(min_x - 0.001, min_y - 0.001,
                        max_x - min_x + 0.002, max_y - min_y + 0.002);
    const int index = this->edges.size();
    this->edges.append({begin, end, controls, bend, dashed});
    this->items.append({bounds, type, index});
}

int ProjectGraphBgfxScene::Index::buildBranch(int begin, int end)
{
    const int index = this->branches.size();
    this->branches.append(Branch{});
    QRectF bounds = this->items.at(this->order.at(begin)).bounds;
    double min_x = bounds.left();
    double min_y = bounds.top();
    double max_x = bounds.right();
    double max_y = bounds.bottom();
    for (int i = begin + 1; i < end; ++i)
    {
        const QRectF &item = this->items.at(this->order.at(i)).bounds;
        min_x = std::min(min_x, item.left());
        min_y = std::min(min_y, item.top());
        max_x = std::max(max_x, item.right());
        max_y = std::max(max_y, item.bottom());
    }
    Branch branch;
    branch.bounds = QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
    if (end - begin <= 12)
    {
        branch.begin = begin;
        branch.end = end;
    }
    else
    {
        const bool split_x = branch.bounds.width() > branch.bounds.height();
        const int middle = begin + (end - begin) / 2;
        std::nth_element(this->order.begin() + begin,
                         this->order.begin() + middle,
                         this->order.begin() + end,
                         [this, split_x](int a, int b)
        {
            const QRectF &first = this->items.at(a).bounds;
            const QRectF &second = this->items.at(b).bounds;
            return split_x ? first.center().x() < second.center().x()
                           : first.center().y() < second.center().y();
        });
        branch.left = buildBranch(begin, middle);
        branch.right = buildBranch(middle, end);
    }
    this->branches[index] = branch;
    return index;
}

void ProjectGraphBgfxScene::Index::visibleItems(
    const QRectF &bounds, QVector<int> *result) const
{
    result->clear();
    if (this->root < 0 || bounds.isEmpty())
        return;
    QVector<int> pending{this->root};
    while (!pending.isEmpty())
    {
        const int index = pending.takeLast();
        const Branch &branch = this->branches.at(index);
        if (!branch.bounds.intersects(bounds))
            continue;
        if (branch.left >= 0)
        {
            pending.append(branch.left);
            pending.append(branch.right);
        }
        else
        {
            for (int i = branch.begin; i < branch.end; ++i)
            {
                const int item_index = this->order.at(i);
                if (this->items.at(item_index).bounds.intersects(bounds))
                    result->append(item_index);
            }
        }
    }
    // Preserve the existing painter order: structural edges, fold routes,
    // ancestry/HEAD routes, cards, fold labels. Ordering is independent of
    // BVH partitioning and changes no selection or visual precedence.
    std::sort(result->begin(), result->end());
}

void ProjectGraphBgfxScene::Index::rebuild(
    const QVector<ProjectGraphNode> &nodes,
    const QVector<Fold> &folds, bool top_down)
{
    clear();
    this->nodes = nodes;
    this->folds = folds;
    QHash<QString, int> by_id;
    by_id.reserve(this->nodes.size());
    for (int i = 0; i < this->nodes.size(); ++i)
        by_id.insert(this->nodes.at(i).id, i);
    const double sign = top_down ? 1.0 : -1.0;
    // Build in draw order, so visibility queries can preserve layering.
    for (const ProjectGraphNode &node : this->nodes)
    {
        const int parent_index = by_id.value(node.container_id, -1);
        if (parent_index < 0)
            continue;
        const ProjectGraphNode &parent = this->nodes.at(parent_index);
        const QRectF start = cardBounds(parent);
        const QRectF end = cardBounds(node);
        const QPointF begin(parent.position.x(), parent.position.y()
                             + sign * start.height() * 0.5);
        const QPointF finish(node.position.x(), node.position.y()
                              - sign * end.height() * 0.5);
        const double bend = std::max(24.0, std::fabs(finish.y() - begin.y()) * 0.45);
        appendEdge(begin, finish, {}, bend, false, Type::Structural);
        ++this->structural_edges;
    }
    for (const Fold &fold : this->folds)
    {
        const int child_index = by_id.value(fold.visible_child_id, -1);
        const int parent_index = by_id.value(fold.visible_parent_id, -1);
        if (child_index < 0)
            continue;
        const ProjectGraphNode &child = this->nodes.at(child_index);
        if (parent_index < 0 && fold.visible_parent_id.isEmpty())
        {
            const int project_index = by_id.value(child.project_group_id, -1);
            if (project_index >= 0)
            {
                const ProjectGraphNode &project = this->nodes.at(project_index);
                appendEdge(project.position + QPointF(0, sign * project_height * 0.5),
                           fold.position - QPointF(0, sign * fold_height * 0.5),
                           {}, 40.0, false, Type::FoldEdge);
            }
        }
        if (parent_index >= 0)
        {
            const ProjectGraphNode &parent = this->nodes.at(parent_index);
            appendEdge(parent.position + QPointF(0, sign * cardBounds(parent).height() * 0.5),
                       fold.position - QPointF(0, sign * fold_height * 0.5),
                       {}, 40.0, true, Type::FoldEdge);
        }
        appendEdge(fold.position + QPointF(0, sign * fold_height * 0.5),
                   child.position - QPointF(0, sign * cardBounds(child).height() * 0.5),
                   {}, 40.0, true, Type::FoldEdge);
    }
    for (const ProjectGraphNode &node : this->nodes)
    {
        for (const QString &parent_id : node.parents)
        {
            const int parent_index = by_id.value(parent_id, -1);
            if (parent_index < 0)
                continue;
            const ProjectGraphNode &parent = this->nodes.at(parent_index);
            appendEdge(parent.position + QPointF(0, sign * cardBounds(parent).height() * 0.5),
                       node.position - QPointF(0, sign * cardBounds(node).height() * 0.5),
                       node.parent_routes.value(parent_id), 42.0, false, Type::Ancestry);
            ++this->ancestry_edges;
        }
        if (node.kind != ProjectGraphNode::Kind::Branch)
            continue;
        const int parent_index = by_id.value(node.head_revision_id, -1);
        if (parent_index < 0)
            continue;
        const ProjectGraphNode &parent = this->nodes.at(parent_index);
        if (parent.kind != ProjectGraphNode::Kind::Revision)
            continue;
        appendEdge(parent.position + QPointF(0, sign * cardBounds(parent).height() * 0.5),
                   node.position - QPointF(0, sign * cardBounds(node).height() * 0.5),
                   node.parent_routes.value(node.head_revision_id), 42.0, false, Type::Head);
        ++this->head_edges;
    }
    for (int i = 0; i < this->nodes.size(); ++i)
        this->items.append({cardBounds(this->nodes.at(i)), Type::Card, i});
    for (int i = 0; i < this->folds.size(); ++i)
    {
        const Fold &fold = this->folds.at(i);
        this->items.append({QRectF(fold.position.x() - ProjectRevisionCardGeometry::width * 0.5,
                                   fold.position.y() - fold_height * 0.5,
                                   ProjectRevisionCardGeometry::width, fold_height),
                            Type::FoldCard, i});
    }
    this->order.reserve(this->items.size());
    this->branches.reserve(this->items.size() / 6 + 2);
    for (int i = 0; i < this->items.size(); ++i)
        this->order.append(i);
    if (!this->items.isEmpty())
        this->root = buildBranch(0, this->order.size());
}

ProjectGraphBgfxScene::Stats ProjectGraphBgfxScene::build(
    MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *atlas,
    const Index &index, const View &view)
{
    const QVector<ProjectGraphNode> &nodes = index.nodes;
    const QVector<Fold> &folds = index.folds;
    Stats stats;
    if (canvas == nullptr || atlas == nullptr || view.zoom <= 0.0
        || view.viewport.isEmpty())
        return stats;
    canvas->clear();
    atlas->beginFrame();
    canvas->setWhiteTexel(atlas->whiteU(), atlas->whiteV());

    const QColor base = view.palette.color(QPalette::Base);
    const QColor ink = view.palette.color(QPalette::Text);
    const QColor muted = view.palette.color(QPalette::PlaceholderText);
    const QColor accent = view.palette.color(QPalette::Highlight);
    canvas->fillRect(hudRect(view.viewport), base.rgba());

    const double grid = 36.0 * view.zoom;
    if (grid >= 12.0)
    {
        const double x0 = std::fmod(view.offset.x(), grid);
        const double y0 = std::fmod(view.offset.y(), grid);
        for (double x = x0; x < view.viewport.right(); x += grid)
            for (double y = y0; y < view.viewport.bottom(); y += grid)
                if (x >= view.viewport.left() && y >= view.viewport.top())
                    canvas->fillRect({x, y, 1.0, 1.0}, tinted(muted, 27));
    }

    QFont title_font = view.font;
    title_font.setBold(true);
    QFont small_font = view.font;
    if (small_font.pointSizeF() > 0.0)
        small_font.setPointSizeF(std::max(7.0, small_font.pointSizeF() - 1.0));

    if (nodes.isEmpty())
    {
        text(canvas, atlas, QStringLiteral("No revisions to display"), title_font,
             view.viewport.adjusted(24, 15, -24, -15), view.zoom, muted.rgba(), &stats);
        return stats;
    }

    // Convert the viewport to graph-space once. A conservative screen-pixel
    // margin retains thick edges and cards touching the viewport boundary.
    const double margin = std::max(12.0, 3.0 * view.zoom + 3.0) / view.zoom;
    const QRectF graph_view((view.viewport.left() - view.offset.x()) / view.zoom,
                            (view.viewport.top() - view.offset.y()) / view.zoom,
                            view.viewport.width() / view.zoom,
                            view.viewport.height() / view.zoom);
    QVector<int> visible_items;
    index.visibleItems(graph_view.adjusted(-margin, -margin, margin, margin),
                       &visible_items);
    stats.candidate_items = visible_items.size();
    stats.structural_edges = index.structural_edges;
    stats.ancestry_edges = index.ancestry_edges;
    stats.head_edges = index.head_edges;
    const QColor flow_green(57, 184, 145);
    for (const int item_index : visible_items)
    {
        const Index::Item &item = index.items.at(item_index);
        if (item.type == Index::Type::Card || item.type == Index::Type::FoldCard)
            continue;
        const Index::Edge &edge = index.edges.at(item.index);
        const bool structural = item.type == Index::Type::Structural;
        const bool fold_edge = item.type == Index::Type::FoldEdge;
        const bool head = item.type == Index::Type::Head;
        const double width = (structural || fold_edge || head ? 2.5 : 3.0)
            * view.zoom;
        const quint32 color = head ? tinted(flow_green, 190)
            : tinted(accent, structural || fold_edge ? 145 : 115);
        route(canvas, view, edge.begin, edge.end, edge.controls, edge.bend,
              width, color, edge.dashed);
    }

    for (const int item_index : visible_items)
    {
        const Index::Item &item = index.items.at(item_index);
        if (item.type != Index::Type::Card)
            continue;
        const ProjectGraphNode &node = nodes.at(item.index);
        const QRectF bounds = screenRect(cardBounds(node), view);
        if (!bounds.intersects(view.viewport.adjusted(-12, -12, 12, 12)))
            continue;
        const bool selected = node.id == view.selection_id;
        const QColor surface = view.palette.color(QPalette::Window);
        QColor border;
        quint32 fill;
        double radius = 11.0;
        double stroke = 1.6;
        if (node.kind == ProjectGraphNode::Kind::Revision)
        {
            border = selected ? accent : QColor::fromRgba(tinted(muted, 125));
            fill = surface.rgba();
            radius = 12.0;
            stroke = selected ? 3.0 : 1.4;
        }
        else
        {
            const bool server = node.kind == ProjectGraphNode::Kind::Server;
            const bool personal = server && node.personal_server;
            const QColor group = node.kind == ProjectGraphNode::Kind::Branch
                ? QColor(57, 184, 145)
                : personal ? QColor(222, 169, 46)
                : server && node.pending_server ? QColor(126, 130, 134)
                : server ? QColor(50, 174, 173) : accent;
            border = QColor::fromRgba(tinted(group,
                selected ? 255 : server ? 230 : node.kind == ProjectGraphNode::Kind::Branch ? 175 : 140));
            fill = tinted(group, personal ? selected ? 185 : 145
                          : selected ? 100 : server ? 64
                          : node.kind == ProjectGraphNode::Kind::Branch ? 35 : 24);
            radius = server ? 15.0 : 11.0;
            stroke = selected ? 3.4 : server ? 2.2 : 1.8;
        }
        rectCard(canvas, bounds, radius * view.zoom, stroke * view.zoom, fill,
                 border.rgba());
        ++stats.cards;

        if (node.kind == ProjectGraphNode::Kind::Revision)
        {
            if (selected)
            {
                const QRectF strip(cardBounds(node).left(), cardBounds(node).top() + 10,
                                   5, cardBounds(node).height() - 20);
                canvas->fillRoundedRect(hudRect(screenRect(strip, view)), 2.0 * view.zoom,
                                        accent.rgba());
            }
            const QRectF original = cardBounds(node);
            const QString short_id = DvmdbIdentity::shortLabel(
                node.display_id.isEmpty() ? node.id : node.display_id);
            const QString title = ProjectRevisionCardGeometry::distinctTitle(node.title, short_id);
            text(canvas, atlas, title, title_font,
                 screenRect(ProjectRevisionCardGeometry::titleRect(original), view),
                 view.zoom, ink.rgba(), &stats);
            text(canvas, atlas, node.branch, small_font,
                 screenRect(ProjectRevisionCardGeometry::revisionIdRect(original), view),
                 view.zoom, muted.rgba(), &stats);
            continue;
        }
        const QRectF original = cardBounds(node);
        const bool server = node.kind == ProjectGraphNode::Kind::Server;
        const bool branch = node.kind == ProjectGraphNode::Kind::Branch;
        const QColor secondary = branch ? QColor(57, 184, 145) : muted;
        const double left = server ? 53.0 : 14.0;
        const QString label = server && node.personal_server
            ? QStringLiteral("♛ ") + node.title : node.title;
        text(canvas, atlas, label, title_font,
             screenRect(original.adjusted(left, 6, -12, -32), view), view.zoom, ink.rgba(), &stats);
        text(canvas, atlas, node.branch, small_font,
             screenRect(original.adjusted(left, 35, -12, -6), view),
             view.zoom, secondary.rgba(), &stats);
        if (server)
        {
            const QRectF badge = screenRect(QRectF(original.left() + 16,
                original.top() + 20, 27, 26), view);
            databaseIcon(canvas, badge, border.rgba(), view.zoom);
        }
    }

    stats.culled_cards = nodes.size() - stats.cards;
    for (const int item_index : visible_items)
    {
        const Index::Item &item = index.items.at(item_index);
        if (item.type != Index::Type::FoldCard)
            continue;
        const Fold &fold = folds.at(item.index);
        const QRectF graph_box(fold.position.x() - ProjectRevisionCardGeometry::width * 0.5,
                               fold.position.y() - fold_height * 0.5,
                               ProjectRevisionCardGeometry::width, fold_height);
        const QRectF box = screenRect(graph_box, view);
        if (!box.intersects(view.viewport))
            continue;
        rectCard(canvas, box, 10.0 * view.zoom, 2.0 * view.zoom,
                 tinted(accent, 28), tinted(accent, 180));
        const QString line_one = QCoreApplication::translate("ProjectRevisionGraph",
            "+ %n hidden revision(s)", nullptr, fold.revision_count);
        const QString line_two = QCoreApplication::translate("ProjectRevisionGraph",
            "Click to unfold");
        const double half = box.height() * 0.5;
        text(canvas, atlas, line_one, small_font,
             QRectF(box.left() + 8, box.top() + 3, box.width() - 16, half - 3),
             view.zoom, ink.rgba(), &stats);
        text(canvas, atlas, line_two, small_font,
             QRectF(box.left() + 8, box.top() + half, box.width() - 16, half - 3),
             view.zoom, ink.rgba(), &stats);
        ++stats.fold_markers;
    }
    return stats;
}

ProjectGraphBgfxScene::Stats ProjectGraphBgfxScene::build(
    MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *atlas,
    const QVector<ProjectGraphNode> &nodes, const QVector<Fold> &folds,
    const View &view)
{
    Index index;
    index.rebuild(nodes, folds, view.top_down);
    return build(canvas, atlas, index, view);
}
