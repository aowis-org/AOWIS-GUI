#include "tabs/tab_settings_widget.h"

#include "settings/keyboard_shortcuts_settings_widget.h"
#include "settings/map_settings_widget.h"
#include "settings/units_settings_widget.h"
#include "settings/wireguard_vpn_settings_widget.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QListWidget>
#include <QStackedWidget>

namespace
{
constexpr int TableOfContentsWidth = 200;
}

SettingsWidget::SettingsWidget(QWidget *parent)
    : QWidget(parent)
{
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    this->table_of_contents = new QListWidget(this);
    this->table_of_contents->setFixedWidth(TableOfContentsWidth);
    this->table_of_contents->setFrameShape(QFrame::NoFrame);
    layout->addWidget(this->table_of_contents);

    this->pages = new QStackedWidget(this);
    layout->addWidget(this->pages, 1);

    this->keyboard_shortcuts_page = new KeyboardShortcutsSettingsWidget(this->pages);
    this->table_of_contents->addItem(tr("Keyboard Shortcuts"));
    this->pages->addWidget(this->keyboard_shortcuts_page);

    this->map_settings_page = new MapSettingsWidget(this->pages);
    this->table_of_contents->addItem(tr("Map Settings"));
    this->pages->addWidget(this->map_settings_page);

    this->units_settings_page = new UnitsSettingsWidget(this->pages);
    this->table_of_contents->addItem(tr("Units"));
    this->pages->addWidget(this->units_settings_page);

    this->wireguard_vpn_page = new WireGuardVpnSettingsWidget(this->pages);
    QListWidgetItem *wireguard_entry = new QListWidgetItem(tr("WireGuard VPN"), this->table_of_contents);
    wireguard_entry->setToolTip(tr("Manage shared WireGuard VPN profiles and view their usage and tunnel status."));
    this->pages->addWidget(this->wireguard_vpn_page);
#if defined(__EMSCRIPTEN__) || defined(Q_OS_WASM)
    // A browser cannot establish the native UDP/TUN WireGuard tunnel used by
    // AOWIS-NETWORK. Keep the option discoverable, but do not open a dead UI.
    wireguard_entry->setFlags(wireguard_entry->flags() & ~Qt::ItemIsEnabled);
    wireguard_entry->setToolTip(tr("WireGuard VPN is unavailable in WebAssembly: browser networking does not provide the required UDP/TUN access."));
#endif

    connect(this->table_of_contents, &QListWidget::currentRowChanged,
            this, [this](int row)
    {
        if (row >= 0)
            this->pages->setCurrentIndex(row);
    });

    this->table_of_contents->setCurrentRow(0);
}

void SettingsWidget::focusShortcut(GuiShortcutId id)
{
    this->table_of_contents->setCurrentRow(0);
    this->keyboard_shortcuts_page->focusShortcut(id);
}

void SettingsWidget::focusUnits()
{
    this->table_of_contents->setCurrentRow(this->pages->indexOf(this->units_settings_page));
}

void SettingsWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && this->table_of_contents->count() == 4)
    {
        this->table_of_contents->item(0)->setText(tr("Keyboard Shortcuts"));
        this->table_of_contents->item(1)->setText(tr("Map Settings"));
        this->table_of_contents->item(2)->setText(tr("Units"));
        QListWidgetItem *entry = this->table_of_contents->item(3);
        entry->setText(tr("WireGuard VPN"));
#if defined(__EMSCRIPTEN__) || defined(Q_OS_WASM)
        entry->setToolTip(tr("WireGuard VPN is unavailable in WebAssembly: browser networking does not provide the required UDP/TUN access."));
#else
        entry->setToolTip(tr("Manage shared WireGuard VPN profiles and view their usage and tunnel status."));
#endif
    }
}
