#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPoint>
#include <QSet>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#include <QVector>
#include <QWidget>

#include "map/core/map_zoom_animation.h"

// GUI-only graph data. No database IDs are inferred from these preview labels.
struct ProjectGraphBranchHead
{
    QString id;   // Stable source-scoped branch ID.
    QString name; // User-chosen branch name.
    bool operator==(const ProjectGraphBranchHead &other) const
    {
        return id == other.id && name == other.name;
    }
};

struct ProjectGraphNode
{
    enum class Kind { Revision, Project, Server, Branch };
    QString id;
    QString title;
    QString branch;
    QString display_id;
    QStringList parents;
    QStringList tags;
    QPointF position;
    Kind kind = Kind::Revision;
    bool pending_server = false; // Unverified database endpoint: muted graph card.
    bool personal_server = false; // Render the designated personal database in gold.
    // Structural hierarchy, separate from immutable revision ancestry.
    QString container_id;
    QString project_group_id;
    // A branch ref is its own selectable GUI node. This links it to the
    // revision it points at, without adding an immutable ancestry edge.
    QString head_revision_id;
    QString origin_branch_node_id; // Creation provenance edge, NOT a revision parent.
    QVector<ProjectGraphBranchHead> heads; // Real HEAD pointers; the GUI gives each its own reference card.
    // Layered-DAG cubic routes keyed by immutable parent revision ID.
    // Coordinates are the same canonical graph-space units as position.
    QHash<QString, QVector<QPointF>> parent_routes;
};

class QFocusEvent;
class QHideEvent;
class QShowEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

// Intentionally renderer-independent. The existing map surface owns the process-
// global bgfx renderer; a second independent bgfx::init would be unsafe.
// This widget is a functional pan/zoom/select fallback until a shared bgfx
// graph view can be implemented without disturbing the map presentation loop.
class ProjectRevisionGraph final : public QWidget
{
    Q_OBJECT
public:
    explicit ProjectRevisionGraph(QWidget *parent = nullptr);
    void setNodes(const QVector<ProjectGraphNode> &nodes);
    void setSelection(const QString &revision_id);
    void fitGraph();
    void setTopDown(bool top_down);
    bool isTopDown() const;
    void setShowFullTree(bool full_tree);
    bool showsFullTree() const;
    int omissionCount() const;
    QPointF omissionViewportPosition(int index) const;
    void expandOmission(int index);

    // Camera inspection also allows hit testing and graph overlays to use
    // exactly the same transform as the renderer.
    QPointF graphToViewport(const QPointF &point) const;
    QPointF revisionViewportPosition(const QString &revision_id) const;
    QPointF nodeViewportPosition(const QString &id) const;
    double zoomFactor() const;
    QString selectionId() const;
    QRectF graphBounds() const;

signals:
    void revisionActivated(const QString &revision_id);
    void branchActivated(const QString &branch_id);
    void serverActivated(const QString &server_id);
    void projectActivated(const QString &project_id);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Omission
    {
        QString hidden_parent_id;
        QString visible_parent_id;
        QString visible_child_id;
        QPointF position;
        int revision_count = 0;
    };
    void rebuildDisplayNodes();
    QRectF omissionRect(const Omission &omission) const;
    const Omission *hitOmission(const QPointF &point) const;
    void expandHiddenAncestors(const QString &parent_id);
    void scheduleInitialFit();
    QPointF mapToGraph(const QPointF &point) const;
    QRectF nodeRect(const ProjectGraphNode &node) const;
    bool hasBranchReferenceCards(const QString &revision_id) const;
    QRectF branchLabelRect(const ProjectGraphNode &node, int index) const;
    void clampCamera();
    const ProjectGraphNode *hitNode(const QPointF &point) const;
    void startNavigation();
    void updateNavigation();
    void stopNavigation();
    void synchronizeZoomHold();
    struct NavigationKeyIdentity
    {
        quint32 scan_code = 0;
        quint32 virtual_key = 0;
        int key = 0;
    };
    static NavigationKeyIdentity navigationKeyIdentity(const QKeyEvent *event);
    static bool sameNavigationKey(const NavigationKeyIdentity &first,
                                  const NavigationKeyIdentity &second);
    void setZoomAnchor(const QPointF &anchor);
    QPointF keyboardDirection() const;
    QVector<ProjectGraphNode> source_nodes;
    QSet<QString> reference_head_revisions;
    QVector<ProjectGraphNode> nodes;
    QVector<Omission> omissions;
    QSet<QString> expanded_revisions;
    QString selected_revision; // Any structural or revision node ID.
    QPointF offset;
    QPointF last_pointer;
    QPointF pan_velocity;
    QPointF mouse_pan_velocity;
    QPointF zoom_anchor;
    QPointF zoom_graph_anchor;
    QTimer navigation_timer;
    QTimer initial_fit_timer;
    QElapsedTimer frame_elapsed;
    QElapsedTimer drag_elapsed;
    MapZoomMotion zoom_motion;
    double zoom = 1.0;
    double drag_distance = 0.0;
    bool top_down = true;
    bool show_full_tree = true;
    bool pending_fit = true;
    bool automatic_fit = false;
    bool preserve_camera = false;
    bool has_fitted_content = false; // First visible server/graph content.
    bool has_fitted_catalogue = false; // First actual project catalogue.
    bool dragging = false;
    bool mouse_pressed = false;
    bool mouse_inertia = false;
    bool key_left = false;
    bool key_right = false;
    bool key_up = false;
    bool key_down = false;
    bool key_zoom_in = false;
    bool key_zoom_out = false;
    bool key_fast = false;
    NavigationKeyIdentity key_left_identity;
    NavigationKeyIdentity key_right_identity;
    NavigationKeyIdentity key_up_identity;
    NavigationKeyIdentity key_down_identity;
    NavigationKeyIdentity key_zoom_in_identity;
    NavigationKeyIdentity key_zoom_out_identity;
};
