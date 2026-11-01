#include "tabs/project_revision_graph.h"
#include "tabs/project_graph_placement.h"
#include "tabs/project_revision_card_geometry.h"
#include "config/shortcut_registry.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QImage>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QVBoxLayout>

#include <cmath>

#include <cstdio>

namespace
{
bool require(bool condition, const char *reason)
{
    if (!condition)
        std::fprintf(stderr, "project_revision_graph: %s\n", reason);
    return condition;
}

void click(QWidget &widget, const QPointF &position)
{
    QMouseEvent press(QEvent::MouseButtonPress, position, position, position,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, position, position, position,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &release);
}

void waitFor(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void sendKey(QWidget &widget, QEvent::Type type, int key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(type, key, modifiers);
    QCoreApplication::sendEvent(&widget, &event);
}

void sendMouse(QWidget &widget, QEvent::Type type, const QPointF &point,
               Qt::MouseButtons buttons)
{
    const Qt::MouseButton changed = type == QEvent::MouseMove
        ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, point, point, point, changed, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

void sendWheel(QWidget &widget, const QPointF &point, const QPoint &angle,
               const QPoint &pixels = QPoint())
{
    QWheelEvent event(point, point, pixels, angle, Qt::NoButton,
                      Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(&widget, &event);
}
}

int main(int argc, char **argv)
{
    // Never read or overwrite the developer's actual shortcut settings.
    QTemporaryDir temporary_configuration;
    if (!require(temporary_configuration.isValid(), "could not create isolated GUI configuration"))
        return 1;
    qputenv("XDG_DATA_HOME", temporary_configuration.path().toUtf8());
    QApplication app(argc, argv);

    // Multi-HEAD cards must keep the two-line revision header fixed while
    // growing only the HEAD badge area. The previous painter positioned the
    // title relative to the card BOTTOM, making it overlap the revision ID
    // and badges as soon as several branch refs pointed to the same revision.
    for (int head_count = 0; head_count <= 8; ++head_count)
    {
        const QRectF card = ProjectRevisionCardGeometry::bounds(QPointF(0, 0), head_count);
        const QRectF title = ProjectRevisionCardGeometry::titleRect(card);
        const QRectF id = ProjectRevisionCardGeometry::revisionIdRect(card);
        if (!require(title.top() - card.top() == 10.0
                         && id.top() - card.top() == 38.0,
                     "revision header shifted when HEAD badge count changed")) return 1;
        if (!require(title.bottom() < id.top(),
                     "revision title overlaps its revision hash")) return 1;
        if (!require(id.bottom() < card.bottom(),
                     "revision hash is outside its card")) return 1;
        for (int i = 0; i < head_count; ++i)
        {
            const QRectF head = ProjectRevisionCardGeometry::headRect(card, i);
            if (!require(id.bottom() < head.top() && head.bottom() < card.bottom(),
                         "HEAD badge overlaps the revision header or card edge")) return 1;
            if (i > 0 && !require(
                    ProjectRevisionCardGeometry::headRect(card, i - 1).bottom() < head.top(),
                    "HEAD badges overlap each other")) return 1;
        }
    }
    if (!require(ProjectRevisionCardGeometry::distinctTitle(
                     QStringLiteral("Revision 1615096adbe2"),
                     QStringLiteral("1615096adbe2")) == QStringLiteral("Revision")
                 && ProjectRevisionCardGeometry::distinctTitle(
                     QStringLiteral("Named commit"),
                     QStringLiteral("1615096adbe2")) == QStringLiteral("Named commit"),
                 "revision card displays a duplicated hash or drops descriptive titles")) return 1;

    ProjectRevisionGraph graph;
    graph.resize(800, 500);
    graph.show();
    app.processEvents();

    ProjectGraphNode node;
    node.id = QStringLiteral("test-revision");
    node.title = QStringLiteral("Initial preview revision");
    node.branch = QStringLiteral("main");
    node.heads = {{QStringLiteral("main"), QStringLiteral("main")}};
    node.position = QPointF(0.0, 0.0);
    graph.setNodes(QVector<ProjectGraphNode>{node});
    graph.setSelection(node.id);
    waitFor(110); // allow first-show geometry and automatic fit to settle

    QString revision_clicked;
    QString branch_clicked;
    QObject::connect(&graph, &ProjectRevisionGraph::revisionActivated,
                     &graph, [&revision_clicked](const QString &id)
    {
        revision_clicked = id;
    });
    QObject::connect(&graph, &ProjectRevisionGraph::branchActivated,
                     &graph, [&branch_clicked](const QString &name)
    {
        branch_clicked = name;
    });

    click(graph, QPointF(400, 250));
    if (!require(revision_clicked == node.id, "revision click did not emit selected hash"))
        return 1;

    // An unborn branch has a standalone selectable HEAD card without
    // needing a committed revision or a badge in another card.
    ProjectGraphNode unborn;
    unborn.kind = ProjectGraphNode::Kind::Branch;
    unborn.id = QStringLiteral("group:branch:unborn-main");
    unborn.title = QStringLiteral("HEAD");
    unborn.branch = QStringLiteral("Branch: main");
    unborn.position = QPointF(0, 0);
    graph.setNodes(QVector<ProjectGraphNode>{unborn});
    graph.setSelection(unborn.id);
    click(graph, graph.nodeViewportPosition(unborn.id));
    if (!require(branch_clicked == QStringLiteral("unborn-main"),
                 "unborn HEAD card did not activate its branch")) return 1;

    // Top-down is the default: older revisions sit ABOVE their newer
    // descendants (HEAD). Bottom-up mirrors the coordinates without
    // modifying revision parents or card hit targets.
    ProjectGraphNode previous = node;
    previous.id = QStringLiteral("parent-revision");
    previous.position = QPointF(0.0, 0.0);
    ProjectGraphNode latest = node;
    latest.position = QPointF(0.0, -320.0);
    latest.parents = {previous.id};
    graph.setNodes(QVector<ProjectGraphNode>{latest, previous});
    if (!require(graph.isTopDown(), "graph did not default to top-down"))
        return 1;
    if (!require(graph.revisionViewportPosition(previous.id).y()
                     < graph.revisionViewportPosition(latest.id).y(),
                 "top-down graph must show older ancestry before HEAD"))
        return 1;
    revision_clicked.clear();
    click(graph, graph.revisionViewportPosition(previous.id));
    if (!require(revision_clicked == previous.id,
                 "root revision hit testing failed in the default orientation"))
        return 1;
    graph.setTopDown(false);
    if (!require(!graph.isTopDown(), "bottom-up orientation was not applied"))
        return 1;
    if (!require(graph.revisionViewportPosition(latest.id).y()
                     < graph.revisionViewportPosition(previous.id).y(),
                 "bottom-up graph must reverse the oldest-to-HEAD presentation"))
        return 1;
    revision_clicked.clear();
    click(graph, graph.revisionViewportPosition(latest.id));
    if (!require(revision_clicked == latest.id,
                 "revision hit testing broke after flipping the DAG"))
        return 1;
    graph.setTopDown(true);
    if (!require(graph.revisionViewportPosition(previous.id).y()
                     < graph.revisionViewportPosition(latest.id).y(),
                 "returning to top-down did not restore oldest-first layout"))
        return 1;
    // Editable-heads canvas mode must retain a visible, clickable summary of
    // every hidden ancestry path. Unfolding one path must not expand the other.
    ProjectGraphNode main_head = node;
    main_head.id = QStringLiteral("head-main");
    main_head.parents = {QStringLiteral("main-intermediate")};
    main_head.position = QPointF(0, -310);
    ProjectGraphNode scenario_head = node;
    scenario_head.id = QStringLiteral("head-scenario");
    scenario_head.branch = QStringLiteral("scenario");
    scenario_head.heads = {{QStringLiteral("scenario"), QStringLiteral("scenario")}};
    scenario_head.parents = {QStringLiteral("intermediate")};
    scenario_head.position = QPointF(285, -310);
    ProjectGraphNode intermediate = scenario_head;
    intermediate.id = QStringLiteral("intermediate");
    intermediate.heads.clear();
    intermediate.parents = {QStringLiteral("root")};
    intermediate.position = QPointF(285, -155);
    ProjectGraphNode main_intermediate = main_head;
    main_intermediate.id = QStringLiteral("main-intermediate");
    main_intermediate.heads.clear();
    main_intermediate.parents = {QStringLiteral("root")};
    main_intermediate.position = QPointF(0, -155);
    ProjectGraphNode root = main_head;
    root.id = QStringLiteral("root");
    root.heads.clear();
    root.parents.clear();
    root.position = QPointF(0, 0);
    graph.setNodes(QVector<ProjectGraphNode>{main_head, scenario_head, main_intermediate, intermediate, root});
    if (!require(graph.showsFullTree() && graph.omissionCount() == 0,
                 "revision graph must default to Full tree"))
        return 1;
    graph.setShowFullTree(false);
    if (!require(!graph.showsFullTree() && graph.omissionCount() == 2
                     && !graph.revisionViewportPosition(root.id).isNull()
                     && graph.revisionViewportPosition(main_intermediate.id).isNull()
                     && graph.revisionViewportPosition(main_head.id) != QPointF(),
                 "Editable heads must preserve the shared ancestor and fold intervening paths"))
        return 1;
    // Fold markers must remain between the HEAD and older shared ancestry,
    // not float elsewhere in the project corridor.
    const double root_y = graph.revisionViewportPosition(root.id).y();
    const double earliest_head_y = qMin(graph.revisionViewportPosition(main_head.id).y(),
                                         graph.revisionViewportPosition(scenario_head.id).y());
    for (int i = 0; i < graph.omissionCount(); ++i)
    {
        const double marker_y = graph.omissionViewportPosition(i).y();
        if (!require(marker_y > root_y && marker_y < earliest_head_y,
                     "fold marker must lie between older ancestor and HEAD"))
            return 1;
    }
    // Pick the folded history belonging to main, not scenario.
    int main_fold = -1;
    for (int i = 0; i < graph.omissionCount(); ++i)
        if (std::abs(graph.omissionViewportPosition(i).x()
            - graph.revisionViewportPosition(root.id).x()) < 0.1) main_fold = i;
    if (!require(main_fold >= 0, "could not find the main branch history fold"))
        return 1;
    click(graph, graph.omissionViewportPosition(main_fold));
    if (!require(!graph.showsFullTree() && graph.omissionCount() == 1
                     && !graph.revisionViewportPosition(main_intermediate.id).isNull()
                     && graph.revisionViewportPosition(intermediate.id).isNull(),
                 "unfolding a graph omission expanded unrelated history or changed view mode"))
        return 1;
    click(graph, graph.omissionViewportPosition(0));
    if (!require(graph.omissionCount() == 0
                     && !graph.revisionViewportPosition(intermediate.id).isNull(),
                 "unfolding the remaining history path failed"))
        return 1;
    // Mode changes reset local unfolding. Otherwise the two-state control
    // appears to do nothing after every omission has been expanded.
    graph.setShowFullTree(true);
    if (!require(graph.showsFullTree() && graph.omissionCount() == 0,
                 "returning to Full tree did not restore all revisions"))
        return 1;
    graph.setShowFullTree(false);
    if (!require(graph.omissionCount() == 2
                     && graph.revisionViewportPosition(main_intermediate.id).isNull()
                     && graph.revisionViewportPosition(intermediate.id).isNull(),
                 "switching back to Editable heads retained old expanded paths"))
        return 1;
    graph.expandOmission(0);
    graph.setShowFullTree(false); // pressing the selected mode re-folds history
    if (!require(graph.omissionCount() == 2,
                 "clicking Editable heads again must collapse expanded history"))
        return 1;
    graph.setNodes(QVector<ProjectGraphNode>{scenario_head, intermediate, root});
    if (!require(graph.omissionCount() == 1
                     && graph.revisionViewportPosition(root.id).isNull(),
                 "loading a new DAG retained expansion state from the old graph"))
        return 1;
    graph.setShowFullTree(true);
    graph.setNodes(QVector<ProjectGraphNode>{main_head, scenario_head, main_intermediate, intermediate, root});
    graph.setShowFullTree(false);
    graph.setSelection(intermediate.id);
    if (!require(!graph.showsFullTree()
                     && !graph.revisionViewportPosition(intermediate.id).isNull(),
                 "selecting history in the sidebar must unfold its graph branch"))
        return 1;
    graph.setShowFullTree(true);
    graph.setNodes(QVector<ProjectGraphNode>{node});
    graph.fitGraph();

    // A graph populated before its containing page first appears must fit
    // against the *final* visible size, not its unlaid-out default size.
    QWidget graph_page;
    QVBoxLayout page_layout(&graph_page);
    page_layout.setContentsMargins(0, 0, 0, 0);
    ProjectRevisionGraph *first_show_graph = new ProjectRevisionGraph(&graph_page);
    page_layout.addWidget(first_show_graph);
    ProjectGraphNode start_node = latest;
    start_node.position = QPointF(0.0, 0.0);
    start_node.parents.clear();
    ProjectGraphNode end_node = previous;
    end_node.position = QPointF(0.0, 500.0);
    first_show_graph->setNodes(QVector<ProjectGraphNode>{start_node, end_node});
    graph_page.resize(960, 340);
    graph_page.show();
    app.processEvents();
    waitFor(110); // dock/splitter startup geometry must settle before fitting
    const double expected_fit = qBound(0.25,
        qMin((first_show_graph->width() - 32.0) / (224.0 + 152.0),
             (first_show_graph->height() - 32.0) / (500.0 + ProjectRevisionCardGeometry::height(1) + 170.0)), 1.5);
    if (!require(std::abs(first_show_graph->zoomFactor() - expected_fit) < 0.02,
                 "initial graph was not fitted after the page received its final size"))
        return 1;
    // Simulate a late dock/splitter resize during the FIRST show. The
    // automatic fit must use the new geometry, without any user button press.
    QWidget late_layout_page;
    QVBoxLayout late_layout(&late_layout_page);
    late_layout.setContentsMargins(0, 0, 0, 0);
    ProjectRevisionGraph *late_graph = new ProjectRevisionGraph(&late_layout_page);
    late_layout.addWidget(late_graph);
    late_graph->setNodes(QVector<ProjectGraphNode>{start_node, end_node});
    late_layout_page.resize(800, 300);
    late_layout_page.show();
    app.processEvents();
    late_layout_page.resize(1020, 650);
    app.processEvents();
    waitFor(110);
    const double late_fit = qBound(0.25,
        qMin((late_graph->width() - 32.0) / (224.0 + 152.0),
             (late_graph->height() - 32.0) / (500.0 + ProjectRevisionCardGeometry::height(1) + 170.0)), 1.5);
    if (!require(std::abs(late_graph->zoomFactor() - late_fit) < 0.02,
                 "initial fit used stale geometry after a delayed page resize"))
        return 1;
    late_layout_page.close();
    // A genuinely new graph fits automatically without the Fit button.
    first_show_graph->setNodes(QVector<ProjectGraphNode>{node});
    const QPointF new_graph_center = first_show_graph->graphToViewport(node.position);
    if (!require(std::abs(new_graph_center.x() - first_show_graph->width() / 2.0) < 1.0
                     && std::abs(new_graph_center.y() - first_show_graph->height() / 2.0) < 1.0,
                 "loading a different graph did not automatically center it"))
        return 1;
    graph_page.close();

    // A small fitted graph is pinned in the viewport: panning cannot move
    // every card offscreen, even with a huge mouse drag.
    graph.setNodes({node});
    graph.fitGraph();
    sendMouse(graph, QEvent::MouseButtonPress, QPointF(300.0, 200.0), Qt::LeftButton);
    sendMouse(graph, QEvent::MouseMove, QPointF(760.0, 400.0), Qt::LeftButton);
    sendMouse(graph, QEvent::MouseButtonRelease, QPointF(760.0, 400.0), Qt::NoButton);
    if (!require(std::abs(graph.revisionViewportPosition(node.id).x() - graph.width() / 2.0) < 0.01
                     && std::abs(graph.revisionViewportPosition(node.id).y() - graph.height() / 2.0) < 0.01,
                 "small fitted graph can be dragged away from the viewport")) return 1;

    // Navigation tests need an actual scrollable canvas. The bounds here
    // exceed the available viewport in X; Y stays centred when undersized.
    ProjectGraphNode far_left = node;
    far_left.id = QStringLiteral("far-left");
    far_left.position = QPointF(-1000, 0);
    far_left.heads.clear();
    ProjectGraphNode far_right = far_left;
    far_right.id = QStringLiteral("far-right");
    far_right.position = QPointF(1000, 0);
    graph.setNodes({node, far_left, far_right});
    graph.fitGraph();

    // Key movement accelerates while held, then glides to a stop when released.
    graph.fitGraph();
    const double initial_x = graph.graphToViewport(QPointF(0.0, 0.0)).x();
    sendKey(graph, QEvent::KeyPress, Qt::Key_A);
    waitFor(90);
    const double holding_x = graph.graphToViewport(QPointF(0.0, 0.0)).x();
    if (!require(holding_x > initial_x + 1.0, "A key did not pan the graph"))
        return 1;
    sendKey(graph, QEvent::KeyRelease, Qt::Key_A);
    waitFor(45);
    if (!require(graph.graphToViewport(QPointF(0.0, 0.0)).x() > holding_x,
                 "keyboard pan stopped abruptly instead of decelerating"))
        return 1;

    // Same transform drives both rendering and hit testing after navigation.
    graph.fitGraph();
    revision_clicked.clear();
    branch_clicked.clear();
    sendKey(graph, QEvent::KeyPress, Qt::Key_D);
    waitFor(85);
    sendKey(graph, QEvent::KeyRelease, Qt::Key_D);
    waitFor(100);
    click(graph, graph.graphToViewport(QPointF(0.0, 0.0)));
    if (!require(revision_clicked == node.id, "revision selection broke after keyboard pan"))
        return 1;
    click(graph, graph.graphToViewport(QPointF(-50.0, 25.0)));
    if (!require(branch_clicked == node.branch, "branch badge selection broke after keyboard pan"))
        return 1;

    // The map's critically damped zoom motion keeps the point under the
    // wheel pointer fixed, including during the animation.
    graph.fitGraph();
    const QPointF wheel_anchor(400.0, 250.0);
    const double before_zoom = graph.zoomFactor();
    const QPointF before_center = graph.graphToViewport(QPointF());
    const QPointF fixed_graph_point = (wheel_anchor - before_center) / before_zoom;
    sendWheel(graph, wheel_anchor, QPoint(0, 120));
    waitFor(130);
    if (!require(graph.zoomFactor() > before_zoom, "wheel did not zoom in"))
        return 1;
    if (!require(std::hypot((graph.graphToViewport(fixed_graph_point) - wheel_anchor).x(),
                            (graph.graphToViewport(fixed_graph_point) - wheel_anchor).y()) < 0.01,
                 "centered wheel zoom lost its anchor without reaching the scene boundary"))
        return 1;

    graph.fitGraph();
    const double before_e = graph.zoomFactor();
    sendKey(graph, QEvent::KeyPress, Qt::Key_E);
    waitFor(85);
    sendKey(graph, QEvent::KeyRelease, Qt::Key_E);
    waitFor(100);
    if (!require(graph.zoomFactor() > before_e, "E key did not smoothly zoom in"))
        return 1;
    sendKey(graph, QEvent::KeyPress, Qt::Key_Q);
    waitFor(160);
    sendKey(graph, QEvent::KeyRelease, Qt::Key_Q);
    waitFor(140);
    if (!require(graph.zoomFactor() < before_e + 0.15, "Q key did not zoom out"))
        return 1;

    // A sufficiently zoomed-in scene has room for inertia; it must stop at
    // content bounds rather than allowing the entire graph to disappear.
    graph.fitGraph();
    sendWheel(graph, QPointF(400, 250), QPoint(0, 480));
    waitFor(260);
    sendMouse(graph, QEvent::MouseButtonPress, QPointF(180.0, 130.0), Qt::LeftButton);
    waitFor(20);
    sendMouse(graph, QEvent::MouseMove, QPointF(225.0, 130.0), Qt::LeftButton);
    waitFor(25);
    sendMouse(graph, QEvent::MouseMove, QPointF(267.0, 130.0), Qt::LeftButton);
    sendMouse(graph, QEvent::MouseButtonRelease, QPointF(267.0, 130.0), Qt::NoButton);
    const double at_release = graph.graphToViewport(QPointF()).x();
    waitFor(60);
    if (!require(graph.graphToViewport(QPointF()).x() > at_release + 1.0,
                 "mouse drag did not carry momentum while inside scene bounds"))
        return 1;
    // Extreme pan in either direction cannot push the scene entirely away.
    for (int direction : {-1, 1})
    {
        const QPointF origin(370.0, 210.0);
        sendMouse(graph, QEvent::MouseButtonPress, origin, Qt::LeftButton);
        sendMouse(graph, QEvent::MouseMove, origin + QPointF(direction * 20000.0, 0), Qt::LeftButton);
        sendMouse(graph, QEvent::MouseButtonRelease,
                  origin + QPointF(direction * 20000.0, 0), Qt::NoButton);
        waitFor(40);
        const QRectF bounds = graph.graphBounds();
        const double left = graph.graphToViewport(bounds.topLeft()).x();
        const double right = graph.graphToViewport(bounds.bottomRight()).x();
        if (!require(left <= 111.0 && right >= graph.width() - 111.0,
                     "drag or inertia escaped the content bounding box")) return 1;
    }

    // The graph and map must consume the very same editable shortcuts. These
    // changes go through the real settings registry (and its change signals).
    GuiShortcutRegistry &shortcuts = guiShortcutRegistry();
    QString binding_error;
    if (!require(shortcuts.setShortcut(GuiShortcutId::MapPanLeft,
                                      QKeySequence(QStringLiteral("Ctrl+J")), &binding_error),
                 "could not remap pan left"))
        return 1;
    if (!require(shortcuts.setShortcut(GuiShortcutId::MapZoomIn,
                                      QKeySequence(QStringLiteral("Alt+I")), &binding_error),
                 "could not remap zoom in"))
        return 1;
    if (!require(graph.toolTip().isEmpty(),
                 "revision graph must not display a navigation hover tooltip"))
        return 1;

    graph.fitGraph();
    const QPointF after_remap_start = graph.graphToViewport(QPointF());
    sendKey(graph, QEvent::KeyPress, Qt::Key_A);
    waitFor(75);
    sendKey(graph, QEvent::KeyRelease, Qt::Key_A);
    if (!require(graph.graphToViewport(QPointF()) == after_remap_start,
                 "old pan key is still active after remapping"))
        return 1;
    sendKey(graph, QEvent::KeyPress, Qt::Key_J, Qt::ControlModifier);
    waitFor(90);
    const double remapped_pan_x = graph.graphToViewport(QPointF()).x();
    if (!require(remapped_pan_x > after_remap_start.x() + 1.0,
                 "Ctrl+J did not pan using the updated setting"))
        return 1;
    // The modifier was released first; a release of the physical key must
    // still clear the held direction and begin inertia instead of sticking.
    sendKey(graph, QEvent::KeyRelease, Qt::Key_J);
    waitFor(50);
    if (!require(graph.graphToViewport(QPointF()).x() > remapped_pan_x,
                 "remapped pan key did not coast after modifier-less release"))
        return 1;

    graph.fitGraph();
    const double zoom_before_remap = graph.zoomFactor();
    sendKey(graph, QEvent::KeyPress, Qt::Key_E);
    waitFor(75);
    sendKey(graph, QEvent::KeyRelease, Qt::Key_E);
    if (!require(graph.zoomFactor() == zoom_before_remap,
                 "old zoom key remained active after remapping"))
        return 1;
    sendKey(graph, QEvent::KeyPress, Qt::Key_I, Qt::AltModifier);
    waitFor(95);
    sendKey(graph, QEvent::KeyRelease, Qt::Key_I);
    waitFor(65);
    if (!require(graph.zoomFactor() > zoom_before_remap,
                 "Alt+I did not zoom using the updated setting"))
        return 1;

    // Rebinding during a held key must cancel it instead of leaking a
    // persistent navigation state across the settings change.
    graph.fitGraph();
    sendKey(graph, QEvent::KeyPress, Qt::Key_J, Qt::ControlModifier);
    waitFor(65);
    if (!require(shortcuts.setShortcut(GuiShortcutId::MapPanLeft,
                                      QKeySequence(QStringLiteral("Ctrl+H")), &binding_error),
                 "could not rebind an active pan direction"))
        return 1;
    const QPointF position_at_rebind = graph.graphToViewport(QPointF());
    waitFor(90);
    if (!require(graph.graphToViewport(QPointF()) == position_at_rebind,
                 "changing a held shortcut left navigation running"))
        return 1;
    sendKey(graph, QEvent::KeyRelease, Qt::Key_J);

    // Losing focus must not leave any held navigation key moving the graph.
    graph.fitGraph();
    sendKey(graph, QEvent::KeyPress, Qt::Key_W);
    waitFor(55);
    QFocusEvent lose_focus(QEvent::FocusOut);
    QCoreApplication::sendEvent(&graph, &lose_focus);
    const QPointF focus_position = graph.graphToViewport(QPointF());
    waitFor(70);
    if (!require(graph.graphToViewport(QPointF()) == focus_position,
                 "focus loss did not stop navigation"))
        return 1;

    // Server and project cards are structural. The graph contains *only*
    // immutable revision nodes: branches are clickable HEAD decorations.
    ProjectGraphNode server_card;
    server_card.kind = ProjectGraphNode::Kind::Server;
    server_card.id = QStringLiteral("group:server:test-source");
    server_card.title = QStringLiteral("Test database");
    server_card.position = QPointF(0, 310);
    // The personal database uses the same structural server card, but a
    // golden surface and border; ordinary servers retain the teal palette.
    // Test the rendered result to catch a missing graph-node designation.
    graph.setNodes({server_card});
    graph.setSelection(QString());
    graph.fitGraph();
    app.processEvents();
    const QPointF teal_center = graph.nodeViewportPosition(server_card.id);
    const QPointF teal_sample = teal_center + QPointF(-117.0 * graph.zoomFactor(), 0.0);
    const QImage teal_image = graph.grab().toImage();
    const QColor teal_pixel = teal_image.pixelColor(teal_sample.toPoint());
    if (!require(teal_pixel.green() > teal_pixel.red(),
                 "ordinary server card lost its teal surface")) return 1;
    server_card.personal_server = true;
    graph.setNodes({server_card});
    app.processEvents();
    const QPointF gold_center = graph.nodeViewportPosition(server_card.id);
    const QPointF gold_sample = gold_center + QPointF(-117.0 * graph.zoomFactor(), 0.0);
    const QImage gold_image = graph.grab().toImage();
    const QColor gold_pixel = gold_image.pixelColor(gold_sample.toPoint());
    if (!require(gold_pixel.red() > gold_pixel.green()
                     && gold_pixel.red() > teal_pixel.red(),
                 "personal working server card is not golden")) return 1;
    server_card.personal_server = false;

    ProjectGraphNode project_card;
    project_card.kind = ProjectGraphNode::Kind::Project;
    project_card.id = QStringLiteral("group:project:test-project");
    project_card.title = QStringLiteral("Test project");
    project_card.container_id = server_card.id;
    project_card.position = QPointF(0, 155);
    ProjectGraphNode revision_card = node;
    revision_card.id = QStringLiteral("revision-with-project");
    revision_card.project_group_id = project_card.id;
    revision_card.position = QPointF(0, 0);
    revision_card.heads = {{QStringLiteral("test-branch"), QStringLiteral("main")}};
    graph.setShowFullTree(true);
    graph.setNodes({server_card, project_card, revision_card});
    graph.fitGraph();
    QString selected_source;
    QString selected_project;
    QObject::connect(&graph, &ProjectRevisionGraph::serverActivated,
        &graph, [&selected_source](const QString &id) { selected_source = id; });
    QObject::connect(&graph, &ProjectRevisionGraph::projectActivated,
        &graph, [&selected_project](const QString &id) { selected_project = id; });
    click(graph, graph.nodeViewportPosition(server_card.id));
    click(graph, graph.nodeViewportPosition(project_card.id));
    if (!require(selected_source == QStringLiteral("test-source")
                 && selected_project == QStringLiteral("test-project"),
                 "server/project cards did not open their inspectors")) return 1;
    QString selected_branch;
    QObject::connect(&graph, &ProjectRevisionGraph::branchActivated,
        &graph, [&selected_branch](const QString &id) { selected_branch = id; });
    // The clickable HEAD chip belongs INSIDE the revision card.
    const QPointF center = graph.revisionViewportPosition(revision_card.id);
    const QPointF badge_point = center + QPointF(-30 * graph.zoomFactor(),
                                                 25 * graph.zoomFactor());
    click(graph, badge_point);
    if (!require(selected_branch == QStringLiteral("test-branch"),
                 "HEAD decoration did not select its branch")) return 1;
    graph.setSelection(QStringLiteral("group:branch:test-branch"));
    if (!require(graph.selectionId() == QStringLiteral("group:branch:test-branch"),
                 "sidebar branch selection did not highlight its HEAD decoration")) return 1;
    graph.setShowFullTree(false);
    if (!require(!graph.nodeViewportPosition(server_card.id).isNull()
                 && !graph.nodeViewportPosition(project_card.id).isNull(),
                 "heads-only view removed structural project/server cards")) return 1;

    // A single ancestry chain remains centered. A branch decoration occupies
    // no independent level and cannot push the HEAD into a second lane.
    QVector<ProjectGraphNode> straight{server_card, project_card, revision_card};
    ProjectGraphPlacement::place(straight, 1.8);
    if (!require(straight.size() == 3
                 && straight.at(0).position.x() == straight.at(1).position.x()
                 && straight.at(1).position.x() == straight.at(2).position.x(),
                 "branch labels created extra graph nodes or shifted ancestry")) return 1;

    // Four commits: oldest -> newer -> HEAD, including a historical branch
    // HEAD that has descendants on another branch.
    QVector<ProjectGraphNode> chronological{server_card, project_card};
    QString previous_revision;
    for (int i = 0; i < 4; ++i)
    {
        ProjectGraphNode revision = revision_card;
        revision.id = QStringLiteral("chronological-%1").arg(i);
        revision.heads.clear();
        revision.parents.clear();
        if (!previous_revision.isEmpty()) revision.parents.append(previous_revision);
        if (i == 1) revision.heads.append({QStringLiteral("old-feature"), QStringLiteral("feature")});
        if (i == 3) revision.heads.append({QStringLiteral("main"), QStringLiteral("main")});
        chronological.append(revision);
        previous_revision = revision.id;
    }
    ProjectGraphPlacement::place(chronological);
    for (int i = 1; i < 4; ++i)
        if (!require(chronological.at(i + 1).position.y()
                     > chronological.at(i + 2).position.y(),
                     "ancestry must descend oldest to HEAD")) return 1;
    if (!require(chronological.size() == 6
                 && chronological.at(3).heads.size() == 1
                 && chronological.at(5).heads.size() == 1,
                 "historical non-leaf branch HEAD must stay on its revision")) return 1;

    // Three named branches sharing exactly one immutable HEAD. All labels
    // remain independently addressable, with sufficient card/rank spacing.
    QVector<ProjectGraphNode> shared{server_card, project_card};
    ProjectGraphNode old_revision = revision_card;
    old_revision.id = QStringLiteral("shared-old");
    old_revision.parents.clear();
    old_revision.heads.clear();
    shared.append(old_revision);
    ProjectGraphNode shared_head = revision_card;
    shared_head.id = QStringLiteral("shared-head");
    shared_head.parents = {old_revision.id};
    shared_head.heads = {{QStringLiteral("a"), QStringLiteral("alpha")},
                         {QStringLiteral("b"), QStringLiteral("beta")},
                         {QStringLiteral("c"), QStringLiteral("gamma")}};
    shared.append(shared_head);
    ProjectGraphPlacement::place(shared);
    if (!require(shared.size() == 4
                 && shared.at(3).position.x() == shared.at(2).position.x()
                 && shared.at(2).position.y() > shared.at(3).position.y(),
                 "shared HEAD must be one revision in the same ancestry lane")) return 1;
    const double gap = shared.at(2).position.y() - shared.at(3).position.y();
    if (!require(gap >= (84.0 + (84.0 + 3.0 * 24.0)) / 2.0 + 40.0,
                 "multiple HEAD labels overlap ancestor cards")) return 1;
    graph.setShowFullTree(true);
    graph.setNodes(shared);
    graph.fitGraph();
    const QPointF head_center = graph.revisionViewportPosition(shared_head.id);
    for (int i = 0; i < 3; ++i)
    {
        selected_branch.clear();
        const double height = 84.0 + shared_head.heads.size() * 24.0;
        click(graph, head_center + QPointF(0, (-height / 2.0 + 78.0 + 24.0 * i)
                                              * graph.zoomFactor()));
        if (!require(selected_branch == shared_head.heads.at(i).id,
                     "shared HEAD labels cannot be clicked independently")) return 1;
    }

    // A UUIDv7 branch has its own identity and selectable visual node,
    // independent of the UUIDv7 revision it references. Three branches can
    // point to the same revision without stacking inside that revision card.
    QVector<ProjectGraphNode> references{server_card, project_card};
    ProjectGraphNode shared_revision = revision_card;
    shared_revision.id = QStringLiteral("uuidv7-revision-1");
    shared_revision.parents.clear();
    shared_revision.heads = {
        {QStringLiteral("uuidv7-branch-1"), QStringLiteral("main")},
        {QStringLiteral("uuidv7-branch-2"), QStringLiteral("revA")},
        {QStringLiteral("uuidv7-branch-3"), QStringLiteral("revAbranch")}
    };
    references.append(shared_revision);
    for (const ProjectGraphBranchHead &head : shared_revision.heads)
    {
        ProjectGraphNode reference;
        reference.kind = ProjectGraphNode::Kind::Branch;
        reference.id = QStringLiteral("group:branch:") + head.id;
        reference.title = head.name;
        reference.branch = QStringLiteral("BRANCH · HEAD");
        reference.project_group_id = project_card.id;
        reference.head_revision_id = shared_revision.id;
        references.append(reference);
    }
    ProjectGraphPlacement::place(references);
    if (!require(references.size() == 6 && references.at(2).heads.size() == 3
                 && references.at(2).parents.isEmpty(),
                 "branch reference cards created additional immutable revisions")) return 1;
    QSet<int> reference_columns;
    for (int index = 3; index < references.size(); ++index)
    {
        if (!require(references.at(index).position.y() < references.at(2).position.y()
                     && references.at(index).head_revision_id == shared_revision.id
                     && references.at(index).parents.isEmpty(),
                     "branch node does not reference the same UUIDv7 HEAD")) return 1;
        reference_columns.insert(qRound(references.at(index).position.x()));
        for (int earlier = 3; earlier < index; ++earlier)
            if (!require(std::abs(references.at(index).position.x()
                     - references.at(earlier).position.x()) >= 196.0,
                     "independent branch cards overlap")) return 1;
    }
    if (!require(reference_columns.size() == 3,
                 "three branch UUIDs collapsed into one graph leaf")) return 1;
    graph.setShowFullTree(true);
    graph.setNodes(references);
    graph.fitGraph();
    for (int index = 3; index < references.size(); ++index)
    {
        selected_branch.clear();
        click(graph, graph.nodeViewportPosition(references.at(index).id));
        if (!require(selected_branch == shared_revision.heads.at(index - 3).id,
                     "independent UUIDv7 branch card did not activate its own branch")) return 1;
    }
    graph.setShowFullTree(false);
    for (int index = 3; index < references.size(); ++index)
        if (!require(!graph.nodeViewportPosition(references.at(index).id).isNull(),
                     "heads-only graph hid a branch reference card")) return 1;

    // A branch created from main must have ONE visual source (main), not
    // separate arrows from main AND the same revision. HEAD remains a real
    // immutable revision pointer regardless of how the cards are connected.
    references[4].origin_branch_node_id = references[3].id;
    references[5].origin_branch_node_id = references[4].id;
    ProjectGraphPlacement::place(references);
    if (!require(references[3].parent_routes.size() == 1
                     && references[3].parent_routes.contains(shared_revision.id)
                     && references[4].parent_routes.size() == 1
                     && references[4].parent_routes.contains(references[3].id)
                     && references[5].parent_routes.size() == 1
                     && references[5].parent_routes.contains(references[4].id),
                 "branch creation drew two incoming links or lost its source branch")) return 1;
    if (!require(references[4].head_revision_id == shared_revision.id
                     && references[5].head_revision_id == shared_revision.id
                     && references[4].parents.isEmpty()
                     && references[5].parents.isEmpty(),
                 "branch provenance changed the immutable HEAD or revision ancestry")) return 1;
    if (!require(references[3].position.y() > references[4].position.y()
                     && references[4].position.y() > references[5].position.y(),
                 "creation-source branches must precede their children")) return 1;
    graph.setShowFullTree(true);
    graph.setNodes(references);
    graph.fitGraph();
    for (int index = 3; index < references.size(); ++index)
        if (!require(!graph.nodeViewportPosition(references[index].id).isNull(),
                     "branch lineage obscured a selectable branch reference")) return 1;

    // Deleting a persisted origin must not leave the child disconnected or
    // manufacture a replacement branch. Its HEAD revision is the fallback.
    references[4].origin_branch_node_id = QStringLiteral("group:branch:deleted");
    ProjectGraphPlacement::place(references);
    if (!require(references[4].parent_routes.size() == 1
                     && references[4].parent_routes.contains(shared_revision.id)
                     && references[4].origin_branch_node_id
                         == QStringLiteral("group:branch:deleted"),
                 "missing origin must fall back to the HEAD revision")) return 1;

    // Placement regression: database roots must share one horizontal layer
    // even with very uneven project widths and revision history depths.
    QVector<ProjectGraphNode> compound;
    for (int database_index = 0; database_index < 8; ++database_index)
    {
        const QString database_id = QStringLiteral("group:server:db-%1").arg(database_index);
        ProjectGraphNode database;
        database.id = database_id;
        database.kind = ProjectGraphNode::Kind::Server;
        compound.append(database);
        const int project_count = database_index == 0 ? 5 : (database_index == 7 ? 0 : 1);
        for (int project_index = 0; project_index < project_count; ++project_index)
        {
            const QString project_id = QStringLiteral("group:project:db-%1:p-%2")
                .arg(database_index).arg(project_index);
            ProjectGraphNode project;
            project.id = project_id;
            project.kind = ProjectGraphNode::Kind::Project;
            project.container_id = database_id;
            compound.append(project);
            const int history_length = project_index == 0 && database_index == 0 ? 9 : 2;
            for (int revision_index = 0; revision_index < history_length; ++revision_index)
            {
                ProjectGraphNode revision;
                revision.id = QStringLiteral("db-%1:p-%2:r-%3")
                    .arg(database_index).arg(project_index).arg(revision_index);
                revision.project_group_id = project_id;
                if (revision_index > 0)
                    revision.parents.append(QStringLiteral("db-%1:p-%2:r-%3")
                        .arg(database_index).arg(project_index).arg(revision_index - 1));
                compound.append(revision);
            }
        }
    }
    ProjectGraphPlacement::place(compound, 1.8);
    QSet<int> database_rows;
    QSet<int> project_rows;
    QVector<double> server_x_positions;
    QHash<QString, QPointF> baseline_positions;
    for (const ProjectGraphNode &placed : compound)
    {
        baseline_positions.insert(placed.id, placed.position);
        if (placed.kind == ProjectGraphNode::Kind::Server)
        {
            database_rows.insert(qRound(placed.position.y()));
            server_x_positions.append(placed.position.x());
        }
        if (placed.kind == ProjectGraphNode::Kind::Project)
            project_rows.insert(qRound(placed.position.y()));
        if (placed.kind == ProjectGraphNode::Kind::Revision)
        {
            for (const QString &parent_id : placed.parents)
                for (const ProjectGraphNode &parent : compound)
                    if (parent.id == parent_id
                        && !require(parent.position.y() > placed.position.y(),
                            "older revisions must precede newer descendants")) return 1;
        }
    }
    if (!require(database_rows.size() == 1 && database_rows.contains(0),
                 "database cards must share the topmost horizontal layer")) return 1;
    if (!require(project_rows.size() == 1 && project_rows.contains(-142),
                 "projects from different servers must share the next layer")) return 1;
    for (int index = 1; index < server_x_positions.size(); ++index)
        if (!require(server_x_positions.at(index) - server_x_positions.at(index - 1) >= 252.0,
                     "database cards are not horizontally ordered and separated")) return 1;
    // The viewport aspect is no longer allowed to wrap servers onto another
    // row or otherwise move their cards when the window is resized.
    ProjectGraphPlacement::place(compound, 0.7);
    for (const ProjectGraphNode &placed : compound)
        if (!require(placed.position == baseline_positions.value(placed.id),
                     "resizing the viewport changed database row placement")) return 1;
    ProjectGraphPlacement::place(compound, 2.7);
    for (const ProjectGraphNode &placed : compound)
        if (!require(placed.position == baseline_positions.value(placed.id),
                     "wide viewport changed database row placement")) return 1;
    for (int index = 0; index < compound.size(); ++index)
    {
        const ProjectGraphNode &lhs = compound.at(index);
        const double lw = lhs.kind == ProjectGraphNode::Kind::Server ? 252.0 : 224.0;
        const double lh = lhs.kind == ProjectGraphNode::Kind::Server ? 76.0
            : lhs.kind == ProjectGraphNode::Kind::Project ? 68.0 : 84.0;
        for (int other = index + 1; other < compound.size(); ++other)
        {
            const ProjectGraphNode &rhs = compound.at(other);
            const double rw = rhs.kind == ProjectGraphNode::Kind::Server ? 252.0 : 224.0;
            const double rh = rhs.kind == ProjectGraphNode::Kind::Server ? 76.0
                : rhs.kind == ProjectGraphNode::Kind::Project ? 68.0 : 84.0;
            const bool collision = std::abs(lhs.position.x() - rhs.position.x()) < (lw + rw) / 2.0
                && std::abs(lhs.position.y() - rhs.position.y()) < (lh + rh) / 2.0;
            if (!require(!collision, "compound layout allows overlapping node cards")) return 1;
        }
    }

    // Same-depth branches must have separate cards even when their textual
    // branch label is identical. Order should be stable across API responses.
    QVector<ProjectGraphNode> same_branch;
    ProjectGraphNode db = server_card;
    db.id = QStringLiteral("group:server:stable");
    ProjectGraphNode proj = project_card;
    proj.id = QStringLiteral("group:project:stable");
    proj.container_id = db.id;
    same_branch.append(db);
    same_branch.append(proj);
    for (const QString &suffix : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")})
    {
        ProjectGraphNode rev;
        rev.id = suffix;
        rev.branch = QStringLiteral("main");
        rev.project_group_id = proj.id;
        same_branch.append(rev);
    }
    ProjectGraphPlacement::place(same_branch, 1.8);
    QHash<QString, QPointF> expected;
    for (const ProjectGraphNode &entry : same_branch)
        expected.insert(entry.id, entry.position);
    if (!require(std::abs(expected.value(QStringLiteral("a")).x()
                          - expected.value(QStringLiteral("b")).x()) >= 224.0,
                 "same-branch revisions at one layer overlap")) return 1;
    std::swap(same_branch[2], same_branch[4]);
    ProjectGraphPlacement::place(same_branch, 1.8);
    for (const ProjectGraphNode &entry : same_branch)
        if (!require(entry.position == expected.value(entry.id),
                     "fetch order changed deterministic graph placement")) return 1;

    // Built-in layered-DAG placement with measured node heights and routed edges.
    // A forked/merged project must reserve its complete rectangular corridor,
    // including the spline lanes for multi-parent joins and the taller
    // revision cards with several branch HEAD references.
    QVector<ProjectGraphNode> dag_compound;
    ProjectGraphNode graph_server = server_card;
    graph_server.id = QStringLiteral("group:server:native-dag-regression");
    dag_compound.append(graph_server);
    const QStringList project_names = {QStringLiteral("forked"),
                                       QStringLiteral("neighbour")};
    for (const QString &name : project_names)
    {
        ProjectGraphNode owner = project_card;
        owner.id = QStringLiteral("group:project:native-dag-%1").arg(name);
        owner.container_id = graph_server.id;
        dag_compound.append(owner);
        const int revision_count = name == QStringLiteral("forked") ? 7 : 2;
        for (int i = 0; i < revision_count; ++i)
        {
            ProjectGraphNode rev = revision_card;
            rev.id = QStringLiteral("native-dag:%1:%2").arg(name).arg(i);
            rev.project_group_id = owner.id;
            rev.container_id.clear();
            rev.parents.clear();
            rev.heads.clear();
            if (name == QStringLiteral("forked"))
            {
                // R0 -> R1 -> R2 -> R4 -> R6; and R1 -> R3 -> R5 -> R6.
                // R4's second child also creates a long-rank connection.
                const QVector<QVector<int>> ancestors = {
                    {}, {0}, {1}, {1}, {2}, {3, 2}, {4, 5}};
                for (int parent : ancestors.at(i))
                    rev.parents.append(QStringLiteral("native-dag:forked:%1").arg(parent));
                if (i == 2)
                    rev.heads.append({QStringLiteral("native-dag:historic"), QStringLiteral("old-tip")});
                if (i == 6)
                    rev.heads = {{QStringLiteral("native-dag:main"), QStringLiteral("main")},
                                 {QStringLiteral("native-dag:release"), QStringLiteral("release")},
                                 {QStringLiteral("native-dag:another"), QStringLiteral("experiment")},
                                 {QStringLiteral("native-dag:extra"), QStringLiteral("topic")}};
            }
            else
            {
                if (i > 0) rev.parents.append(QStringLiteral("native-dag:neighbour:0"));
                if (i == 1)
                    rev.heads.append({QStringLiteral("native-dag:neighbour-head"), QStringLiteral("main")});
            }
            dag_compound.append(rev);
        }
    }
    ProjectGraphPlacement::place(dag_compound);
    QHash<QString, ProjectGraphNode> by_dag_id;
    for (const ProjectGraphNode &node : dag_compound)
        by_dag_id.insert(node.id, node);
    for (int i = 0; i < dag_compound.size(); ++i)
    {
        const ProjectGraphNode &node = dag_compound.at(i);
        const QRectF card = node.kind == ProjectGraphNode::Kind::Revision
            ? ProjectRevisionCardGeometry::bounds(node.position, node.heads.size())
            : QRectF(node.position.x() - (node.kind == ProjectGraphNode::Kind::Server ? 126.0 : 112.0),
                     node.position.y() - (node.kind == ProjectGraphNode::Kind::Server ? 38.0 : 34.0),
                     node.kind == ProjectGraphNode::Kind::Server ? 252.0 : 224.0,
                     node.kind == ProjectGraphNode::Kind::Server ? 76.0 : 68.0);
        for (const QString &parent_id : node.parents)
        {
            const ProjectGraphNode parent = by_dag_id.value(parent_id);
            if (!require(parent.position.y() > node.position.y(),
                         "layered DAG positioned a child above its ancestor")) return 1;
        }
        for (int j = i + 1; j < dag_compound.size(); ++j)
        {
            const ProjectGraphNode &other = dag_compound.at(j);
            const QRectF other_card = other.kind == ProjectGraphNode::Kind::Revision
                ? ProjectRevisionCardGeometry::bounds(other.position, other.heads.size())
                : QRectF(other.position.x() - (other.kind == ProjectGraphNode::Kind::Server ? 126.0 : 112.0),
                         other.position.y() - (other.kind == ProjectGraphNode::Kind::Server ? 38.0 : 34.0),
                         other.kind == ProjectGraphNode::Kind::Server ? 252.0 : 224.0,
                         other.kind == ProjectGraphNode::Kind::Server ? 76.0 : 68.0);
            if (!require(!card.intersects(other_card),
                         "measured DAG node cards overlap across projects or ranks")) return 1;
        }
    }
    {
        for (const ProjectGraphNode &node : dag_compound)
        {
            if (node.kind != ProjectGraphNode::Kind::Revision)
                continue;
            if (!require(node.parent_routes.size() == node.parents.size(),
                         "Native layout did not return a cubic route for each parent edge")) return 1;
            for (const QString &parent_id : node.parents)
            {
                const QVector<QPointF> curve = node.parent_routes.value(parent_id);
                if (!require(curve.size() >= 4 && (curve.size() - 1) % 3 == 0,
                             "Native layout provided an invalid Bezier route")) return 1;
                for (const QPointF &point : curve)
                    if (!require(std::isfinite(point.x()) && std::isfinite(point.y()),
                                 "Native layout returned non-finite spline coordinates")) return 1;
            }
        }
    }
    std::reverse(dag_compound.begin() + 1, dag_compound.end());
    ProjectGraphPlacement::place(dag_compound);
    for (const ProjectGraphNode &node : dag_compound)
        if (!require(node.position == by_dag_id.value(node.id).position,
                     "Native layered layout changed after reordering the catalogue")) return 1;

    // Camera ownership contract: the initial empty canvas may be framed,
    // followed by the first server skeleton and the first real catalogue.
    // Every later selection, rebuild, graph click and mode change preserves
    // the user's camera (subject only to the scene boundary clamp).
    ProjectRevisionGraph stable_camera;
    stable_camera.resize(820, 520);
    stable_camera.show();
    waitFor(110); // the initial empty-canvas fit may already have occurred
    ProjectGraphNode pending_server = server_card;
    pending_server.id = QStringLiteral("group:server:delayed");
    pending_server.position = QPointF(900, 600);
    stable_camera.setNodes({pending_server});
    waitFor(110);
    if (!require(std::abs(stable_camera.nodeViewportPosition(pending_server.id).x() - 410.0) < 2.0,
                 "the first available server did not receive its initial fit")) return 1;

    ProjectGraphNode left_project = project_card;
    left_project.id = QStringLiteral("group:project:camera-left");
    left_project.container_id = pending_server.id;
    left_project.position = QPointF(-1600, -200);
    ProjectGraphNode right_project = left_project;
    right_project.id = QStringLiteral("group:project:camera-right");
    right_project.position = QPointF(1600, 200);
    pending_server.position = QPointF(0, -200);
    stable_camera.setNodes({pending_server, left_project, right_project});
    waitFor(110);
    const double first_catalogue_zoom = stable_camera.zoomFactor();
    if (!require(first_catalogue_zoom < 0.5,
                 "first real catalogue was not framed after the server skeleton")) return 1;

    sendWheel(stable_camera, QPointF(410, 260), QPoint(0, 480));
    waitFor(700);
    const double manual_zoom = stable_camera.zoomFactor();
    if (!require(manual_zoom > first_catalogue_zoom + 0.1,
                 "could not establish a user-controlled camera zoom")) return 1;
    const QPointF viewport_before = stable_camera.graphToViewport(QPointF(0, 0));
    stable_camera.setSelection(right_project.id); // deliberately outside the viewport
    if (!require(stable_camera.selectionId() == right_project.id
                     && stable_camera.zoomFactor() == manual_zoom
                     && stable_camera.graphToViewport(QPointF(0, 0)) == viewport_before,
                 "selecting an off-screen project moved or fitted the camera")) return 1;

    // Simulate a selection-triggered graph rebuild and a background catalogue
    // refresh. Neither must undo the user's zoom or pan.
    std::swap(left_project.title, right_project.title);
    stable_camera.setNodes({right_project, pending_server, left_project});
    if (!require(stable_camera.zoomFactor() == manual_zoom
                     && stable_camera.graphToViewport(QPointF(0, 0)) == viewport_before,
                 "rebuilding graph nodes changed the established camera")) return 1;
    stable_camera.setSelection(left_project.id);
    if (!require(stable_camera.zoomFactor() == manual_zoom
                     && stable_camera.graphToViewport(QPointF(0, 0)) == viewport_before,
                 "switching selected projects moved the camera")) return 1;

    // Layout display controls may change the geometry, but never the zoom.
    stable_camera.setShowFullTree(false);
    stable_camera.setShowFullTree(true);
    stable_camera.setTopDown(false);
    stable_camera.setTopDown(true);
    if (!require(stable_camera.zoomFactor() == manual_zoom,
                 "graph mode or orientation controls unexpectedly fitted the camera")) return 1;

    // Double-click is not an undocumented second Fit action.
    sendMouse(stable_camera, QEvent::MouseButtonDblClick, QPointF(410, 260), Qt::LeftButton);
    if (!require(stable_camera.zoomFactor() == manual_zoom,
                 "double-click unexpectedly fitted the graph")) return 1;
    stable_camera.fitGraph(); // only the explicit Fit action resets framing
    if (!require(std::abs(stable_camera.zoomFactor() - manual_zoom) > 0.1
                     && std::abs(stable_camera.zoomFactor() - first_catalogue_zoom) < 0.01,
                 "explicit Fit graph did not restore the catalogue framing")) return 1;

    std::puts("Project revision graph navigation and hierarchy tests passed");
    return 0;
}
