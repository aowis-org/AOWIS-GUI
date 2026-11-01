#pragma once

#include "tabs/project_revision_graph.h"
#include "tabs/project_branch_visual_link.h"
#include "tabs/project_revision_card_geometry.h"
#include "graph/layered_dag_layout.h"

#include <QHash>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

// Deterministic compound layout for Database -> Project -> oldest revision
// -> newest revision (HEAD). Branches are references attached at their HEAD
// rank, never ancestors or descendants of a revision.
//
// Positions use the graph's canonical bottom-up coordinates. The renderer
// mirrors them for the default top-down presentation. This algorithm does not
// create ancestry edges: it only positions the real nodes supplied by the GUI.
namespace ProjectGraphPlacement
{
constexpr double card_width = ProjectRevisionCardGeometry::width;
constexpr double project_gap_x = 48.0;
constexpr double database_gap_x = 84.0;
constexpr double project_header_y = 142.0;
constexpr double first_graph_layer_y = 286.0;

struct ProjectBlock
{
    int node_index = -1;
    QVector<int> revision_indices;
    QVector<int> branch_indices;
    QVector<QPointF> revision_positions;
    QVector<QHash<QString, QVector<QPointF>>> revision_routes;
    QVector<QPointF> branch_positions;
    QVector<QHash<QString, QVector<QPointF>>> branch_routes;
    double width = card_width;
    double ancestry_center_x = card_width / 2.0;
};

struct DatabaseBlock
{
    int node_index = -1;
    QVector<ProjectBlock> projects;
    QVector<QPointF> project_offsets;
    double width = 252.0;
};

inline void layoutRevisions(ProjectBlock &block, const QVector<ProjectGraphNode> &nodes)
{
    // Branch refs are not revisions. Each uses one visual parent: the
    // persisted creation-source branch, or its HEAD revision as a fallback.
    // Only Kind::Revision.parents represent immutable revision ancestry.
    std::vector<AowisGraphLayout::LayeredVertex> vertices;
    const int revision_count = block.revision_indices.size();
    vertices.reserve(std::size_t(revision_count + block.branch_indices.size()));
    for (int index : block.revision_indices)
    {
        const ProjectGraphNode &revision = nodes.at(index);
        AowisGraphLayout::LayeredVertex vertex;
        vertex.id = revision.id.toStdString();
        for (const QString &parent : revision.parents)
            vertex.parents.push_back(parent.toStdString());
        vertex.width = card_width;
        vertex.height = ProjectRevisionCardGeometry::height(0);
        vertices.push_back(vertex);
    }
    for (int index : block.branch_indices)
    {
        const ProjectGraphNode &branch = nodes.at(index);
        AowisGraphLayout::LayeredVertex vertex;
        vertex.id = branch.id.toStdString();
        const QString parent_id = branch.head_revision_id;
        if (!parent_id.isEmpty())
            vertex.parents.push_back(parent_id.toStdString());
        vertex.width = 196.0;
        vertex.height = 70.0;
        vertices.push_back(vertex);
    }

    const AowisGraphLayout::LayeredDag dag =
        AowisGraphLayout::layoutLayeredDag(vertices);
    block.revision_positions.resize(revision_count);
    block.revision_routes.resize(revision_count);
    block.branch_positions.resize(block.branch_indices.size());
    block.branch_routes.resize(block.branch_indices.size());
    if (!dag.valid)
    {
        // Keep corrupt histories selectable, without inventing DAG ancestry.
        for (int i = 0; i < revision_count; ++i)
            block.revision_positions[i] = QPointF(card_width / 2.0,
                first_graph_layer_y + 140.0 * i);
        for (int i = 0; i < block.branch_indices.size(); ++i)
            block.branch_positions[i] = QPointF(card_width / 2.0,
                first_graph_layer_y + 140.0 * revision_count + 110.0 * i);
        block.width = card_width;
        block.ancestry_center_x = card_width / 2.0;
        return;
    }

    block.width = std::max(card_width, dag.width);
    block.ancestry_center_x = block.width / 2.0;
    const double horizontal_offset = (block.width - dag.width) / 2.0;
    for (int i = 0; i < revision_count + block.branch_indices.size(); ++i)
    {
        const std::pair<double, double> point = dag.positions.at(std::size_t(i));
        const QPointF position(horizontal_offset + point.first,
                                first_graph_layer_y + point.second);
        const bool is_branch = i >= revision_count;
        const int local_index = is_branch ? i - revision_count : i;
        if (is_branch) block.branch_positions[local_index] = position;
        else block.revision_positions[local_index] = position;
        QHash<QString, QVector<QPointF>> &routes = is_branch
            ? block.branch_routes[local_index] : block.revision_routes[local_index];
        for (const AowisGraphLayout::LayeredRoute &edge : dag.parent_routes.at(std::size_t(i)))
        {
            QVector<QPointF> curve;
            curve.reserve(int(edge.cubic.size()));
            for (const std::pair<double, double> &control : edge.cubic)
                curve.append(QPointF(horizontal_offset + control.first,
                                     first_graph_layer_y + control.second));
            routes.insert(QString::fromStdString(edge.parent), curve);
        }
    }
}

inline void place(QVector<ProjectGraphNode> &nodes, double /*viewport_aspect*/ = 1.7)
{
    QVector<DatabaseBlock> databases;
    QHash<QString, int> database_index;
    QHash<QString, QPair<int, int>> project_index;

    for (int index = 0; index < nodes.size(); ++index)
    {
        const ProjectGraphNode &node = nodes.at(index);
        if (node.kind == ProjectGraphNode::Kind::Server)
        {
            database_index.insert(node.id, databases.size());
            DatabaseBlock database;
            database.node_index = index;
            databases.append(database);
        }
    }
    for (int index = 0; index < nodes.size(); ++index)
    {
        const ProjectGraphNode &node = nodes.at(index);
        if (node.kind != ProjectGraphNode::Kind::Project)
            continue;
        const int db = database_index.value(node.container_id, -1);
        if (db < 0)
            continue;
        ProjectBlock project;
        project.node_index = index;
        project_index.insert(node.id, qMakePair(db, databases.at(db).projects.size()));
        databases[db].projects.append(project);
    }
    for (int index = 0; index < nodes.size(); ++index)
    {
        const ProjectGraphNode &node = nodes.at(index);
        if (node.kind != ProjectGraphNode::Kind::Revision
            && node.kind != ProjectGraphNode::Kind::Branch)
            continue;
        const QPair<int, int> match = project_index.value(node.project_group_id,
                                                          qMakePair(-1, -1));
        if (match.first >= 0)
        {
            ProjectBlock &block = databases[match.first].projects[match.second];
            if (node.kind == ProjectGraphNode::Kind::Revision)
                block.revision_indices.append(index);
            else block.branch_indices.append(index);
        }
    }

    // Stable project order across catalogue refreshes irrespective of backend
    // response ordering. All branch/revision indices have already been bound
    // to their owning ProjectBlock, so the blocks can now be reordered safely.
    for (DatabaseBlock &database : databases)
        std::sort(database.projects.begin(), database.projects.end(),
            [&](const ProjectBlock &lhs, const ProjectBlock &rhs)
            {
                return nodes.at(lhs.node_index).id < nodes.at(rhs.node_index).id;
            });

    // A database is a strict compound parent. Its projects are SIBLINGS in
    // one rank, with mutually disjoint x corridors for every descendant.
    // Never wrap projects of the same server into vertical shelves: a server
    // -> project edge would necessarily cross another project's subtree.
    // Wrapping happens only at the outer database-group boundary.
    for (DatabaseBlock &database : databases)
    {
        double cursor_x = 0.0;
        for (ProjectBlock &project : database.projects)
        {
            layoutRevisions(project, nodes);
            if (cursor_x > 0.0)
                cursor_x += project_gap_x;
            database.project_offsets.append(QPointF(cursor_x, 0.0));
            cursor_x += project.width;
        }
        database.width = std::max(252.0, cursor_x);
    }

    if (databases.isEmpty())
        return;

    // All database roots share one layer. Give each complete database group
    // its own horizontal corridor, then grow every project/revision subtree
    // downward from that common baseline. Never wrap database groups into
    // additional rows: a deep history must not push another server card down.
    double total_width = 0.0;
    for (const DatabaseBlock &database : databases)
        total_width += database.width;
    total_width += database_gap_x * (databases.size() - 1);

    double group_x = -total_width / 2.0;
    for (const DatabaseBlock &database : databases)
    {
        // Center the database over its project revision corridors.
        double left_project_x = 0.0;
        double right_project_x = 0.0;
        for (int pi = 0; pi < database.projects.size(); ++pi)
        {
            const double x = database.project_offsets.at(pi).x()
                + (database.projects.size() == 1
                    ? (database.width - database.projects.at(pi).width) / 2.0 : 0.0)
                + database.projects.at(pi).ancestry_center_x;
            if (pi == 0) left_project_x = x;
            right_project_x = x;
        }
        const double server_x = database.projects.isEmpty()
            ? database.width / 2.0 : (left_project_x + right_project_x) / 2.0;
        nodes[database.node_index].position = QPointF(group_x + server_x, 0.0);
        for (int pi = 0; pi < database.projects.size(); ++pi)
        {
            const ProjectBlock &project = database.projects.at(pi);
            const QPointF offset = database.project_offsets.at(pi);
            // Centre a single narrow project beneath its wider server card.
            const double project_x = group_x + offset.x()
                + (database.projects.size() == 1
                    ? (database.width - project.width) / 2.0 : 0.0);
            nodes[project.node_index].position = QPointF(
                project_x + project.ancestry_center_x, -project_header_y);
            for (int ri = 0; ri < project.revision_indices.size(); ++ri)
            {
                const QPointF position = project.revision_positions.at(ri);
                ProjectGraphNode &revision = nodes[project.revision_indices.at(ri)];
                revision.position = QPointF(project_x + position.x(), -position.y());
                revision.parent_routes.clear();
                if (ri < project.revision_routes.size())
                {
                    const QHash<QString, QVector<QPointF>> &routes = project.revision_routes.at(ri);
                    for (auto edge = routes.constBegin(); edge != routes.constEnd(); ++edge)
                    {
                        QVector<QPointF> shifted;
                        for (const QPointF &point : edge.value())
                            shifted.append(QPointF(project_x + point.x(), -point.y()));
                        revision.parent_routes.insert(edge.key(), shifted);
                    }
                }
            }
            for (int bi = 0; bi < project.branch_indices.size(); ++bi)
            {
                ProjectGraphNode &branch = nodes[project.branch_indices.at(bi)];
                const QPointF position = project.branch_positions.at(bi);
                branch.position = QPointF(project_x + position.x(), -position.y());
                branch.parent_routes.clear();
                if (bi < project.branch_routes.size())
                {
                    const QHash<QString, QVector<QPointF>> &routes = project.branch_routes.at(bi);
                    for (auto edge = routes.constBegin(); edge != routes.constEnd(); ++edge)
                    {
                        QVector<QPointF> shifted;
                        for (const QPointF &point : edge.value())
                            shifted.append(QPointF(project_x + point.x(), -point.y()));
                        branch.parent_routes.insert(edge.key(), shifted);
                    }
                }
            }
        }
        group_x += database.width + database_gap_x;
    }
}
}
