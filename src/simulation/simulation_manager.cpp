#include "simulation/simulation_manager.h"

#include "simulation/simulation_statistics_dialog.h"
#include "simulation/epanet_network_file_format.h"
#include "import/epanet_js_project_importer.h"
#include "import/epanet_js_unit_profile.h"
#include "config/unit_profile_manager.h"

#include <aowis/epanet/epanet_runner.h>
#include <aowis/epanet/epanet_result_import.h>
#include <aowis/epanet/epanet_result_run.h>
#include <aowis/epanet/epanet_run_request.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QByteArray>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSet>
#include <QTimer>
#include <QTemporaryDir>

#include <memory>
#include <utility>

#ifndef Q_OS_WASM
#include <QSaveFile>
#else
#include <emscripten.h>

EM_JS(void, aowisOpenNetworkProjectFile, (),
{
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".inp,.ejsdb";
    input.style.display = "none";

    const cleanup = () => {
        input.remove();
    };

    input.addEventListener("change", async () => {
        const file = input.files && input.files.length > 0 ? input.files[0] : null;
        if (!file)
        {
            cleanup();
            return;
        }

        try
        {
            const bytes = new Uint8Array(await file.arrayBuffer());
            const file_name_utf8 = stringToNewUTF8(file.name);
            const contents = _malloc(bytes.length);
            if (bytes.length > 0)
                HEAPU8.set(bytes, contents);
            _aowisReceiveNetworkProjectFile(file_name_utf8, contents, bytes.length);

            _free(contents);
            _free(file_name_utf8);
        }
        finally
        {
            cleanup();
        }
    }, { once: true });

    document.body.appendChild(input);
    input.click();
});

