#ifndef SIMULATION_MANAGER_H
#define SIMULATION_MANAGER_H

#include <QObject>
#include <QByteArray>
#include <QApplication>
#include <QScreen>
#include <QTextBrowser>
#include <QFontDatabase>
#include <QDialog>
#include <QList>
#include <QPointer>
#include <QThread>
#include <QVBoxLayout>

#include <atomic>
#include <memory>
#include <aowis/model/hydraulic/network_hydraulic.h>
#include <aowis/model/hydraulic/hydraulic_simulation_results.h>
#include <aowis/model/hydraulic/hydraulic_simulation_status.h>
#include <aowis/model/hydraulic/hydraulic_types.h>
#include "network/hydraulic_data.h"

struct EpanetResultImport;
struct EpanetResultRun;
class SimulationStatisticsDialog;

class SimulationManager : public QObject
{
    Q_OBJECT
public:
    explicit SimulationManager(HydraulicData *hydraulic_data, QObject *parent = nullptr);
    ~SimulationManager() override;

    void runOrStop(const QList<WaterQualityAnalysisType> &quality_analyses);
    void run(const QList<WaterQualityAnalysisType> &quality_analyses);
    void stop();
    void showSimulationStatistics();
    void showSimulationDiagnostics();
    void showEpanetLog();
    void importNetworkProject();
    void importNetworkProjectResource(const QString &resource_path, const QString &file_name);
    void exportEpanetNetwork();
    void exportSelectedEpanetNetwork(const NetworkHydraulic &network);

private slots:
    void importNetworkProjectContent(const QString &file_name, const QByteArray &file_content);

private:
    HydraulicData *hydraulic_data = nullptr;
    QString epanet_log;
    QPointer<QDialog> dialog_simulation_statistics = nullptr;
    QPointer<QThread> simulation_thread = nullptr;
    std::shared_ptr<std::atomic_bool> simulation_cancellation_flag;
    bool simulation_running = false;
    bool epanet_js_import_in_progress = false;

    void finishSimulation(const EpanetResultRun &run_result);
    void finishEpanetNetworkImport(EpanetResultImport import_result, QWidget *parent_widget);
    SimulationStatisticsDialog *ensureSimulationStatisticsDialog();

signals:
    void signalSimulationStarted();
    void signalSimulationStopRequested();
    void signalSimulationFinished(bool cancelled);
    void signalEpanetLogAvailabilityChanged(bool available);
    void signalEpanetNetworkImported();
};
#endif // SIMULATION_MANAGER_H
