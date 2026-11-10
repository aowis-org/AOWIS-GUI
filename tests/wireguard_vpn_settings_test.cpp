#include "settings/wireguard_vpn_settings_widget.h"

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