EM_JS(void, aowisDownloadTextFile, (const char *filename_utf8, const char *contents_utf8),
{
    if (filename_utf8 === 0 || contents_utf8 === 0)
        return;

    const filename = UTF8ToString(filename_utf8);
    const contents = UTF8ToString(contents_utf8);
    const blob = new Blob([contents], { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    link.style.display = "none";
    document.body.appendChild(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
});
#endif

#ifdef Q_OS_WASM
namespace
{
QPointer<SimulationManager> pending_project_import_manager;
}

extern "C" EMSCRIPTEN_KEEPALIVE void aowisReceiveNetworkProjectFile(
    const char *file_name_utf8,
    const char *contents,
    int size)
{
    if (!pending_project_import_manager || file_name_utf8 == nullptr || size < 0)
        return;

    const QString file_name = QString::fromUtf8(file_name_utf8);
    const QByteArray file_content(contents, size);
    QMetaObject::invokeMethod(
        pending_project_import_manager,
        "importNetworkProjectContent",
        Qt::DirectConnection,
        Q_ARG(QString, file_name),
        Q_ARG(QByteArray, file_content));
}
#endif

namespace
{
void showAndActivateDialog(QDialog *dialog)
{
    if (dialog == nullptr)
        return;

    if (dialog->isMinimized())
        dialog->setWindowState(dialog->windowState() & ~Qt::WindowMinimized);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void showMessageBox(
    QWidget *parent,
    QMessageBox::Icon icon,
    const QString &title,
    const QString &text)
{
    QMessageBox *message_box = new QMessageBox(icon, title, text, QMessageBox::Ok, parent);
    message_box->setAttribute(Qt::WA_DeleteOnClose);
    message_box->open();
    message_box->raise();
    message_box->activateWindow();
}

QString diagnosticDetails(const HydraulicSimulationDiagnostic &diagnostic)
{
    QStringList details;
    if (!diagnostic.message.isEmpty())
        details.append(diagnostic.message);
    if (!diagnostic.message_backend.isEmpty())
        details.append(diagnostic.message_backend);
    details.append(diagnostic.details);
    return details.join('\n');
}

QString epanetJsConversionDiagnosticDetails(
    const EpanetJsProjectConversionResult &conversion_result)
{
    QStringList details;

    for (const EpanetJsConversionDiagnostic &diagnostic : conversion_result.diagnostics)
    {
        QString prefix = diagnostic.severity == EpanetJsConversionDiagnosticSeverity::Error
            ? QStringLiteral("Error")
            : QStringLiteral("Warning");
        if (!diagnostic.code.isEmpty())
            prefix += QStringLiteral(" [%1]").arg(diagnostic.code);

        QString location;
        if (!diagnostic.table_name.isEmpty())
            location = diagnostic.table_name;
        if (diagnostic.source_id.has_value())
        {
            if (!location.isEmpty())
                location += QLatin1Char(' ');
            location += QStringLiteral("id=%1").arg(diagnostic.source_id.value());
        }

        QString text = prefix;
        if (!location.isEmpty())
            text += QStringLiteral(" (%1)").arg(location);
        if (!diagnostic.message.isEmpty())
            text += QStringLiteral(": %1").arg(diagnostic.message);
        details.append(text);
    }

    return details.join(QStringLiteral("\n\n"));
}

QString importFailureDetails(const EpanetResultImport &result)
{
    QStringList details;
    if (!result.status.message.isEmpty())
        details.append(result.status.message);
    if (!result.status.message_backend.isEmpty())
        details.append(result.status.message_backend);
    details.append(result.status.details);

    for (const HydraulicSimulationDiagnostic &diagnostic : result.diagnostics)
    {
        const QString diagnostic_text = diagnosticDetails(diagnostic);
        if (!diagnostic_text.isEmpty() && !details.contains(diagnostic_text))
            details.append(diagnostic_text);
    }

    if (details.isEmpty())
        return QStringLiteral("EPANET could not import the INP project.");

    return details.join(QStringLiteral("\n\n"));
}

void showDetailedMessageBox(
    QWidget *parent,
    QMessageBox::Icon icon,
    const QString &title,
    const QString &text,
    const QString &details)
{
    QMessageBox *message_box = new QMessageBox(icon, title, text, QMessageBox::Ok, parent);
    message_box->setAttribute(Qt::WA_DeleteOnClose);
    if (!details.isEmpty())
        message_box->setDetailedText(details);
    message_box->open();
    message_box->raise();
    message_box->activateWindow();
}

QString exportFailureDetails(const HydraulicSimulationStatus &status)
{
    QStringList details;

    if (!status.message.isEmpty())
        details.append(status.message);

    if (!status.message_backend.isEmpty())
        details.append(status.message_backend);

    for (const QString &detail : status.details)
        details.append(detail);

    if (details.isEmpty())
        return QStringLiteral("EPANET could not generate the INP document.");

    return details.join('\n');
}

QString qualityAnalysisName(WaterQualityAnalysisType analysis)
{
    switch (analysis)
    {
    case WaterQualityAnalysisType::None:
        return QStringLiteral("None");
    case WaterQualityAnalysisType::Chemical:
        return QStringLiteral("Chemical");
    case WaterQualityAnalysisType::WaterAge:
        return QStringLiteral("Water age");
    case WaterQualityAnalysisType::SourceTrace:
        return QStringLiteral("Source trace");
    }

    return QStringLiteral("Unknown quality analysis");
}

QString runReportText(const EpanetResultRun &run_result)
{
    QStringList sections;

    if (!run_result.report_lines.isEmpty())
    {
        sections.append(QStringLiteral("=== Hydraulics ==="));
        sections.append(run_result.report_lines.join('\n'));
    }

    for (const EpanetQualityResult &quality_result : run_result.quality_results)
    {
        if (quality_result.report_lines.isEmpty())
            continue;

        sections.append(QStringLiteral("=== Water quality: %1 ===")
                            .arg(qualityAnalysisName(quality_result.options.analysis)));
        sections.append(quality_result.report_lines.join('\n'));
    }

    return sections.join(QStringLiteral("\n\n"));
}
}

SimulationManager::SimulationManager(HydraulicData *hydraulic_data, QObject *parent)
    : QObject{parent},
    hydraulic_data(hydraulic_data)
{
}

SimulationManager::~SimulationManager()
{
    if (this->simulation_cancellation_flag)
        this->simulation_cancellation_flag->store(true);

#ifndef Q_OS_WASM
    if (this->simulation_thread && this->simulation_thread->isRunning())
        this->simulation_thread->wait();
#endif
}

void SimulationManager::runOrStop(const QList<WaterQualityAnalysisType> &quality_analyses)
{
    if (this->simulation_running)
    {
        stop();
        return;
    }

    run(quality_analyses);
}

void SimulationManager::stop()
{
    if (!this->simulation_running || !this->simulation_cancellation_flag)
        return;

    if (!this->simulation_cancellation_flag->exchange(true))
        emit signalSimulationStopRequested();
}

void SimulationManager::run(const QList<WaterQualityAnalysisType> &quality_analyses)
{
    if (this->simulation_running)
        return;

    const NetworkHydraulic network_hydraulic = this->hydraulic_data->networkHydraulic();

    EpanetRunRequest run_request;
    run_request.network = network_hydraulic;

    for (const WaterQualityAnalysisType analysis : quality_analyses)
    {
        WaterQualitySolverOptions quality_options;
        for (const WaterQualitySolverOptions &stored_options : this->hydraulic_data->simulationQualityRunOptions())
        {
            if (stored_options.analysis == analysis)
            {
                quality_options = stored_options;
                break;
            }
        }

        quality_options.analysis = analysis;
        if (analysis == WaterQualityAnalysisType::Chemical && quality_options.chemical_name.isEmpty())
            quality_options.chemical_name = QStringLiteral("Chlorine");
        else if (analysis == WaterQualityAnalysisType::SourceTrace)
            quality_options.trace_node_uuid = this->hydraulic_data->sourceTraceOriginNodeUuid();

        run_request.quality_runs.append(quality_options);
    }

    this->simulation_running = true;
    emit signalSimulationStarted();

    this->epanet_log.clear();
    if (this->dialog_simulation_statistics)
    {
        SimulationStatisticsDialog *statistics_dialog =
            qobject_cast<SimulationStatisticsDialog *>(this->dialog_simulation_statistics.data());
        if (statistics_dialog != nullptr)
            statistics_dialog->setEpanetLog(QString());
    }
    emit signalEpanetLogAvailabilityChanged(false);

    std::shared_ptr<EpanetResultRun> run_result = std::make_shared<EpanetResultRun>();
    const std::shared_ptr<std::atomic_bool> cancellation_flag = std::make_shared<std::atomic_bool>(false);
    this->simulation_cancellation_flag = cancellation_flag;

    QThread *thread = QThread::create([run_request, run_result, cancellation_flag]()
    {
        EpanetRunner runner;
        *run_result = runner.run(run_request, [cancellation_flag]()
        {
            return cancellation_flag->load();
        });
    });
    this->simulation_thread = thread;

    connect(thread, &QThread::finished, this, [this, thread, run_result, cancellation_flag]()
    {
        if (this->simulation_thread == thread)
            this->simulation_thread = nullptr;
        if (this->simulation_cancellation_flag == cancellation_flag)
            this->simulation_cancellation_flag.reset();

        this->simulation_running = false;
        finishSimulation(*run_result);
        emit signalSimulationFinished(run_result->cancelled);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void SimulationManager::finishSimulation(const EpanetResultRun &run_result)
{
    this->epanet_log = runReportText(run_result);
    if (this->dialog_simulation_statistics)
    {
        SimulationStatisticsDialog *statistics_dialog =
            qobject_cast<SimulationStatisticsDialog *>(this->dialog_simulation_statistics.data());
        if (statistics_dialog != nullptr)
            statistics_dialog->setEpanetLog(this->epanet_log);
    }
    emit signalEpanetLogAvailabilityChanged(!this->epanet_log.isEmpty());
    qDebug().noquote() << this->epanet_log;

    HydraulicSimulationResultTimeline hydraulic_timeline = run_result.result_timeline;
    if (!run_result.diagnostics.isEmpty())
        hydraulic_timeline.diagnostics = run_result.diagnostics;

    QList<WaterQualitySimulationResultTimeline> quality_timelines;
    for (const EpanetQualityResult &quality_result : run_result.quality_results)
        quality_timelines.append(quality_result.result_timeline);

    this->hydraulic_data->setSimulationResultTimeline(hydraulic_timeline);
    this->hydraulic_data->setWaterQualitySimulationResultTimelines(quality_timelines);

    bool has_error_diagnostic = false;
    bool has_warning_diagnostic = false;
    for (const HydraulicSimulationDiagnostic &diagnostic : run_result.diagnostics)
    {
        if (diagnostic.severity == HydraulicSimulationDiagnosticSeverity::Warning)
            has_warning_diagnostic = true;

        if (diagnostic.severity == HydraulicSimulationDiagnosticSeverity::Error
            || diagnostic.severity == HydraulicSimulationDiagnosticSeverity::Fatal)
        {
            has_error_diagnostic = true;
        }
    }

    if (run_result.cancelled)
        return;

    if (run_result.state == EpanetRunState::Error
        || run_result.state == EpanetRunState::Warning
        || has_error_diagnostic
        || has_warning_diagnostic)
    {
        showSimulationDiagnostics();
    }

    if (run_result.state == EpanetRunState::Error)
    {
        qWarning().noquote() << "EPANET simulation failed:" << run_result.status.message;

        if (!run_result.status.message_backend.isEmpty())
            qWarning().noquote() << run_result.status.message_backend;

        return;
    }

    if (run_result.state == EpanetRunState::Warning
        || has_error_diagnostic
        || has_warning_diagnostic
        || hydraulic_timeline.validity != HydraulicSimulationResultValidity::Valid)
    {
        return;
    }

    // A fully clean run (no errors, no warnings) used to need its own
    // confirmation popup here. It doesn't anymore - "Simulation Results" ->
    // "Diagnostics" now says so itself (see
    // SimulationDiagnosticsWidget::refresh()) for anyone who checks,
    // without interrupting anyone who doesn't.
}

SimulationStatisticsDialog *SimulationManager::ensureSimulationStatisticsDialog()
{
    if (this->dialog_simulation_statistics)
    {
        SimulationStatisticsDialog *statistics_dialog =
            qobject_cast<SimulationStatisticsDialog *>(this->dialog_simulation_statistics.data());
        if (statistics_dialog != nullptr)
            statistics_dialog->setEpanetLog(this->epanet_log);
        return statistics_dialog;
    }

    QWidget *main_window = qobject_cast<QWidget *>(parent());
    if (main_window == nullptr)
        main_window = QApplication::activeWindow();

    SimulationStatisticsDialog *statistics_dialog =
        new SimulationStatisticsDialog(this->hydraulic_data, main_window);
    statistics_dialog->setEpanetLog(this->epanet_log);
    this->dialog_simulation_statistics = statistics_dialog;
    return statistics_dialog;
}

void SimulationManager::showSimulationStatistics()
{
    if (!this->hydraulic_data->hasSimulationResults() && this->epanet_log.isEmpty())
        return;

    SimulationStatisticsDialog *statistics_dialog = ensureSimulationStatisticsDialog();
    if (statistics_dialog != nullptr
        && !this->hydraulic_data->hasSimulationResults() && !this->epanet_log.isEmpty())
    {
        statistics_dialog->showEpanetLogTab();
    }

    showAndActivateDialog(this->dialog_simulation_statistics);
}

void SimulationManager::showSimulationDiagnostics()
{
    if (this->hydraulic_data == nullptr)
        return;

    const std::optional<HydraulicSimulationResultTimeline> &result_timeline =
        this->hydraulic_data->simulationResultTimeline();
    if (!result_timeline.has_value() || result_timeline->diagnostics.isEmpty())
        return;

    SimulationStatisticsDialog *statistics_dialog = ensureSimulationStatisticsDialog();
    if (statistics_dialog != nullptr)
        statistics_dialog->showDiagnosticsTab();

    showAndActivateDialog(this->dialog_simulation_statistics);
}

void SimulationManager::showEpanetLog()
{
    showSimulationStatistics();

    if (!this->dialog_simulation_statistics)
        return;

    SimulationStatisticsDialog *statistics_dialog =
        qobject_cast<SimulationStatisticsDialog *>(this->dialog_simulation_statistics.data());
    if (statistics_dialog != nullptr)
        statistics_dialog->showEpanetLogTab();
}

bool SimulationManager::refuseReplacementDuringPrivateEditing(QWidget *parent_widget) const
{
    if (this->hydraulic_data == nullptr || !this->hydraulic_data->logicalEditCaptureEnabled())
        return false;
    showMessageBox(
        parent_widget,
        QMessageBox::Warning,
        tr("Private draft editing active"),
        tr("The editor is bound to a private DVMDB draft. Stop private editing before "
           "opening or importing another network; the current draft was not changed."));
    return true;
}

void SimulationManager::importNetworkProject()
{
    this->import_into_private_head = false;
    if (refuseReplacementDuringPrivateEditing(QApplication::activeWindow()))
        return;
    chooseNetworkProjectFile();
}

void SimulationManager::importNetworkProjectForHead()
{
    this->import_into_private_head = true;
    chooseNetworkProjectFile();
}

void SimulationManager::chooseNetworkProjectFile()
{
#ifdef Q_OS_WASM
    pending_project_import_manager = this;
    aowisOpenNetworkProjectFile();
#else
    QWidget *main_window = QApplication::activeWindow();
    QPointer<SimulationManager> manager(this);

    QFileDialog::getOpenFileContent(
        tr("Water network projects (*.inp *.ejsdb)"),
        [manager](const QString &file_name, const QByteArray &file_content)
        {
            if (!manager) return;
            if (file_name.isEmpty()) {
                manager->import_into_private_head = false;
                return;
            }

            QMetaObject::invokeMethod(
                manager,
                "importNetworkProjectContent",
                Qt::DirectConnection,
                Q_ARG(QString, file_name),
                Q_ARG(QByteArray, file_content));
        },
        main_window);
#endif
}

void SimulationManager::importNetworkProjectResource(
    const QString &resource_path,
    const QString &file_name)
{
    if (resource_path.isEmpty() || file_name.isEmpty())
        return;

    QWidget *main_window = QApplication::activeWindow();
    QPointer<QWidget> parent_widget(main_window);
    this->import_into_private_head = false;
    if (refuseReplacementDuringPrivateEditing(parent_widget))
        return;

    QFile resource_file(resource_path);
    if (!resource_file.open(QIODevice::ReadOnly))
    {
        showMessageBox(
            parent_widget,
            QMessageBox::Critical,
            tr("Example project failed"),
            tr("Could not open the bundled example revision: %1").arg(file_name));
        return;
    }

    const QByteArray file_content = resource_file.readAll();
    resource_file.close();
    importNetworkProjectContent(file_name, file_content);
}

void SimulationManager::importNetworkProjectContent(
    const QString &file_name,
    const QByteArray &file_content)
{
    if (file_name.isEmpty())
        return;

    QWidget *main_window = QApplication::activeWindow();
    QPointer<QWidget> parent_widget(main_window);

    if (EpanetJsProjectImporter::accepts(file_name, file_content))
    {
        if (this->epanet_js_import_in_progress)
            return;

        this->epanet_js_import_in_progress = true;
        QProgressDialog *progress = new QProgressDialog(
            tr("Importing epanet-js project…"), QString(), 0, 0, parent_widget);
        progress->setWindowTitle(tr("Importing project"));
        progress->setCancelButton(nullptr);
        progress->setMinimumDuration(0);
        progress->setWindowModality(Qt::WindowModal);
        progress->setAttribute(Qt::WA_DeleteOnClose);
        progress->show();

        // Return to the event loop before starting the synchronous import.
        // This is important for the browser to paint the indicator on WASM.
        QPointer<SimulationManager> manager(this);
        QPointer<QProgressDialog> progress_guard(progress);
        QTimer::singleShot(50, this,
            [manager, progress_guard, parent_widget, file_content]()
            {
                if (!manager)
                    return;

                EpanetJsProjectImportResult import_result =
                    EpanetJsProjectImporter::importBytes(file_content);
                manager->epanet_js_import_in_progress = false;
                if (progress_guard)
                    progress_guard->close();

                if (!import_result.recognized)
                {
                    manager->import_into_private_head = false;
                    showDetailedMessageBox(
                        parent_widget,
                        QMessageBox::Critical,
                        tr("epanet-js import failed"),
                        tr("The selected SQLite project could not be recognized as an epanet-js project."),
                        import_result.read_error);
                    return;
                }

                EpanetJsProjectConversionResult &conversion_result = import_result.conversion;
                if (!conversion_result.success)
                {
                    QString details = epanetJsConversionDiagnosticDetails(conversion_result);
                    if (details.isEmpty())
                        details = conversion_result.errorSummary();

                    manager->import_into_private_head = false;
                    showDetailedMessageBox(
                        parent_widget,
                        QMessageBox::Critical,
                        tr("epanet-js import failed"),
                        tr("The epanet-js project was recognized, but it could not be translated "
                           "into the AOWIS hydraulic model."),
                        details);
                    return;
                }

                const QString diagnostic_text = epanetJsConversionDiagnosticDetails(conversion_result);
                int warning_count = 0;
                for (const EpanetJsConversionDiagnostic &diagnostic : conversion_result.diagnostics)
                {
                    if (diagnostic.severity == EpanetJsConversionDiagnosticSeverity::Warning)
                        ++warning_count;
                }

                if (manager->import_into_private_head) {
                    manager->import_into_private_head = false;
                    emit manager->signalPrivateHeadNetworkImported(
                        std::move(conversion_result.network));
                    return;
                }

                // Unit-profile creation is optional and never affects import conversion.
                // Capture the source selections before moving the converted network.
                QStringList unrepresented_units;
                const QJsonObject imported_units = EpanetJsUnitProfile::selections(
                    conversion_result.source_display_units, &unrepresented_units);
                const QString imported_project_name = conversion_result.network.id;

                // A private session may have started while the file was chosen.
                if (manager->refuseReplacementDuringPrivateEditing(parent_widget))
                    return;
                manager->hydraulic_data->replaceNetworkHydraulic(
                    std::move(conversion_result.network),
                    std::move(conversion_result.quality_runs));
                emit manager->signalEpanetNetworkImported();

                if (!imported_units.isEmpty())
                {
                    UnitProfileManager &profiles = UnitProfileManager::instance();
                    const QString matching_id = EpanetJsUnitProfile::matchingBuiltinId(imported_units);
                    QString matching_name;
                    for (const UnitProfileManager::Profile &profile : profiles.profiles()) {
                        if (profile.builtin && profile.id == matching_id) {
                            matching_name = profile.name;
                            break;
                        }
                    }
                    const bool matches_builtin = !matching_name.isEmpty();
                    const QString base_name = imported_project_name.trimmed().isEmpty()
                        ? tr("Imported EPANET-JS units")
                        : tr("%1 (EPANET-JS units)").arg(imported_project_name);
                    QString profile_name = base_name;
                    int suffix = 2;
                    QSet<QString> existing_names;
                    for (const UnitProfileManager::Profile &profile : profiles.profiles())
                        existing_names.insert(profile.name);
                    while (existing_names.contains(profile_name))
                        profile_name = base_name + QStringLiteral(" (%1)").arg(suffix++);

                    QDialog *confirmation = new QDialog(parent_widget);
                    confirmation->setWindowTitle(tr("Unit system for imported EPANET-JS project"));
                    confirmation->setMinimumWidth(460);
                    confirmation->setAttribute(Qt::WA_DeleteOnClose);
                    QVBoxLayout *layout = new QVBoxLayout(confirmation);
                    QLabel *prompt = new QLabel(matches_builtin
                        ? tr("The imported project's units match the built-in \"%1\" unit system. "
                             "You can use it directly; no new unit system is needed.\n\n"
                             "If you prefer your own named unit system that you can edit later, "
                             "you can create a copy instead. You can also do this at any time "
                             "in Settings → Units.")
                              .arg(matching_name)
                        : tr("The imported project's units do not match any built-in AOWIS "
                             "unit system. You can create a named, editable unit system "
                             "using these display preferences.\n\n"
                             "This is optional: importing the project does not depend on it. "
                             "You can also set up a custom unit system later in Settings → Units."),
                        confirmation);
                    prompt->setWordWrap(true);
                    layout->addWidget(prompt);

                    QCheckBox *create_copy = nullptr;
                    if (matches_builtin) {
                        create_copy = new QCheckBox(tr("Create a named, editable copy instead"), confirmation);
                        create_copy->setChecked(false);
                        layout->addWidget(create_copy);
                    }
                    QLabel *name_label = new QLabel(tr("New unit system name:"), confirmation);
                    layout->addWidget(name_label);
                    QLineEdit *name_edit = new QLineEdit(profile_name, confirmation);
                    name_label->setBuddy(name_edit);
                    layout->addWidget(name_edit);
                    if (matches_builtin) {
                        name_label->hide();
                        name_edit->hide();
                        QObject::connect(create_copy, &QCheckBox::toggled,
                                         name_label, &QWidget::setVisible);
                        QObject::connect(create_copy, &QCheckBox::toggled,
                                         name_edit, &QWidget::setVisible);
                    }
                    if (!unrepresented_units.isEmpty()) {
                        QLabel *warning = new QLabel(
                            tr("Some source unit choices are not available as AOWIS display "
                               "settings: %1\nA custom unit system cannot reproduce those "
                               "particular choices. This does not affect the imported network values.")
                                .arg(unrepresented_units.join(QStringLiteral(", "))),
                            confirmation);
                        warning->setWordWrap(true);
                        layout->addWidget(warning);
                    }
                    QCheckBox *activate_profile = new QCheckBox(
                        matches_builtin ? tr("Use \"%1\" after import").arg(matching_name)
                                        : tr("Use the new unit system after import"), confirmation);
                    activate_profile->setChecked(true);
                    layout->addWidget(activate_profile);
                    if (matches_builtin) {
                        QObject::connect(create_copy, &QCheckBox::toggled,
                                         activate_profile, [activate_profile, matching_name](bool create) {
                            activate_profile->setText(create
                                ? QObject::tr("Use the new unit system after import")
                                : QObject::tr("Use \"%1\" after import").arg(matching_name));
                        });
                    }
                    QDialogButtonBox *buttons = new QDialogButtonBox(
                        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, confirmation);
                    layout->addWidget(buttons);
                    QObject::connect(buttons, &QDialogButtonBox::rejected,
                                     confirmation, &QDialog::reject);
                    QObject::connect(buttons, &QDialogButtonBox::accepted,
                                     confirmation, [confirmation, name_edit, activate_profile,
                                                    create_copy, matching_id, matches_builtin, imported_units] {
                        if (matches_builtin && !create_copy->isChecked()) {
                            if (activate_profile->isChecked()) {
                                UnitProfileManager &profiles = UnitProfileManager::instance();
                                for (int index = 0; index < profiles.profiles().size(); ++index) {
                                    if (profiles.profiles().at(index).id == matching_id
                                        && profiles.profiles().at(index).builtin) {
                                        (void)profiles.select(index);
                                        break;
                                    }
                                }
                            }
                            confirmation->accept();
                            return;
                        }
                        const QString name = name_edit->text().trimmed();
                        if (!UnitProfileManager::instance().createFromUnits(
                                name, imported_units, activate_profile->isChecked())) {
                            QMessageBox *warning = new QMessageBox(
                                QMessageBox::Warning, QObject::tr("Invalid unit profile name"),
                                QObject::tr("The profile name must be nonempty and unique. "
                                            "Choose a different name."),
                                QMessageBox::Ok, confirmation);
                            warning->setAttribute(Qt::WA_DeleteOnClose);
                            warning->open();
                            return;
                        }
                        confirmation->accept();
                    });
                    confirmation->open(); // Nonblocking, including under Qt WebAssembly.
                }

                if (warning_count > 0)
                {
                    showDetailedMessageBox(
                        parent_widget,
                        QMessageBox::Warning,
                        tr("Project imported with warnings"),
                        tr("The epanet-js project was imported, but %1 warning(s) were reported.")
                            .arg(warning_count),
                        diagnostic_text);
                }
            });
        return;
    }

    QTemporaryDir temporary_directory;
    if (!temporary_directory.isValid())
    {
        showMessageBox(
            parent_widget,
            QMessageBox::Critical,
            tr("EPANET import failed"),
            tr("Could not create temporary storage for the selected INP file."));
        return;
    }

    const QString local_file_name = QFileInfo(file_name).fileName();
    const QString temporary_file_path = temporary_directory.filePath(
        local_file_name.isEmpty() ? QStringLiteral("import.inp") : local_file_name);

    QFile temporary_file(temporary_file_path);
    if (!temporary_file.open(QIODevice::WriteOnly)
        || temporary_file.write(file_content) != file_content.size())
    {
        showMessageBox(
            parent_widget,
            QMessageBox::Critical,
            tr("EPANET import failed"),
            tr("Could not prepare the selected INP file for import."));
        return;
    }

    temporary_file.close();

    EpanetRunner runner;
    EpanetResultImport import_result = runner.importInp(temporary_file_path);
    if (this->import_into_private_head) {
        this->import_into_private_head = false;
        if (!import_result.status.success) {
            showDetailedMessageBox(parent_widget, QMessageBox::Critical,
                tr("EPANET import failed"),
                tr("The selected EPANET network could not be imported."),
                importFailureDetails(import_result));
            return;
        }
        emit signalPrivateHeadNetworkImported(std::move(import_result.request.network));
        return;
    }
    finishEpanetNetworkImport(std::move(import_result), parent_widget);
}

void SimulationManager::finishEpanetNetworkImport(
    EpanetResultImport import_result,
    QWidget *parent_widget)
{
    if (!import_result.status.success)
    {
        showDetailedMessageBox(
            parent_widget,
            QMessageBox::Critical,
            tr("EPANET import failed"),
            tr("The selected EPANET project could not be imported."),
            importFailureDetails(import_result));
        return;
    }

    const int diagnostic_count = import_result.diagnostics.size();
    const bool complete = import_result.complete;
    const QString diagnostic_text = importFailureDetails(import_result);

    // A private session may have started while the file was chosen.
    if (refuseReplacementDuringPrivateEditing(parent_widget))
        return;
    this->hydraulic_data->replaceNetworkHydraulic(
        std::move(import_result.request.network),
        std::move(import_result.request.quality_runs));
    emit signalEpanetNetworkImported();

    if (!complete)
    {
        showDetailedMessageBox(
            parent_widget,
            QMessageBox::Warning,
            tr("Project imported with warnings"),
            tr("The project was imported, but %1 import issue(s) were reported.")
                .arg(diagnostic_count),
            diagnostic_text);
    }
}

void SimulationManager::exportEpanetNetwork()
{
    exportSelectedEpanetNetwork(this->hydraulic_data->networkHydraulic());
}

void SimulationManager::exportSelectedEpanetNetwork(const NetworkHydraulic &network_hydraulic)
{
    QWidget *main_window = QApplication::activeWindow();

#ifndef Q_OS_WASM
    QFileDialog dialog(main_window, tr("Export EPANET network"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters(QStringList{tr("EPANET INP files (*.inp)")});
    dialog.setDefaultSuffix(QStringLiteral("inp"));
    dialog.selectFile(QStringLiteral("network.inp"));

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QStringList selected_files = dialog.selectedFiles();
    if (selected_files.isEmpty())
        return;

    const QString file_path = selected_files.constFirst();
    if (!EpanetNetworkFileFormat::acceptsInpExportPath(file_path))
    {
        showMessageBox(
            main_window,
            QMessageBox::Warning,
            tr("Unsupported EPANET export format"),
            tr("AOWIS currently exports EPANET .inp files only. "
               "The .ejsdb format is supported for import, not export. "
               "Choose a filename ending in .inp."));
        return;
    }
#endif

    EpanetRunRequest export_request;
    export_request.network = network_hydraulic;
    export_request.quality_runs = this->hydraulic_data->simulationQualityRunOptions();

    EpanetRunner runner;
    const EpanetResultInp export_result = runner.retrieveInp(export_request);

    if (!export_result.status.success)
    {
        const QString details = exportFailureDetails(export_result.status);
        qWarning().noquote() << "EPANET INP export failed:" << details;
        showMessageBox(main_window, QMessageBox::Critical, tr("EPANET export failed"), details);
        return;
    }

    if (export_result.inp_text.isEmpty())
    {
        const QString message = tr("EPANET generated an empty INP document.");
        qWarning().noquote() << message;
        showMessageBox(main_window, QMessageBox::Critical, tr("EPANET export failed"), message);
        return;
    }

    const QByteArray inp_data = export_result.inp_text.toUtf8();

#ifdef Q_OS_WASM
    const QByteArray filename = QByteArrayLiteral("network.inp");
    aowisDownloadTextFile(filename.constData(), inp_data.constData());
#else
    QSaveFile output_file(file_path);
    if (!output_file.open(QIODevice::WriteOnly))
    {
        showMessageBox(
            main_window,
            QMessageBox::Critical,
            tr("EPANET export failed"),
            tr("Could not open the selected file for writing:\n%1").arg(output_file.errorString()));
        return;
    }

    const qint64 bytes_written = output_file.write(inp_data);
    if (bytes_written != static_cast<qint64>(inp_data.size()))
    {
        const QString error_message = output_file.errorString();
        output_file.cancelWriting();
        showMessageBox(
            main_window,
            QMessageBox::Critical,
            tr("EPANET export failed"),
            tr("Could not write the complete INP document:\n%1").arg(error_message));
        return;
    }

    if (!output_file.commit())
    {
        showMessageBox(
            main_window,
            QMessageBox::Critical,
            tr("EPANET export failed"),
            tr("Could not finalize the exported INP file:\n%1").arg(output_file.errorString()));
    }
#endif
}
