#include "tabs/project_revision_graph.h"
#include "projects/dvmdb_identity.h"
#include "tabs/project_revision_card_geometry.h"
#include "tabs/project_branch_visual_link.h"

#include "config/gui_configuration.h"
#include "config/shortcut_registry.h"

#include <QApplication>
#include <QFocusEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <QtMath>
#include <cmath>
#include <algorithm>
#include <functional>
#include <limits>
#include <QHash>
#include <QMap>

namespace
{
constexpr double project_node_height = 68.0;
constexpr double server_node_width = 252.0;
constexpr double server_node_height = 76.0;
constexpr int frame_interval_ms = 16;
// Let the nested dock host and splitter settle before the very first fit.
constexpr int initial_fit_settle_ms = 50;
constexpr double pan_speed = 900.0;
constexpr double fast_pan_multiplier = 2.0;
constexpr double pan_acceleration = 2800.0;
constexpr double fast_acceleration_multiplier = 1.75;
constexpr double pan_deceleration = 3600.0;
constexpr double mouse_pan_deceleration = 2600.0;
constexpr double mouse_maximum_speed = 2400.0;
constexpr double mouse_inertia_minimum_speed = 70.0;
constexpr int release_timeout_ms = 100;
constexpr double mouse_velocity_smoothing = 0.65;
constexpr double velocity_stop_threshold = 0.5;
constexpr double keyboard_zoom_octaves_per_second = 1.5;
constexpr double min_zoom = 0.25;
constexpr double max_zoom = 3.0;
constexpr double camera_margin_px = 110.0;

qreal vectorLength(const QPointF &point)
{
    return std::hypot(point.x(), point.y());
}

QPointF moveTowards(const QPointF &current, const QPointF &target, double maximum_delta)
{
    const QPointF delta = target - current;
    const double length = vectorLength(delta);
    if (length <= maximum_delta || qFuzzyIsNull(length))
        return target;
    return current + delta / length * maximum_delta;
}
QColor tint(const QColor &color, int alpha)
{
    QColor result = color;
    result.setAlpha(alpha);
    return result;
}
}

ProjectRevisionGraph::ProjectRevisionGraph(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("projectRevisionGraph"));
    setMinimumSize(340, 250);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::OpenHandCursor);
    this->zoom_motion.setLimits(std::log2(min_zoom), std::log2(max_zoom));
    this->navigation_timer.setInterval(frame_interval_ms);
    this->navigation_timer.setTimerType(Qt::PreciseTimer);
    connect(&this->navigation_timer, &QTimer::timeout, this, &ProjectRevisionGraph::updateNavigation);
    this->initial_fit_timer.setSingleShot(true);
    this->initial_fit_timer.setInterval(initial_fit_settle_ms);
    connect(&this->initial_fit_timer, &QTimer::timeout, this, [this]()
    {
        if (this->pending_fit && isVisible())
        {
            this->automatic_fit = true;
            fitGraph();
            this->automatic_fit = false;
        }
    });
    // Navigation bindings are shared with the map, but no overlay or
    // hover tooltip is drawn over the revision graph.
    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged, this,
            [this](GuiShortcutId id)
    {
        // A rebinding can happen while the graph is focused and a key is held.
        // Discard the old binding's hold so it cannot become stuck.
        switch (id)
        {
            case GuiShortcutId::MapPanLeft:
            case GuiShortcutId::MapPanRight:
            case GuiShortcutId::MapPanUp:
            case GuiShortcutId::MapPanDown:
            case GuiShortcutId::MapZoomIn:
            case GuiShortcutId::MapZoomOut:
                stopNavigation();
                break;
            default:
                break;
        }
    });
    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutsReset, this,
            [this]()
    {
        stopNavigation();
    });
}

