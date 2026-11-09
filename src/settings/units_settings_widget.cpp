#include "config/live_ui_text.h"
#include "settings/units_settings_widget.h"
#include "config/unit_presented_quantities.h"
#include "config/unit_profile_manager.h"
#include "config/unit_display_text.h"
#include "config/unit_profile_menu.h"
#include "config/unit_profile_notifications.h"
#include <aowis/model/units/unit_profile.h>
#include <QComboBox>
#include <QToolButton>
#include <QSizePolicy>
#include <QMenu>
#include <QAction>
#ifdef Q_OS_WASM
#include "widgets/wasm_popup_menu.h"
#endif
#include <QFormLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QPointer>
#include <QLineEdit>
#include <QFont>
#include <QLayoutItem>
#include <QStringList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QTimer>
#include <functional>

namespace {
using UnitField = aowis::units::UnitField;
using Profile = UnitProfileManager::Profile;
QString asQString(std::string_view value);
const QStringList flowTemplates = [] {
    QStringList names;
    for (const std::string_view value : aowis::units::flow_template_names) names.append(asQString(value));
    return names;
}();
QString asQString(std::string_view value)
{
    return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
}

#ifndef Q_OS_WASM
void updateMenuChecks(QMenu *menu, int index)
{
    for (QAction *action : menu->actions()) {
        if (action->menu()) updateMenuChecks(action->menu(), index);
        else if (action->isCheckable()) action->setChecked(action->data().toInt() == index);
    }
}
#endif

}

UnitsSettingsWidget::UnitsSettingsWidget(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 20, 24, 20);
    QLabel *title = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Unit Profiles"), this);
    QFont font = title->font(); font.setPointSize(font.pointSize() + 2); font.setBold(true);
    title->setFont(font);
    outer->addWidget(title);
    QLabel *info = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Display-unit preferences only. Changes update unit-aware views and editors; canonical Model values remain unchanged."), this);
    info->setWordWrap(true);
    outer->addWidget(info);
    QHBoxLayout *controls = new QHBoxLayout;
    this->profile_selector = new QComboBox(this);
    this->profile_selector->hide();
    this->profile_button = new QToolButton(this);
    this->profile_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    this->profile_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    controls->addWidget(this->profile_button, 1);
#ifdef Q_OS_WASM
    this->profile_menu = new WasmPopupMenu(this->profile_button);
    connect(this->profile_button, &QToolButton::clicked, this, [this] {
        if (this->profile_menu->isPopupVisible()) this->profile_menu->closeAll();
        else this->profile_menu->popupBelow(this->profile_button);
    });
#else
    this->profile_button->setPopupMode(QToolButton::InstantPopup);
    this->profile_menu = new QMenu(this->profile_button);
    this->profile_button->setMenu(this->profile_menu);
