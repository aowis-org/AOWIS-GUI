#include "config/live_ui_text.h"
#include "entity_inspector/entity_inspector_text.h"
#include "entity_inspector/entity_inspector_demand_point.h"

#include <QGridLayout>
#include <QSignalBlocker>

namespace
{
QDate meterUnsetDate()
{
    return QDate(100, 1, 1);
}

std::optional<QDate> optionalMeterDate(QDateEdit *edit)
{
    if (edit == nullptr || edit->date() == meterUnsetDate())
        return std::nullopt;
    return edit->date();
}
}

EntityInspectorDemandPoint::EntityInspectorDemandPoint(
    HydraulicData *hydraulic_data, const QUuid &uuid, QWidget *parent)
    : EntityInspectorWidget(hydraulic_data, parent),
      demand_point_uuid(uuid)
{
    addGroupOverviewImage(":/icon/demand_point.png", QString());
    addGroupGeneral(QString());
    addGroupPosition();
    bindDemandPoint(this->demand_point_uuid, "Demand Point");

    addGroupDetails();
    addGroupAttachment();
    addGroupDemands(false);
    addGroupMeter();
    addGroupGraphs();
    addGroupHistory();
    addStretches();

    connect(hydraulic_data, &HydraulicData::signalDemandPointChanged,
            this, [this](const QUuid &uuid_changed)
    {
        if (uuid_changed != this->demand_point_uuid)
            return;
        refreshDetails();
        refreshAttachment();
        refreshMeter();
    });
    connect(hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]()
    {
        refreshDetails();
        refreshAttachment();
        refreshMeter();
    });

    refreshDetails();
    refreshAttachment();
    refreshMeter();
}

void EntityInspectorDemandPoint::addGroupDetails()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Details"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_description = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Description"), nullptr);
    this->edit_description = new QPlainTextEdit();
    LiveUiText::bind(this->edit_description, [this] {
        this->edit_description->setPlaceholderText(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "Describe this demand point...")));
    });
    this->edit_description->setMaximumHeight(90);

    QLabel *label_tags = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Tags"), nullptr);
    this->line_tags = new QLineEdit();
    LiveUiText::bind(this->line_tags, [this] {
        this->line_tags->setPlaceholderText(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "household, school, irrigation")));
    });
    LiveUiText::bind(this->line_tags, [this] {
        this->line_tags->setToolTip(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "Comma-separated tags. Empty entries and duplicates are ignored.")));
    });

    QLabel *label_comment = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Comment"), nullptr);
    this->edit_comment = new QPlainTextEdit();
    LiveUiText::bind(this->edit_comment, [this] {
        this->edit_comment->setPlaceholderText(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "Free-form modelling or operational note...")));
    });
    this->edit_comment->setMaximumHeight(90);

    grid->addWidget(label_description, 0, 0, Qt::AlignTop);
    grid->addWidget(this->edit_description, 0, 1);
    grid->addWidget(label_tags, 1, 0);
    grid->addWidget(this->line_tags, 1, 1);
    grid->addWidget(label_comment, 2, 0, Qt::AlignTop);
    grid->addWidget(this->edit_comment, 2, 1);
    grid->setColumnStretch(1, 1);

    connect(this->edit_description, &QPlainTextEdit::textChanged, this, [this]()
    {
        hydraulicData()->setDemandPointDescription(
            this->demand_point_uuid, this->edit_description->toPlainText());
    });
    connect(this->edit_comment, &QPlainTextEdit::textChanged, this, [this]()
    {
        hydraulicData()->setDemandPointComment(
            this->demand_point_uuid, this->edit_comment->toPlainText());
    });
    connect(this->line_tags, &QLineEdit::editingFinished, this, [this]()
    {
        hydraulicData()->setDemandPointTags(this->demand_point_uuid, editedTags());
    });

    this->layoutConfiguration()->addWidget(group);
}

QStringList EntityInspectorDemandPoint::editedTags() const
{
    QStringList result;
    const QStringList entered = this->line_tags->text().split(',', Qt::KeepEmptyParts);
    for (const QString &entered_tag : entered)
    {
        const QString tag = entered_tag.trimmed();
        if (tag.isEmpty() || result.contains(tag))
            continue;
        result.append(tag);
    }
    return result;
}

