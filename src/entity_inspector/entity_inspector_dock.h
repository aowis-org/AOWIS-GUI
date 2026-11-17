#ifndef ENTITY_INSPECTOR_DOCK_H
#define ENTITY_INSPECTOR_DOCK_H

#include <QWidget>
#include <QDockWidget>

#include <QVBoxLayout>

#include "entity_inspector/entity_inspector_tank.h"
#include "entity_inspector/entity_inspector_junction.h"
#include "entity_inspector/entity_inspector_pipe.h"
#include "entity_inspector/entity_inspector_pump.h"
#include "entity_inspector/entity_inspector_valve.h"
#include "entity_inspector/entity_inspector_reservoir.h"
#include "entity_inspector/entity_inspector_demand_point.h"

#include "widgets/group_box_collapsible.h"
#include "network/hydraulic_data.h"

#include <aowis/model/hydraulic/network_hydraulic_distribution.h>
#include "common/_enums_structs.h"
#include "common/_sizes.h"
#include "map/core/map_models.h"

class EntityInspectorDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit EntityInspectorDock(HydraulicData *hydraulic_data, QWidget *parent = nullptr);
    
    //void showEntity(MapNetworkStructs &entity);
    
    void clearEntity();
    
private:
    HydraulicData *hydraulic_data = nullptr;
    void setInspector(EntityInspectorWidget *inspector);
    
    EntityInspectorWidget *widget_current = nullptr;
    int tab_index_current = 0;
    
    HeadlossFormulas headloss_formulas_current =
        HeadlossFormulas(HeadlossFormula::HazenWilliams);
    
public slots:
    virtual void onHeadlossFormulaChanged(HeadlossFormulas formulas);
    
public slots:
    void showEntityTank(const HydraulicNodeTank &tank);
    void showEntityJunction(const HydraulicNodeJunction &junction);
    void showEntityPipe(HydraulicLinkPipe pipe);
    void showEntityPump(HydraulicLinkPump pump);
    void showEntityValve(HydraulicLinkValve valve);
    void showEntityReservoir(const HydraulicNodeReservoir &reservoir);
    void showEntityDemandPoint(HydraulicDemandPoint demand_point);
    
};

#endif // ENTITY_INSPECTOR_DOCK_H
