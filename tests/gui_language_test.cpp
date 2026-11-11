#include "app/history_dock.h"
#include "settings/keyboard_shortcuts_settings_widget.h"
#include "config/gui_configuration.h"
#include "entity_inspector/entity_inspector_text.h"
#include "entity_inspector/tank_inspector_presentation.h"
#include "config/gui_language.h"
#include "config/gui_preferences.h"
#include "tabs/projects_inspector_panels.h"

#include <QApplication>
#include <QListWidget>
#include <QTreeWidget>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLabel>
#include <QLocale>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

class LanguageLabel final : public QLabel
{
protected:
    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            setText(QCoreApplication::translate("TopControlBar", "Language"));
    }
};

template<typename T>
static bool matches(const QString &language, const char *field, const T &actual, const T &expected)
{
    if (actual == expected)
        return true;
    qCritical() << language << field << "expected" << expected << "actual" << actual;
    return false;
}

int main(int argc, char **argv)
{
    QTemporaryDir directory;
    if (!directory.isValid())
    {
        qCritical() << "Temporary settings directory:" << directory.errorString();
        return 1;
    }
    qputenv("XDG_DATA_HOME", directory.path().toUtf8());
    qputenv("HOME", directory.path().toUtf8());
    qputenv("APPDATA", directory.path().toUtf8());
    // Preferences live in the AOWIS-CONFIG store under the config location.
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    QApplication app(argc, argv);
    GuiLanguage &language = GuiLanguage::instance();
    QWidget window;
    QVBoxLayout layout(&window);
    LanguageLabel label;
    ProjectInspectorOverview overview;
    layout.addWidget(&label);
    layout.addWidget(&overview);
    overview.name->setText(QStringLiteral("West supply"));
    overview.description->setPlainText(QStringLiteral("Draft description"));
    QFormLayout *form = static_cast<QFormLayout *>(overview.fields->layout());
    const QLabel *description_label = static_cast<QLabel *>(form->labelForField(overview.description));
    const char *const descriptions[] = {"Description", "Description", "الوصف", "Beschreibung", "Descripción"};
    const char *const imports[] = {"Import network…", "Importer un réseau…", "استيراد شبكة…",
                                   "Netz importieren…", "Importar red…"};
    RevisionNetworkOverview network;
    layout.addWidget(&network);
    const QString title = QStringLiteral("Main %2");
    QTabWidget tabs;
    QWidget entity_page;
    QVBoxLayout entity_layout(&entity_page);
    QLabel geometry_label;
    EntityInspectorText::setCaption(&geometry_label, "Diameter");
    EntityInspectorText::setCaption(&geometry_label, "Equivalent<br>Diameter");
    entity_layout.addWidget(&geometry_label);
    QComboBox entity_choices;
    EntityInspectorText::addChoice(&entity_choices, "Open", 1);
    EntityInspectorText::addChoice(&entity_choices, "Closed", 2);
    entity_choices.addItem(QStringLiteral("Open"), QStringLiteral("user-pattern-id"));
    entity_choices.setCurrentIndex(1);
    entity_layout.addWidget(&entity_choices);
    // Pump control rows retain UUIDs and enum values separately from their
    // localized captions. Repeated translation must not emit model-edit signals.
    QComboBox pump_control_choices;
    EntityInspectorText::addChoice(&pump_control_choices, "Turn on", 4);
    EntityInspectorText::addFormattedChoice(&pump_control_choices, "Tank: %1",
                                            QStringLiteral("North tank"), 7);
    EntityInspectorText::addFormattedChoice(&pump_control_choices, "[Missing Curve] %1",
                                            QStringLiteral("123e4567"), 9);
    pump_control_choices.setCurrentIndex(1);
    entity_layout.addWidget(&pump_control_choices);
    // Retained Valve Inspector configuration controls must change presentation
    // without changing enum/UUID identities or triggering hydraulic edits.
    QComboBox valve_type_choices;
    EntityInspectorText::addChoice(&valve_type_choices,
        QT_TRANSLATE_NOOP("LiveUiText", "PRV: Pressure Reducing Valve"), 101);
    EntityInspectorText::addChoice(&valve_type_choices,
        QT_TRANSLATE_NOOP("LiveUiText", "FCV: Flow Control Valve"), 102);
    valve_type_choices.setCurrentIndex(1);
    entity_layout.addWidget(&valve_type_choices);
    QComboBox valve_curve_choices;
    const QString missing_valve_curve = QStringLiteral("123e4567-e89b-12d3-a456-426614174001");
    EntityInspectorText::addChoice(&valve_curve_choices,
        QT_TRANSLATE_NOOP("LiveUiText", "Select curve..."), QUuid());
    EntityInspectorText::addFormattedChoice(&valve_curve_choices,
        QT_TRANSLATE_NOOP("LiveUiText", "[Missing Curve] %1"), missing_valve_curve,
        missing_valve_curve);
    valve_curve_choices.setCurrentIndex(1);
    entity_layout.addWidget(&valve_curve_choices);
    // Tank curve selectors must translate their UI-owned placeholder without
    // modifying network-owned curve IDs/names or the selected UUID. In-place
    // tooltip refresh must not trigger a numeric edit or replace a typed value.
    QComboBox tank_curve_choices;
    const QUuid tank_named_curve_uuid(QStringLiteral("{123e4567-e89b-12d3-a456-426614174031}"));
    const QUuid tank_missing_curve_uuid(QStringLiteral("{123e4567-e89b-12d3-a456-426614174032}"));
    const QString tank_named_curve = QStringLiteral("North capacity curve");
    tank_curve_choices.addItem(TankInspectorPresentation::selectCurveText(), QUuid());
    tank_curve_choices.setItemData(0, TankInspectorPresentation::SelectCurve,
                                   TankInspectorPresentation::curve_text_role);
    tank_curve_choices.addItem(tank_named_curve, tank_named_curve_uuid);
    tank_curve_choices.addItem(TankInspectorPresentation::missingCurveText(tank_missing_curve_uuid),
                               tank_missing_curve_uuid);
    tank_curve_choices.setItemData(2, TankInspectorPresentation::MissingCurve,
                                   TankInspectorPresentation::curve_text_role);
    TankInspectorPresentation::bindCurveChoices(&tank_curve_choices);
    tank_curve_choices.setCurrentIndex(2);
    entity_layout.addWidget(&tank_curve_choices);
    QDoubleSpinBox tank_diameter_editor;
    tank_diameter_editor.setDecimals(3);
    tank_diameter_editor.setValue(17.125);
    TankInspectorPresentation::setSpinTooltip(&tank_diameter_editor,
        "Stored cylindrical tank diameter.");
    entity_layout.addWidget(&tank_diameter_editor);
    int tank_edit_signals = 0;
    int tank_selection_signals = 0;
    QObject::connect(&tank_diameter_editor, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        [&tank_edit_signals](double) { ++tank_edit_signals; });
    QObject::connect(&tank_curve_choices, &QComboBox::currentIndexChanged,
        [&tank_selection_signals](int) { ++tank_selection_signals; });
    int valve_selection_changes = 0;
    QObject::connect(&valve_type_choices, &QComboBox::currentIndexChanged,
        [&valve_selection_changes](int) { ++valve_selection_changes; });
    QObject::connect(&valve_curve_choices, &QComboBox::currentIndexChanged,
        [&valve_selection_changes](int) { ++valve_selection_changes; });
    int pump_selection_changes = 0;
    QObject::connect(&pump_control_choices, &QComboBox::currentIndexChanged,
                     [&pump_selection_changes](int) { ++pump_selection_changes; });
    int entity_selection_changes = 0;
    QObject::connect(&entity_choices, &QComboBox::currentIndexChanged,
                     [&entity_selection_changes](int) { ++entity_selection_changes; });
    RevisionInspectorOverview details;
    RevisionInspectorBranch branch_page(details.name);
    tabs.addTab(&branch_page, QStringLiteral("branch"));
    tabs.addTab(&details, QStringLiteral("details"));
    tabs.addTab(&entity_page, QStringLiteral("entity"));
    tabs.setCurrentWidget(&details);
    KeyboardShortcutsSettingsWidget shortcuts;
    tabs.addTab(&shortcuts, QStringLiteral("shortcuts"));
    DatabaseInspectorOverview database;
    tabs.addTab(&database, QStringLiteral("database"));
    database.name->setText(QStringLiteral("Personal DVMDB"));
    QLineEdit *search = shortcuts.findChild<QLineEdit *>();
    QTreeWidget *commands = shortcuts.findChild<QTreeWidget *>();
    if (search == nullptr || commands == nullptr)
        return 10;
    search->setText(QStringLiteral("zoom"));
    // Exercise the retained inspector presentation refresh contract with the
    // real compiled catalogues, not just a synthetic translator. These views
    // keep their identity and stored data as language changes.
    QLabel inspector_simulation_status;
    LiveUiText::bind(&inspector_simulation_status, [&inspector_simulation_status] {
        inspector_simulation_status.setText(QCoreApplication::translate(
            "EntityInspectorPresentation", "No valid simulation results available."));
    });
    layout.addWidget(&inspector_simulation_status);
    QLabel inspector_demand_summary;
    const int inspector_demand_count = 2;
    const QString inspector_base_demand = QStringLiteral("12.500 m³/h");
    LiveUiText::bind(&inspector_demand_summary, [&inspector_demand_summary, &inspector_demand_count,
                                                &inspector_base_demand] {
        inspector_demand_summary.setText(QCoreApplication::translate(
            "EntityInspectorPresentation", "%n demand categories\nTotal base demand: %1",
            nullptr, inspector_demand_count).arg(inspector_base_demand));
    });
    layout.addWidget(&inspector_demand_summary);
    // Presentation-only terrain result and Source Trace labels share the real
    // EntityInspectorPresentation catalogue. Changing languages must not
    // replace retained values or emit editing signals.
    QLabel inspector_terrain_result;
    const QString inspector_terrain_value = QStringLiteral("143.250");
    LiveUiText::bind(&inspector_terrain_result, [&inspector_terrain_result, &inspector_terrain_value] {
        inspector_terrain_result.setText(QCoreApplication::translate(
            "EntityInspectorPresentation", "Retrieved %1 m%2.")
            .arg(inspector_terrain_value, QString()));
    });
    layout.addWidget(&inspector_terrain_result);
    QLabel inspector_source_trace;
    const QString inspector_source_uuid = QStringLiteral("123e4567-e89b-12d3-a456-426614174000");
    LiveUiText::bind(&inspector_source_trace, [&inspector_source_trace, &inspector_source_uuid] {
        inspector_source_trace.setText(QCoreApplication::translate(
            "EntityInspectorPresentation", "Unknown node (%1)").arg(inspector_source_uuid));
    });
    layout.addWidget(&inspector_source_trace);
    HistoryDock history;
    layout.addWidget(&history);
    HistoryDockContext context;
    context.project = QStringLiteral("Water");
    context.branch = QStringLiteral("main");
    HistoryDockEntry commit;
    commit.title = QStringLiteral("Store tank elevation");
    commit.identity = QStringLiteral("test-commit");
    context.commits.append(commit);
    history.setContext(context);
    int requests = 0;
    QObject::connect(&history, &HistoryDock::branchHistoryPageRequested, [&requests] { ++requests; });
    QObject::connect(&history, &HistoryDock::archivedActionPageRequested, [&requests] { ++requests; });
    QListWidget *history_rows = history.findChild<QListWidget *>(QStringLiteral("historyDockTimeline"));
    if (history_rows == nullptr)
        return 10;
    layout.addWidget(&tabs);
    window.show();
    overview.name->setFocus();
    app.processEvents();
    // Finish queued focus handling from showing the window before seeding the selection.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    overview.name->setSelection(5, 6);
    const QString draft = QStringLiteral("Check north pump\nDraft changes");
    details.commit_message->setPlainText(draft);
    QTextCursor cursor = details.commit_message->textCursor();
    cursor.setPosition(6);
    cursor.setPosition(11, QTextCursor::KeepAnchor);
    details.commit_message->setTextCursor(cursor);
    branch_page.name->setText(QStringLiteral("main / north"));
    const char *const commit_labels[] = {"Commit message", "Message de commit", "رسالة تسجيل المراجعة",
                                        "Commit-Nachricht", "Mensaje de commit"};
    QFormLayout *details_form = static_cast<QFormLayout *>(details.fields->layout());
    const QLabel *commit_label = static_cast<QLabel *>(details_form->labelForField(details.commit_message));
    int draft_changes = 0;
    QObject::connect(details.commit_message, &QPlainTextEdit::textChanged, [&draft_changes] { ++draft_changes; });
    const QLocale numeric_locale;
    const char *const names[] = {"Language", "Langue", "اللغة", "Sprache", "Idioma"};
    const char *const cancel[] = {"Cancel", "Annuler", "إلغاء", "Abbrechen", "Cancelar"};
    const char *const draft_close[] = {"Close", "Fermer", "إغلاق", "Schließen", "Cerrar"};
    const char *const stats_titles[] = {"Simulation Results", "Résultats de simulation", "نتائج المحاكاة",
                                       "Simulationsergebnisse", "Resultados de simulación"};
    const char *const edit_actions[] = {"Stop private editing", "Terminer la modification privée", "إنهاء التحرير الخاص",
                                        "Private Bearbeitung beenden", "Detener edición privada"};
    const char *const slider_off[] = {"Off", "Désactivé", "إيقاف", "Aus", "Desactivado"};
    const char *const open_network[] = {"Open network", "Ouvrir le réseau", "فتح الشبكة",
                                        "Netz öffnen", "Abrir red"};
    const char *const map_starting[] = {"Starting map renderer…",
        "Démarrage du moteur de rendu cartographique…", "جارٍ تشغيل عارض الخريطة…",
        "Kartenrenderer wird gestartet…", "Iniciando el renderizador del mapa…"};
    const char *const diagnostic_details[] = {"<p><b>Details</b><ul>",
        "<p><b>Détails</b><ul>", "<p><b>التفاصيل</b><ul>",
        "<p><b>Details</b><ul>", "<p><b>Detalles</b><ul>"};
    const char *const working_copy_progress[] = {
        "Reading personal DVMDB drafts…",
        "Lecture des brouillons du DVMDB personnel…",
        "جارٍ قراءة المسودات من قاعدة DVMDB الشخصية…",
        "Entwürfe der persönlichen DVMDB werden gelesen…",
        "Leyendo los borradores de la DVMDB personal…"
    };
    const char *const project_commit_actions[] = {
        "Commit HEAD", "Valider HEAD", "اعتماد HEAD", "HEAD committen", "Confirmar HEAD"
    };
    const char *const project_preview_names[] = {
        "Sample North Network", "Réseau d’exemple Nord", "شبكة الشمال النموذجية",
        "Beispielnetz Nord", "Red Norte de ejemplo"
    };
    const char *const project_delete_actions[] = {
        "Delete entire branch", "Supprimer toute la branche", "حذف الفرع بالكامل",
        "Gesamten Branch löschen", "Eliminar toda la rama"
    };
    const char *const project_graph_empty[] = {
        "No revisions to display\nSelect a project or create a branch.",
        "Aucune révision à afficher\nSélectionnez un projet ou créez une branche.",
        "لا توجد مراجعات لعرضها\nاختر مشروعًا أو أنشئ فرعًا.",
        "Keine Revisionen zur Anzeige\nWählen Sie ein Projekt oder erstellen Sie einen Branch.",
        "No hay revisiones para mostrar\nSeleccione un proyecto o cree una rama."
    };
    for (std::size_t step = 1; step <= GuiLanguages.size(); ++step)
    {
        const std::size_t index = step % GuiLanguages.size();
        const QString code = QString::fromLatin1(GuiLanguages[index].code);
        if (!matches(code, "select language", language.select(code), true))
            return 2;
        app.processEvents();
        // The toolbar must distinguish a hydraulic model from a network connection,
        // including after switching the UI language without restarting the app.
        const char *const no_hydraulic_network[] = {
            "No hydraulic network open", "Aucun réseau hydraulique ouvert",
            "لا توجد شبكة هيدروليكية مفتوحة", "Kein hydraulisches Netz geöffnet",
            "Ninguna red hidráulica abierta"
        };
        if (!matches(code, "empty hydraulic network status",
                     QCoreApplication::translate("LiveUiText", "No hydraulic network open"),
                     QString::fromUtf8(no_hydraulic_network[index])))
            return 23;
        const char *const unit_menu_titles[] = {
            "Manage Units…", "Gérer les unités…", "إدارة الوحدات…",
            "Einheiten verwalten…", "Administrar unidades…"
        };
        const char *const builtin_profile_labels[] = {
            "LPS — liters per second", "LPS — litres par seconde",
            "LPS — لترات في الثانية", "LPS — Liter pro Sekunde",
            "LPS — litros por segundo"
        };
        if (!matches(code, "unit popup catalogue",
                     QCoreApplication::translate("UnitProfileMenu", "Manage Units…"),
                     QString::fromUtf8(unit_menu_titles[index]))
            || !matches(code, "builtin unit profile catalogue",
                        QCoreApplication::translate("UnitProfileMenu", "LPS — liters per second"),
                        QString::fromUtf8(builtin_profile_labels[index])))
            return 22;
        if (!matches(code, "toolbar label", label.text(), QString::fromUtf8(names[index]))
            || !matches(code, "HUD cancel", QCoreApplication::translate("MapBgfxHud", "Cancel"), QString::fromUtf8(cancel[index]))
            || !matches(code, "live working-copy dialog catalogue",
                        QCoreApplication::translate("PersonalWorkingCopyDialog", "Close"),
                        QString::fromUtf8(draft_close[index]))
            || !matches(code, "live simulation results catalogue",
                        QCoreApplication::translate("SimulationStatisticsDialog", "Simulation Results"),
                        QString::fromUtf8(stats_titles[index]))
            || !matches(code, "live MainWindow catalogue",
                        QCoreApplication::translate("MainWindow", "Stop private editing"),
                        QString::fromUtf8(edit_actions[index]))
            || !matches(code, "live map slider catalogue",
                        QCoreApplication::translate("MapMonitorMenuWidget", "Off"),
                        QString::fromUtf8(slider_off[index]))
            || !matches(code, "private-network confirmation catalogue",
                        QCoreApplication::translate("MainWindow", "Open network"),
                        QString::fromUtf8(open_network[index]))
            || !matches(code, "retained renderer startup message",
                        QCoreApplication::translate("MapMonitorContainer", "Starting map renderer…"),
                        QString::fromUtf8(map_starting[index]))
            || !matches(code, "diagnostics rich-text catalogue",
                        QCoreApplication::translate("SimulationDiagnosticsWidget",
                                                    "<p><b>Details</b><ul>"),
                        QString::fromUtf8(diagnostic_details[index]))
            || !matches(code, "active language", language.code(), code)
            || !matches(code, "numeric locale preserved", numeric_locale == QLocale(), true)
            || !matches(code, "layout direction", QApplication::layoutDirection(), QLocale(code).textDirection()))
            return 3;
        if (!matches(code, "working-copy progress translation",
                QCoreApplication::translate("PersonalWorkingCopyDialog", "Reading personal DVMDB drafts…"),
                QString::fromUtf8(working_copy_progress[index]))
            || !matches(code, "graph empty-state translation",
                QCoreApplication::translate("ProjectRevisionGraph",
                    "No revisions to display\nSelect a project or create a branch."),
                QString::fromUtf8(project_graph_empty[index])))
            return 19;
        if (!matches(code, "project HEAD commit catalogue",
                     QCoreApplication::translate("ProjectsWidget", "Commit HEAD"),
                     QString::fromUtf8(project_commit_actions[index]))
            || !matches(code, "preview project language",
                        QCoreApplication::translate("ProjectsWidget", "Sample North Network"),
                        QString::fromUtf8(project_preview_names[index]))
            || !matches(code, "destructive branch confirmation catalogue",
                        QCoreApplication::translate("ProjectsWidget", "Delete entire branch"),
                        QString::fromUtf8(project_delete_actions[index])))
            return 20;
        const QString pending_edits = QCoreApplication::translate("ProjectsWidget",
            "%n edit(s) of this draft are queued on this device but not yet saved to DVMDB. "
            "Open the network for editing so they are saved, stop editing, then commit.",
            nullptr, 2);
        if (!matches(code, "project pending edit plural", pending_edits.contains(QStringLiteral("2")), true)
            || !matches(code, "project plural unresolved", pending_edits.contains(QStringLiteral("%n")), false))
            return 20;
        const QString branch_warning = QCoreApplication::translate("ProjectsWidget",
            "Could not configure this DVMDB as private (HTTP %1: %2). The server may need "
            "the updated DVMDB build or may already have a conflicting repository role.")
                .arg(QStringLiteral("409"), QStringLiteral("conflict"));
        if (!matches(code, "project DVMDB error placeholders",
                     branch_warning.contains(QStringLiteral("409"))
                     && branch_warning.contains(QStringLiteral("conflict")), true))
            return 20;
        const QString hidden_revisions = QCoreApplication::translate("ProjectRevisionGraph",
            "+ %n hidden revision(s)\nClick to unfold", nullptr, 2);
        if (!matches(code, "graph hidden revision count", hidden_revisions.contains(QStringLiteral("2")), true)
            || !matches(code, "graph hidden revision placeholder", hidden_revisions.contains(QStringLiteral("%n")), false))
            return 19;
        // Real .qm catalogue coverage complements the synthetic translator
        // test in aowis-hydraulic-libraries-widget. A fresh or already open
        // library page must have localized collection names in all languages.
        const char *const collection_names[] = {"Pipe materials", "Matériaux des conduites",
            "مواد الأنابيب", "Rohrmaterialien", "Materiales de tuberías"};
        const char *const library_dialog_titles[] = {"Add hydraulic library item",
            "Ajouter un élément de bibliothèque hydraulique",
            "إضافة عنصر إلى المكتبة الهيدروليكية",
            "Hydraulisches Bibliothekselement hinzufügen",
            "Añadir elemento a la biblioteca hidráulica"};
        if (!matches(code, "library collection catalogue",
                     QCoreApplication::translate("HydraulicLibrariesWidget", "Pipe materials"),
                     QString::fromUtf8(collection_names[index]))
            || !matches(code, "library modal title catalogue",
                        QCoreApplication::translate("HydraulicLibrariesWidget", "Add hydraulic library item"),
                        QString::fromUtf8(library_dialog_titles[index])))
            return 12;
        const char *const valve_prv_names[] = {
            "PRV: Pressure Reducing Valve", "PRV : réducteur de pression",
            "PRV: صمام تخفيض الضغط", "PRV: Druckminderventil",
            "PRV: Válvula reductora de presión"
        };
        const char *const valve_curve_prompts[] = {
            "Select curve...", "Sélectionner une courbe...", "اختر منحنى...",
            "Kurve auswählen...", "Seleccionar curva..."
        };
        const QString missing_curve_label = QCoreApplication::translate(
            "LiveUiText", "[Missing Curve] %1").arg(missing_valve_curve);
        if (!matches(code, "live valve type choice", valve_type_choices.itemText(0),
                     QString::fromUtf8(valve_prv_names[index]))
            || !matches(code, "live valve curve prompt", valve_curve_choices.itemText(0),
                        QString::fromUtf8(valve_curve_prompts[index]))
            || !matches(code, "live missing-curve caption", valve_curve_choices.itemText(1),
                        missing_curve_label)
            || !matches(code, "valve type selection", valve_type_choices.currentData().toInt(), 102)
            || !matches(code, "valve curve UUID", valve_curve_choices.currentData().toString(),
                        missing_valve_curve)
            || !matches(code, "no valve selection events", valve_selection_changes, 0))
            return 16;
        const char *const control_categories[] = {"Rule controls", "Règles de commande",
            "قواعد التحكم", "Steuerregeln", "Reglas de control"};
        const char *const control_dialog_titles[] = {"Hydraulic rule control",
            "Règle de commande hydraulique", "قاعدة التحكم الهيدروليكي",
            "Hydraulische Steuerregel", "Regla de control hidráulico"};
        const char *const map_label_titles[] = {"Hydraulic map label",
            "Étiquette de carte hydraulique", "تسمية الخريطة الهيدروليكية",
            "Hydraulische Kartenbeschriftung", "Etiqueta del mapa hidráulico"};
        if (!matches(code, "controls category catalogue",
                     QCoreApplication::translate("HydraulicControlsWidget", "Rule controls"),
                     QString::fromUtf8(control_categories[index]))
            || !matches(code, "controls modal catalogue",
                        QCoreApplication::translate("HydraulicControlsWidget", "Hydraulic rule control"),
                        QString::fromUtf8(control_dialog_titles[index]))
            || !matches(code, "map label modal catalogue",
                        QCoreApplication::translate("HydraulicControlsWidget", "Hydraulic map label"),
                        QString::fromUtf8(map_label_titles[index])))
            return 13;
        const char *const inspector_simulation_messages[] = {
            "No valid simulation results available.",
            "Aucun résultat de simulation valide disponible.",
            "لا تتوفر نتائج محاكاة صالحة.",
            "Keine gültigen Simulationsergebnisse verfügbar.",
            "No hay resultados de simulación válidos disponibles."
        };
        const QString translated_demand_summary = QCoreApplication::translate(
            "EntityInspectorPresentation", "%n demand categories\nTotal base demand: %1",
            nullptr, inspector_demand_count).arg(inspector_base_demand);
        const char *const terrain_results[] = {
            "Retrieved 143.250 m.", "Altitude récupérée : 143.250 m.",
            "تم استرجاع الارتفاع: 143.250 م.", "Geländehöhe abgerufen: 143.250 m.",
            "Elevación obtenida: 143.250 m."
        };
        const char *const unknown_nodes[] = {
            "Unknown node (%1)", "Nœud inconnu (%1)",
            "عقدة غير معروفة (%1)", "Unbekannter Knoten (%1)",
            "Nodo desconocido (%1)"
        };
        const char *const trace_dialogs[] = {
            "Change Source Trace Origin?", "Changer l’origine du traçage de source ?",
            "تغيير أصل تتبّع المصدر؟", "Ursprung der Quellverfolgung ändern?",
            "¿Cambiar el origen del trazado de fuente?"
        };
        if (!matches(code, "live inspector terrain result", inspector_terrain_result.text(),
                     QString::fromUtf8(terrain_results[index]))
            || !matches(code, "live inspector source trace", inspector_source_trace.text(),
                        QString::fromUtf8(unknown_nodes[index]).arg(inspector_source_uuid))
            || !matches(code, "source trace dialog catalogue",
                        QCoreApplication::translate("EntityInspectorPresentation", "Change Source Trace Origin?"),
                        QString::fromUtf8(trace_dialogs[index]))
            || !matches(code, "terrain retained numeric value", inspector_terrain_result.text().contains(inspector_terrain_value), true)
            || !matches(code, "trace retained UUID", inspector_source_trace.text().contains(inspector_source_uuid), true))
            return 15;
        if (!matches(code, "live inspector simulation status", inspector_simulation_status.text(),
                     QString::fromUtf8(inspector_simulation_messages[index]))
            || !matches(code, "live inspector demand summary", inspector_demand_summary.text(),
                        translated_demand_summary)
            || !matches(code, "inspector summary numeric data", inspector_demand_summary.text().contains(inspector_base_demand), true)
            || !matches(code, "inspector plural substitution", inspector_demand_summary.text().contains(QStringLiteral("%n")), false))
            return 14;
        if (!matches(code, "description label", description_label->text(), QString::fromUtf8(descriptions[index]))
            || !matches(code, "import action", overview.import_network->text(), QString::fromUtf8(imports[index]))
            || !matches(code, "project name selection", overview.name->selectedText(), QStringLiteral("supply"))
            || !matches(code, "project description", overview.description->toPlainText(), QStringLiteral("Draft description")))
            return 7;
        const QTextCursor current_cursor = details.commit_message->textCursor();
        if (!matches(code, "commit label", commit_label->text(), QString::fromUtf8(commit_labels[index]))
            || !matches(code, "commit draft", details.commit_message->toPlainText(), draft)
            || !matches(code, "draft change signals", draft_changes, 0)
            || !matches(code, "draft selection anchor", current_cursor.anchor(), 6)
            || !matches(code, "draft cursor", current_cursor.position(), 11)
            || !matches(code, "details tab selected", tabs.currentWidget() == &details, true)
            || !matches(code, "branch name", branch_page.name->text(), QStringLiteral("main / north")))
            return 8;
        if (step == 1)
        {
            if (!matches(code, "loading summary", network.network_status->text(), QStringLiteral("Vérification du réseau…")))
                return 9;
            QTimer::singleShot(0, &network, [&network, title] {
                network.showCounts(QJsonObject{{QStringLiteral("pipes"), 3}}, 3);
                network.setStatus({"Network: %1", {title}},
                                  {"\nSaved private working copy · generation %1", {QStringLiteral("42")}});
            });
            app.processEvents();
        }
        const QString summary = QCoreApplication::translate("ProjectsWidget", "Network: %1").arg(title)
            + QCoreApplication::translate("ProjectsWidget", "\nSaved private working copy · generation %1")
                .arg(QStringLiteral("42"));
        const QString total = QCoreApplication::translate("ProjectsWidget", "Total hydraulic entities: %1")
            .arg(numeric_locale.toString(3));
        if (!matches(code, "saved network summary", network.network_status->text(), summary)
            || !matches(code, "pipe count", network.counts.at(1)->text(), numeric_locale.toString(3))
            || !matches(code, "network total", network.other_counts->text(), total))
            return 9;
        KeyboardShortcutsSettingsWidget fresh_shortcuts;
        QTreeWidget *fresh_commands = fresh_shortcuts.findChild<QTreeWidget *>();
        DatabaseInspectorOverview fresh_database;
        HistoryDock fresh_history;
        fresh_history.setContext(context);
        QListWidget *fresh_rows = fresh_history.findChild<QListWidget *>(QStringLiteral("historyDockTimeline"));
        const char *const history_titles[] = {"History", "Historique", "السجل", "Verlauf", "Historial"};
        if (!matches(code, "live History title", history.windowTitle(), QString::fromUtf8(history_titles[index]))
            || !matches(code, "History requests on language switch", requests, 0)
            || !matches(code, "History row count", history_rows->count(), fresh_rows->count())
            || !matches(code, "hidden shortcut header", commands->headerItem()->text(1), fresh_commands->headerItem()->text(1))
            || !matches(code, "typed shortcut filter", search->text(), QStringLiteral("zoom"))
            || !matches(code, "hidden database action", database.create_project->text(), fresh_database.create_project->text())
            || !matches(code, "database name", database.name->text(), QStringLiteral("Personal DVMDB")))
            return 10;
        for (int row = 0; row < history_rows->count(); ++row)
            if (!matches(code, "live History row", history_rows->item(row)->text(), fresh_rows->item(row)->text()))
                return 10;
        const QString translated_tank = QCoreApplication::translate("LiveUiText", "Tank: %1")
            .arg(QStringLiteral("North tank"));
        const QString translated_missing = QCoreApplication::translate("LiveUiText", "[Missing Curve] %1")
            .arg(QStringLiteral("123e4567"));
        const QString translated_rules = QCoreApplication::translate(
            "EntityInspectorPump",
            "%n rule-based control also targets this pump. The controls above are the pump's simple EPANET controls.",
            nullptr, 3);
        if (!matches(code, "pump formatted node", pump_control_choices.itemText(1), translated_tank)
            || !matches(code, "pump missing curve", pump_control_choices.itemText(2), translated_missing)
            || !matches(code, "pump enum identity", pump_control_choices.itemData(1).toInt(), 7)
            || !matches(code, "pump selection retained", pump_control_choices.currentIndex(), 1)
            || !matches(code, "pump selection notifications", pump_selection_changes, 0)
            || !matches(code, "pump formatted raw label", pump_control_choices.itemText(1).contains(QStringLiteral("North tank")), true)
            || !matches(code, "pump plural count", translated_rules.contains(QStringLiteral("%n")), false)
            || !matches(code, "pump plural translated", translated_rules.contains(QStringLiteral("EPANET")), true))
            return 16;
        const char *const tank_select_labels[] = {
            "[Select Curve]", "[Sélectionner une courbe]", "[اختيار منحنى]",
            "[Kurve auswählen]", "[Seleccionar curva]"
        };
        const char *const tank_stored_diameter_tips[] = {
            "Stored cylindrical tank diameter.", "Diamètre du réservoir cylindrique enregistré.",
            "القطر المخزَّن للخزان الأسطواني.",
            "Gespeicherter Durchmesser des zylindrischen Behälters.",
            "Diámetro almacenado del tanque cilíndrico."
        };
        const char *const tank_missing_labels[] = {
            "[Missing Curve] %1", "[Courbe manquante] %1", "[منحنى مفقود] %1",
            "[Fehlende Kurve] %1", "[Curva ausente] %1"
        };
        const QString expected_missing_tank_curve = QString::fromUtf8(tank_missing_labels[index])
            .arg(tank_missing_curve_uuid.toString(QUuid::WithoutBraces));
        if (!matches(code, "live tank select curve", tank_curve_choices.itemText(0),
                     QString::fromUtf8(tank_select_labels[index]))
            || !matches(code, "live tank missing curve", tank_curve_choices.itemText(2),
                        expected_missing_tank_curve)
            || !matches(code, "tank user curve name retained", tank_curve_choices.itemText(1),
                        tank_named_curve)
            || !matches(code, "tank selected curve UUID retained", tank_curve_choices.currentData().toUuid(),
                        tank_missing_curve_uuid)
            || !matches(code, "tank user curve UUID retained", tank_curve_choices.itemData(1).toUuid(),
                        tank_named_curve_uuid)
            || !matches(code, "tank selection unchanged", tank_curve_choices.currentIndex(), 2)
            || !matches(code, "tank selection signals", tank_selection_signals, 0)
            || !matches(code, "tank numeric editor unchanged", tank_diameter_editor.value(), 17.125)
            || !matches(code, "tank edit signals", tank_edit_signals, 0)
            || !matches(code, "tank editable tooltip", tank_diameter_editor.toolTip(),
                        QString::fromUtf8(tank_stored_diameter_tips[index])))
            return 18;
        if (index == 2)
        {
            // Switching geometry modes changes which tooltip key is retained.
            // Updating the key while Arabic is active must not commit a value.
            TankInspectorPresentation::setSpinTooltip(&tank_diameter_editor,
                "Interpolated from the selected volume curve at the maximum level.");
            if (!matches(code, "tank derived tooltip after mode change",
                    tank_diameter_editor.toolTip(), QCoreApplication::translate(
                        "EntityInspectorTank",
                        "Interpolated from the selected volume curve at the maximum level.")))
                return 18;
            TankInspectorPresentation::setSpinTooltip(&tank_diameter_editor,
                "Stored cylindrical tank diameter.");
            if (!matches(code, "tank editable tooltip restored",
                    tank_diameter_editor.toolTip(),
                    QString::fromUtf8(tank_stored_diameter_tips[index])))
                return 18;
        }
        if (index > 0 && (QCoreApplication::translate("EntityInspectorTank",
                "Interpolated from the selected volume curve at the maximum level.")
                == QStringLiteral("Interpolated from the selected volume curve at the maximum level.")
            || QCoreApplication::translate("EntityInspectorTank",
                "Fraction of tank volume assigned to the inlet/outlet compartment for the two-compartment model.")
                == QStringLiteral("Fraction of tank volume assigned to the inlet/outlet compartment for the two-compartment model.")))
        {
            qCritical() << code << "tank translation catalogue entry missing";
            return 18;
        }
        if (index > 0 && (translated_tank == QStringLiteral("Tank: North tank")
            || QCoreApplication::translate("LiveUiText", "Pipe materials are defined by the network material library.")
                == QStringLiteral("Pipe materials are defined by the network material library.")))
        {
            qCritical() << code << "pump/pipe catalogue entry missing";
            return 16;
        }
        const char *const closed[] = {"Closed", "Fermé", "مغلق", "Geschlossen", "Cerrado"};
        if (!matches(code, "hidden inspector enum", entity_choices.currentText(), QString::fromUtf8(closed[index]))
            || !matches(code, "inspector enum identity", entity_choices.currentData().toInt(), 2)
            || !matches(code, "inspector selection signals", entity_selection_changes, 0)
            || !matches(code, "literal pattern name", entity_choices.itemText(2), QStringLiteral("Open"))
            || !matches(code, "active geometry caption", geometry_label.text(),
                        LiveUiText::translate("Equivalent<br>Diameter")))
            return 11;
        // Persisted in AOWIS-CONFIG, not only in the cache.
        GuiPreferences::reload();
        if (!matches(code, "persisted language",
                     GuiPreferences::value(QStringLiteral("interface"), QStringLiteral("language")).toString(), code))
            return 4;
    }
    if (!GuiPreferences::update(QStringLiteral("interface"),
                                QJsonObject{{QStringLiteral("language"), QStringLiteral("de")}}))
        return 5;
    language.restore();
    if (!matches(QStringLiteral("restore"), "stored language", language.code(), QStringLiteral("de"))
        || !matches(QStringLiteral("xx"), "accepted unknown language", language.select(QStringLiteral("xx")), false))
        return 5;
    if (!GuiPreferences::update(QStringLiteral("interface"),
                                QJsonObject{{QStringLiteral("language"), QStringLiteral("invalid")}}))
        return 6;
    language.restore();
    return matches(QStringLiteral("restore"), "invalid settings fallback", language.code(), QStringLiteral("en")) ? 0 : 6;
}
