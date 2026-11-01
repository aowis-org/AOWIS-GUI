#include "network/hydraulic_data.h"
#include "projects/hydraulic_draft_event_adapter.h"
#include "tabs/tab_hydraulic_controls_widget.h"

#include <aowis/model/serialization/model_codec.h>
#include <aowis/model/uuid.h>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include <cstdio>

namespace
{
bool require(bool valid, const char *message)
{
    if (!valid) std::fprintf(stderr, "FAIL: %s\n", message);
    return valid;
}

QPushButton *buttonWithText(QWidget *widget, const QString &text)
{
    for (QPushButton *button : widget->findChildren<QPushButton *>())
        if (button->text() == text) return button;
    return nullptr;
}

bool clickSave(QDialog *dialog)
{
    QDialogButtonBox *box = dialog->findChild<QDialogButtonBox *>();
    if (!box || !box->button(QDialogButtonBox::Save)) return false;
    box->button(QDialogButtonBox::Save)->click();
    return true;
}

bool labelDialog(HydraulicControlsWidget *widget, bool creating,
                 const QString &id, const QString &text)
{
    QPushButton *button = buttonWithText(widget, creating ? QStringLiteral("+ Add")
                                                         : QStringLiteral("Edit"));
    if (!button || !button->isEnabled()) return false;
    bool completed = false;
    QTimer::singleShot(0, widget, [&]()
    {
        QDialog *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        QLineEdit *name = dialog->findChild<QLineEdit *>(QStringLiteral("labelId"));
        QLineEdit *caption = dialog->findChild<QLineEdit *>(QStringLiteral("labelText"));
        QLineEdit *latitude = dialog->findChild<QLineEdit *>(QStringLiteral("labelLatitude"));
        QLineEdit *longitude = dialog->findChild<QLineEdit *>(QStringLiteral("labelLongitude"));
        QComboBox *anchor = dialog->findChild<QComboBox *>(QStringLiteral("labelAnchor"));
        if (!name || !caption || !latitude || !longitude || !anchor) return;
        anchor->setCurrentIndex(1); // existing junction
        name->setText(id);
        caption->setText(text);
        latitude->setText(QStringLiteral("50.1234"));
        longitude->setText(QStringLiteral("8.6789"));
        completed = clickSave(dialog);
    });
    button->click();
    return completed;
}

bool ruleDialog(HydraulicControlsWidget *widget, bool creating, const QString &id)
{
    QPushButton *button = buttonWithText(widget, creating ? QStringLiteral("+ Add")
                                                         : QStringLiteral("Edit"));
    if (!button || !button->isEnabled()) return false;
    bool completed = false;
    QTimer::singleShot(0, widget, [&]()
    {
        QDialog *rule = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!rule) return;
        QLineEdit *name = rule->findChild<QLineEdit *>(QStringLiteral("ruleId"));
        if (!name) return;
        name->setText(id);
        if (!creating)
        {
            completed = clickSave(rule);
            return;
        }
        QPushButton *add_premise = buttonWithText(rule, QStringLiteral("+ Premise"));
        QPushButton *add_action = buttonWithText(rule, QStringLiteral("+ Action"));
        if (!add_premise || !add_action) return;
        QTimer::singleShot(0, rule, [rule, add_action, &completed]()
        {
            QDialog *premise = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!premise || premise == rule) return;
            QComboBox *object = premise->findChild<QComboBox *>(QStringLiteral("premiseObject"));
            QComboBox *variable = premise->findChild<QComboBox *>(QStringLiteral("premiseVariable"));
            QLineEdit *elapsed = premise->findChild<QLineEdit *>(QStringLiteral("premiseElapsedTime"));
            if (!object || !variable || !elapsed) return;
            object->setCurrentIndex(2); // System (no referenced node)
            variable->setCurrentIndex(9); // Time
            elapsed->setText(QStringLiteral("60"));
            if (!clickSave(premise)) return;
            QTimer::singleShot(0, rule, [rule, add_action, &completed]()
            {
                QTimer::singleShot(0, rule, [rule, &completed]()
                {
                    QDialog *action = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!action || action == rule) return;
                    QComboBox *link = action->findChild<QComboBox *>(QStringLiteral("actionLink"));
                    QComboBox *status = action->findChild<QComboBox *>(QStringLiteral("actionStatus"));
                    if (!link || !status) return;
                    link->setCurrentIndex(1); // existing pipe
                    status->setCurrentIndex(1); // Open
                    if (!clickSave(action)) return;
                    QTimer::singleShot(0, rule, [rule, &completed]()
                    {
                        completed = clickSave(rule);
                    });
                });
                add_action->click();
            });
        });
        add_premise->click();
    });
    button->click();
    return completed;
}

