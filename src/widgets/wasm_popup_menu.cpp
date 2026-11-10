#include "widgets/wasm_popup_menu.h"

#include <QApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLayoutItem>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QSizePolicy>
#include <QTimer>
#include <QTouchEvent>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
constexpr int MenuMinimumWidth = 180;
constexpr int MenuItemMinimumHeight = 34;
constexpr int MenuMargin = 2;
constexpr int MenuScreenMargin = 4;
constexpr int SubMenuCloseDelayMs = 350;
constexpr int SubMenuOverlapPx = 3;

QString menuItemStyleSheet()
{
    return QStringLiteral(
        "QPushButton {"
        "  text-align: left;"
        "  border: 0;"
        "  padding: 5px 12px;"
        "  background: transparent;"
        "}"
        "QPushButton:hover {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}"
        "QPushButton:pressed {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}");
}

QPoint globalInputPosition(QEvent *event, bool *valid)
{
    if (valid != nullptr)
        *valid = false;

    if (event == nullptr)
        return QPoint();

    if (event->type() == QEvent::MouseButtonPress)
    {
        QMouseEvent *mouse_event = static_cast<QMouseEvent *>(event);
        if (valid != nullptr)
            *valid = true;
        return mouse_event->globalPosition().toPoint();
    }

    if (event->type() == QEvent::TouchBegin)
    {
        QTouchEvent *touch_event = static_cast<QTouchEvent *>(event);
        const QList<QEventPoint> points = touch_event->points();
        if (points.isEmpty())
            return QPoint();
        if (valid != nullptr)
            *valid = true;
        return points.first().globalPosition().toPoint();
    }

    return QPoint();
}
}

