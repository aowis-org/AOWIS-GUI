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
    
    // A pending blank annotation belongs to the editor session, not the
    // currently displayed Inspector. Discard it on network/revision reload,
    // never merely because another annotation was selected.
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]()
    {
        if (this->widget_annotation && this->widget_annotation->isCreating())
        {
            clearEntity();
            setVisible(false);
        }
        if (!this->pending_annotation_drafts.isEmpty())
        {
            this->pending_annotation_drafts.clear();
            emit mapAnnotationDraftCleared();
            emit mapAnnotationDraftsChanged();
        }
    });
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
    // deleteLater() is not immediate. If the old annotation editor has a
    // debounced text edit, commit it against its original UUID NOW, before
    // selection changes or its owning draft/network can be replaced.
    if (this->widget_annotation)
    {
        this->widget_annotation->flushPendingEdit();
        // deleteLater() leaves the outgoing widget alive until the next Qt
        // event loop pass. It must not react to another session/History
        // signal or emit a stale draft update after selection changes.
        QObject::disconnect(this->hydraulic_data, nullptr, this->widget_annotation, nullptr);
        QObject::disconnect(this->widget_annotation, nullptr, this, nullptr);
    }
    if (this->widget_current)
        this->widget_current->deleteLater();
    if (this->widget_annotation)
        this->widget_annotation->deleteLater();
    this->widget_current = nullptr;
    this->widget_annotation = nullptr;
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

const QList<MapAnnotation> &EntityInspectorDock::pendingMapAnnotations() const
{
    return this->pending_annotation_drafts;
}

std::optional<MapAnnotation> EntityInspectorDock::pendingMapAnnotation(const QUuid &uuid) const
{
    for (const MapAnnotation &draft : this->pending_annotation_drafts)
        if (draft.uuid == uuid)
            return draft;
    return std::nullopt;
}

void EntityInspectorDock::updateInspectorDraftChoices()
{
    if (this->widget_annotation)
        this->widget_annotation->setPendingDraftChoices(this->pending_annotation_drafts);
}

void EntityInspectorDock::rememberDraft(const MapAnnotation &annotation)
{
    for (MapAnnotation &draft : this->pending_annotation_drafts)
    {
        if (draft.uuid == annotation.uuid)
        {
            draft = annotation;
            emit mapAnnotationDraftUpdated(annotation);
            emit mapAnnotationDraftsChanged();
            updateInspectorDraftChoices();
            return;
        }
    }
    this->pending_annotation_drafts.append(annotation);
    emit mapAnnotationDraftUpdated(annotation);
    emit mapAnnotationDraftsChanged();
    updateInspectorDraftChoices();
}

void EntityInspectorDock::discardDraft(const QUuid &uuid)
{
    for (qsizetype i = 0; i < this->pending_annotation_drafts.size(); ++i)
    {
        if (this->pending_annotation_drafts.at(i).uuid != uuid)
            continue;
        this->pending_annotation_drafts.removeAt(i);
        emit mapAnnotationDraftCleared();
        emit mapAnnotationDraftsChanged();
        updateInspectorDraftChoices();
        return;
    }
}

void EntityInspectorDock::removePendingMapAnnotation(const QUuid &uuid)
{
    if (this->widget_annotation && this->widget_annotation->isCreating()
        && this->widget_annotation->draftUuid() == uuid)
    {
        // Do not recreate the discarded draft from the edit flush.
        this->widget_annotation->discardDraft();
        clearEntity();
        setVisible(false);
    }
    discardDraft(uuid);
}

void EntityInspectorDock::beginMapAnnotation(const CoordinateWGS84 &coordinate)
{
    MapAnnotation draft;
    draft.uuid = QUuid::createUuid(); // Local placeholder, never a persisted ID.
    draft.position_wgs84 = coordinate;
    draft.style.visible = true;
    draft.style.show_background = true;
    beginMapAnnotation(draft);
}

void EntityInspectorDock::beginMapAnnotation(const MapAnnotation &draft)
{
    if (draft.uuid.isNull())
        return;
    clearEntity();
    rememberDraft(draft);
    this->widget_annotation = new EntityInspectorAnnotation(
        this->hydraulic_data, std::nullopt, draft.position_wgs84, this, draft);
    connect(this->widget_annotation, &EntityInspectorAnnotation::annotationCreated,
            this, [this, uuid = draft.uuid](const QUuid &id)
    {
        discardDraft(uuid);
        emit mapAnnotationSelected(id);
    });
    connect(this->widget_annotation, &EntityInspectorAnnotation::draftPreviewChanged,
            this, &EntityInspectorDock::rememberDraft);
    connect(this->widget_annotation, &EntityInspectorAnnotation::annotationSelected,
            this, &EntityInspectorDock::showMapAnnotation);
    connect(this->widget_annotation, &EntityInspectorAnnotation::draftDeleted,
            this, &EntityInspectorDock::removePendingMapAnnotation);
    setWidget(this->widget_annotation);
    updateInspectorDraftChoices();
    setVisible(true);
    this->widget_annotation->focusText();
}

void EntityInspectorDock::selectPendingMapAnnotation(const QUuid &uuid)
{
    const std::optional<MapAnnotation> draft = pendingMapAnnotation(uuid);
    if (!draft)
        return;
    if (this->widget_annotation && this->widget_annotation->isCreating()
        && this->widget_annotation->draftUuid() == uuid)
    {
        this->widget_annotation->focusText();
        setVisible(true);
        return;
    }
    beginMapAnnotation(*draft);
}

void EntityInspectorDock::movePendingMapAnnotation(const QUuid &uuid,
                                                     const CoordinateWGS84 &position)
{
    const std::optional<MapAnnotation> draft = pendingMapAnnotation(uuid);
    if (!draft)
        return;
    if (this->widget_annotation && this->widget_annotation->isCreating()
        && this->widget_annotation->draftUuid() == uuid)
    {
        this->widget_annotation->moveDraft(position);
        return;
    }
    MapAnnotation moved = *draft;
    moved.position_wgs84 = position;
    rememberDraft(moved);
}

void EntityInspectorDock::showMapAnnotation(const QUuid &uuid)
{
    if (pendingMapAnnotation(uuid))
    {
        selectPendingMapAnnotation(uuid);
        return;
    }
    const std::optional<MapAnnotation> annotation = this->hydraulic_data->mapAnnotation(uuid);
    if (!annotation)
        return;
    clearEntity();
    this->widget_annotation = new EntityInspectorAnnotation(this->hydraulic_data, annotation,
                                                           annotation->position_wgs84, this);
    connect(this->widget_annotation, &EntityInspectorAnnotation::annotationDeleted,
            this, [this] { clearEntity(); setVisible(false); });
    connect(this->widget_annotation, &EntityInspectorAnnotation::annotationSelected,
            this, &EntityInspectorDock::showMapAnnotation);
    setWidget(this->widget_annotation);
    updateInspectorDraftChoices();
    setVisible(true);
    emit mapAnnotationSelected(uuid);
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
