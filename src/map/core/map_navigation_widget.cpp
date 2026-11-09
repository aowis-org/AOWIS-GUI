#include "map/core/map_navigation_widget.h"

#include "config/gui_configuration.h"
#include "map/core/gui_map_provider_catalog.h"
#include "config/shortcut_registry.h"
#include "map/core/map_symbology_slider.h"
#include "map/core/map_symbology_length_units.h"
#include "config/unit_aware_view.h"
#include "config/unit_profile_manager.h"

#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStringList>
#include <QtMath>

#ifdef Q_OS_WASM
#include "widgets/wasm_popup_menu.h"
#endif

namespace
{
constexpr GuiShortcutId ProviderShortcutIds[] = {
    GuiShortcutId::MapProviderArcGisSat,
    GuiShortcutId::MapProviderOpenStreetMap,
    GuiShortcutId::MapProviderOpenTopoMap,
    GuiShortcutId::MapProviderCycloOsm
};
constexpr int ProviderShortcutCount = sizeof(ProviderShortcutIds) / sizeof(ProviderShortcutIds[0]);
}

MapNavigationWidget::MapNavigationWidget(MapWidget *map, CanvasMode mode, QWidget *keyboard_focus_target, QWidget *parent)
    : QWidget{parent},
    mode( mode ),
    grid( new QGridLayout(this) ),
    map( map ),
    keyboard_focus_target( keyboard_focus_target != nullptr ? keyboard_focus_target : map )
{
    // These shortcuts are independent of the provider selector: there are no
    // longer any radio buttons, and the selected server layer may be custom.
    // Qt handles them while the sidebar is visible; fullscreen key forwarding
    // below handles them when the sidebar is hidden.
    for (int index = 0; index < ProviderShortcutCount; ++index)
    {
        const MapProvider provider = static_cast<MapProvider>(index + 1);
        QShortcut *shortcut = new QShortcut(this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, provider]
        {
            activateMapProvider(provider);
        });
        this->provider_shortcuts[index] = shortcut;
    }
    connect(&guiShortcutRegistry(), &GuiShortcutRegistry::shortcutChanged,
            this, [this](GuiShortcutId)
    {
        refreshShortcutPresentation();
    });

    const MapViewMode initial_view_mode = this->map->model()->viewMode();
    connect(this->map->model(), &MapModel::viewModeChanged, this, [this](MapViewMode view_mode)
    {
        sync3dIconsForViewMode(view_mode);
        syncIconSizeSliderForViewMode(view_mode);
    });

    this->label_map_opacity = new QLabel(tr("Opacity"), this);
    QSlider *slider_map_visibility = new QSlider(Qt::Horizontal);
    this->slider_map_opacity = slider_map_visibility;
    slider_map_visibility->setRange(0, 100);
    connect(slider_map_visibility, &QSlider::valueChanged, this, &MapNavigationWidget::signalSlideOpacityChanged);

    this->label_icon_size = new QLabel(
        this->mode == CanvasMode::Monitor ? tr("Icon Size") : tr("Icon Size [%]"));
    if (this->mode == CanvasMode::Monitor)
    {
        this->combo_icon_size_unit = new QComboBox(this);
        this->combo_icon_size_unit->addItem(QStringLiteral("m"),
            static_cast<int>(NetworkSymbologySizeUnit::Meters));
        this->combo_icon_size_unit->addItem(QStringLiteral("px"),
            static_cast<int>(NetworkSymbologySizeUnit::Pixels));
        this->combo_icon_size_unit->setCurrentIndex(1);
        this->combo_icon_size_unit->setToolTip(tr(
            "Meters keep a true world-space icon size. Pixels keep a compact visual size while preserving 3D perspective."));
        this->slider_icon_size = new MapSymbologySlider(
            NetworkSymbologyMinimumIconSizePx,
            NetworkSymbologyMaximumIconSizePx,
            NetworkSymbologyDefaultIconSizePx,
            tr("Sets the reservoir, tank, pump and valve icon size."),
            QStringLiteral(" px"), this);
        connect(this->slider_icon_size, &QSlider::valueChanged, this, [this](int raw_value)
        {
            const MapViewMode view_mode = this->map->model()->viewMode();
            const bool is_3d = view_mode == MapViewMode::Globe;
            // The Monitor globe shares the Editor's percentage-based pixmap
            // overlay. Keep the 2D Monitor's metre/pixel settings separate.
            if (is_3d)
            {
                this->icon_size_3d_percent = raw_value;
                emit signalIconSizeChanged(raw_value);
                return;
            }
            const NetworkSymbologySizeUnit unit = this->icon_size_2d_unit;
            if (unit == NetworkSymbologySizeUnit::Meters)
                this->icon_size_2d_m = raw_value;
            else
                this->icon_size_2d_px = raw_value;
            emit signalMonitorIconSizeChanged(unit, raw_value);
        });
        connect(this->combo_icon_size_unit, &QComboBox::currentIndexChanged, this,
            [this](int index)
        {
            const NetworkSymbologySizeUnit unit = static_cast<NetworkSymbologySizeUnit>(
                this->combo_icon_size_unit->itemData(index).toInt());
            if (this->map->model()->viewMode() == MapViewMode::Globe
                || this->icon_size_2d_unit == unit)
                return;

            this->icon_size_2d_unit = unit;
            syncIconSizeSliderForViewMode(this->map->model()->viewMode());
            emit signalMonitorIconSizeUnitChanged(unit);
        });
    }
    else
    {
        this->slider_icon_size = new QSlider(Qt::Horizontal);
        this->slider_icon_size->setToolTip(tr("Scales entity icons and 3D entity models."));
        connect(this->slider_icon_size, &QSlider::valueChanged, this, [this](int size_percent)
        {
            if (this->map->model()->viewMode() == MapViewMode::Globe)
                this->icon_size_3d_percent = size_percent;
            else
                this->icon_size_2d_percent = size_percent;

            emit signalIconSizeChanged(size_percent);
        });
    }
    syncIconSizeSliderForViewMode(initial_view_mode);
    
    // Pan and zoom live on the map's HUD (joystick, zoom buttons).
    this->combo_provider = new QComboBox(this);
    this->combo_provider->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this->combo_provider, &QWidget::customContextMenuRequested,
            this, [this](const QPoint &position)
    {
#ifdef Q_OS_WASM
        WasmPopupMenu *menu = new WasmPopupMenu(this->combo_provider);
        menu->setDeleteOnClose(true);
#else
        QMenu *menu = new QMenu(this->combo_provider);
        menu->setAttribute(Qt::WA_DeleteOnClose);
#endif
        for (int index = 0; index < ProviderShortcutCount; ++index)
        {
            const GuiShortcutId shortcut_id = ProviderShortcutIds[index];
            const QString provider_name = mapProviderCatalog().provider(
                MapProviderCatalog::builtinId(index + 1)) == nullptr
                ? tr("Map provider %1").arg(index + 1)
                : mapProviderCatalog().provider(MapProviderCatalog::builtinId(index + 1))->name;
            const QString action_text = tr("Edit %1 shortcut (%2)…").arg(
                provider_name, guiShortcutPresentation(shortcut_id));
#ifdef Q_OS_WASM
            menu->addAction(action_text, [shortcut_id]
            {
                guiShortcutRegistry().requestEdit(shortcut_id);
            });
#else
            QAction *action = menu->addAction(action_text);
            connect(action, &QAction::triggered, this, [shortcut_id]
            {
                guiShortcutRegistry().requestEdit(shortcut_id);
            });
#endif
        }
        menu->popup(this->combo_provider->mapToGlobal(position));
    });
    this->grid->addWidget(this->combo_provider, 0, 0, 1, 3);
    connect(this->combo_provider, &QComboBox::currentIndexChanged, this, [this](int index)
    {
        if (index < 0) return;
        this->map->model()->setProviderId(this->combo_provider->itemData(index).toString());
    });
    connect(&mapProviderCatalog(), &MapProviderCatalog::catalogChanged,
            this, &MapNavigationWidget::refreshProviderChoices);
    connect(this->map->model(), &MapModel::providerIdChanged,
            this, [this](const QString &) {refreshProviderChoices();});
    refreshProviderChoices();
    this->grid->addWidget(this->label_map_opacity, 1, 0, 1, 3);
    this->grid->addWidget(slider_map_visibility, 2, 0, 1, 3);
    const int icon_label_row = this->check_3d_icons != nullptr ? 4 : 3;
    const int icon_slider_row = icon_label_row + 1;
    const int label_controls_row = icon_slider_row + 1;
    if (this->check_3d_icons != nullptr)
        this->grid->addWidget(this->check_3d_icons, 3, 0, 1, 3);
    this->grid->addWidget(this->label_icon_size, icon_label_row, 0, 1,
        this->mode == CanvasMode::Monitor ? 2 : 3);
    if (this->combo_icon_size_unit != nullptr)
        this->grid->addWidget(this->combo_icon_size_unit, icon_label_row, 2);
    this->grid->addWidget(this->slider_icon_size, icon_slider_row, 0, 1, 3);
    this->grid->addWidget(createLabelControls(), label_controls_row, 0, 1, 3);
    
    if (this->mode == CanvasMode::Monitor)
    {
        new UnitAwareView(this, UnitProfileManager::instance().presentation(),
            [this] { refreshIconSizeUnitPresentation(); });
        refreshIconSizeUnitPresentation();
    }

    // No network yet: imagery unfaded (leftmost); setNetworkPresent()
    // moves the slider once a network is loaded.
    if (this->mode == CanvasMode::Edit || this->mode == CanvasMode::Monitor)
    {
        slider_map_visibility->setValue(0);
        QTimer::singleShot(0, this, [this, slider_map_visibility]()
        {
            emit signalSlideOpacityChanged(slider_map_visibility->value());
        });
    }
    
    
}