WasmPopupMenu::WasmPopupMenu(QWidget *owner, WasmPopupMenu *parent_menu)
    : QFrame(owner != nullptr ? owner->window() : nullptr),
      owner_widget(owner),
      parent_menu(parent_menu),
      root_menu(parent_menu != nullptr ? parent_menu->root_menu : this),
      menu_layout(new QVBoxLayout(this)),
      submenu_close_timer(new QTimer(this))
{
#ifdef Q_OS_WASM
    // WebAssembly renders the bgfx map into its own browser canvas layered
    // above the ordinary Qt canvas. Popups that stay inside the main Qt
    // window will therefore render beneath the map. Make the menu a real Qt
    // popup window so the browser-side z-index stacking can place it above the
    // bgfx canvas like any other transient top-level Qt window.
    setWindowFlag(Qt::Popup, true);
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowFlag(Qt::NoDropShadowWindowHint, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
#endif
    this->submenu_close_timer->setSingleShot(true);
    this->submenu_close_timer->setInterval(SubMenuCloseDelayMs);
    connect(this->submenu_close_timer, &QTimer::timeout, this, [this]
    {
        if (this->active_submenu != nullptr
            && this->pending_close_submenu == this->active_submenu)
        {
            this->active_submenu->closeChildMenus();
            this->active_submenu->hide();
            this->active_submenu = nullptr;
            this->active_submenu_item = nullptr;
        }
        this->pending_close_submenu = nullptr;
    });

    this->menu_layout->setContentsMargins(MenuMargin, MenuMargin, MenuMargin, MenuMargin);
    this->menu_layout->setSpacing(0);
    configureVisuals();
    hide();
}

WasmPopupMenu::~WasmPopupMenu()
{
    if (this == this->root_menu)
    {
        removeRootEventFilter();
        if (this->owner_widget != nullptr)
        {
            const quintptr active_value = this->owner_widget->property(
                "aowis_active_wasm_popup").value<quintptr>();
            if (reinterpret_cast<WasmPopupMenu *>(active_value) == this)
                this->owner_widget->setProperty("aowis_active_wasm_popup", QVariant());
        }
    }
}

void WasmPopupMenu::setDeleteOnClose(bool enabled)
{
    this->delete_on_close = enabled;
}

bool WasmPopupMenu::isPopupVisible() const
{
    return isVisible();
}

QPushButton *WasmPopupMenu::addAction(const QString &text,
                              const std::function<void()> &callback,
                              const QString &tool_tip)
{
    QPushButton *button = new QPushButton(text, this);
    button->setFlat(true);
    button->setMinimumHeight(MenuItemMinimumHeight);
    button->setMinimumWidth(MenuMinimumWidth - MenuMargin * 2);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(menuItemStyleSheet());
    button->setToolTip(tool_tip);
    button->installEventFilter(this);

    connect(button, &QPushButton::clicked, this, [this, callback]
    {
        if (callback)
            callback();
        closeAll();
    });

    this->menu_layout->addWidget(button);
    return button;
}

WasmPopupMenu *WasmPopupMenu::addSubMenu(const QString &text)
{
    QPushButton *button = new QPushButton(text + QStringLiteral("    ›"), this);
    button->setFlat(true);
    button->setMinimumHeight(MenuItemMinimumHeight);
    button->setMinimumWidth(MenuMinimumWidth - MenuMargin * 2);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(menuItemStyleSheet());

    WasmPopupMenu *submenu = new WasmPopupMenu(this->owner_widget, this);
    connect(this->root_menu, &QObject::destroyed, submenu, &QObject::deleteLater);
    button->setProperty("aowis_wasm_submenu", QVariant::fromValue<quintptr>(
        reinterpret_cast<quintptr>(submenu)));
    button->installEventFilter(this);

    connect(button, &QPushButton::clicked, this, [this, button, submenu]
    {
        openSubMenu(button, submenu);
    });

    this->menu_layout->addWidget(button);
    return submenu;
}

void WasmPopupMenu::addSeparator()
{
    QFrame *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setContentsMargins(6, 3, 6, 3);
    this->menu_layout->addWidget(separator);
}

QLabel *WasmPopupMenu::addSection(const QString &text)
{
    QLabel *label = new QLabel(text, this);
    QFont font = label->font();
    font.setBold(true);
    if (font.pointSizeF() > 0.0)
        font.setPointSizeF(qMax(7.0, font.pointSizeF() - 1.0));
    label->setFont(font);
    label->setMinimumHeight(24);
    label->setContentsMargins(10, 4, 10, 2);

    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);

    this->menu_layout->addWidget(label);
    return label;
}

void WasmPopupMenu::addWidget(QWidget *widget, int index)
{
    if (widget == nullptr)
        return;

    widget->setParent(this);
    if (index >= 0 && index < this->menu_layout->count())
        this->menu_layout->insertWidget(index, widget);
    else
        this->menu_layout->addWidget(widget);
}

