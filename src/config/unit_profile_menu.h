#pragma once

#include "config/unit_profile_manager.h"
#include "config/live_ui_text.h"
#ifdef Q_OS_WASM
#include "widgets/wasm_popup_menu.h"
#include <QPushButton>
#else
#include <QAction>
#include <QMenu>
#endif
#include <QObject>
#include <QCoreApplication>

#include <cstddef>
#include <functional>
#include <string_view>

// Shared layout for the toolbar, Settings selector and custom-profile source menu.
// Resolve built-ins by stable ID rather than by their position in the collection.
namespace UnitProfileMenu {
#ifdef Q_OS_WASM
using Menu = WasmPopupMenu;
#else
using Menu = QMenu;
#endif

enum class Group { Canonical, FeaturedMetric, OtherMetric, Imperial, Custom };

[[nodiscard]] inline Group group(const UnitProfileManager::Profile &profile)
{
    if (!profile.builtin) return Group::Custom;
    if (profile.id == QStringLiteral("canonical")) return Group::Canonical;
    for (std::size_t i = 0; i < aowis::units::flow_template_names.size(); ++i) {
        if (profile.id != QString::fromLatin1(aowis::units::flow_template_names[i].data(),
                                              static_cast<qsizetype>(aowis::units::flow_template_names[i].size())))
            continue;
        if (i < 2) return Group::FeaturedMetric;
        return i < 6 ? Group::OtherMetric : Group::Imperial;
    }
    // Unknown built-in presets remain accessible rather than being hidden.
    return Group::OtherMetric;
}

[[nodiscard]] inline QString label(const UnitProfileManager::Profile &profile)
{
    if (!profile.builtin) return profile.name;
    if (profile.id == QStringLiteral("canonical"))
        return QCoreApplication::translate("UnitProfileMenu", "AOWIS Canonical — standard AOWIS units");
    if (profile.id == QStringLiteral("CMH"))
        return QCoreApplication::translate("UnitProfileMenu", "CMH — cubic meters per hour");
    if (profile.id == QStringLiteral("LPS"))
        return QCoreApplication::translate("UnitProfileMenu", "LPS — liters per second");
    if (profile.id == QStringLiteral("LPM"))
        return QCoreApplication::translate("UnitProfileMenu", "LPM — liters per minute");
    if (profile.id == QStringLiteral("MLD"))
        return QCoreApplication::translate("UnitProfileMenu", "MLD — million liters per day");
    if (profile.id == QStringLiteral("CMD"))
        return QCoreApplication::translate("UnitProfileMenu", "CMD — cubic meters per day");
    if (profile.id == QStringLiteral("CMS"))
        return QCoreApplication::translate("UnitProfileMenu", "CMS — cubic meters per second");
    if (profile.id == QStringLiteral("CFS"))
        return QCoreApplication::translate("UnitProfileMenu", "CFS — cubic feet per second");
    if (profile.id == QStringLiteral("GPM"))
        return QCoreApplication::translate("UnitProfileMenu", "GPM — US gallons per minute");
    if (profile.id == QStringLiteral("MGD"))
        return QCoreApplication::translate("UnitProfileMenu", "MGD — million US gallons per day");
    if (profile.id == QStringLiteral("IMGD"))
        return QCoreApplication::translate("UnitProfileMenu", "IMGD — million Imperial gallons per day");
    if (profile.id == QStringLiteral("AFD"))
        return QCoreApplication::translate("UnitProfileMenu", "AFD — acre-feet per day");
    // Preserve names of future built-in profiles until they have a description.
    return profile.name;
}

[[nodiscard]] inline Menu *subMenu(Menu *parent, const char *source)
{
    const QString title = QCoreApplication::translate("UnitProfileMenu", source);
#ifdef Q_OS_WASM
    Menu *submenu = parent->addSubMenu(title);
    // The custom WebAssembly menu represents submenu triggers as QPushButtons.
    // Bind the trigger itself so a visible popup also changes language in place.
    const QList<QPushButton *> triggers = parent->findChildren<QPushButton *>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!triggers.isEmpty())
    {
        QPushButton *trigger = triggers.last();
        LiveUiText::bind(trigger, [trigger, source] {
            trigger->setText(QCoreApplication::translate("UnitProfileMenu", source)
                             + QStringLiteral("    ›"));
        });
    }
    return submenu;
#else
    Menu *submenu = parent->addMenu(title);
    submenu->menuAction()->setProperty("aowis.unit.menuSource", QString::fromUtf8(source));
    return submenu;
#endif
}

