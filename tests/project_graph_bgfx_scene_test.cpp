#include "tabs/project_graph_bgfx_scene.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QVector>

#include <cmath>
#include <cstdio>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "project_graph_bgfx_scene: %s\n", message);
    return condition;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MapBgfxHudCanvas canvas;
    MapBgfxHudTextCache atlas(2048);
    ProjectGraphBgfxScene::View view;
    view.viewport = QRectF(0, 0, 1280, 800);
    view.offset = QPointF(640, 130);
    view.zoom = 1.0;
    view.palette = app.palette();
    view.font = app.font();
    view.selection_id = QStringLiteral("revision");

    ProjectGraphNode server;
    server.kind = ProjectGraphNode::Kind::Server;
    server.id = QStringLiteral("server");
    server.title = QStringLiteral("Personal DVMDB");
    server.personal_server = true;
    server.position = QPointF(0, 0);

    ProjectGraphNode project;
    project.kind = ProjectGraphNode::Kind::Project;
    project.id = QStringLiteral("project");
    project.title = QStringLiteral("Water Network");
    project.container_id = server.id;
    project.position = QPointF(0, 150);

    ProjectGraphNode revision;
    revision.kind = ProjectGraphNode::Kind::Revision;
    revision.id = QStringLiteral("revision");
    revision.title = QStringLiteral("Initial commit");
    revision.branch = QStringLiteral("main");
    revision.display_id = QStringLiteral("019aabbc-1111-7111-8111-123456789abc");
    revision.container_id = project.id;
    revision.position = QPointF(0, 310);

    ProjectGraphNode branch;
    branch.kind = ProjectGraphNode::Kind::Branch;
    branch.id = QStringLiteral("branch");
    branch.title = QStringLiteral("main HEAD");
    branch.branch = QStringLiteral("main");
    branch.head_revision_id = revision.id;
    branch.position = QPointF(40, 450);
    branch.parent_routes.insert(revision.id,
        {QPointF(0, 350), QPointF(-25, 390), QPointF(40, 400), QPointF(40, 415)});

    const QVector<ProjectGraphNode> nodes{server, project, revision, branch};
    const ProjectGraphBgfxScene::Stats result = ProjectGraphBgfxScene::build(
        &canvas, &atlas, nodes, {}, view);
    if (!require(result.cards == 4 && result.structural_edges == 2
                 && result.head_edges == 1 && result.ancestry_edges == 0,
                 "server/project/revision/HEAD geometry or links are missing")) return 1;
    if (!require(result.text_runs >= 7, "GPU text atlas missing card labels")) return 1;
    if (!require(!canvas.vertices().isEmpty() && !canvas.indices().isEmpty()
                 && !canvas.overflowed(), "GPU geometry is empty or overflowed")) return 1;
    for (const quint32 index : canvas.indices())
        if (!require(index < quint32(canvas.vertices().size()), "invalid GPU triangle index"))
            return 1;
    for (const MapBgfxHudVertex &vertex : canvas.vertices())
        if (!require(std::isfinite(vertex.x) && std::isfinite(vertex.y),
                     "invalid projected camera coordinate")) return 1;

    // The same input graph can be emitted with a flipped camera and without
    // rewriting graph topology. A selected revision remains a separate card.
    view.top_down = false;
    view.zoom = 0.5;
    const ProjectGraphBgfxScene::Stats reversed = ProjectGraphBgfxScene::build(
        &canvas, &atlas, nodes, {}, view);
    if (!require(reversed.cards == 4 && reversed.head_edges == 1,
                 "zoom or bottom-up projection lost a graph component")) return 1;

    // Viewport culling is performed before card drawing/text rasterization.
    view.offset = QPointF(-4000, -4000);
    const ProjectGraphBgfxScene::Stats outside = ProjectGraphBgfxScene::build(
        &canvas, &atlas, nodes, {}, view);
    if (!require(outside.cards == 0 && outside.culled_cards == 4,
                 "offscreen cards should not emit GPU card geometry")) return 1;

    // Fold markers can be emitted with the same project HEAD identity and
    // contain explicit links rather than shortcuts across hidden revisions.
    view.offset = QPointF(640, 130);
    view.zoom = 1.0;
    const QVector<ProjectGraphBgfxScene::Fold> folds{
        {QStringLiteral("revision"), QStringLiteral("branch"), QPointF(25, 385), 37}};
    const ProjectGraphBgfxScene::Stats folded = ProjectGraphBgfxScene::build(
        &canvas, &atlas, nodes, folds, view);
    if (!require(folded.fold_markers == 1, "fold marker not emitted")) return 1;
    if (!require(!atlas.atlas().isNull(), "GPU text atlas unavailable")) return 1;
    return 0;
}
