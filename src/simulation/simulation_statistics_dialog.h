#ifndef SIMULATION_STATISTICS_DIALOG_H
#define SIMULATION_STATISTICS_DIALOG_H

#include <QDialog>
#include <QString>

class HydraulicData;
class QTableWidget;
class QTabWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QEvent;
class SimulationDiagnosticsWidget;

class SimulationStatisticsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SimulationStatisticsDialog(HydraulicData *hydraulic_data, QWidget *parent = nullptr);
    void setEpanetLog(const QString &epanet_log);
    void showEpanetLogTab();
    void showDiagnosticsTab();

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslateView();
    HydraulicData *hydraulic_data = nullptr;
    SimulationDiagnosticsWidget *widget_diagnostics = nullptr;
    QTreeWidget *tree_summary = nullptr;
    QLabel *summary_hint = nullptr;
    QTableWidget *table_timeline = nullptr;
    QTabWidget *tabs = nullptr;
    QTextBrowser *text_epanet_log = nullptr;
    QTreeWidgetItem *summary_head_error_item = nullptr;
    QTreeWidgetItem *summary_flow_change_item = nullptr;

    void refresh();
    void refreshSummary();
    void refreshTimeline();
    void refreshSummaryUnits();
    void refreshTimelineUnits();
    void syncCurrentResultSelection(int result_index);
};
#endif // SIMULATION_STATISTICS_DIALOG_H
