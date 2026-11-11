#pragma once

#include "map/bgfx/map_bgfx_glyph_atlas.h"
#include "map/bgfx/map_bgfx_hud_canvas.h"
#include "map/bgfx/map_bgfx_hud_text_cache.h"
#include "tabs/project_revision_graph.h"

#include <QFont>
#include <QHash>
#include <QPalette>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

// CPU staging for the Projects DAG's bgfx draw lists. This generates native
// GPU-ready triangles and glyph quads; it never initializes bgfx or owns a
// window. The native graph viewport submits the shape canvas with
// MapBgfxHudGpu and the text canvas with MapBgfxLabelGpu (fs_label_sdf)
// through the process-global frame coordinator, text on top.
//
// Text is signed-distance-field glyphs (MapBgfxGlyphAtlas): one atlas entry
// per glyph for every size, scaled by the camera zoom on the GPU, so card
// titles stay sharp at every zoom and device pixel ratio. (It used to be
// whole runs rasterized per quarter-zoom bucket and minified through a
// point-sampled atlas, which dropped and doubled glyph columns.)
//
// The widget's camera, graph layout, selection and input handling remain the
// source of truth. Geometry is generated in *viewport* coordinates, which is
// important: the HUD program already has a pixel-space orthographic camera.
class ProjectGraphBgfxScene final
{
public:
    struct Fold
    {
        QString visible_parent_id;
        QString visible_child_id;
        QPointF position;
        int revision_count = 0;
    };

    struct View
    {
        QPointF offset;
        QRectF viewport;
        double zoom = 1.0;
        bool top_down = true;
        QString selection_id;
        QPalette palette;
        QFont font;
    };

    struct Stats
    {
        int cards = 0;
        int structural_edges = 0;
        int ancestry_edges = 0;
        int head_edges = 0;
        int fold_markers = 0;
        int culled_cards = 0;
        int text_runs = 0;
        // Text runs waiting for glyphs (budget spent): draw again next frame.
        int text_deferred = 0;
        // Number of indexed objects inspected after spatial pruning.
        int candidate_items = 0;
    };

    // Retained CPU spatial index. Rebuild when the *displayed* graph/layout
    // changes, never just because the camera pans, zooms or selection changes.
    // Node data is implicitly shared with the canonical graph until changed;
    // stored edges use graph-space coordinates and pre-resolved endpoints.
    class Index final
    {
    public:
        void rebuild(const QVector<ProjectGraphNode> &nodes, const QVector<Fold> &folds,
                     bool top_down = true);
        void clear();
        int nodeCount() const { return this->nodes.size(); }

    private:
        friend class ProjectGraphBgfxScene;
        enum class Type { Structural, FoldEdge, Ancestry, Head, Card, FoldCard };
        struct Edge
        {
            QPointF begin;
            QPointF end;
            QVector<QPointF> controls;
            double bend = 0.0;
            bool dashed = false;
        };
        struct Item
        {
            QRectF bounds;
            Type type = Type::Card;
            int index = 0;
        };
        struct Branch
        {
            QRectF bounds;
            int left = -1;
            int right = -1;
            int begin = 0;
            int end = 0;
        };

        void appendEdge(const QPointF &begin, const QPointF &end,
                        const QVector<QPointF> &controls, double bend,
                        bool dashed, Type type);
        int buildBranch(int begin, int end);
        void visibleItems(const QRectF &bounds, QVector<int> *result) const;

        QVector<ProjectGraphNode> nodes;
        QVector<Fold> folds;
        QVector<Edge> edges;
        QVector<Item> items;
        QVector<int> order;
        QVector<Branch> branches;
        int root = -1;
        int structural_edges = 0;
        int ancestry_edges = 0;
        int head_edges = 0;
    };

    // Where text goes: its own canvas (cleared by build()) and glyph atlas.
    struct TextTarget
    {
        MapBgfxHudCanvas *canvas = nullptr;
        MapBgfxGlyphAtlas *glyphs = nullptr;
        // New glyph distance fields per build (only the first frames).
        int glyph_budget = 128;
    };

    // Fast path: retained graph-space index + viewport-specific GPU geometry.
    // atlas supplies the white texel of the solid shapes in canvas.
    static Stats build(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *atlas,
                       const TextTarget &text, const Index &index, const View &view);

    // Compatibility entry point for standalone one-shot callers. Production
    // and the performance benchmark must retain and reuse Index themselves.
    // This convenience overload intentionally builds a temporary index.
    static Stats build(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *atlas,
                       const TextTarget &text, const QVector<ProjectGraphNode> &nodes,
                       const QVector<Fold> &folds, const View &view);
};
