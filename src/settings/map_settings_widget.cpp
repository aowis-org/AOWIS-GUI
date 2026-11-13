#include "config/live_ui_text.h"
#include "settings/map_settings_widget.h"

#include "config/gui_configuration.h"
#ifdef AOWIS_STANDALONE
#include <aowis/map/settings_repository.h>
#endif
#include "map/core/gui_map_provider_catalog.h"
#include "map/core/gui_available_map_provider_directory.h"
#include "services/map_inherited_provider_client.h"
#include "services/map_server_transport_factory.h"
#ifdef AOWIS_STANDALONE
#include "services/standalone_map_provider_catalog_bridge.h"
#endif
#include "config/map_server_client_configuration.h"
#include "config/wireguard_profile_storage.h"
#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "settings/wireguard_profile_import.h"
#endif
#include "map/render/map_globe_terrain_levels.h"
#include "widgets/slider_number_control.h"

#include <QApplication>
#include <QCheckBox>
#include <QAbstractItemView>
#include <QByteArray>
#include <QEvent>
#include <QVariant>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QPointer>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QHash>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUuid>
#include <QSignalBlocker>
#include <QFont>
#include <functional>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>
#include <memory>
#include <QUrl>

namespace
{
// Debounce persistent writes while a slider is actively being dragged.
constexpr int SettingsSaveDebounceMs = 400;
constexpr double NavigationSensitivityMinimum = 0.1;
constexpr double NavigationSensitivityMaximum = 1.5;
constexpr double NavigationSensitivityStep = 0.05;


QLabel *sectionTitle(const char *text, QWidget *parent)
{
    QLabel *label = LiveUiText::caption<QLabel>(text, parent);
    QFont font = label->font();
    font.setPointSize(font.pointSize() + 2);
    font.setBold(true);
    label->setFont(font);
    return label;
}

QLabel *helpLabel(const char *text, QWidget *parent)
{
    QLabel *label = LiveUiText::caption<QLabel>(text, parent);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setItalic(true);
    label->setFont(font);
    return label;
}
}

MapSettingsWidget::MapSettingsWidget(QWidget *parent)
    : QWidget(parent)
{
    QScrollArea *scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    QWidget *content = new QWidget(scroll_area);
    QVBoxLayout *content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(24, 20, 24, 20);
    content_layout->setSpacing(20);

    QLabel *title = sectionTitle(QT_TRANSLATE_NOOP("LiveUiText", "Map Settings"), content);
    content_layout->addWidget(title);

    buildNavigationSection(content);
    buildPerformanceSection(content);
    buildServerSection(content);
#ifdef AOWIS_STANDALONE
    // Keep embedded MAP acquisition settings adjacent to the provider editor,
    // not stranded above the much larger server/provider section.
    buildEmbeddedCascadeSection(content);
#endif
    content_layout->addStretch(1);

    scroll_area->setWidget(content);

    QVBoxLayout *outer_layout = new QVBoxLayout(this);
    outer_layout->setContentsMargins(0, 0, 0, 0);
    outer_layout->addWidget(scroll_area);
}

MapSettingsWidget::~MapSettingsWidget()
{
#ifdef AOWIS_STANDALONE
    // Do not discard a pending edit if the settings widget is destroyed before
    // the debounce timeout (for example, during application shutdown).
    QWidget *focused_widget = QApplication::focusWidget();
    if (this->cascade_upstreams_table != nullptr && focused_widget != nullptr &&
        this->cascade_upstreams_table->isAncestorOf(focused_widget))
        focused_widget->clearFocus();
    if (this->cascade_save_debounce != nullptr && this->cascade_save_debounce->isActive())
    {
        this->cascade_save_debounce->stop();
        saveEmbeddedCascadeSettings();
    }
#endif
}

void MapSettingsWidget::buildNavigationSection(QWidget *parent_widget)
{
    QGroupBox *group = new QGroupBox(parent_widget);
    LiveUiText::bind(group, [group] { group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Map Controls"))); });
    QVBoxLayout *group_layout = new QVBoxLayout(group);

    group_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText", "Adjust map mouse controls. Mouse wheel zoom applies to both 2D and 3D (globe). "
                       "Defaults: wheel zoom 0.50x, 3D pan 0.80x, 3D orbit 0.80x."),
        group));

    QFormLayout *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    const GuiMapNavigationConfiguration &current = guiConfiguration().map_navigation;

    this->scroll_zoom_sensitivity_control = new SliderNumberControl(group);
    this->scroll_zoom_sensitivity_control->setRange(
        NavigationSensitivityMinimum, NavigationSensitivityMaximum);
    this->scroll_zoom_sensitivity_control->setSingleStep(NavigationSensitivityStep);
    this->scroll_zoom_sensitivity_control->setDecimals(2);
    this->scroll_zoom_sensitivity_control->setSliderStep(0.01);
    this->scroll_zoom_sensitivity_control->setSuffix(QStringLiteral("x"));
    this->scroll_zoom_sensitivity_control->setValue(current.scroll_zoom_sensitivity);
    form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Mouse wheel zoom speed"), group), this->scroll_zoom_sensitivity_control);

    this->mouse_3d_pan_sensitivity_control = new SliderNumberControl(group);
    this->mouse_3d_pan_sensitivity_control->setRange(
        NavigationSensitivityMinimum, NavigationSensitivityMaximum);
    this->mouse_3d_pan_sensitivity_control->setSingleStep(NavigationSensitivityStep);
    this->mouse_3d_pan_sensitivity_control->setDecimals(2);
    this->mouse_3d_pan_sensitivity_control->setSliderStep(0.01);
    this->mouse_3d_pan_sensitivity_control->setSuffix(QStringLiteral("x"));
    this->mouse_3d_pan_sensitivity_control->setValue(current.mouse_3d_pan_sensitivity);
    form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "3D mouse pan speed"), group), this->mouse_3d_pan_sensitivity_control);

    this->orbit_3d_sensitivity_control = new SliderNumberControl(group);
    this->orbit_3d_sensitivity_control->setRange(
        NavigationSensitivityMinimum, NavigationSensitivityMaximum);
    this->orbit_3d_sensitivity_control->setSingleStep(NavigationSensitivityStep);
    this->orbit_3d_sensitivity_control->setDecimals(2);
    this->orbit_3d_sensitivity_control->setSliderStep(0.01);
    this->orbit_3d_sensitivity_control->setSuffix(QStringLiteral("x"));
    this->orbit_3d_sensitivity_control->setValue(current.orbit_3d_sensitivity);
    form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "3D orbit speed"), group), this->orbit_3d_sensitivity_control);

    group_layout->addLayout(form);

    this->navigation_status = new QLabel(group);
    this->navigation_status->setWordWrap(true);
    group_layout->addWidget(this->navigation_status);

    QPushButton *restore_defaults = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Restore Defaults"), group);
    group_layout->addWidget(restore_defaults, 0, Qt::AlignLeft);

    this->navigation_save_debounce = new QTimer(this);
    this->navigation_save_debounce->setSingleShot(true);
    this->navigation_save_debounce->setInterval(SettingsSaveDebounceMs);
    connect(this->navigation_save_debounce, &QTimer::timeout,
            this, &MapSettingsWidget::saveNavigationSettingsNow);

    connect(this->scroll_zoom_sensitivity_control, &SliderNumberControl::valueChanged,
            this, [this](double) { scheduleNavigationSave(); });
    connect(this->mouse_3d_pan_sensitivity_control, &SliderNumberControl::valueChanged,
            this, [this](double) { scheduleNavigationSave(); });
    connect(this->orbit_3d_sensitivity_control, &SliderNumberControl::valueChanged,
            this, [this](double) { scheduleNavigationSave(); });

    connect(restore_defaults, &QPushButton::clicked, this, [this]
    {
        const GuiMapNavigationConfiguration defaults;
        this->scroll_zoom_sensitivity_control->setValue(defaults.scroll_zoom_sensitivity);
        this->mouse_3d_pan_sensitivity_control->setValue(defaults.mouse_3d_pan_sensitivity);
        this->orbit_3d_sensitivity_control->setValue(defaults.orbit_3d_sensitivity);
        saveNavigationSettingsNow();
    });

    static_cast<QVBoxLayout *>(parent_widget->layout())->addWidget(group);
}

