#include "map/core/map_annotation_distribution_targets.h"
#include "entity_inspector/entity_inspector_annotation.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QFont>
#include <functional>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

EntityInspectorAnnotation::EntityInspectorAnnotation(HydraulicData *data,
    const std::optional<MapAnnotation> &existing, const CoordinateWGS84 &new_position,
    QWidget *parent)
    : QWidget(parent), data_(data), annotation_uuid_(existing ? existing->uuid : QUuid()),
      creating_(!existing.has_value()), initial_position_(new_position)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 12, 10, 12);
    this->heading_ = new QLabel(tr("Map annotation"), this);
    QFont heading_font = this->heading_->font();
    heading_font.setBold(true);
    this->heading_->setFont(heading_font);
    layout->addWidget(this->heading_);
    this->browse_ = new QComboBox(this);
    this->browse_->setToolTip(tr("Find another annotation in this distribution network"));
    layout->addWidget(this->browse_);
    connect(this->browse_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index)
    {
        if (index < 0 || this->updating_ui_)
            return;
        const QUuid chosen(this->browse_->itemData(index).toString());
        if (!chosen.isNull() && chosen != this->annotation_uuid_)
            emit annotationSelected(chosen);
    });

    this->form_ = new QFormLayout();
    this->form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    this->text_ = new QPlainTextEdit(this);
    this->text_->setPlaceholderText(tr("Enter annotation text"));
    this->text_->setMaximumBlockCount(64);
    this->text_->setFixedHeight(108);
    this->form_->addRow(tr("Text"), this->text_);

    this->latitude_ = new QDoubleSpinBox(this);
    this->latitude_->setRange(-90.0, 90.0);
    this->latitude_->setDecimals(8);
    this->latitude_->setSingleStep(0.00001);
    this->latitude_->setSuffix(QStringLiteral(" °"));
    this->form_->addRow(tr("Latitude"), this->latitude_);
    this->longitude_ = new QDoubleSpinBox(this);
    this->longitude_->setRange(-180.0, 180.0);
    this->longitude_->setDecimals(8);
    this->longitude_->setSingleStep(0.00001);
    this->longitude_->setSuffix(QStringLiteral(" °"));
    this->form_->addRow(tr("Longitude"), this->longitude_);

    this->size_ = new QComboBox(this);
    this->size_->addItem(tr("Small"));
    this->size_->addItem(tr("Normal"));
    this->size_->addItem(tr("Large"));
    this->size_->setCurrentIndex(int(MapAnnotationTextSize::Normal));
    this->form_->addRow(tr("Text size"), this->size_);
    this->background_ = new QCheckBox(tr("Show background"), this);
    this->form_->addRow(QString(), this->background_);
    this->visible_ = new QCheckBox(tr("Visible on map"), this);
    this->form_->addRow(QString(), this->visible_);
    this->anchor_ = new QComboBox(this);
    this->form_->addRow(tr("Attached to"), this->anchor_);
    layout->addLayout(this->form_);

    this->message_ = new QLabel(this);
    this->message_->setWordWrap(true);
    layout->addWidget(this->message_);
    this->create_ = new QPushButton(tr("Create annotation"), this);
    this->delete_ = new QPushButton(tr("Delete annotation"), this);
    layout->addWidget(this->create_);
    layout->addWidget(this->delete_);
    layout->addStretch();

    this->commit_timer_.setSingleShot(true);
    this->commit_timer_.setInterval(350);
    connect(&this->commit_timer_, &QTimer::timeout, this, &EntityInspectorAnnotation::commit);
    connect(this->text_, &QPlainTextEdit::textChanged, this, [this]
    {
        if (!this->updating_ui_ && !this->creating_)
            this->commit_timer_.start();
        if (this->creating_)
            this->create_->setEnabled(this->data_->logicalEditCaptureEnabled()
                                      && !this->text_->toPlainText().trimmed().isEmpty());
    });
    const std::function<void()> changed = [this]
    {
        if (!this->updating_ui_ && !this->creating_)
            this->commit_timer_.start();
    };
    connect(this->latitude_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this, changed](double)
    {
        if (!this->updating_ui_)
            this->position_dirty_ = true;
        changed();
    });
    connect(this->longitude_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this, changed](double)
    {
        if (!this->updating_ui_)
            this->position_dirty_ = true;
        changed();
    });
    connect(this->size_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [changed](int) { changed(); });
    connect(this->background_, &QCheckBox::toggled, this, [changed](bool) { changed(); });
    connect(this->visible_, &QCheckBox::toggled, this, [changed](bool) { changed(); });
    connect(this->anchor_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [changed](int) { changed(); });
    connect(this->create_, &QPushButton::clicked, this, [this]
    {
        if (!this->creating_ || !this->data_->logicalEditCaptureEnabled())
            return;
        CoordinateWGS84 coordinate = this->initial_position_;
        coordinate.latitude_deg = this->latitude_->value();
        coordinate.longitude_deg = this->longitude_->value();
        QUuid id;
        MapAnnotationStyle style;
        style.text_size = static_cast<MapAnnotationTextSize>(this->size_->currentIndex());
        style.show_background = this->background_->isChecked();
        style.visible = this->visible_->isChecked();
        std::optional<MapAnnotationAnchor> anchor;
        const QUuid selected(this->anchor_->currentData().toString());
        if (!selected.isNull())
        {
            anchor = MapAnnotationAnchor{selected,
                this->anchor_->currentData(Qt::UserRole + 1).toString()};
            MapAnnotation positioned;
            positioned.anchor = anchor;
            if (!MapAnnotationPosition::place(
                    this->data_->networkHydraulicDistribution(), positioned, coordinate))
            {
                this->message_->setText(tr("Invalid annotation or failed working-copy transaction."));
                return;
            }
            anchor = positioned.anchor;
        }
        if (!this->data_->createMapAnnotation(this->text_->toPlainText().trimmed(),
                                               coordinate, &id, style, anchor))
        {
            this->message_->setText(tr("Annotation could not be recorded in the working copy."));
            return;
        }
        emit annotationCreated(id);
    });
    connect(this->delete_, &QPushButton::clicked, this, [this]
    {
        if (this->creating_ || !this->data_->logicalEditCaptureEnabled())
            return;
        if (!this->data_->deleteMapAnnotation(this->annotation_uuid_))
        {
            this->message_->setText(tr("Annotation could not be deleted."));
            return;
        }
        emit annotationDeleted();
    });
    connect(this->data_, &HydraulicData::signalLogicalEditCaptureAboutToDisable,
            this, &EntityInspectorAnnotation::flushPendingEdit);
    connect(this->data_, &HydraulicData::signalLogicalEditCaptureChanged,
            this, &EntityInspectorAnnotation::refreshPermission);
    connect(this->data_, &HydraulicData::signalMapAnnotationsChanged,
            this, &EntityInspectorAnnotation::refreshFromModel);
    connect(this->data_, &HydraulicData::signalNetworkLoaded,
            this, &EntityInspectorAnnotation::refreshFromModel);
    connect(this->data_, &HydraulicData::signalNetworkGeometryChanged,
            this, [this](quint64)
    {
        if (!this->commit_timer_.isActive())
            refreshFromModel();
    });
    refreshFromModel();
    refreshPermission();
}

