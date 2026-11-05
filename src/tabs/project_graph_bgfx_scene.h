#pragma once

#include "map/bgfx/map_bgfx_hud_canvas.h"
#include "map/bgfx/map_bgfx_hud_text_cache.h"
#include "tabs/project_revision_graph.h"

#include <QFont>
#include <QPalette>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

// CPU staging for the Projects DAG's bgfx draw list. This generates native
// GPU-ready triangles and atlas sprites; it never initializes bgfx or owns a
// window. The eventual graph surface will submit this canvas through the
// process-global frame coordinator, using MapBgfxHudGpu's existing shader.
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
    };

    // Build as many individual draw elements as the atlas and transient
    // limits permit. A future large-DAG pass will add persistent GPU buffers,
    // zoom-level LOD and incremental geometry updates. Do not use this
    // immediate-mode staging as a 100k-revision performance claim.
    static Stats build(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *atlas,
                       const QVector<ProjectGraphNode> &nodes,
                       const QVector<Fold> &folds, const View &view);
};
