#include "map/core/map_annotation_distribution_targets.h"
#include "entity_inspector/entity_inspector_annotation.h"

#include <QApplication>
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
    QWidget *parent, const std::optional<MapAnnotation> &draft)
    : QWidget(parent), data_(data), annotation_uuid_(existing ? existing->uuid : QUuid()),
      creating_(!existing.has_value()), initial_position_(new_position), initial_draft_(draft)
{
    if (this->initial_draft_)
        this->draft_uuid_ = this->initial_draft_->uuid;
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
    this->background_->setObjectName(QStringLiteral("mapAnnotationBackground"));
    this->form_->addRow(QString(), this->background_);
    this->visible_ = new QCheckBox(tr("Visible on monitor map"), this);
    this->visible_->setObjectName(QStringLiteral("mapAnnotationVisible"));
    this->form_->addRow(QString(), this->visible_);
    this->anchor_ = new QComboBox(this);
    this->form_->addRow(tr("Attached to"), this->anchor_);
    layout->addLayout(this->form_);

    this->message_ = new QLabel(this);
    this->message_->setWordWrap(true);
    // Place the writable/read-only explanation ABOVE the form, where a
    // narrow dock cannot hide it below the latitude/longitude controls.
    layout->insertWidget(2, this->message_);
    this->delete_ = new QPushButton(tr("Delete annotation"), this);
    layout->addWidget(this->delete_);
    layout->addStretch();

    this->commit_timer_.setSingleShot(true);
    this->commit_timer_.setInterval(350);
    connect(&this->commit_timer_, &QTimer::timeout, this, &EntityInspectorAnnotation::commit);
    const std::function<void()> changed = [this]
    {
        if (this->updating_ui_ || (!this->creating_ && !this->data_->logicalEditCaptureEnabled()))
            return;
        // A blank new annotation is a visible local draft. MODEL rejects
        // blank persisted text, so the first valid text automatically creates
        // the real record through the existing durable MODEL edit event.
        publishDraftPreview();
        this->commit_timer_.start();
    };
    connect(this->text_, &QPlainTextEdit::textChanged, this, changed);
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
    connect(this->delete_, &QPushButton::clicked, this, [this]
    {
        if (this->creating_)
        {
            emit draftDeleted(this->draft_uuid_);
            return;
        }
        if (!this->data_->logicalEditCaptureEnabled())
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
    // An editing session (one History item) also ends when keyboard focus
    // leaves this inspector, e.g. to the map or another panel. Popups of its
    // own controls are children of them and do not end it; neither does the
    // whole window losing focus.
    this->focus_connection_ = connect(qApp, &QApplication::focusChanged, this,
                                      [this](QWidget *old, QWidget *now)
    {
        if (now != nullptr && containsWidget(old) && !containsWidget(now))
            flushPendingEdit();
    });
    connect(this->data_, &HydraulicData::signalLogicalEditCaptureChanged,
            this, [this](bool enabled)
    {
        refreshPermission();
        if (enabled && this->creating_ && !this->text_->toPlainText().trimmed().isEmpty())
            this->commit_timer_.start();
    });
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
    this->visible_->setText(tr("Visible on monitor map"));
    this->delete_->setText(this->creating_ ? tr("Discard draft") : tr("Delete annotation"));
    refreshFromModel();
    refreshPermission();
}

EntityInspectorAnnotation::~EntityInspectorAnnotation()
{
    // Child widgets are destroyed after this body and the members; a focus
    // change during that must not reach this half-destroyed inspector.
    QObject::disconnect(this->focus_connection_);
    flushPendingEdit();
}

void EntityInspectorAnnotation::flushPendingEdit()
{
    if (this->commit_timer_.isActive())
    {
        // Stop before publishing: a failed synchronous queue append can revoke
        // edit permission and re-enter the closing signal in the same call stack.
        this->commit_timer_.stop();
        commit();
    }
    // End of the editing session: everything typed since it began becomes
    // ONE MODEL event, so one History item.
    if (this->data_ != nullptr)
        this->data_->finishMapAnnotationEdit();
}

bool EntityInspectorAnnotation::containsWidget(const QWidget *widget) const
{
    // Walk parents across windows: a combo box popup is its own window.
    for (const QWidget *at = widget; at != nullptr; at = at->parentWidget())
        if (at == this)
            return true;
    return false;
}

void EntityInspectorAnnotation::refreshAnnotationChoices()
{
    const QSignalBlocker blocker(this->browse_);
    this->browse_->clear();
    for (const MapAnnotation &draft : this->pending_drafts_)
    {
        const QString summary = draft.text.simplified().left(40);
        const QString coordinates = QStringLiteral(" (%1°, %2°)")
            .arg(draft.position_wgs84.latitude_deg, 0, 'f', 5)
            .arg(draft.position_wgs84.longitude_deg, 0, 'f', 5);
        this->browse_->addItem(summary.isEmpty() ? tr("New annotation") + coordinates : summary,
                               draft.uuid.toString(QUuid::WithoutBraces));
    }
    for (const MapAnnotation &annotation : this->data_->mapAnnotations())
    {
        const QString summary = annotation.text.simplified().left(40);
        this->browse_->addItem(summary.isEmpty() ? tr("Untitled annotation") : summary,
                               annotation.uuid.toString(QUuid::WithoutBraces));
    }
    const QUuid chosen_uuid = this->creating_ ? this->draft_uuid_ : this->annotation_uuid_;
    if (!chosen_uuid.isNull())
    {
        const int selected = this->browse_->findData(
            chosen_uuid.toString(QUuid::WithoutBraces));
        if (selected >= 0)
            this->browse_->setCurrentIndex(selected);
    }
}

bool EntityInspectorAnnotation::isCreating() const
{
    return this->creating_;
}

QUuid EntityInspectorAnnotation::draftUuid() const
{
    return this->draft_uuid_;
}

void EntityInspectorAnnotation::discardDraft()
{
    this->discarded_ = true;
    this->commit_timer_.stop();
}

void EntityInspectorAnnotation::setPendingDraftChoices(const QList<MapAnnotation> &drafts)
{
    this->pending_drafts_ = drafts;
    refreshAnnotationChoices();
}

CoordinateWGS84 EntityInspectorAnnotation::enteredPosition() const
{
    CoordinateWGS84 coordinate = this->initial_position_;
    coordinate.latitude_deg = this->latitude_->value();
    coordinate.longitude_deg = this->longitude_->value();
    return coordinate;
}

void EntityInspectorAnnotation::publishDraftPreview()
{
    // The map must acknowledge a placement even when the selected revision is
    // read-only. This is a local preview, NOT a MODEL edit or DVMDB record.
    // Text can be drafted locally at any time; durable MODEL edits still
    // require a verified writable working copy.
    if (!this->creating_ || this->updating_ui_ || this->discarded_)
        return;
    MapAnnotation preview;
    preview.uuid = this->draft_uuid_;
    preview.position_wgs84 = enteredPosition();
    preview.text = this->text_->toPlainText();
    preview.style.text_size = static_cast<MapAnnotationTextSize>(this->size_->currentIndex());
    // The same visibility flag applies to local drafts and persisted records.
    // Even a hidden draft remains selectable from the Inspector list.
    preview.style.show_background = this->background_->isChecked();
    preview.style.visible = this->visible_->isChecked();
    const QUuid anchor_id(this->anchor_->currentData().toString());
    if (!anchor_id.isNull())
    {
        preview.anchor = MapAnnotationAnchor{
            anchor_id, this->anchor_->currentData(Qt::UserRole + 1).toString()};
        MapAnnotationPosition::place(this->data_->networkHydraulicDistribution(),
                                     preview, enteredPosition());
    }
    emit draftPreviewChanged(preview);
}

void EntityInspectorAnnotation::moveDraft(const CoordinateWGS84 &position)
{
    if (!this->creating_ || this->discarded_)
        return;
    this->latitude_->setValue(position.latitude_deg);
    this->longitude_->setValue(position.longitude_deg);
    publishDraftPreview();
}

void EntityInspectorAnnotation::focusText()
{
    this->text_->setFocus(Qt::OtherFocusReason);
}

void EntityInspectorAnnotation::refreshPermission()
{
    const bool writable = this->data_ != nullptr && this->data_->logicalEditCaptureEnabled()
        && (this->creating_ || this->data_->mapAnnotation(this->annotation_uuid_).has_value());
    // Local drafts are always editable. Their content remains explicitly
    // unsaved until a verified private working copy accepts the MODEL edit.
    const bool editable = this->creating_ || writable;
    this->text_->setReadOnly(!editable);
    this->latitude_->setReadOnly(!editable);
    this->longitude_->setReadOnly(!editable);
    this->size_->setEnabled(editable);
    this->background_->setEnabled(editable);
    this->visible_->setEnabled(editable);
    this->anchor_->setEnabled(editable);
    this->delete_->setVisible(true);
    this->delete_->setText(this->creating_ ? tr("Discard draft") : tr("Delete annotation"));
    this->delete_->setEnabled(editable);
    if (this->creating_ && !writable)
        this->message_->setText(tr("Unsaved draft: open a writable private working copy to save."));
    else if (!writable)
        this->message_->setText(tr("Read-only: open a writable private working copy to edit."));
    else if (this->creating_)
        this->message_->setText(tr("Draft: type text to save automatically."));
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
        if (this->updating_ui_ || this->draft_initialized_)
            return; // Network updates must not erase a moved, unsaved draft.
        this->updating_ui_ = true;
        const MapAnnotation draft = this->initial_draft_.value_or(MapAnnotation());
        this->text_->setPlainText(draft.text);
        this->latitude_->setValue(this->initial_position_.latitude_deg);
        this->longitude_->setValue(this->initial_position_.longitude_deg);
        this->size_->setCurrentIndex(int(draft.style.text_size));
        this->background_->setChecked(draft.style.show_background);
        this->visible_->setChecked(draft.style.visible);
        refreshAnchorChoices(draft.anchor ? std::optional<MapAnnotation>(draft) : std::nullopt);
        this->position_dirty_ = false;
        this->draft_initialized_ = true;
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
    // Same text (the field may hold trailing whitespace the model trims):
    // keep the field and its cursor.
    if (annotation->text != this->text_->toPlainText().trimmed())
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
    if (this->discarded_ || !this->data_->logicalEditCaptureEnabled())
        return;
    if (this->creating_)
    {
        const QString value = this->text_->toPlainText().trimmed();
        if (value.isEmpty())
            return; // Blank placeholder stays local; no invalid MODEL event.
        MapAnnotationStyle style;
        style.text_size = static_cast<MapAnnotationTextSize>(this->size_->currentIndex());
        style.show_background = this->background_->isChecked();
        style.visible = this->visible_->isChecked();
        std::optional<MapAnnotationAnchor> anchor;
        const QUuid selected(this->anchor_->currentData().toString());
        if (!selected.isNull())
        {
            MapAnnotation positioned;
            positioned.anchor = MapAnnotationAnchor{
                selected, this->anchor_->currentData(Qt::UserRole + 1).toString()};
            if (!MapAnnotationPosition::place(
                    this->data_->networkHydraulicDistribution(), positioned, enteredPosition()))
            {
                this->message_->setText(tr("Invalid annotation or failed working-copy transaction."));
                return;
            }
            anchor = positioned.anchor;
        }
        QUuid id;
        // The creation joins the editing session: it and the typing after it
        // are recorded together when the session ends.
        this->data_->beginMapAnnotationEdit(QUuid());
        // signalMapAnnotationsChanged is emitted synchronously. Do not let
        // that refresh erase the unsaved text/caret before creation completes.
        this->updating_ui_ = true;
        const bool created = this->data_->createMapAnnotation(
            value, enteredPosition(), &id, style, anchor);
        this->updating_ui_ = false;
        if (!created)
        {
            // Close the session begun for this creation: an unrelated later
            // creation must not join it.
            this->data_->finishMapAnnotationEdit();
            this->message_->setText(tr("Annotation could not be recorded in the working copy."));
            return;
        }
        this->annotation_uuid_ = id;
        this->creating_ = false;
        this->position_dirty_ = false;
        refreshAnnotationChoices();
        refreshPermission();
        // Keep this QWidget and its focused text cursor alive. Replacing the
        // inspector during typing loses keystrokes and visibly interrupts UX.
        emit annotationCreated(id);
        return;
    }
    const std::optional<MapAnnotation> current = this->data_->mapAnnotation(this->annotation_uuid_);
    if (!current)
        return;
    MapAnnotation updated = *current;
    updated.text = this->text_->toPlainText().trimmed();
    if (updated.text.isEmpty())
    {
        this->message_->setText(tr("Enter annotation text"));
        return;
    }
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
    // Live on the map now, recorded as one event when the session ends.
    this->data_->beginMapAnnotationEdit(this->annotation_uuid_);
    // The synchronous refresh must not reset the text cursor while typing.
    this->updating_ui_ = true;
    const bool updated_ok = this->data_->updateMapAnnotation(updated);
    this->updating_ui_ = false;
    if (!updated_ok)
        this->message_->setText(tr("Invalid annotation or failed working-copy transaction."));
    else
    {
        this->position_dirty_ = false;
        this->message_->clear();
    }
}
