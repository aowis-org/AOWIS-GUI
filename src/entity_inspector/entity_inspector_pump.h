#ifndef ENTITY_INSPECTOR_PUMP_H
#define ENTITY_INSPECTOR_PUMP_H

#include <functional>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QUuid>
#include <QWidget>

#include "entity_inspector/entity_inspector_widget.h"

#include "widgets/group_box_collapsible.h"

#include <aowis/model/hydraulic/network_hydraulic.h>
class UnitNumericSpinBox;
class UnitEnergyPriceSpinBox;

class EntityInspectorPump : public EntityInspectorWidget
{
    Q_OBJECT

public:
    explicit EntityInspectorPump(HydraulicData *hydraulic_data, const HydraulicLinkPump &pump, QWidget *parent = nullptr);

private:
    void addGroupPumpInput();
    void addGroupControls();
    void addGroupEnergyCostInput();
    void addGroupEnergy();
    void bindPump();
    void refreshPump();
    void refreshPumpMeasurements();
    void refreshPumpEnergyPrice();
    void refreshPumpControls();
    void refreshPumpControlUnits();
    void refreshPumpRuleSummary();
    QUuid firstPumpControlTriggerNodeUuid() const;
    bool updatePumpSimpleControl(
        const QUuid &control_uuid,
        const std::function<void(HydraulicControlSimple &)> &mutation);
    void populateSpeedPatternCombo(const QUuid &pattern_uuid);
    void populateEfficiencyInputCombo(const HydraulicLinkPump &pump);
    void populatePricePatternCombo(const HydraulicLinkPump &pump);

    HydraulicData *hydraulic_data = nullptr;
    QUuid pump_uuid;

    QComboBox *combo_type = nullptr;
    QLabel *label_constant_power = nullptr;
    UnitNumericSpinBox *spin_constant_power = nullptr;
    UnitNumericSpinBox *spin_speed_initial = nullptr;
    QComboBox *combo_status_initial = nullptr;
    QComboBox *combo_speed_pattern = nullptr;

    QTableWidget *table_controls = nullptr;
    QComboBox *combo_new_control_type = nullptr;
    QPushButton *button_add_control = nullptr;
    QLabel *label_rule_controls = nullptr;

    QComboBox *combo_efficiency_curve = nullptr;
    UnitEnergyPriceSpinBox *spin_energy_price = nullptr;
    QComboBox *combo_price_pattern = nullptr;
};

#endif // ENTITY_INSPECTOR_PUMP_H