QStringList EntityInspectorDemandPoint::editedMeterTags() const
{
    QStringList result;
    const QStringList entered = this->line_meter_tags->text().split(',', Qt::KeepEmptyParts);
    for (const QString &entered_tag : entered)
    {
        const QString tag = entered_tag.trimmed();
        if (tag.isEmpty() || result.contains(tag))
            continue;
        result.append(tag);
    }
    return result;
}

void EntityInspectorDemandPoint::refreshDetails()
{
    const std::optional<HydraulicDemandPoint> demand_point =
        hydraulicData()->demandPoint(this->demand_point_uuid);
    if (!demand_point.has_value())
        return;

    if (this->edit_description &&
        this->edit_description->toPlainText() != demand_point->metadata.description)
    {
        const QSignalBlocker blocker(this->edit_description);
        this->edit_description->setPlainText(demand_point->metadata.description);
    }
    if (this->edit_comment &&
        this->edit_comment->toPlainText() != demand_point->metadata.comment)
    {
        const QSignalBlocker blocker(this->edit_comment);
        this->edit_comment->setPlainText(demand_point->metadata.comment);
    }
    if (this->line_tags)
    {
        const QString tags_text = demand_point->metadata.tags.join(QStringLiteral(", "));
        if (this->line_tags->text() != tags_text)
        {
            const QSignalBlocker blocker(this->line_tags);
            this->line_tags->setText(tags_text);
        }
    }
}

