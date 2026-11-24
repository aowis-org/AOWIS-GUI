#pragma once

#include "tabs/project_revision_graph.h"

#include <QHash>
#include <QSet>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>

// Project cards on different databases that carry the same DVMDB project
// UUIDv7 are clones of each other. The graph joins them with a dashed line in
// the project card colour. N copies are chained in reading order (left to
// right, then top to bottom), so they draw N-1 links instead of a crossing
// mesh. A second, dotted line in the revision card colour joins the last
// commit two clones both have, so it shows how far they agree. Both the
// QPainter and the bgfx paths use this one geometry, so they always agree.
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

// The revision UUIDv7 of a revision card ("<project>|<revision>" in a
// database graph, the bare ID in the preview).
inline QString revisionIdentity(const ProjectGraphNode &node)
{
    return node.display_id.isEmpty() ? node.id : node.display_id;
}

// Marks, for every pair of clones, the newest revisions both copies have: a
// shared revision none of whose children (by immutable parent links) is
// shared too. Linear history gives one such revision, the last commit both
// databases agree on; diverged or extra branches can give one per line of
// history. Clears previous marks first. Call on the complete node set,
// before folding.
inline void markSyncFrontiers(QVector<ProjectGraphNode> &nodes)
{
    for (ProjectGraphNode &node : nodes) node.clone_sync_keys.clear();
    QHash<QString, QString> identity_by_id; // any node ID -> revision identity
    QHash<QString, QHash<QString, int>> revisions_by_project; // project card -> revision -> node index
    for (int index = 0; index < nodes.size(); ++index)
    {
        const ProjectGraphNode &node = nodes.at(index);
        if (node.kind != ProjectGraphNode::Kind::Revision || node.project_group_id.isEmpty()) continue;
        identity_by_id.insert(node.id, revisionIdentity(node));
        revisions_by_project[node.project_group_id].insert(revisionIdentity(node), index);
    }
    QHash<QString, QStringList> copies; // clone identity -> project card IDs
    QStringList order;
    for (const ProjectGraphNode &node : nodes)
    {
        if (node.kind != ProjectGraphNode::Kind::Project || node.clone_identity.isEmpty()) continue;
        if (!copies.contains(node.clone_identity)) order.append(node.clone_identity);
        copies[node.clone_identity].append(node.id);
    }
    for (const QString &identity : order)
    {
        QStringList members = copies.value(identity);
        members.sort();
        for (int a = 0; a < members.size(); ++a)
            for (int b = a + 1; b < members.size(); ++b)
            {
                const QHash<QString, int> first = revisions_by_project.value(members.at(a));
                const QHash<QString, int> second = revisions_by_project.value(members.at(b));
                QSet<QString> common;
                for (QHash<QString, int>::const_iterator it = first.cbegin(); it != first.cend(); ++it)
                    if (second.contains(it.key())) common.insert(it.key());
                QSet<QString> has_common_child;
                for (const QString &revision : common)
                    for (const QString &parent : nodes.at(first.value(revision)).parents)
                    {
                        const QString parent_identity = identity_by_id.value(parent, parent);
                        if (common.contains(parent_identity)) has_common_child.insert(parent_identity);
                    }
                QStringList frontier;
                for (const QString &revision : common)
                    if (!has_common_child.contains(revision)) frontier.append(revision);
                frontier.sort();
                for (const QString &revision : frontier)
                {
                    const QString key = members.at(a) + QLatin1Char('\n') + members.at(b)
                        + QLatin1Char('\n') + revision;
                    nodes[first.value(revision)].clone_sync_keys.append(key);
                    nodes[second.value(revision)].clone_sync_keys.append(key);
                }
            }
    }
}

// Dotted links between the two revision cards that share a sync key; a key
// whose cards are not both present (e.g. folded away) draws nothing.
inline QVector<Link> computeSync(const QVector<ProjectGraphNode> &nodes, const Bounds &bounds)
{
    QHash<QString, QVector<int>> cards;
    QStringList order;
    for (int index = 0; index < nodes.size(); ++index)
    {
        const ProjectGraphNode &node = nodes.at(index);
        if (node.kind != ProjectGraphNode::Kind::Revision) continue;
        for (const QString &key : node.clone_sync_keys)
        {
            if (!cards.contains(key)) order.append(key);
            cards[key].append(index);
        }
    }
    order.sort();
    QVector<Link> links;
    for (const QString &key : order)
    {
        const QVector<int> pair = cards.value(key);
        if (pair.size() != 2) continue;
        links.append({pair.at(0), pair.at(1), route(bounds(nodes.at(pair.at(0))), bounds(nodes.at(pair.at(1))))});
    }
    return links;
}
}
