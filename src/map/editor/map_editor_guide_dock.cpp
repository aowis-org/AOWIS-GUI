#include "map/editor/map_editor_guide_dock.h"

#include "common/_sizes.h"

#include <QCloseEvent>
#include <QFrame>
#include <QTextBrowser>
#include <QTextDocument>

MapEditorGuideDock::MapEditorGuideDock(QWidget *parent)
    : QDockWidget("Map Editor Guide", parent)
{
    setMinimumWidth(Sizes::SidebarRightWidth);
    resize(Sizes::SidebarRightWidth, height());
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    QTextBrowser *guide = new QTextBrowser(this);
    guide->setReadOnly(true);
    guide->setOpenLinks(false);
    guide->setOpenExternalLinks(false);
    guide->setFrameShape(QFrame::NoFrame);
    guide->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    guide->setTextInteractionFlags(Qt::TextSelectableByMouse);
    guide->document()->setDocumentMargin(12.0);
    guide->setHtml(R"HTML(
        <h2>Map Editor Controls</h2>
        <p>
            <b>Left mouse:</b> navigate and select.<br>
            <b>Right mouse:</b> place and edit.
        </p>

        <h3>Navigate and select</h3>
        <ul>
            <li><b>Left-drag map:</b> pan.</li>
            <li><b>Mouse wheel:</b> zoom in/out.</li>
            <li><b>Left-click:</b> select an element.</li>
            <li><b>Shift + left-click:</b> add or remove an element from the selection.</li>
            <li><b>Select tool + right-drag map:</b> rectangle selection.</li>
            <li><b>Del:</b> delete selected elements.</li>
        </ul>

        <h3>Place elements</h3>
        <ul>
            <li><b>Nodes:</b> choose a node tool, then right-click the map.</li>
            <li><b>Pipes:</b> right-click the start node, optional bend points, then the end node.</li>
            <li><b>Pumps and valves:</b> right-click the two nodes to connect.</li>
            <li><b>Demand points:</b> place the point, then right-click it and choose <i>Attach to network</i>; right-click a junction or any position on a pipe.</li>
            <li><b>Annotations:</b> choose Annotation, then right-click a map position to open the Annotation Inspector; enter text and choose Create annotation. Left-click an existing annotation to select or drag it.</li>
        </ul>

        <h3>Edit elements</h3>
        <ul>
            <li><b>Right-click an element:</b> open its editing menu.</li>
            <li><b>Right-click a pipe segment:</b> add a vertex.</li>
            <li><b>Right-click a pipe vertex:</b> move, delete, or convert it to a junction.</li>
            <li><b>Demand-point attachment:</b> attachment lines and handles stay visible; right-click a handle to move, reattach, detach, or convert a pipe attachment to a junction.</li>
            <li>After choosing a move action, <b>right-click to confirm</b>.</li>
            <li>During an entity, pipe-vertex, or demand-point attachment move, press <b>Esc</b> to cancel.</li>
        </ul>

        <p><b>Esc:</b> cancel the active action; otherwise return to selection mode.</p>
    )HTML");

    setWidget(guide);
}

bool MapEditorGuideDock::shouldBeVisible() const
{
    return this->map_editor_active && this->edit_network_section_active && this->requested_visible;
}

void MapEditorGuideDock::setMapEditorActive(bool active)
{
    this->map_editor_active = active;
    updateVisibility();
}

void MapEditorGuideDock::setEditNetworkSectionActive(bool active)
{
    this->edit_network_section_active = active;
    updateVisibility();
}

void MapEditorGuideDock::setRequestedVisible(bool visible)
{
    this->requested_visible = visible;
    updateVisibility();
}

void MapEditorGuideDock::closeEvent(QCloseEvent *event)
{
    this->requested_visible = false;
    emit requestedVisibilityChanged(false);
    QDockWidget::closeEvent(event);
}

void MapEditorGuideDock::updateVisibility()
{
    setVisible(shouldBeVisible());
}
