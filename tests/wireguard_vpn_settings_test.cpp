#include "settings/wireguard_vpn_settings_widget.h"
#include "network/wireguard_runtime_status.h"

#include "config/project_server_profile_storage.h"
#include "config/wireguard_profile_storage.h"
#include "projects/dvmdb_identity.h"

#include <QApplication>
#include <QAbstractButton>
#include <QColor>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPalette>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
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

QTextCharFormat formatAt(QPlainTextEdit *editor, int line, int column)
{
    const QTextBlock block = editor->document()->findBlockByNumber(line);
    if (!block.isValid() || block.layout() == nullptr) return {};
    const QList<QTextLayout::FormatRange> ranges = block.layout()->formats();
    for (const QTextLayout::FormatRange &range : ranges)
        if (column >= range.start && column < range.start + range.length)
            return range.format;
    return {};
}

void answerNextMessage(QMessageBox::StandardButton response)
{
    QTimer::singleShot(0, [response] {
        if (QMessageBox *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        {
            // QMessageBox tracks the actual clicked button. Calling done()
            // directly bypasses that bookkeeping and can make question()
            // return NoButton instead of the requested Yes/Ok response.
            if (QAbstractButton *button = message->button(response)) button->click();
        }
    });
}

// A profile the embedded engine accepts (one interface address, one peer).
const QString valid_profile = QStringLiteral(
    "[Interface]\nPrivateKey = k5yQ1m2b3c4d5e6f7g8h9i0j1k2l3m4n5o6p7q8r9s0=\nAddress = 10.8.0.2/32\n"
    "[Peer]\nPublicKey = V0hERXJ4MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU=\n"
    "Endpoint = vpn.example.invalid:51820\nAllowedIPs = 10.8.0.0/24\n");
}

