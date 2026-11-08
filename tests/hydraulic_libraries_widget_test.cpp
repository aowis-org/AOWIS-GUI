#include "network/hydraulic_data.h"
#include "tabs/tab_hydraulic_libraries_widget.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QTimer>
#include <QTranslator>

#include <cstdio>

namespace
{
// Deliberately independent of installed .qm files: this test validates that
// existing widgets respond to a translator change, rather than construction.
class LibraryTestTranslator : public QTranslator
{
public:
    // No .qm file is loaded. QTranslator::isEmpty() would otherwise return
    // true, so Qt would install our synthetic translator without emitting
    // LanguageChange. That tests lookups, but not actual live UI refresh.
    bool isEmpty() const override { return false; }

    QString translate(const char *context, const char *source_text,
                      const char *, int) const override
    {
        if (qstrcmp(context, "HydraulicLibrariesWidget") != 0)
            return QString();
        return QStringLiteral("TEST: ") + QString::fromUtf8(source_text);
    }
};

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
    // Switch the language on an already-constructed, active collection widget.
    // Renaming category captions must not select another collection, reload
    // hydraulic data or emit a logical edit / autosave transaction.
    category->setCurrentIndex(5);
    const int edits_before_language = events;
    int category_changes = 0;
    QObject::connect(category, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     &widget, [&category_changes](int) { ++category_changes; });
    LibraryTestTranslator translator;
    if (!require(QCoreApplication::installTranslator(&translator),
                 "could not install synthetic library translator")) return 1;
    app.processEvents();
    if (!require(category->currentIndex() == 5 && category_changes == 0,
                 "language change replaced the selected library collection")
        || !require(category->itemText(5) == QStringLiteral("TEST: Valve headloss curves"),
                    "live collection title was not retranslated")
        || !require(events == edits_before_language,
                    "language change generated a hydraulic edit")) return 1;
    QCoreApplication::removeTranslator(&translator);
    app.processEvents();
    if (!require(category->currentIndex() == 5 && category_changes == 0
                 && category->itemText(5) == QStringLiteral("Valve headloss curves"),
                 "collection did not revert to English in place")) return 1;

    // The dialog runs a nested event loop. Language changes must update its
    // title, form labels, buttons and hint without touching the user's draft.
    const int before_dialog = events;
    bool checked_modal_switch = false;
    QPushButton *add_button = buttonWithText(&widget, QStringLiteral("+ Add"));
    if (!require(add_button != nullptr, "missing library Add button")) return 1;
    QTimer::singleShot(0, &widget, [&]()
    {
        QDialog *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) return;
        QLineEdit *id_field = dialog->findChild<QLineEdit *>();
        const QList<QPlainTextEdit *> editors = dialog->findChildren<QPlainTextEdit *>();
        QDialogButtonBox *buttons = dialog->findChild<QDialogButtonBox *>();
        QFormLayout *form = dialog->findChild<QFormLayout *>();
        if (id_field == nullptr || editors.size() != 2 || buttons == nullptr || form == nullptr)
        {
            dialog->reject();
            return;
        }
        QPlainTextEdit *values = editors.at(1);
        const QString draft = QStringLiteral("12.25, 38.75\n14.25, 39.75");
        id_field->setText(QStringLiteral("DRAFT_NOT_SAVED"));
        values->setPlainText(draft);
        QTextCursor selection = values->textCursor();
        selection.setPosition(1);
        selection.setPosition(6, QTextCursor::KeepAnchor);
        values->setTextCursor(selection);
        if (!QCoreApplication::installTranslator(&translator))
        {
            dialog->reject();
            return;
        }
        app.processEvents();
        const QLabel *id_caption = qobject_cast<QLabel *>(form->labelForField(id_field));
        const bool in_translation = dialog->windowTitle()
                == QStringLiteral("TEST: Add hydraulic library item")
            && id_caption != nullptr && id_caption->text() == QStringLiteral("TEST: ID:")
            && buttons->button(QDialogButtonBox::Save)->text() == QStringLiteral("TEST: Save")
            && category->currentIndex() == 5
            && id_field->text() == QStringLiteral("DRAFT_NOT_SAVED")
            && values->toPlainText() == draft
            && values->textCursor().anchor() == 1
            && values->textCursor().position() == 6;
        QCoreApplication::removeTranslator(&translator);
        app.processEvents();
        const bool back_to_english = dialog->windowTitle() == QStringLiteral("Add hydraulic library item")
            && id_caption != nullptr && id_caption->text() == QStringLiteral("ID:")
            && buttons->button(QDialogButtonBox::Save)->text() == QStringLiteral("Save")
            && id_field->text() == QStringLiteral("DRAFT_NOT_SAVED")
            && values->toPlainText() == draft
            && values->textCursor().anchor() == 1
            && values->textCursor().position() == 6;
        checked_modal_switch = in_translation && back_to_english;
        dialog->reject();
    });
    add_button->click();
    if (!require(checked_modal_switch, "open library dialog did not retranslate while preserving draft")
        || !require(events == before_dialog, "modal language switch created an autosave edit")
        || !require(category_changes == 0, "modal language switch changed collection")) return 1;

    std::puts("Hydraulic libraries GUI: eight collections and live language switching passed");
    return 0;
}
