#ifndef ENTITY_INSPECTOR_TANK_H
#define ENTITY_INSPECTOR_TANK_H

#include <optional>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QUuid>
#include <QWidget>

#include "entity_inspector/entity_inspector_widget.h"

#include "widgets/group_box_collapsible.h"

#include <aowis/model/hydraulic/network_hydraulic.h>

class EntityInspectorTank : public EntityInspectorWidget
{
    Q_OBJECT

public:
    explicit EntityInspectorTank(HydraulicData *hydraulic_data, const QUuid &uuid, QWidget *parent = nullptr);

private:
    void addGroupGeometry();
    void bindGeometry();
    void refreshGeometry();
    void populateVolumeCurveCombo(const QUuid &volume_curve_uuid);
    void updateGeometryModeUi(HydraulicNodeTankGeometryInputType geometry_input_type);
    void updateDerivedGeometry(const HydraulicNodeTank &tank);
    std::optional<double> volumeCurveVolumeAtLevel(const QUuid &curve_uuid, double water_level_m) const;
    void setSpinEditable(QDoubleSpinBox *spin, const char *tooltip = nullptr);
    void setSpinDerived(QDoubleSpinBox *spin, const std::optional<double> &value,
                        const char *tooltip = nullptr);
    HydraulicNodeTankGeometryInputType tankGeometryInputType() const;

    HydraulicData *hydraulic_data = nullptr;
    QUuid tank_uuid;

    GroupBoxCollapsible *group_geometry = nullptr;

    QDoubleSpinBox *spin_level_initial = nullptr;
    QDoubleSpinBox *spin_level_minimum = nullptr;
    QDoubleSpinBox *spin_level_maximum = nullptr;
    QCheckBox *check_overflow = nullptr;

    QComboBox *combo_geometry_type = nullptr;

    QLabel *label_diameter = nullptr;
    QDoubleSpinBox *spin_diameter = nullptr;
    QLabel *label_area = nullptr;
    QDoubleSpinBox *spin_area = nullptr;
    QLabel *label_volume_minimum = nullptr;
    QDoubleSpinBox *spin_volume_minimum = nullptr;
    QLabel *label_volume_maximum = nullptr;
    QDoubleSpinBox *spin_volume_maximum = nullptr;
    QLabel *label_volume_curve = nullptr;
    QComboBox *combo_volume_curve = nullptr;

    void addGroupQuality();
    void refreshQuality();
    void updateQualityUi();
    QComboBox *combo_quality_mixing = nullptr;
    QDoubleSpinBox *spin_quality_mixing_fraction = nullptr;
    QCheckBox *check_quality_override_bulk_reaction = nullptr;
    QDoubleSpinBox *spin_quality_bulk_reaction_coefficient = nullptr;

private slots:
    void onGroupExpand(GroupBoxCollapsible *group);
    void onGeometryTypeChanged(int index);
};

#endif // ENTITY_INSPECTOR_TANK_H
