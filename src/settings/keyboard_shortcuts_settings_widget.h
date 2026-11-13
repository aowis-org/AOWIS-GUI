#ifndef KEYBOARD_SHORTCUTS_SETTINGS_WIDGET_H
#define KEYBOARD_SHORTCUTS_SETTINGS_WIDGET_H

#include "config/shortcut_registry.h"

#include <QHash>
#include <QWidget>

class QComboBox;
class QEvent;
class QLabel;
class QKeySequenceEdit;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class KeyboardShortcutsSettingsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit KeyboardShortcutsSettingsWidget(QWidget *parent = nullptr);

    void focusShortcut(GuiShortcutId id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLineEdit *shortcut_search = nullptr;
    QLabel *shortcut_status = nullptr;
    QTreeWidget *shortcut_tree = nullptr;
    QPushButton *reset_all_shortcuts = nullptr;
    QHash<int, QTreeWidgetItem *> shortcut_items;
    QHash<int, QKeySequenceEdit *> shortcut_editors;
    // Hold modifiers (globe orbit / free look): a combo of the allowed keys.
    QHash<int, QTreeWidgetItem *> hold_modifier_items;
    QHash<int, QComboBox *> hold_modifier_editors;

    void buildShortcutSettings();
    void refreshShortcutEditor(GuiShortcutId id);
    void refreshHoldModifierEditor(GuiHoldModifierId id);
    void commitHoldModifierEditor(GuiHoldModifierId id);
    void refreshShortcutFilter();
    void validateShortcutEditor(GuiShortcutId id, const QKeySequence &sequence);
    void commitShortcutEditor(GuiShortcutId id);
    void showStatus(const QString &message, bool error);
};

#endif // KEYBOARD_SHORTCUTS_SETTINGS_WIDGET_H
