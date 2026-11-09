#ifndef ENTITY_INSPECTOR_PIPE_H
#define ENTITY_INSPECTOR_PIPE_H

#include <QObject>
#include <QWidget>
#include <QVBoxLayout>
#include <QGridLayout>

#include <QPixmap>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QStackedWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QUuid>

#include "entity_inspector/entity_inspector_widget.h"

#include "widgets/group_box_collapsible.h"

#include "common/_enums_structs.h"
#include "common/_sizes.h"
#include "map/core/map_models.h"

class EntityInspectorPipe : public EntityInspectorWidget
{
    Q_OBJECT
public:
    explicit EntityInspectorPipe(HydraulicData *hydraulic_data, const HydraulicLinkPipe &pipe, QWidget *parent = nullptr);

private:
    HydraulicData *hydraulic_data = nullptr;
    QUuid pipe_uuid;

    QLabel *picture = nullptr;

    void addGroupGeometry();
    QComboBox *combo_status_initial = nullptr;
    QDoubleSpinBox *spin_diameter = nullptr;
    QDoubleSpinBox *spin_length_calculated = nullptr;
    QCheckBox *check_length_measured = nullptr;
    QDoubleSpinBox *spin_length_measured = nullptr;

    void addGroupRoughness();
    QComboBox *combo_material = nullptr;
    QComboBox *combo_roughness_mode = nullptr;
    QDoubleSpinBox *spin_roughness_hw = nullptr;
    QDoubleSpinBox *spin_roughness_dw = nullptr;
    QDoubleSpinBox *spin_roughness_cm = nullptr;
    QDoubleSpinBox *spin_loss_coefficient = nullptr;

    void addGroupQuality();
    QCheckBox *check_override_bulk = nullptr;
    QCheckBox *check_override_wall = nullptr;
    QDoubleSpinBox *spin_bulk_reaction = nullptr;
    QDoubleSpinBox *spin_wall_reaction = nullptr;
    QDoubleSpinBox *spin_wall_reaction_zero = nullptr;
    QStackedWidget *stack_wall_reaction = nullptr;
    void refreshPipeQuality();
    void updateQualityUi();
    void refreshWallReactionTooltip();
    void refreshRoughnessTooltips();

    void refreshPipe();
    void refreshPipeGeometry();
    void updateRoughnessUi(const HydraulicLinkPipe &pipe);

signals:

protected slots:
    void onHeadlossFormulaChanged(HeadlossFormulas formulas) override;
};

#endif // ENTITY_INSPECTOR_PIPE_H