void MapSettingsWidget::buildPerformanceSection(QWidget *parent_widget)
{
    QGroupBox *group = new QGroupBox(parent_widget);
    LiveUiText::bind(group, [group] { group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Map Performance"))); });
    QVBoxLayout *group_layout = new QVBoxLayout(group);

    group_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText", "These apply immediately to the 3D map renderer and are saved as you change them."),
        group));

    QFormLayout *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    const GuiMapPerformanceConfiguration &current = guiConfiguration().map_performance;

    this->terrain_detail_reduction_control = new SliderNumberControl(group);
    this->terrain_detail_reduction_control->setRange(
        0.0, double(MapGlobeTerrainMaximumDetailReduction));
    this->terrain_detail_reduction_control->setSingleStep(1.0);
    this->terrain_detail_reduction_control->setDecimals(0);
    this->terrain_detail_reduction_control->setSliderStep(1.0);
    this->terrain_detail_reduction_control->setValue(double(current.terrain_detail_reduction));
    form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Terrain detail reduction"), group), this->terrain_detail_reduction_control);
    form->addRow(QString(), helpLabel(QT_TRANSLATE_NOOP("LiveUiText", "0 draws every elevation sample the terrain source provides at the map's "
                       "level of detail: one terrain cell per sample spacing, about 4 px on "
                       "screen. Each step halves the terrain resolution (about 8, 16 and 32 px "
                       "per cell) and needs a quarter of the triangles. Zoomed in past the "
                       "source's own resolution, every setting shows the full source detail, "
                       "never more."),
        group));

    group_layout->addLayout(form);

    this->performance_status = new QLabel(group);
    this->performance_status->setWordWrap(true);
    group_layout->addWidget(this->performance_status);

    QPushButton *restore_defaults = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Restore Defaults"), group);
    group_layout->addWidget(restore_defaults, 0, Qt::AlignLeft);

    this->performance_save_debounce = new QTimer(this);
    this->performance_save_debounce->setSingleShot(true);
    this->performance_save_debounce->setInterval(SettingsSaveDebounceMs);
    connect(this->performance_save_debounce, &QTimer::timeout,
            this, &MapSettingsWidget::savePerformanceSettingsNow);

    connect(this->terrain_detail_reduction_control, &SliderNumberControl::valueChanged,
            this, [this](double) { schedulePerformanceSave(); });

    connect(restore_defaults, &QPushButton::clicked, this, [this]
    {
        const GuiMapPerformanceConfiguration defaults;
        this->terrain_detail_reduction_control->setValue(double(defaults.terrain_detail_reduction));
        savePerformanceSettingsNow();
    });

    static_cast<QVBoxLayout *>(parent_widget->layout())->addWidget(group);
}

void MapSettingsWidget::buildServerSection(QWidget *parent_widget)
{
#ifndef AOWIS_STANDALONE
    // The external MAP server connection is irrelevant in Standalone. Do not
    // present dead/disabled inputs next to the embedded provider editor.
    QGroupBox *group = new QGroupBox(parent_widget);
    LiveUiText::bind(group, [group] { group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Map Server Settings"))); });
    QVBoxLayout *group_layout = new QVBoxLayout(group);

#ifdef __EMSCRIPTEN__
    group_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText", "In this build, the map server connection is set by the hosting page and "
                       "cannot be changed here."),
        group));
#else
    group_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText", "Connection changes are saved when you finish editing a field "
                       "and apply immediately."), group));
#endif

    QFormLayout *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    const MapServerClientConfiguration &current = mapServerClientConfiguration();

    this->server_base_url_edit = new QLineEdit(current.base_url, group);
    this->server_base_url_edit->setPlaceholderText(QStringLiteral("http://aowis-server-map.localhost:80"));
    form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Base URL"), group), this->server_base_url_edit);

    this->server_api_key_edit = new QLineEdit(QString::fromUtf8(current.api_key), group);
    LiveUiText::bind(this->server_api_key_edit, [this] {
        this->server_api_key_edit->setPlaceholderText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "(none)")));
    });
    form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "API key"), group), this->server_api_key_edit);

    this->server_delete_api_key_edit = new QLineEdit(
        QString::fromUtf8(current.delete_api_key), group);
    LiveUiText::bind(this->server_delete_api_key_edit, [this] {
        this->server_delete_api_key_edit->setPlaceholderText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "(none)")));
    });
    form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Delete API key"), group), this->server_delete_api_key_edit);

    this->server_wireguard_combo = new QComboBox(group);
    this->server_wireguard_combo->setObjectName(QStringLiteral("mapServerWireGuardProfile"));
    this->reloadServerWireGuardProfiles(current.wireguard_profile_id);
    form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Connection"), group),
                 this->server_wireguard_combo);

    group_layout->addLayout(form);
#ifndef __EMSCRIPTEN__
    group_layout->addLayout(wireGuardProfileButtons(group, [this](const QString &profile_id)
    {
        this->reloadServerWireGuardProfiles(profile_id);
        this->saveServerSettings();
    }));
#endif

    this->server_status = new QLabel(group);
    this->server_status->setWordWrap(true);
    group_layout->addWidget(this->server_status);

    for (QLineEdit *field : {this->server_base_url_edit, this->server_api_key_edit,
                             this->server_delete_api_key_edit})
        connect(field, &QLineEdit::editingFinished,
                this, &MapSettingsWidget::saveServerSettings);
    connect(this->server_wireguard_combo, &QComboBox::currentIndexChanged,
            this, [this](int) { this->saveServerSettings(); });

#ifdef __EMSCRIPTEN__
    this->server_base_url_edit->setEnabled(false);
    this->server_api_key_edit->setEnabled(false);
    this->server_delete_api_key_edit->setEnabled(false);
    this->server_wireguard_combo->setEnabled(false);
#endif

    static_cast<QVBoxLayout *>(parent_widget->layout())->addWidget(group);
#endif

    QGroupBox *providers_group = new QGroupBox(parent_widget);
    LiveUiText::bind(providers_group, [providers_group] { providers_group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "2D Map Providers"))); });
    QVBoxLayout *providers_layout = new QVBoxLayout(providers_group);
#ifdef AOWIS_STANDALONE
    providers_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText",
        "Standalone providers are editable and saved locally. The cascading upstreams and low-bandwidth settings are just below this table."), providers_group));