#endif
    QPushButton *create_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "New custom profile..."), this);
    controls->addWidget(create_button);
    this->rename_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Rename..."), this);
    controls->addWidget(this->rename_button);
    this->delete_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Delete"), this);
    controls->addWidget(this->delete_button);
    outer->addLayout(controls);
    this->field_search = new QLineEdit(this);
    this->field_search->setClearButtonEnabled(true);
    LiveUiText::bind(this->field_search, [this] {
        this->field_search->setPlaceholderText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Search quantities or units…")));
    });
    outer->addWidget(this->field_search);

    this->fields_tree = new QTreeWidget(this);
    this->fields_tree->setColumnCount(3);
    LiveUiText::bind(this->fields_tree, [this] {
        this->fields_tree->setHeaderLabels({LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Quantity")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Display unit")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Canonical unit"))});
    });
    this->fields_tree->setRootIsDecorated(true);
    this->fields_tree->setAlternatingRowColors(true);
    this->fields_tree->setUniformRowHeights(false);
    this->fields_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    this->fields_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->fields_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    this->fields_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    this->fields_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    outer->addWidget(this->fields_tree, 1);
    connect(this->field_search, &QLineEdit::textChanged, this,
            [this](const QString &) { this->filterFields(); });
    this->load();
    connect(this->profile_selector, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { this->refreshFields(); this->save(); });
    connect(create_button, &QPushButton::clicked, this, [this] { this->createProfile(); });
    connect(this->rename_button, &QPushButton::clicked, this, [this] { this->renameProfile(); });
    connect(this->delete_button, &QPushButton::clicked, this, [this] {
        const int index = this->profile_selector->currentIndex();
        if (index < 0 || index >= UnitProfileManager::instance().profiles().size() || UnitProfileManager::instance().profiles().at(index).builtin) return;

        const QString profile_name = UnitProfileManager::instance().profiles().at(index).name;
        const QString profile_id = UnitProfileManager::instance().profiles().at(index).id;
        QMessageBox *confirmation = new QMessageBox(
            QMessageBox::Question,
            QStringLiteral("Delete unit profile"),
            QStringLiteral("Delete the custom unit profile \"%1\"?\nThis cannot be undone.").arg(profile_name),
            QMessageBox::Yes | QMessageBox::No,
            this);
        confirmation->setAttribute(Qt::WA_DeleteOnClose);
        LiveUiText::bind(confirmation, [confirmation, profile_name] {
            confirmation->setWindowTitle(LiveUiText::translate(
                QT_TRANSLATE_NOOP("LiveUiText", "Delete unit profile")));
            confirmation->setText(LiveUiText::translate(
                QT_TRANSLATE_NOOP("LiveUiText", "Delete the custom unit profile \"%1\"?\nThis cannot be undone."))
                .arg(profile_name));
        });
        confirmation->setDefaultButton(QMessageBox::No);
        confirmation->setEscapeButton(QMessageBox::No);
        connect(confirmation, &QMessageBox::finished, this, [this, profile_id](int result) {
            if (result != QMessageBox::Yes) return;
            // Resolve the profile again: the selection or profile list may have changed
            // while this nonblocking dialog was open.
            for (int i = 0; i < UnitProfileManager::instance().profiles().size(); ++i) {
                if (UnitProfileManager::instance().profiles().at(i).builtin || UnitProfileManager::instance().profiles().at(i).id != profile_id) continue;
                (void)UnitProfileManager::instance().remove(i, this);
                this->refreshSelector(UnitProfileManager::instance().activeIndex());
                break;
            }
        });
        confirmation->open();
    });
    LiveUiText::bind(this, [this] { refreshLanguage(); });
}

void UnitsSettingsWidget::load()
{
    this->refreshSelector(UnitProfileManager::instance().activeIndex());
    UnitProfileNotifications::instance().subscribe(this, [this] {
        QTimer::singleShot(0, this, [this] {
            this->refreshSelector(UnitProfileManager::instance().activeIndex());
        });
    });
}
void UnitsSettingsWidget::save() const
{
    (void)UnitProfileManager::instance().select(this->profile_selector->currentIndex(), const_cast<UnitsSettingsWidget *>(this));
}
void UnitsSettingsWidget::refreshSelector(int selected)
{
    QSignalBlocker blocker(this->profile_selector);
    this->profile_selector->clear();
    for (const Profile &profile : UnitProfileManager::instance().profiles())
        this->profile_selector->addItem(UnitProfileMenu::label(profile));
    this->profile_selector->setCurrentIndex(selected);
    this->profile_button->setText(UnitProfileMenu::label(UnitProfileManager::instance().profiles().at(selected)) + QStringLiteral("  ▾"));
    UnitProfileMenu::populate(this->profile_menu, this,
                              UnitProfileManager::instance().profiles(), selected,
                              [this](int index) { this->profile_selector->setCurrentIndex(index); });
    this->refreshFields();
}
void UnitsSettingsWidget::refreshLanguage()
{
    const QVector<Profile> &profiles = UnitProfileManager::instance().profiles();
    const QSignalBlocker blocker(this->profile_selector);
    for (int index = 0; index < this->profile_selector->count() && index < profiles.size(); ++index)
        this->profile_selector->setItemText(index, UnitProfileMenu::label(profiles.at(index)));
    const int index = this->profile_selector->currentIndex();
    if (index >= 0 && index < profiles.size())
        this->profile_button->setText(UnitProfileMenu::label(profiles.at(index))
                                      + QStringLiteral("  ▾"));
    UnitProfileMenu::retranslate(this->profile_menu, profiles);
    // Deliberately leave the unit editors, selected IDs, search draft and table
    // positions untouched. Only profile menu presentation is refreshed here.
}

