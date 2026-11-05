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
// The HUD uses a point-sampled texture. If the on-screen text quad has
// significantly more physical pixels than the corresponding atlas region,
// glyph pixels are visibly enlarged (the original high-zoom regression).
bool textIsNotMagnified(const MapBgfxHudCanvas &canvas,
                        const MapBgfxHudTextCache &atlas)
{
    const QVector<MapBgfxHudVertex> &vertices = canvas.vertices();
    int text_quads = 0;
    for (qsizetype index = 0; index + 3 < vertices.size(); ++index)
    {
        const MapBgfxHudVertex &a = vertices.at(index);
        const MapBgfxHudVertex &b = vertices.at(index + 1);
        const MapBgfxHudVertex &c = vertices.at(index + 2);
        const MapBgfxHudVertex &d = vertices.at(index + 3);
        // Unlike the solid geometry, text quads have a nonzero UV extent.
        if (!(b.u > a.u && c.v > b.v && b.v == a.v && c.u == b.u
              && d.u == a.u && d.v == c.v))
            continue;
        const double atlas_pixels = double(b.u - a.u) * atlas.atlas().width();
        const double screen_pixels = double(b.x - a.x) * atlas.devicePixelRatio();
        if (!(screen_pixels > 0.0) || screen_pixels > atlas_pixels + 2.0)
            return false;
        ++text_quads;
    }
    return text_quads > 0;
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
    // Only six fields in this fixture have text: server title, project title,
    // revision title and branch, HEAD title and branch. The server/project
    // branch fields are intentionally empty and must not create atlas runs.
    if (!require(result.text_runs == 6, "GPU text atlas label count is incorrect")) return 1;
    if (!require(!canvas.vertices().isEmpty() && !canvas.indices().isEmpty()
                 && !canvas.overflowed(), "GPU geometry is empty or overflowed")) return 1;
    if (!require(textIsNotMagnified(canvas, atlas),
                 "base-zoom card labels magnify atlas glyph pixels")) return 1;

    // The graph may zoom to 3x. Card geometry still grows with the camera,
    // but raster text must receive higher-resolution glyphs rather than
    // enlarging the original 1x bitmap. Exercise zoom buckets and HiDPI.
    for (const double zoom : {1.01, 1.33, 1.75, 2.5, 3.0})
    {
        view.zoom = zoom;
        const ProjectGraphBgfxScene::Stats zoomed = ProjectGraphBgfxScene::build(
            &canvas, &atlas, nodes, {}, view);
        if (!require(zoomed.cards > 0 && zoomed.text_runs > 0,
                     "high-zoom graph cards or labels are missing")) return 1;
        if (!require(textIsNotMagnified(canvas, atlas),
                     "high-zoom graph text magnifies atlas glyph pixels")) return 1;
    }
    atlas.setDevicePixelRatio(2.0);
    view.zoom = 3.0;
    const ProjectGraphBgfxScene::Stats hidpi = ProjectGraphBgfxScene::build(
        &canvas, &atlas, nodes, {}, view);
    if (!require(hidpi.text_runs > 0 && textIsNotMagnified(canvas, atlas),
                 "HiDPI high-zoom graph text magnifies atlas pixels")) return 1;
    atlas.setDevicePixelRatio(1.0);
    view.zoom = 1.0;
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
    if (!require(outside.cards == 0 && outside.culled_cards == 4
                 && outside.text_runs == 0,
                 "offscreen cards should not emit GPU card geometry or text")) return 1;

    // Regression: invisible ancestry and structural links must NOT be
    // tessellated into the GPU staging buffers. Comparing against the same
    // offscreen cards without edges isolates the route draw-list overhead
    // from the background grid geometry.
    const qsizetype offscreen_vertices = canvas.vertices().size();
    const qsizetype offscreen_indices = canvas.indices().size();
    QVector<ProjectGraphNode> unlinked = nodes;
    for (ProjectGraphNode &node : unlinked)
    {
        node.container_id.clear();
        node.parents.clear();
        node.parent_routes.clear();
        node.head_revision_id.clear();
    }
    const ProjectGraphBgfxScene::Stats unlinked_stats = ProjectGraphBgfxScene::build(
        &canvas, &atlas, unlinked, {}, view);
    if (!require(unlinked_stats.cards == 0
                 && canvas.vertices().size() == offscreen_vertices
                 && canvas.indices().size() == offscreen_indices,
                 "offscreen edges generated GPU geometry")) return 1;

    // Endpoints can be offscreen while a long ancestry connection crosses
    // the viewport. Conservative cubic-hull culling must retain that line.
    view.zoom = 1.0;
    view.offset = QPointF(640, 130);
    ProjectGraphNode upper = revision;
    ProjectGraphNode lower = revision;
    upper.id = QStringLiteral("upper");
    lower.id = QStringLiteral("lower");
    upper.position = QPointF(0, -1000);
    lower.position = QPointF(0, 2000);
    lower.parents = {upper.id};
    const QVector<ProjectGraphNode> crossing{upper, lower};
    ProjectGraphBgfxScene::build(&canvas, &atlas, crossing, {}, view);
    const qsizetype crossing_vertices = canvas.vertices().size();
    const qsizetype crossing_indices = canvas.indices().size();
    lower.parents.clear();
    ProjectGraphBgfxScene::build(&canvas, &atlas,
                                 QVector<ProjectGraphNode>{upper, lower}, {}, view);
    if (!require(crossing_vertices > canvas.vertices().size()
                 && crossing_indices > canvas.indices().size(),
                 "edge crossing viewport was incorrectly culled")) return 1;

    // Fold markers can be emitted with the same project HEAD identity and
    // contain explicit links rather than shortcuts across hidden revisions.
    view.offset = QPointF(640, 130);
    view.zoom = 1.0;
    const QVector<ProjectGraphBgfxScene::Fold> folds{
        {QStringLiteral("revision"), QStringLiteral("branch"), QPointF(25, 385), 37}};
    const ProjectGraphBgfxScene::Stats folded = ProjectGraphBgfxScene::build(
        &canvas, &atlas, nodes, folds, view);
    if (!require(folded.fold_markers == 1 && folded.text_runs == 8,
                 "fold marker or its two labels not emitted")) return 1;
    if (!require(!atlas.atlas().isNull(), "GPU text atlas unavailable")) return 1;
    return 0;
}