#endif
    providers_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText", "Provider changes are saved automatically. Each provider can fall back to another "
        "when you zoom beyond its maximum level. Fallbacks can form a chain: "
        "A → B → C, as long as each source offers more detail. "
        "Your fallback choices are personal; they don't change the server's providers. "
        "With no further fallback, the highest-resolution tiles are smoothly enlarged."), providers_group));
    this->provider_table = new QTableWidget(0, 6, providers_group);
    LiveUiText::bind(this->provider_table, [this] {
        this->provider_table->setHorizontalHeaderLabels({LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "On")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Display name")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Tile URL")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Min")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Max")), LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "My fallback"))});
    });
    this->provider_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    this->provider_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    this->provider_table->setMinimumHeight(220);
    providers_layout->addWidget(this->provider_table);
    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *add = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Add provider"), providers_group);
    QPushButton *remove = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Remove selected"), providers_group);
    QPushButton *reload = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Reload from server"), providers_group);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(reload);
    providers_layout->addLayout(buttons);
    this->provider_status = new QLabel(providers_group);
    this->provider_status->setWordWrap(true);
    providers_layout->addWidget(this->provider_status);
    connect(add, &QPushButton::clicked, this, [this]
    {
        this->loading_provider_table = true;
        addProviderRow(QUuid::createUuid().toString(QUuid::WithoutBraces),
                       tr("New map provider"),
                       QString(), 0, 19, true);
        const int added_row = this->provider_table->rowCount() - 1;
        this->provider_table->item(added_row, 2)->setData(Qt::UserRole + 1, true);
        this->provider_table->item(added_row, 2)->setToolTip(
            tr("Enter an XYZ tile URL template such as "
                           "https://tiles.example.org/{z}/{x}/{y}.png"));
        this->loading_provider_table = false;
        refreshFallbackChoices();
        this->provider_table->setCurrentCell(added_row, 2);
        this->provider_table->editItem(this->provider_table->item(added_row, 2));
        this->setProviderStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", 
            "Enter the new provider's tile URL. Changes save when the row is complete."));
    });
    connect(remove, &QPushButton::clicked, this, [this]
    {
        const int row = this->provider_table->currentRow();
        if (row >= 0)
        {
            const QString uuid = this->provider_table->item(row, 1)->data(Qt::UserRole).toString();
            this->pending_fallback_choices.remove(uuid);
            this->loading_provider_table = true;
            this->provider_table->removeRow(row);
            this->loading_provider_table = false;
            refreshFallbackChoices();
            scheduleProviderSave();
        }
    });
    this->provider_save_debounce = new QTimer(this);
    this->provider_save_debounce->setSingleShot(true);
    this->provider_save_debounce->setInterval(650);
    connect(this->provider_save_debounce, &QTimer::timeout,
            this, &MapSettingsWidget::saveProviderEdits);
    connect(this->provider_table, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *)
    {
        if (this->loading_provider_table) return;
        refreshFallbackChoices();
        scheduleProviderSave();
    });
    connect(reload, &QPushButton::clicked, this, [this]
    {
        // Reload is an explicit request to discard uncommitted table edits.
        this->provider_save_debounce->stop();
        this->provider_edits_pending = false;
        mapProviderCatalog().refresh();
    });
    connect(&mapProviderCatalog(), &MapProviderCatalog::catalogChanged, this, [this, add, remove]
    {
        this->provider_save_in_flight = false;
        // New provider UUIDs and preferences made while zoom limits were being
        // edited can only be committed after the server confirms the catalog.
        for (QHash<QString, QString>::iterator iterator =
                 this->pending_fallback_choices.begin();
             iterator != this->pending_fallback_choices.end(); )
        {
            if (mapProviderCatalog().setFallback(iterator.key(), iterator.value()))
                iterator = this->pending_fallback_choices.erase(iterator);
            else
                ++iterator;
        }
        if (this->provider_edits_pending)
            this->provider_save_debounce->start();
        else
        {
            // A server response must not destroy an active cell editor with
            // uncommitted text (which QTableWidget has not emitted yet).
            const QWidget *focused = QApplication::focusWidget();
            if (focused == nullptr || !this->provider_table->isAncestorOf(focused))
                refreshProviderTable();
        }
        this->provider_table->setEnabled(true);
        add->setEnabled(true);
        remove->setEnabled(true);
    });
    connect(&mapProviderCatalog(), &MapProviderCatalog::errorOccurred, this,
            [this](const QString &message)
    {
        if (this->provider_save_in_flight)
        {
            this->provider_save_in_flight = false;
            this->provider_edits_pending = true;
        }
        this->provider_status->setProperty("aowisLocalizedSource", QVariant());
        this->provider_status->setText(message);
    });
    connect(&mapProviderCatalog(), &MapProviderCatalog::providersSaved, this, [this]
    {
        this->setProviderStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", "Provider changes saved."));
    });
    refreshProviderTable();
    this->provider_table->setEnabled(mapProviderCatalog().serverLoaded());
    add->setEnabled(mapProviderCatalog().serverLoaded());
    remove->setEnabled(mapProviderCatalog().serverLoaded());

#ifdef __EMSCRIPTEN__
    this->setProviderStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", 
        "Provider edits affect the server; per-user fallback persistence is supported in native GUI builds."));
#endif
    // Inherited providers are shown separately from the editable local catalog:
    // the PUT /providers payload must never contain upstream-owned definitions.
    QGroupBox *inherited_group = new QGroupBox(parent_widget);
    LiveUiText::bind(inherited_group, [inherited_group]
    {
        inherited_group->setTitle(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "MAP provider acquisition and upstreams")));
    });
    QVBoxLayout *inherited_layout = new QVBoxLayout(inherited_group);
    inherited_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText",
        "This section lists only additional providers inherited from upstream MAP servers. "
        "Providers already defined locally appear in the editor above, even if an upstream "
        "also offers them. Inherited definitions are read-only; an administrator key "
        "is required to change their local availability."), inherited_group));
    this->inherited_provider_table = new QTableWidget(0, 5, inherited_group);
    LiveUiText::bind(this->inherited_provider_table, [this]
    {
        this->inherited_provider_table->setHorizontalHeaderLabels({
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "On")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Display name")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Tile URL")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Zoom")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Upstream servers"))});
    });
    this->inherited_provider_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->inherited_provider_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->inherited_provider_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    this->inherited_provider_table->setMinimumHeight(140);
    inherited_layout->addWidget(this->inherited_provider_table);
    this->inherited_provider_status = new QLabel(inherited_group);
    this->inherited_provider_status->setWordWrap(true);
    inherited_layout->addWidget(this->inherited_provider_status);

    inherited_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText",
        "Upstream routing is configured separately for each provider, including locally "
        "defined providers also offered upstream. Select the permitted MAP servers and "
        "their priority. Choose Automatic (MAP then permitted origin), Cascade only, "
        "or Direct only for locally defined providers if permitted. Changes are "
        "local and saved automatically."), inherited_group));
    this->upstream_routing_table = new QTableWidget(0, 5, inherited_group);
    LiveUiText::bind(this->upstream_routing_table, [this]
    {
        this->upstream_routing_table->setHorizontalHeaderLabels({
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Provider")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Advertised by")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Preferred order")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Acquisition")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Routing"))});
    });
    this->upstream_routing_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->upstream_routing_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->upstream_routing_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    this->upstream_routing_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    this->upstream_routing_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    this->upstream_routing_table->setMinimumHeight(140);
    inherited_layout->addWidget(this->upstream_routing_table);
    this->inherited_provider_client = new MapInheritedProviderClient(this);
    connect(this->inherited_provider_client, &MapInheritedProviderClient::catalogChanged,
            this, &MapSettingsWidget::refreshInheritedProviderTable);
    connect(this->inherited_provider_client, &MapInheritedProviderClient::errorOccurred,
            this, [this](const QString &error)
    {
        this->inherited_provider_status->setText(error);
    });
    connect(this->inherited_provider_client, &MapInheritedProviderClient::updateFinished,
            this, [this](bool success, const QString &error)
    {
        if (!success) this->inherited_provider_status->setText(error);
        else guiAvailableMapProviderDirectory().refresh();
    });
    connect(this->inherited_provider_table, &QTableWidget::itemChanged,
            this, [this](QTableWidgetItem *item)
    {
        if (this->loading_provider_table || item == nullptr || item->column() != 0 ||
            this->inherited_provider_client == nullptr ||
            !this->inherited_provider_client->canEdit() ||
            this->inherited_provider_client->updating()) return;
        this->inherited_provider_table->setEnabled(false);
        this->inherited_provider_client->setEnabled(item->data(Qt::UserRole).toString(),
            item->checkState() == Qt::Checked);
    });
    connect(this->inherited_provider_client, &MapInheritedProviderClient::updateFinished,
            this, [this](bool, const QString &)
    {
        this->inherited_provider_table->setEnabled(true);
    });
#ifdef AOWIS_STANDALONE
    this->inherited_provider_client->setServer(
        StandaloneMapProviderCatalogBridge::instance().catalogServer());
    connect(&StandaloneMapProviderCatalogBridge::instance(),
        &StandaloneMapProviderCatalogBridge::providersChanged, this, [this]
    {
        this->inherited_provider_client->refresh();
    });
    this->inherited_provider_client->refresh();
#else
    whenGuiMapServerReachable(this, [this](const MapServerRestConfiguration &server)
    {
        this->inherited_provider_client->setServer(server);
        this->inherited_provider_client->refresh();
    });
#endif
    connect(reload, &QPushButton::clicked, this, [this]
    {
        this->inherited_provider_client->refresh();
    });
    refreshInheritedProviderTable();
    static_cast<QVBoxLayout *>(parent_widget->layout())->addWidget(providers_group);
    static_cast<QVBoxLayout *>(parent_widget->layout())->addWidget(inherited_group);
}

