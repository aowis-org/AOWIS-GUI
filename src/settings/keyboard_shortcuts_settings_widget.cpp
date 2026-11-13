#include "config/live_ui_text.h"
#include "settings/keyboard_shortcuts_settings_widget.h"

#include "config/gui_configuration.h"
#include "config/gui_config_store.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <iterator>
#include <QCoreApplication>
#include <QSignalBlocker>
#include <QHBoxLayout>

namespace
{
constexpr int shortcut_id_role = Qt::UserRole + 1;
// Rows that are not key sequences: a hold modifier (its GuiHoldModifierId)
// or a fixed mouse gesture (any value; shown, not editable).
constexpr int hold_modifier_id_role = Qt::UserRole + 2;
constexpr int fixed_gesture_role = Qt::UserRole + 3;

// A registry display name / category in the user's language (GuiShortcuts).
QString translatedShortcutName(const QString &name)
{
    return QCoreApplication::translate("GuiShortcuts", name.toUtf8().constData());
}

// The globe's fixed mouse gestures, listed with the hold modifiers so the
// page shows every globe navigation input (mouse buttons are not remappable).
struct FixedGesture
{
    const char *action;
    const char *input;
};
const FixedGesture fixed_gestures[] = {
    {QT_TRANSLATE_NOOP("GuiShortcuts", "Orbit around crosshair terrain (drag)"),
     QT_TRANSLATE_NOOP("GuiShortcuts", "Middle mouse button")},
};

QString normalizedSearchText(const QString &text)
{
    return text.trimmed().toLower();
}
}

KeyboardShortcutsSettingsWidget::KeyboardShortcutsSettingsWidget(QWidget *parent)
    : QWidget(parent)
{
    buildShortcutSettings();

    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged,
            this, [this](GuiShortcutId id)
    {
        refreshShortcutEditor(id);
        refreshShortcutFilter();
    });

    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::holdModifierChanged,
            this, [this](GuiHoldModifierId id)
    {
        refreshHoldModifierEditor(id);
        refreshShortcutFilter();
    });

    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutsReset,
            this, [this]
    {
        showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "All keyboard shortcuts were reset to their advertised defaults.")),
                   false);
    });
}

bool KeyboardShortcutsSettingsWidget::eventFilter(QObject *watched, QEvent *event)
{
    QKeySequenceEdit *editor = qobject_cast<QKeySequenceEdit *>(watched);
    if (editor != nullptr)
    {
        if (event->type() == QEvent::FocusIn)
            guiShortcutRegistry().setShortcutCaptureActive(true);
        else if (event->type() == QEvent::FocusOut)
            guiShortcutRegistry().setShortcutCaptureActive(false);
    }

    return QWidget::eventFilter(watched, event);
}