void ProjectRevisionGraph::rebuildDisplayNodes()
{
    this->nodes.clear();
    this->omissions.clear();
    QHash<QString, const ProjectGraphNode *> by_id;
    for (const ProjectGraphNode &node : this->source_nodes)
        by_id.insert(node.id, &node);

    QSet<QString> visible;
    if (this->show_full_tree)
    {
        for (const ProjectGraphNode &node : this->source_nodes)
            visible.insert(node.id);
    }
    else
    {
        // Always preserve the heads and their real branch junctions. For
        // example, two branch HEADs must both remain connected to their
        // common ancestor, even when the history between them is folded.
        QHash<QString, int> descendant_head_counts;
        QHash<QString, QStringList> children_by_id;
        for (const ProjectGraphNode &node : this->source_nodes)
            for (const QString &parent_id : node.parents)
                children_by_id[parent_id].append(node.id);

        for (const ProjectGraphNode &head : this->source_nodes)
        {
            if (head.kind != ProjectGraphNode::Kind::Revision || head.heads.isEmpty())
                continue;
            QSet<QString> visited;
            std::function<void(const QString &)> visit;
            visit = [&](const QString &id)
            {
                if (visited.contains(id) || !by_id.contains(id))
                    return;
                visited.insert(id);
                descendant_head_counts[id] += head.heads.size();
                for (const QString &parent : by_id.value(id)->parents)
                    visit(parent);
            };
            visit(head.id);
        }

        for (const ProjectGraphNode &node : this->source_nodes)
        {
            if (node.kind != ProjectGraphNode::Kind::Revision)
            {
                visible.insert(node.id);
                continue;
            }
            if (!node.heads.isEmpty() || this->expanded_revisions.contains(node.id))
                visible.insert(node.id);
            const int count = descendant_head_counts.value(node.id);
            if (count < 2)
                continue;
            // Retain the nearest shared ancestor at each branch junction,
            // rather than showing an entire shared trunk. A branch point
            // cannot have a descendant with the same set of HEADs.
            bool deeper_common_ancestor = false;
            for (const QString &child_id : children_by_id.value(node.id))
                if (descendant_head_counts.value(child_id) == count)
                {
                    deeper_common_ancestor = true;
                    break;
                }
            if (!deeper_common_ancestor)
                visible.insert(node.id);
        }
    }

    for (const ProjectGraphNode &node : this->source_nodes)
        if (visible.contains(node.id))
            this->nodes.append(node);

    if (!this->show_full_tree)
    {
        for (const ProjectGraphNode &node : this->nodes)
        {
            if (node.kind != ProjectGraphNode::Kind::Revision)
                continue;
            for (const QString &parent_id : node.parents)
            {
                if (visible.contains(parent_id) || !by_id.contains(parent_id))
                    continue;

                QSet<QString> hidden;
                QSet<QString> visible_ancestors;
                std::function<void(const QString &)> walk;
                walk = [&](const QString &id)
                {
                    if (visible.contains(id))
                    {
                        visible_ancestors.insert(id);
                        return;
                    }
                    if (hidden.contains(id))
                        return;
                    const ProjectGraphNode *ancestor = by_id.value(id, nullptr);
                    if (ancestor == nullptr)
                        return;
                    hidden.insert(id);
                    for (const QString &parent : ancestor->parents)
                        walk(parent);
                };
                walk(parent_id);

                if (visible_ancestors.isEmpty())
                {
                    // A single-head project can still summarize old history
                    // without manufacturing a non-existent branch junction.
                    Omission omission;
                    omission.hidden_parent_id = parent_id;
                    omission.visible_child_id = node.id;
                    // Place the fold toward its hidden older parent. This
                    // uses actual source coordinates, regardless of the view
                    // orientation selected by the user.
                    const ProjectGraphNode *hidden_parent = by_id.value(parent_id, nullptr);
                    const double direction = hidden_parent != nullptr
                        && hidden_parent->position.y() > node.position.y() ? 1.0 : -1.0;
                    omission.position = node.position + QPointF(0.0, 80.0 * direction);
                    omission.revision_count = hidden.size();
                    this->omissions.append(omission);
                }
                else
                {
                    for (const QString &ancestor_id : visible_ancestors)
                    {
                        const ProjectGraphNode *ancestor = by_id.value(ancestor_id);
                        if (ancestor == nullptr)
                            continue;
                        Omission omission;
                        omission.hidden_parent_id = parent_id;
                        omission.visible_parent_id = ancestor_id;
                        omission.visible_child_id = node.id;
                        omission.position = (node.position + ancestor->position) / 2.0;
                        omission.revision_count = hidden.size();
                        this->omissions.append(omission);
                    }
                }
            }
        }

        // The full-DAG layered routes refer to the uncompressed scene.
        // Folded views reroute visible ancestry using their own geometry.
        for (ProjectGraphNode &node : this->nodes)
            node.parent_routes.clear();

        // Compress hidden levels while respecting variable card heights:
        // several branch HEAD labels may make a revision taller than 155px.
        QVector<double> levels;
        QMap<double, double> level_heights;
        for (const ProjectGraphNode &node : this->nodes)
        {
            levels.append(node.position.y());
            level_heights[node.position.y()] = qMax(level_heights.value(node.position.y(), 0.0),
                                                    nodeRect(node).height());
        }
        for (const Omission &omission : this->omissions)
        {
            levels.append(omission.position.y());
            level_heights[omission.position.y()] = qMax(level_heights.value(omission.position.y(), 0.0),
                                                        58.0);
        }
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        QVector<double> centres(levels.size(), 0.0);
        for (int i = 1; i < levels.size(); ++i)
            centres[i] = centres[i - 1] + qMax(155.0,
                (level_heights.value(levels.at(i - 1), 84.0)
                 + level_heights.value(levels.at(i), 84.0)) / 2.0 + 44.0);
        for (ProjectGraphNode &node : this->nodes)
            node.position.setY(centres.at(int(std::lower_bound(levels.begin(), levels.end(),
                node.position.y()) - levels.begin())));
        for (Omission &omission : this->omissions)
            omission.position.setY(centres.at(int(std::lower_bound(levels.begin(), levels.end(),
                omission.position.y()) - levels.begin())));
    }

    // The project connects to the oldest visible revision roots, not HEAD.
    // A visible revision is already connected when it has a visible parent or
    // a folded-parent marker. Branch labels are independent HEAD annotations.
    QSet<QString> anchored_revisions;
    for (const ProjectGraphNode &node : this->nodes)
    {
        if (node.kind != ProjectGraphNode::Kind::Revision)
            continue;
        for (const QString &parent_id : node.parents)
            if (visible.contains(parent_id))
                anchored_revisions.insert(node.id);
    }
    for (const Omission &omission : this->omissions)
        anchored_revisions.insert(omission.visible_child_id);
    for (ProjectGraphNode &node : this->nodes)
    {
        if (node.kind == ProjectGraphNode::Kind::Revision)
            node.container_id = anchored_revisions.contains(node.id)
                ? QString() : node.project_group_id;
        else if (node.kind == ProjectGraphNode::Kind::Branch)
        {
            // A HEAD with a visible immutable parent uses the HEAD -> revision
            // edge below. An unborn HEAD instead belongs directly to the
            // project, so its card must not be left floating in the graph.
            node.container_id = visible.contains(node.head_revision_id)
                ? QString() : node.project_group_id;
        }
    }

    // Reflect positions, not the painter transform: labels and click targets
    // stay upright. Source positions run from the oldest to newest revision.
    if (!this->top_down || (this->nodes.isEmpty() && this->omissions.isEmpty()))
        return;
    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    for (const ProjectGraphNode &node : this->nodes)
    {
        min_y = qMin(min_y, node.position.y());
        max_y = qMax(max_y, node.position.y());
    }
    for (const Omission &omission : this->omissions)
    {
        min_y = qMin(min_y, omission.position.y());
        max_y = qMax(max_y, omission.position.y());
    }
    for (ProjectGraphNode &node : this->nodes)
    {
        node.position.setY(min_y + max_y - node.position.y());
        for (auto route = node.parent_routes.begin(); route != node.parent_routes.end(); ++route)
            for (QPointF &control : route.value())
                control.setY(min_y + max_y - control.y());
    }
    for (Omission &omission : this->omissions)
        omission.position.setY(min_y + max_y - omission.position.y());
}

void ProjectRevisionGraph::setShowFullTree(bool full_tree)
{
    // Returning from Full tree, or clicking Editable heads again, must fold
    // back any paths unfolded interactively in the previous heads-only view.
    // Otherwise the mode control appears to stop working after a graph click.
    if (this->show_full_tree == full_tree
        && (full_tree || this->expanded_revisions.isEmpty()))
        return;
    if (!full_tree)
        this->expanded_revisions.clear();
    this->show_full_tree = full_tree;
    rebuildDisplayNodes();
    if (this->pending_fit)
        scheduleInitialFit();
    else
    {
        clampCamera();
        update();
    }
}

bool ProjectRevisionGraph::showsFullTree() const
{
    return this->show_full_tree;
}

int ProjectRevisionGraph::omissionCount() const
{
    return this->omissions.size();
}

QPointF ProjectRevisionGraph::omissionViewportPosition(int index) const
{
    if (index < 0 || index >= this->omissions.size())
        return QPointF();
    return graphToViewport(this->omissions.at(index).position);
}

void ProjectRevisionGraph::expandHiddenAncestors(const QString &parent_id)
{
    QHash<QString, const ProjectGraphNode *> by_id;
    for (const ProjectGraphNode &node : this->source_nodes)
        by_id.insert(node.id, &node);
    std::function<void(const QString &)> expand;
    expand = [&](const QString &id)
    {
        const ProjectGraphNode *node = by_id.value(id, nullptr);
        if (node == nullptr || this->expanded_revisions.contains(id))
            return;
        this->expanded_revisions.insert(id);
        for (const QString &parent : node->parents)
            expand(parent);
    };
    expand(parent_id);
    rebuildDisplayNodes();
    if (this->pending_fit)
        scheduleInitialFit();
    else
    {
        clampCamera();
        update();
    }
}

