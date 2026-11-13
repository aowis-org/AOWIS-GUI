#pragma once

#include <QObject>
#include <QKeySequence>
#include <QString>
#include <QVector>

class QWidget;

enum class GuiShortcutId
{
    SidebarToggle,
    Fullscreen,
    MapMonitorFullscreen,
    SimulationRun,
    SimulationRunAlternate,
    MapZoomIn,
    MapZoomOut,
    MapPanUp,
    MapPanDown,
    MapPanLeft,
    MapPanRight,
    MapProviderArcGisSat,
    MapProviderOpenStreetMap,
    MapProviderOpenTopoMap,
    MapProviderCycloOsm,
    MapEditorSelect,
    MapEditorDelete,
    MapEditorAddPipe,
    MapEditorAddJunction,
    MapEditorAddValve,
    MapEditorAddDemandPoint,
    MapEditorAddPump,
    MapEditorAddTank,
    MapEditorAddPowerSource,
    MapEditorAddReservoir,
    MapEditorAddNote
};

// Modifier keys held during a mouse gesture (no key sequence: QKeySequenceEdit
// cannot capture a lone modifier). The gesture runs while the key is held
// and the mouse moves without a button pressed.
enum class GuiHoldModifierId
{
    // Globe: orbit around the terrain under the crosshair (default Ctrl).
    GlobeOrbit,
    // Globe: free look from the camera (default Alt).
    GlobeLook
};

struct GuiHoldModifierDefinition
{
    GuiHoldModifierId id;
    QString config_key;
    QString display_name;
    QString category;
    QString scope;
    Qt::KeyboardModifier default_modifier;
};

struct GuiShortcutDefinition
{
    GuiShortcutId id;
    QString config_key;
    QString display_name;
    QString category;
    QString scope;
    QString default_shortcut;
};

class GuiShortcutRegistry : public QObject
{
    Q_OBJECT

public:
    explicit GuiShortcutRegistry(QObject *parent = nullptr);

    const QVector<GuiShortcutDefinition> &definitions() const;
    const GuiShortcutDefinition *definition(GuiShortcutId id) const;
    QString shortcut(GuiShortcutId id) const;
    QKeySequence keySequence(GuiShortcutId id) const;
    QString conflictDisplayName(GuiShortcutId id, const QKeySequence &sequence) const;

    bool setShortcut(GuiShortcutId id, const QKeySequence &sequence, QString *error_message = nullptr);
    bool resetShortcut(GuiShortcutId id, QString *error_message = nullptr);
    void resetAllShortcuts();
    void requestEdit(GuiShortcutId id);

    // Hold modifiers (globe orbit / free look). Choices: Ctrl, Alt, Meta.
    // Shift is reserved (faster keyboard pan); two gestures never share one.
    const QVector<GuiHoldModifierDefinition> &holdModifierDefinitions() const;
    const GuiHoldModifierDefinition *holdModifierDefinition(GuiHoldModifierId id) const;
    static QVector<Qt::KeyboardModifier> holdModifierChoices();
    Qt::KeyboardModifier holdModifier(GuiHoldModifierId id) const;
    // The key whose press / release starts / ends the gesture.
    Qt::Key holdModifierKey(GuiHoldModifierId id) const;
    // Display name of the gesture holding modifier, or empty.
    QString holdModifierConflictDisplayName(GuiHoldModifierId id, Qt::KeyboardModifier modifier) const;
    bool setHoldModifier(GuiHoldModifierId id, Qt::KeyboardModifier modifier,
                         QString *error_message = nullptr);
    bool resetHoldModifier(GuiHoldModifierId id, QString *error_message = nullptr);
    bool shortcutCaptureActive() const;
    void setShortcutCaptureActive(bool active);

signals:
    void shortcutChanged(GuiShortcutId id);
    void holdModifierChanged(GuiHoldModifierId id);
    void shortcutsReset();
    void editShortcutRequested(GuiShortcutId id);
    void shortcutCaptureActiveChanged(bool active);

private:
    QVector<GuiShortcutDefinition> shortcut_definitions;
    QVector<QString> shortcut_values;
    QVector<GuiHoldModifierDefinition> hold_modifier_definitions;
    QVector<Qt::KeyboardModifier> hold_modifier_values;
    bool shortcut_capture_active = false;

    int indexForId(GuiShortcutId id) const;
    int indexForHoldModifier(GuiHoldModifierId id) const;
    bool persistShortcut(const GuiShortcutDefinition &definition, const QString &value,
                         QString *error_message);
};

GuiShortcutRegistry &guiShortcutRegistry();
QString guiShortcutDisplayText(const QKeySequence &sequence);
// Portable name of a hold modifier ("Ctrl", "Alt", "Win" / "Meta"): the
// stored value and the text shown in settings and the map HUD.
QString guiHoldModifierText(Qt::KeyboardModifier modifier);
// Inverse of guiHoldModifierText(); NoModifier for anything else.
Qt::KeyboardModifier guiHoldModifierFromText(const QString &text);
QString guiShortcutPresentation(GuiShortcutId id);
void installShortcutEditContextMenu(QWidget *widget, GuiShortcutId id);