#ifdef AOWIS_STANDALONE
void MapSettingsWidget::appendCascadeUpstream(
    const QString &name, const QString &url, const QString &api_key,
    const QString &wireguard_profile_id)
{
    // Rows are also rebuilt while reordering. Never persist an intermediate
    // table containing only some of the rows.
    const QSignalBlocker table_blocker(this->cascade_upstreams_table);
    const int row = this->cascade_upstreams_table->rowCount();
    this->cascade_upstreams_table->insertRow(row);
    this->cascade_upstreams_table->setItem(row, 0, new QTableWidgetItem(name));
    this->cascade_upstreams_table->setItem(row, 1, new QTableWidgetItem(url));
    QLineEdit *key_edit = new QLineEdit(api_key, this->cascade_upstreams_table);
    key_edit->setEchoMode(QLineEdit::Password);
    this->cascade_upstreams_table->setCellWidget(row, 2, key_edit);
    connect(key_edit, &QLineEdit::textChanged, this,
            [this](const QString &) { scheduleEmbeddedCascadeSave(); });
    QComboBox *connection = new QComboBox(this->cascade_upstreams_table);
    connection->setObjectName(QStringLiteral("embeddedMapCascadeConnection"));
    fillWireGuardConnectionCombo(connection, wireguard_profile_id);
    this->cascade_upstreams_table->setCellWidget(row, 3, connection);
    connect(connection, &QComboBox::currentIndexChanged, this,
            [this](int) { scheduleEmbeddedCascadeSave(); });
}

QString MapSettingsWidget::cascadeUpstreamProfile(int row) const
{
    const QComboBox *connection =
        qobject_cast<QComboBox *>(this->cascade_upstreams_table->cellWidget(row, 3));
    return connection != nullptr ? connection->currentData().toString() : QString();
}

void MapSettingsWidget::reloadCascadeUpstreamProfiles()
{
    if (this->cascade_upstreams_table == nullptr) return;
    for (int row = 0; row < this->cascade_upstreams_table->rowCount(); ++row)
    {
        QComboBox *connection =
            qobject_cast<QComboBox *>(this->cascade_upstreams_table->cellWidget(row, 3));
        if (connection == nullptr) continue;
        const QSignalBlocker blocker(connection);
        fillWireGuardConnectionCombo(connection, connection->currentData().toString());
    }
}

void MapSettingsWidget::moveCascadeUpstream(int direction)
{
    const int selected = this->cascade_upstreams_table->currentRow();
    const int destination = selected + direction;
    if (selected < 0 || destination < 0 || destination >= this->cascade_upstreams_table->rowCount())
        return;

    QVector<Aowis::Map::MapCascadeUpstream> entries;
    for (int row = 0; row < this->cascade_upstreams_table->rowCount(); ++row)
    {
        const QLineEdit *key = qobject_cast<QLineEdit *>(this->cascade_upstreams_table->cellWidget(row, 2));
        Aowis::Map::MapCascadeUpstream entry;
        entry.name = this->cascade_upstreams_table->item(row, 0)->text();
        entry.url = QUrl(this->cascade_upstreams_table->item(row, 1)->text(), QUrl::StrictMode);
        entry.api_key = key != nullptr ? key->text().toUtf8() : QByteArray();
        entry.wireguard_profile_id = cascadeUpstreamProfile(row);
        entries.append(entry);
    }
    std::swap(entries[selected], entries[destination]);
    this->cascade_upstreams_table->setRowCount(0);
    for (const Aowis::Map::MapCascadeUpstream &entry : entries)
        appendCascadeUpstream(entry.name, entry.url.toString(QUrl::FullyEncoded),
                              QString::fromUtf8(entry.api_key), entry.wireguard_profile_id);
    this->cascade_upstreams_table->setCurrentCell(destination, 0);
    scheduleEmbeddedCascadeSave();
}

void MapSettingsWidget::buildEmbeddedCascadeSection(QWidget *parent_widget)
{
    QGroupBox *group = new QGroupBox(parent_widget);
    LiveUiText::bind(group, [group] {
        group->setTitle(LiveUiText::translate(
            QT_TRANSLATE_NOOP("LiveUiText", "Embedded MAP cascading")));
    });
    QVBoxLayout *layout = new QVBoxLayout(group);
    layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText",
        "Standalone only: each missing map or terrain tile is requested from these MAP servers "
        "in order. Local cache is checked first. Prefer HTTPS for API keys. "
        "Each upstream is reached directly or through a WireGuard profile of "
        "Settings → WireGuard VPN; a WireGuard upstream is never contacted directly. "
        "Changes apply immediately."), group));

    this->cascade_upstreams_table = new QTableWidget(0, 4, group);
    this->cascade_upstreams_table->setObjectName(QStringLiteral("embeddedMapCascadeUpstreams"));
    LiveUiText::bind(this->cascade_upstreams_table, [this] {
        this->cascade_upstreams_table->setHorizontalHeaderLabels({
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Name")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Upstream MAP URL")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Read API key")),
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Connection"))});
    });
    this->cascade_upstreams_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    this->cascade_upstreams_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    this->cascade_upstreams_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    this->cascade_upstreams_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    this->cascade_upstreams_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->cascade_upstreams_table->setSelectionMode(QAbstractItemView::SingleSelection);
    this->cascade_upstreams_table->setMinimumHeight(170);
    layout->addWidget(this->cascade_upstreams_table);

    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *add = LiveUiText::caption<QPushButton>(
        QT_TRANSLATE_NOOP("LiveUiText", "Add upstream"), group);
    QPushButton *remove = LiveUiText::caption<QPushButton>(
        QT_TRANSLATE_NOOP("LiveUiText", "Remove selected"), group);
    QPushButton *move_up = LiveUiText::caption<QPushButton>(
        QT_TRANSLATE_NOOP("LiveUiText", "Move up"), group);
    QPushButton *move_down = LiveUiText::caption<QPushButton>(
        QT_TRANSLATE_NOOP("LiveUiText", "Move down"), group);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(move_up);
    buttons->addWidget(move_down);
    buttons->addStretch();
    layout->addLayout(buttons);
    layout->addLayout(wireGuardProfileButtons(group, [this](const QString &profile_id)
    {
        // A new profile goes to the selected upstream (or the only one).
        int row = this->cascade_upstreams_table->currentRow();
        if (row < 0 && this->cascade_upstreams_table->rowCount() == 1) row = 0;
        this->reloadCascadeUpstreamProfiles();
        if (row < 0) return;
        QComboBox *connection =
            qobject_cast<QComboBox *>(this->cascade_upstreams_table->cellWidget(row, 3));
        if (connection == nullptr) return;
        const int index = connection->findData(profile_id);
        if (index >= 0) connection->setCurrentIndex(index); // Saves.
    }));
    connect(add, &QPushButton::clicked, this, [this] {
        if (this->cascade_upstreams_table->rowCount() >= 32)
        {
            this->cascade_status->setText(tr("At most 32 MAP upstream servers are supported."));
            return;
        }
        int index = 1;
        QString name;
        do
        {
            name = QStringLiteral("upstream%1").arg(index++);
            bool found = false;
            for (int row = 0; row < this->cascade_upstreams_table->rowCount(); ++row)
            {
                if (this->cascade_upstreams_table->item(row, 0)->text() == name)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
        } while (true);
        appendCascadeUpstream(name, QString(), QString(), QString());
        scheduleEmbeddedCascadeSave();
        const int row = this->cascade_upstreams_table->rowCount() - 1;
        this->cascade_upstreams_table->setCurrentCell(row, 1);
        this->cascade_upstreams_table->editItem(this->cascade_upstreams_table->item(row, 1));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = this->cascade_upstreams_table->currentRow();
        if (row >= 0)
        {
            this->cascade_upstreams_table->removeRow(row);
            scheduleEmbeddedCascadeSave();
        }
    });
    connect(move_up, &QPushButton::clicked, this, [this] { moveCascadeUpstream(-1); });
    connect(move_down, &QPushButton::clicked, this, [this] { moveCascadeUpstream(1); });

    this->cascade_allow_direct_fetch = LiveUiText::caption<QCheckBox>(
        QT_TRANSLATE_NOOP("LiveUiText", "Allow direct OSM/imagery/Copernicus downloads as last resort"), group);
    layout->addWidget(this->cascade_allow_direct_fetch);
    this->cascade_poor_internet = LiveUiText::caption<QCheckBox>(
        QT_TRANSLATE_NOOP("LiveUiText", "Poor Internet mode (optimized MAP-to-MAP transfers)"), group);
    layout->addWidget(this->cascade_poor_internet);
    this->cascade_adaptive_compression = LiveUiText::caption<QCheckBox>(
        QT_TRANSLATE_NOOP("LiveUiText", "Adaptive PNG/JPEG selection"), group);
    layout->addWidget(this->cascade_adaptive_compression);
    QFormLayout *quality_form = new QFormLayout();
    this->cascade_jpeg_quality = new QSpinBox(group);
    this->cascade_jpeg_quality->setRange(1, 100);
    quality_form->addRow(LiveUiText::caption<QLabel>(
        QT_TRANSLATE_NOOP("LiveUiText", "JPEG quality"), group), this->cascade_jpeg_quality);
    layout->addLayout(quality_form);

    Aowis::Map::MapCascadeConfig stored;
    QString error;
    std::unique_ptr<Aowis::Map::SettingsRepository> repository =
        Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&error);
    const bool valid = repository != nullptr &&
        repository->readCascade(&stored, &this->cascade_revision, &error);
    if (!valid)
        stored.allow_direct_fetch = false;
    for (const Aowis::Map::MapCascadeUpstream &upstream : stored.upstreams)
        appendCascadeUpstream(upstream.name, upstream.url.toString(QUrl::FullyEncoded),
                              QString::fromUtf8(upstream.api_key), upstream.wireguard_profile_id);
    this->cascade_allow_direct_fetch->setChecked(stored.allow_direct_fetch);
    this->cascade_poor_internet->setChecked(stored.poor_internet_mode);
    this->cascade_adaptive_compression->setChecked(stored.adaptive_compression);
    this->cascade_jpeg_quality->setValue(stored.jpeg_quality);

    this->cascade_status = new QLabel(group);
    this->cascade_status->setObjectName(QStringLiteral("embeddedMapCascadeStatus"));
    this->cascade_status->setWordWrap(true);
    layout->addWidget(this->cascade_status);
    if (!valid)
        this->cascade_status->setText(tr("Invalid stored cascade settings: %1. Remote fetching is disabled until they are corrected here.").arg(error));

    // Match the rest of Map Settings: save automatically after an edit settles.
    // Connect only after loading persisted values to avoid rewriting them at startup.
    this->cascade_save_debounce = new QTimer(this);
    this->cascade_save_debounce->setSingleShot(true);
    this->cascade_save_debounce->setInterval(500);
    connect(this->cascade_save_debounce, &QTimer::timeout,
            this, &MapSettingsWidget::saveEmbeddedCascadeSettings);
    connect(this->cascade_upstreams_table, &QTableWidget::itemChanged,
            this, [this](QTableWidgetItem *) { scheduleEmbeddedCascadeSave(); });
    connect(this->cascade_allow_direct_fetch, &QCheckBox::toggled,
            this, [this](bool) { scheduleEmbeddedCascadeSave(); });
    connect(this->cascade_poor_internet, &QCheckBox::toggled,
            this, [this](bool) { scheduleEmbeddedCascadeSave(); });
    connect(this->cascade_adaptive_compression, &QCheckBox::toggled,
            this, [this](bool) { scheduleEmbeddedCascadeSave(); });
    connect(this->cascade_jpeg_quality, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { scheduleEmbeddedCascadeSave(); });

    static_cast<QVBoxLayout *>(parent_widget->layout())->addWidget(group);
}