void WasmPopupMenu::clear()
{
    closeChildMenus();

    while (QLayoutItem *item = this->menu_layout->takeAt(0))
    {
        QWidget *widget = item->widget();
        if (widget != nullptr)
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

void WasmPopupMenu::popup(const QPoint &global_position)
{
    if (this != this->root_menu)
        return;

    if (isVisible())
    {
        closeAll();
        return;
    }

    if (this->owner_widget != nullptr)
    {
        const quintptr active_value = this->owner_widget->property(
            "aowis_active_wasm_popup").value<quintptr>();
        WasmPopupMenu *active_menu = reinterpret_cast<WasmPopupMenu *>(active_value);
        if (active_menu != nullptr && active_menu != this)
            active_menu->closeAll();
        this->owner_widget->setProperty(
            "aowis_active_wasm_popup",
            QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(this)));
    }

    installRootEventFilter();
    showAtGlobalPosition(global_position, false);
    setFocus(Qt::PopupFocusReason);
}

void WasmPopupMenu::popupBelow(QWidget *anchor)
{
    if (anchor == nullptr)
        return;

    this->owner_click_exempt = true;
    const QPoint global_position = anchor->mapToGlobal(QPoint(0, anchor->height() + 1));
    popup(global_position);
}

void WasmPopupMenu::closeAll()
{
    WasmPopupMenu *root = this->root_menu;
    if (root == nullptr)
        return;

    root->closeChildMenus();
    root->hide();
    root->removeRootEventFilter();
    if (root->owner_widget != nullptr)
    {
        const quintptr active_value = root->owner_widget->property(
            "aowis_active_wasm_popup").value<quintptr>();
        if (reinterpret_cast<WasmPopupMenu *>(active_value) == root)
            root->owner_widget->setProperty("aowis_active_wasm_popup", QVariant());
    }

    if (root->delete_on_close)
        root->deleteLater();
}

bool WasmPopupMenu::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qApp && this == this->root_menu)
    {
        if (event->type() == QEvent::KeyPress)
        {
            QKeyEvent *key_event = static_cast<QKeyEvent *>(event);
            if (key_event->key() == Qt::Key_Escape)
            {
                closeAll();
                return true;
            }
        }
        else if (event->type() == QEvent::MouseButtonPress
                 || event->type() == QEvent::TouchBegin)
        {
            bool valid = false;
            const QPoint global_position = globalInputPosition(event, &valid);
            if (valid
                && !containsGlobalPoint(global_position)
                && !ownerContainsGlobalPoint(global_position))
            {
                closeAll();
            }
        }
        else if (event->type() == QEvent::ApplicationDeactivate)
        {
            closeAll();
        }

        return QFrame::eventFilter(watched, event);
    }

    QPushButton *button = qobject_cast<QPushButton *>(watched);
    if (button != nullptr && event->type() == QEvent::Enter)
    {
        WasmPopupMenu *ancestor = this->parent_menu;
        while (ancestor != nullptr)
        {
            ancestor->cancelPendingSubMenuClose();
            ancestor = ancestor->parent_menu;
        }

        const quintptr submenu_value = button->property("aowis_wasm_submenu").value<quintptr>();
        WasmPopupMenu *submenu = reinterpret_cast<WasmPopupMenu *>(submenu_value);
        if (submenu != nullptr)
            openSubMenu(button, submenu);
        else
            scheduleChildMenusClose();
    }

    return QFrame::eventFilter(watched, event);
}

void WasmPopupMenu::enterEvent(QEnterEvent *event)
{
    WasmPopupMenu *ancestor = this->parent_menu;
    while (ancestor != nullptr)
    {
        ancestor->cancelPendingSubMenuClose();
        ancestor = ancestor->parent_menu;
    }

    QFrame::enterEvent(event);
}

void WasmPopupMenu::configureVisuals()
{
    setObjectName(QStringLiteral("aowis_wasm_popup_menu"));
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    setMinimumWidth(MenuMinimumWidth);
    setStyleSheet(QStringLiteral(
        "QFrame#aowis_wasm_popup_menu {"
        "  background: palette(window);"
        "  border: 1px solid palette(mid);"
        "}"));
}

void WasmPopupMenu::installRootEventFilter()
{
    if (this != this->root_menu || this->application_filter_installed)
        return;

    qApp->installEventFilter(this);
    this->application_filter_installed = true;
}

void WasmPopupMenu::removeRootEventFilter()
{
    if (this != this->root_menu || !this->application_filter_installed)
        return;

    qApp->removeEventFilter(this);
    this->application_filter_installed = false;
}

void WasmPopupMenu::closeChildMenus()
{
    cancelPendingSubMenuClose();

    if (this->active_submenu != nullptr)
    {
        this->active_submenu->closeChildMenus();
        this->active_submenu->hide();
        this->active_submenu = nullptr;
        this->active_submenu_item = nullptr;
    }
}

void WasmPopupMenu::scheduleChildMenusClose()
{
    if (this->active_submenu == nullptr)
        return;

    this->pending_close_submenu = this->active_submenu;
    this->submenu_close_timer->start();
}