void UnitsSettingsWidget::refreshFields()
{
    this->fields_tree->clear();
    const int index = this->profile_selector->currentIndex();
    if (index < 0 || index >= UnitProfileManager::instance().profiles().size()) return;
    const bool builtin = UnitProfileManager::instance().profiles().at(index).builtin;
    this->delete_button->setEnabled(!builtin);
    this->rename_button->setEnabled(!builtin);
    this->profile_button->setText(UnitProfileMenu::label(UnitProfileManager::instance().profiles().at(index)) + QStringLiteral("  ▾"));
    // Refresh checked menu entries without rebuilding menus while a selection is in progress.
#ifndef Q_OS_WASM
    updateMenuChecks(this->profile_menu, index);
#endif
    QString last_group;
    QTreeWidgetItem *category_item = nullptr;
    // Keep dimensionless quantities and percentages at the bottom of the view.
    // This changes presentation order only; the Model registry remains authoritative.
    for (int pass = 0; pass < 2; ++pass) {
        last_group.clear();
        category_item = nullptr;
        for (const UnitField &field : aowis::units::unit_fields) {
            if (!UnitPresentedQuantities::isPresented(field)) continue;
            const bool bottom_group = field.group == "Dimensionless" || field.group == "Percentages";
            if (bottom_group != (pass == 1)) continue;
            const QString group = asQString(field.group);
            if (group != last_group) {
                category_item = new QTreeWidgetItem(this->fields_tree);
                category_item->setText(0, group);
                QFont category_font = category_item->font(0);
                category_font.setBold(true);
                category_item->setFont(0, category_font);
                category_item->setFirstColumnSpanned(true);
                last_group = group;
            }
            const QString key = asQString(field.key);
            QTreeWidgetItem *item = new QTreeWidgetItem(category_item);
            item->setText(0, UnitDisplayText::quantity(field.key));
            item->setData(0, Qt::UserRole, key);
            const QString canonical_id = asQString(aowis::units::canonicalUnit(field.key));
            item->setText(2, UnitDisplayText::symbol(canonical_id));
            QComboBox *selector = new QComboBox(this->fields_tree);
            for (const QString &unit_id : asQString(field.choices).split(QLatin1Char(';')))
                selector->addItem(UnitDisplayText::symbol(unit_id), unit_id);
            const QString unit_id = UnitProfileManager::instance().profiles().at(index).units.value(key).toString();
            selector->setCurrentIndex(qMax(0, selector->findData(unit_id)));
            selector->setEnabled(!builtin && selector->count() > 1);
            LiveUiText::bind(selector, [selector, canonical_id] {
                selector->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Canonical: %1")).arg(UnitDisplayText::symbol(canonical_id)));
            });
            this->fields_tree->setItemWidget(item, 1, selector);
            connect(selector, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this, index, key, selector](int) {
                const QString selected_id = selector->currentData().toString();
                if (!aowis::units::isAllowedUnit(key.toStdString(), selected_id.toStdString())) return;
                if (!UnitProfileManager::instance().setUnit(index, key, selected_id, this)) return;
                // Notifications exclude their origin. Synchronize the existing
                // controls with resolved profile units, including inherited
                // demand and tank units, without creating explicit overrides.
                const QJsonObject &resolved_units = UnitProfileManager::instance().profiles().at(index).units;
                for (int group_index = 0; group_index < this->fields_tree->topLevelItemCount(); ++group_index) {
                    QTreeWidgetItem *group = this->fields_tree->topLevelItem(group_index);
                    for (int field_index = 0; field_index < group->childCount(); ++field_index) {
                        QTreeWidgetItem *field_item = group->child(field_index);
                        QComboBox *field_selector = qobject_cast<QComboBox *>(
                            this->fields_tree->itemWidget(field_item, 1));
                        if (!field_selector) continue;
                        const QString field_key = field_item->data(0, Qt::UserRole).toString();
                        const int selected_index = field_selector->findData(
                            resolved_units.value(field_key).toString());
                        if (selected_index < 0 || selected_index == field_selector->currentIndex()) continue;
                        const QSignalBlocker blocker(field_selector);
                        field_selector->setCurrentIndex(selected_index);
                    }
                }
            });
        }
    }
    this->fields_tree->expandAll();
    this->filterFields();
}
void UnitsSettingsWidget::filterFields()
{
    const QString query = this->field_search->text().trimmed();
    for (int i = 0; i < this->fields_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *category = this->fields_tree->topLevelItem(i);
        const bool category_match = category->text(0).contains(query, Qt::CaseInsensitive);
        bool visible_child = false;
        for (int j = 0; j < category->childCount(); ++j) {
            QTreeWidgetItem *item = category->child(j);
            QComboBox *selector = qobject_cast<QComboBox *>(this->fields_tree->itemWidget(item, 1));
            QString available_units;
            if (selector != nullptr) {
                for (int k = 0; k < selector->count(); ++k)
                    available_units += selector->itemText(k) + QLatin1Char(' ');
            }
            const bool match = query.isEmpty() || category_match
                || item->text(0).contains(query, Qt::CaseInsensitive)
                || item->data(0, Qt::UserRole).toString().contains(query, Qt::CaseInsensitive)
                || item->text(2).contains(query, Qt::CaseInsensitive)
                || available_units.contains(query, Qt::CaseInsensitive);
            item->setHidden(!match);
            visible_child |= match;
        }
        category->setHidden(!visible_child);
    }
}