// Retitle existing native actions without clearing the menu, replacing checkable
// items, disconnecting callbacks or modifying the active unit-profile identity.
inline void retranslate(Menu *menu, const QVector<UnitProfileManager::Profile> &profiles)
{
#ifdef Q_OS_WASM
    // WebAssembly profile and submenu buttons have LiveUiText bindings created
    // by populate(); changing the language does not reconstruct their widgets.
    (void)menu;
    (void)profiles;
#else
    if (menu == nullptr) return;
    for (QAction *action : menu->actions())
    {
        const QVariant index = action->property("aowis.unit.profileIndex");
        const QVariant source = action->property("aowis.unit.menuSource");
        if (index.isValid() && index.toInt() >= 0 && index.toInt() < profiles.size())
            action->setText(label(profiles.at(index.toInt())));
        else if (source.isValid())
            action->setText(QCoreApplication::translate("UnitProfileMenu",
                source.toString().toUtf8().constData()));
        if (action->menu() != nullptr)
            retranslate(action->menu(), profiles);
    }
#endif
}

inline void populate(Menu *menu, QObject *owner,
                     const QVector<UnitProfileManager::Profile> &profiles,
                     int selectedIndex,
                     const std::function<void(int)> &onSelect,
                     const std::function<void()> &onManage = {})
{
#ifdef Q_OS_WASM
    (void)owner;
    (void)selectedIndex;
#endif
    menu->clear();
    const std::function<void(Group, Menu *)> addGroup =
#ifdef Q_OS_WASM
        [&profiles, &onSelect](Group wanted, Menu *target) {
#else
        [owner, &profiles, selectedIndex, &onSelect](Group wanted, Menu *target) {
#endif
            for (int index = 0; index < profiles.size(); ++index) {
                const UnitProfileManager::Profile &profile = profiles.at(index);
                if (group(profile) != wanted) continue;
#ifdef Q_OS_WASM
                QPushButton *action = target->addAction(label(profile), [onSelect, index] { onSelect(index); });
                LiveUiText::bind(action, [action, profile] { action->setText(label(profile)); });
#else
                QAction *action = target->addAction(label(profile));
                action->setData(index);
                action->setProperty("aowis.unit.profileIndex", index);
                if (selectedIndex >= 0) {
                    action->setCheckable(true);
                    action->setChecked(index == selectedIndex);
                }
                QObject::connect(action, &QAction::triggered, owner,
                                 [onSelect, index] { onSelect(index); });
#endif
            }
        };

    addGroup(Group::Canonical, menu);
    menu->addSeparator();
    addGroup(Group::FeaturedMetric, menu);
    addGroup(Group::OtherMetric, subMenu(menu, QT_TRANSLATE_NOOP("UnitProfileMenu", "Other metric")));
    addGroup(Group::Imperial, subMenu(menu, QT_TRANSLATE_NOOP("UnitProfileMenu", "Imperial / US")));
#ifndef Q_OS_WASM
    menu->addSeparator();
#endif
    Menu *custom = subMenu(menu, QT_TRANSLATE_NOOP("UnitProfileMenu", "Custom"));
    addGroup(Group::Custom, custom);
    if (onManage) {
        custom->addSeparator();
#ifdef Q_OS_WASM
        QPushButton *action = custom->addAction(
            QCoreApplication::translate("UnitProfileMenu", "Manage Units…"), onManage);
        LiveUiText::bind(action, [action] {
            action->setText(QCoreApplication::translate("UnitProfileMenu", "Manage Units…"));
        });
#else
        QAction *action = custom->addAction(
            QCoreApplication::translate("UnitProfileMenu", "Manage Units…"));
        action->setProperty("aowis.unit.menuSource", QStringLiteral("Manage Units…"));
        QObject::connect(action, &QAction::triggered, owner, onManage);
#endif
    }
}
} // namespace UnitProfileMenu
