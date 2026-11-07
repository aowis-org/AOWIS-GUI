#ifndef TAB_SETTINGS_WIDGET_H
#define TAB_SETTINGS_WIDGET_H

#include "config/shortcut_registry.h"

#include <QWidget>

class KeyboardShortcutsSettingsWidget;
class MapSettingsWidget;
class UnitsSettingsWidget;
class QListWidget;
class QStackedWidget;

// Settings page: a left-hand table of contents next to a stacked content
// area, so new settings categories (Map Settings, and whatever comes after
// it) are added as their own entry rather than growing one long page.
class SettingsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsWidget(QWidget *parent = nullptr);

    void focusShortcut(GuiShortcutId id);
    void focusUnits();

protected:
    void changeEvent(QEvent *event) override;

private:
    QListWidget *table_of_contents = nullptr;
    QStackedWidget *pages = nullptr;
    KeyboardShortcutsSettingsWidget *keyboard_shortcuts_page = nullptr;
    MapSettingsWidget *map_settings_page = nullptr;
    UnitsSettingsWidget *units_settings_page = nullptr;
};

#endif // TAB_SETTINGS_WIDGET_H
