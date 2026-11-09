#include "config/live_ui_text.h"
#include "tabs/tab_hydraulic_controls_widget.h"
#include "network/hydraulic_data.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <optional>
#include <cstddef>

namespace
{
// This entire screen is GUI-owned.  Fixed translation contexts survive the
// renderer extraction and do not depend on the locations of C++ source files.
QString controlText(const char *source)
{
    return QCoreApplication::translate("HydraulicControlsWidget", source);
}

void localizeRow(QFormLayout *form, QWidget *field, const char *source, QWidget *parent)
{
    QLabel *label = new QLabel(parent);
    LiveUiText::bind(label, [label, source]() { label->setText(controlText(source)); });
    form->addRow(label, field);
}

void localizePlaceholder(QLineEdit *field)
{
    LiveUiText::bind(field, [field]() {
        field->setPlaceholderText(controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Unset")));
    });
}

// Qt retranslates standard QDialogButtonBox buttons itself.  Restore our
// catalogue after that event so the labels also change on already-open modals.
class ControlDialogButtons final : public QDialogButtonBox
{
public:
    explicit ControlDialogButtons(QWidget *parent)
        : QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, parent)
    {
        retranslate();
    }

protected:
    bool event(QEvent *event) override
    {
        const bool handled = QDialogButtonBox::event(event);
        if (event->type() == QEvent::LanguageChange)
            retranslate();
        return handled;
    }

private:
    void retranslate()
    {
        QPushButton *save = button(QDialogButtonBox::Save);
        QPushButton *cancel = button(QDialogButtonBox::Cancel);
        if (save != nullptr)
            save->setText(controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Save")));
        if (cancel != nullptr)
            cancel->setText(controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Cancel")));
    }
};

void localizeTitle(QDialog *dialog, const char *source)
{
    LiveUiText::bind(dialog, [dialog, source]() { dialog->setWindowTitle(controlText(source)); });
}

const char *const logic_names[] = {
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "IF"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "AND"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "OR")
};
const char *const object_names[] = {
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Node"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Link"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "System")
};
const char *const variable_names[] = {
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Demand"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Head"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Grade"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Level"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Pressure"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Flow"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Status"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Setting"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Power"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Time"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Clock Time"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Fill Time"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Drain Time")
};
const char *const comparison_names[] = {
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "="),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "!="),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "<="),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", ">="),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "<"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", ">"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "IS"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "IS NOT"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "BELOW"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "ABOVE")
};
const char *const status_names[] = {
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "(unset)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Open"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Closed"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Active")
};
const char *const setting_names[] = {
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Pump speed ratio"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Valve pressure head (m)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Valve flow (m³/h)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Valve loss coefficient"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Valve position (%)")
};
const char *const numeric_names[] = {
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Demand (m³/h)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Hydraulic head (m)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Water level (m)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Pressure head (m)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Flow (m³/h)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Power (kW)")
};
const char *const time_names[] = {
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Elapsed time (s)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Time of day (s)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Fill time (s)"),
    QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Drain time (s)")
};

template<std::size_t N>
QString enumText(const char *const (&names)[N], int index)
{
    if (index < 0 || index >= int(N))
        return QString();
    return controlText(names[index]);
}

template<typename Entity>
const Entity *lookup(const QList<Entity> &items, const QUuid &uuid)
{
    for (const Entity &item : items)
        if (item.uuid == uuid) return &item;
    return nullptr;
}

template<std::size_t N>
QComboBox *enumeration(QWidget *parent, const char *const (&labels)[N], int selected)
{
    QComboBox *combo = new QComboBox(parent);
    for (const char *source : labels)
        combo->addItem(controlText(source));
    combo->setCurrentIndex(selected);
    // Preserve enum ordinal/currentIndex and never emit value-change signals.
    const char *const *sources = labels;
    LiveUiText::bind(combo, [combo, sources]() {
        const QSignalBlocker blocker(combo);
        for (int index = 0; index < int(N); ++index)
            combo->setItemText(index, controlText(sources[index]));
    });
    return combo;
}