bool deleteSelected(HydraulicControlsWidget *widget)
{
    QPushButton *button = buttonWithText(widget, QStringLiteral("Delete"));
    if (!button || !button->isEnabled()) return false;
    bool confirmed = false;
    QTimer::singleShot(0, widget, [&]()
    {
        QMessageBox *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!question) return;
        QAbstractButton *yes = question->button(QMessageBox::Yes);
        if (!yes) return;
        yes->click();
        confirmed = true;
    });
    button->click();
    return confirmed;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    NetworkHydraulic initial;
    initial.uuid = createUuidV7();
    initial.id = QStringLiteral("HC-02.8 controls/labels");
    HydraulicNodeJunction from;
    from.uuid = createUuidV7();
    from.id = QStringLiteral("J1");
    HydraulicNodeJunction to;
    to.uuid = createUuidV7();
    to.id = QStringLiteral("J2");
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.id = QStringLiteral("P1");
    pipe.node_uuid_from = from.uuid;
    pipe.node_uuid_to = to.uuid;
    initial.nodes_junctions = {from, to};
    initial.links_pipes = {pipe};

    HydraulicData data;
    data.replaceNetworkHydraulic(initial, {});
    NetworkHydraulic staged = initial;
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &encoded)
    {
        if (!encoded.isEmpty()) events.append(encoded);
    });
    data.setLogicalEditCaptureEnabled(true);

    HydraulicControlsWidget widget(&data);
    widget.show();
    app.processEvents();
    QComboBox *category = widget.findChild<QComboBox *>();
    QListWidget *rows = widget.findChild<QListWidget *>();
    if (!require(category != nullptr && rows != nullptr, "controls/labels selector absent")) return 1;

    category->setCurrentIndex(1);
    if (!require(labelDialog(&widget, true, QStringLiteral("L1"), QStringLiteral("First"))
                 && rows->count() == 1 && events.size() == 1
                 && data.networkHydraulic().map_labels.first().text == QStringLiteral("First")
                 && data.networkHydraulic().map_labels.first().anchor_node_uuid == from.uuid,
                 "create label must update network and emit transaction")) return 1;
    if (!require(labelDialog(&widget, false, QStringLiteral("L2"), QStringLiteral("Changed"))
                 && rows->count() == 1 && events.size() == 2
                 && data.networkHydraulic().map_labels.first().text == QStringLiteral("Changed"),
                 "edit label must update network and emit transaction")) return 1;
    if (!require(labelDialog(&widget, false, QStringLiteral("L2"), QStringLiteral("Changed"))
                 && events.size() == 2, "unchanged label must not emit transaction")) return 1;
    if (!require(deleteSelected(&widget) && rows->count() == 0 && events.size() == 3,
                 "delete label must emit transaction")) return 1;

    category->setCurrentIndex(0);
    if (!require(ruleDialog(&widget, true, QStringLiteral("R1"))
                 && rows->count() == 1 && events.size() == 4
                 && data.networkHydraulic().controls_rules.size() == 1
                 && data.networkHydraulic().controls_rules.first().premises.size() == 1
                 && data.networkHydraulic().controls_rules.first().actions_then.size() == 1,
                 "create multi-clause rule must emit transaction")) return 1;
    if (!require(ruleDialog(&widget, false, QStringLiteral("R2"))
                 && rows->count() == 1 && events.size() == 5
                 && data.networkHydraulic().controls_rules.first().id == QStringLiteral("R2"),
                 "edit rule must emit transaction")) return 1;
    if (!require(ruleDialog(&widget, false, QStringLiteral("R2")) && events.size() == 5,
                 "unchanged rule must not emit transaction")) return 1;
    if (!require(deleteSelected(&widget) && rows->count() == 0 && events.size() == 6,
                 "delete rule must emit transaction")) return 1;

    QString error;
    for (const QByteArray &event : events)
    {
        QList<PersonalWorkingCopyChange> records;
        QList<HydraulicDraftMembershipEdit> membership;
        if (!require(HydraulicDraftEventAdapter::prepareWithMembership(
                         event, staged, records, membership, &error),
                     "GUI event must replay through DVMDB adapter"))
        {
            std::fprintf(stderr, "Replay error: %s\n", error.toUtf8().constData());
            return 1;
        }
    }
    if (!require(aowis::model::ModelCodec<NetworkHydraulic>::encode(staged)
                  == aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()),
                 "GUI edits must replay to canonical full hydraulic network")) return 1;
    return 0;
}
