// Acceptance test running inside the actual GUI executable. Linking the normal
// application target gives this test the same inspector, model and Qt resources
// as a user session without launching MainWindow or initializing bgfx.
#include "config/gui_language.h"
#include "entity_inspector/entity_inspector_tank.h"
#include "entity_inspector/entity_inspector_dock.h"
#include "network/hydraulic_data.h"
#include "widgets/group_box_collapsible.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/serialization/model_codec.h>

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>

#include <array>
#include <functional>

namespace
{
bool expect(bool condition, const QString &language, const char *description)
{
    if (condition)
        return true;
    qCritical().noquote() << "Live Entity Inspector" << language << "FAIL:" << description;
    return false;
}
}

// Annotation editor acceptance in the real QWidget dock. This specifically
// exercises the synchronous handoff of the inspector's 350-ms text debounce;
// the MODEL/queue replay suite covers the resulting canonical transactions.
int runAnnotationInspectorAcceptance(QApplication &app)
{
    const QString check = QStringLiteral("annotation inspector");
    NetworkHydraulicDistribution network;
    network.uuid = QUuid(QStringLiteral("{019a6010-0001-7000-8000-000000000011}"));
    network.id = QStringLiteral("Inspection network");

    MapAnnotation first;
    first.uuid = QUuid(QStringLiteral("{019a6010-0001-7000-8000-000000000012}"));
    first.text = QStringLiteral("First annotation");
    first.position_wgs84.latitude_deg = 50.0;
    first.position_wgs84.longitude_deg = 8.0;

    MapAnnotation second;
    second.uuid = QUuid(QStringLiteral("{019a6010-0001-7000-8000-000000000013}"));
    second.text = QStringLiteral("Second annotation");
    second.position_wgs84.latitude_deg = 50.1;
    second.position_wgs84.longitude_deg = 8.1;

    // A right-click starts a visible, empty GUI placeholder immediately.
    // MODEL requires nonblank text, so no persisted event may be created yet.
    {
        HydraulicData draft_data;
        draft_data.replaceNetworkHydraulicDistribution(network, {});
        QList<QByteArray> draft_events;
        QObject::connect(&draft_data, &HydraulicData::signalLogicalEditRecorded,
                         &draft_data, [&draft_events](const QByteArray &event)
        {
            draft_events.append(event);
        });
        draft_data.setLogicalEditCaptureEnabled(true);
        EntityInspectorDock draft_dock(&draft_data);
        QList<MapAnnotation> previews;
        int clears = 0;
        QUuid selected_uuid;
        QObject::connect(&draft_dock, &EntityInspectorDock::mapAnnotationDraftUpdated,
                         &draft_dock, [&previews](const MapAnnotation &value)
        {
            previews.append(value);
        });
        QObject::connect(&draft_dock, &EntityInspectorDock::mapAnnotationDraftCleared,
                         &draft_dock, [&clears] { ++clears; });
        QObject::connect(&draft_dock, &EntityInspectorDock::mapAnnotationSelected,
                         &draft_dock, [&selected_uuid](const QUuid &id)
        {
            selected_uuid = id;
        });
        CoordinateWGS84 position;
        position.latitude_deg = 50.2;
        position.longitude_deg = 8.3;
        draft_dock.beginMapAnnotation(position);
        EntityInspectorAnnotation *draft_inspector =
            qobject_cast<EntityInspectorAnnotation *>(draft_dock.widget());
        QPlainTextEdit *draft_text = draft_inspector
            ? draft_inspector->findChild<QPlainTextEdit *>() : nullptr;
        bool has_create_button = false;
        if (draft_inspector)
            for (QPushButton *button : draft_inspector->findChildren<QPushButton *>())
                has_create_button |= button->text() == QStringLiteral("Create annotation");
        if (!expect(draft_text && !draft_text->isReadOnly() && !has_create_button &&
                    previews.size() == 1 && previews.last().text.isEmpty() &&
                    previews.last().style.visible && !previews.last().uuid.isNull() &&
                    draft_data.mapAnnotations().isEmpty() && draft_events.isEmpty(),
                    check, "right-click did not show an editable, blank unsaved draft"))
            return 28;

        // A second right click MUST retain the first empty marker, and the
        // Inspector must allow reselecting either draft without recreating it.
        const QUuid first_draft_id = previews.last().uuid;
        CoordinateWGS84 other_position = position;
        other_position.latitude_deg = 50.4;
        other_position.longitude_deg = 8.5;
        draft_dock.beginMapAnnotation(other_position);
        const QList<MapAnnotation> two_drafts = draft_dock.pendingMapAnnotations();
        if (!expect(two_drafts.size() == 2 && two_drafts[0].uuid == first_draft_id &&
                    two_drafts[0].text.isEmpty() && two_drafts[1].uuid != first_draft_id &&
                    two_drafts[1].position_wgs84.latitude_deg == 50.4 &&
                    draft_events.isEmpty(), check,
                    "second placement replaced the first annotation placeholder"))
            return 29;
        const QUuid second_draft_id = two_drafts[1].uuid;
        EntityInspectorAnnotation *second_inspector =
            qobject_cast<EntityInspectorAnnotation *>(draft_dock.widget());
        QComboBox *pending_selector = second_inspector
            ? second_inspector->findChild<QComboBox *>() : nullptr;
        if (!expect(pending_selector && pending_selector->count() >= 2 &&
                    pending_selector->itemData(0).toString() ==
                        first_draft_id.toString(QUuid::WithoutBraces), check,
                    "Inspector picker did not list both unsaved annotations"))
            return 38;
        pending_selector->setCurrentIndex(0);
        draft_inspector = qobject_cast<EntityInspectorAnnotation *>(draft_dock.widget());
        if (!expect(draft_inspector && draft_inspector->draftUuid() == first_draft_id,
                    check, "Inspector dropdown could not reopen the first placeholder"))
            return 39;
        draft_dock.selectPendingMapAnnotation(first_draft_id);
        draft_inspector = qobject_cast<EntityInspectorAnnotation *>(draft_dock.widget());
        draft_text = draft_inspector ? draft_inspector->findChild<QPlainTextEdit *>() : nullptr;
        if (!expect(draft_inspector && draft_inspector->draftUuid() == first_draft_id &&
                    draft_text && !draft_text->isReadOnly() &&
                    draft_dock.pendingMapAnnotations().size() == 2, check,
                    "reselecting a pending annotation did not restore its editable Inspector"))
            return 30;
        draft_dock.selectPendingMapAnnotation(second_draft_id);
        draft_inspector = qobject_cast<EntityInspectorAnnotation *>(draft_dock.widget());
        draft_text = draft_inspector ? draft_inspector->findChild<QPlainTextEdit *>() : nullptr;
        if (!expect(draft_inspector && draft_inspector->draftUuid() == second_draft_id &&
                    draft_text && !draft_text->isReadOnly() &&
                    draft_dock.pendingMapAnnotations().size() == 2, check,
                    "selecting the second placeholder lost the first one"))
            return 34;
        // The currently selected draft can become a real, revisioned entity;
        // other blank drafts stay on the map and remain independently usable.
        const QWidget *original_inspector = draft_inspector;
        draft_text->setPlainText(QStringLiteral("Annotation created automatically"));
        draft_inspector->flushPendingEdit();
        const QUuid persisted_uuid = selected_uuid;
        const std::optional<MapAnnotation> created = draft_data.mapAnnotation(persisted_uuid);
        if (!expect(draft_events.size() == 1 && created &&
                    created->text == QStringLiteral("Annotation created automatically") &&
                    draft_dock.widget() == original_inspector &&
                    !draft_inspector->isCreating() && !draft_text->isReadOnly() &&
                    draft_dock.pendingMapAnnotations().size() == 1 &&
                    draft_dock.pendingMapAnnotations().first().uuid == first_draft_id, check,
                    "typing did not auto-create only the selected placeholder"))
            return 31;
        draft_text->setPlainText(QStringLiteral("Edited again"));
        draft_inspector->flushPendingEdit();
        const std::optional<MapAnnotation> edited = draft_data.mapAnnotation(persisted_uuid);
        if (!expect(draft_events.size() == 2 && edited &&
                    edited->text == QStringLiteral("Edited again"), check,
                    "in-place auto-created inspector could not keep editing"))
            return 32;

        draft_data.setLogicalEditCaptureEnabled(false);
        CoordinateWGS84 third_position = position;
        third_position.latitude_deg = 50.6;
        draft_dock.beginMapAnnotation(third_position);
        draft_inspector = qobject_cast<EntityInspectorAnnotation *>(draft_dock.widget());
        draft_text = draft_inspector ? draft_inspector->findChild<QPlainTextEdit *>() : nullptr;
        if (!expect(draft_text && !draft_text->isReadOnly() &&
                    draft_dock.pendingMapAnnotations().size() == 2 &&
                    draft_events.size() == 2, check,
                    "read-only session prevented local placeholder editing"))
            return 33;
        const QUuid third_draft_id = draft_inspector->draftUuid();
        draft_text->setPlainText(QStringLiteral("Unsaved while read-only"));
        draft_inspector->flushPendingEdit();
        const std::optional<MapAnnotation> unsaved = draft_dock.pendingMapAnnotation(third_draft_id);
        if (!expect(unsaved && unsaved->text == QStringLiteral("Unsaved while read-only") &&
                    draft_events.size() == 2 && !draft_data.mapAnnotation(third_draft_id),
                    check, "local-only draft was silently saved or lost in read-only mode"))
            return 35;
        // Local drafts must obey the same Inspector visibility/background
        // checkboxes as persisted annotations, without writing to DVMDB.
        QCheckBox *draft_visible = draft_inspector->findChild<QCheckBox *>(
            QStringLiteral("mapAnnotationVisible"));
        QCheckBox *draft_background = draft_inspector->findChild<QCheckBox *>(
            QStringLiteral("mapAnnotationBackground"));
        if (!expect(draft_visible && draft_background, check,
                    "draft Inspector style controls are unavailable"))
            return 40;
        draft_visible->setChecked(false);
        draft_background->setChecked(true);
        const std::optional<MapAnnotation> hidden_draft =
            draft_dock.pendingMapAnnotation(third_draft_id);
        if (!expect(hidden_draft && !hidden_draft->style.visible &&
                    hidden_draft->style.show_background && draft_events.size() == 2,
                    check, "draft visibility/background toggles ignored or saved unexpectedly"))
            return 41;
        draft_visible->setChecked(true);
        if (!expect(draft_dock.pendingMapAnnotation(third_draft_id)->style.visible,
                    check, "showing a hidden draft again did not restore map visibility"))
            return 42;
        // A fourth placement must not erase that already-typed local draft.
        CoordinateWGS84 fourth_position = position;
        fourth_position.latitude_deg = 50.7;
        draft_dock.beginMapAnnotation(fourth_position);
        if (!expect(draft_dock.pendingMapAnnotations().size() == 3 &&
                    draft_dock.pendingMapAnnotation(third_draft_id) &&
                    draft_dock.pendingMapAnnotation(third_draft_id)->text ==
                        QStringLiteral("Unsaved while read-only"),
                    check, "placing another marker lost unsaved Inspector text"))
            return 36;
        const QUuid fourth_draft_id = draft_dock.pendingMapAnnotations().last().uuid;
        draft_dock.removePendingMapAnnotation(fourth_draft_id);
        if (!expect(draft_dock.pendingMapAnnotations().size() == 2 && clears > 0 &&
                    draft_events.size() == 2, check,
                    "discarding one blank draft also removed unrelated placeholders"))
            return 37;
    }

    HydraulicData data;
    data.replaceNetworkHydraulicDistribution(network, {}, {first, second});
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded, &data,
        [&events](const QByteArray &encoded) { events.append(encoded); });
    data.setLogicalEditCaptureEnabled(true);

    EntityInspectorDock dock(&data);
    dock.showMapAnnotation(first.uuid);
    app.processEvents();
    EntityInspectorAnnotation *inspector =
        qobject_cast<EntityInspectorAnnotation *>(dock.widget());
    QPlainTextEdit *text = inspector ? inspector->findChild<QPlainTextEdit *>() : nullptr;
    if (!expect(text != nullptr && !text->isReadOnly(), check,
                "writable annotation inspector did not open"))
        return 20;

    // Switch before the debounce timer has elapsed. Exactly one edit must be
    // recorded against the old UUID, not lost or written to the next item.
    text->setPlainText(QStringLiteral("First annotation edited"));
    dock.showMapAnnotation(second.uuid);
    const std::optional<MapAnnotation> saved_first = data.mapAnnotation(first.uuid);
    const std::optional<MapAnnotation> unchanged_second = data.mapAnnotation(second.uuid);
    if (!expect(events.size() == 1 && saved_first && unchanged_second &&
                saved_first->text == QStringLiteral("First annotation edited") &&
                unchanged_second->text == second.text,
                check, "switching selection dropped or misrouted a pending edit"))
        return 21;
    app.processEvents();
    inspector = qobject_cast<EntityInspectorAnnotation *>(dock.widget());
    text = inspector ? inspector->findChild<QPlainTextEdit *>() : nullptr;
    if (!expect(text != nullptr && text->toPlainText() == second.text,
                check, "the second inspector did not load its own annotation"))
        return 22;

    // Closing a writable session must flush before revoking write permission,
    // otherwise a rapid History/Monitor transition silently drops the edit.
    text->setPlainText(QStringLiteral("Second annotation edited"));
    data.setLogicalEditCaptureEnabled(false);
    const std::optional<MapAnnotation> saved_second = data.mapAnnotation(second.uuid);
    if (!expect(events.size() == 2 && saved_second &&
                saved_second->text == QStringLiteral("Second annotation edited") &&
                !data.logicalEditCaptureEnabled() && text->isReadOnly(), check,
                "leaving the writable draft did not flush the pending edit"))
        return 23;
    dock.clearEntity();
    app.processEvents();
    if (!expect(events.size() == 2, check,
                "delayed inspector destruction produced an extra edit"))
        return 24;

    // Network replacement must flush the old inspector BEFORE loading the new
    // network and annotation layer; it must not mutate the incoming layer.
    data.setLogicalEditCaptureEnabled(true);
    dock.showMapAnnotation(first.uuid);
    inspector = qobject_cast<EntityInspectorAnnotation *>(dock.widget());
    text = inspector ? inspector->findChild<QPlainTextEdit *>() : nullptr;
    if (!expect(text != nullptr, check, "could not reopen annotation inspector"))
        return 25;
    text->setPlainText(QStringLiteral("Before network replacement"));
    NetworkHydraulicDistribution different_network = network;
    different_network.uuid = QUuid(QStringLiteral("{019a6010-0001-7000-8000-000000000014}"));
    MapAnnotation incoming = first;
    incoming.text = QStringLiteral("From a different revision");
    data.replaceNetworkHydraulicDistribution(different_network, {}, {incoming});
    const std::optional<MapAnnotation> loaded = data.mapAnnotation(first.uuid);
    if (!expect(events.size() == 3 && !data.logicalEditCaptureEnabled() && loaded &&
                loaded->text == incoming.text,
                check, "network replacement misapplied or dropped a pending annotation edit"))
        return 26;
    dock.clearEntity();
    app.processEvents();
    if (!expect(events.size() == 3, check, "post-replacement inspector committed a stale edit"))
        return 27;

    // The two Inspector checkboxes must change the canonical style; they
    // are not merely visual controls. Rendering checks are in the bgfx suite.
    {
        HydraulicData style_data;
        style_data.replaceNetworkHydraulicDistribution(network, {}, {first});
        style_data.setLogicalEditCaptureEnabled(true);
        EntityInspectorDock style_dock(&style_data);
        style_dock.showMapAnnotation(first.uuid);
        EntityInspectorAnnotation *style_inspector =
            qobject_cast<EntityInspectorAnnotation *>(style_dock.widget());
        QCheckBox *background = style_inspector
            ? style_inspector->findChild<QCheckBox *>(QStringLiteral("mapAnnotationBackground"))
            : nullptr;
        QCheckBox *visible = style_inspector
            ? style_inspector->findChild<QCheckBox *>(QStringLiteral("mapAnnotationVisible"))
            : nullptr;
        if (!expect(background && visible && background->isEnabled() && visible->isEnabled(),
                    check, "annotation style checkboxes are missing or disabled"))
            return 28;
        background->setChecked(true);
        style_inspector->flushPendingEdit();
        std::optional<MapAnnotation> updated = style_data.mapAnnotation(first.uuid);
        if (!expect(updated && updated->style.show_background, check,
                    "Show background did not persist into the annotation MODEL"))
            return 29;
        visible->setChecked(false);
        style_inspector->flushPendingEdit();
        updated = style_data.mapAnnotation(first.uuid);
        if (!expect(updated && !updated->style.visible, check,
                    "Visible on monitor map did not persist the hidden annotation"))
            return 30;
        visible->setChecked(true);
        style_inspector->flushPendingEdit();
        updated = style_data.mapAnnotation(first.uuid);
        if (!expect(updated && updated->style.visible, check,
                    "Visible on monitor map could not restore the hidden annotation"))
            return 31;
        style_dock.clearEntity();
        app.processEvents();
    }
    // Typing with pauses publishes each pause to the live annotation (the map
    // follows), but the whole editing session is ONE History item.
    {
        HydraulicData typing_data;
        typing_data.replaceNetworkHydraulicDistribution(network, {}, {first});
        QList<QByteArray> typing_events;
        QObject::connect(&typing_data, &HydraulicData::signalLogicalEditRecorded, &typing_data,
            [&typing_events](const QByteArray &encoded) { typing_events.append(encoded); });
        typing_data.setLogicalEditCaptureEnabled(true);
        EntityInspectorDock typing_dock(&typing_data);
        typing_dock.showMapAnnotation(first.uuid);
        EntityInspectorAnnotation *typing_inspector =
            qobject_cast<EntityInspectorAnnotation *>(typing_dock.widget());
        QPlainTextEdit *typing_text = typing_inspector
            ? typing_inspector->findChild<QPlainTextEdit *>() : nullptr;
        if (!expect(typing_text != nullptr, check, "typing fixture inspector did not open"))
            return 43;
        const std::function<void()> pause = [&app]
        {
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < 500)
                app.processEvents(QEventLoop::AllEvents, 50);
        };
        for (const QString &value : {QStringLiteral("F"), QStringLiteral("Fi"),
                                     QStringLiteral("First, typed")})
        {
            typing_text->setPlainText(value);
            pause();
        }
        const std::optional<MapAnnotation> live = typing_data.mapAnnotation(first.uuid);
        if (!expect(typing_events.isEmpty() && live &&
                    live->text == QStringLiteral("First, typed") &&
                    typing_text->toPlainText() == QStringLiteral("First, typed"), check,
                    "typing with pauses recorded History items before the edit ended"))
            return 44;
        typing_inspector->flushPendingEdit();
        QString decode_error;
        const std::optional<aowis::model::ModelEditTransaction> one = typing_events.size() == 1
            ? aowis::model::ModelEditTransactionCodec::decode(typing_events.first(), &decode_error)
            : std::nullopt;
        if (!expect(one && one->edits.size() == 1 &&
                    one->edits.first().before ==
                        aowis::model::ModelCodec<MapAnnotation>::encode(first) &&
                    one->edits.first().after ==
                        aowis::model::ModelCodec<MapAnnotation>::encode(*live), check,
                    "an editing session with pauses was not ONE update from start to end"))
            return 45;
        typing_text->setPlainText(QStringLiteral("Second session"));
        pause();
        typing_dock.clearEntity();
        app.processEvents();
        if (!expect(typing_events.size() == 2, check,
                    "a second editing session was not its own History item"))
            return 46;
    }
    qInfo() << "PASS: annotation inspector flushes safely across selection,"
               " session close, and network replacement; style toggles persist;"
               " one History item per editing session";
    return 0;
}

