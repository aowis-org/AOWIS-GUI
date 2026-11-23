// Acceptance test running inside the actual GUI executable. Linking the normal
// application target gives this test the same inspector, model and Qt resources
// as a user session without launching MainWindow or initializing bgfx.
#include "config/gui_language.h"
#include "entity_inspector/entity_inspector_tank.h"
#include "entity_inspector/entity_inspector_dock.h"
#include "network/hydraulic_data.h"
#include "widgets/group_box_collapsible.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>

#include <array>

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

        // Abandon a blank draft: it must disappear without an invalid empty
        // annotation being inserted into revision History.
        draft_dock.clearEntity();
        if (!expect(clears > 0 && draft_events.isEmpty() &&
                    draft_data.mapAnnotations().isEmpty(), check,
                    "abandoning blank draft wrote an annotation"))
            return 29;
        draft_dock.beginMapAnnotation(position);
        draft_inspector = qobject_cast<EntityInspectorAnnotation *>(draft_dock.widget());
        draft_text = draft_inspector ? draft_inspector->findChild<QPlainTextEdit *>() : nullptr;
        if (!expect(draft_text && !draft_text->isReadOnly(), check,
                    "reopening blank draft did not restore the text editor"))
            return 30;
        const QWidget *original_inspector = draft_inspector;
        draft_text->setPlainText(QStringLiteral("Annotation created automatically"));
        draft_inspector->flushPendingEdit();
        const QUuid persisted_uuid = selected_uuid;
        const std::optional<MapAnnotation> created = draft_data.mapAnnotation(persisted_uuid);
        if (!expect(draft_events.size() == 1 && created &&
                    created->text == QStringLiteral("Annotation created automatically") &&
                    draft_dock.widget() == original_inspector &&
                    !draft_inspector->isCreating() && !draft_text->isReadOnly(), check,
                    "typing did not auto-create a persisted annotation in-place"))
            return 31;
        draft_text->setPlainText(QStringLiteral("Edited again"));
        draft_inspector->flushPendingEdit();
        const std::optional<MapAnnotation> edited = draft_data.mapAnnotation(persisted_uuid);
        if (!expect(draft_events.size() == 2 && edited &&
                    edited->text == QStringLiteral("Edited again"), check,
                    "in-place auto-created inspector could not keep editing"))
            return 32;

        draft_data.setLogicalEditCaptureEnabled(false);
        draft_dock.beginMapAnnotation(position);
        draft_inspector = qobject_cast<EntityInspectorAnnotation *>(draft_dock.widget());
        draft_text = draft_inspector ? draft_inspector->findChild<QPlainTextEdit *>() : nullptr;
        const int preview_count_before = previews.size();
        if (draft_inspector)
            draft_inspector->publishDraftPreview();
        if (!expect(draft_text && draft_text->isReadOnly() &&
                    preview_count_before == previews.size() &&
                    draft_events.size() == 2, check,
                    "read-only History position exposed a fake writable draft"))
            return 33;
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

    qInfo() << "PASS: annotation inspector flushes safely across selection,"
               " session close, and network replacement";
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