void EntityInspectorAnnotation::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && this->heading_ != nullptr)
        retranslate();
}

void EntityInspectorAnnotation::retranslate()
{
    this->heading_->setText(tr("Map annotation"));
    this->browse_->setToolTip(tr("Find another annotation in this distribution network"));
    this->text_->setPlaceholderText(tr("Enter annotation text"));
    const std::function<void(QWidget *, const QString &)> label = [this](
        QWidget *field, const QString &value)
    {
        QLabel *text = qobject_cast<QLabel *>(this->form_->labelForField(field));
        if (text != nullptr)
            text->setText(value);
    };
    label(this->text_, tr("Text"));
    label(this->latitude_, tr("Latitude"));
    label(this->longitude_, tr("Longitude"));
    label(this->size_, tr("Text size"));
    label(this->anchor_, tr("Attached to"));
    this->size_->setItemText(0, tr("Small"));
    this->size_->setItemText(1, tr("Normal"));
    this->size_->setItemText(2, tr("Large"));
    this->background_->setText(tr("Show background"));
    this->visible_->setText(tr("Visible on map"));
    this->create_->setText(tr("Create annotation"));
    this->delete_->setText(tr("Delete annotation"));
    refreshFromModel();
    refreshPermission();
}

EntityInspectorAnnotation::~EntityInspectorAnnotation()
{
    flushPendingEdit();
}

