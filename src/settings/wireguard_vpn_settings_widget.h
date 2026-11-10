#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QEvent;

// Settings GUI only. Profiles are session-local drafts until the central AOWIS
// configuration store is designed. Never serialize WireGuard private keys to
// QSettings or log them. The runtime and service registries can later publish
// their real snapshots through the two explicit update entry points below.
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

    // Future service/tunnel adapters supply data keyed by stable profile ID.
    // No counts or connection states are fabricated by this settings page.
    void setServiceReferences(const QString &profile_id, const QList<ServiceReference> &services);
    void setTunnelStatus(const QString &profile_id, const TunnelStatus &status);

    QStringList profileIds() const;
    QString profileIdForName(const QString &name) const;
    QString profileConfig(const QString &profile_id) const;

signals:
    void serviceActivated(const QString &service_id);

protected:
    void changeEvent(QEvent *event) override;

private:
    struct Profile
    {
        QString id;
        QString name;
        QString description;
        QString configuration;
        bool draft = false;
    };

    QList<Profile> profiles;
    QHash<QString, QList<ServiceReference>> references;
    QHash<QString, TunnelStatus> runtime;
    QString selected_id;
    bool selecting = false;
    bool loading_editor = false;

    QLabel *title = nullptr;
    QLabel *introduction = nullptr;
    QLabel *session_notice = nullptr;
    QLabel *empty_notice = nullptr;
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
    QString configurationValidation(const QString &configuration) const;
};