QWidget *MapNavigationWidget::createLabelControls()
{
    // Map labels (EPANET's "Map Options -> Notation"): the IDs and, in the
    // monitor, the values of the metric the map is coloured by, written
    // next to the entities. Decluttered on the map: labels never overlap
    // each other or a symbol, so zooming in reveals more of them.
    QWidget *box = new QWidget(this);
    QGridLayout *box_grid = new QGridLayout(box);
    box_grid->setContentsMargins(0, 0, 0, 0);
    this->label_labels_title = new QLabel(tr("Labels"), box);
    box_grid->addWidget(this->label_labels_title, 0, 0, 1, 2);

    this->check_label_node_ids = new QCheckBox(tr("Node IDs"), box);
    this->check_label_node_ids->setToolTip(tr(
        "Write the ID of every junction, tank, reservoir and demand point next to it "
        "(where there is room; zoom in for more)."));
    this->check_label_link_ids = new QCheckBox(tr("Link IDs"), box);
    this->check_label_link_ids->setToolTip(tr(
        "Write the ID of every pipe, pump and valve at its middle "
        "(where there is room; zoom in for more)."));
    box_grid->addWidget(this->check_label_node_ids, 1, 0);
    box_grid->addWidget(this->check_label_link_ids, 1, 1);
    connect(this->check_label_node_ids, &QCheckBox::toggled, this, [this](bool)
    {
        emit signalLabelSettingsChanged(labelSettings());
    });
    connect(this->check_label_link_ids, &QCheckBox::toggled, this, [this](bool)
    {
        emit signalLabelSettingsChanged(labelSettings());
    });

    if (this->mode == CanvasMode::Monitor)
    {
        this->check_label_node_values = new QCheckBox(tr("Node Values"), box);
        this->check_label_node_values->setToolTip(tr(
            "Write the value of the node metric the map is coloured by (legend unit)."));
        this->check_label_link_values = new QCheckBox(tr("Link Values"), box);
        this->check_label_link_values->setToolTip(tr(
            "Write the value of the link metric the map is coloured by (legend unit)."));
        box_grid->addWidget(this->check_label_node_values, 2, 0);
        box_grid->addWidget(this->check_label_link_values, 2, 1);
        connect(this->check_label_node_values, &QCheckBox::toggled, this, [this](bool)
        {
            emit signalLabelSettingsChanged(labelSettings());
        });
        connect(this->check_label_link_values, &QCheckBox::toggled, this, [this](bool)
        {
            emit signalLabelSettingsChanged(labelSettings());
        });
    }
    return box;
}