void MapSettingsWidget::scheduleEmbeddedCascadeSave()
{
    if (this->cascade_save_debounce == nullptr)
        return;
    const char *message = QT_TRANSLATE_NOOP(
        "MapSettingsWidget", "Saving embedded MAP settings automatically...");
    this->cascade_status->setProperty("aowisLocalizedSource", QByteArray(message));
    this->cascade_status->setText(tr(message));
    this->cascade_save_debounce->start();
}

void MapSettingsWidget::saveEmbeddedCascadeSettings()
{
    Aowis::Map::MapCascadeConfig configuration;
    configuration.allow_direct_fetch = this->cascade_allow_direct_fetch->isChecked();
    configuration.poor_internet_mode = this->cascade_poor_internet->isChecked();
    configuration.adaptive_compression = this->cascade_adaptive_compression->isChecked();
    configuration.jpeg_quality = this->cascade_jpeg_quality->value();
    for (int row = 0; row < this->cascade_upstreams_table->rowCount(); ++row)
    {
        const QLineEdit *key = qobject_cast<QLineEdit *>(this->cascade_upstreams_table->cellWidget(row, 2));
        Aowis::Map::MapCascadeUpstream upstream;
        upstream.name = this->cascade_upstreams_table->item(row, 0)->text().trimmed();
        upstream.url = QUrl(this->cascade_upstreams_table->item(row, 1)->text().trimmed(),
                            QUrl::StrictMode);
        upstream.api_key = key != nullptr ? key->text().toUtf8() : QByteArray();
        upstream.wireguard_profile_id = cascadeUpstreamProfile(row);
        configuration.upstreams.append(upstream);
    }
    QString error;
    // Saving a corrected record must remain possible even when legacy
    // migration failed: the MAP API validates this complete replacement.
    std::unique_ptr<Aowis::Map::SettingsRepository> repository =
        Aowis::Map::SettingsRepository::openForCurrentUser(
            QStringLiteral("standalone-map"), &error);
    QString updated_revision;
    if (repository == nullptr ||
        !repository->saveCascade(configuration, this->cascade_revision,
                                 &updated_revision, &error))
    {
        // Incomplete upstream rows are expected while someone is typing.
        // Validation protects the previous valid configuration until the row
        // becomes complete; never silently persist a partial cascade.
        this->cascade_status->setProperty("aowisLocalizedSource", QByteArray());
        this->cascade_status->setText(tr("Not saved yet: %1. Correct the settings to save automatically.").arg(error));
        return;
    }
    this->cascade_revision = updated_revision;
    // Apply live: the embedded map server, terrain and provider discovery.
    QString apply_error;
    if (!StandaloneMapProviderCatalogBridge::instance().reloadCascade(&apply_error))
    {
        this->cascade_status->setProperty("aowisLocalizedSource", QByteArray());
        this->cascade_status->setText(
            tr("Saved, but not applied: %1. The previous settings stay in use.").arg(apply_error));
        return;
    }
    const char *message = QT_TRANSLATE_NOOP(
        "MapSettingsWidget", "Embedded MAP settings saved and applied.");
    this->cascade_status->setProperty("aowisLocalizedSource", QByteArray(message));
    this->cascade_status->setText(tr(message));
}
#endif

void MapSettingsWidget::addProviderRow(const QString &uuid, const QString &name,
                                      const QString &url, int minimum_zoom,
                                      int maximum_zoom, bool enabled,
                                      const QString &fallback_id)
{
    const int row = this->provider_table->rowCount();
    this->provider_table->insertRow(row);
    QTableWidgetItem *enabled_item = new QTableWidgetItem();
    enabled_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
    enabled_item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    this->provider_table->setItem(row, 0, enabled_item);
    QTableWidgetItem *name_item = new QTableWidgetItem(name);
    name_item->setData(Qt::UserRole, uuid);
    name_item->setToolTip(tr("Provider UUID: %1").arg(uuid));
    this->provider_table->setItem(row, 1, name_item);
    this->provider_table->setItem(row, 2, new QTableWidgetItem(url));
    QSpinBox *minimum = new QSpinBox(this->provider_table);
    minimum->setRange(0, 30);
    minimum->setValue(minimum_zoom);
    this->provider_table->setCellWidget(row, 3, minimum);
    QSpinBox *maximum = new QSpinBox(this->provider_table);
    maximum->setRange(0, 30);
    maximum->setValue(maximum_zoom);
    this->provider_table->setCellWidget(row, 4, maximum);
    QComboBox *fallback = new QComboBox(this->provider_table);
    fallback->addItem(tr("None (enlarge available tiles)"), QString());
    for (const MapProviderEntry &entry : mapProviderCatalog().providers())
    {
        if (entry.enabled && entry.id != uuid && entry.maximum_zoom > maximum_zoom &&
            entry.minimum_zoom <= maximum_zoom + 1)
            fallback->addItem(entry.name, entry.id);
    }
    const int chosen = fallback->findData(fallback_id);
    if (chosen >= 0) fallback->setCurrentIndex(chosen);
    this->provider_table->setCellWidget(row, 5, fallback);
    connect(maximum, &QSpinBox::valueChanged, this,
            [this](int)
    {
        if (this->loading_provider_table) return;
        refreshFallbackChoices();
        scheduleProviderSave();
    });
    connect(fallback, &QComboBox::currentIndexChanged, this, [this, fallback, uuid](int)
    {
        if (this->loading_provider_table) return;
        for (int row_index = 0; row_index < this->provider_table->rowCount(); ++row_index)
        {
            const QTableWidgetItem *item = this->provider_table->item(row_index, 1);
            if (item != nullptr && item->data(Qt::UserRole).toString() == uuid &&
                this->provider_table->cellWidget(row_index, 5) == fallback)
            {
                rememberFallbackChange(row_index);
                return;
            }
        }
    });
    connect(minimum, &QSpinBox::valueChanged, this,
            [this](int)
    {
        if (this->loading_provider_table) return;
        refreshFallbackChoices();
        scheduleProviderSave();
    });
}