void KeyboardShortcutsSettingsWidget::buildShortcutSettings()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);

    QLabel *title = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Keyboard Shortcuts"), this);
    QFont title_font = title->font();
    title_font.setPointSize(title_font.pointSize() + 4);
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    // Saved as AOWIS-CONFIG preferences, in browsers too.
    QLabel *description = new QLabel(this);
    LiveUiText::bind(description, [description] {
        description->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Change application shortcuts here. Changes apply immediately and are saved to %1.")).arg(guiConfigStoreLocation()));
    });
    description->setWordWrap(true);
    description->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(description);

    QHBoxLayout *search_row = new QHBoxLayout();
    this->shortcut_search = new QLineEdit(this);
    this->shortcut_search->setClearButtonEnabled(true);
    LiveUiText::bind(this->shortcut_search, [this] {
        this->shortcut_search->setPlaceholderText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Search commands or shortcuts…")));
    });
    search_row->addWidget(this->shortcut_search, 1);

    this->reset_all_shortcuts = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Reset All to Defaults"), this);
    search_row->addWidget(this->reset_all_shortcuts);
    layout->addLayout(search_row);

    this->shortcut_status = new QLabel(this);
    this->shortcut_status->setWordWrap(true);
    this->shortcut_status->setMinimumHeight(this->shortcut_status->fontMetrics().height() + 6);
    layout->addWidget(this->shortcut_status);

    this->shortcut_tree = new QTreeWidget(this);
    this->shortcut_tree->setColumnCount(5);
    LiveUiText::bind(this->shortcut_tree, [this] {
        this->shortcut_tree->setHeaderLabels({LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Action")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Shortcut")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Scope")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Default")), QString()});
    });
    this->shortcut_tree->setRootIsDecorated(true);
    this->shortcut_tree->setAlternatingRowColors(true);
    this->shortcut_tree->setUniformRowHeights(false);
    this->shortcut_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    this->shortcut_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->shortcut_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    this->shortcut_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    this->shortcut_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    this->shortcut_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    this->shortcut_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(this->shortcut_tree, 1);

    QHash<QString, QTreeWidgetItem *> category_items;
    const QVector<GuiShortcutDefinition> &definitions = guiShortcutRegistry().definitions();
    for (const GuiShortcutDefinition &definition : definitions)
    {
        QTreeWidgetItem *category_item = category_items.value(definition.category, nullptr);
        if (category_item == nullptr)
        {
            category_item = new QTreeWidgetItem(this->shortcut_tree);
            category_item->setText(0, definition.category);
            QFont category_font = category_item->font(0);
            category_font.setBold(true);
            category_item->setFont(0, category_font);
            category_item->setFirstColumnSpanned(true);
            category_items.insert(definition.category, category_item);
        }

        QTreeWidgetItem *item = new QTreeWidgetItem(category_item);
        item->setText(0, definition.display_name);
        item->setText(2, definition.scope);
        item->setText(3, definition.default_shortcut);
        item->setData(0, shortcut_id_role, static_cast<int>(definition.id));
        this->shortcut_items.insert(static_cast<int>(definition.id), item);

        QKeySequenceEdit *editor = new QKeySequenceEdit(
            guiShortcutRegistry().keySequence(definition.id), this->shortcut_tree);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        editor->setMaximumSequenceLength(1);
#endif
        editor->setClearButtonEnabled(true);
        LiveUiText::bind(editor, [editor] {
        editor->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Press a key combination. Clear the field to disable this shortcut.")));
    });
        editor->installEventFilter(this);
        this->shortcut_tree->setItemWidget(item, 1, editor);
        this->shortcut_editors.insert(static_cast<int>(definition.id), editor);

        QPushButton *reset_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Reset"), this->shortcut_tree);
        LiveUiText::bind(reset_button, [reset_button, key = definition.default_shortcut] {
            reset_button->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset to %1")).arg(key));
        });
        this->shortcut_tree->setItemWidget(item, 4, reset_button);

        connect(editor, &QKeySequenceEdit::keySequenceChanged, this,
                [this, id = definition.id](const QKeySequence &sequence)
        {
            validateShortcutEditor(id, sequence);
        });
        connect(editor, &QKeySequenceEdit::editingFinished, this,
                [this, id = definition.id]
        {
            commitShortcutEditor(id);
        });
        connect(reset_button, &QPushButton::clicked, this,
                [this, id = definition.id]
        {
            QString error_message;
            if (!guiShortcutRegistry().resetShortcut(id, &error_message))
            {
                refreshShortcutEditor(id);
                showStatus(error_message, true);
                return;
            }

            const GuiShortcutDefinition *shortcut_definition = guiShortcutRegistry().definition(id);
            if (shortcut_definition != nullptr)
            {
                showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "%1 reset to %2."))
                               .arg(translatedShortcutName(shortcut_definition->display_name),
                                    shortcut_definition->default_shortcut),
                           false);
            }
        });
    }

    // Hold modifiers and the fixed mouse gestures of the globe.
    for (const GuiHoldModifierDefinition &definition : guiShortcutRegistry().holdModifierDefinitions())
    {
        QTreeWidgetItem *category_item = category_items.value(definition.category, nullptr);
        if (category_item == nullptr)
        {
            category_item = new QTreeWidgetItem(this->shortcut_tree);
            QFont category_font = category_item->font(0);
            category_font.setBold(true);
            category_item->setFont(0, category_font);
            category_item->setFirstColumnSpanned(true);
            category_items.insert(definition.category, category_item);
        }
        QTreeWidgetItem *item = new QTreeWidgetItem(category_item);
        item->setText(3, guiHoldModifierText(definition.default_modifier));
        item->setData(0, hold_modifier_id_role, static_cast<int>(definition.id));
        this->hold_modifier_items.insert(static_cast<int>(definition.id), item);

        QComboBox *editor = new QComboBox(this->shortcut_tree);
        for (const Qt::KeyboardModifier choice : GuiShortcutRegistry::holdModifierChoices())
            editor->addItem(guiHoldModifierText(choice), static_cast<int>(choice));
        LiveUiText::bind(editor, [editor] {
            editor->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Hold this key and move the mouse (no button pressed) over the 3D map. Shift is reserved for faster keyboard panning.")));
        });
        this->shortcut_tree->setItemWidget(item, 1, editor);
        this->hold_modifier_editors.insert(static_cast<int>(definition.id), editor);
        refreshHoldModifierEditor(definition.id);

        QPushButton *reset_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Reset"), this->shortcut_tree);
        LiveUiText::bind(reset_button, [reset_button, key = guiHoldModifierText(definition.default_modifier)] {
            reset_button->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset to %1")).arg(key));
        });
        this->shortcut_tree->setItemWidget(item, 4, reset_button);

        connect(editor, &QComboBox::activated, this,
                [this, id = definition.id](int) { commitHoldModifierEditor(id); });
        connect(reset_button, &QPushButton::clicked, this, [this, id = definition.id]
        {
            QString error_message;
            if (!guiShortcutRegistry().resetHoldModifier(id, &error_message))
            {
                refreshHoldModifierEditor(id);
                showStatus(error_message, true);
                return;
            }
            refreshHoldModifierEditor(id);
            const GuiHoldModifierDefinition *hold_definition =
                guiShortcutRegistry().holdModifierDefinition(id);
            if (hold_definition != nullptr)
            {
                showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "%1 reset to %2."))
                               .arg(translatedShortcutName(hold_definition->display_name),
                                    guiHoldModifierText(hold_definition->default_modifier)),
                           false);
            }
        });
    }
    {
        const GuiHoldModifierDefinition *first_hold =
            guiShortcutRegistry().holdModifierDefinitions().isEmpty()
            ? nullptr : &guiShortcutRegistry().holdModifierDefinitions().first();
        QTreeWidgetItem *category_item = first_hold != nullptr
            ? category_items.value(first_hold->category, nullptr) : nullptr;
        if (category_item != nullptr)
        {
            // Texts are set (and retranslated) by the LiveUiText binding below.
            for (std::size_t index = 0; index < std::size(fixed_gestures); ++index)
            {
                QTreeWidgetItem *item = new QTreeWidgetItem(category_item);
                item->setData(0, fixed_gesture_role, true);
                item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            }
        }
    }

    LiveUiText::bind(this->shortcut_tree, [this] {
        for (const GuiHoldModifierDefinition &definition : guiShortcutRegistry().holdModifierDefinitions())
        {
            QTreeWidgetItem *item = this->hold_modifier_items.value(static_cast<int>(definition.id));
            item->setText(0, QCoreApplication::translate("GuiShortcuts", definition.display_name.toUtf8().constData()));
            item->setText(2, QCoreApplication::translate("GuiShortcuts", definition.scope.toUtf8().constData()));
            item->parent()->setText(0, QCoreApplication::translate("GuiShortcuts", definition.category.toUtf8().constData()));
            // The fixed gestures of this category, in fixed_gestures order.
            int fixed_index = 0;
            for (int child = 0; child < item->parent()->childCount(); ++child)
            {
                QTreeWidgetItem *fixed = item->parent()->child(child);
                if (!fixed->data(0, fixed_gesture_role).toBool()
                    || fixed_index >= int(std::size(fixed_gestures)))
                    continue;
                const FixedGesture &gesture = fixed_gestures[fixed_index++];
                fixed->setText(0, QCoreApplication::translate("GuiShortcuts", gesture.action));
                fixed->setText(1, QCoreApplication::translate("GuiShortcuts", gesture.input));
                fixed->setText(2, QCoreApplication::translate("GuiShortcuts", definition.scope.toUtf8().constData()));
                fixed->setText(3, QCoreApplication::translate("GuiShortcuts", gesture.input));
            }
        }
        for (const GuiShortcutDefinition &definition : guiShortcutRegistry().definitions())
        {
            QTreeWidgetItem *item = this->shortcut_items.value(static_cast<int>(definition.id));
            item->setText(0, QCoreApplication::translate("GuiShortcuts", definition.display_name.toUtf8().constData()));
            item->setText(2, QCoreApplication::translate("GuiShortcuts", definition.scope.toUtf8().constData()));
            item->parent()->setText(0, QCoreApplication::translate("GuiShortcuts", definition.category.toUtf8().constData()));
        }
        refreshShortcutFilter();
    });
    this->shortcut_tree->expandAll();

    connect(this->shortcut_search, &QLineEdit::textChanged,
            this, [this]
    {
        refreshShortcutFilter();
    });

    connect(this->reset_all_shortcuts, &QPushButton::clicked,
            this, [this]
    {
        QMessageBox::StandardButton result = QMessageBox::question(
            this,
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset Keyboard Shortcuts")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset all keyboard shortcuts to their advertised defaults?")),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (result == QMessageBox::Yes)
            guiShortcutRegistry().resetAllShortcuts();
    });
}