void EntityInspectorAnnotation::flushPendingEdit()
{
    if (!this->commit_timer_.isActive())
        return;
    // Stop before publishing: a failed synchronous queue append can revoke
    // edit permission and re-enter the closing signal in the same call stack.
    this->commit_timer_.stop();
    commit();
}

void EntityInspectorAnnotation::refreshAnnotationChoices()
{
    const QSignalBlocker blocker(this->browse_);
    this->browse_->clear();
    if (this->creating_)
        this->browse_->addItem(tr("New annotation"), QString());
    for (const MapAnnotation &annotation : this->data_->mapAnnotations())
    {
        const QString summary = annotation.text.simplified().left(40);
        this->browse_->addItem(summary.isEmpty() ? tr("Untitled annotation") : summary,
                               annotation.uuid.toString(QUuid::WithoutBraces));
    }
    if (!this->annotation_uuid_.isNull())
    {
        const int selected = this->browse_->findData(
            this->annotation_uuid_.toString(QUuid::WithoutBraces));
        if (selected >= 0)
            this->browse_->setCurrentIndex(selected);
    }
}

void EntityInspectorAnnotation::focusText()
{
    this->text_->setFocus(Qt::OtherFocusReason);
}

void EntityInspectorAnnotation::refreshPermission()
{
    const bool writable = this->data_ != nullptr && this->data_->logicalEditCaptureEnabled()
        && (this->creating_ || this->data_->mapAnnotation(this->annotation_uuid_).has_value());
    this->text_->setReadOnly(!writable);
    this->latitude_->setReadOnly(!writable);
    this->longitude_->setReadOnly(!writable);
    this->size_->setEnabled(writable);
    this->background_->setEnabled(writable);
    this->visible_->setEnabled(writable);
    this->anchor_->setEnabled(writable);
    this->create_->setVisible(this->creating_);
    this->delete_->setVisible(!this->creating_);
    this->create_->setEnabled(writable && !this->text_->toPlainText().trimmed().isEmpty());
    this->delete_->setEnabled(writable);
    if (!writable)
        this->message_->setText(tr("Read-only: open a writable private working copy to edit."));
    else
        this->message_->clear();
}

void EntityInspectorAnnotation::refreshAnchorChoices(
    const std::optional<MapAnnotation> &current)
{
    const QSignalBlocker blocked(this->anchor_);
    this->anchor_->clear();
    this->anchor_->addItem(tr("None"), QString());
    const QList<MapAnnotationPosition::Target> targets = MapAnnotationPosition::targets(
        this->data_->networkHydraulicDistribution());
    int chosen = 0;
    for (const MapAnnotationPosition::Target &target : targets)
    {
        const QString identity = target.uuid.toString(QUuid::WithoutBraces);
        this->anchor_->addItem(tr("%1 (%2)").arg(target.id, target.type), identity);
        const int index = this->anchor_->count() - 1;
        this->anchor_->setItemData(index, target.type, Qt::UserRole + 1);
        if (current && current->anchor && current->anchor->entity_uuid == target.uuid &&
            current->anchor->entity_type == target.type)
            chosen = index;
    }
    if (current && current->anchor && chosen == 0)
    {
        // Preserve an unresolved reference while the user edits text; never
        // silently detach it merely because its target isn't in this revision.
        this->anchor_->addItem(
            tr("%1 (%2)").arg(current->anchor->entity_uuid.toString(QUuid::WithoutBraces),
                              current->anchor->entity_type),
            current->anchor->entity_uuid.toString(QUuid::WithoutBraces));
        chosen = this->anchor_->count() - 1;
        this->anchor_->setItemData(chosen, current->anchor->entity_type, Qt::UserRole + 1);
    }
    this->anchor_->setCurrentIndex(chosen);
}