void ProjectRevisionGraph::expandOmission(int index)
{
    if (index < 0 || index >= this->omissions.size())
        return;
    const QString parent_id = this->omissions.at(index).hidden_parent_id;
    expandHiddenAncestors(parent_id);
}

QRectF ProjectRevisionGraph::omissionRect(const Omission &omission) const
{
    return QRectF(omission.position.x() - ProjectRevisionCardGeometry::width / 2.0,
                  omission.position.y() - 29.0, ProjectRevisionCardGeometry::width, 58.0);
}

const ProjectRevisionGraph::Omission *ProjectRevisionGraph::hitOmission(const QPointF &point) const
{
    for (const Omission &omission : this->omissions)
        if (omissionRect(omission).contains(point))
            return &omission;
    return nullptr;
}

void ProjectRevisionGraph::setNodes(const QVector<ProjectGraphNode> &nodes)
{
    // Rebuilding the catalogue, including a changed DAG topology, must not
    // reset an established camera. Only the first non-empty load is fitted;
    // after that, users control framing with the Fit graph button.
    bool same_layout = this->source_nodes.size() == nodes.size();
    if (same_layout)
    {
        for (int i = 0; i < nodes.size(); ++i)
        {
            const ProjectGraphNode &old_node = this->source_nodes.at(i);
            const ProjectGraphNode &new_node = nodes.at(i);
            if (old_node.id != new_node.id || old_node.position != new_node.position
                || old_node.parents != new_node.parents
                || old_node.heads != new_node.heads
                || old_node.parent_routes != new_node.parent_routes
                || old_node.kind != new_node.kind
                || old_node.head_revision_id != new_node.head_revision_id
                || old_node.container_id != new_node.container_id
                || old_node.project_group_id != new_node.project_group_id)
            {
                same_layout = false;
                break;
            }
        }
    }
    if (!same_layout)
        this->expanded_revisions.clear();
    this->source_nodes = nodes;
    rebuildDisplayNodes();
    // The first server card can arrive before the asynchronous catalogue.
    // Frame that placeholder once, then frame the first actual project load
    // once. Later refreshes, clicks and selection-driven rebuilds only clamp.
    const bool has_catalogue = std::any_of(this->nodes.cbegin(), this->nodes.cend(),
        [](const ProjectGraphNode &node) { return node.kind != ProjectGraphNode::Kind::Server; });
    if (!this->preserve_camera && !same_layout && this->has_fitted_content
        && !this->nodes.isEmpty() && isVisible())
    {
        // Replacing an already visible graph must establish its new camera
        // synchronously: callers query node viewport positions immediately.
        // The deferred timer remains responsible for startup layout settling.
        this->automatic_fit = true;
        fitGraph();
        this->automatic_fit = false;
    }
    else if ((!this->has_fitted_content && !this->nodes.isEmpty())
             || (has_catalogue && !this->has_fitted_catalogue)
             || (!this->preserve_camera && !same_layout && !this->nodes.isEmpty()))
        scheduleInitialFit();
    else if (this->pending_fit)
        scheduleInitialFit();
    else
    {
        // Bounds may change with source visibility or incoming revisions.
        // Keep the zoom and viewport anchor whenever the new bounds allow it.
        clampCamera();
        update();
    }
}

void ProjectRevisionGraph::setTopDown(bool top_down)
{
    if (this->top_down == top_down)
        return;
    this->top_down = top_down;
    rebuildDisplayNodes();
    if (this->pending_fit)
        scheduleInitialFit();
    else
    {
        clampCamera();
        update();
    }
}

bool ProjectRevisionGraph::isTopDown() const
{
    return this->top_down;
}

void ProjectRevisionGraph::setSelection(const QString &revision_id)
{
    if (this->selected_revision == revision_id)
        return;
    this->selected_revision = revision_id;
    bool revision_selection = false;
    for (const ProjectGraphNode &node : this->source_nodes)
        if (node.id == revision_id && node.kind == ProjectGraphNode::Kind::Revision)
        {
            revision_selection = true;
            break;
        }
    if (!this->show_full_tree && revision_selection)
    {
        bool visible = false;
        for (const ProjectGraphNode &node : this->nodes)
            if (node.id == revision_id) { visible = true; break; }
        if (!visible)
        {
            // A historical revision selected from the left panel must never
            // become an invisible graph selection. Unfold the containing head
            // path, leaving the other branches in heads-only mode.
            QHash<QString, const ProjectGraphNode *> by_id;
            for (const ProjectGraphNode &node : this->source_nodes)
                by_id.insert(node.id, &node);
            std::function<bool(const QString &, QSet<QString> &)> contains;
            contains = [&](const QString &id, QSet<QString> &visited) -> bool
            {
                if (id == revision_id) return true;
                if (visited.contains(id)) return false;
                visited.insert(id);
                const ProjectGraphNode *node = by_id.value(id, nullptr);
                if (node == nullptr) return false;
                for (const QString &parent_id : node->parents)
                    if (contains(parent_id, visited)) return true;
                return false;
            };
            bool revealed = false;
            for (const ProjectGraphNode &head : this->source_nodes)
            {
                if (head.kind != ProjectGraphNode::Kind::Revision || head.heads.isEmpty()) continue;
                for (const QString &parent_id : head.parents)
                {
                    QSet<QString> visited;
                    if (!contains(parent_id, visited)) continue;
                    expandHiddenAncestors(parent_id);
                    revealed = true;
                }
            }
            if (!revealed)
                expandHiddenAncestors(revision_id);
        }
    }
    // Selection highlights a node but never pans or fits the camera.
    update();
}

QRectF ProjectRevisionGraph::nodeRect(const ProjectGraphNode &node) const
{
    if (node.kind == ProjectGraphNode::Kind::Revision)
        return ProjectRevisionCardGeometry::bounds(node.position,
            0);
    const double width = node.kind == ProjectGraphNode::Kind::Server
        ? server_node_width : node.kind == ProjectGraphNode::Kind::Branch
            ? 196.0 : ProjectRevisionCardGeometry::width;
    const double height = node.kind == ProjectGraphNode::Kind::Server
        ? server_node_height : node.kind == ProjectGraphNode::Kind::Branch
            ? 70.0 : project_node_height;
    return QRectF(node.position.x() - width / 2.0,
                  node.position.y() - height / 2.0, width, height);
}

QPointF ProjectRevisionGraph::mapToGraph(const QPointF &point) const
{
    return (point - this->offset) / this->zoom;
}

