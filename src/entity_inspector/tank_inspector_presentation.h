#pragma once

#include "config/live_ui_text.h"

#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QUuid>

// Presentation-only bindings for Tank Inspector controls. Curve UUIDs, model
// curve names and numeric editors are never changed by a language switch.
namespace TankInspectorPresentation
{
constexpr int curve_text_role = Qt::UserRole + 7350;
enum CurveTextKind { ModelCurve = 0, SelectCurve = 1, MissingCurve = 2 };

inline QString selectCurveText()
{
    return QCoreApplication::translate("EntityInspectorTank", "[Select Curve]");
}

inline QString missingCurveText(const QUuid &uuid)
{
    return QCoreApplication::translate("EntityInspectorTank", "[Missing Curve] %1")
        .arg(uuid.toString(QUuid::WithoutBraces));
}

inline void retranslateCurveChoices(QComboBox *combo)
{
    const QSignalBlocker blocker(combo);
    for (int index = 0; index < combo->count(); ++index)
    {
        const int kind = combo->itemData(index, curve_text_role).toInt();
        if (kind == SelectCurve)
            combo->setItemText(index, selectCurveText());
        else if (kind == MissingCurve)
            combo->setItemText(index, missingCurveText(combo->itemData(index, Qt::UserRole).toUuid()));
    }
}

inline void bindCurveChoices(QComboBox *combo)
{
    LiveUiText::bind(combo, [combo] { retranslateCurveChoices(combo); });
}

// Retain the *source key*, not its English rendering, when an editor switches
// between an input/derived value or hydraulic geometry modes.
inline void refreshSpinTooltip(QDoubleSpinBox *spin)
{
    const QByteArray source = spin->property("aowisTankTooltipSource").toByteArray();
    spin->setToolTip(source.isEmpty() ? QString()
        : QCoreApplication::translate("EntityInspectorTank", source.constData()));
}

inline void setSpinTooltip(QDoubleSpinBox *spin, const char *source)
{
    spin->setProperty("aowisTankTooltipSource", QByteArray(source ? source : ""));
    if (!spin->property("aowisTankTooltipBound").toBool())
    {
        spin->setProperty("aowisTankTooltipBound", true);
        LiveUiText::bind(spin, [spin] { refreshSpinTooltip(spin); });
    }
    else
        refreshSpinTooltip(spin);
}
}
