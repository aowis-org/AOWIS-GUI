#pragma once

#include <QWidget>
#include <QUuid>

class HydraulicData;
class QComboBox;
class QListWidget;
class QPushButton;

// Transaction-safe editing surface for the eight NetworkHydraulic libraries.
// Does not write MODEL collections directly: all changes go through HydraulicData.
class HydraulicLibrariesWidget : public QWidget
{
public:
    explicit HydraulicLibrariesWidget(HydraulicData *hydraulic_data, QWidget *parent = nullptr);
    void refresh(const QUuid &select_uuid = QUuid());

protected:
    void showEvent(QShowEvent *event) override;

private:
    void editEntry(bool creating);
    void deleteEntry();
    QUuid selectedUuid() const;
    void updateButtons();

    HydraulicData *hydraulic_data = nullptr;
    QComboBox *category = nullptr;
    QListWidget *entries = nullptr;
    QPushButton *edit_button = nullptr;
    QPushButton *delete_button = nullptr;
};
