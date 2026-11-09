#ifndef UNITS_SETTINGS_WIDGET_H
#define UNITS_SETTINGS_WIDGET_H
#include <QWidget>
#include <QJsonObject>
#include <QVector>
class QComboBox;
class QToolButton;
class QMenu;
class WasmPopupMenu;
class QLabel;
class QPushButton;
class QTreeWidget;
class QLineEdit;
class UnitsSettingsWidget : public QWidget
{
public:
    explicit UnitsSettingsWidget(QWidget *parent = nullptr);
private:
    QComboBox *profile_selector = nullptr;
    QToolButton *profile_button = nullptr;
#ifdef Q_OS_WASM
    WasmPopupMenu *profile_menu = nullptr;
#else
    QMenu *profile_menu = nullptr;
#endif
    QPushButton *rename_button = nullptr;
    QPushButton *delete_button = nullptr;
    QTreeWidget *fields_tree = nullptr;
    QLineEdit *field_search = nullptr;
    bool rebuilding = false;
    void load();
    void save() const;
    void refreshSelector(int selected);
    void refreshLanguage();
    void refreshFields();
    void filterFields();
    void createProfile();
    void renameProfile();
};
#endif
