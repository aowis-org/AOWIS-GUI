#include "config/live_ui_text.h"
#include "settings/map_settings_widget.h"

#include "config/gui_configuration.h"
#include "map/core/gui_map_provider_catalog.h"
#include "config/map_server_client_configuration.h"
#include "map/render/map_globe_terrain_levels.h"
#include "widgets/slider_number_control.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHash>
#include <QSpinBox>
#include <QTableWidget>
#include <QUuid>
#include <QSignalBlocker>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

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
    content_layout->addStretch(1);

    scroll_area->setWidget(content);

    QVBoxLayout *outer_layout = new QVBoxLayout(this);
    outer_layout->setContentsMargins(0, 0, 0, 0);
    outer_layout->addWidget(scroll_area);
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
    QGroupBox *group = new QGroupBox(parent_widget);
    LiveUiText::bind(group, [group] { group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Map Server Settings"))); });
    QVBoxLayout *group_layout = new QVBoxLayout(group);

#ifdef __EMSCRIPTEN__
    group_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText", "In this build, the map server connection is set by the hosting page and "
                       "cannot be changed here."),
        group));
#else
    group_layout->addWidget(helpLabel(QT_TRANSLATE_NOOP("LiveUiText", "Connection changes are saved when you finish editing a field. "
                       "Restart AOWIS for them to take effect."),
        group));
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

    group_layout->addLayout(form);

    this->server_status = new QLabel(group);
    this->server_status->setWordWrap(true);
    group_layout->addWidget(this->server_status);

    for (QLineEdit *field : {this->server_base_url_edit, this->server_api_key_edit,
                             this->server_delete_api_key_edit})
        connect(field, &QLineEdit::editingFinished,
                this, &MapSettingsWidget::saveServerSettings);

#ifdef __EMSCRIPTEN__
    this->server_base_url_edit->setEnabled(false);
    this->server_api_key_edit->setEnabled(false);
    this->server_delete_api_key_edit->setEnabled(false);
#endif

    static_cast<QVBoxLayout *>(parent_widget->layout())->addWidget(group);

    QGroupBox *providers_group = new QGroupBox(parent_widget);
    LiveUiText::bind(providers_group, [providers_group] { providers_group->setTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "2D Map Providers"))); });
    QVBoxLayout *providers_layout = new QVBoxLayout(providers_group);
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
                       QStringLiteral("New map provider"),
                       QString(), 0, 19, true);
        const int added_row = this->provider_table->rowCount() - 1;
        this->provider_table->item(added_row, 2)->setToolTip(
            QStringLiteral("Enter an XYZ tile URL template such as "
                           "https://tiles.example.org/{z}/{x}/{y}.png"));
        this->loading_provider_table = false;
        refreshFallbackChoices();
        this->provider_table->setCurrentCell(added_row, 2);
        this->provider_table->editItem(this->provider_table->item(added_row, 2));
        this->provider_status->setText(QStringLiteral(
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
        this->provider_status->setText(message);
    });
    connect(&mapProviderCatalog(), &MapProviderCatalog::providersSaved, this, [this]
    {
        this->provider_status->setText(QStringLiteral("Provider changes saved."));
    });
    refreshProviderTable();
    this->provider_table->setEnabled(mapProviderCatalog().serverLoaded());
    add->setEnabled(mapProviderCatalog().serverLoaded());
    remove->setEnabled(mapProviderCatalog().serverLoaded());
#ifdef AOWIS_STANDALONE
    providers_group->setEnabled(false);
    this->provider_status->setText(QStringLiteral("Server provider editing requires a REST map server."));
#endif
#ifdef __EMSCRIPTEN__
    this->provider_status->setText(QStringLiteral(
        "Provider edits affect the server; per-user fallback persistence is supported in native GUI builds."));
#endif
    static_cast<QVBoxLayout *>(parent_widget->layout())->addWidget(providers_group);
}

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
    name_item->setToolTip(QStringLiteral("Provider UUID: %1").arg(uuid));
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
    fallback->addItem(QStringLiteral("None (enlarge available tiles)"), QString());
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
        fallback->addItem(QStringLiteral("None (enlarge available tiles)"), QString());
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
        this->provider_status->setText(QStringLiteral(
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
            this->provider_status->setText(QStringLiteral(
                "Enter a provider name and tile URL, and check its zoom limits."));
            return;
        }
        proposed.append(entry);
    }
    this->provider_edits_pending = false;
    this->provider_save_in_flight = true;
    this->provider_status->setText(QStringLiteral("Updating map providers..."));
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
        showNavigationStatus(QStringLiteral("Saved."), false);
    else
        showNavigationStatus(QStringLiteral("Failed to save map control settings."), true);
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
        showPerformanceStatus(QStringLiteral("Saved."), false);
    else
        showPerformanceStatus(QStringLiteral("Failed to save map performance settings."), true);
}

void MapSettingsWidget::saveServerSettings()
{
    MapServerClientConfiguration configuration;
    configuration.base_url = this->server_base_url_edit->text();
    configuration.api_key = this->server_api_key_edit->text().toUtf8();
    configuration.delete_api_key = this->server_delete_api_key_edit->text().toUtf8();

    if (saveMapServerClientConfiguration(configuration))
    {
        showServerStatus(
            QStringLiteral("Connection preferences saved. Restart AOWIS to use them."),
            false);
    }
    else
    {
        showServerStatus(QStringLiteral("Failed to save map server settings."), true);
    }
}

void MapSettingsWidget::showNavigationStatus(const QString &message, bool error)
{
    this->navigation_status->setText(message);
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

void MapSettingsWidget::showPerformanceStatus(const QString &message, bool error)
{
    this->performance_status->setText(message);
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

void MapSettingsWidget::showServerStatus(const QString &message, bool error)
{
    this->server_status->setText(message);
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