void KeyboardShortcutsSettingsWidget::focusShortcut(GuiShortcutId id)
{
    this->shortcut_search->clear();
    QTreeWidgetItem *item = this->shortcut_items.value(static_cast<int>(id), nullptr);
    QKeySequenceEdit *editor = this->shortcut_editors.value(static_cast<int>(id), nullptr);
    if (item == nullptr || editor == nullptr)
        return;

    QTreeWidgetItem *category_item = item->parent();
    if (category_item != nullptr)
        category_item->setExpanded(true);

    this->shortcut_tree->setCurrentItem(item);
    this->shortcut_tree->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    editor->setFocus(Qt::ShortcutFocusReason);

    const GuiShortcutDefinition *definition = guiShortcutRegistry().definition(id);
    if (definition != nullptr)
        showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Editing keyboard shortcut for %1."))
                       .arg(translatedShortcutName(definition->display_name)), false);
}

void KeyboardShortcutsSettingsWidget::refreshShortcutEditor(GuiShortcutId id)
{
    QKeySequenceEdit *editor = this->shortcut_editors.value(static_cast<int>(id), nullptr);
    if (editor == nullptr)
        return;

    const QKeySequence desired = guiShortcutRegistry().keySequence(id);
    if (editor->keySequence() != desired)
        editor->setKeySequence(desired);
}

