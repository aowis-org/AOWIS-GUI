#pragma once

#include <QHash>
#include <QList>
#include <QString>
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
class QShowEvent;

// Settings → WireGuard VPN. Profiles are the GUI's AOWIS-CONFIG WireGuard
// records (config/wireguard_profile_storage.h): New and Import create
// unsaved drafts; Save stores them, Delete removes them. "Used by" lists the
// configured connections whose transport references a profile; deletion is
// refused while any does, checked inside the same owner session as the
// delete. With the embedded engine (AOWIS_GUI_WIREGUARD_PROBE) the text is
// validated by AOWIS-NETWORK's parseWireGuardConf(), the parser tunnels use.
// Never log profile text or keys.
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

    struct PeerStatus
    {
        QString endpoint;
        QString last_handshake; // Already formatted by the runtime adapter
        quint64 received_bytes = 0;
        quint64 sent_bytes = 0;
    };

    struct TunnelStatus
    {
        enum class State { Unknown, Stopped, Starting, Running, Error };
        State state = State::Unknown;
        int active_consumers = -1; // -1: not reported, not zero
        quint64 received_bytes = 0;
        quint64 sent_bytes = 0;
        QString error;
        QList<PeerStatus> peers;
    };

    explicit WireGuardVpnSettingsWidget(QWidget *parent = nullptr);

    // Reloads profiles and their connection references from storage; also
    // done whenever the page is shown. Unsaved drafts and edits are kept.
    void refreshFromStorage();

    // A future tunnel runtime adapter supplies status keyed by profile ID.
    // No connection state is fabricated by this settings page.
    void setTunnelStatus(const QString &profile_id, const TunnelStatus &status);

    QStringList profileIds() const;
    QString profileIdForName(const QString &name) const;
    QString profileConfig(const QString &profile_id) const;

signals:
    void serviceActivated(const QString &service_id);

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
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
    QHash<QString, TunnelStatus> runtime;
    QString selected_id;
    int unreadable_profiles = 0;
    bool selecting = false;
    bool loading_editor = false;

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
    QLabel *validation_label = nullptr;
    QLabel *usage_info = nullptr;
    QTreeWidget *usage_table = nullptr;
    QLabel *status_info = nullptr;
    QLabel *tunnel_state = nullptr;
    QLabel *active_consumers = nullptr;
    QLabel *traffic = nullptr;
    QLabel *tunnel_error = nullptr;
    QTreeWidget *peers_table = nullptr;

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
    void refreshControls();
    void retranslate();
    bool hasChanges() const;
    // Blocking problem, or empty; warnings (ignored directives) go to *notes.
    QString configurationValidation(const QString &configuration, QString *notes = nullptr) const;
};