QPointF ProjectRevisionGraph::graphToViewport(const QPointF &point) const
{
    return point * this->zoom + this->offset;
}

QPointF ProjectRevisionGraph::revisionViewportPosition(const QString &revision_id) const
{
    for (const ProjectGraphNode &node : this->nodes)
        if (node.kind == ProjectGraphNode::Kind::Revision && node.id == revision_id)
            return graphToViewport(node.position);
    return QPointF();
}

QPointF ProjectRevisionGraph::nodeViewportPosition(const QString &id) const
{
    for (const ProjectGraphNode &node : this->nodes)
        if (node.id == id)
            return graphToViewport(node.position);
    if (id.startsWith(QStringLiteral("group:branch:")))
    {
        const QString branch_id = id.mid(QStringLiteral("group:branch:").size());
        for (const ProjectGraphNode &node : this->nodes)
            for (const ProjectGraphBranchHead &head : node.heads)
                if (head.id == branch_id)
                    return graphToViewport(node.position);
    }
    return QPointF();
}

double ProjectRevisionGraph::zoomFactor() const
{
    return this->zoom;
}

QString ProjectRevisionGraph::selectionId() const
{
    return this->selected_revision;
}

QString ProjectRevisionGraph::nodeBranchCaption(const QString &node_id) const
{
    for (const ProjectGraphNode &node : this->source_nodes)
        if (node.id == node_id) return node.branch;
    return {};
}

QRectF ProjectRevisionGraph::graphBounds() const
{
    QRectF bounds;
    bool first = true;
    for (const ProjectGraphNode &node : this->nodes)
    {
        bounds = first ? nodeRect(node) : bounds.united(nodeRect(node));
        first = false;
    }
    for (const Omission &omission : this->omissions)
    {
        bounds = first ? omissionRect(omission) : bounds.united(omissionRect(omission));
        first = false;
    }
    return bounds;
}

void ProjectRevisionGraph::clampCamera()
{
    if (width() <= 0 || height() <= 0 || this->nodes.isEmpty())
        return;
    const QRectF bounds = graphBounds();
    const QPointF previous = this->offset;
    const double margin_x = qMin(camera_margin_px, width() * 0.22);
    const double margin_y = qMin(camera_margin_px, height() * 0.22);
    const double scene_width = bounds.width() * this->zoom;
    const double scene_height = bounds.height() * this->zoom;
    if (scene_width + 2.0 * margin_x <= width())
        this->offset.setX(width() / 2.0 - bounds.center().x() * this->zoom);
    else
        this->offset.setX(qBound(width() - margin_x - bounds.right() * this->zoom,
                                 this->offset.x(), margin_x - bounds.left() * this->zoom));
    if (scene_height + 2.0 * margin_y <= height())
        this->offset.setY(height() / 2.0 - bounds.center().y() * this->zoom);
    else
        this->offset.setY(qBound(height() - margin_y - bounds.bottom() * this->zoom,
                                 this->offset.y(), margin_y - bounds.top() * this->zoom));
    const QPointF correction = this->offset - previous;
    if (!qFuzzyIsNull(correction.x()))
        this->pan_velocity.setX(0.0);
    if (!qFuzzyIsNull(correction.y()))
        this->pan_velocity.setY(0.0);
    // Keep a zoom animation's anchor in sync with actual bounded movement.
    this->zoom_anchor += correction;
}

const ProjectGraphNode *ProjectRevisionGraph::hitNode(const QPointF &point) const
{
    for (const ProjectGraphNode &node : this->nodes)
    {
        if (nodeRect(node).adjusted(-8, -8, 8, 8).contains(point))
            return &node;
    }
    return nullptr;
}

void ProjectRevisionGraph::scheduleInitialFit()
{
    this->pending_fit = true;
    // Restart after every layout change. This matters when a QStackedWidget
    // shows the Projects page while the QMainWindow/docks/splitter are still
    // negotiating their final sizes.
    if (isVisible())
        this->initial_fit_timer.start();
    update();
}

void ProjectRevisionGraph::fitGraph()
{
    if (!this->automatic_fit)
        this->preserve_camera = true;
    if (!isVisible() || width() <= 0 || height() <= 0)
    {
        scheduleInitialFit();
        return;
    }
    this->initial_fit_timer.stop();
    this->pending_fit = false;
    stopNavigation();
    if (this->nodes.isEmpty())
    {
        this->zoom = 1.0;
        this->offset = QPointF(width() / 2.0, height() / 2.0);
        update();
        return;
    }

    this->has_fitted_content = true;
    for (const ProjectGraphNode &node : this->nodes)
        if (node.kind != ProjectGraphNode::Kind::Server)
        {
            this->has_fitted_catalogue = true;
            break;
        }
    QRectF bounds = graphBounds();
    bounds.adjust(-76, -85, 76, 85);
    this->zoom = qBound(min_zoom, qMin((width() - 32.0) / bounds.width(),
                                  (height() - 32.0) / bounds.height()), 1.5);
    this->offset = QPointF(width() / 2.0, height() / 2.0) - bounds.center() * this->zoom;
    clampCamera();
    update();
}

void ProjectRevisionGraph::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (this->pending_fit)
        scheduleInitialFit();
}

void ProjectRevisionGraph::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (this->pending_fit)
    {
        // The first visible geometry may still be a placeholder. Defer the
        // initial fit until the containing layout has settled.
        scheduleInitialFit();
        return;
    }
    if (!this->preserve_camera && this->has_fitted_content)
    {
        // The first automatic fit has already run. Refit against the actual
        // new viewport immediately, rather than leaving the camera computed
        // from the old dock/splitter dimensions until a timer fires.
        this->automatic_fit = true;
        fitGraph();
        this->automatic_fit = false;
        return;
    }
    if (!event->oldSize().isValid())
    {
        // An invalid old size is not a new graph load. Preserve a camera
        // the user has already positioned instead of doing a second fit.
        clampCamera();
        update();
        return;
    }

    // Later dock changes are NOT graph loads. They preserve zoom and the
    // viewport center rather than repeatedly discarding manual navigation.
    this->offset += QPointF((event->size().width() - event->oldSize().width()) / 2.0,
                            (event->size().height() - event->oldSize().height()) / 2.0);
    clampCamera();
    update();
}

