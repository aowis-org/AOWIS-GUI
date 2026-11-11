#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSyntaxHighlighter;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QEvent;
class QHideEvent;
class QShowEvent;
class QTimer;

// Settings → WireGuard VPN. Profiles are the GUI's AOWIS-CONFIG WireGuard
// records (config/wireguard_profile_storage.h): New and Import create
// unsaved drafts; Save stores them, Delete removes them. "Used by" lists the
// configured connections whose transport references a profile; deletion is
// refused while any does, checked inside the same owner session as the
// delete. With the embedded engine (AOWIS_GUI_WIREGUARD_PROBE) the text is
// validated by AOWIS-NETWORK's parseWireGuardConf(), the parser tunnels use.
// Never log profile text or keys. The Status tab shows the profile's single
// shared tunnel: the connections using it now and, while visible and running,
// redacted engine telemetry polled once per second (handshake age, encrypted
// traffic and throughput, peer endpoint, keepalive, SOCKS5 relays) with
// plain-language health hints.
class WireGuardVpnSettingsWidget final : public QWidget
{
    Q_OBJECT

public:
    struct ServiceReference
    {
        QString id;       // Stable identity of the configured AOWIS service
        QString name;
        QString kind;     // e.g. DVMDB, Map server (presentation only)
        QString endpoint;
    };

    explicit WireGuardVpnSettingsWidget(QWidget *parent = nullptr);

    // Reloads profiles and their connection references from storage; also
    // done whenever the page is shown. Unsaved drafts and edits are kept.
    void refreshFromStorage();

    QStringList profileIds() const;
    QString profileIdForName(const QString &name) const;
    QString profileConfig(const QString &profile_id) const;

signals:
    void serviceActivated(const QString &service_id);

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Profile
    {
        QString id;       // Stored UUIDv7, or "draft:<uuid>" until first saved
        QString revision; // Stored revision; empty for drafts
        QString name;
        QString description;
        QString configuration;
        bool draft = false;
    };

    QList<Profile> profiles;
    QHash<QString, QList<ServiceReference>> references;
    QString selected_id;
    int unreadable_profiles = 0;
    bool selecting = false;
    bool loading_editor = false;
    // While hidden, the editor shows a mask instead of each PrivateKey value;
    // the real values (in line order) live here and are substituted back by
    // currentConfiguration(). Never shown, logged or copied to the clipboard.
    bool private_key_visible = false;
    QStringList hidden_private_keys;

    QLabel *title = nullptr;
    QLabel *introduction = nullptr;
    QLabel *session_notice = nullptr;
    QLabel *empty_notice = nullptr;
    QLabel *damaged_notice = nullptr;
    QPushButton *remove_damaged_button = nullptr;
    QListWidget *profile_list = nullptr;
    QPushButton *new_button = nullptr;
    QPushButton *import_button = nullptr;
    QPushButton *delete_button = nullptr;
    QPushButton *save_button = nullptr;
    QPushButton *revert_button = nullptr;
    QTabWidget *detail_tabs = nullptr;
    QLineEdit *name_edit = nullptr;
    QPlainTextEdit *description_edit = nullptr;
    QPlainTextEdit *configuration_edit = nullptr;
    QSyntaxHighlighter *configuration_highlighter = nullptr;
    QLabel *editor_caption = nullptr;
    QPushButton *private_key_button = nullptr; // Checkable "Show/edit private key"
    QLabel *validation_label = nullptr;
    QLabel *usage_info = nullptr;
    QTreeWidget *usage_table = nullptr;
    QLabel *status_info = nullptr;
    QLabel *tunnel_state = nullptr;
    QLabel *active_consumers = nullptr;
    QLabel *traffic = nullptr;
    QLabel *tunnel_error = nullptr;
    QLabel *peer_telemetry_notice = nullptr; // Health hints; hidden when there are none
    QLabel *last_handshake = nullptr;
    QLabel *throughput = nullptr;
    QLabel *peer_endpoint = nullptr;
    QLabel *keepalive = nullptr;
    QLabel *uptime = nullptr;
    QLabel *relays = nullptr;
    QTimer *telemetry_timer = nullptr;

    // Throughput is derived from consecutive counter samples of one tunnel
    // instance (profile + engine start time); anything else resets it.
    struct RateSample
    {
        QString profile_id;
        qint64 tunnel_started_ms = 0;
        qint64 sampled_ms = 0;  // telemetry_clock time of the counters below
        quint64 received = 0;
        quint64 sent = 0;
        double received_per_second = 0.0;
        double sent_per_second = 0.0;
        bool has_counters = false;
        bool has_rate = false;
    };
    RateSample rate_sample;
    QElapsedTimer telemetry_clock;

    Profile *findProfile(const QString &id);
    const Profile *findProfile(const QString &id) const;
    void createProfile(const QString &name, const QString &configuration);
    void importProfile();
    void deleteProfile();
    void removeDamagedProfiles();
    void reportStorageError(const QString &title, const QString &message);
    void saveProfile();
    void revertProfile();
    bool leaveCurrentProfile();
    void onSelectionChanged();
    void loadSelection();
    void refreshProfileList();
    void refreshUsage();
    void refreshStatus();
    void refreshTelemetry(bool running);
    QString durationText(qint64 seconds) const;
    QString bytesText(quint64 bytes) const;
    QString rateText(double bytes_per_second) const;
    void refreshControls();
    void retranslate();
    bool hasChanges() const;
    // The real configuration: editor text with hidden private keys restored.
    // Use this, never configuration_edit->toPlainText(), for any logic.
    QString currentConfiguration() const;
    void showConfiguration(const QString &configuration);
    void setPrivateKeyVisible(bool visible);
    void refreshPrivateKeyControls();
    // Blocking problem, or empty; warnings (ignored directives) go to *notes.
    QString configurationValidation(const QString &configuration, QString *notes = nullptr) const;
};
