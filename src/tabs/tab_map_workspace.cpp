#include "tabs/tab_map_workspace.h"

#include "map/render/map_render_diagnostics.h"
#include "common/_sizes.h"
#include "tabs/tab_map_editor_container.h"
#include "tabs/map_mode_tool_panel.h"
#include "tabs/map_mode_tab_bar.h"
#include "tabs/map_workspace_viewport_page.h"
#include "tabs/tab_map_monitor_container.h"

#include <QDebug>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

MapWorkspaceWidget::MapWorkspaceWidget(MapEditorContainer *editor,
                                       MapMonitorContainer *monitor, QWidget *parent)
    : QWidget(parent)
{
    Q_ASSERT(editor != nullptr);
    Q_ASSERT(monitor != nullptr);
    this->monitor_controller = monitor;
    this->editor_controller = editor;
    this->shared_map = monitor->getMap();
    Q_ASSERT(this->shared_map == editor->getMap());
    this->monitor_tools = monitor->toolPanel();
    this->editor_tools = editor->toolPanel();
    Q_ASSERT(this->monitor_tools != nullptr);
    Q_ASSERT(this->editor_tools != nullptr);
    Q_ASSERT(monitor->mapViewport() != this->monitor_tools);
    Q_ASSERT(editor->mapViewport() != this->editor_tools);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The single native map viewport stays visible across all five map modes.
    // The monitor controller remains a hidden logic-only widget.
    this->viewport_page = new MapWorkspaceViewportPage(
        monitor, this->monitor_tools, monitor->mapViewport(), this);
    layout->addWidget(this->viewport_page);
    // Keep the map viewport on the SAME page for all five modes.
    // Only the left panel switches. Never remove/reparent the native surface.
    this->mode_tool_stack = new QStackedWidget(this->viewport_page);
    this->viewport_page->layout()->removeWidget(this->monitor_tools);
    editor->QWidget::layout()->removeWidget(this->editor_tools);
    this->mode_tool_stack->addWidget(this->monitor_tools);
    this->mode_tool_stack->addWidget(this->editor_tools);
    this->mode_tool_stack->addWidget(createEmptyModeTools(DrainageMonitor));
    this->mode_tool_stack->addWidget(createEmptyModeTools(DrainageEditor));
    this->management_tools = new MapModeToolPanel(
        editor->managementControls(), Sizes::SidebarMapEditLeftWidth,
        MapModeToolPanel::ScrollbarWidthPolicy::Fixed, true, this->mode_tool_stack);
    this->mode_tool_stack->addWidget(this->management_tools);
    static_cast<QHBoxLayout *>(this->viewport_page->layout())->insertWidget(0, this->mode_tool_stack);
    this->viewport_page->setSidebarWidget(this->mode_tool_stack);
    this->mode_tool_stack->setCurrentIndex(0);
    // The editor is a hidden interaction controller; it owns no visible map.
    editor->setParent(this);
    editor->hide();
    monitor->setEditorInputCanvas(editor->inputCanvas());

    // One authoritative MapModel follows its workspace-owned viewport.
    this->shared_map->model()->setParent(this);

    this->editor_tools->setModeSelector(createModeSelector(DistributionEditor));
    this->monitor_tools->setModeSelector(createModeSelector(DistributionMonitor));
    this->management_tools->setModeSelector(createModeSelector(MapManagement));
}

MapWorkspaceWidget::~MapWorkspaceWidget()
{
    // Editor closures contain pointers into the shared monitor viewport.
    // Dispose of the editor controller before the viewport and its bgfx
    // native window, irrespective of QObject child insertion order.
    if (this->editor_controller != nullptr && this->monitor_controller != nullptr)
    {
        this->editor_controller->activateSharedSurface(false);
        this->monitor_controller->setEditorMode(false);
    }
    if (this->monitor_controller != nullptr)
        this->monitor_controller->setEditorInputCanvas(nullptr);
    delete this->editor_controller;
    this->editor_controller = nullptr;
}

void MapWorkspaceWidget::setMapFullscreen(bool fullscreen)
{
    // Hiding a tool panel *inside* QStackedWidget leaves the stack's
    // width reserved and produces a blank strip beside the native map.
    // Hide the entire sidebar stack so QHBoxLayout expands the viewport.
    this->viewport_page->setMapFullscreen(fullscreen);
}

MapWorkspaceWidget::Mode MapWorkspaceWidget::currentMode() const
{
    return this->selected_mode;
}

bool MapWorkspaceWidget::isDistributionEditorActive() const
{
    return currentMode() == DistributionEditor;
}

bool MapWorkspaceWidget::isDistributionMonitorActive() const
{
    return currentMode() == DistributionMonitor;
}

MapWidget *MapWorkspaceWidget::activeMap() const
{
    return this->shared_map;
}

MapModel *MapWorkspaceWidget::activeModel() const
{
    MapWidget *map = activeMap();
    return map != nullptr ? map->model() : nullptr;
}

