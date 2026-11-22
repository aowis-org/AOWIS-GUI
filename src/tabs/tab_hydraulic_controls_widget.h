#pragma once

#include <QWidget>
#include <QUuid>

class HydraulicData;
class QListWidget;
class QPushButton;

// GUI rule controls for the current NetworkHydraulicDistribution. All persistent edits are
// routed through HydraulicData (HC-02.7), never through mutable list access.
class HydraulicControlsWidget : public QWidget
{
public:
    explicit HydraulicControlsWidget(HydraulicData *hydraulic_data, QWidget *parent = nullptr);
    void refresh(const QUuid &select_uuid = QUuid());

protected:
    void showEvent(QShowEvent *event) override;

private:
    QUuid selectedUuid() const;
    void updateButtons();
    void editEntry(bool creating);
    void deleteEntry();

    HydraulicData *hydraulic_data = nullptr;
    QListWidget *entries = nullptr;
    QPushButton *edit_button = nullptr;
    QPushButton *delete_button = nullptr;
};