void EntityInspectorAnnotation::refreshFromModel()
{
    refreshAnnotationChoices();
    if (this->creating_)
    {
        if (this->updating_ui_)
            return;
        this->updating_ui_ = true;
        this->latitude_->setValue(this->initial_position_.latitude_deg);
        this->longitude_->setValue(this->initial_position_.longitude_deg);
        refreshAnchorChoices(std::nullopt);
        this->position_dirty_ = false;
        this->updating_ui_ = false;
        return;
    }
    const std::optional<MapAnnotation> annotation = this->data_->mapAnnotation(this->annotation_uuid_);
    if (!annotation)
    {
        this->message_->setText(tr("Annotation no longer exists at this History position."));
        this->text_->setReadOnly(true);
        this->delete_->setEnabled(false);
        return;
    }
    // Do not overwrite a user's pending typing with an unrelated model refresh.
    if (this->commit_timer_.isActive() || this->updating_ui_)
        return;
    this->updating_ui_ = true;
    this->text_->setPlainText(annotation->text);
    const std::optional<CoordinateWGS84> resolved = MapAnnotationPosition::resolve(
        this->data_->networkHydraulicDistribution(), *annotation);
    const CoordinateWGS84 &position = resolved ? *resolved : annotation->position_wgs84;
    this->latitude_->setValue(position.latitude_deg);
    this->longitude_->setValue(position.longitude_deg);
    this->size_->setCurrentIndex(int(annotation->style.text_size));
    this->background_->setChecked(annotation->style.show_background);
    this->visible_->setChecked(annotation->style.visible);
    refreshAnchorChoices(annotation);
    this->position_dirty_ = false;
    this->updating_ui_ = false;
}

void EntityInspectorAnnotation::commit()
{
    if (this->creating_ || !this->data_->logicalEditCaptureEnabled())
        return;
    const std::optional<MapAnnotation> current = this->data_->mapAnnotation(this->annotation_uuid_);
    if (!current)
        return;
    MapAnnotation updated = *current;
    updated.text = this->text_->toPlainText().trimmed();
    const QUuid selected_anchor(this->anchor_->currentData().toString());
    const QString selected_type = this->anchor_->currentData(Qt::UserRole + 1).toString();
    const bool anchor_changed = (!current->anchor && !selected_anchor.isNull()) ||
        (current->anchor && (current->anchor->entity_uuid != selected_anchor ||
                             current->anchor->entity_type != selected_type));
    if (anchor_changed || this->position_dirty_)
    {
        CoordinateWGS84 position = current->position_wgs84;
        const std::optional<CoordinateWGS84> actual = MapAnnotationPosition::resolve(
            this->data_->networkHydraulicDistribution(), *current);
        if (actual)
            position = *actual;
        if (this->position_dirty_)
        {
            position.latitude_deg = this->latitude_->value();
            position.longitude_deg = this->longitude_->value();
        }
        updated.anchor.reset();
        if (!selected_anchor.isNull())
            updated.anchor = MapAnnotationAnchor{selected_anchor, selected_type};
        if (!MapAnnotationPosition::place(
                this->data_->networkHydraulicDistribution(), updated, position))
        {
            this->message_->setText(tr("Invalid annotation or failed working-copy transaction."));
            return;
        }
    }
    updated.style.text_size = static_cast<MapAnnotationTextSize>(this->size_->currentIndex());
    updated.style.show_background = this->background_->isChecked();
    updated.style.visible = this->visible_->isChecked();
    if (!this->data_->updateMapAnnotation(updated))
        this->message_->setText(tr("Invalid annotation or failed working-copy transaction."));
    else
    {
        this->position_dirty_ = false;
        this->message_->clear();
    }
}