void MapNavigationWidget::refreshLocalizedText()
{
    this->label_map_opacity->setText(tr("Opacity"));
    this->label_labels_title->setText(tr("Labels"));
    this->check_label_node_ids->setText(tr("Node IDs"));
    this->check_label_node_ids->setToolTip(tr(
        "Write the ID of every junction, tank, reservoir and demand point next to it "
        "(where there is room; zoom in for more)."));
    this->check_label_link_ids->setText(tr("Link IDs"));
    this->check_label_link_ids->setToolTip(tr(
        "Write the ID of every pipe, pump and valve at its middle "
        "(where there is room; zoom in for more)."));
    if (this->check_label_node_values != nullptr)
    {
        this->check_label_node_values->setText(tr("Node Values"));
        this->check_label_node_values->setToolTip(tr(
            "Write the value of the node metric the map is coloured by (legend unit)."));
    }
    if (this->check_label_link_values != nullptr)
    {
        this->check_label_link_values->setText(tr("Link Values"));
        this->check_label_link_values->setToolTip(tr(
            "Write the value of the link metric the map is coloured by (legend unit)."));
    }
    const bool globe = this->map->model()->viewMode() == MapViewMode::Globe;
    this->label_icon_size->setText(this->mode != CanvasMode::Monitor || globe
        ? tr("Icon Size [%]") : tr("Icon Size"));
    if (this->mode == CanvasMode::Monitor)
    {
        this->combo_icon_size_unit->setToolTip(tr(
            "Meters keep a true world-space icon size. Pixels keep a compact visual size while preserving 3D perspective."));
        this->refreshIconSizeUnitPresentation();
        MapSymbologySlider *slider = static_cast<MapSymbologySlider *>(this->slider_icon_size);
        if (globe)
            slider->setDescription(tr("Scales 3D entity icons as in the Distribution Editor."));
        else if (this->icon_size_2d_unit == NetworkSymbologySizeUnit::Meters)
            slider->setDescription(tr("Sets the true world-space reservoir, tank, pump and valve icon size."));
        else
            slider->setDescription(tr("Sets the reservoir, tank, pump and valve icon size at the 3D focus depth or directly on the 2D map."));
    }
    else
        this->slider_icon_size->setToolTip(tr("Scales entity icons and 3D entity models."));
    this->refreshShortcutPresentation();
}

void MapNavigationWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && this->label_labels_title != nullptr)
        this->refreshLocalizedText();
}

MapLabelSettings MapNavigationWidget::labelSettings() const
{
    MapLabelSettings settings;
    settings.node_ids = this->check_label_node_ids != nullptr
        && this->check_label_node_ids->isChecked();
    settings.link_ids = this->check_label_link_ids != nullptr
        && this->check_label_link_ids->isChecked();
    settings.node_values = this->check_label_node_values != nullptr
        && this->check_label_node_values->isChecked();
    settings.link_values = this->check_label_link_values != nullptr
        && this->check_label_link_values->isChecked();
    return settings;
}

void MapNavigationWidget::refreshIconSizeUnitPresentation()
{
    if (this->mode != CanvasMode::Monitor || this->combo_icon_size_unit == nullptr
        || this->slider_icon_size == nullptr)
        return;

    const MapSymbologyLengthUnits::Presentation display =
        MapSymbologyLengthUnits::forContext(
            UnitProfileManager::instance().presentation());
    // The enum remains Meters: the selected profile changes only the label.
    this->combo_icon_size_unit->setItemText(0, display.unit);
    this->combo_icon_size_unit->setToolTip(tr(
        "World-space icon sizes are stored in metres and displayed in %1. "
        "Pixels retain their independent screen-space sizing.").arg(display.unit));

    const bool is_3d = this->map->model()->viewMode() == MapViewMode::Globe;
    if (is_3d)
        return; // The 3D Monitor uses a percentage, independent of unit profiles.
    const NetworkSymbologySizeUnit active_unit = this->icon_size_2d_unit;
    if (active_unit == NetworkSymbologySizeUnit::Meters)
    {
        MapSymbologySlider *slider = static_cast<MapSymbologySlider *>(this->slider_icon_size);
        slider->setDisplayPresentation(QStringLiteral(" ") + display.unit,
                                       display.metres_to_display, display.decimals);
    }
    // No slider value change, model write, or symbology change signal.
}

