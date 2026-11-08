#pragma once

#include "config/live_ui_text.h"
#include "widgets/group_box_collapsible.h"

#include <QComboBox>
#include <QLabel>
#include <QSignalBlocker>

namespace EntityInspectorText
{
inline GroupBoxCollapsible *group(const char *source)
{
    GroupBoxCollapsible *box = new GroupBoxCollapsible();
    LiveUiText::bind(box, [box, source] { box->setTitle(LiveUiText::translate(source)); });
    return box;
}

// Dynamic field names (for example a valve's setting) retain the current
// source string as the hydraulic configuration changes.
inline void setCaption(QLabel *label, const char *source)
{
    const bool first = !label->property("aowisCaptionSource").isValid();
    label->setProperty("aowisCaptionSource", QByteArray(source));
    if (first)
        LiveUiText::bind(label, [label] {
            label->setText(LiveUiText::translate(label->property("aowisCaptionSource").toByteArray().constData()));
        });
    else
        label->setText(LiveUiText::translate(source));
}

// Only tagged rows represent UI choices. Pattern names and other model data
// in the same combo retain their original display text and item data.
inline void addChoice(QComboBox *combo, const char *source, const QVariant &value = {})
{
    constexpr int source_role = Qt::UserRole + 7341;
    combo->addItem(LiveUiText::translate(source), value);
    combo->setItemData(combo->count() - 1, QByteArray(source), source_role);
    if (combo->property("aowisTranslatedChoices").toBool())
        return;
    combo->setProperty("aowisTranslatedChoices", true);
    LiveUiText::bind(combo, [combo] {
        const QSignalBlocker blocker(combo);
        for (int row = 0; row < combo->count(); ++row)
        {
            const QByteArray key = combo->itemData(row, source_role).toByteArray();
            if (!key.isEmpty())
                combo->setItemText(row, LiveUiText::translate(key.constData()));
        }
    });
}
}