void MapSettingsWidget::refreshInheritedProviderTable()
{
    if (this->inherited_provider_table == nullptr ||
        this->inherited_provider_client == nullptr) return;
    const QSignalBlocker blocker(this->inherited_provider_table);
    const bool old_loading = this->loading_provider_table;
    this->loading_provider_table = true;
    this->inherited_provider_table->setRowCount(0);
    for (const MapInheritedProviderEntry &entry : this->inherited_provider_client->providers())
    {
        const int row = this->inherited_provider_table->rowCount();
        this->inherited_provider_table->insertRow(row);
        QTableWidgetItem *enabled = new QTableWidgetItem();
        enabled->setCheckState(entry.enabled ? Qt::Checked : Qt::Unchecked);
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (this->inherited_provider_client->canEdit()) flags |= Qt::ItemIsUserCheckable;
        enabled->setFlags(flags);
        enabled->setData(Qt::UserRole, entry.id);
        this->inherited_provider_table->setItem(row, 0, enabled);
        this->inherited_provider_table->setItem(row, 1, new QTableWidgetItem(entry.name));
        this->inherited_provider_table->setItem(row, 2, new QTableWidgetItem(entry.url));
        this->inherited_provider_table->setItem(row, 3,
            new QTableWidgetItem(QStringLiteral("%1–%2")
                .arg(entry.minimum_zoom).arg(entry.maximum_zoom)));
        this->inherited_provider_table->setItem(row, 4,
            new QTableWidgetItem(entry.upstreams.join(QStringLiteral(", "))));
    }
    this->loading_provider_table = old_loading;
    this->inherited_provider_table->setEnabled(!this->inherited_provider_client->updating());
    if (!this->inherited_provider_client->loaded())
        this->inherited_provider_status->setText(tr("Waiting for MAP's available-provider catalog."));
    else if (this->inherited_provider_client->providers().isEmpty())
    {
        const int local = this->inherited_provider_client->localProviderCount();
        const int also_upstream = this->inherited_provider_client->locallyDefinedWithUpstreams();
        if (also_upstream > 0)
            this->inherited_provider_status->setText(
                tr("MAP advertises %1 local providers; %2 are also offered upstream. "
                   "No additional providers to inherit.").arg(local).arg(also_upstream));
        else if (local > 0)
            this->inherited_provider_status->setText(
                tr("MAP advertises %1 local providers. No additional upstream providers "
                   "are available.").arg(local));
        else
            this->inherited_provider_status->setText(
                tr("MAP advertises no providers. Check its local catalog and upstream settings."));
    }
    else if (!this->inherited_provider_client->canEdit())
        this->inherited_provider_status->setText(tr("Read-only: configure the MAP admin/delete key to change inherited provider availability."));
    else
        this->inherited_provider_status->setText(tr("Inherited availability changes are saved immediately on this MAP server."));

    // This is the read-only available catalog, never the editable provider
    // definitions. Local and inherited providers may both have MAP upstreams.
    this->upstream_routing_table->setRowCount(0);
    for (const MapProviderRouteEntry &route : this->inherited_provider_client->routes())
    {
        const int row = this->upstream_routing_table->rowCount();
        this->upstream_routing_table->insertRow(row);
        this->upstream_routing_table->setItem(row, 0, new QTableWidgetItem(route.name));
        this->upstream_routing_table->setItem(row, 1,
            new QTableWidgetItem(route.upstreams.join(QStringLiteral(", "))));
        this->upstream_routing_table->setItem(row, 2,
            new QTableWidgetItem(route.selected_upstreams.join(QStringLiteral(" → "))));
        QComboBox *mode = new QComboBox(this->upstream_routing_table);
        mode->addItem(tr("Automatic"), QStringLiteral("automatic"));
        mode->addItem(tr("Cascade only"), QStringLiteral("cascade"));
        if (route.direct_fetch_allowed)
            mode->addItem(tr("Direct only"), QStringLiteral("direct"));
        const int current = mode->findData(route.acquisition_mode);
        mode->setCurrentIndex(current < 0 ? 0 : current);
        mode->setEnabled(this->inherited_provider_client->canEdit() &&
                         !this->inherited_provider_client->updating());
        this->upstream_routing_table->setCellWidget(row, 3, mode);
        connect(mode, &QComboBox::currentIndexChanged, this, [this, mode, id = route.id](int index)
        {
            if (index < 0 || this->inherited_provider_client == nullptr) return;
            this->inherited_provider_client->setAcquisitionMode(
                id, mode->itemData(index).toString());
        });
        QPushButton *edit = new QPushButton(tr("Configure…"), this->upstream_routing_table);
        edit->setEnabled(!route.upstreams.isEmpty() &&
                         this->inherited_provider_client->canEdit() &&
                         !this->inherited_provider_client->updating());
        this->upstream_routing_table->setCellWidget(row, 4, edit);
        connect(edit, &QPushButton::clicked, this, [this, id = route.id]
        {
            editProviderUpstreamRoute(id);
        });
    }
}

void MapSettingsWidget::editProviderUpstreamRoute(const QString &id)
{
    if (this->inherited_provider_client == nullptr ||
        !this->inherited_provider_client->canEdit() ||
        this->inherited_provider_client->updating()) return;
    MapProviderRouteEntry chosen;
    bool found = false;
    for (const MapProviderRouteEntry &entry : this->inherited_provider_client->routes())
        if (entry.id == id) { chosen = entry; found = true; break; }
    if (!found || chosen.upstreams.isEmpty()) return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("MAP upstream priority — %1").arg(chosen.name));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QLabel *explanation = new QLabel(tr(
        "Check the MAP servers allowed for this provider. Move checked servers up or down "
        "to set their priority. Changes save automatically; Reset restores global order."),
        &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    QListWidget *list = new QListWidget(&dialog);
    QStringList ordered = chosen.selected_upstreams;
    for (const QString &name : chosen.upstreams)
        if (!ordered.contains(name)) ordered.append(name);
    for (const QString &name : ordered)
    {
        QListWidgetItem *item = new QListWidgetItem(name, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(chosen.selected_upstreams.contains(name) ?
                            Qt::Checked : Qt::Unchecked);
    }
    list->setCurrentRow(0);
    layout->addWidget(list);
    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *up = new QPushButton(tr("Move up"), &dialog);
    QPushButton *down = new QPushButton(tr("Move down"), &dialog);
    QPushButton *reset = new QPushButton(tr("Reset to global order"), &dialog);
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addWidget(reset);
    layout->addLayout(buttons);
    QLabel *status = new QLabel(&dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    QDialogButtonBox *close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(close);

    const std::function<void()> submit = [this, id, list, status, up, down, reset]()
    {
        if (this->inherited_provider_client->updating()) return;
        QStringList selected;
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->checkState() == Qt::Checked)
                selected.append(list->item(row)->text());
        if (selected.isEmpty())
        {
            status->setText(tr("Select at least one upstream, or use Reset."));
            return;
        }
        list->setEnabled(false);
        up->setEnabled(false);
        down->setEnabled(false);
        reset->setEnabled(false);
        status->setText(tr("Updating MAP routing…"));
        this->inherited_provider_client->setUpstreamRoute(id, selected);
    };
    connect(list, &QListWidget::itemChanged, &dialog,
            [list, submit, status](QListWidgetItem *changed)
    {
        bool any_checked = false;
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->checkState() == Qt::Checked) any_checked = true;
        if (!any_checked)
        {
            const QSignalBlocker blocker(list);
            changed->setCheckState(Qt::Checked);
            status->setText(QObject::tr("At least one upstream must remain selected."));
            return;
        }
        submit();
    });
    const std::function<void(int)> move = [list, submit](int difference)
    {
        const int row = list->currentRow();
        const int target = row + difference;
        if (row < 0 || target < 0 || target >= list->count()) return;
        {
            const QSignalBlocker blocker(list);
            QListWidgetItem *item = list->takeItem(row);
            list->insertItem(target, item);
            list->setCurrentRow(target);
        }
        submit();
    };
    connect(up, &QPushButton::clicked, &dialog, [move]() { move(-1); });
    connect(down, &QPushButton::clicked, &dialog, [move]() { move(1); });
    connect(reset, &QPushButton::clicked, &dialog,
        [this, id, list, status, up, down, reset, chosen]()
    {
        if (this->inherited_provider_client->updating()) return;
        {
            const QSignalBlocker blocker(list);
            list->clear();
            for (const QString &name : chosen.upstreams)
            {
                QListWidgetItem *item = new QListWidgetItem(name, list);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Checked);
            }
            list->setCurrentRow(0);
        }
        list->setEnabled(false);
        up->setEnabled(false);
        down->setEnabled(false);
        reset->setEnabled(false);
        status->setText(tr("Resetting MAP routing…"));
        this->inherited_provider_client->setUpstreamRoute(id, {});
    });
    connect(this->inherited_provider_client, &MapInheritedProviderClient::updateFinished,
            &dialog, [list, status, up, down, reset](bool success, const QString &error)
    {
        list->setEnabled(true);
        up->setEnabled(true);
        down->setEnabled(true);
        reset->setEnabled(true);
        status->setText(success ? QObject::tr("Routing saved on this MAP server.") : error);
    });
    dialog.exec();
}

