#include "config/live_ui_text.h"
#include "config/shortcut_registry.h"
#ifdef Q_OS_WASM
#include "widgets/wasm_popup_menu.h"
#endif

#include "config/gui_configuration.h"

#include <QAction>
#include <QMenu>
#include <QPushButton>
#include "config/gui_preferences.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QWidget>

namespace
{
QString configuredShortcut(const GuiShortcutConfiguration &configuration, GuiShortcutId id)
{
    switch (id)
    {
        case GuiShortcutId::SidebarToggle:
            return configuration.sidebar_toggle;
        case GuiShortcutId::Fullscreen:
            return configuration.fullscreen;
        case GuiShortcutId::MapMonitorFullscreen:
            return configuration.map_monitor_fullscreen;
        case GuiShortcutId::SimulationRun:
            return configuration.simulation_run;
        case GuiShortcutId::SimulationRunAlternate:
            return configuration.simulation_run_alternate;
        case GuiShortcutId::MapZoomIn:
            return configuration.map_zoom_in;
        case GuiShortcutId::MapZoomOut:
            return configuration.map_zoom_out;
        case GuiShortcutId::MapPanUp:
            return configuration.map_pan_up;
        case GuiShortcutId::MapPanDown:
            return configuration.map_pan_down;
        case GuiShortcutId::MapPanLeft:
            return configuration.map_pan_left;
        case GuiShortcutId::MapPanRight:
            return configuration.map_pan_right;
        case GuiShortcutId::MapProviderArcGisSat:
            return configuration.map_provider_arcgis_sat;
        case GuiShortcutId::MapProviderOpenStreetMap:
            return configuration.map_provider_openstreetmap;
        case GuiShortcutId::MapProviderOpenTopoMap:
            return configuration.map_provider_opentopomap;
        case GuiShortcutId::MapProviderCycloOsm:
            return configuration.map_provider_cycloosm;
        case GuiShortcutId::MapEditorSelect:
            return configuration.map_editor_select;
        case GuiShortcutId::MapEditorDelete:
            return configuration.map_editor_delete;
        case GuiShortcutId::MapEditorAddPipe:
            return configuration.map_editor_add_pipe;
        case GuiShortcutId::MapEditorAddJunction:
            return configuration.map_editor_add_junction;
        case GuiShortcutId::MapEditorAddValve:
            return configuration.map_editor_add_valve;
        case GuiShortcutId::MapEditorAddDemandPoint:
            return configuration.map_editor_add_demand_point;
        case GuiShortcutId::MapEditorAddPump:
            return configuration.map_editor_add_pump;
        case GuiShortcutId::MapEditorAddTank:
            return configuration.map_editor_add_tank;
        case GuiShortcutId::MapEditorAddPowerSource:
            return configuration.map_editor_add_power_source;
        case GuiShortcutId::MapEditorAddReservoir:
            return configuration.map_editor_add_reservoir;
        case GuiShortcutId::MapEditorAddNote:
            return configuration.map_editor_add_note;
    }

    return QString();
}

bool shortcutAllowsShiftModifier(GuiShortcutId id)
{
    return id == GuiShortcutId::MapZoomIn
        || id == GuiShortcutId::MapZoomOut
        || id == GuiShortcutId::MapPanUp
        || id == GuiShortcutId::MapPanDown
        || id == GuiShortcutId::MapPanLeft
        || id == GuiShortcutId::MapPanRight;
}

bool shortcutSequencesConflict(GuiShortcutId first_id, const QKeySequence &first,
                               GuiShortcutId second_id, const QKeySequence &second)
{
    if (first.isEmpty() || second.isEmpty())
        return false;
    if (first == second)
        return true;
    if (first.count() != 1 || second.count() != 1)
        return false;

    const QKeyCombination first_combination = first[0];
    const QKeyCombination second_combination = second[0];
    if (first_combination.key() != second_combination.key())
        return false;

    if (shortcutAllowsShiftModifier(first_id))
    {
        const Qt::KeyboardModifiers first_modifiers = first_combination.keyboardModifiers();
        if (!first_modifiers.testFlag(Qt::ShiftModifier)
            && second_combination.keyboardModifiers() == (first_modifiers | Qt::ShiftModifier))
        {
            return true;
        }
    }

    if (shortcutAllowsShiftModifier(second_id))
    {
        const Qt::KeyboardModifiers second_modifiers = second_combination.keyboardModifiers();
        if (!second_modifiers.testFlag(Qt::ShiftModifier)
            && first_combination.keyboardModifiers() == (second_modifiers | Qt::ShiftModifier))
        {
            return true;
        }
    }

    return false;
}

QVector<GuiShortcutDefinition> createDefinitions()
{
    const GuiShortcutConfiguration defaults;
    QVector<GuiShortcutDefinition> definitions;
    definitions.reserve(26);

    definitions.append({GuiShortcutId::SidebarToggle, QStringLiteral("sidebar_toggle"),
                        QStringLiteral("Toggle right sidebar"), QStringLiteral("Application"),
                        QStringLiteral("Application"), defaults.sidebar_toggle});
    definitions.append({GuiShortcutId::Fullscreen, QStringLiteral("fullscreen"),
                        QStringLiteral("Toggle fullscreen"), QStringLiteral("Application"),
                        QStringLiteral("Application"), defaults.fullscreen});
    definitions.append({GuiShortcutId::MapMonitorFullscreen, QStringLiteral("map_monitor_fullscreen"),
                        QStringLiteral("Toggle map fullscreen"), QStringLiteral("Map Navigation"),
                        QStringLiteral("Map Monitor"), defaults.map_monitor_fullscreen});
    definitions.append({GuiShortcutId::SimulationRun, QStringLiteral("simulation_run"),
                        QStringLiteral("Run simulation"), QStringLiteral("Simulation"),
                        QStringLiteral("Application"), defaults.simulation_run});
    definitions.append({GuiShortcutId::SimulationRunAlternate, QStringLiteral("simulation_run_alternate"),
                        QStringLiteral("Run simulation (alternate)"), QStringLiteral("Simulation"),
                        QStringLiteral("Application"), defaults.simulation_run_alternate});

    definitions.append({GuiShortcutId::MapZoomIn, QStringLiteral("map_zoom_in"),
                        QStringLiteral("Zoom in"), QStringLiteral("Map Navigation"),
                        QStringLiteral("Map"), defaults.map_zoom_in});
    definitions.append({GuiShortcutId::MapZoomOut, QStringLiteral("map_zoom_out"),
                        QStringLiteral("Zoom out"), QStringLiteral("Map Navigation"),
                        QStringLiteral("Map"), defaults.map_zoom_out});
    definitions.append({GuiShortcutId::MapPanUp, QStringLiteral("map_pan_up"),
                        QStringLiteral("Pan up"), QStringLiteral("Map Navigation"),
                        QStringLiteral("Map"), defaults.map_pan_up});
    definitions.append({GuiShortcutId::MapPanDown, QStringLiteral("map_pan_down"),
                        QStringLiteral("Pan down"), QStringLiteral("Map Navigation"),
                        QStringLiteral("Map"), defaults.map_pan_down});
    definitions.append({GuiShortcutId::MapPanLeft, QStringLiteral("map_pan_left"),
                        QStringLiteral("Pan left"), QStringLiteral("Map Navigation"),
                        QStringLiteral("Map"), defaults.map_pan_left});
    definitions.append({GuiShortcutId::MapPanRight, QStringLiteral("map_pan_right"),
                        QStringLiteral("Pan right"), QStringLiteral("Map Navigation"),
                        QStringLiteral("Map"), defaults.map_pan_right});
    definitions.append({GuiShortcutId::MapProviderArcGisSat, QStringLiteral("map_provider_arcgis_sat"),
                        QStringLiteral("ArcGIS SAT"), QStringLiteral("Map Providers"),
                        QStringLiteral("Map"), defaults.map_provider_arcgis_sat});
    definitions.append({GuiShortcutId::MapProviderOpenStreetMap, QStringLiteral("map_provider_openstreetmap"),
                        QStringLiteral("OpenStreetMap"), QStringLiteral("Map Providers"),
                        QStringLiteral("Map"), defaults.map_provider_openstreetmap});
    definitions.append({GuiShortcutId::MapProviderOpenTopoMap, QStringLiteral("map_provider_opentopomap"),
                        QStringLiteral("OpenTopoMap"), QStringLiteral("Map Providers"),
                        QStringLiteral("Map"), defaults.map_provider_opentopomap});
    definitions.append({GuiShortcutId::MapProviderCycloOsm, QStringLiteral("map_provider_cycloosm"),
                        QStringLiteral("CycloOSM"), QStringLiteral("Map Providers"),
                        QStringLiteral("Map"), defaults.map_provider_cycloosm});

    definitions.append({GuiShortcutId::MapEditorSelect, QStringLiteral("map_editor_select"),
                        QStringLiteral("Selection mode"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_select});
    definitions.append({GuiShortcutId::MapEditorDelete, QStringLiteral("map_editor_delete"),
                        QStringLiteral("Delete selected"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_delete});
    definitions.append({GuiShortcutId::MapEditorAddPipe, QStringLiteral("map_editor_add_pipe"),
                        QStringLiteral("Add pipe / cable"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_pipe});
    definitions.append({GuiShortcutId::MapEditorAddJunction, QStringLiteral("map_editor_add_junction"),
                        QStringLiteral("Add junction"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_junction});
    definitions.append({GuiShortcutId::MapEditorAddValve, QStringLiteral("map_editor_add_valve"),
                        QStringLiteral("Add valve / switch"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_valve});
    definitions.append({GuiShortcutId::MapEditorAddDemandPoint, QStringLiteral("map_editor_add_demand_point"),
                        QStringLiteral("Add demand point"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_demand_point});
    definitions.append({GuiShortcutId::MapEditorAddPump, QStringLiteral("map_editor_add_pump"),
                        QStringLiteral("Add pump"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_pump});
    definitions.append({GuiShortcutId::MapEditorAddTank, QStringLiteral("map_editor_add_tank"),
                        QStringLiteral("Add tank"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_tank});
    definitions.append({GuiShortcutId::MapEditorAddPowerSource, QStringLiteral("map_editor_add_power_source"),
                        QStringLiteral("Add power source"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_power_source});
    definitions.append({GuiShortcutId::MapEditorAddReservoir, QStringLiteral("map_editor_add_reservoir"),
                        QStringLiteral("Add reservoir"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_reservoir});
    definitions.append({GuiShortcutId::MapEditorAddNote, QStringLiteral("map_editor_add_note"),
                        QStringLiteral("Add note"), QStringLiteral("Map Editor"),
                        QStringLiteral("Map Editor"), defaults.map_editor_add_note});

    return definitions;
}

QVector<GuiHoldModifierDefinition> createHoldModifierDefinitions()
{
    // QT_TRANSLATE_NOOP: translated in context GuiShortcuts by the settings
    // page like the shortcut names.
    QVector<GuiHoldModifierDefinition> definitions;
    definitions.append({GuiHoldModifierId::GlobeOrbit, QStringLiteral("globe_orbit_modifier"),
                        QString::fromLatin1(QT_TRANSLATE_NOOP("GuiShortcuts", "Orbit around crosshair terrain (hold + move mouse)")),
                        QString::fromLatin1(QT_TRANSLATE_NOOP("GuiShortcuts", "Globe Navigation (hold)")),
                        QString::fromLatin1(QT_TRANSLATE_NOOP("GuiShortcuts", "3D Map")), Qt::ControlModifier});
    definitions.append({GuiHoldModifierId::GlobeLook, QStringLiteral("globe_look_modifier"),
                        QString::fromLatin1(QT_TRANSLATE_NOOP("GuiShortcuts", "Look around from the camera (hold + move mouse)")),
                        QString::fromLatin1(QT_TRANSLATE_NOOP("GuiShortcuts", "Globe Navigation (hold)")),
                        QString::fromLatin1(QT_TRANSLATE_NOOP("GuiShortcuts", "3D Map")), Qt::AltModifier});
    return definitions;
}
}

GuiShortcutRegistry::GuiShortcutRegistry(QObject *parent)
    : QObject(parent),
      shortcut_definitions(createDefinitions())
{
    this->shortcut_values.reserve(this->shortcut_definitions.size());
    const GuiShortcutConfiguration &configured = guiConfiguration().shortcuts;
    for (const GuiShortcutDefinition &definition : this->shortcut_definitions)
        this->shortcut_values.append(configuredShortcut(configured, definition.id));

    // Hold modifiers live in the same preferences group; a missing, unknown
    // or clashing stored value falls back to the default.
    this->hold_modifier_definitions = createHoldModifierDefinitions();
    const QJsonObject stored = GuiPreferences::group(guiShortcutPreferenceGroup());
    for (const GuiHoldModifierDefinition &definition : this->hold_modifier_definitions)
    {
        const Qt::KeyboardModifier modifier =
            guiHoldModifierFromText(stored.value(definition.config_key).toString());
        const bool allowed = holdModifierChoices().contains(modifier)
            && !this->hold_modifier_values.contains(modifier);
        this->hold_modifier_values.append(allowed ? modifier : definition.default_modifier);
    }
    // Two stored defaults may still clash after a fallback: keep the first.
    for (int index = 0; index < this->hold_modifier_values.size(); ++index)
    {
        for (int other = 0; other < index; ++other)
        {
            if (this->hold_modifier_values.at(other) == this->hold_modifier_values.at(index))
            {
                for (const Qt::KeyboardModifier choice : holdModifierChoices())
                {
                    if (!this->hold_modifier_values.contains(choice))
                    {
                        this->hold_modifier_values[index] = choice;
                        break;
                    }
                }
            }
        }
    }
}

const QVector<GuiHoldModifierDefinition> &GuiShortcutRegistry::holdModifierDefinitions() const
{
    return this->hold_modifier_definitions;
}

const GuiHoldModifierDefinition *GuiShortcutRegistry::holdModifierDefinition(GuiHoldModifierId id) const
{
    const int index = indexForHoldModifier(id);
    if (index < 0)
        return nullptr;
    return &this->hold_modifier_definitions.at(index);
}

QVector<Qt::KeyboardModifier> GuiShortcutRegistry::holdModifierChoices()
{
    // Shift is the faster keyboard pan modifier, never a gesture key.
    return {Qt::ControlModifier, Qt::AltModifier, Qt::MetaModifier};
}

Qt::KeyboardModifier GuiShortcutRegistry::holdModifier(GuiHoldModifierId id) const
{
    const int index = indexForHoldModifier(id);
    if (index < 0)
        return Qt::NoModifier;
    return this->hold_modifier_values.at(index);
}

Qt::Key GuiShortcutRegistry::holdModifierKey(GuiHoldModifierId id) const
{
    switch (holdModifier(id))
    {
        case Qt::ControlModifier:
            return Qt::Key_Control;
        case Qt::AltModifier:
            return Qt::Key_Alt;
        case Qt::MetaModifier:
            return Qt::Key_Meta;
        default:
            break;
    }
    return Qt::Key_unknown;
}

QString GuiShortcutRegistry::holdModifierConflictDisplayName(GuiHoldModifierId id,
                                                             Qt::KeyboardModifier modifier) const
{
    for (int index = 0; index < this->hold_modifier_definitions.size(); ++index)
    {
        const GuiHoldModifierDefinition &definition = this->hold_modifier_definitions.at(index);
        if (definition.id != id && this->hold_modifier_values.at(index) == modifier)
            return definition.display_name;
    }
    return QString();
}

bool GuiShortcutRegistry::setHoldModifier(GuiHoldModifierId id, Qt::KeyboardModifier modifier,
                                          QString *error_message)
{
    const int index = indexForHoldModifier(id);
    if (index < 0 || !holdModifierChoices().contains(modifier))
    {
        if (error_message != nullptr)
            *error_message = tr("This key cannot be used for a map gesture.");
        return false;
    }
    const QString conflict = holdModifierConflictDisplayName(id, modifier);
    if (!conflict.isEmpty())
    {
        if (error_message != nullptr)
            *error_message = tr("%1 is already used for \"%2\".")
                                 .arg(guiHoldModifierText(modifier),
                                      QCoreApplication::translate("GuiShortcuts",
                                                                  conflict.toUtf8().constData()));
        return false;
    }
    if (this->hold_modifier_values.at(index) == modifier)
        return true;
    const GuiHoldModifierDefinition &definition = this->hold_modifier_definitions.at(index);
    if (!GuiPreferences::update(guiShortcutPreferenceGroup(),
                                QJsonObject{{definition.config_key, guiHoldModifierText(modifier)}}))
    {
        if (error_message != nullptr)
            *error_message = tr("Failed to save the keyboard shortcut configuration.");
        return false;
    }
    this->hold_modifier_values[index] = modifier;
    emit holdModifierChanged(id);
    return true;
}

bool GuiShortcutRegistry::resetHoldModifier(GuiHoldModifierId id, QString *error_message)
{
    const GuiHoldModifierDefinition *definition = holdModifierDefinition(id);
    if (definition == nullptr)
        return false;
    // The other gesture may hold this default now: give it the other's
    // default back first (defaults never clash).
    const QString conflict = holdModifierConflictDisplayName(id, definition->default_modifier);
    if (!conflict.isEmpty())
    {
        for (const GuiHoldModifierDefinition &other : this->hold_modifier_definitions)
        {
            if (other.id != id && holdModifier(other.id) == definition->default_modifier)
            {
                // Temporarily free the key: a third choice, then the default.
                for (const Qt::KeyboardModifier choice : holdModifierChoices())
                {
                    if (choice != definition->default_modifier && choice != holdModifier(id))
                    {
                        if (!setHoldModifier(other.id, choice, error_message))
                            return false;
                        break;
                    }
                }
            }
        }
    }
    return setHoldModifier(id, definition->default_modifier, error_message);
}

int GuiShortcutRegistry::indexForHoldModifier(GuiHoldModifierId id) const
{
    for (int index = 0; index < this->hold_modifier_definitions.size(); ++index)
    {
        if (this->hold_modifier_definitions.at(index).id == id)
            return index;
    }
    return -1;
}

const QVector<GuiShortcutDefinition> &GuiShortcutRegistry::definitions() const
{
    return this->shortcut_definitions;
}

const GuiShortcutDefinition *GuiShortcutRegistry::definition(GuiShortcutId id) const
{
    const int index = indexForId(id);
    if (index < 0)
        return nullptr;
    return &this->shortcut_definitions.at(index);
}

QString GuiShortcutRegistry::shortcut(GuiShortcutId id) const
{
    const int index = indexForId(id);
    if (index < 0)
        return QString();
    return this->shortcut_values.at(index);
}

QKeySequence GuiShortcutRegistry::keySequence(GuiShortcutId id) const
{
    return guiShortcutKeySequence(shortcut(id));
}

QString GuiShortcutRegistry::conflictDisplayName(GuiShortcutId id, const QKeySequence &sequence) const
{
    if (sequence.isEmpty())
        return QString();

    for (int index = 0; index < this->shortcut_definitions.size(); ++index)
    {
        const GuiShortcutDefinition &definition = this->shortcut_definitions.at(index);
        if (definition.id == id)
            continue;

        if (shortcutSequencesConflict(
                definition.id, guiShortcutKeySequence(this->shortcut_values.at(index)),
                id, sequence))
        {
            return definition.display_name;
        }
    }

    return QString();
}

bool GuiShortcutRegistry::setShortcut(GuiShortcutId id, const QKeySequence &sequence,
                                      QString *error_message)
{
    const int index = indexForId(id);
    if (index < 0)
    {
        if (error_message != nullptr)
            *error_message = QStringLiteral("Unknown keyboard shortcut.");
        return false;
    }

    const QString conflict = conflictDisplayName(id, sequence);
    if (!conflict.isEmpty())
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("This shortcut is already assigned to \"%1\".")
                                 .arg(conflict);
        }
        return false;
    }

    const QString value = guiShortcutDisplayText(sequence);
    const GuiShortcutDefinition &definition = this->shortcut_definitions.at(index);
    if (!persistShortcut(definition, value, error_message))
        return false;

    if (this->shortcut_values.at(index) == value)
        return true;

    this->shortcut_values[index] = value;
    emit shortcutChanged(id);
    return true;
}

bool GuiShortcutRegistry::resetShortcut(GuiShortcutId id, QString *error_message)
{
    const GuiShortcutDefinition *shortcut_definition = definition(id);
    if (shortcut_definition == nullptr)
        return false;

    return setShortcut(id, guiShortcutKeySequence(shortcut_definition->default_shortcut),
                       error_message);
}

void GuiShortcutRegistry::resetAllShortcuts()
{
    bool changed = false;
    for (int index = 0; index < this->shortcut_definitions.size(); ++index)
    {
        const GuiShortcutDefinition &definition = this->shortcut_definitions.at(index);
        const QString default_value = definition.default_shortcut;
        QString error_message;
        if (!persistShortcut(definition, default_value, &error_message))
            continue;

        if (this->shortcut_values.at(index) != default_value)
        {
            this->shortcut_values[index] = default_value;
            emit shortcutChanged(definition.id);
            changed = true;
        }
    }

    // Hold modifiers: write both defaults (they never clash with each other).
    for (int index = 0; index < this->hold_modifier_definitions.size(); ++index)
    {
        const GuiHoldModifierDefinition &definition = this->hold_modifier_definitions.at(index);
        if (this->hold_modifier_values.at(index) == definition.default_modifier)
            continue;
        if (!GuiPreferences::update(guiShortcutPreferenceGroup(),
                                    QJsonObject{{definition.config_key,
                                                 guiHoldModifierText(definition.default_modifier)}}))
        {
            continue;
        }
        this->hold_modifier_values[index] = definition.default_modifier;
        emit holdModifierChanged(definition.id);
        changed = true;
    }

    if (changed)
        emit shortcutsReset();
}

void GuiShortcutRegistry::requestEdit(GuiShortcutId id)
{
    emit editShortcutRequested(id);
}

bool GuiShortcutRegistry::shortcutCaptureActive() const
{
    return this->shortcut_capture_active;
}

void GuiShortcutRegistry::setShortcutCaptureActive(bool active)
{
    if (this->shortcut_capture_active == active)
        return;

    this->shortcut_capture_active = active;
    emit shortcutCaptureActiveChanged(active);
}

int GuiShortcutRegistry::indexForId(GuiShortcutId id) const
{
    for (int index = 0; index < this->shortcut_definitions.size(); ++index)
    {
        if (this->shortcut_definitions.at(index).id == id)
            return index;
    }
    return -1;
}

bool GuiShortcutRegistry::persistShortcut(const GuiShortcutDefinition &definition,
                                          const QString &value,
                                          QString *error_message)
{
    // AOWIS-CONFIG preferences, in browsers too (config/gui_preferences.h).
    if (!GuiPreferences::update(guiShortcutPreferenceGroup(), QJsonObject{{definition.config_key, value}}))
    {
        if (error_message != nullptr)
            *error_message = tr("Failed to save the keyboard shortcut configuration.");
        return false;
    }
    return true;
}

GuiShortcutRegistry &guiShortcutRegistry()
{
    static GuiShortcutRegistry registry;
    return registry;
}

QString guiShortcutDisplayText(const QKeySequence &sequence)
{
    QString text = sequence.toString(QKeySequence::PortableText);
    text.replace(QStringLiteral("Meta+"), QStringLiteral("Win+"), Qt::CaseInsensitive);
    return text;
}

QString guiHoldModifierText(Qt::KeyboardModifier modifier)
{
    switch (modifier)
    {
        case Qt::ControlModifier:
            return QStringLiteral("Ctrl");
        case Qt::AltModifier:
            return QStringLiteral("Alt");
        case Qt::MetaModifier:
            // Same naming as guiShortcutDisplayText().
            return QStringLiteral("Win");
        case Qt::ShiftModifier:
            return QStringLiteral("Shift");
        default:
            break;
    }
    return QString();
}

Qt::KeyboardModifier guiHoldModifierFromText(const QString &text)
{
    const QString key = text.trimmed();
    if (key.compare(QStringLiteral("Ctrl"), Qt::CaseInsensitive) == 0)
        return Qt::ControlModifier;
    if (key.compare(QStringLiteral("Alt"), Qt::CaseInsensitive) == 0)
        return Qt::AltModifier;
    if (key.compare(QStringLiteral("Win"), Qt::CaseInsensitive) == 0
        || key.compare(QStringLiteral("Meta"), Qt::CaseInsensitive) == 0)
        return Qt::MetaModifier;
    if (key.compare(QStringLiteral("Shift"), Qt::CaseInsensitive) == 0)
        return Qt::ShiftModifier;
    return Qt::NoModifier;
}

QString guiShortcutPresentation(GuiShortcutId id)
{
    const QString shortcut = guiShortcutRegistry().shortcut(id);
    return shortcut.isEmpty() ? QStringLiteral("—") : shortcut;
}

void installShortcutEditContextMenu(QWidget *widget, GuiShortcutId id)
{
    if (widget == nullptr)
        return;

    widget->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(widget, &QWidget::customContextMenuRequested, widget,
                     [widget, id](const QPoint &position)
    {
#ifdef Q_OS_WASM
        WasmPopupMenu *menu = new WasmPopupMenu(widget);
        menu->setDeleteOnClose(true);
        QPushButton *edit_action = menu->addAction(
            QCoreApplication::translate("GuiShortcutRegistry", "Edit keyboard shortcut…"), [id]
        {
            guiShortcutRegistry().requestEdit(id);
        });
        LiveUiText::bind(menu, [edit_action]
        {
            edit_action->setText(QCoreApplication::translate(
                "GuiShortcutRegistry", "Edit keyboard shortcut…"));
        });
        menu->popup(widget->mapToGlobal(position));
#else
        QMenu *menu = new QMenu(widget);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        QAction *edit_action = menu->addAction(
            QCoreApplication::translate("GuiShortcutRegistry", "Edit keyboard shortcut…"));
        QObject::connect(edit_action, &QAction::triggered, widget, [id]
        {
            guiShortcutRegistry().requestEdit(id);
        });
        LiveUiText::bind(menu, [edit_action]
        {
            edit_action->setText(QCoreApplication::translate(
                "GuiShortcutRegistry", "Edit keyboard shortcut…"));
        });
        menu->popup(widget->mapToGlobal(position));
#endif
    });
}