void KeyboardShortcutsSettingsWidget::refreshShortcutFilter()
{
    const QString search = normalizedSearchText(this->shortcut_search->text());
    for (int category_index = 0; category_index < this->shortcut_tree->topLevelItemCount(); ++category_index)
    {
        QTreeWidgetItem *category_item = this->shortcut_tree->topLevelItem(category_index);
        bool category_visible = false;
        for (int item_index = 0; item_index < category_item->childCount(); ++item_index)
        {
            QTreeWidgetItem *item = category_item->child(item_index);
            if (item->data(0, hold_modifier_id_role).isValid()
                || item->data(0, fixed_gesture_role).isValid())
            {
                // Hold modifier / fixed gesture rows: their visible texts.
                QString input = item->text(1);
                const QVariant hold_id = item->data(0, hold_modifier_id_role);
                if (hold_id.isValid())
                    input = guiHoldModifierText(guiShortcutRegistry().holdModifier(
                        static_cast<GuiHoldModifierId>(hold_id.toInt())));
                const QString searchable = (item->text(0) + QLatin1Char(' ') + item->text(2)
                                            + QLatin1Char(' ') + category_item->text(0)
                                            + QLatin1Char(' ') + input + QLatin1Char(' ')
                                            + item->text(3)).toLower();
                const bool visible = search.isEmpty() || searchable.contains(search);
                item->setHidden(!visible);
                category_visible = category_visible || visible;
                continue;
            }
            const GuiShortcutId id = static_cast<GuiShortcutId>(
                item->data(0, shortcut_id_role).toInt());
            const GuiShortcutDefinition *definition = guiShortcutRegistry().definition(id);
            if (definition == nullptr)
                continue;

            const QString searchable = (item->text(0) + QLatin1Char(' ') + item->text(2)
                                        + QLatin1Char(' ') + category_item->text(0) + QLatin1Char(' ')
                                        + definition->display_name + QLatin1Char(' ')
                                        + definition->category + QLatin1Char(' ')
                                        + definition->scope + QLatin1Char(' ')
                                        + guiShortcutRegistry().shortcut(id) + QLatin1Char(' ')
                                        + definition->default_shortcut).toLower();
            const bool visible = search.isEmpty() || searchable.contains(search);
            item->setHidden(!visible);
            category_visible = category_visible || visible;
        }
        category_item->setHidden(!category_visible);
    }
}

