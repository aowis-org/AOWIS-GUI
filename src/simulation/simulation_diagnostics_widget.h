#ifndef SIMULATION_DIAGNOSTICS_WIDGET_H
#define SIMULATION_DIAGNOSTICS_WIDGET_H

#include <QWidget>

class HydraulicData;
class QLabel;
class QListWidget;
class QTextBrowser;
class QEvent;

class SimulationDiagnosticsWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SimulationDiagnosticsWidget(HydraulicData *hydraulic_data, QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void refresh();
    void showDiagnosticDetails(int diagnostic_index);
    void selectDiagnosticEntity(int diagnostic_index);

    HydraulicData *hydraulic_data = nullptr;
    QLabel *label_result_validity = nullptr;
    QListWidget *list_diagnostics = nullptr;
    QTextBrowser *text_details = nullptr;
};

#endif // SIMULATION_DIAGNOSTICS_WIDGET_H