void UnitsSettingsWidget::createProfile()
{
    QDialog *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    LiveUiText::bind(dialog, [dialog] { dialog->setWindowTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "New unit profile"))); });
    dialog->setMinimumWidth(440);
    QVBoxLayout *layout = new QVBoxLayout(dialog);
    layout->addWidget(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Start from profile:"), dialog));
    QToolButton *base_button = new QToolButton(dialog);
    base_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(base_button);
    dialog->setProperty("sourceIndex", qMax(0, this->profile_selector->currentIndex()));
    const int source = dialog->property("sourceIndex").toInt();
    base_button->setText(UnitProfileMenu::label(UnitProfileManager::instance().profiles().at(source)) + QStringLiteral("  ▾"));
    QLineEdit *name_edit = new QLineEdit(dialog);
    const std::function<QString(int)> suggestedName = [](int index) {
        return UnitProfileManager::instance().profiles().at(index).name + QStringLiteral(" (custom)");
    };
    name_edit->setText(suggestedName(source));
    const std::function<void(int)> selectBase =
        [dialog, base_button, name_edit, suggestedName](int index) {
        const QString previousSuggestion = suggestedName(dialog->property("sourceIndex").toInt());
        const bool useSuggestion = name_edit->text() == previousSuggestion;
        dialog->setProperty("sourceIndex", index);
        base_button->setText(UnitProfileMenu::label(UnitProfileManager::instance().profiles().at(index)) + QStringLiteral("  ▾"));
        if (useSuggestion) name_edit->setText(suggestedName(index));
    };
#ifdef Q_OS_WASM
    WasmPopupMenu *base_menu = new WasmPopupMenu(base_button);
    connect(base_button, &QToolButton::clicked, dialog, [base_button, base_menu] {
        if (base_menu->isPopupVisible()) base_menu->closeAll();
        else base_menu->popupBelow(base_button);
    });
#else
    base_button->setPopupMode(QToolButton::InstantPopup);
    QMenu *base_menu = new QMenu(base_button);
    base_button->setMenu(base_menu);
#endif
    UnitProfileMenu::populate(base_menu, dialog,
                              UnitProfileManager::instance().profiles(), -1, selectBase);
    LiveUiText::bind(base_button, [dialog, base_button, base_menu] {
        const QVector<Profile> &profiles = UnitProfileManager::instance().profiles();
        const int index = dialog->property("sourceIndex").toInt();
        if (index >= 0 && index < profiles.size())
            base_button->setText(UnitProfileMenu::label(profiles.at(index)) + QStringLiteral("  ▾"));
        UnitProfileMenu::retranslate(base_menu, profiles);
    });
    layout->addWidget(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Profile name:"), dialog));
    layout->addWidget(name_edit);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [this, dialog, name_edit] {
    const int source = dialog->property("sourceIndex").toInt();
    if (source < 0 || source >= UnitProfileManager::instance().profiles().size()) return;
    const QString name = name_edit->text().trimmed();
    if (name.isEmpty()) return;
    for (const Profile &profile : UnitProfileManager::instance().profiles()) {
        if (profile.name == name) {
            QMessageBox *message = new QMessageBox(QMessageBox::Warning, QStringLiteral("Unit profiles"),
                QStringLiteral("A profile with this name already exists."), QMessageBox::Ok, this);
            message->setAttribute(Qt::WA_DeleteOnClose);
            LiveUiText::bind(message, [message] {
                message->setWindowTitle(LiveUiText::translate(
                    QT_TRANSLATE_NOOP("LiveUiText", "Unit profiles")));
                message->setText(LiveUiText::translate(
                    QT_TRANSLATE_NOOP("LiveUiText", "A profile with this name already exists.")));
            });
            message->open();
            return;
        }
    }
    if (UnitProfileManager::instance().create(source, name, this))
        this->refreshSelector(UnitProfileManager::instance().activeIndex());
    });
    dialog->open();
}

