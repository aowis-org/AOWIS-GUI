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
    if (control_points.size() >= 4 && (control_points.size() - 1) % 3 == 0)
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

void text(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *atlas,
          const QString &value, const QFont &font, const QRectF &rect,
          double zoom, quint32 color, ProjectGraphBgfxScene::Stats *stats)
{
    if (value.isEmpty() || rect.width() <= 0.0 || rect.height() <= 0.0)
        return;
    const QFontMetricsF metrics(font);
    const QString display = metrics.elidedText(value, Qt::ElideRight,
                                               int(rect.width() / zoom));
    const MapBgfxHudSprite sprite = atlas->text(display, font);
    if (!sprite.valid)
        return;
    const double rendered_height = sprite.height * zoom;
    const double y = rect.top() + (rect.height() - rendered_height) * 0.5;
    canvas->sprite({rect.left() - sprite.origin_x * zoom, y - sprite.origin_y * zoom,
                    sprite.width * zoom, rendered_height}, sprite, color);
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

ProjectGraphBgfxScene::Stats ProjectGraphBgfxScene::build(
    MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *atlas,
    const QVector<ProjectGraphNode> &nodes, const QVector<Fold> &folds,
    const View &view)
{
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

    QHash<QString, const ProjectGraphNode *> by_id;
    by_id.reserve(nodes.size());
    for (const ProjectGraphNode &node : nodes)
        by_id.insert(node.id, &node);
    const double sign = view.top_down ? 1.0 : -1.0;
    for (const ProjectGraphNode &node : nodes)
    {
        const ProjectGraphNode *parent = by_id.value(node.container_id, nullptr);
        if (parent == nullptr)
            continue;
        const QRectF start = cardBounds(*parent);
        const QRectF end = cardBounds(node);
        const QPointF begin(parent->position.x(), parent->position.y()
                             + sign * start.height() * 0.5);
        const QPointF finish(node.position.x(), node.position.y()
                              - sign * end.height() * 0.5);
        const double bend = std::max(24.0, std::fabs(finish.y() - begin.y()) * 0.45);
        route(canvas, view, begin, finish, {}, bend, 2.5 * view.zoom,
              tinted(accent, 145));
        ++stats.structural_edges;
    }

    for (const Fold &fold : folds)
    {
        const ProjectGraphNode *child = by_id.value(fold.visible_child_id, nullptr);
        const ProjectGraphNode *parent = by_id.value(fold.visible_parent_id, nullptr);
        if (child == nullptr)
            continue;
        if (parent == nullptr && fold.visible_parent_id.isEmpty())
        {
            const ProjectGraphNode *project = by_id.value(child->project_group_id, nullptr);
            if (project != nullptr)
                route(canvas, view,
                      project->position + QPointF(0, sign * project_height * 0.5),
                      fold.position - QPointF(0, sign * fold_height * 0.5),
                      {}, 40.0, 2.5 * view.zoom, tinted(accent, 145));
        }
        if (parent != nullptr)
            route(canvas, view,
                  parent->position + QPointF(0, sign * cardBounds(*parent).height() * 0.5),
                  fold.position - QPointF(0, sign * fold_height * 0.5),
                  {}, 40.0, 2.5 * view.zoom, tinted(accent, 145), true);
        route(canvas, view, fold.position + QPointF(0, sign * fold_height * 0.5),
              child->position - QPointF(0, sign * cardBounds(*child).height() * 0.5),
              {}, 40.0, 2.5 * view.zoom, tinted(accent, 145), true);
    }

    for (const ProjectGraphNode &node : nodes)
    {
        for (const QString &parent_id : node.parents)
        {
            const ProjectGraphNode *parent = by_id.value(parent_id, nullptr);
            if (parent == nullptr)
                continue;
            const QPointF begin = parent->position
                + QPointF(0, sign * cardBounds(*parent).height() * 0.5);
            const QPointF end = node.position
                - QPointF(0, sign * cardBounds(node).height() * 0.5);
            route(canvas, view, begin, end, node.parent_routes.value(parent_id),
                  42.0, 3.0 * view.zoom, tinted(accent, 115));
            ++stats.ancestry_edges;
        }
        if (node.kind != ProjectGraphNode::Kind::Branch)
            continue;
        const ProjectGraphNode *parent = by_id.value(node.head_revision_id, nullptr);
        if (parent == nullptr || parent->kind != ProjectGraphNode::Kind::Revision)
            continue;
        route(canvas, view, parent->position + QPointF(0, sign * cardBounds(*parent).height() * 0.5),
              node.position - QPointF(0, sign * cardBounds(node).height() * 0.5),
              node.parent_routes.value(node.head_revision_id), 42.0, 2.5 * view.zoom,
              tinted(QColor(57, 184, 145), 190));
        ++stats.head_edges;
    }

    for (const ProjectGraphNode &node : nodes)
    {
        const QRectF bounds = screenRect(cardBounds(node), view);
        if (!bounds.intersects(view.viewport.adjusted(-12, -12, 12, 12)))
        {
            ++stats.culled_cards;
            continue;
        }
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

    for (const Fold &fold : folds)
    {
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