int runEntityInspectorLanguageAcceptance(QApplication &app)
{
    GuiLanguage &language = GuiLanguage::instance();
    if (!language.select(QStringLiteral("en")))
        return 2;

    const QUuid tank_uuid(QStringLiteral("{019a6010-0001-7000-8000-000000000001}"));
    const QUuid curve_uuid(QStringLiteral("{019a6010-0001-7000-8000-000000000002}"));
    NetworkHydraulicDistribution network;
    HydraulicNodeTank tank;
    tank.uuid = tank_uuid;
    tank.id = QStringLiteral("Tank North");
    tank.diameter_m = 17.125;
    tank.volume_curve_uuid = curve_uuid;
    tank.geometry_input_type = HydraulicNodeTankGeometryInputType::Cylindrical;
    network.nodes_tanks.append(tank);
    HydraulicCurveTankVolume curve;
    curve.uuid = curve_uuid;
    curve.id = QStringLiteral("User curve name");
    network.curves_tank_volume.append(curve);

    HydraulicData model;
    model.replaceNetworkHydraulicDistribution(network, {});
    EntityInspectorTank inspector(&model, tank_uuid);
    inspector.resize(460, 720);
    inspector.show();
    inspector.setCurrentTabIndex(1);
    app.processEvents();

    QLineEdit *name = inspector.findChild<QLineEdit *>(QStringLiteral("entityInspectorName"));
    QComboBox *geometry = inspector.findChild<QComboBox *>(
        QStringLiteral("entityInspectorTankGeometryType"));
    QComboBox *curves = inspector.findChild<QComboBox *>(
        QStringLiteral("entityInspectorTankVolumeCurve"));
    QDoubleSpinBox *diameter = inspector.findChild<QDoubleSpinBox *>(
        QStringLiteral("entityInspectorTankDiameter"));
    QTabWidget *tabs = inspector.findChild<QTabWidget *>();
    GroupBoxCollapsible *geometry_group = nullptr;
    const QList<GroupBoxCollapsible *> groups = inspector.findChildren<GroupBoxCollapsible *>();
    for (GroupBoxCollapsible *candidate : groups)
    {
        if (candidate->title() == QStringLiteral("Geometry / Levels"))
        {
            geometry_group = candidate;
            break;
        }
    }
    if (!expect(name && geometry && curves && diameter && tabs && geometry_group,
                QStringLiteral("setup"), "could not locate real Tank Inspector controls"))
        return 3;
    if (!expect(geometry->count() == 4 && curves->count() == 2,
                QStringLiteral("setup"), "unexpected tank geometry or curve controls"))
        return 4;

    // Seed an in-progress text edit without emitting textEdited (which would
    // intentionally be a hydraulic model edit). A language switch must not
    // commit, discard, or move the selection in this retained editor.
    const QString draft = QStringLiteral("Tank north — work in progress");
    name->setText(draft);
    name->setFocus();
    app.processEvents();
    name->setSelection(5, 7);
    const int draft_selection_start = name->selectionStart();
    const QString draft_selection = name->selectedText();
    const QString curve_caption = curves->itemText(1);
    const QVariant selected_geometry = geometry->currentData();
    const double displayed_diameter = diameter->value();
    const QVariant selected_curve = curves->itemData(1, Qt::UserRole);
    const QLineEdit *const name_identity = name;
    const QComboBox *const geometry_identity = geometry;
    const QComboBox *const curve_identity = curves;
    const QDoubleSpinBox *const diameter_identity = diameter;
    const QTabWidget *const tabs_identity = tabs;

    int geometry_notifications = 0;
    int curve_notifications = 0;
    int node_mutations = 0;
    int draft_transactions = 0;
    QObject::connect(geometry, &QComboBox::currentIndexChanged,
                     [&geometry_notifications](int) { ++geometry_notifications; });
    QObject::connect(curves, &QComboBox::currentIndexChanged,
                     [&curve_notifications](int) { ++curve_notifications; });
    QObject::connect(&model, &HydraulicData::signalNodeChanged,
                     [&node_mutations](InfrastructureEntity, const QUuid &) { ++node_mutations; });
    QObject::connect(&model, &HydraulicData::signalLogicalEditRecorded,
                     [&draft_transactions](const QByteArray &) { ++draft_transactions; });

    const std::array<const char *, 9> sequence{{
        "en", "fr", "ar", "de", "es", "en", "ar", "fr", "en"
    }};
    for (const char *requested_code : sequence)
    {
        const QString code = QString::fromLatin1(requested_code);
        if (!expect(language.select(code), code, "cannot install real translation catalogue"))
            return 5;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LanguageChange);
        app.processEvents();

        const QString geometry_text = QCoreApplication::translate("LiveUiText", "Geometry / Levels");
        const QString uniform_area = QCoreApplication::translate("LiveUiText", "Uniform Area");
        const QString select_curve = QCoreApplication::translate("EntityInspectorTank", "[Select Curve]");
        const QString diameter_tip = QCoreApplication::translate(
            "EntityInspectorTank", "Stored cylindrical tank diameter.");
        const QString input_tab = QCoreApplication::translate("LiveUiText", "Hydraulic Input");

        // If a non-English catalogue silently falls back, the test cannot pass
        // by comparing two equally untranslated strings.
        if (!expect(code == QStringLiteral("en") ||
                    (geometry_text != QStringLiteral("Geometry / Levels") &&
                     uniform_area != QStringLiteral("Uniform Area") &&
                     diameter_tip != QStringLiteral("Stored cylindrical tank diameter.")),
                    code, "catalogue entries are missing"))
            return 6;
        if (!expect(geometry_group->title() == geometry_text &&
                    geometry->itemText(1) == uniform_area &&
                    curves->itemText(0) == select_curve &&
                    diameter->toolTip() == diameter_tip &&
                    tabs->tabToolTip(1) == input_tab,
                    code, "visible/hidden Tank Inspector captions failed live translation"))
            return 7;
        if (!expect(QApplication::layoutDirection() == (code == QStringLiteral("ar")
                     ? Qt::RightToLeft : Qt::LeftToRight), code, "RTL direction is stale"))
            return 8;
        if (!expect(inspector.findChild<QLineEdit *>(QStringLiteral("entityInspectorName")) == name_identity &&
                    inspector.findChild<QComboBox *>(QStringLiteral("entityInspectorTankGeometryType")) == geometry_identity &&
                    inspector.findChild<QComboBox *>(QStringLiteral("entityInspectorTankVolumeCurve")) == curve_identity &&
                    inspector.findChild<QDoubleSpinBox *>(QStringLiteral("entityInspectorTankDiameter")) == diameter_identity &&
                    inspector.findChild<QTabWidget *>() == tabs_identity,
                    code, "language switching recreated inspector widgets"))
            return 9;
        if (!expect(tabs->currentIndex() == 1 && name->text() == draft &&
                    name->selectionStart() == draft_selection_start &&
                    name->selectedText() == draft_selection &&
                    geometry->currentData() == selected_geometry &&
                    curves->itemData(1, Qt::UserRole) == selected_curve &&
                    curves->itemText(1) == curve_caption &&
                    diameter->value() == displayed_diameter,
                    code, "a draft, selected tab, curve identity or value was modified"))
            return 10;
        if (!expect(geometry_notifications == 0 && curve_notifications == 0 &&
                    node_mutations == 0 && draft_transactions == 0,
                    code, "translation generated hydraulic-edit signals"))
            return 11;
        const std::optional<HydraulicNodeTank> current = model.tank(tank_uuid);
        if (!expect(current.has_value() && current->id == QStringLiteral("Tank North") &&
                    current->geometry_input_type == HydraulicNodeTankGeometryInputType::Cylindrical &&
                    current->volume_curve_uuid == curve_uuid && current->diameter_m == 17.125,
                    code, "underlying MODEL data changed"))
            return 12;
    }
    inspector.hide();
    language.select(QStringLiteral("en"));
    qInfo() << "PASS: real EntityInspectorTank survives 9 live language switches without edits";
    return runAnnotationInspectorAcceptance(app);
}
