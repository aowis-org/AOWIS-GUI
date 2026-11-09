// Acceptance test running inside the actual GUI executable. Linking the normal
// application target gives this test the same inspector, model and Qt resources
// as a user session without launching MainWindow or initializing bgfx.
#include "config/gui_language.h"
#include "entity_inspector/entity_inspector_tank.h"
#include "network/hydraulic_data.h"
#include "widgets/group_box_collapsible.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLineEdit>
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

int runEntityInspectorLanguageAcceptance(QApplication &app)
{
    GuiLanguage &language = GuiLanguage::instance();
    if (!language.select(QStringLiteral("en")))
        return 2;

    const QUuid tank_uuid(QStringLiteral("{019a6010-0001-7000-8000-000000000001}"));
    const QUuid curve_uuid(QStringLiteral("{019a6010-0001-7000-8000-000000000002}"));
    NetworkHydraulic network;
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
    model.replaceNetworkHydraulic(network, {});
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
    return 0;
}