void UnitsSettingsWidget::renameProfile()
{
    const int index = this->profile_selector->currentIndex();
    if (index < 0 || index >= UnitProfileManager::instance().profiles().size() || UnitProfileManager::instance().profiles().at(index).builtin) return;
    QInputDialog *dialog = new QInputDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    LiveUiText::bind(dialog, [dialog] { dialog->setWindowTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Rename unit profile"))); });
    LiveUiText::bind(dialog, [dialog] { dialog->setLabelText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Profile name:"))); });
    dialog->setInputMode(QInputDialog::TextInput);
    dialog->setTextValue(UnitProfileManager::instance().profiles().at(index).name);
    const QString profile_id = UnitProfileManager::instance().profiles().at(index).id;
    connect(dialog, &QDialog::accepted, this, [this, dialog, profile_id] {
        // A different profile may occupy the original index if the list
        // changed while this nonblocking dialog was open.
        int index = -1;
        for (int i = 0; i < UnitProfileManager::instance().profiles().size(); ++i) {
            const Profile &profile = UnitProfileManager::instance().profiles().at(i);
            if (profile.id == profile_id && !profile.builtin) {
                index = i;
                break;
            }
        }
        if (index < 0) return;
        const QString name = dialog->textValue().trimmed();
        if (name.isEmpty() || name == UnitProfileManager::instance().profiles().at(index).name) return;
        for (int i = 0; i < UnitProfileManager::instance().profiles().size(); ++i) {
            if (i != index && UnitProfileManager::instance().profiles().at(i).name == name) {
                QMessageBox *message = new QMessageBox(QMessageBox::Warning, QStringLiteral("Unit profiles"),
                    QStringLiteral("A profile with this name already exists."), QMessageBox::Ok, this);
                message->setAttribute(Qt::WA_DeleteOnClose);
                LiveUiText::bind(message, [message] {
                    message->setWindowTitle(LiveUiText::translate(
                        QT_TRANSLATE_NOOP("LiveUiText", "Unit profiles")));
                    message->setText(LiveUiText::translate(
                        QT_TRANSLATE_NOOP("LiveUiText", "A profile with this name already exists.")));
                });
                message->open();
                return;
            }
        }
        if (UnitProfileManager::instance().rename(index, name, this))
            this->refreshSelector(UnitProfileManager::instance().activeIndex());
    });
    dialog->open();
}