void ProjectRevisionGraph::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor base = palette().color(QPalette::Base);
    const QColor ink = palette().color(QPalette::Text);
    const QColor muted = palette().color(QPalette::PlaceholderText);
    const QColor accent = palette().color(QPalette::Highlight);
    painter.fillRect(rect(), base);

    // A subtle reference grid reinforces the graph-as-workspace metaphor.
    painter.setPen(QPen(tint(muted, 27), 1));
    const double grid = 36.0 * this->zoom;
    if (grid >= 12.0)
    {
        const double start_x = std::fmod(this->offset.x(), grid);
        const double start_y = std::fmod(this->offset.y(), grid);
        for (double x = start_x; x < width(); x += grid)
            for (double y = start_y; y < height(); y += grid)
                painter.drawPoint(QPointF(x, y));
    }

    if (this->nodes.isEmpty())
    {
        painter.setPen(muted);
        painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter,
                         tr("No revisions to display\nSelect a project or create a branch."));
        return;
    }

    painter.save();
    painter.translate(this->offset);
    painter.scale(this->zoom, this->zoom);

    // Database -> project -> oldest loaded revision are ownership edges.
    // Branch labels are annotations on HEAD, not ancestry intermediates.
    QHash<QString, const ProjectGraphNode *> visible_by_id;
    for (const ProjectGraphNode &node : this->nodes)
        visible_by_id.insert(node.id, &node);
    const double structural_sign = this->top_down ? 1.0 : -1.0;
    for (const ProjectGraphNode &node : this->nodes)
    {
        const ProjectGraphNode *container = visible_by_id.value(node.container_id, nullptr);
        if (container == nullptr)
            continue;
        // Branch names are decorations within the revision cards.
        const QRectF start_box = nodeRect(*container);
        const QRectF end_box = nodeRect(node);
        const QPointF begin(container->position.x(), container->position.y()
            + structural_sign * start_box.height() / 2.0);
        QPointF end(node.position.x(), node.position.y()
            - structural_sign * end_box.height() / 2.0);
        QPainterPath path(begin);
        // A server's project children occupy one sibling rank. Their entire
        // descendant DAGs are in disjoint corridors, so there is no need for
        // detours through (or around) an unrelated project's revision cards.
        if (std::abs(begin.x() - end.x()) < 0.01)
            path.lineTo(end);
        else
        {
            const double bend = qMax(24.0, std::abs(end.y() - begin.y()) * 0.45);
            path.cubicTo(begin + QPointF(0, structural_sign * bend),
                         end - QPointF(0, structural_sign * bend), end);
        }
        painter.setPen(QPen(tint(accent, 145), 2.5, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    // When heads-only mode hides every loaded ancestor of a HEAD, connect
    // the project to the omission marker, then the marker to the HEAD. A
    // project -> HEAD shortcut would bypass the folded chronological chain.
    for (const Omission &omission : this->omissions)
    {
        if (!omission.visible_parent_id.isEmpty())
            continue;
        const ProjectGraphNode *child = visible_by_id.value(omission.visible_child_id, nullptr);
        if (child == nullptr)
            continue;
        const ProjectGraphNode *project = visible_by_id.value(child->project_group_id, nullptr);
        if (project == nullptr)
            continue;
        const QPointF begin(project->position.x(), project->position.y()
            + structural_sign * project_node_height / 2.0);
        const QPointF end(omission.position.x(), omission.position.y()
            - structural_sign * 29.0);
        QPainterPath path(begin);
        if (std::abs(begin.x() - end.x()) < 0.01)
            path.lineTo(end);
        else
            path.cubicTo(begin + QPointF(0, structural_sign * 40.0),
                         end - QPointF(0, structural_sign * 40.0), end);
        painter.setPen(QPen(tint(accent, 145), 2.5, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    // Fold markers preserve the oldest -> newest visual order. A collapse
    // never changes the underlying revision parent edges or HEAD pointers.
    for (const Omission &omission : this->omissions)
    {
        const ProjectGraphNode *child = visible_by_id.value(omission.visible_child_id, nullptr);
        const ProjectGraphNode *parent = visible_by_id.value(omission.visible_parent_id, nullptr);
        if (child == nullptr)
            continue;
        const double sign = this->top_down ? 1.0 : -1.0;
        painter.setPen(QPen(tint(accent, 145), 2.5, Qt::DashLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        const auto draw_segment = [&](const QPointF &begin, const QPointF &end)
        {
            QPainterPath path(begin);
            path.cubicTo(begin + QPointF(0, sign * 40.0),
                         end - QPointF(0, sign * 40.0), end);
            painter.drawPath(path);
        };
        if (parent != nullptr)
            draw_segment(parent->position + QPointF(0, sign * nodeRect(*parent).height() / 2.0),
                         omission.position - QPointF(0, sign * 29.0));
        draw_segment(omission.position + QPointF(0, sign * 29.0),
                     child->position - QPointF(0, sign * nodeRect(*child).height() / 2.0));
    }

    // Revision ancestry remains based on immutable parent IDs. Render the
    // chronological visual direction: older parent -> newer child (HEAD).
    for (const ProjectGraphNode &node : this->nodes)
    {
        for (const QString &parent_id : node.parents)
        {
            const ProjectGraphNode *parent = visible_by_id.value(parent_id, nullptr);
            if (parent == nullptr)
                continue;
            const double sign = this->top_down ? 1.0 : -1.0;
            const QPointF begin = parent->position + QPointF(0, sign * nodeRect(*parent).height() / 2.0);
            const QPointF end = node.position - QPointF(0, sign * nodeRect(node).height() / 2.0);
            QPainterPath path(begin);
            const QVector<QPointF> route = node.parent_routes.value(parent_id);
            if (route.size() >= 4 && (route.size() - 1) % 3 == 0)
            {
                // The same layered-DAG engine that positioned the variable-size
                // cards also routed this edge around other revision cards.
                path = QPainterPath(route.first());
                for (int point = 1; point + 2 < route.size(); point += 3)
                    path.cubicTo(route.at(point), route.at(point + 1),
                                 route.at(point + 2));
            }
            else if (std::abs(begin.x() - end.x()) < 0.01)
                path.lineTo(end);
            else
                path.cubicTo(begin + QPointF(0, sign * 42.0),
                             end - QPointF(0, sign * 42.0), end);
            painter.setPen(QPen(tint(accent, 115), 3.0, Qt::SolidLine, Qt::RoundCap));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
        }
    }

    // HEAD is connected to its last loaded immutable revision, when present.
    // An unborn HEAD has no parent revision and remains a root of its lane.
    for (const ProjectGraphNode &head : this->nodes)
    {
        if (head.kind != ProjectGraphNode::Kind::Branch) continue;
        const ProjectGraphNode *parent = visible_by_id.value(head.head_revision_id, nullptr);
        if (parent == nullptr || parent->kind != ProjectGraphNode::Kind::Revision) continue;
        const double sign = this->top_down ? 1.0 : -1.0;
        const QPointF begin = parent->position + QPointF(0, sign * nodeRect(*parent).height() / 2.0);
        const QPointF end = head.position - QPointF(0, sign * nodeRect(head).height() / 2.0);
        QPainterPath path(begin);
        const QVector<QPointF> route = head.parent_routes.value(head.head_revision_id);
        if (route.size() >= 4 && (route.size() - 1) % 3 == 0)
        {
            path = QPainterPath(route.first());
            for (int i = 1; i + 2 < route.size(); i += 3)
                path.cubicTo(route.at(i), route.at(i + 1), route.at(i + 2));
        }
        else if (std::abs(begin.x() - end.x()) < 0.01) path.lineTo(end);
        else path.cubicTo(begin + QPointF(0, sign * 42.0),
                          end - QPointF(0, sign * 42.0), end);
        painter.setPen(QPen(tint(QColor(57, 184, 145), 190), 2.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    QFont title_font = font();
    title_font.setBold(true);
    QFont small_font = font();
    small_font.setPointSizeF(qMax(7.0, small_font.pointSizeF() - 1.0));
    for (const ProjectGraphNode &node : this->nodes)
    {
        const QRectF box = nodeRect(node);
        const bool selected = node.id == this->selected_revision;
        const QColor surface = palette().color(QPalette::Window);
        if (node.kind != ProjectGraphNode::Kind::Revision)
        {
            const bool server = node.kind == ProjectGraphNode::Kind::Server;
            const bool branch_reference = node.kind == ProjectGraphNode::Kind::Branch;
            if (branch_reference)
            {
                // Render a branch as a separate selectable HEAD reference,
                // visibly distinct from an immutable revision card.
                const QColor green(57, 184, 145);
                painter.setPen(QPen(tint(green, selected ? 255 : 175),
                                    selected ? 3.2 : 1.8));
                painter.setBrush(tint(green, selected ? 100 : 35));
                painter.drawRoundedRect(box, 11, 11);
                painter.setFont(title_font);
                painter.setPen(ink);
                painter.drawText(box.adjusted(13, 6, -11, -29),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    painter.fontMetrics().elidedText(node.title, Qt::ElideRight,
                        int(box.width() - 24)));
                painter.setFont(small_font);
                painter.setPen(tint(green, 235));
                painter.drawText(box.adjusted(13, 34, -11, -7),
                    Qt::AlignLeft | Qt::AlignVCenter, node.branch);
                continue;
            }
            const bool personal_server = server && node.personal_server;
            const QColor group_color = personal_server ? QColor(222, 169, 46)
                : server && node.pending_server ? QColor(126, 130, 134)
                : server ? QColor(50, 174, 173) : accent;
            painter.setPen(QPen(tint(group_color, selected ? 255 : (server ? 230 : 140)),
                                selected ? 3.4 : server ? 2.2 : 1.6));
            painter.setBrush(tint(group_color, personal_server ? (selected ? 185 : 145)
                : selected ? 100 : server ? 64 : 24));
            painter.drawRoundedRect(box, server ? 15 : 11, server ? 15 : 11);
            if (server)
            {
                // Compact database cylinder icon: unmistakably different
                // from a revision card, with no platform icon dependency.
                const QRectF icon_box(box.left() + 16, box.top() + 20, 27, 26);
                painter.setPen(QPen(group_color, 1.7));
                painter.setBrush(tint(group_color, 45));
                painter.drawRoundedRect(icon_box, 5, 5);
                painter.drawEllipse(QRectF(icon_box.left(), icon_box.top(),
                                           icon_box.width(), 8));
                painter.drawArc(QRectF(icon_box.left(), icon_box.top() + 9,
                                       icon_box.width(), 8), 180 * 16, 180 * 16);
                painter.setFont(title_font);
                painter.setPen(ink);
                // Keep the existing database icon at the left of every server.
                // Personal servers additionally show a crown directly after
                // their name, with enough space reserved when it is elided.
                const QString crown = QStringLiteral("♛");
                const QFontMetrics title_metrics = painter.fontMetrics();
                const qreal crown_width = personal_server
                    ? title_metrics.horizontalAdvance(crown) + 6.0 : 0.0;
                const QRectF title_box = box.adjusted(53, 10, -12, -29);
                const int title_width = qMax(0, int(title_box.width() - crown_width));
                const QString title = title_metrics.elidedText(
                    node.title, Qt::ElideRight, title_width);
                painter.drawText(title_box.adjusted(0, 0, -crown_width, 0),
                    Qt::AlignVCenter | Qt::AlignLeft, title);
                if (personal_server)
                {
                    const qreal crown_x = title_box.left()
                        + title_metrics.horizontalAdvance(title) + 6.0;
                    painter.drawText(QRectF(crown_x, title_box.top(),
                                            title_metrics.horizontalAdvance(crown),
                                            title_box.height()),
                        Qt::AlignVCenter | Qt::AlignLeft, crown);
                }
                painter.setFont(small_font);
                painter.setPen(muted);
                painter.drawText(box.adjusted(53, 40, -12, -9),
                    Qt::AlignVCenter | Qt::AlignLeft, node.branch);
            }
            else
            {
                painter.setFont(title_font);
                painter.setPen(ink);
                painter.drawText(box.adjusted(15, 8, -12, -31),
                    Qt::AlignVCenter | Qt::AlignLeft,
                    painter.fontMetrics().elidedText(node.title, Qt::ElideRight,
                        int(box.width() - 27)));
                painter.setFont(small_font);
                painter.setPen(muted);
                painter.drawText(box.adjusted(15, 34, -12, -8),
                    Qt::AlignVCenter | Qt::AlignLeft,
                    painter.fontMetrics().elidedText(node.branch, Qt::ElideRight,
                                                     int(box.width() - 27)));
            }
            continue;
        }
        painter.setBrush(surface);
        painter.setPen(QPen(selected ? accent : tint(muted, 125), selected ? 3 : 1.4));
        painter.drawRoundedRect(box, 12, 12);
        if (selected)
        {
            painter.setBrush(accent);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(QRectF(box.left(), box.top() + 10, 5, box.height() - 20), 2, 2);
        }

        // The first two lines are anchored to the TOP of the card. Using
        // box.adjusted(..., negative bottom inset) moved the title downward
        // whenever HEAD references increased the card height, causing the
        // title, hash and badges to paint on top of one another.
        const QRectF title_box = ProjectRevisionCardGeometry::titleRect(box);
        const QRectF id_box = ProjectRevisionCardGeometry::revisionIdRect(box);
        const QString short_id = DvmdbIdentity::shortLabel(
            node.display_id.isEmpty() ? node.id : node.display_id);
        const QString title = ProjectRevisionCardGeometry::distinctTitle(node.title, short_id);
        painter.setPen(ink);
        painter.setFont(title_font);
        painter.drawText(title_box, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(title, Qt::ElideRight,
                                                          int(title_box.width())));
        painter.setFont(small_font);
        painter.setPen(muted);
        painter.drawText(id_box, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(node.branch, Qt::ElideRight,
                                                          int(id_box.width())));


    }
    // Fold markers are explicitly interactive graph elements, not tooltips.
    painter.setFont(small_font);
    for (const Omission &omission : this->omissions)
    {
        const QRectF box = omissionRect(omission);
        painter.setPen(QPen(tint(accent, 180), 2, Qt::DashLine));
        painter.setBrush(tint(accent, 28));
        painter.drawRoundedRect(box, 10, 10);
        painter.setPen(ink);
        painter.drawText(box.adjusted(8, 4, -8, -4), Qt::AlignCenter | Qt::TextWordWrap,
                         tr("+ %n hidden revision(s)\nClick to unfold", nullptr,
                            omission.revision_count));
    }
    painter.restore();
}

void ProjectRevisionGraph::startNavigation()
{
    if (!isVisible() || this->navigation_timer.isActive())
        return;
    this->frame_elapsed.start();
    this->navigation_timer.start();
}

QPointF ProjectRevisionGraph::keyboardDirection() const
{
    QPointF direction;
    if (this->key_left)
        direction.rx() += 1.0;
    if (this->key_right)
        direction.rx() -= 1.0;
    if (this->key_up)
        direction.ry() += 1.0;
    if (this->key_down)
        direction.ry() -= 1.0;
    return direction;
}

void ProjectRevisionGraph::stopNavigation()
{
    this->navigation_timer.stop();
    this->pan_velocity = QPointF();
    this->mouse_pan_velocity = QPointF();
    this->mouse_inertia = false;
    this->dragging = false;
    this->mouse_pressed = false;
    this->drag_distance = 0.0;
    this->key_left = false;
    this->key_right = false;
    this->key_up = false;
    this->key_down = false;
    this->key_zoom_in = false;
    this->key_zoom_out = false;
    this->key_fast = false;
    this->zoom_motion.cancel();
    setCursor(Qt::OpenHandCursor);
}

void ProjectRevisionGraph::updateNavigation()
{
    if (!isVisible())
    {
        stopNavigation();
        return;
    }

    const double dt = qBound(0.0, this->frame_elapsed.restart() / 1000.0, 0.05);
    if (dt <= 0.0)
        return;

    if (this->zoom_motion.active())
    {
        const double next_octaves = this->zoom_motion.advance(std::log2(this->zoom), dt);
        const double next_zoom = std::exp2(next_octaves);
        if (next_zoom != this->zoom)
        {
            this->zoom = next_zoom;
            this->offset = this->zoom_anchor - this->zoom_graph_anchor * this->zoom;
            clampCamera();
            update();
        }
    }

    if (!this->mouse_pressed)
    {
        const QPointF direction = keyboardDirection();
        double acceleration = direction.isNull()
            ? (this->mouse_inertia ? mouse_pan_deceleration : pan_deceleration)
            : pan_acceleration;
        if (!direction.isNull() && this->key_fast)
            acceleration *= fast_acceleration_multiplier;
        const QPointF target = direction * (pan_speed * (this->key_fast ? fast_pan_multiplier : 1.0));
        this->pan_velocity = moveTowards(this->pan_velocity, target, acceleration * dt);
        if (direction.isNull() && vectorLength(this->pan_velocity) < velocity_stop_threshold)
        {
            this->pan_velocity = QPointF();
            this->mouse_inertia = false;
        }
        if (!this->pan_velocity.isNull())
        {
            const QPointF pan_delta = this->pan_velocity * dt;
            this->offset += pan_delta;
            this->zoom_anchor += pan_delta;
            clampCamera();
            update();
        }
    }

    if (!this->zoom_motion.active() && keyboardDirection().isNull()
        && this->pan_velocity.isNull() && !this->mouse_pressed)
    {
        this->navigation_timer.stop();
    }
}

void ProjectRevisionGraph::setZoomAnchor(const QPointF &anchor)
{
    this->zoom_anchor = anchor;
    this->zoom_graph_anchor = mapToGraph(anchor);
}

void ProjectRevisionGraph::synchronizeZoomHold()
{
    const int direction = int(this->key_zoom_in) - int(this->key_zoom_out);
    if (direction != 0)
        setZoomAnchor(QPointF(width() / 2.0, height() / 2.0));
    const double rate = keyboard_zoom_octaves_per_second * (this->key_fast ? 2.0 : 1.0);
    this->zoom_motion.setHold(std::log2(this->zoom), direction, rate,
                              MapZoomAnimation::KeyboardTapOctaves);
    if (this->zoom_motion.active())
        startNavigation();
}

void ProjectRevisionGraph::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    this->mouse_pressed = true;
    this->last_pointer = event->position();
    this->drag_distance = 0.0;
    this->dragging = false;
    this->mouse_inertia = false;
    this->pan_velocity = QPointF();
    this->mouse_pan_velocity = QPointF();
    this->drag_elapsed.start();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void ProjectRevisionGraph::mouseMoveEvent(QMouseEvent *event)
{
    if (this->mouse_pressed && (event->buttons() & Qt::LeftButton))
    {
        const QPointF delta = event->position() - this->last_pointer;
        this->last_pointer = event->position();
        this->drag_distance += vectorLength(delta);
        if (this->drag_distance >= QApplication::startDragDistance())
            this->dragging = true;
        const qint64 elapsed_ms = this->drag_elapsed.restart();
        if (this->dragging && !delta.isNull())
        {
            this->preserve_camera = true;
            const QPointF before_pan = this->offset;
            this->offset += delta;
            this->zoom_anchor += delta;
            clampCamera();
            const QPointF applied_delta = this->offset - before_pan;
            if (elapsed_ms > 0)
            {
                const double elapsed_seconds = qBound(0.001, elapsed_ms / 1000.0, 0.1);
                QPointF measured_velocity = applied_delta / elapsed_seconds;
                const double speed = vectorLength(measured_velocity);
                if (speed > mouse_maximum_speed)
                    measured_velocity *= mouse_maximum_speed / speed;
                if (this->mouse_pan_velocity.isNull())
                    this->mouse_pan_velocity = measured_velocity;
                else
                    this->mouse_pan_velocity = this->mouse_pan_velocity * (1.0 - mouse_velocity_smoothing)
                        + measured_velocity * mouse_velocity_smoothing;
            }
            update();
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ProjectRevisionGraph::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && this->mouse_pressed)
    {
        this->mouse_pressed = false;
        setCursor(Qt::OpenHandCursor);
        if (!this->dragging)
        {
            const QPointF point = mapToGraph(event->position());
            if (const Omission *omission = hitOmission(point))
            {
                const QString parent_id = omission->hidden_parent_id;
                expandHiddenAncestors(parent_id);
            }
            else if (const ProjectGraphNode *node = hitNode(point))
            {
                if (node->kind == ProjectGraphNode::Kind::Server)
                    emit serverActivated(node->id.mid(QStringLiteral("group:server:").size()));
                else if (node->kind == ProjectGraphNode::Kind::Project)
                    emit projectActivated(node->id.mid(QStringLiteral("group:project:").size()));
                else if (node->kind == ProjectGraphNode::Kind::Branch)
                    emit headActivated(node->id.mid(QStringLiteral("group:branch:").size()));
                else
                    emit revisionActivated(node->id);
            }
        }
        else if (this->drag_elapsed.isValid()
                 && this->drag_elapsed.elapsed() <= release_timeout_ms
                 && vectorLength(this->mouse_pan_velocity) >= mouse_inertia_minimum_speed)
        {
            this->pan_velocity = this->mouse_pan_velocity;
            this->mouse_inertia = true;
            startNavigation();
        }
        this->mouse_pan_velocity = QPointF();
        this->dragging = false;
        this->drag_distance = 0.0;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ProjectRevisionGraph::wheelEvent(QWheelEvent *event)
{
    // A trackpad may send pixel-only wheel deltas. The map uses the same
    // conversion and critically damped zoom motion for both wheel devices.
    const double steps = event->angleDelta().y() != 0
        ? event->angleDelta().y() / 120.0 : event->pixelDelta().y() / 240.0;
    if (std::isfinite(steps) && std::abs(steps) > 1e-9)
    {
        this->preserve_camera = true;
        setZoomAnchor(event->position());
        this->zoom_motion.addImpulse(std::log2(this->zoom), steps * 0.5);
        startNavigation();
    }
    event->accept();
}

ProjectRevisionGraph::NavigationKeyIdentity ProjectRevisionGraph::navigationKeyIdentity(
    const QKeyEvent *event)
{
    NavigationKeyIdentity identity;
    identity.scan_code = event->nativeScanCode();
    identity.virtual_key = event->nativeVirtualKey();
    identity.key = event->key();
    return identity;
}

bool ProjectRevisionGraph::sameNavigationKey(const NavigationKeyIdentity &first,
                                            const NavigationKeyIdentity &second)
{
    if (first.scan_code != 0 && second.scan_code != 0)
        return first.scan_code == second.scan_code;
    if (first.virtual_key != 0 && second.virtual_key != 0)
        return first.virtual_key == second.virtual_key;
    return first.key == second.key;
}

void ProjectRevisionGraph::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Shift)
    {
        this->key_fast = true;
        if (this->key_zoom_in || this->key_zoom_out)
            synchronizeZoomHold();
        startNavigation();
        event->accept();
        return;
    }

    const Qt::KeyboardModifiers arrow_modifiers =
        event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    const GuiShortcutRegistry &registry = guiShortcutRegistry();
    const Qt::KeyboardModifiers fast_modifier = Qt::ShiftModifier;

    bool *key = nullptr;
    NavigationKeyIdentity *identity = nullptr;
    bool zoom_key = false;
    if ((event->key() == Qt::Key_Left && arrow_modifiers == Qt::NoModifier)
        || guiShortcutMatches(event, registry.shortcut(GuiShortcutId::MapPanLeft), fast_modifier))
    {
        key = &this->key_left;
        identity = &this->key_left_identity;
    }
    else if ((event->key() == Qt::Key_Right && arrow_modifiers == Qt::NoModifier)
             || guiShortcutMatches(event, registry.shortcut(GuiShortcutId::MapPanRight), fast_modifier))
    {
        key = &this->key_right;
        identity = &this->key_right_identity;
    }
    else if ((event->key() == Qt::Key_Up && arrow_modifiers == Qt::NoModifier)
             || guiShortcutMatches(event, registry.shortcut(GuiShortcutId::MapPanUp), fast_modifier))
    {
        key = &this->key_up;
        identity = &this->key_up_identity;
    }
    else if ((event->key() == Qt::Key_Down && arrow_modifiers == Qt::NoModifier)
             || guiShortcutMatches(event, registry.shortcut(GuiShortcutId::MapPanDown), fast_modifier))
    {
        key = &this->key_down;
        identity = &this->key_down_identity;
    }
    else if (guiShortcutMatches(event, registry.shortcut(GuiShortcutId::MapZoomIn), fast_modifier))
    {
        key = &this->key_zoom_in;
        identity = &this->key_zoom_in_identity;
        zoom_key = true;
    }
    else if (guiShortcutMatches(event, registry.shortcut(GuiShortcutId::MapZoomOut), fast_modifier))
    {
        key = &this->key_zoom_out;
        identity = &this->key_zoom_out_identity;
        zoom_key = true;
    }
    if (key == nullptr)
    {
        QWidget::keyPressEvent(event);
        return;
    }

    this->preserve_camera = true;
    this->key_fast = event->modifiers().testFlag(Qt::ShiftModifier);
    if (!*key)
    {
        *key = true;
        *identity = navigationKeyIdentity(event);
        if (zoom_key)
            synchronizeZoomHold();
    }
    startNavigation();
    event->accept();
}

void ProjectRevisionGraph::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Shift)
    {
        if (!event->isAutoRepeat())
            this->key_fast = false;
        if (this->key_zoom_in || this->key_zoom_out)
            synchronizeZoomHold();
        event->accept();
        return;
    }

    // Match the key that actually went down, not its now possibly changed
    // modifier combination (Ctrl/Alt/Shift can be released first).
    struct HeldKey
    {
        bool *pressed;
        const NavigationKeyIdentity *identity;
        bool zoom;
    };
    const HeldKey held_keys[] = {
        {&this->key_left, &this->key_left_identity, false},
        {&this->key_right, &this->key_right_identity, false},
        {&this->key_up, &this->key_up_identity, false},
        {&this->key_down, &this->key_down_identity, false},
        {&this->key_zoom_in, &this->key_zoom_in_identity, true},
        {&this->key_zoom_out, &this->key_zoom_out_identity, true},
    };
    const NavigationKeyIdentity released = navigationKeyIdentity(event);
    bool handled = false;
    bool zoom_released = false;
    for (const HeldKey &held_key : held_keys)
    {
        if (!*held_key.pressed || !sameNavigationKey(*held_key.identity, released))
            continue;
        handled = true;
        if (!event->isAutoRepeat())
        {
            *held_key.pressed = false;
            zoom_released = zoom_released || held_key.zoom;
        }
    }
    if (!handled)
    {
        QWidget::keyReleaseEvent(event);
        return;
    }
    if (!event->isAutoRepeat())
    {
        if (zoom_released)
            synchronizeZoomHold();
        startNavigation();
    }
    event->accept();
}

void ProjectRevisionGraph::focusOutEvent(QFocusEvent *event)
{
    stopNavigation();
    QWidget::focusOutEvent(event);
}

void ProjectRevisionGraph::hideEvent(QHideEvent *event)
{
    stopNavigation();
    QWidget::hideEvent(event);
}