QLineEdit *numberField(QWidget *parent, const std::optional<double> &value)
{
    return new QLineEdit(value ? QString::number(*value, 'g', 17) : QString(), parent);
}

QLineEdit *timeField(QWidget *parent, const std::optional<quint64> &value)
{
    return new QLineEdit(value ? QString::number(*value) : QString(), parent);
}

bool readNumber(QLineEdit *field, std::optional<double> *out)
{
    const QString text = field->text().trimmed();
    if (text.isEmpty()) { out->reset(); return true; }
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value)) return false;
    *out = value;
    return true;
}

bool readTime(QLineEdit *field, std::optional<quint64> *out)
{
    const QString text = field->text().trimmed();
    if (text.isEmpty()) { out->reset(); return true; }
    bool ok = false;
    const quint64 value = text.toULongLong(&ok);
    if (!ok) return false;
    *out = value;
    return true;
}

void addSettingFields(QFormLayout *form, QWidget *parent,
                      const HydraulicControlLinkSetting &setting,
                      QList<QLineEdit *> *fields)
{
    const std::optional<double> values[] = {
        setting.pump_speed_ratio, setting.valve_pressure_head_m,
        setting.valve_flow_m3_per_h, setting.valve_loss_coefficient,
        setting.valve_position_percent
    };
    for (int index = 0; index < 5; ++index)
    {
        QLineEdit *field = numberField(parent, values[index]);
        localizePlaceholder(field);
        localizeRow(form, field, setting_names[index], parent);
        fields->append(field);
    }
}

bool readSettingFields(const QList<QLineEdit *> &fields, HydraulicControlLinkSetting *setting)
{
    return fields.size() == 5
        && readNumber(fields.at(0), &setting->pump_speed_ratio)
        && readNumber(fields.at(1), &setting->valve_pressure_head_m)
        && readNumber(fields.at(2), &setting->valve_flow_m3_per_h)
        && readNumber(fields.at(3), &setting->valve_loss_coefficient)
        && readNumber(fields.at(4), &setting->valve_position_percent);
}

void addReference(QComboBox *combo, const char *kind, const QString &id, const QUuid &uuid)
{
    combo->addItem(QString(), uuid);
    const int index = combo->count() - 1;
    combo->setItemData(index, QString::fromUtf8(kind), Qt::UserRole + 1);
    combo->setItemData(index, id, Qt::UserRole + 2);
}

void localizeReferences(QComboBox *combo)
{
    const QSignalBlocker blocker(combo);
    for (int index = 0; index < combo->count(); ++index)
    {
        const QByteArray source = combo->itemData(index, Qt::UserRole + 1).toString().toUtf8();
        const QString caption = controlText(source.constData());
        combo->setItemText(index, index == 0 ? caption
            : caption.arg(combo->itemData(index, Qt::UserRole + 2).toString()));
    }
}

