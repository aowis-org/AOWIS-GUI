#include "network/hydraulic_data.h"
#include "projects/hydraulic_draft_event_adapter.h"
#include "tabs/tab_hydraulic_controls_widget.h"

#include <aowis/model/serialization/model_codec.h>
#include <aowis/model/uuid.h>

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
#include <QPushButton>
#include <QTimer>
#include <QTranslator>

#include <cstdio>

namespace
{
class ControlTestTranslator final : public QTranslator
{
public:
    // A synthetic translator without a .qm must be nonempty for Qt to post
    // LanguageChange to existing top-level widgets and their children.
    bool isEmpty() const override { return false; }

    QString translate(const char *context, const char *source, const char *, int) const override
    {
        if (qstrcmp(context, "HydraulicControlsWidget") != 0)
            return QString();
        return QStringLiteral("TEST: ") + QString::fromUtf8(source);
    }
};

bool hasLabel(QWidget *widget, const QString &label)
{
    for (QLabel *entry : widget->findChildren<QLabel *>())
        if (entry->text() == label)
            return true;
    return false;
}

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
    NetworkHydraulicDistribution initial;
    initial.uuid = createUuidV7();
    initial.id = QStringLiteral("HC-02.8 rule controls");
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
    data.replaceNetworkHydraulicDistribution(initial, {});
    NetworkHydraulicDistribution staged = initial;
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
    QListWidget *rows = widget.findChild<QListWidget *>();
    if (!require(rows != nullptr, "rule controls list absent")) return 1;
    // MODEL A3: map labels are map annotations in their own layer, not a
    // hydraulic collection, so this widget offers rule controls only.
    if (!require(widget.findChild<QComboBox *>() == nullptr,
                 "rule controls widget still offers a collection selector")) return 1;

    if (!require(ruleDialog(&widget, true, QStringLiteral("R1"))
                 && rows->count() == 1 && events.size() == 1
                 && data.networkHydraulicDistribution().controls_rules.size() == 1
                 && data.networkHydraulicDistribution().controls_rules.first().premises.size() == 1
                 && data.networkHydraulicDistribution().controls_rules.first().actions_then.size() == 1,
                 "create multi-clause rule must emit transaction")) return 1;
    if (!require(ruleDialog(&widget, false, QStringLiteral("R2"))
                 && rows->count() == 1 && events.size() == 2
                 && data.networkHydraulicDistribution().controls_rules.first().id == QStringLiteral("R2"),
                 "edit rule must emit transaction")) return 1;
    if (!require(ruleDialog(&widget, false, QStringLiteral("R2")) && events.size() == 2,
                 "unchanged rule must not emit transaction")) return 1;
    if (!require(deleteSelected(&widget) && rows->count() == 0 && events.size() == 3,
                 "delete rule must emit transaction")) return 1;

    ControlTestTranslator translator;
    const int edits_before_language = events.size();

    // A rule dialog has nested premise and action dialogs. Switching the
    // language with two modal loops running must also preserve the rule draft.
    bool rule_checked = false;
    bool premise_checked = false;
    QPushButton *add_rule = buttonWithText(&widget, QStringLiteral("+ Add"));
    if (!require(add_rule != nullptr, "missing rule Add button")) return 1;
    QTimer::singleShot(0, &widget, [&]()
    {
        QDialog *rule = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (rule == nullptr) return;
        QLineEdit *id = rule->findChild<QLineEdit *>(QStringLiteral("ruleId"));
        QPushButton *add_premise = buttonWithText(rule, QStringLiteral("+ Premise"));
        if (id == nullptr || add_premise == nullptr)
        {
            rule->reject();
            return;
        }
        id->setText(QStringLiteral("pending RULE"));
        id->setCursorPosition(4);
        QCoreApplication::installTranslator(&translator);
        app.processEvents();
        rule_checked = rule->windowTitle() == QStringLiteral("TEST: Hydraulic rule control")
            && hasLabel(rule, QStringLiteral("TEST: Premises (IF / AND / OR)"))
            && id->text() == QStringLiteral("pending RULE") && id->cursorPosition() == 4;
        QTimer::singleShot(0, rule, [&]()
        {
            QDialog *premise = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (premise == nullptr || premise == rule) return;
            QComboBox *object = premise->findChild<QComboBox *>(QStringLiteral("premiseObject"));
            QComboBox *variable = premise->findChild<QComboBox *>(QStringLiteral("premiseVariable"));
            QLineEdit *elapsed = premise->findChild<QLineEdit *>(QStringLiteral("premiseElapsedTime"));
            if (object == nullptr || variable == nullptr || elapsed == nullptr)
            {
                premise->reject();
                return;
            }
            object->setCurrentIndex(2); // System
            variable->setCurrentIndex(9); // Time
            elapsed->setText(QStringLiteral("12345"));
            elapsed->setCursorPosition(3);
            QCoreApplication::removeTranslator(&translator);
            app.processEvents();
            QCoreApplication::installTranslator(&translator);
            app.processEvents();
            premise_checked = premise->windowTitle() == QStringLiteral("TEST: Rule premise")
                && hasLabel(premise, QStringLiteral("TEST: Elapsed time (s)"))
                && object->itemText(2) == QStringLiteral("TEST: System")
                && variable->itemText(9) == QStringLiteral("TEST: Time")
                && object->currentIndex() == 2 && variable->currentIndex() == 9
                && elapsed->text() == QStringLiteral("12345") && elapsed->cursorPosition() == 3;
            premise->reject();
        });
        add_premise->click();
        QCoreApplication::removeTranslator(&translator);
        app.processEvents();
        rule_checked = rule_checked
            && id->text() == QStringLiteral("pending RULE") && id->cursorPosition() == 4
            && rule->windowTitle() == QStringLiteral("Hydraulic rule control");
        rule->reject();
    });
    add_rule->click();
    if (!require(rule_checked && premise_checked,
                 "nested rule/premise live language switch did not preserve drafts")
        || !require(events.size() == edits_before_language,
                    "cancelled localized rule dialog generated an edit")) return 1;

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
    if (!require(aowis::model::ModelCodec<NetworkHydraulicDistribution>::encode(staged)
                  == aowis::model::ModelCodec<NetworkHydraulicDistribution>::encode(data.networkHydraulicDistribution()),
                 "GUI edits must replay to canonical full hydraulic network")) return 1;
    return 0;
}
