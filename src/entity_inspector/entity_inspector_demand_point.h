#ifndef ENTITY_INSPECTOR_DEMAND_POINT_H
#define ENTITY_INSPECTOR_DEMAND_POINT_H

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QUuid>
#include <QWidget>

#include <functional>

#include "entity_inspector/entity_inspector_widget.h"
#include "widgets/group_box_collapsible.h"

#include <aowis/model/hydraulic/network_hydraulic_distribution.h>

class EntityInspectorDemandPoint : public EntityInspectorWidget
{
    Q_OBJECT
public:
    explicit EntityInspectorDemandPoint(
        HydraulicData *hydraulic_data, const QUuid &uuid, QWidget *parent = nullptr);

private:
    QUuid demand_point_uuid;

    QPlainTextEdit *edit_description = nullptr;
    QPlainTextEdit *edit_comment = nullptr;
    QLineEdit *line_tags = nullptr;

    QLabel *label_attachment_type_value = nullptr;
    QLabel *label_attachment_target_value = nullptr;
    QLabel *label_attachment_position_value = nullptr;
    QLabel *label_pipe_allocation_mode = nullptr;
    QComboBox *combo_pipe_allocation_mode = nullptr;
    QLabel *label_pipe_assigned_junction = nullptr;
    QComboBox *combo_pipe_assigned_junction = nullptr;
    QPushButton *button_attachment_select = nullptr;
    QPushButton *button_attachment_locate = nullptr;
    QPushButton *button_attachment_detach = nullptr;

    QWidget *widget_meter_fields = nullptr;
    QLabel *label_meter_uuid_value = nullptr;
    QPushButton *button_meter_add = nullptr;
    QPushButton *button_meter_remove = nullptr;
    QLineEdit *line_meter_id = nullptr;
    QCheckBox *check_meter_enabled = nullptr;
    QComboBox *combo_meter_model_role = nullptr;
    QDateEdit *date_meter_added = nullptr;
    QDateEdit *date_meter_installed = nullptr;
    QLineEdit *line_meter_manufacturer = nullptr;
    QLineEdit *line_meter_model = nullptr;
    QLineEdit *line_meter_serial = nullptr;
    QPlainTextEdit *edit_meter_description = nullptr;
    QPlainTextEdit *edit_meter_comment = nullptr;
    QLineEdit *line_meter_tags = nullptr;

    void addGroupDetails();
    void addGroupAttachment();
    void addGroupMeter();
    void addGroupGraphs();
    void refreshDetails();
    void refreshAttachment();
    void refreshMeter();
    void updateMeter(const std::function<void(WaterMeter &)> &mutation);
    QStringList editedTags() const;
    QStringList editedMeterTags() const;
};

#endif // ENTITY_INSPECTOR_DEMAND_POINT_H
