#pragma once

#include "tabs/map_workspace_mode.h"

#include <QList>
#include <QWidget>

class MapModeToolPanel;
class MapWorkspaceViewportPage;
class MapEditorContainer;
class MapMonitorContainer;
class MapWidget;
class MapModel;
class QStackedWidget;
class QTabBar;

// Five map modes share one persistent viewport and camera. Distribution
// uses monitor/editor rendering profiles; drainage uses basemap-only mode.
class MapWorkspaceWidget final : public QWidget
{
    Q_OBJECT

public:
    enum Mode
    {
        DistributionMonitor = 0,
        DistributionEditor = 1,
        DrainageMonitor = 2,
        DrainageEditor = 3,
        MapManagement = 4
    };
    Q_ENUM(Mode)
    // Keep Qt's meta-enum numeric values aligned with the independent mode
    // contract without asking moc to evaluate namespace constexpr symbols.
    static_assert(DistributionMonitor == MapWorkspace::DistributionMonitor);
    static_assert(DistributionEditor == MapWorkspace::DistributionEditor);
    static_assert(DrainageMonitor == MapWorkspace::DrainageMonitor);
    static_assert(DrainageEditor == MapWorkspace::DrainageEditor);
    static_assert(MapManagement == MapWorkspace::MapManagement);

    explicit MapWorkspaceWidget(MapEditorContainer *editor,
                                MapMonitorContainer *monitor,
                                QWidget *parent = nullptr);

    ~MapWorkspaceWidget() override;
    Mode currentMode() const;
    bool isDistributionEditorActive() const;
    bool isDistributionMonitorActive() const;

    // Non-owning access to the one persistent map and navigation model,
    // including while a drainage mode is active.
    MapWidget *activeMap() const;
    MapModel *activeModel() const;
    // Distribution tools are independent of the renderer. Drainage tools
    // intentionally remain empty until domain functionality is implemented.
    MapModeToolPanel *activeToolPanel() const;


public slots:
    void setCurrentMode(int index);
    void setMapFullscreen(bool fullscreen);

signals:
    void languageChanged();
    void currentModeChanged(int index);

protected:
    void changeEvent(QEvent *event) override;

private:
    QWidget *createModeSelector(Mode mode);
    QWidget *createEmptyModeTools(Mode mode);

    Mode selected_mode = DistributionMonitor;
    MapMonitorContainer *monitor_controller = nullptr;
    MapEditorContainer *editor_controller = nullptr;
    QStackedWidget *mode_tool_stack = nullptr;
    MapWorkspaceViewportPage *viewport_page = nullptr;
    MapWidget *shared_map = nullptr;
    MapModeToolPanel *monitor_tools = nullptr;
    MapModeToolPanel *editor_tools = nullptr;
    MapModeToolPanel *management_tools = nullptr;
    QList<QTabBar *> mode_tabs;
};
