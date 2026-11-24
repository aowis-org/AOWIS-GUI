#include "tabs/project_graph_bgfx_scene.h"
#include "tabs/project_graph_clone_links.h"

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
// Width of the first glyph quad of the text canvas (0: none). Text is
// signed-distance glyphs scaled by the camera zoom, so a glyph's quad must
// grow exactly with the zoom (no rasterization buckets, no point-sampled
// minification).
double firstGlyphWidth(const MapBgfxHudCanvas &text_canvas)
{
    const QVector<MapBgfxHudVertex> &vertices = text_canvas.vertices();
    if (vertices.size() < 4)
        return 0.0;
    return double(vertices.at(1).x - vertices.at(0).x);
}

bool glyphQuadsValid(const MapBgfxHudCanvas &text_canvas)
{
    const QVector<MapBgfxHudVertex> &vertices = text_canvas.vertices();
    if (vertices.isEmpty() || vertices.size() % 4 != 0)
        return false;
    for (const MapBgfxHudVertex &vertex : vertices)
    {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || vertex.u < 0.0f
            || vertex.u > 1.0f || vertex.v < 0.0f || vertex.v > 1.0f)
            return false;
    }
    return true;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MapBgfxHudCanvas canvas;
    MapBgfxHudTextCache atlas(2048);
    MapBgfxHudCanvas text_canvas;
    MapBgfxGlyphAtlas glyphs(1024);
    ProjectGraphBgfxScene::TextTarget text_target;
    text_target.canvas = &text_canvas;
    text_target.glyphs = &glyphs;
    // Every glyph of the fixture in the first build.
    text_target.glyph_budget = 1000;
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

    const QVector<ProjectGraphNode> no_badge_nodes{server, project, revision, branch};
    ProjectGraphBgfxScene::build(&canvas, &atlas, text_target, no_badge_nodes, {}, view);
    const qsizetype plain_card_vertices = canvas.vertices().size();
    server.availability_color = QColor(QStringLiteral("#339950"));
    server.availability_status = QStringLiteral("Available");
    const QVector<ProjectGraphNode> nodes{server, project, revision, branch};
    const ProjectGraphBgfxScene::Stats result = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, nodes, {}, view);
    if (!require(result.cards == 4 && result.structural_edges == 2
                 && result.head_edges == 1 && result.ancestry_edges == 0,
                 "server/project/revision/HEAD geometry or links are missing")) return 1;
    if (!require(canvas.vertices().size() > plain_card_vertices,
                 "bgfx graph server availability dot has no rendered geometry")) return 1;
    // Only six fields in this fixture have text: server title, project title,
    // revision title and branch, HEAD title and branch. The server/project
    // branch fields are intentionally empty and must not create atlas runs.
    if (!require(result.text_runs == 6, "GPU text atlas label count is incorrect")) return 1;
    if (!require(!canvas.vertices().isEmpty() && !canvas.indices().isEmpty()
                 && !canvas.overflowed(), "GPU geometry is empty or overflowed")) return 1;
    if (!require(result.text_deferred == 0 && glyphQuadsValid(text_canvas),
                 "card labels are not glyph quads inside the atlas")) return 1;
    // Zoom scaling on one card (the others may be culled at high zoom).
    const QVector<ProjectGraphNode> server_only{server};
    ProjectGraphBgfxScene::build(&canvas, &atlas, text_target, server_only, {}, view);
    const double base_glyph_width = firstGlyphWidth(text_canvas);
    if (!require(base_glyph_width > 0.0, "card labels have no glyph quads")) return 1;

    // The graph zooms from below 1x to 3x. Text scales with the cards on the
    // GPU: the same glyph entries, quads exactly zoom times as large, and no
    // new distance fields for a zoom.
    const int glyph_entries = glyphs.glyphCount();
    for (const double zoom : {0.35, 0.8, 1.01, 1.33, 1.75, 2.5, 3.0})
    {
        view.zoom = zoom;
        const ProjectGraphBgfxScene::Stats zoomed = ProjectGraphBgfxScene::build(
            &canvas, &atlas, text_target, nodes, {}, view);
        if (!require(zoomed.cards > 0 && zoomed.text_runs > 0 && zoomed.text_deferred == 0,
                     "zoomed graph cards or labels are missing")) return 1;
        ProjectGraphBgfxScene::build(&canvas, &atlas, text_target, server_only, {}, view);
        if (!require(glyphQuadsValid(text_canvas)
                         && std::fabs(firstGlyphWidth(text_canvas) - zoom * base_glyph_width)
                                < 0.01 * zoom * base_glyph_width + 1e-3,
                     "graph text does not scale exactly with the zoom")) return 1;
    }
    if (!require(glyphs.glyphCount() == glyph_entries,
                 "zooming created new glyph entries")) return 1;
    view.zoom = 1.0;

    // A small budget defers the remaining text instead of stalling a frame;
    // later frames complete it.
    MapBgfxGlyphAtlas cold(1024);
    ProjectGraphBgfxScene::TextTarget budgeted = text_target;
    budgeted.glyphs = &cold;
    budgeted.glyph_budget = 4;
    const ProjectGraphBgfxScene::Stats first = ProjectGraphBgfxScene::build(
        &canvas, &atlas, budgeted, nodes, {}, view);
    if (!require(first.text_deferred > 0 && cold.glyphCount() <= 4,
                 "glyph budget does not defer text")) return 1;
    int frames = 1;
    ProjectGraphBgfxScene::Stats later = first;
    while (later.text_deferred > 0 && frames < 100)
    {
        later = ProjectGraphBgfxScene::build(&canvas, &atlas, budgeted, nodes, {}, view);
        ++frames;
    }
    if (!require(later.text_deferred == 0 && later.text_runs == result.text_runs,
                 "deferred graph text never completes")) return 1;
    for (const quint32 index : canvas.indices())
        if (!require(index < quint32(canvas.vertices().size()), "invalid GPU triangle index"))
            return 1;
    for (const MapBgfxHudVertex &vertex : canvas.vertices())
        if (!require(std::isfinite(vertex.x) && std::isfinite(vertex.y),
                     "invalid projected camera coordinate")) return 1;

    // The scene receives already laid-out horizontal cards. Verify that
    // structural and HEAD links survive both orientation and viewport zoom.
    QVector<ProjectGraphNode> horizontal = nodes;
    for (ProjectGraphNode &card : horizontal)
    {
        card.position = QPointF(card.position.y() * 2.0, card.position.x());
        card.parent_routes.clear();
    }
    view.top_down = false;
    view.zoom = 0.5;
    const ProjectGraphBgfxScene::Stats horizontal_stats = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, horizontal, {}, view);
    if (!require(horizontal_stats.cards == 4 && horizontal_stats.head_edges == 1
                 && horizontal_stats.structural_edges == 2 && glyphQuadsValid(text_canvas),
                 "horizontal projection lost cards, edges or upright labels")) return 1;
    // A narrow viewport between cards should still contain the horizontal
    // link even though both card endpoints are offscreen.
    ProjectGraphBgfxScene::View between = view;
    between.zoom = 1.0;
    between.viewport = QRectF(0, 0, 26, 100);
    between.offset = QPointF(-145, 50);
    const ProjectGraphBgfxScene::Stats with_link = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, horizontal, {}, between);
    const qsizetype link_vertices = canvas.vertices().size();
    horizontal[1].container_id.clear();
    const ProjectGraphBgfxScene::Stats without_link = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, horizontal, {}, between);
    if (!require(with_link.cards == 0 && without_link.cards == 0
                 && link_vertices > canvas.vertices().size(),
                 "horizontal spatial index culled a visible edge between offscreen cards")) return 1;
    view.top_down = true;

    // Viewport culling is performed before card drawing/text rasterization.
    view.offset = QPointF(-4000, -4000);
    const ProjectGraphBgfxScene::Stats outside = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, nodes, {}, view);
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
        &canvas, &atlas, text_target, unlinked, {}, view);
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
    ProjectGraphBgfxScene::build(&canvas, &atlas, text_target, crossing, {}, view);
    const qsizetype crossing_vertices = canvas.vertices().size();
    const qsizetype crossing_indices = canvas.indices().size();
    lower.parents.clear();
    ProjectGraphBgfxScene::build(&canvas, &atlas, text_target,
                                 QVector<ProjectGraphNode>{upper, lower}, {}, view);
    if (!require(crossing_vertices > canvas.vertices().size()
                 && crossing_indices > canvas.indices().size(),
                 "edge crossing viewport was incorrectly culled")) return 1;

    // Production uses a retained spatial index. A long chain must query
    // only nearby cards/routes per pan frame (not scan the full DAG). The
    // ancestry total still describes the complete indexed graph.
    QVector<ProjectGraphNode> long_chain;
    long_chain.reserve(6000);
    for (int i = 0; i < 6000; ++i)
    {
        ProjectGraphNode item;
        item.kind = ProjectGraphNode::Kind::Revision;
        item.id = QStringLiteral("n%1").arg(i);
        item.title = QStringLiteral("Revision %1").arg(i);
        item.position = QPointF(0, i * 155.0);
        if (i > 0)
            item.parents.append(QStringLiteral("n%1").arg(i - 1));
        long_chain.append(item);
    }
    ProjectGraphBgfxScene::Index spatial_index;
    spatial_index.rebuild(long_chain, {}, true);
    view.zoom = 1.0;
    view.top_down = true;
    view.offset = QPointF(640, 400 - 3000 * 155.0);
    const ProjectGraphBgfxScene::Stats indexed = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, spatial_index, view);
    if (!require(indexed.cards > 0 && indexed.cards < 12
                 && indexed.candidate_items < 80
                 && indexed.culled_cards == long_chain.size() - indexed.cards
                 && indexed.ancestry_edges == 5999,
                 "spatial query traversed offscreen history or lost ancestry")) return 1;
    view.offset += QPointF(0, -1550);
    const ProjectGraphBgfxScene::Stats panned = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, spatial_index, view);
    if (!require(panned.cards > 0 && panned.candidate_items < 80
                 && panned.ancestry_edges == indexed.ancestry_edges,
                 "panning must reuse the retained graph index")) return 1;

    // After a real graph/layout replacement, the previous index must not
    // continue showing old cards or stale edges.
    spatial_index.rebuild(nodes, {}, true);
    view.offset = QPointF(640, 130);
    const ProjectGraphBgfxScene::Stats replaced = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, spatial_index, view);
    if (!require(spatial_index.nodeCount() == nodes.size()
                 && replaced.cards == 4 && replaced.ancestry_edges == 0,
                 "rebuild did not replace stale spatial/indexed geometry")) return 1;

    // A long line whose endpoints are outside the view must remain indexed.
    // Otherwise a naive endpoint-only query could silently hide merge routes.
    spatial_index.rebuild(crossing, {}, true);
    const ProjectGraphBgfxScene::Stats indexed_crossing = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, spatial_index, view);
    if (!require(indexed_crossing.cards == 0
                 && indexed_crossing.candidate_items >= 1,
                 "indexed edge with offscreen endpoints was lost")) return 1;

    // Fold markers can be emitted with the same project HEAD identity and
    // contain explicit links rather than shortcuts across hidden revisions.
    view.offset = QPointF(640, 130);
    view.zoom = 1.0;
    const QVector<ProjectGraphBgfxScene::Fold> folds{
        {QStringLiteral("revision"), QStringLiteral("branch"), QPointF(25, 385), 37}};
    const ProjectGraphBgfxScene::Stats folded = ProjectGraphBgfxScene::build(
        &canvas, &atlas, text_target, nodes, folds, view);
    if (!require(folded.fold_markers == 1 && folded.text_runs == 8,
                 "fold marker or its two labels not emitted")) return 1;
    if (!require(!glyphs.atlas().isNull(), "GPU glyph atlas unavailable")) return 1;

    // Clones: project cards with the same DVMDB project UUIDv7 on two
    // databases get one dashed link below the ownership edges; a third
    // unrelated project gets none.
    {
        ProjectGraphNode other_server = server;
        other_server.id = QStringLiteral("other-server");
        other_server.personal_server = false;
        other_server.position = QPointF(420, 0);
        ProjectGraphNode original = project;
        original.clone_identity = QStringLiteral("019aabbc-2222-7222-8222-123456789abc");
        ProjectGraphNode clone = project;
        clone.id = QStringLiteral("clone");
        clone.container_id = other_server.id;
        clone.position = QPointF(420, 150);
        clone.clone_identity = original.clone_identity;
        ProjectGraphNode unrelated = project;
        unrelated.id = QStringLiteral("unrelated");
        unrelated.container_id = other_server.id;
        unrelated.position = QPointF(840, 150);
        unrelated.clone_identity = QStringLiteral("019aabbc-3333-7333-8333-123456789abc");
        const QVector<ProjectGraphNode> cloned{server, other_server, original, clone, unrelated};
        view.offset = QPointF(200, 130);
        view.zoom = 1.0;
        const ProjectGraphBgfxScene::Stats without_links = ProjectGraphBgfxScene::build(
            &canvas, &atlas, text_target, QVector<ProjectGraphNode>{server, other_server, project, clone, unrelated},
            {}, view);
        const qsizetype unlinked_vertices = canvas.vertices().size();
        const ProjectGraphBgfxScene::Stats linked = ProjectGraphBgfxScene::build(
            &canvas, &atlas, text_target, cloned, {}, view);
        if (!require(without_links.clone_edges == 0 && linked.clone_edges == 1
                         && linked.structural_edges == 3 && linked.cards == 5,
                     "cloned project cards are not joined by exactly one link")) return 1;
        if (!require(canvas.vertices().size() > unlinked_vertices,
                     "the clone link has no rendered geometry")) return 1;
        // The last commit both copies have gets a dotted line of its own.
        ProjectGraphNode original_revision;
        original_revision.kind = ProjectGraphNode::Kind::Revision;
        original_revision.id = QStringLiteral("p-original|r1");
        original_revision.display_id = QStringLiteral("r1");
        original_revision.project_group_id = original.id;
        original_revision.position = QPointF(0, 300);
        ProjectGraphNode clone_revision = original_revision;
        clone_revision.id = QStringLiteral("p-clone|r1");
        clone_revision.project_group_id = clone.id;
        clone_revision.position = QPointF(420, 300);
        QVector<ProjectGraphNode> synced{server, other_server, original, clone, unrelated,
                                         original_revision, clone_revision};
        ProjectGraphCloneLinks::markSyncFrontiers(synced);
        const qsizetype unsynced_vertices = [&] {
            QVector<ProjectGraphNode> plain = synced;
            for (ProjectGraphNode &node : plain) node.clone_sync_keys.clear();
            ProjectGraphBgfxScene::build(&canvas, &atlas, text_target, plain, {}, view);
            return canvas.vertices().size();
        }();
        const ProjectGraphBgfxScene::Stats sync = ProjectGraphBgfxScene::build(
            &canvas, &atlas, text_target, synced, {}, view);
        if (!require(sync.clone_edges == 1 && sync.clone_sync_edges == 1,
                     "the last common commit of two clones has no dotted line")) return 1;
        if (!require(canvas.vertices().size() > unsynced_vertices,
                     "the dotted sync line has no rendered geometry")) return 1;
        // A dashed link stays indexed when only its middle is in view.
        ProjectGraphBgfxScene::Index clone_index;
        clone_index.rebuild(cloned, {}, true);
        view.viewport = QRectF(0, 0, 120, 120);
        view.offset = QPointF(-150, -90);
        const ProjectGraphBgfxScene::Stats middle = ProjectGraphBgfxScene::build(
            &canvas, &atlas, text_target, clone_index, view);
        if (!require(middle.clone_edges == 1 && middle.candidate_items >= 1,
                     "a clone link between offscreen cards was culled")) return 1;
    }
    return 0;
}