void WasmPopupMenu::cancelPendingSubMenuClose()
{
    this->submenu_close_timer->stop();
    this->pending_close_submenu = nullptr;
}

void WasmPopupMenu::showAtGlobalPosition(const QPoint &global_position, bool submenu)
{
    Q_UNUSED(submenu);

    adjustSize();
    const QSize requested_size = sizeHint().expandedTo(minimumSizeHint());
    resize(requested_size);

    const QRect bounds = availableParentRect();
    QPoint position = global_position;

    if (!isWindow())
    {
        QWidget *parent_widget = parentWidget();
        if (parent_widget == nullptr)
            return;
        position = parent_widget->mapFromGlobal(global_position);
    }

    position.setX(qBound(
        bounds.left(), position.x(), qMax(bounds.left(), bounds.right() - width() + 1)));
    position.setY(qBound(
        bounds.top(), position.y(), qMax(bounds.top(), bounds.bottom() - height() + 1)));

    move(position);
    show();
    raise();
}

void WasmPopupMenu::openSubMenu(QWidget *item, WasmPopupMenu *submenu)
{
    if (item == nullptr || submenu == nullptr)
        return;

    cancelPendingSubMenuClose();

    if (this->active_submenu != nullptr && this->active_submenu != submenu)
    {
        this->active_submenu->closeChildMenus();
        this->active_submenu->hide();
    }

    this->active_submenu = submenu;
    this->active_submenu_item = item;

    submenu->adjustSize();
    const QPoint item_right = item->mapToGlobal(
        QPoint(qMax(0, item->width() - SubMenuOverlapPx), 0));
    const QPoint item_left = item->mapToGlobal(
        QPoint(-submenu->sizeHint().width() + SubMenuOverlapPx, 0));

    const QRect bounds = availableParentRect();
    QPoint right_test = item_right;
    if (!submenu->isWindow())
    {
        QWidget *parent_widget = parentWidget();
        if (parent_widget == nullptr)
            return;
        right_test = parent_widget->mapFromGlobal(item_right);
    }
    const bool fits_right = right_test.x() + submenu->sizeHint().width() <= bounds.right();
    submenu->showAtGlobalPosition(fits_right ? item_right : item_left, true);
}

bool WasmPopupMenu::containsGlobalPoint(const QPoint &global_position) const
{
    if (isVisible())
    {
        const QRect global_rect(mapToGlobal(QPoint(0, 0)), size());
        if (global_rect.contains(global_position))
            return true;
    }

    return this->active_submenu != nullptr
        && this->active_submenu->containsGlobalPoint(global_position);
}

bool WasmPopupMenu::ownerContainsGlobalPoint(const QPoint &global_position) const
{
    if (!this->owner_click_exempt
        || this->owner_widget == nullptr
        || !this->owner_widget->isVisible())
    {
        return false;
    }

    const QRect owner_rect(this->owner_widget->mapToGlobal(QPoint(0, 0)), this->owner_widget->size());
    return owner_rect.contains(global_position);
}

QRect WasmPopupMenu::availableParentRect() const
{
    QWidget *parent_widget = parentWidget();
    if (parent_widget != nullptr)
    {
        if (isWindow())
            return QRect(parent_widget->mapToGlobal(QPoint(0, 0)), parent_widget->size()).adjusted(
                MenuScreenMargin, MenuScreenMargin, -MenuScreenMargin, -MenuScreenMargin);

        return parent_widget->rect().adjusted(
            MenuScreenMargin, MenuScreenMargin, -MenuScreenMargin, -MenuScreenMargin);
    }

    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen != nullptr)
        return screen->availableGeometry().adjusted(
            MenuScreenMargin, MenuScreenMargin, -MenuScreenMargin, -MenuScreenMargin);

    return QRect();
}
