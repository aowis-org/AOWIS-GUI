#include "network/hydraulic_data.h"
#include "tabs/tab_hydraulic_libraries_widget.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

#include <cstdio>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

QPushButton *buttonWithText(QWidget *widget, const QString &text)
{
    for (QPushButton *button : widget->findChildren<QPushButton *>())
        if (button->text() == text)
            return button;
    return nullptr;
}

bool editThroughDialog(HydraulicLibrariesWidget *widget, bool creating,
                       const QString &id, const QString &data)
{
    QPushButton *button = buttonWithText(widget, creating ? QStringLiteral("+ Add")
                                                          : QStringLiteral("Edit"));
    if (button == nullptr || !button->isEnabled())
        return false;
    bool filled = false;
    QTimer::singleShot(0, widget, [&filled, id, data]()
    {
        QDialog *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr)
            return;
        QLineEdit *id_field = dialog->findChild<QLineEdit *>();
        const QList<QPlainTextEdit *> editors = dialog->findChildren<QPlainTextEdit *>();
        QDialogButtonBox *buttons = dialog->findChild<QDialogButtonBox *>();
        if (id_field == nullptr || editors.size() != 2 || buttons == nullptr)
        {
            dialog->reject();
            return;
        }
        id_field->setText(id);
        editors.at(1)->setPlainText(data);
        filled = true;
        buttons->button(QDialogButtonBox::Save)->click();
    });
    button->click();
    return filled;
}

bool deleteThroughDialog(HydraulicLibrariesWidget *widget)
{
    QPushButton *button = buttonWithText(widget, QStringLiteral("Delete"));
    if (button == nullptr || !button->isEnabled())
        return false;
    bool confirmed = false;
    QTimer::singleShot(0, widget, [&confirmed]()
    {
        QMessageBox *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (dialog == nullptr)
            return;
        // Exercise the actual standard button. Calling done(Yes) bypasses
        // QMessageBox's clicked-button handling and can leave its result
        // different from a user-confirmed Yes in offscreen Qt tests.
        QAbstractButton *yes_button = dialog->button(QMessageBox::Yes);
        if (yes_button == nullptr)
        {
            dialog->reject();
            return;
        }
        yes_button->click();
        confirmed = true;
    });
    button->click();
    return confirmed;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    HydraulicData data;
    HydraulicLibrariesWidget widget(&data);
    widget.show();
    app.processEvents();

    QComboBox *category = widget.findChild<QComboBox *>();
    QListWidget *rows = widget.findChild<QListWidget *>();
    if (!require(category != nullptr && rows != nullptr, "library selector or list absent"))
        return 1;

    const QString initial_values[] = {
        QStringLiteral("0, 110, 0.15, -"), QStringLiteral("1.25"),
        QStringLiteral("1, 20"), QStringLiteral("5, 35"),
        QStringLiteral("5, 80"), QStringLiteral("5, 0.3"),
        QStringLiteral("40, 60"), QStringLiteral("2, 4")
    };
    const QString updated_values[] = {
        QStringLiteral("2, 120, 0.25, -"), QStringLiteral("1.50"),
        QStringLiteral("2, 30"), QStringLiteral("6, 36"),
        QStringLiteral("6, 81"), QStringLiteral("6, 0.4"),
        QStringLiteral("50, 70"), QStringLiteral("3, 5")
    };
    int events = 0;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &encoded)
    {
        if (!encoded.isEmpty())
            ++events;
    });
    data.setLogicalEditCaptureEnabled(true);

    for (int index = 0; index < 8; ++index)
    {
        category->setCurrentIndex(index);
        const QString initial_name = QStringLiteral("ITEM_%1").arg(index);
        const QString changed_name = QStringLiteral("RENAMED_%1").arg(index);
        const int before = events;
        if (!require(editThroughDialog(&widget, true, initial_name, initial_values[index]),
                     "failed to create library item via GUI")) return 1;
        if (!require(rows->count() == 1 && events == before + 1,
                     "create did not update list or emit transaction")) return 1;
        if (!require(!rows->currentItem()->data(Qt::UserRole).toUuid().isNull(),
                     "new library UUID missing")) return 1;

        if (!require(editThroughDialog(&widget, false, changed_name, updated_values[index]),
                     "failed to update library item via GUI")) return 1;
        if (!require(rows->count() == 1 && rows->item(0)->text() == changed_name
                     && events == before + 2, "update did not emit transaction")) return 1;
        const NetworkHydraulic &network = data.networkHydraulic();
        bool payload_ok = false;
        switch (index)
        {
        case 0: payload_ok = network.pipe_materials.size() == 1
                    && network.pipe_materials.constFirst().roughness_by_age.size() == 1
                    && network.pipe_materials.constFirst().roughness_by_age.constFirst().age_years == 2; break;
        case 1: payload_ok = network.patterns_time.size() == 1
                    && network.patterns_time.constFirst().multipliers.constFirst() == 1.5; break;
        case 2: payload_ok = network.curves_tank_volume.size() == 1
                    && network.curves_tank_volume.constFirst().points.constFirst().water_level_m == 2; break;
        case 3: payload_ok = network.curves_pump_head.size() == 1
                    && network.curves_pump_head.constFirst().points.constFirst().flow_m3_per_h == 6; break;
        case 4: payload_ok = network.curves_pump_efficiency.size() == 1
                    && network.curves_pump_efficiency.constFirst().points.constFirst().flow_m3_per_h == 6; break;
        case 5: payload_ok = network.curves_valve_headloss.size() == 1
                    && network.curves_valve_headloss.constFirst().points.constFirst().flow_m3_per_h == 6; break;
        case 6: payload_ok = network.curves_valve_characteristic.size() == 1
                    && network.curves_valve_characteristic.constFirst().points.constFirst().position_percent == 50; break;
        case 7: payload_ok = network.curves_generic.size() == 1
                    && network.curves_generic.constFirst().points.constFirst().x == 3; break;
        default: break;
        }
        if (!require(payload_ok, "dialog did not persist edited numerical payload")) return 1;
        if (!require(editThroughDialog(&widget, false, changed_name, updated_values[index])
                     && events == before + 2,
                     "no-op library edit generated an autosave transaction")) return 1;

        if (!require(deleteThroughDialog(&widget),
                     "failed to confirm library deletion via GUI")) return 1;
        if (rows->count() != 0 || events != before + 3)
        {
            std::fprintf(stderr,
                         "HC-02.6 delete collection=%d: remaining_rows=%d, "
                         "events=%d, expected_events=%d, capture_enabled=%d\n",
                         index, rows->count(), events, before + 3,
                         data.logicalEditCaptureEnabled() ? 1 : 0);
            if (!require(false, "delete did not emit transaction or refresh list")) return 1;
        }
    }
    std::puts("Hydraulic libraries GUI: all eight collections passed create/update/delete capture");
    return 0;
}