void MapNavigationWidget::sync3dIconsForViewMode(MapViewMode view_mode)
{
    if (this->check_3d_icons == nullptr)
        return;

    this->check_3d_icons->setVisible(view_mode == MapViewMode::Globe);
}

void MapNavigationWidget::syncIconSizeSliderForViewMode(MapViewMode view_mode)
{
    if (this->slider_icon_size == nullptr)
        return;

    const bool is_3d = view_mode == MapViewMode::Globe;
    if (this->mode == CanvasMode::Monitor)
    {
        if (this->label_icon_size != nullptr)
            this->label_icon_size->setText(is_3d
                ? tr("Icon Size [%]") : tr("Icon Size"));
        if (this->combo_icon_size_unit != nullptr)
            this->combo_icon_size_unit->setVisible(!is_3d);
        MapSymbologySlider *symbology_slider =
            static_cast<MapSymbologySlider *>(this->slider_icon_size);
        if (is_3d)
        {
            // Match the Editor globe overlay: 50–600%, default 100%,
            // with an independent value when switching between 2D and 3D.
            this->icon_size_3d_percent = qBound(50, this->icon_size_3d_percent, 600);
            symbology_slider->setConfiguration(
                50, 600, 100, this->icon_size_3d_percent,
                tr("Scales 3D entity icons as in the Distribution Editor."),
                QStringLiteral(" %"));
            emit signalIconSizeChanged(this->icon_size_3d_percent);
            return;
        }
        const NetworkSymbologySizeUnit unit = this->icon_size_2d_unit;
        const int remembered_value = unit == NetworkSymbologySizeUnit::Meters
            ? qRound(this->icon_size_2d_m) : this->icon_size_2d_px;
        if (unit == NetworkSymbologySizeUnit::Meters)
        {
            const MapSymbologyLengthUnits::Presentation display =
                MapSymbologyLengthUnits::forContext(
                    UnitProfileManager::instance().presentation());
            symbology_slider->setConfiguration(
                qRound(NetworkSymbologyMinimumIconSizeM),
                qRound(NetworkSymbologyMaximumIconSizeM),
                qRound(NetworkSymbologyDefaultIconSizeM),
                remembered_value,
                tr("Sets the true world-space reservoir, tank, pump and valve icon size."),
                QStringLiteral(" ") + display.unit,
                display.metres_to_display, display.decimals);
        }
        else
        {
            symbology_slider->setConfiguration(
                NetworkSymbologyMinimumIconSizePx,
                NetworkSymbologyMaximumIconSizePx,
                NetworkSymbologyDefaultIconSizePx,
                remembered_value,
                tr("Sets the reservoir, tank, pump and valve icon size at the 3D focus depth or directly on the 2D map."),
                QStringLiteral(" px"));
        }

        if (this->combo_icon_size_unit != nullptr)
        {
            const QSignalBlocker blocker(this->combo_icon_size_unit);
            const int combo_index = this->combo_icon_size_unit->findData(static_cast<int>(unit));
            if (combo_index >= 0)
                this->combo_icon_size_unit->setCurrentIndex(combo_index);
        }

        emit signalMonitorIconSizeChanged(unit, remembered_value);
        return;
    }

    const int maximum = view_mode == MapViewMode::Globe ? 600 : 250;
    const int remembered_value = view_mode == MapViewMode::Globe
        ? this->icon_size_3d_percent
        : this->icon_size_2d_percent;
    const int bounded_value = qBound(50, remembered_value, maximum);

    {
        const QSignalBlocker blocker(this->slider_icon_size);
        this->slider_icon_size->setRange(50, maximum);
        this->slider_icon_size->setValue(bounded_value);
    }

    if (view_mode == MapViewMode::Globe)
        this->icon_size_3d_percent = bounded_value;
    else
        this->icon_size_2d_percent = bounded_value;

    emit signalIconSizeChanged(bounded_value);
}

void MapNavigationWidget::activateMapProvider(MapProvider provider)
{
    this->map->changeMapProvider(provider);

    QWidget *focus_target = this->keyboard_focus_target;
    QTimer::singleShot(0, focus_target, [focus_target]
    {
        if (focus_target->isVisible())
            focus_target->setFocus(Qt::ShortcutFocusReason);
    });
}