void MapSettingsWidget::refreshProviderTable()
{
    this->loading_provider_table = true;
    this->provider_table->setRowCount(0);
    for (const MapProviderEntry &entry : mapProviderCatalog().providers())
        addProviderRow(entry.id, entry.name, entry.url,
                       entry.minimum_zoom, entry.maximum_zoom,
                       entry.enabled, mapProviderCatalog().fallback(entry.id));
    refreshFallbackChoices();
    this->loading_provider_table = false;
}

void MapSettingsWidget::refreshFallbackChoices()
{
    struct AvailableProvider
    {
        QString id;
        QString name;
        int minimum_zoom = 0;
        int maximum_zoom = 0;
        bool enabled = false;
    };
    QVector<AvailableProvider> choices;
    for (int row = 0; row < this->provider_table->rowCount(); ++row)
    {
        QTableWidgetItem *name = this->provider_table->item(row, 1);
        QTableWidgetItem *enabled = this->provider_table->item(row, 0);
        QSpinBox *min = static_cast<QSpinBox *>(this->provider_table->cellWidget(row, 3));
        QSpinBox *max = static_cast<QSpinBox *>(this->provider_table->cellWidget(row, 4));
        if (name == nullptr || enabled == nullptr || min == nullptr || max == nullptr)
            continue;
        choices.append({name->data(Qt::UserRole).toString(), name->text(),
                        min->value(), max->value(), enabled->checkState() == Qt::Checked});
    }
    for (int row = 0; row < this->provider_table->rowCount(); ++row)
    {
        QComboBox *fallback = static_cast<QComboBox *>(this->provider_table->cellWidget(row, 5));
        if (fallback == nullptr || row >= choices.size()) continue;
        const AvailableProvider &selected = choices.at(row);
        const QString wanted = fallback->currentData().toString();
        const QSignalBlocker blocker(fallback);
        fallback->clear();
        fallback->addItem(tr("None (enlarge available tiles)"), QString());
        for (const AvailableProvider &alternative : choices)
        {
            if (alternative.enabled && alternative.id != selected.id &&
                alternative.maximum_zoom > selected.maximum_zoom &&
                alternative.minimum_zoom <= selected.maximum_zoom + 1)
                fallback->addItem(alternative.name, alternative.id);
        }
        const int index = fallback->findData(wanted);
        fallback->setCurrentIndex(index < 0 ? 0 : index);
        if (index < 0 && !wanted.isEmpty() && !this->loading_provider_table)
            rememberFallbackChange(row);
    }
}

void MapSettingsWidget::rememberFallbackChange(int row)
{
    const QString uuid = this->provider_table->item(row, 1)->data(Qt::UserRole).toString();
    const QString chosen = static_cast<QComboBox *>(
        this->provider_table->cellWidget(row, 5))->currentData().toString();
    // Personal preferences are immediate when the provider exists. If a row
    // was just added or its zoom limits are still awaiting confirmation, apply
    // the preference as soon as the updated catalog arrives.
    if (mapProviderCatalog().setFallback(uuid, chosen))
        this->pending_fallback_choices.remove(uuid);
    else
        this->pending_fallback_choices.insert(uuid, chosen);
}

void MapSettingsWidget::scheduleProviderSave()
{
    if (this->loading_provider_table) return;
    this->provider_edits_pending = true;
    this->provider_save_debounce->start();
}

void MapSettingsWidget::saveProviderEdits()
{
    if (!this->provider_edits_pending || this->provider_save_in_flight) return;
    if (!mapProviderCatalog().serverLoaded())
    {
        this->setProviderStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", 
            "Cannot edit providers until the server catalog has loaded. Check the map server connection and click Reload."));
        return;
    }
    QVector<MapProviderEntry> proposed;
    for (int row = 0; row < this->provider_table->rowCount(); ++row)
    {
        MapProviderEntry entry;
        entry.id = this->provider_table->item(row, 1)->data(Qt::UserRole).toString();
        entry.name = this->provider_table->item(row, 1)->text().trimmed();
        entry.url = this->provider_table->item(row, 2)->text().trimmed();
        entry.enabled = this->provider_table->item(row, 0)->checkState() == Qt::Checked;
        entry.minimum_zoom = static_cast<QSpinBox *>(this->provider_table->cellWidget(row, 3))->value();
        entry.maximum_zoom = static_cast<QSpinBox *>(this->provider_table->cellWidget(row, 4))->value();
        const MapProviderEntry *previous = mapProviderCatalog().provider(entry.id);
        if (previous != nullptr)
        {
            entry.coverage = previous->coverage;
            entry.remote_fetch_enabled = previous->remote_fetch_enabled;
        }
        if (entry.name.isEmpty() || entry.url.isEmpty() ||
            entry.minimum_zoom > entry.maximum_zoom)
        {
            this->setProviderStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", 
                "Enter a provider name and tile URL, and check its zoom limits."));
            return;
        }
        proposed.append(entry);
    }
    this->provider_edits_pending = false;
    this->provider_save_in_flight = true;
    this->setProviderStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", "Updating map providers..."));
    mapProviderCatalog().saveProviders(proposed);
}

void MapSettingsWidget::scheduleNavigationSave()
{
    GuiMapNavigationConfiguration configuration;
    configuration.scroll_zoom_sensitivity = this->scroll_zoom_sensitivity_control->value();
    configuration.mouse_3d_pan_sensitivity = this->mouse_3d_pan_sensitivity_control->value();
    configuration.orbit_3d_sensitivity = this->orbit_3d_sensitivity_control->value();
    applyGuiMapNavigationConfiguration(configuration);
    this->navigation_save_debounce->start();
}

void MapSettingsWidget::saveNavigationSettingsNow()
{
    GuiMapNavigationConfiguration configuration;
    configuration.scroll_zoom_sensitivity = this->scroll_zoom_sensitivity_control->value();
    configuration.mouse_3d_pan_sensitivity = this->mouse_3d_pan_sensitivity_control->value();
    configuration.orbit_3d_sensitivity = this->orbit_3d_sensitivity_control->value();

    if (saveGuiMapNavigationConfiguration(configuration))
        showNavigationStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", "Saved."), false);
    else
        showNavigationStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", "Failed to save map control settings."), true);
}

void MapSettingsWidget::schedulePerformanceSave()
{
    this->performance_save_debounce->start();
}

void MapSettingsWidget::savePerformanceSettingsNow()
{
    GuiMapPerformanceConfiguration configuration;
    configuration.terrain_detail_reduction =
        qRound(this->terrain_detail_reduction_control->value());

    if (saveGuiMapPerformanceConfiguration(configuration))
        showPerformanceStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", "Saved."), false);
    else
        showPerformanceStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", "Failed to save map performance settings."), true);
}

