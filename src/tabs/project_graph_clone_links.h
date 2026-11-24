#pragma once

#include "tabs/project_revision_graph.h"

#include <QColor>
#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>

// Project cards on different databases that carry the same DVMDB project
// UUIDv7 are clones of each other. The graph joins them with a dashed line.
// N copies are chained in reading order (left to right, then top to bottom),
// so they draw N-1 links instead of a crossing mesh. Both the QPainter and
// the bgfx paths use this one geometry, so they always agree.
namespace ProjectGraphCloneLinks
{
struct Link
{
    int first = -1;  // index of the earlier card in nodes
    int second = -1; // index of the later card
    // One cubic Bezier: begin, two control points, end (graph space).
    QVector<QPointF> route;
};

using Bounds = std::function<QRectF(const ProjectGraphNode &)>;

// Distinct from ownership (accent), ancestry and HEAD (green) edges and the
// personal database gold.
inline QColor color()
{
    return QColor(155, 110, 220);
}

// Side-to-side when the cards are apart horizontally, otherwise from the
// bottom of the upper card to the top of the lower one.
inline QVector<QPointF> route(const QRectF &first, const QRectF &second)
{
    const QRectF &left = first.center().x() <= second.center().x() ? first : second;
    const QRectF &right = &left == &first ? second : first;
    if (right.left() >= left.right())
    {
        const QPointF begin(left.right(), left.center().y());
        const QPointF end(right.left(), right.center().y());
        const double reach = std::max(30.0, (end.x() - begin.x()) * 0.4);
        return {begin, begin + QPointF(reach, 0.0), end - QPointF(reach, 0.0), end};
    }
    const QRectF &upper = first.center().y() <= second.center().y() ? first : second;
    const QRectF &lower = &upper == &first ? second : first;
    const QPointF begin(upper.center().x(), upper.bottom());
    const QPointF end(lower.center().x(), lower.top());
    const double reach = std::max(30.0, std::fabs(end.y() - begin.y()) * 0.4);
    return {begin, begin + QPointF(0.0, reach), end - QPointF(0.0, reach), end};
}

inline QVector<Link> compute(const QVector<ProjectGraphNode> &nodes, const Bounds &bounds)
{
    QHash<QString, QVector<int>> groups;
    QStringList order; // deterministic iteration, independent of hashing
    for (int index = 0; index < nodes.size(); ++index)
    {
        const ProjectGraphNode &node = nodes.at(index);
        if (node.kind != ProjectGraphNode::Kind::Project || node.clone_identity.isEmpty())
            continue;
        if (!groups.contains(node.clone_identity))
            order.append(node.clone_identity);
        groups[node.clone_identity].append(index);
    }
    QVector<Link> links;
    for (const QString &identity : order)
    {
        QVector<int> members = groups.value(identity);
        if (members.size() < 2)
            continue;
        std::sort(members.begin(), members.end(), [&nodes](int lhs, int rhs)
        {
            const QPointF &a = nodes.at(lhs).position;
            const QPointF &b = nodes.at(rhs).position;
            if (a.x() != b.x()) return a.x() < b.x();
            if (a.y() != b.y()) return a.y() < b.y();
            return lhs < rhs;
        });
        for (int position = 0; position + 1 < members.size(); ++position)
        {
            const int first = members.at(position);
            const int second = members.at(position + 1);
            links.append({first, second, route(bounds(nodes.at(first)), bounds(nodes.at(second)))});
        }
    }
    return links;
}
}