void MapNavigationWidget::refreshShortcutPresentation()
{
    QStringList shortcuts;
    for (int provider_number = 1; provider_number <= ProviderShortcutCount; ++provider_number)
    {
        const int shortcut_index = provider_number - 1;
        const GuiShortcutId shortcut_id = ProviderShortcutIds[shortcut_index];
        QShortcut *shortcut = this->provider_shortcuts[shortcut_index];
        shortcut->setKey(guiShortcutRegistry().keySequence(shortcut_id));

        const QString provider_id = MapProviderCatalog::builtinId(provider_number);
        const int combo_index = this->combo_provider->findData(provider_id);
        shortcut->setEnabled(combo_index >= 0 && !shortcut->key().isEmpty());
        if (combo_index < 0)
            continue;

        const QString name = this->combo_provider->itemText(combo_index);
        const QString hotkey = guiShortcutPresentation(shortcut_id);
        this->combo_provider->setItemData(combo_index,
            tr("%1\nKeyboard shortcut: %2").arg(name, hotkey), Qt::ToolTipRole);
        shortcuts.append(QStringLiteral("%1: %2").arg(hotkey, name));
    }

    this->combo_provider->setToolTip(tr("Select a map layer advertised by the server."
        "\nKeyboard shortcuts:\n%1\nRight-click to edit shortcuts.").arg(
        shortcuts.join(QLatin1Char('\n'))));
}

void MapNavigationWidget::refreshProviderChoices()
{
    const QString active_id = this->map->model()->providerId();
    const QSignalBlocker blocker(this->combo_provider);
    this->combo_provider->clear();
    for (const MapProviderEntry &entry : mapProviderCatalog().providers())
    {
        if (entry.enabled)
        {
            this->combo_provider->addItem(entry.name, entry.id);
            this->combo_provider->setItemData(this->combo_provider->count() - 1,
                entry.name, Qt::ToolTipRole);
        }
    }
    this->combo_provider->setCurrentIndex(this->combo_provider->findData(active_id));
    refreshShortcutPresentation();
}

void MapNavigationWidget::mapProviderChange(MapProvider provider)
{
    Q_UNUSED(provider)
    refreshProviderChoices();
}

bool MapNavigationWidget::triggerProviderShortcut(const QKeySequence &pressed)
{
    if (!this->isEnabled())
        return false;

    for (int index = 0; index < ProviderShortcutCount; ++index)
    {
        const QShortcut *shortcut = this->provider_shortcuts[index];
        if (!shortcut->isEnabled() || shortcut->key().matches(pressed) != QKeySequence::ExactMatch)
            continue;
        activateMapProvider(static_cast<MapProvider>(index + 1));
        return true;
    }
    return false;
}

void MapNavigationWidget::setNetworkPresent(bool present)
{
    if (present == this->network_present || this->slider_map_opacity == nullptr)
        return;
    this->network_present = present;
    // valueChanged() emits signalSlideOpacityChanged() (an unchanged value
    // emits nothing: nothing changes either).
    this->slider_map_opacity->setValue(present ? NetworkOpacityPercent : 0);
}

bool MapNavigationWidget::triggerButtonShortcut(QWidget *root, const QKeyEvent *event)
{
    if (root == nullptr || event == nullptr || event->isAutoRepeat())
        return false;
    const QKeySequence pressed(event->keyCombination());
    const QList<MapNavigationWidget *> navigation_widgets = root->findChildren<MapNavigationWidget *>();
    for (MapNavigationWidget *navigation : navigation_widgets)
    {
        if (navigation->triggerProviderShortcut(pressed))
            return true;
    }
    const QList<QAbstractButton *> buttons = root->findChildren<QAbstractButton *>();
    for (QAbstractButton *button : buttons)
    {
        // What QShortcutMap would accept in the main window: an enabled
        // button whose own page is shown.
        if (!button->isEnabled() || !button->isVisibleTo(root) || button->shortcut().isEmpty())
            continue;
        if (button->shortcut().matches(pressed) != QKeySequence::ExactMatch)
            continue;
        button->click();
        return true;
    }
    return false;
}