void EntityInspectorDemandPoint::addGroupAttachment()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Network Attachment"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_type = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Type"), nullptr);
    this->label_attachment_type_value = new QLabel();
    this->label_attachment_type_value->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QLabel *label_target = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Target"), nullptr);
    this->label_attachment_target_value = new QLabel();
    this->label_attachment_target_value->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QLabel *label_position = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Pipe Position"), nullptr);
    this->label_attachment_position_value = new QLabel();
    this->label_attachment_position_value->setTextInteractionFlags(Qt::TextSelectableByMouse);

    this->label_pipe_allocation_mode = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Demand<br>Allocation"), nullptr);
    this->combo_pipe_allocation_mode = new QComboBox();
    EntityInspectorText::addChoice(this->combo_pipe_allocation_mode, QT_TRANSLATE_NOOP("LiveUiText", "Split by pipe position"), static_cast<int>(HydraulicDemandPointPipeAllocationMode::InterpolateByPosition));
    EntityInspectorText::addChoice(this->combo_pipe_allocation_mode, QT_TRANSLATE_NOOP("LiveUiText", "Assigned junction"), static_cast<int>(HydraulicDemandPointPipeAllocationMode::AssignedJunction));
    LiveUiText::bind(this->combo_pipe_allocation_mode, [this] {
        this->combo_pipe_allocation_mode->setToolTip(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText",
                "'Split by pipe position' mode distributes demand between both pipe endpoint junctions according "
                "to the demand-point position. <br>"
                "'Assigned junction' mode keeps the visual pipe attachment "
                "but applies all demand to one selected endpoint junction.")));
    });

    this->label_pipe_assigned_junction = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Assigned<br>Junction"), nullptr);
    this->combo_pipe_assigned_junction = new QComboBox();
    LiveUiText::bind(this->combo_pipe_assigned_junction, [this] {
        this->combo_pipe_assigned_junction->setToolTip(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText",
                "The pipe endpoint junction that receives 100% of this demand point's demand.")));
    });

    this->button_attachment_select = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Attach / Reattach on Map"), nullptr);
    this->button_attachment_locate = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Find Attachment on Map"), nullptr);
    this->button_attachment_detach = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Detach"), nullptr);

    grid->addWidget(label_type, 0, 0);
    grid->addWidget(this->label_attachment_type_value, 0, 1);
    grid->addWidget(label_target, 1, 0);
    grid->addWidget(this->label_attachment_target_value, 1, 1);
    grid->addWidget(label_position, 2, 0);
    grid->addWidget(this->label_attachment_position_value, 2, 1);
    grid->addWidget(this->label_pipe_allocation_mode, 3, 0);
    grid->addWidget(this->combo_pipe_allocation_mode, 3, 1);
    grid->addWidget(this->label_pipe_assigned_junction, 4, 0);
    grid->addWidget(this->combo_pipe_assigned_junction, 4, 1);
    grid->addWidget(this->button_attachment_select, 5, 0, 1, 2);
    grid->addWidget(this->button_attachment_locate, 6, 0, 1, 2);
    grid->addWidget(this->button_attachment_detach, 7, 0, 1, 2);
    grid->setColumnStretch(1, 1);

    connect(this->combo_pipe_allocation_mode, &QComboBox::currentIndexChanged,
            this, [this](int index)
    {
        if (index < 0)
            return;

        const HydraulicDemandPointPipeAllocationMode allocation_mode =
            static_cast<HydraulicDemandPointPipeAllocationMode>(
                this->combo_pipe_allocation_mode->itemData(index).toInt());
        if (!hydraulicData()->setDemandPointPipeAllocationMode(
                this->demand_point_uuid, allocation_mode))
        {
            refreshAttachment();
        }
    });
    connect(this->combo_pipe_assigned_junction, &QComboBox::currentIndexChanged,
            this, [this](int index)
    {
        if (index < 0)
            return;

        const QUuid junction_uuid =
            this->combo_pipe_assigned_junction->itemData(index).toUuid();
        if (junction_uuid.isNull())
            return;
        if (!hydraulicData()->setDemandPointPipeAssignedJunction(
                this->demand_point_uuid, junction_uuid))
        {
            refreshAttachment();
        }
    });
    connect(this->button_attachment_select, &QPushButton::clicked, this, [this]()
    {
        hydraulicData()->requestDemandPointAttachmentSelection(this->demand_point_uuid);
    });
    connect(this->button_attachment_detach, &QPushButton::clicked, this, [this]()
    {
        hydraulicData()->clearDemandPointAttachment(this->demand_point_uuid);
    });
    connect(this->button_attachment_locate, &QPushButton::clicked, this, [this]()
    {
        const std::optional<HydraulicDemandPoint> demand_point =
            hydraulicData()->demandPoint(this->demand_point_uuid);
        if (!demand_point.has_value())
            return;

        if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Pipe)
        {
            hydraulicData()->requestEntityLocate(
                InfrastructureEntity::Pipe, demand_point->attachment.pipe_uuid);
        }
        else if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Junction)
        {
            hydraulicData()->requestEntityLocate(
                InfrastructureEntity::Junction, demand_point->attachment.junction_uuid);
        }
    });

    this->layoutConfiguration()->addWidget(group);
}

