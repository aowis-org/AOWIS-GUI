#include "settings/wireguard_vpn_settings_widget.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>

namespace
{
bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

void answerNextMessage(QMessageBox::StandardButton response)
{
    QTimer::singleShot(0, [response] {
        if (QMessageBox *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            message->done(response);
    });
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    WireGuardVpnSettingsWidget page;
    QListWidget *list = page.findChild<QListWidget *>(QStringLiteral("wireguardVpnProfileList"));
    QLineEdit *name = page.findChild<QLineEdit *>(QStringLiteral("wireguardVpnName"));
    QPlainTextEdit *editor = page.findChild<QPlainTextEdit *>(QStringLiteral("wireguardVpnEditor"));
    QPlainTextEdit *description = page.findChild<QPlainTextEdit *>(QStringLiteral("wireguardVpnDescription"));
    QPushButton *save = page.findChild<QPushButton *>(QStringLiteral("wireguardVpnSave"));
    QPushButton *create = page.findChild<QPushButton *>(QStringLiteral("wireguardVpnNew"));
    QPushButton *remove = page.findChild<QPushButton *>(QStringLiteral("wireguardVpnDelete"));
    QTabWidget *tabs = page.findChild<QTabWidget *>(QStringLiteral("wireguardVpnDetails"));
    QTreeWidget *used_by = page.findChild<QTreeWidget *>(QStringLiteral("wireguardVpnUsedBy"));
    QLabel *state = page.findChild<QLabel *>(QStringLiteral("wireguardVpnTunnelState"));
    QLabel *active = page.findChild<QLabel *>(QStringLiteral("wireguardVpnActiveConsumers"));
    if (!check(list && name && editor && description && save && create && remove && tabs && used_by && state && active,
               "required controls are missing")) return 1;
    if (!check(list->count() == 0 && tabs->isHidden() && page.profileIds().isEmpty(),
               "new manager must not fabricate saved profiles")) return 1;

    create->click();
    if (!check(list->count() == 1 && page.profileIds().size() == 1 && !save->isEnabled(),
               "blank profile should be editable and validated")) return 1;
    name->setText(QStringLiteral("Office VPN"));
    description->setPlainText(QStringLiteral("Office infrastructure"));
    editor->setPlainText(QStringLiteral("[Interface]\nPrivateKey = local-test-key\nAddress = 10.8.0.2/32\n"
                                         "[Peer]\nPublicKey = test-peer-key\nAllowedIPs = 10.8.0.0/24\n"));
    if (!check(save->isEnabled(), "Save should activate on valid user edits")) return 1;
    save->click();
    const QString id = page.profileIdForName(QStringLiteral("Office VPN"));
    if (!check(!id.isEmpty() && page.profileConfig(id).contains(QStringLiteral("PrivateKey"))
               && !save->isEnabled(), "saved draft was not preserved in session model")) return 1;
    if (!check(list->item(0)->text().contains(QStringLiteral("unavailable"), Qt::CaseInsensitive),
               "GUI must not fabricate runtime status or service counts")) return 1;

    name->setText(QStringLiteral("Renamed office VPN"));
    save->click();
    if (!check(page.profileIdForName(QStringLiteral("Renamed office VPN")) == id
               && page.profileIdForName(QStringLiteral("Office VPN")).isEmpty(),
               "renaming must retain a stable profile identity")) return 1;
    QEvent language_change(QEvent::LanguageChange);
    QApplication::sendEvent(&page, &language_change);
    if (!check(page.profileIdForName(QStringLiteral("Renamed office VPN")) == id,
               "language change modified profile identity")) return 1;

    // Reverting changes must not overwrite the committed in-session configuration.
    editor->setPlainText(QStringLiteral("invalid text"));
    page.findChild<QPushButton *>(QStringLiteral("wireguardVpnRevert"))->click();
    if (!check(editor->toPlainText() == page.profileConfig(id), "revert lost committed contents")) return 1;

    WireGuardVpnSettingsWidget::ServiceReference service;
    service.id = QStringLiteral("db-service-id");
    service.name = QStringLiteral("Personal DVMDB");
    service.kind = QStringLiteral("DVMDB");
    service.endpoint = QStringLiteral("http://10.8.0.10:8080");
    page.setServiceReferences(id, {service});
    if (!check(list->item(0)->text().contains(QStringLiteral("1")) && used_by->topLevelItemCount() == 1,
               "configured service usage did not update")) return 1;

    WireGuardVpnSettingsWidget::TunnelStatus tunnel;
    tunnel.state = WireGuardVpnSettingsWidget::TunnelStatus::State::Running;
    tunnel.active_consumers = 1;
    tunnel.received_bytes = 4096;
    tunnel.sent_bytes = 2048;
    page.setTunnelStatus(id, tunnel);
    if (!check(state->text() == QStringLiteral("Running") && active->text() == QStringLiteral("1"),
               "runtime snapshot did not update status")) return 1;

    // The GUI refuses to delete a referenced profile even without a backend.
    answerNextMessage(QMessageBox::Ok);
    remove->click();
    if (!check(page.profileIds().size() == 1, "referenced profile was deleted")) return 1;
    page.setServiceReferences(id, {});
    answerNextMessage(QMessageBox::Yes);
    remove->click();
    if (!check(page.profileIds().isEmpty(), "unreferenced profile was not deleted")) return 1;

    create->click();
    if (!check(page.profileIds().size() == 1, "second draft was not created")) return 1;
    page.findChild<QPushButton *>(QStringLiteral("wireguardVpnRevert"))->click();
    if (!check(page.profileIds().isEmpty(), "reverting unsaved draft must remove it")) return 1;

    std::puts("WireGuard VPN settings GUI regression checks passed");
    return 0;
}