QComboBox *referenceSelector(QWidget *parent, const NetworkHydraulic &network,
                             bool links, const QUuid &selected)
{
    QComboBox *combo = new QComboBox(parent);
    addReference(combo, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "(none)"), QString(), QUuid());
    if (links)
    {
        for (const HydraulicLinkPipe &item : network.links_pipes)
            addReference(combo, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Pipe: %1"), item.id, item.uuid);
        for (const HydraulicLinkPump &item : network.links_pumps)
            addReference(combo, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Pump: %1"), item.id, item.uuid);
        for (const HydraulicLinkValve &item : network.links_valves)
            addReference(combo, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Valve: %1"), item.id, item.uuid);
    }
    else
    {
        for (const HydraulicNodeJunction &item : network.nodes_junctions)
            addReference(combo, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Junction: %1"), item.id, item.uuid);
        for (const HydraulicNodeReservoir &item : network.nodes_reservoirs)
            addReference(combo, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Reservoir: %1"), item.id, item.uuid);
        for (const HydraulicNodeTank &item : network.nodes_tanks)
            addReference(combo, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Tank: %1"), item.id, item.uuid);
    }
    for (int index = 0; index < combo->count(); ++index)
    {
        if (combo->itemData(index).toUuid() == selected)
        {
            combo->setCurrentIndex(index);
            break;
        }
    }
    LiveUiText::bind(combo, [combo]() { localizeReferences(combo); });
    return combo;
}

QFormLayout *scrollingForm(QDialog *dialog, QVBoxLayout *layout)
{
    QScrollArea *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    QWidget *body = new QWidget(scroll);
    QFormLayout *form = new QFormLayout(body);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);
    return form;
}

void addDialogButtons(QDialog *dialog, QVBoxLayout *layout)
{
    QDialogButtonBox *buttons = new ControlDialogButtons(dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
}

bool editPremise(QWidget *parent, const NetworkHydraulic &network,
                 HydraulicControlRulePremise *premise)
{
    QDialog dialog(parent);
    localizeTitle(&dialog, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Rule premise"));
    dialog.resize(545, 670);
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QFormLayout *form = scrollingForm(&dialog, layout);
    QComboBox *logical = enumeration(&dialog, logic_names, int(premise->logical_operator));
    QComboBox *object = enumeration(&dialog, object_names, int(premise->object));
    object->setObjectName(QStringLiteral("premiseObject"));
    QComboBox *reference = new QComboBox(&dialog);
    const QUuid old_uuid = premise->object_uuid;
    const std::function<void()> refresh_references = [object, reference, &dialog, &network, old_uuid]()
    {
        const bool system = object->currentIndex() == int(HydraulicControlRuleObject::System);
        QComboBox *choices = referenceSelector(&dialog, network,
            object->currentIndex() == int(HydraulicControlRuleObject::Link), old_uuid);
        reference->clear();
        for (int index = 0; index < choices->count(); ++index)
        {
            reference->addItem(choices->itemText(index), choices->itemData(index));
            reference->setItemData(index, choices->itemData(index, Qt::UserRole + 1), Qt::UserRole + 1);
            reference->setItemData(index, choices->itemData(index, Qt::UserRole + 2), Qt::UserRole + 2);
        }
        reference->setCurrentIndex(choices->currentIndex());
        reference->setEnabled(!system);
        delete choices;
    };
    refresh_references();
    LiveUiText::bind(reference, [reference]() { localizeReferences(reference); });
    QObject::connect(object, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     &dialog, [refresh_references](int) { refresh_references(); });
    QComboBox *variable = enumeration(&dialog, variable_names, int(premise->variable));
    variable->setObjectName(QStringLiteral("premiseVariable"));
    QComboBox *comparison = enumeration(&dialog, comparison_names, int(premise->comparison));
    QComboBox *status = enumeration(&dialog, status_names,
        premise->status ? int(*premise->status) + 1 : 0);
    localizeRow(form, logical, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Logic"), &dialog);
    localizeRow(form, object, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Object type"), &dialog);
    localizeRow(form, reference, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Object"), &dialog);
    localizeRow(form, variable, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Variable"), &dialog);
    localizeRow(form, comparison, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Comparison"), &dialog);
    localizeRow(form, status, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Status"), &dialog);
    const std::optional<double> numeric_values[] = {premise->demand_m3_per_h,
        premise->hydraulic_head_m, premise->water_level_m, premise->pressure_head_m,
        premise->flow_m3_per_h, premise->power_kw};
    QList<QLineEdit *> values;
    for (int index = 0; index < 6; ++index)
    {
        QLineEdit *field = numberField(&dialog, numeric_values[index]);
        localizePlaceholder(field);
        localizeRow(form, field, numeric_names[index], &dialog);
        values.append(field);
    }
    QList<QLineEdit *> setting_fields;
    addSettingFields(form, &dialog, premise->link_setting, &setting_fields);
    const std::optional<quint64> time_values[] = {premise->elapsed_time_s,
        premise->time_of_day_s, premise->fill_time_s, premise->drain_time_s};
    QList<QLineEdit *> times;
    for (int index = 0; index < 4; ++index)
    {
        QLineEdit *field = timeField(&dialog, time_values[index]);
        if (index == 0) field->setObjectName(QStringLiteral("premiseElapsedTime"));
        localizePlaceholder(field);
        localizeRow(form, field, time_names[index], &dialog);
        times.append(field);
    }
    addDialogButtons(&dialog, layout);
    QDialogButtonBox *buttons = dialog.findChild<QDialogButtonBox *>();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
    {
        HydraulicControlRulePremise edited = *premise;
        edited.logical_operator = HydraulicControlRuleLogicalOperator(logical->currentIndex());
        edited.object = HydraulicControlRuleObject(object->currentIndex());
        edited.object_uuid = edited.object == HydraulicControlRuleObject::System
            ? QUuid() : reference->currentData().toUuid();
        edited.variable = HydraulicControlRuleVariable(variable->currentIndex());
        edited.comparison = HydraulicControlRuleOperator(comparison->currentIndex());
        edited.status = status->currentIndex() == 0 ? std::optional<HydraulicControlRuleStatus>()
            : std::optional<HydraulicControlRuleStatus>(HydraulicControlRuleStatus(status->currentIndex() - 1));
        if (!readNumber(values.at(0), &edited.demand_m3_per_h)
            || !readNumber(values.at(1), &edited.hydraulic_head_m)
            || !readNumber(values.at(2), &edited.water_level_m)
            || !readNumber(values.at(3), &edited.pressure_head_m)
            || !readNumber(values.at(4), &edited.flow_m3_per_h)
            || !readNumber(values.at(5), &edited.power_kw)
            || !readSettingFields(setting_fields, &edited.link_setting)
            || !readTime(times.at(0), &edited.elapsed_time_s)
            || !readTime(times.at(1), &edited.time_of_day_s)
            || !readTime(times.at(2), &edited.fill_time_s)
            || !readTime(times.at(3), &edited.drain_time_s))
        {
            QMessageBox::warning(&dialog, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Invalid premise")),
                controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Numerical fields must contain finite numbers, and times must be non-negative integers.")));
            return;
        }
        if (edited.object != HydraulicControlRuleObject::System && edited.object_uuid.isNull())
        {
            QMessageBox::warning(&dialog, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Invalid premise")),
                controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Select an existing node or link.")));
            return;
        }
        *premise = edited;
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}

bool editAction(QWidget *parent, const NetworkHydraulic &network, HydraulicControlRuleAction *action)
{
    QDialog dialog(parent);
    localizeTitle(&dialog, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Rule action"));
    dialog.resize(490, 470);
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QFormLayout *form = scrollingForm(&dialog, layout);
    QComboBox *reference = referenceSelector(&dialog, network, true, action->link_uuid);
    reference->setObjectName(QStringLiteral("actionLink"));
    QComboBox *status = enumeration(&dialog, status_names,
        action->status ? int(*action->status) + 1 : 0);
    status->setObjectName(QStringLiteral("actionStatus"));
    localizeRow(form, reference, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Target link"), &dialog);
    localizeRow(form, status, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Status"), &dialog);
    QList<QLineEdit *> setting_fields;
    addSettingFields(form, &dialog, action->setting, &setting_fields);
    addDialogButtons(&dialog, layout);
    QDialogButtonBox *buttons = dialog.findChild<QDialogButtonBox *>();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
    {
        HydraulicControlRuleAction edited = *action;
        edited.link_uuid = reference->currentData().toUuid();
        edited.status = status->currentIndex() == 0 ? std::optional<HydraulicControlRuleStatus>()
            : std::optional<HydraulicControlRuleStatus>(HydraulicControlRuleStatus(status->currentIndex() - 1));
        if (edited.link_uuid.isNull() || !readSettingFields(setting_fields, &edited.setting))
        {
            QMessageBox::warning(&dialog, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Invalid action")),
                controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Select a valid link and enter finite setting values.")));
            return;
        }
        *action = edited;
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}

QString premiseSummary(const HydraulicControlRulePremise &premise)
{
    return QStringLiteral("%1 | %2 | %3 | %4")
        .arg(enumText(logic_names, int(premise.logical_operator)),
             enumText(object_names, int(premise.object)),
             premise.object_uuid.toString(QUuid::WithoutBraces),
             enumText(variable_names, int(premise.variable)));
}

QString actionSummary(const HydraulicControlRuleAction &action)
{
    return controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Link: %1 | %2"))
        .arg(action.link_uuid.toString(QUuid::WithoutBraces),
             action.status ? enumText(status_names, int(*action.status) + 1)
                           : controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Setting")));
}

void clauseEditor(QDialog *dialog, QVBoxLayout *layout, const char *label,
                  const NetworkHydraulic &network, QList<HydraulicControlRulePremise> *premises)
{
    QLabel *heading = new QLabel(dialog);
    LiveUiText::bind(heading, [heading, label]() { heading->setText(controlText(label)); });
    layout->addWidget(heading);
    QListWidget *list = new QListWidget(dialog);
    list->setObjectName(QString::fromUtf8(label));
    layout->addWidget(list);
    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *add = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "+ Premise"), dialog);
    QPushButton *edit = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Edit Premise"), dialog);
    QPushButton *remove = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Remove Premise"), dialog);
    buttons->addWidget(add); buttons->addWidget(edit); buttons->addWidget(remove);
    layout->addLayout(buttons);
    const std::function<void(int)> redraw = [list, premises](int selected)
    {
        list->clear();
        for (const HydraulicControlRulePremise &item : *premises)
            list->addItem(premiseSummary(item));
        if (selected >= 0 && selected < list->count()) list->setCurrentRow(selected);
    };
    redraw(-1);
    LiveUiText::bind(list, [list, premises]() {
        const QSignalBlocker blocker(list);
        for (int index = 0; index < list->count() && index < premises->size(); ++index)
            list->item(index)->setText(premiseSummary(premises->at(index)));
    });
    QObject::connect(add, &QPushButton::clicked, dialog, [dialog, &network, premises, redraw]()
    {
        HydraulicControlRulePremise item;
        if (!editPremise(dialog, network, &item)) return;
        premises->append(item);
        redraw(premises->size() - 1);
    });
    QObject::connect(edit, &QPushButton::clicked, dialog, [dialog, &network, premises, list, redraw]()
    {
        const int index = list->currentRow();
        if (index < 0 || index >= premises->size()) return;
        HydraulicControlRulePremise item = premises->at(index);
        if (editPremise(dialog, network, &item)) { (*premises)[index] = item; redraw(index); }
    });
    QObject::connect(remove, &QPushButton::clicked, dialog, [premises, list, redraw]()
    {
        const int index = list->currentRow();
        if (index < 0 || index >= premises->size()) return;
        premises->removeAt(index);
        redraw(-1);
    });
    QObject::connect(list, &QListWidget::itemDoubleClicked, dialog,
                     [edit](QListWidgetItem *) { edit->click(); });
}

void clauseEditor(QDialog *dialog, QVBoxLayout *layout, const char *label,
                  const NetworkHydraulic &network, QList<HydraulicControlRuleAction> *actions)
{
    QLabel *heading = new QLabel(dialog);
    LiveUiText::bind(heading, [heading, label]() { heading->setText(controlText(label)); });
    layout->addWidget(heading);
    QListWidget *list = new QListWidget(dialog);
    list->setObjectName(QString::fromUtf8(label));
    layout->addWidget(list);
    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *add = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "+ Action"), dialog);
    QPushButton *edit = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Edit Action"), dialog);
    QPushButton *remove = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Remove Action"), dialog);
    buttons->addWidget(add); buttons->addWidget(edit); buttons->addWidget(remove);
    layout->addLayout(buttons);
    const std::function<void(int)> redraw = [list, actions](int selected)
    {
        list->clear();
        for (const HydraulicControlRuleAction &item : *actions)
            list->addItem(actionSummary(item));
        if (selected >= 0 && selected < list->count()) list->setCurrentRow(selected);
    };
    redraw(-1);
    LiveUiText::bind(list, [list, actions]() {
        const QSignalBlocker blocker(list);
        for (int index = 0; index < list->count() && index < actions->size(); ++index)
            list->item(index)->setText(actionSummary(actions->at(index)));
    });
    QObject::connect(add, &QPushButton::clicked, dialog, [dialog, &network, actions, redraw]()
    {
        HydraulicControlRuleAction item;
        if (!editAction(dialog, network, &item)) return;
        actions->append(item);
        redraw(actions->size() - 1);
    });
    QObject::connect(edit, &QPushButton::clicked, dialog, [dialog, &network, actions, list, redraw]()
    {
        const int index = list->currentRow();
        if (index < 0 || index >= actions->size()) return;
        HydraulicControlRuleAction item = actions->at(index);
        if (editAction(dialog, network, &item)) { (*actions)[index] = item; redraw(index); }
    });
    QObject::connect(remove, &QPushButton::clicked, dialog, [actions, list, redraw]()
    {
        const int index = list->currentRow();
        if (index < 0 || index >= actions->size()) return;
        actions->removeAt(index);
        redraw(-1);
    });
    QObject::connect(list, &QListWidget::itemDoubleClicked, dialog,
                     [edit](QListWidgetItem *) { edit->click(); });
}

bool editRule(QWidget *parent, const NetworkHydraulic &network, HydraulicControlRule *rule)
{
    QDialog dialog(parent);
    localizeTitle(&dialog, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Hydraulic rule control"));
    dialog.resize(690, 720);
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QFormLayout *form = new QFormLayout();
    QLineEdit *id = new QLineEdit(rule->id, &dialog);
    id->setObjectName(QStringLiteral("ruleId"));
    QLineEdit *priority = new QLineEdit(QString::number(rule->priority, 'g', 17), &dialog);
    priority->setObjectName(QStringLiteral("rulePriority"));
    QCheckBox *enabled = new QCheckBox(&dialog);
    enabled->setChecked(rule->enabled);
    localizeRow(form, id, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "ID"), &dialog);
    localizeRow(form, priority, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Priority"), &dialog);
    localizeRow(form, enabled, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Enabled"), &dialog);
    layout->addLayout(form);
    HydraulicControlRule edited = *rule;
    clauseEditor(&dialog, layout, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Premises (IF / AND / OR)"), network, &edited.premises);
    clauseEditor(&dialog, layout, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "THEN actions"), network, &edited.actions_then);
    clauseEditor(&dialog, layout, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "ELSE actions"), network, &edited.actions_else);
    addDialogButtons(&dialog, layout);
    QDialogButtonBox *buttons = dialog.findChild<QDialogButtonBox *>();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
    {
        if (edited.premises.isEmpty() || edited.actions_then.isEmpty())
        {
            QMessageBox::warning(&dialog, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Incomplete rule")),
                controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "A rule needs at least one premise and one THEN action.")));
            return;
        }
        bool priority_ok = false;
        const double priority_value = priority->text().trimmed().toDouble(&priority_ok);
        if (!priority_ok || !std::isfinite(priority_value))
        {
            QMessageBox::warning(&dialog, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Invalid priority")),
                controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Priority must be a finite number.")));
            return;
        }
        edited.id = id->text().trimmed();
        edited.priority = priority_value;
        edited.enabled = enabled->isChecked();
        *rule = edited;
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}

bool readFinite(QLineEdit *field, double *out, double minimum, double maximum)
{
    bool ok = false;
    const double value = field->text().trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(value) || value < minimum || value > maximum) return false;
    *out = value;
    return true;
}

bool editLabel(QWidget *parent, const NetworkHydraulic &network, HydraulicMapLabel *label)
{
    QDialog dialog(parent);
    localizeTitle(&dialog, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Hydraulic map label"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QFormLayout *form = new QFormLayout();
    QLineEdit *id = new QLineEdit(label->id, &dialog);
    id->setObjectName(QStringLiteral("labelId"));
    QLineEdit *text = new QLineEdit(label->text, &dialog);
    text->setObjectName(QStringLiteral("labelText"));
    QLineEdit *latitude = new QLineEdit(QString::number(label->coordinate_wgs84.latitude_deg, 'g', 17), &dialog);
    latitude->setObjectName(QStringLiteral("labelLatitude"));
    QLineEdit *longitude = new QLineEdit(QString::number(label->coordinate_wgs84.longitude_deg, 'g', 17), &dialog);
    longitude->setObjectName(QStringLiteral("labelLongitude"));
    QLineEdit *altitude = numberField(&dialog, label->coordinate_wgs84.altitude_m);
    altitude->setObjectName(QStringLiteral("labelAltitude"));
    QComboBox *anchor = referenceSelector(&dialog, network, false, label->anchor_node_uuid);
    anchor->setObjectName(QStringLiteral("labelAnchor"));
    localizeRow(form, id, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "ID"), &dialog);
    localizeRow(form, text, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Text"), &dialog);
    localizeRow(form, latitude, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Latitude (°)"), &dialog);
    localizeRow(form, longitude, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Longitude (°)"), &dialog);
    localizeRow(form, altitude, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Altitude (m; optional)"), &dialog);
    localizeRow(form, anchor, QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Anchor node (optional)"), &dialog);
    layout->addLayout(form);
    addDialogButtons(&dialog, layout);
    QDialogButtonBox *buttons = dialog.findChild<QDialogButtonBox *>();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
    {
        HydraulicMapLabel edited = *label;
        if (!readFinite(latitude, &edited.coordinate_wgs84.latitude_deg, -90.0, 90.0)
            || !readFinite(longitude, &edited.coordinate_wgs84.longitude_deg, -180.0, 180.0)
            || !readNumber(altitude, &edited.coordinate_wgs84.altitude_m))
        {
            QMessageBox::warning(&dialog, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Invalid coordinates")),
                controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Provide finite latitude [-90,90], longitude [-180,180], and optional altitude.")));
            return;
        }
        edited.id = id->text().trimmed();
        edited.text = text->text();
        edited.anchor_node_uuid = anchor->currentData().toUuid();
        *label = edited;
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}
}

HydraulicControlsWidget::HydraulicControlsWidget(HydraulicData *hydraulic_data, QWidget *parent)
    : QWidget(parent), hydraulic_data(hydraulic_data)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *description = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Rule controls and map labels belong to the currently loaded hydraulic network. "
        "Changes use its durable working-copy autosave transactions."), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    QHBoxLayout *toolbar = new QHBoxLayout();
    this->category = new QComboBox(this);
    const char *const names[] = {
        QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Rule controls"),
        QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Map labels")
    };
    this->category->addItems({controlText(names[0]), controlText(names[1])});
    LiveUiText::bind(this->category, [this]() {
        const QSignalBlocker blocker(this->category);
        this->category->setItemText(0, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Rule controls")));
        this->category->setItemText(1, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Map labels")));
    });
    toolbar->addWidget(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Collection:"), this));
    toolbar->addWidget(this->category, 1);
    QPushButton *add = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "+ Add"), this);
    this->edit_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Edit"), this);
    this->delete_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Delete"), this);
    toolbar->addWidget(add);
    toolbar->addWidget(this->edit_button);
    toolbar->addWidget(this->delete_button);
    layout->addLayout(toolbar);
    this->entries = new QListWidget(this);
    layout->addWidget(this->entries, 1);
    connect(this->category, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refresh(); });
    connect(this->entries, &QListWidget::itemSelectionChanged,
            this, [this]() { updateButtons(); });
    connect(this->entries, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { editEntry(false); });
    connect(add, &QPushButton::clicked, this, [this]() { editEntry(true); });
    connect(this->edit_button, &QPushButton::clicked, this, [this]() { editEntry(false); });
    connect(this->delete_button, &QPushButton::clicked, this, [this]() { deleteEntry(); });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded,
            this, [this]() { refresh(); });
    refresh();
}

void HydraulicControlsWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh(selectedUuid());
}

QUuid HydraulicControlsWidget::selectedUuid() const
{
    const QListWidgetItem *item = this->entries->currentItem();
    return item ? item->data(Qt::UserRole).toUuid() : QUuid();
}

void HydraulicControlsWidget::updateButtons()
{
    const bool selected = !selectedUuid().isNull();
    this->edit_button->setEnabled(selected);
    this->delete_button->setEnabled(selected);
}

void HydraulicControlsWidget::refresh(const QUuid &select_uuid)
{
    this->entries->clear();
    const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
    if (this->category->currentIndex() == 0)
    {
        for (const HydraulicControlRule &rule : network.controls_rules)
        {
            QListWidgetItem *item = new QListWidgetItem(rule.id, this->entries);
            item->setData(Qt::UserRole, rule.uuid);
            item->setToolTip(rule.uuid.toString(QUuid::WithoutBraces));
        }
    }
    else
    {
        for (const HydraulicMapLabel &label : network.map_labels)
        {
            QListWidgetItem *item = new QListWidgetItem(label.id, this->entries);
            item->setData(Qt::UserRole, label.uuid);
            item->setToolTip(label.text + QStringLiteral("\n")
                + label.uuid.toString(QUuid::WithoutBraces));
        }
    }
    for (int index = 0; index < this->entries->count(); ++index)
    {
        if (this->entries->item(index)->data(Qt::UserRole).toUuid() != select_uuid) continue;
        this->entries->setCurrentRow(index);
        break;
    }
    updateButtons();
}

void HydraulicControlsWidget::editEntry(bool creating)
{
    const QUuid uuid = creating ? QUuid() : selectedUuid();
    if (!creating && uuid.isNull()) return;
    const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
    QUuid changed;
    if (this->category->currentIndex() == 0)
    {
        HydraulicControlRule rule;
        if (!creating)
        {
            const HydraulicControlRule *existing = lookup(network.controls_rules, uuid);
            if (!existing) return;
            rule = *existing;
        }
        if (!editRule(this, network, &rule)) return;
        if (creating) changed = this->hydraulic_data->addRuleControl(rule);
        else if (this->hydraulic_data->updateRuleControl(rule)) changed = rule.uuid;
    }
    else
    {
        HydraulicMapLabel label;
        if (!creating)
        {
            const HydraulicMapLabel *existing = lookup(network.map_labels, uuid);
            if (!existing) return;
            label = *existing;
        }
        if (!editLabel(this, network, &label)) return;
        if (creating) changed = this->hydraulic_data->addMapLabel(label);
        else if (this->hydraulic_data->updateMapLabel(label)) changed = label.uuid;
    }
    if (changed.isNull())
    {
        QMessageBox::warning(this, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Cannot save hydraulic edit")),
            controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "The edit was rejected. Check entity references, coordinates and the active network.")));
        return;
    }
    refresh(changed);
}

void HydraulicControlsWidget::deleteEntry()
{
    const QUuid uuid = selectedUuid();
    if (uuid.isNull()) return;
    if (QMessageBox::question(this, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Delete hydraulic entity")),
        controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Delete the selected entry from this network?"))) != QMessageBox::Yes)
        return;
    const bool removed = this->category->currentIndex() == 0
        ? this->hydraulic_data->removeRuleControl(uuid)
        : this->hydraulic_data->removeMapLabel(uuid);
    if (!removed)
    {
        QMessageBox::warning(this, controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "Cannot delete")),
            controlText(QT_TRANSLATE_NOOP("HydraulicControlsWidget", "The entry could not be removed from the current network.")));
        return;
    }
    refresh();
}