MapModeToolPanel *MapWorkspaceWidget::activeToolPanel() const
{
    switch (currentMode())
    {
    case DistributionMonitor:
        return this->monitor_tools;
    case DistributionEditor:
        return this->editor_tools;
    case DrainageMonitor:
    case DrainageEditor:
        return nullptr;
    case MapManagement:
        return this->management_tools;
    }
    return nullptr;
}

void MapWorkspaceWidget::setCurrentMode(int index)
{
    if (MapWorkspace::modeInfo(index) == nullptr)
        return;
    if (static_cast<int>(this->selected_mode) == index)
        return;

    {
        const MapRenderDiagnostics::Scope timing(
            MapRenderDiagnostics::Counter::WorkspaceModeSwitch);
        const bool was_editor_input = this->selected_mode == DistributionEditor
            || this->selected_mode == MapManagement;
        const bool editor_input = index == DistributionEditor || index == MapManagement;
        const bool management = index == MapManagement;
        const bool basemap_only = !MapWorkspace::rendersDistributionNetwork(index);

        // Synchronously retire editor callbacks before another mode can use
        // the shared native window. The monitor controller owns navigation in drainage placeholders;
        // management reuses editor input only for cache rectangle selection.
        if (was_editor_input && !editor_input)
            this->editor_controller->activateSharedSurface(false);
        if (editor_input)
        {
            this->editor_controller->setManagementActive(management);
            this->monitor_controller->setEditorMode(true);
            if (!was_editor_input)
                this->editor_controller->activateSharedSurface(true);
            this->monitor_controller->setBasemapOnly(basemap_only);
        }
        else
        {
            this->monitor_controller->setEditorMode(false);
            this->monitor_controller->setBasemapOnly(basemap_only);
        }

        // Changing modes moves only the tools, NEVER the viewport or its
        // native surface. Drainage is an imagery/terrain-only render profile.
        this->mode_tool_stack->setCurrentIndex(index);
        this->selected_mode = static_cast<Mode>(index);
        for (QTabBar *tab_bar : this->mode_tabs)
        {
            if (tab_bar->currentIndex() != index)
                tab_bar->setCurrentIndex(index);
        }
        emit currentModeChanged(index);
    }
    if (MapRenderDiagnostics::enabled())
    {
        qInfo() << "map workspace mode" << MapWorkspace::modeInfo(index)->title
                << "(diagnostics: AOWIS_MAP_RENDER_DIAGNOSTICS=1)";
        MapRenderDiagnostics::reportNow();
    }
}

QWidget *MapWorkspaceWidget::createModeSelector(Mode mode)
{
    QWidget *selector = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(selector);
    layout->setContentsMargins(3, 4, 3, 5);
    layout->setSpacing(2);

    // Five compact icon tabs share the existing map sidebar.
    // The full mode name remains visible beneath the icon tabs.
    QTabBar *tabs = new MapModeTabBar(selector);
    tabs->setObjectName(QStringLiteral("mapModeTabs"));
    tabs->setAccessibleName(tr("Map modes"));
    tabs->setExpanding(true);
    tabs->setUsesScrollButtons(false);
    tabs->setIconSize(QSize(26, 26));
    for (const MapWorkspace::ModeInfo &description : MapWorkspace::Modes)
    {
        const QString title = tr(description.title);
        const int index = tabs->addTab(QIcon(QString::fromUtf8(description.icon_resource)), QString());
        tabs->setTabToolTip(index, title);
        tabs->setTabWhatsThis(index, title);
    }
    tabs->setCurrentIndex(mode);
    layout->addWidget(tabs);

    QLabel *title = new QLabel(tr(MapWorkspace::Modes[mode].title), selector);
    title->setAlignment(Qt::AlignCenter);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    connect(this, &MapWorkspaceWidget::languageChanged, selector, [tabs, title, mode] {
        tabs->setAccessibleName(tr("Map modes"));
        title->setText(tr(MapWorkspace::Modes[mode].title));
        for (int index = 0; index < tabs->count(); ++index)
        {
            const QString text = tr(MapWorkspace::Modes[index].title);
            tabs->setTabToolTip(index, text);
            tabs->setTabWhatsThis(index, text);
        }
    });
    this->mode_tabs.append(tabs);
    connect(tabs, &QTabBar::currentChanged, this, &MapWorkspaceWidget::setCurrentMode);
    return selector;
}

QWidget *MapWorkspaceWidget::createEmptyModeTools(Mode mode)
{
    // Drainage controls intentionally remain empty, but all five selectors
    // occupy the same sidebar stack so the persistent map never disappears.
    QWidget *sidebar = new QWidget(this->mode_tool_stack);
    QVBoxLayout *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(createModeSelector(mode));
    layout->addStretch(1);
    return sidebar;
}

void MapWorkspaceWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        emit languageChanged();
}