void EntityInspectorDemandPoint::refreshAttachment()
{
    const std::optional<HydraulicDemandPoint> demand_point =
        hydraulicData()->demandPoint(this->demand_point_uuid);
    if (!demand_point.has_value())
        return;

    QString type_text = QStringLiteral("Unattached");
    QString target_text = QStringLiteral("—");
    QString position_text = QStringLiteral("—");
    bool attached = false;
    bool pipe_attached = false;
    bool assigned_junction_mode = false;

    const QSignalBlocker allocation_mode_blocker(this->combo_pipe_allocation_mode);
    const QSignalBlocker assigned_junction_blocker(this->combo_pipe_assigned_junction);
    this->combo_pipe_assigned_junction->clear();

    if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Pipe)
    {
        type_text = QStringLiteral("Pipe");
        attached = true;
        pipe_attached = true;
        const std::optional<HydraulicLinkPipe> pipe =
            hydraulicData()->pipe(demand_point->attachment.pipe_uuid);
        target_text = pipe.has_value() && !pipe->id.isEmpty()
            ? pipe->id
            : demand_point->attachment.pipe_uuid.toString(QUuid::WithoutBraces);
        position_text = QStringLiteral("%1 %")
            .arg(demand_point->attachment.pipe_position * 100.0, 0, 'f', 2);

        const int mode_index = this->combo_pipe_allocation_mode->findData(
            static_cast<int>(demand_point->attachment.pipe_allocation_mode));
        if (mode_index >= 0)
            this->combo_pipe_allocation_mode->setCurrentIndex(mode_index);

        assigned_junction_mode =
            demand_point->attachment.pipe_allocation_mode
            == HydraulicDemandPointPipeAllocationMode::AssignedJunction;

        if (pipe.has_value())
        {
            const std::optional<HydraulicNodeJunction> from_junction =
                hydraulicData()->junction(pipe->node_uuid_from);
            if (from_junction.has_value())
            {
                const QString text = from_junction->id.isEmpty()
                    ? from_junction->uuid.toString(QUuid::WithoutBraces)
                    : from_junction->id;
                this->combo_pipe_assigned_junction->addItem(text, from_junction->uuid);
            }

            if (pipe->node_uuid_to != pipe->node_uuid_from)
            {
                const std::optional<HydraulicNodeJunction> to_junction =
                    hydraulicData()->junction(pipe->node_uuid_to);
                if (to_junction.has_value())
                {
                    const QString text = to_junction->id.isEmpty()
                        ? to_junction->uuid.toString(QUuid::WithoutBraces)
                        : to_junction->id;
                    this->combo_pipe_assigned_junction->addItem(text, to_junction->uuid);
                }
            }
        }

        const int assigned_index = this->combo_pipe_assigned_junction->findData(
            demand_point->attachment.pipe_assigned_junction_uuid);
        if (assigned_index >= 0)
            this->combo_pipe_assigned_junction->setCurrentIndex(assigned_index);
        else
            this->combo_pipe_assigned_junction->setCurrentIndex(-1);
    }

    else if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Junction)
    {
        type_text = QStringLiteral("Junction");
        attached = true;
        const std::optional<HydraulicNodeJunction> junction =
            hydraulicData()->junction(demand_point->attachment.junction_uuid);
        target_text = junction.has_value() && !junction->id.isEmpty()
            ? junction->id
            : demand_point->attachment.junction_uuid.toString(QUuid::WithoutBraces);
    }

    this->label_attachment_type_value->setText(type_text);
    this->label_attachment_target_value->setText(target_text);
    this->label_attachment_position_value->setText(position_text);
    this->label_pipe_allocation_mode->setVisible(pipe_attached);
    this->combo_pipe_allocation_mode->setVisible(pipe_attached);
    this->label_pipe_assigned_junction->setVisible(pipe_attached && assigned_junction_mode);
    this->combo_pipe_assigned_junction->setVisible(pipe_attached && assigned_junction_mode);
    this->combo_pipe_assigned_junction->setEnabled(
        pipe_attached && assigned_junction_mode
        && this->combo_pipe_assigned_junction->count() > 0);
    this->button_attachment_locate->setEnabled(attached);
    this->button_attachment_detach->setEnabled(attached);
}