void MapSettingsWidget::saveServerSettings()
{
    MapServerClientConfiguration configuration;
    configuration.base_url = this->server_base_url_edit->text();
    configuration.api_key = this->server_api_key_edit->text().toUtf8();
    configuration.delete_api_key = this->server_delete_api_key_edit->text().toUtf8();
    configuration.wireguard_profile_id = this->server_wireguard_combo->currentData().toString();

    if (saveMapServerClientConfiguration(configuration))
    {
        showServerStatus(
            QT_TRANSLATE_NOOP("MapSettingsWidget", "Connection preferences saved and applied."),
            false);
    }
    else
    {
        showServerStatus(QT_TRANSLATE_NOOP("MapSettingsWidget", "Failed to save map server settings."), true);
    }
}

void MapSettingsWidget::reloadServerWireGuardProfiles(const QString &profile_id)
{
    const QSignalBlocker blocker(this->server_wireguard_combo);
    fillWireGuardConnectionCombo(this->server_wireguard_combo, profile_id);
}

void MapSettingsWidget::fillWireGuardConnectionCombo(QComboBox *combo, const QString &profile_id)
{
    combo->clear();
    combo->setToolTip(tr("WireGuard profiles are managed in Settings → WireGuard VPN"));
    combo->addItem(tr("Direct connection"), QString());
    int selected = profile_id.isEmpty() ? 0 : -1;
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    // Only builds with the embedded engine can use a profile; elsewhere a
    // stored selection stays visible as not usable (never direct).
    for (const WireGuardProfileEntry &entry : listWireGuardProfiles())
    {
        combo->addItem(tr("WireGuard: %1").arg(entry.name), entry.id);
        if (entry.id == profile_id) selected = combo->count() - 1;
    }
#endif
    if (selected < 0)
    {
        combo->addItem(tr("Missing profile (not usable)"), profile_id);
        selected = combo->count() - 1;
    }
    combo->setCurrentIndex(selected);
}

QHBoxLayout *MapSettingsWidget::wireGuardProfileButtons(
    QWidget *parent_widget, std::function<void(const QString &)> imported)
{
    QHBoxLayout *row = new QHBoxLayout();
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    QPushButton *import = LiveUiText::caption<QPushButton>(
        QT_TRANSLATE_NOOP("LiveUiText", "Import WireGuard profile…"), parent_widget);
    connect(import, &QPushButton::clicked, this, [this, imported]()
    {
        importWireGuardProfileFromFile(this, imported);
    });
    row->addWidget(import);
#else
    Q_UNUSED(imported)
#endif
    QPushButton *manage = LiveUiText::caption<QPushButton>(
        QT_TRANSLATE_NOOP("LiveUiText", "Manage WireGuard profiles…"), parent_widget);
    connect(manage, &QPushButton::clicked, this, &MapSettingsWidget::manageWireGuardProfilesRequested);
    row->addWidget(manage);
    row->addStretch();
    return row;
}

void MapSettingsWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Profiles are created and deleted in Settings -> WireGuard VPN.
    if (!event->spontaneous() && this->server_wireguard_combo != nullptr)
        this->reloadServerWireGuardProfiles(this->server_wireguard_combo->currentData().toString());
#ifdef AOWIS_STANDALONE
    if (!event->spontaneous())
        this->reloadCascadeUpstreamProfiles();
#endif
}

void MapSettingsWidget::hideEvent(QHideEvent *event)
{
#ifdef AOWIS_STANDALONE
    // Focus loss commits a currently edited table cell before the final save.
    QWidget *focused_widget = QApplication::focusWidget();
    if (this->cascade_upstreams_table != nullptr && focused_widget != nullptr &&
        this->cascade_upstreams_table->isAncestorOf(focused_widget))
        focused_widget->clearFocus();
    if (this->cascade_save_debounce != nullptr && this->cascade_save_debounce->isActive())
    {
        this->cascade_save_debounce->stop();
        saveEmbeddedCascadeSettings();
    }
#endif
    QWidget::hideEvent(event);
}

void MapSettingsWidget::setProviderStatus(const char *source)
{
    this->provider_status->setProperty("aowisLocalizedSource", QByteArray(source));
    this->provider_status->setText(tr(source));
}

void MapSettingsWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::LanguageChange || this->provider_table == nullptr)
        return;
    if (this->server_wireguard_combo != nullptr)
        this->reloadServerWireGuardProfiles(this->server_wireguard_combo->currentData().toString());
#ifdef AOWIS_STANDALONE
    this->reloadCascadeUpstreamProfiles();
#endif

    // Do not rebuild the table, change its model values, commit cell editors,
    // or restart a pending provider save. Only refresh presentation fields.
    const QSignalBlocker table_blocker(this->provider_table);
    for (int row = 0; row < this->provider_table->rowCount(); ++row)
    {
        QTableWidgetItem *name = this->provider_table->item(row, 1);
        if (name != nullptr)
            name->setToolTip(tr("Provider UUID: %1").arg(name->data(Qt::UserRole).toString()));
        QTableWidgetItem *url = this->provider_table->item(row, 2);
        if (url != nullptr && url->data(Qt::UserRole + 1).toBool())
            url->setToolTip(tr("Enter an XYZ tile URL template such as "
                "https://tiles.example.org/{z}/{x}/{y}.png"));
        QComboBox *fallback = static_cast<QComboBox *>(this->provider_table->cellWidget(row, 5));
        if (fallback != nullptr && fallback->count() > 0)
        {
            const QSignalBlocker blocker(fallback);
            fallback->setItemText(0, tr("None (enlarge available tiles)"));
        }
    }
#ifdef AOWIS_STANDALONE
    if (this->cascade_status != nullptr)
    {
        const QByteArray source = this->cascade_status->property("aowisLocalizedSource").toByteArray();
        if (!source.isEmpty())
            this->cascade_status->setText(tr(source.constData()));
    }
#endif
    for (QLabel *status : {this->navigation_status, this->performance_status,
                           this->server_status, this->provider_status})
    {
        if (status == nullptr)
            continue;
        const QByteArray source = status->property("aowisLocalizedSource").toByteArray();
        if (!source.isEmpty())
            status->setText(tr(source.constData()));
    }
}

void MapSettingsWidget::showNavigationStatus(const char *message, bool error)
{
    this->navigation_status->setProperty("aowisLocalizedSource", QByteArray(message));
    this->navigation_status->setText(tr(message));
    QPalette palette = this->navigation_status->palette();
    if (error)
    {
        QColor color = this->palette().color(QPalette::Text);
        color = color.lightness() > 128 ? QColor(255, 120, 120) : QColor(180, 20, 20);
        palette.setColor(QPalette::WindowText, color);
    }
    else
    {
        palette.setColor(QPalette::WindowText, this->palette().color(QPalette::Text));
    }
    this->navigation_status->setPalette(palette);
}

void MapSettingsWidget::showPerformanceStatus(const char *message, bool error)
{
    this->performance_status->setProperty("aowisLocalizedSource", QByteArray(message));
    this->performance_status->setText(tr(message));
    QPalette palette = this->performance_status->palette();
    if (error)
    {
        QColor color = this->palette().color(QPalette::Text);
        color = color.lightness() > 128 ? QColor(255, 120, 120) : QColor(180, 20, 20);
        palette.setColor(QPalette::WindowText, color);
    }
    else
    {
        palette.setColor(QPalette::WindowText, this->palette().color(QPalette::Text));
    }
    this->performance_status->setPalette(palette);
}

void MapSettingsWidget::showServerStatus(const char *message, bool error)
{
    this->server_status->setProperty("aowisLocalizedSource", QByteArray(message));
    this->server_status->setText(tr(message));
    QPalette palette = this->server_status->palette();
    if (error)
    {
        QColor color = this->palette().color(QPalette::Text);
        color = color.lightness() > 128 ? QColor(255, 120, 120) : QColor(180, 20, 20);
        palette.setColor(QPalette::WindowText, color);
    }
    else
    {
        palette.setColor(QPalette::WindowText, this->palette().color(QPalette::Text));
    }
    this->server_status->setPalette(palette);
}
