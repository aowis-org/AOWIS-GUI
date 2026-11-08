#ifndef WASM_POPUP_MENU_H
#define WASM_POPUP_MENU_H

#include <QFrame>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QString>

#include <functional>

class QEnterEvent;
class QEvent;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;
class QWidget;

class WasmPopupMenu : public QFrame
{
public:
    explicit WasmPopupMenu(QWidget *owner, WasmPopupMenu *parent_menu = nullptr);
    ~WasmPopupMenu() override;

    void setDeleteOnClose(bool enabled);
    bool isPopupVisible() const;

    QPushButton *addAction(const QString &text,
                   const std::function<void()> &callback,
                   const QString &tool_tip = QString());
    WasmPopupMenu *addSubMenu(const QString &text);
    void addSeparator();
    QLabel *addSection(const QString &text);
    void addWidget(QWidget *widget, int index = -1);
    void clear();

    void popup(const QPoint &global_position);
    void popupBelow(QWidget *anchor);
    void closeAll();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;

private:
    QPointer<QWidget> owner_widget;
    WasmPopupMenu *parent_menu = nullptr;
    WasmPopupMenu *root_menu = nullptr;
    WasmPopupMenu *active_submenu = nullptr;
    QWidget *active_submenu_item = nullptr;
    QVBoxLayout *menu_layout = nullptr;
    bool delete_on_close = false;
    bool application_filter_installed = false;
    bool owner_click_exempt = false;
    QTimer *submenu_close_timer = nullptr;
    WasmPopupMenu *pending_close_submenu = nullptr;

    void configureVisuals();
    void installRootEventFilter();
    void removeRootEventFilter();
    void closeChildMenus();
    void scheduleChildMenusClose();
    void cancelPendingSubMenuClose();
    void showAtGlobalPosition(const QPoint &global_position, bool submenu);
    void openSubMenu(QWidget *item, WasmPopupMenu *submenu);
    bool containsGlobalPoint(const QPoint &global_position) const;
    bool ownerContainsGlobalPoint(const QPoint &global_position) const;
    QRect availableParentRect() const;
};

#endif // WASM_POPUP_MENU_H