void KeyboardShortcutsSettingsWidget::validateShortcutEditor(GuiShortcutId id, const QKeySequence &sequence)
{
    const QString conflict = guiShortcutRegistry().conflictDisplayName(id, sequence);
    if (!conflict.isEmpty())
    {
        showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Conflict: this shortcut is already assigned to \"%1\"."))
                       .arg(translatedShortcutName(conflict)), true);
        return;
    }

    const GuiShortcutDefinition *definition = guiShortcutRegistry().definition(id);
    if (definition == nullptr)
        return;

    if (sequence.isEmpty())
        showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "%1 will have no keyboard shortcut."))
                       .arg(translatedShortcutName(definition->display_name)), false);
    else
        showStatus(QStringLiteral("%1 → %2").arg(translatedShortcutName(definition->display_name),
                                                  guiShortcutDisplayText(sequence)), false);
}

void KeyboardShortcutsSettingsWidget::commitShortcutEditor(GuiShortcutId id)
{
    QKeySequenceEdit *editor = this->shortcut_editors.value(static_cast<int>(id), nullptr);
    if (editor == nullptr)
        return;

    QString error_message;
    if (!guiShortcutRegistry().setShortcut(id, editor->keySequence(), &error_message))
    {
        refreshShortcutEditor(id);
        showStatus(error_message, true);
        return;
    }

    const GuiShortcutDefinition *definition = guiShortcutRegistry().definition(id);
    if (definition == nullptr)
        return;

    const QString value = guiShortcutRegistry().shortcut(id);
    if (value.isEmpty())
        showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "%1 shortcut disabled."))
                       .arg(translatedShortcutName(definition->display_name)), false);
    else
        showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "%1 set to %2."))
                       .arg(translatedShortcutName(definition->display_name), value), false);
}

void KeyboardShortcutsSettingsWidget::refreshHoldModifierEditor(GuiHoldModifierId id)
{
    QComboBox *editor = this->hold_modifier_editors.value(static_cast<int>(id), nullptr);
    if (editor == nullptr)
        return;
    const int index = editor->findData(static_cast<int>(guiShortcutRegistry().holdModifier(id)));
    if (index >= 0 && editor->currentIndex() != index)
    {
        const QSignalBlocker blocker(editor);
        editor->setCurrentIndex(index);
    }
}

void KeyboardShortcutsSettingsWidget::commitHoldModifierEditor(GuiHoldModifierId id)
{
    QComboBox *editor = this->hold_modifier_editors.value(static_cast<int>(id), nullptr);
    if (editor == nullptr)
        return;
    const Qt::KeyboardModifier modifier =
        static_cast<Qt::KeyboardModifier>(editor->currentData().toInt());
    QString error_message;
    if (!guiShortcutRegistry().setHoldModifier(id, modifier, &error_message))
    {
        // Back to the stored key (conflicts are not saved).
        refreshHoldModifierEditor(id);
        showStatus(error_message, true);
        return;
    }
    const GuiHoldModifierDefinition *definition = guiShortcutRegistry().holdModifierDefinition(id);
    if (definition != nullptr)
    {
        showStatus(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "%1 set to %2."))
                       .arg(translatedShortcutName(definition->display_name),
                            guiHoldModifierText(modifier)),
                   false);
    }
}

void KeyboardShortcutsSettingsWidget::showStatus(const QString &message, bool error)
{
    this->shortcut_status->setText(message);
    QPalette palette = this->shortcut_status->palette();
    palette.setColor(QPalette::WindowText,
                     error ? palette.color(QPalette::BrightText)
                           : this->palette().color(QPalette::Text));
    if (error)
    {
        QColor color = this->palette().color(QPalette::Text);
        if (color.lightness() > 128)
            color = QColor(255, 120, 120);
        else
            color = QColor(180, 20, 20);
        palette.setColor(QPalette::WindowText, color);
    }
    this->shortcut_status->setPalette(palette);
}
