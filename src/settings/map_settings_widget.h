#ifndef MAP_SETTINGS_WIDGET_H
#define MAP_SETTINGS_WIDGET_H

#include <QWidget>
#include <QEvent>
#include <QHash>
#include <QString>

#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTableWidget;
class QComboBox;
class QHBoxLayout;
class QShowEvent;
class QHideEvent;
class QCheckBox;
class QSpinBox;
class QNetworkAccessManager;
class SliderNumberControl;
class MapInheritedProviderClient;

// Settings > Map Settings: navigation sensitivity, terrain performance knobs
// for the globe, and map server connection details.
class MapSettingsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MapSettingsWidget(QWidget *parent = nullptr);
    ~MapSettingsWidget() override;

signals:
    // "Manage WireGuard profiles…": show Settings -> WireGuard VPN.
    void manageWireGuardProfilesRequested();

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void setProviderStatus(const char *source);
    // Navigation section.
    SliderNumberControl *scroll_zoom_sensitivity_control = nullptr;
    SliderNumberControl *mouse_3d_pan_sensitivity_control = nullptr;
    SliderNumberControl *orbit_3d_sensitivity_control = nullptr;
    QLabel *navigation_status = nullptr;
    QTimer *navigation_save_debounce = nullptr;

    // Performance section.
    SliderNumberControl *terrain_detail_reduction_control = nullptr;
    QLabel *performance_status = nullptr;
    QTimer *performance_save_debounce = nullptr;

    // Server section.
    QLineEdit *server_base_url_edit = nullptr;
    QLineEdit *server_api_key_edit = nullptr;
    QLineEdit *server_delete_api_key_edit = nullptr;
    // Direct connection or a stored WireGuard profile (by id).
    QComboBox *server_wireguard_combo = nullptr;
    QLabel *server_status = nullptr;
    QTableWidget *provider_table = nullptr;
    QLabel *provider_status = nullptr;
    QHash<QString, QString> pending_fallback_choices;
    QTimer *provider_save_debounce = nullptr;
    bool loading_provider_table = false;
    bool provider_edits_pending = false;
    bool provider_save_in_flight = false;
    void refreshProviderTable();
    void refreshFallbackChoices();
    void rememberFallbackChange(int row);
    void scheduleProviderSave();
    void addProviderRow(const QString &uuid, const QString &name, const QString &url,
                        int minimum_zoom, int maximum_zoom, bool enabled,
                        const QString &fallback_id = QString());
    void saveProviderEdits();
    void refreshInheritedProviderTable();
    void editProviderUpstreamRoute(const QString &id);
    QTableWidget *inherited_provider_table = nullptr;
    QTableWidget *upstream_routing_table = nullptr;
    QLabel *inherited_provider_status = nullptr;
    MapInheritedProviderClient *inherited_provider_client = nullptr;

    void buildNavigationSection(QWidget *parent_widget);
    void buildPerformanceSection(QWidget *parent_widget);
    void buildServerSection(QWidget *parent_widget);
#ifdef AOWIS_STANDALONE
    void buildEmbeddedCascadeSection(QWidget *parent_widget);
    void appendCascadeUpstream(const QString &name, const QString &url, const QString &api_key,
                               const QString &wireguard_profile_id);
    // The row's connection: empty for direct, else the WireGuard profile ID.
    QString cascadeUpstreamProfile(int row) const;
    // Refills every row's connection selector, keeping its selection.
    void reloadCascadeUpstreamProfiles();
    void moveCascadeUpstream(int direction);
    void scheduleEmbeddedCascadeSave();
    void saveEmbeddedCascadeSettings();
    QTimer *cascade_save_debounce = nullptr;
    QTableWidget *cascade_upstreams_table = nullptr;
    QCheckBox *cascade_allow_direct_fetch = nullptr;
    QCheckBox *cascade_poor_internet = nullptr;
    QSpinBox *cascade_jpeg_quality = nullptr;
    QLabel *cascade_status = nullptr;
    QString cascade_revision;
#endif
    void scheduleNavigationSave();
    void saveNavigationSettingsNow();
    void schedulePerformanceSave();
    void savePerformanceSettingsNow();
    void saveServerSettings();
    // Refills the WireGuard selector and selects profile_id. A selected id
    // that names no stored profile stays selected as "missing", so the map
    // server keeps failing closed instead of silently becoming direct.
    void reloadServerWireGuardProfiles(const QString &profile_id);
    // The shared connection selector of MAP connections (as DVMDB
    // connections): "Direct connection", "WireGuard: <profile>" per central
    // profile, and a missing selected profile as "not usable" (fails closed).
    void fillWireGuardConnectionCombo(QComboBox *combo, const QString &profile_id);
    // "Import WireGuard profile…" (builds with the embedded engine; calls
    // imported(profile_id)) and "Manage WireGuard profiles…".
    QHBoxLayout *wireGuardProfileButtons(QWidget *parent_widget,
                                         std::function<void(const QString &)> imported);
    void showNavigationStatus(const char *message, bool error);
    void showPerformanceStatus(const char *message, bool error);
    void showServerStatus(const char *message, bool error);
};

#endif // MAP_SETTINGS_WIDGET_H
