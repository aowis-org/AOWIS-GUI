#include "entity_inspector/entity_inspector_text.h"
#include "entity_inspector/entity_inspector_dock.h"
#include "config/gui_configuration.h"
#include "config/shortcut_registry.h"


EntityInspectorDock::EntityInspectorDock(HydraulicData *hydraulic_data, QWidget *parent)
    : QDockWidget(parent), hydraulic_data(hydraulic_data)
{
    const std::function<void()> refresh = [this] {
        const QString shortcut = guiShortcutPresentation(GuiShortcutId::SidebarToggle);
        setWindowTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Entity Inspector  |  [%1] Toggle sidebar")).arg(shortcut));
        setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Press %1 to show or hide the right sidebar.")).arg(shortcut));
    };
    LiveUiText::bind(this, refresh);
    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged, this, [refresh](GuiShortcutId id) {
        if (id == GuiShortcutId::SidebarToggle)
            refresh();
    });
    setMinimumWidth(Sizes::SidebarRightWidth);
    setMaximumWidth(Sizes::SidebarRightWidth);
    this->resize(Sizes::SidebarRightWidth, this->height());
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    
    //setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setAllowedAreas(Qt::RightDockWidgetArea);
    
    setFeatures(QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    
    connect(this->hydraulic_data, &HydraulicData::signalSelectedTank, this, &EntityInspectorDock::showEntityTank);
    
    connect(this->hydraulic_data, &HydraulicData::signalSelectedJunction, this, &EntityInspectorDock::showEntityJunction);
    
    connect(this->hydraulic_data, &HydraulicData::signalSelectedPipe, this, &EntityInspectorDock::showEntityPipe);
    
    connect(this->hydraulic_data, &HydraulicData::signalSelectedPump, this, &EntityInspectorDock::showEntityPump);
    
    connect(this->hydraulic_data, &HydraulicData::signalSelectedValve, this, &EntityInspectorDock::showEntityValve);
    
    connect(this->hydraulic_data, &HydraulicData::signalSelectedReservoir, this, &EntityInspectorDock::showEntityReservoir);
    
    connect(this->hydraulic_data, &HydraulicData::signalSelectedDemandPoint, this, &EntityInspectorDock::showEntityDemandPoint);
    
    setVisible(false);
    
    //showEntityTank();
    //showEntityJunction();
    //showEntityPipe();
    //showEntityPump();
    //showEntityValve();
    //showEntityReservoir();
    //showEntityDemandPoint();
}

void EntityInspectorDock::clearEntity()
{
    if (this->widget_current)
        this->widget_current->deleteLater();
    
    this->widget_current = nullptr;
}

void EntityInspectorDock::setInspector(EntityInspectorWidget *inspector)
{
    clearEntity();
    
    this->widget_current = inspector;
    this->widget_current->setCurrentTabIndex(this->tab_index_current);
    connect(this->widget_current, &EntityInspectorWidget::signalCurrentTabChanged, this, [this](int index)
    {
        this->tab_index_current = index;
    });
    setWidget(this->widget_current);
    
    this->widget_current->onHeadlossFormulaChanged(this->headloss_formulas_current);
    
    setVisible(true);
}

void EntityInspectorDock::showEntityTank(const HydraulicNodeTank &tank)
{
    EntityInspectorTank *inspector = new EntityInspectorTank(this->hydraulic_data, tank.uuid);
    setInspector(inspector);
}
void EntityInspectorDock::showEntityJunction(const HydraulicNodeJunction &junction)
{
    EntityInspectorJunction *inspector = new EntityInspectorJunction(this->hydraulic_data, junction.uuid);
    setInspector(inspector);
}
void EntityInspectorDock::showEntityPipe(HydraulicLinkPipe pipe)
{
    EntityInspectorPipe *inspector = new EntityInspectorPipe(this->hydraulic_data, pipe);
    setInspector(inspector);
}
void EntityInspectorDock::showEntityPump(HydraulicLinkPump pump)
{
    EntityInspectorPump *inspector = new EntityInspectorPump(this->hydraulic_data, pump);
    setInspector(inspector);
}
void EntityInspectorDock::showEntityValve(HydraulicLinkValve valve)
{
    EntityInspectorValve *inspector = new EntityInspectorValve(this->hydraulic_data, valve);
    setInspector(inspector);
}
void EntityInspectorDock::showEntityReservoir(const HydraulicNodeReservoir &reservoir)
{
    EntityInspectorReservoir *inspector = new EntityInspectorReservoir(this->hydraulic_data, reservoir.uuid);
    setInspector(inspector);
}
void EntityInspectorDock::showEntityDemandPoint(HydraulicDemandPoint demand_point)
{
    EntityInspectorDemandPoint *inspector =
        new EntityInspectorDemandPoint(this->hydraulic_data, demand_point.uuid);
    setInspector(inspector);
}

void EntityInspectorDock::onHeadlossFormulaChanged(HeadlossFormulas formulas)
{
    this->headloss_formulas_current = formulas;
    
    if (this->widget_current != nullptr) {
        this->widget_current->onHeadlossFormulaChanged(formulas);
    }
}