void EntityInspectorDemandPoint::addGroupMeter()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Water Meter"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_uuid = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Meter UUID"), nullptr);
    this->label_meter_uuid_value = new QLabel(QStringLiteral("—"));
    this->label_meter_uuid_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->label_meter_uuid_value->setWordWrap(true);

    this->button_meter_add = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Add Water Meter"), nullptr);
    this->button_meter_remove = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Remove Water Meter"), nullptr);

    grid->addWidget(label_uuid, 0, 0);
    grid->addWidget(this->label_meter_uuid_value, 0, 1);
    grid->addWidget(this->button_meter_add, 1, 0, 1, 2);
    grid->addWidget(this->button_meter_remove, 2, 0, 1, 2);

    this->widget_meter_fields = new QWidget(group);
    QGridLayout *fields = new QGridLayout(this->widget_meter_fields);
    fields->setContentsMargins(0, 0, 0, 0);

    QLabel *label_id = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "ID"), nullptr);
    this->line_meter_id = new QLineEdit();

    this->check_meter_enabled = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Enabled"), nullptr);

    QLabel *label_model_role = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Model Role"), nullptr);
    this->combo_meter_model_role = new QComboBox();
    EntityInspectorText::addChoice(this->combo_meter_model_role, QT_TRANSLATE_NOOP("LiveUiText", "[Unspecified]"), static_cast<int>(EntityModelRole::Unspecified));
    EntityInspectorText::addChoice(this->combo_meter_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Existing Asset"), static_cast<int>(EntityModelRole::ExistingAsset));
    EntityInspectorText::addChoice(this->combo_meter_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Planned Asset"), static_cast<int>(EntityModelRole::PlannedAsset));
    EntityInspectorText::addChoice(this->combo_meter_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Virtual / Model-Only"), static_cast<int>(EntityModelRole::VirtualModelElement));
    EntityInspectorText::addChoice(this->combo_meter_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Boundary Condition"), static_cast<int>(EntityModelRole::BoundaryCondition));
    EntityInspectorText::addChoice(this->combo_meter_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Temporary / Testing"), static_cast<int>(EntityModelRole::TemporaryTesting));
    EntityInspectorText::addChoice(this->combo_meter_model_role, QT_TRANSLATE_NOOP("LiveUiText", "Retired Asset"), static_cast<int>(EntityModelRole::RetiredAsset));

    QLabel *label_date_added = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Date Added"), nullptr);
    this->date_meter_added = new QDateEdit();
    this->date_meter_added->setCalendarPopup(true);
    this->date_meter_added->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    this->date_meter_added->setMinimumDate(meterUnsetDate());
    this->date_meter_added->setSpecialValueText("[Not set]");
    this->date_meter_added->setDate(meterUnsetDate());

    QLabel *label_date_installed = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Installation Date"), nullptr);
    this->date_meter_installed = new QDateEdit();
    this->date_meter_installed->setCalendarPopup(true);
    this->date_meter_installed->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    this->date_meter_installed->setMinimumDate(meterUnsetDate());
    this->date_meter_installed->setSpecialValueText("[Not set]");
    this->date_meter_installed->setDate(meterUnsetDate());

    QLabel *label_manufacturer = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Manufacturer"), nullptr);
    this->line_meter_manufacturer = new QLineEdit();

    QLabel *label_model = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Model"), nullptr);
    this->line_meter_model = new QLineEdit();

    QLabel *label_serial = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Serial Number"), nullptr);
    this->line_meter_serial = new QLineEdit();

    QLabel *label_description = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Description"), nullptr);
    this->edit_meter_description = new QPlainTextEdit();
    this->edit_meter_description->setMaximumHeight(80);

    QLabel *label_tags = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Tags"), nullptr);
    this->line_meter_tags = new QLineEdit();
    LiveUiText::bind(this->line_meter_tags, [this] {
        this->line_meter_tags->setPlaceholderText(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "billing, smart-meter, lorawan")));
    });
    LiveUiText::bind(this->line_meter_tags, [this] {
        this->line_meter_tags->setToolTip(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "Comma-separated tags. Empty entries and duplicates are ignored.")));
    });

    QLabel *label_comment = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Comment"), nullptr);
    this->edit_meter_comment = new QPlainTextEdit();
    this->edit_meter_comment->setMaximumHeight(80);

    int row = 0;
    fields->addWidget(label_id, row, 0);
    fields->addWidget(this->line_meter_id, row++, 1);
    fields->addWidget(this->check_meter_enabled, row++, 0, 1, 2);
    fields->addWidget(label_model_role, row, 0);
    fields->addWidget(this->combo_meter_model_role, row++, 1);
    fields->addWidget(label_date_added, row, 0);
    fields->addWidget(this->date_meter_added, row++, 1);
    fields->addWidget(label_date_installed, row, 0);
    fields->addWidget(this->date_meter_installed, row++, 1);
    fields->addWidget(label_manufacturer, row, 0);
    fields->addWidget(this->line_meter_manufacturer, row++, 1);
    fields->addWidget(label_model, row, 0);
    fields->addWidget(this->line_meter_model, row++, 1);
    fields->addWidget(label_serial, row, 0);
    fields->addWidget(this->line_meter_serial, row++, 1);
    fields->addWidget(label_description, row, 0, Qt::AlignTop);
    fields->addWidget(this->edit_meter_description, row++, 1);
    fields->addWidget(label_tags, row, 0);
    fields->addWidget(this->line_meter_tags, row++, 1);
    fields->addWidget(label_comment, row, 0, Qt::AlignTop);
    fields->addWidget(this->edit_meter_comment, row++, 1);
    fields->setColumnStretch(1, 1);

    grid->addWidget(this->widget_meter_fields, 3, 0, 1, 2);
    grid->setColumnStretch(1, 1);

    connect(this->button_meter_add, &QPushButton::clicked, this, [this]()
    {
        hydraulicData()->addDemandPointMeter(this->demand_point_uuid);
    });
    connect(this->button_meter_remove, &QPushButton::clicked, this, [this]()
    {
        hydraulicData()->removeDemandPointMeter(this->demand_point_uuid);
    });
    connect(this->line_meter_id, &QLineEdit::textEdited, this, [this](const QString &id)
    {
        updateMeter([id](WaterMeter &meter)
        {
            meter.id = id;
        });
    });
    connect(this->check_meter_enabled, &QCheckBox::toggled, this, [this](bool enabled)
    {
        updateMeter([enabled](WaterMeter &meter)
        {
            meter.metadata.enabled = enabled;
        });
    });
    connect(this->combo_meter_model_role, &QComboBox::currentIndexChanged, this, [this](int)
    {
        const EntityModelRole model_role = static_cast<EntityModelRole>(
            this->combo_meter_model_role->currentData().toInt());
        updateMeter([model_role](WaterMeter &meter)
        {
            meter.metadata.model_role = model_role;
        });
    });
    connect(this->date_meter_added, &QDateEdit::dateChanged, this, [this](const QDate &)
    {
        const std::optional<QDate> date_added = optionalMeterDate(this->date_meter_added);
        updateMeter([date_added](WaterMeter &meter)
        {
            meter.metadata.date_added = date_added;
        });
    });
    connect(this->date_meter_installed, &QDateEdit::dateChanged, this, [this](const QDate &)
    {
        const std::optional<QDate> date_installed =
            optionalMeterDate(this->date_meter_installed);
        updateMeter([date_installed](WaterMeter &meter)
        {
            meter.metadata.date_installed = date_installed;
        });
    });
    connect(this->line_meter_manufacturer, &QLineEdit::textEdited,
            this, [this](const QString &manufacturer)
    {
        updateMeter([manufacturer](WaterMeter &meter)
        {
            meter.manufacturer = manufacturer;
        });
    });
    connect(this->line_meter_model, &QLineEdit::textEdited, this, [this](const QString &model)
    {
        updateMeter([model](WaterMeter &meter)
        {
            meter.model = model;
        });
    });
    connect(this->line_meter_serial, &QLineEdit::textEdited, this, [this](const QString &serial)
    {
        updateMeter([serial](WaterMeter &meter)
        {
            meter.serial_number = serial;
        });
    });
    connect(this->edit_meter_description, &QPlainTextEdit::textChanged, this, [this]()
    {
        const QString description = this->edit_meter_description->toPlainText();
        updateMeter([description](WaterMeter &meter)
        {
            meter.metadata.description = description;
        });
    });
    connect(this->edit_meter_comment, &QPlainTextEdit::textChanged, this, [this]()
    {
        const QString comment = this->edit_meter_comment->toPlainText();
        updateMeter([comment](WaterMeter &meter)
        {
            meter.metadata.comment = comment;
        });
    });
    connect(this->line_meter_tags, &QLineEdit::editingFinished, this, [this]()
    {
        const QStringList tags = editedMeterTags();
        updateMeter([tags](WaterMeter &meter)
        {
            meter.metadata.tags = tags;
        });
    });

    layoutSimMeas()->addWidget(group);
}