int main(int argc, char **argv)
{
    // Profiles and connections live in the GUI's AOWIS-CONFIG store; keep it
    // isolated from the developer's real configuration.
    QTemporaryDir configuration;
    if (!check(configuration.isValid(), "temporary configuration unavailable")) return 1;
    qputenv("XDG_CONFIG_HOME", configuration.path().toUtf8());
    qputenv("XDG_DATA_HOME", configuration.path().toUtf8());
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

    // Highlighting is presentation-only and must survive palette changes.
    const QString sample = QStringLiteral(
        "# title comment\n[Interface]\nPrivateKey = placeholder-secret\n"
        "Address = 10.8.0.2/32\n; alternate comment\n[Peer]\n"
        "PublicKey = placeholder-public\nAllowedIPs = 10.8.0.0/24 # route comment\n"
        "OtherKey = other-value\nbroken line\n"
        "Endpoint = vpn.example.org:51820;not-a-comment\n");
    const QPalette original_palette = editor->palette();
    QPalette light_palette = original_palette;
    light_palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
    light_palette.setColor(QPalette::Text, QColor(QStringLiteral("#111111")));
    editor->setPalette(light_palette);
    editor->setPlainText(sample);
    QApplication::processEvents();
    const QTextCharFormat section = formatAt(editor, 1, 2);
    const QTextCharFormat directive = formatAt(editor, 2, 2);
    const QTextCharFormat value = formatAt(editor, 2, 16);
    const QTextCharFormat separator = formatAt(editor, 2, 11);
    const QTextCharFormat comment = formatAt(editor, 0, 2);
    const QTextCharFormat inline_comment = formatAt(editor, 7, 32);
    const QTextCharFormat unknown = formatAt(editor, 8, 2);
    const QTextCharFormat invalid = formatAt(editor, 9, 2);
    const QTextCharFormat peer_section = formatAt(editor, 5, 2);
    const QTextCharFormat literal_semicolon = formatAt(editor, 10, 36);
    if (!check(section.fontWeight() == QFont::Bold
               && section.foreground().style() != Qt::NoBrush
               && peer_section.foreground().color() == section.foreground().color()
               && directive.fontWeight() == QFont::DemiBold
               && directive.foreground().color() != value.foreground().color()
               && separator.foreground().color() != directive.foreground().color()
               && comment.fontItalic() && inline_comment.fontItalic()
               && unknown.foreground().color() != directive.foreground().color()
               && invalid.underlineStyle() == QTextCharFormat::WaveUnderline
               && !literal_semicolon.fontItalic(),
               "WireGuard sections, keys, values, comments or invalid lines are not highlighted")) return 1;
    QPalette dark_palette = light_palette;
    dark_palette.setColor(QPalette::Base, QColor(QStringLiteral("#101826")));
    dark_palette.setColor(QPalette::Text, QColor(QStringLiteral("#e7ebf1")));
    editor->setPalette(dark_palette);
    QApplication::processEvents();
    if (!check(formatAt(editor, 1, 2).foreground().color() != section.foreground().color()
               && editor->toPlainText() == sample,
               "palette change must recolor syntax without modifying configuration")) return 1;
    editor->setPalette(original_palette);
    editor->clear();

    create->click();
    if (!check(list->count() == 1 && page.profileIds().size() == 1 && !save->isEnabled(),
               "blank profile should be editable and validated")) return 1;
    name->setText(QStringLiteral("Office VPN"));
    description->setPlainText(QStringLiteral("Office infrastructure"));
    editor->setPlainText(valid_profile);
    if (!check(save->isEnabled(), "Save should activate on valid user edits")) return 1;
    save->click();
    const QString id = page.profileIdForName(QStringLiteral("Office VPN"));
    if (!check(DvmdbIdentity::isUuidV7(id) && page.profileConfig(id) == valid_profile && !save->isEnabled(),
               "saved draft did not become a stored profile")) return 1;
    std::string stored_text;
    QString storage_error;
    if (!check(listWireGuardProfiles().size() == 1
                   && readWireGuardProfileText(id, stored_text, &storage_error)
                   && QString::fromStdString(stored_text) == valid_profile,
               "profile text was not stored verbatim in AOWIS-CONFIG")) return 1;
    {
        WireGuardVpnSettingsWidget reopened;
        if (!check(reopened.profileIdForName(QStringLiteral("Office VPN")) == id
                       && reopened.profileConfig(id) == valid_profile,
                   "stored profile not shown by a new settings page")) return 1;
    }
    if (!check(list->item(0)->text().contains(QStringLiteral("Not used"))
                   && list->item(0)->text().contains(QStringLiteral("Not started"))
                   && state->text() == QStringLiteral("Not started"),
               "Unstarted profile must not claim a running tunnel")) return 1;

    name->setText(QStringLiteral("Renamed office VPN"));
    save->click();
    if (!check(page.profileIdForName(QStringLiteral("Renamed office VPN")) == id
                   && page.profileIdForName(QStringLiteral("Office VPN")).isEmpty()
                   && listWireGuardProfiles().size() == 1
                   && listWireGuardProfiles().at(0).name == QStringLiteral("Renamed office VPN"),
               "renaming must retain the stored profile identity")) return 1;
    QEvent language_change(QEvent::LanguageChange);
    QApplication::sendEvent(&page, &language_change);
    if (!check(page.profileIdForName(QStringLiteral("Renamed office VPN")) == id,
               "language change modified profile identity")) return 1;

    // Reverting changes must not overwrite the stored configuration.
    editor->setPlainText(QStringLiteral("invalid text"));
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    // The editor uses AOWIS-NETWORK's parser: what the engine cannot run is
    // never saved, and a second peer is something it cannot run.
    if (!check(!save->isEnabled(), "unusable configuration could be saved")) return 1;
    editor->setPlainText(valid_profile + QStringLiteral("[Peer]\nPublicKey = V0hERXJ4MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU=\n"));
    QLabel *validation = page.findChild<QLabel *>(QStringLiteral("wireguardVpnValidation"));
    if (!check(!save->isEnabled() && validation != nullptr && validation->text().contains(QStringLiteral("Line")),
               "parser diagnostics are not shown with their line")) return 1;
    // Ignored wg-quick directives are warnings: saveable, but explained.
    QString with_hook = valid_profile;
    with_hook.replace(QStringLiteral("Address = 10.8.0.2/32\n"),
                      QStringLiteral("Address = 10.8.0.2/32\nPostUp = /bin/true\n"));
    editor->setPlainText(with_hook);
    if (!check(save->isEnabled() && validation->text().contains(QStringLiteral("ignored")),
               "ignored directive must be saveable with a warning")) return 1;
#endif
    page.findChild<QPushButton *>(QStringLiteral("wireguardVpnRevert"))->click();
    if (!check(editor->toPlainText() == page.profileConfig(id), "revert lost stored contents")) return 1;

    // "Used by" is computed from configured connections.
    ProjectServerProfile connection;
    connection.id = DvmdbIdentity::newUuidV7();
    connection.name = QStringLiteral("Personal DVMDB");
    connection.endpoint = QStringLiteral("http://10.8.0.10:8080");
    connection.auth_mode = QStringLiteral("None");
    connection.wireguard_profile_id = id;
    QList<ProjectServerProfile> connections{connection};
    if (!check(saveProjectServerProfiles(connections), "could not store a connection")) return 1;
    page.refreshFromStorage();
    if (!check(list->item(0)->text().contains(QStringLiteral("Used by 1 connection"))
                   && used_by->topLevelItemCount() == 1
                   && used_by->topLevelItem(0)->text(0) == QStringLiteral("Personal DVMDB"),
               "configured connection usage did not update")) return 1;

    // This is the same process-wide monitor used by DvmdbConnection.
    // The widget must update automatically, without an injected GUI snapshot.
    WireGuardRuntimeStatus &monitor = WireGuardRuntimeStatus::instance();
    QObject source;
    monitor.starting(id, &source);
    if (!check(state->text() == QStringLiteral("Starting") && active->text() == QStringLiteral("0"),
               "starting a real transport did not update the settings page")) return 1;
    monitor.running(&source);
    if (!check(state->text() == QStringLiteral("Running") && active->text() == QStringLiteral("1"),
               "running transport did not update status")) return 1;
    QLabel *info = page.findChild<QLabel *>(QStringLiteral("wireguardVpnStatusInfo"));
    if (!check(info != nullptr && info->text().contains(QStringLiteral("not yet confirmed")),
               "proxy startup incorrectly claimed peer reachability")) return 1;
    monitor.httpReachable(&source);
    if (!check(info->text().contains(QStringLiteral("An HTTP response was received")),
               "successful HTTP transport did not update reachability")) return 1;
    QObject second_source;
    monitor.starting(id, &second_source);
    monitor.running(&second_source);
    if (!check(active->text() == QStringLiteral("2"),
               "multiple active transports were not counted")) return 1;
    monitor.stopped(&second_source);
    if (!check(active->text() == QStringLiteral("1") && state->text() == QStringLiteral("Running"),
               "stopping one transport incorrectly stopped the whole profile")) return 1;
    monitor.stopped(&source);
    if (!check(state->text() == QStringLiteral("Stopped") && active->text() == QStringLiteral("0"),
               "last transport close did not stop the profile")) return 1;
    monitor.starting(id, &source);
    monitor.failed(&source, QStringLiteral("Simulated no-handshake error"));
    if (!check(state->text() == QStringLiteral("Error")
               && info->text().contains(QStringLiteral("failed")),
               "failed runtime attempt did not produce an error state")) return 1;
    monitor.starting(id, &source);
    monitor.running(&source);
    if (!check(state->text() == QStringLiteral("Running")
               && monitor.snapshot(id).last_error.isEmpty(),
               "successful retry retained a stale error")) return 1;
    monitor.stopped(&source);

    // A referenced profile is never deleted.
    answerNextMessage(QMessageBox::Ok);
    remove->click();
    if (!check(page.profileIds().size() == 1 && listWireGuardProfiles().size() == 1,
               "referenced profile was deleted")) return 1;
    connections.clear();
    if (!check(saveProjectServerProfiles(connections), "could not remove the connection")) return 1;
    answerNextMessage(QMessageBox::Yes);
    remove->click();
    if (!check(page.profileIds().isEmpty() && listWireGuardProfiles().isEmpty(),
               "unreferenced profile was not deleted from storage")) return 1;

    create->click();
    if (!check(page.profileIds().size() == 1, "second draft was not created")) return 1;
    page.findChild<QPushButton *>(QStringLiteral("wireguardVpnRevert"))->click();
    if (!check(page.profileIds().isEmpty() && listWireGuardProfiles().isEmpty(),
               "reverting unsaved draft must remove it without storing")) return 1;

    // Damaged records are reported and can be removed deliberately.
    const aowis::config::WireGuardProfiles *storage = guiWireGuardProfiles();
    if (!check(storage != nullptr
                   && storage->collection().create(R"({"type":"not-wireguard"})").hasValue(),
               "could not plant a damaged profile record")) return 1;
    page.refreshFromStorage();
    QLabel *damaged = page.findChild<QLabel *>(QStringLiteral("wireguardVpnDamagedNotice"));
    QPushButton *remove_damaged = page.findChild<QPushButton *>(QStringLiteral("wireguardVpnRemoveDamaged"));
    if (!check(damaged != nullptr && remove_damaged != nullptr && !damaged->isHidden()
                   && !remove_damaged->isHidden() && list->count() == 0,
               "damaged profile record not reported")) return 1;
    answerNextMessage(QMessageBox::Yes);
    remove_damaged->click();
    const aowis::config::Result<aowis::config::WireGuardProfileListing> after = storage->list();
    if (!check(after && after.value().unreadable.empty() && damaged->isHidden(),
               "damaged profile record not removed")) return 1;

    std::puts("WireGuard VPN settings GUI regression checks passed");
    return 0;
}