void EntityInspectorDemandPoint::updateMeter(
    const std::function<void(WaterMeter &)> &mutation)
{
    const std::optional<HydraulicDemandPoint> demand_point =
        hydraulicData()->demandPoint(this->demand_point_uuid);
    if (!demand_point.has_value() || !demand_point->meter.has_value())
        return;

    WaterMeter meter = demand_point->meter.value();
    mutation(meter);
    hydraulicData()->setDemandPointMeter(this->demand_point_uuid, meter);
}

void EntityInspectorDemandPoint::refreshMeter()
{
    if (this->widget_meter_fields == nullptr)
        return;

    const std::optional<HydraulicDemandPoint> demand_point =
        hydraulicData()->demandPoint(this->demand_point_uuid);
    if (!demand_point.has_value())
        return;

    const bool has_meter = demand_point->meter.has_value();
    this->widget_meter_fields->setVisible(has_meter);
    this->button_meter_add->setVisible(!has_meter);
    this->button_meter_remove->setVisible(has_meter);

    if (!has_meter)
    {
        this->label_meter_uuid_value->setText(QStringLiteral("—"));
        return;
    }

    const WaterMeter &meter = demand_point->meter.value();
    this->label_meter_uuid_value->setText(meter.uuid.toString(QUuid::WithoutBraces));

    if (this->line_meter_id->text() != meter.id)
    {
        const QSignalBlocker blocker(this->line_meter_id);
        this->line_meter_id->setText(meter.id);
    }
    if (this->check_meter_enabled->isChecked() != meter.metadata.enabled)
    {
        const QSignalBlocker blocker(this->check_meter_enabled);
        this->check_meter_enabled->setChecked(meter.metadata.enabled);
    }

    const int role_index =
        this->combo_meter_model_role->findData(static_cast<int>(meter.metadata.model_role));
    if (role_index >= 0 && this->combo_meter_model_role->currentIndex() != role_index)
    {
        const QSignalBlocker blocker(this->combo_meter_model_role);
        this->combo_meter_model_role->setCurrentIndex(role_index);
    }

    const QDate date_added =
        meter.metadata.date_added.has_value() ? meter.metadata.date_added.value()
                                              : meterUnsetDate();
    if (this->date_meter_added->date() != date_added)
    {
        const QSignalBlocker blocker(this->date_meter_added);
        this->date_meter_added->setDate(date_added);
    }

    const QDate date_installed =
        meter.metadata.date_installed.has_value() ? meter.metadata.date_installed.value()
                                                  : meterUnsetDate();
    if (this->date_meter_installed->date() != date_installed)
    {
        const QSignalBlocker blocker(this->date_meter_installed);
        this->date_meter_installed->setDate(date_installed);
    }

    if (this->line_meter_manufacturer->text() != meter.manufacturer)
    {
        const QSignalBlocker blocker(this->line_meter_manufacturer);
        this->line_meter_manufacturer->setText(meter.manufacturer);
    }
    if (this->line_meter_model->text() != meter.model)
    {
        const QSignalBlocker blocker(this->line_meter_model);
        this->line_meter_model->setText(meter.model);
    }
    if (this->line_meter_serial->text() != meter.serial_number)
    {
        const QSignalBlocker blocker(this->line_meter_serial);
        this->line_meter_serial->setText(meter.serial_number);
    }
    if (this->edit_meter_description->toPlainText() != meter.metadata.description)
    {
        const QSignalBlocker blocker(this->edit_meter_description);
        this->edit_meter_description->setPlainText(meter.metadata.description);
    }
    if (this->edit_meter_comment->toPlainText() != meter.metadata.comment)
    {
        const QSignalBlocker blocker(this->edit_meter_comment);
        this->edit_meter_comment->setPlainText(meter.metadata.comment);
    }

    const QString tags_text = meter.metadata.tags.join(QStringLiteral(", "));
    if (this->line_meter_tags->text() != tags_text)
    {
        const QSignalBlocker blocker(this->line_meter_tags);
        this->line_meter_tags->setText(tags_text);
    }
}

void EntityInspectorDemandPoint::addGroupGraphs()
{
    GroupBoxCollapsible *group = EntityInspectorText::group(QT_TRANSLATE_NOOP("LiveUiText", "Graphs"));
    QGridLayout *grid = new QGridLayout(group);

    QLabel *label_devnote = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Water-meter observations and demand time series will be shown here."), nullptr);
    label_devnote->setWordWrap(true);

    grid->addWidget(label_devnote, 0, 0);
    layoutSimMeas()->addWidget(group);
}
