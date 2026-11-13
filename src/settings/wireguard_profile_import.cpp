#include "settings/wireguard_profile_import.h"

#ifdef AOWIS_GUI_WIREGUARD_PROBE

#include "config/wireguard_profile_storage.h"

#include <aowis/network/wireguard_conf.h>

#include <QCoreApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStringList>

#include <cstddef>
#include <string_view>
#include <utility>

namespace
{
// Sources are marked with QT_TRANSLATE_NOOP in this context.
QString translated(const char *source)
{
    return QCoreApplication::translate("WireGuardProfileImport", source);
}

void warn(QWidget *parent, const QString &message)
{
    // open(), not exec(): no nested event loop.
    QMessageBox *dialog = new QMessageBox(QMessageBox::Warning,
        translated(QT_TRANSLATE_NOOP("WireGuardProfileImport", "WireGuard profile not imported")), message, QMessageBox::Ok, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->open();
}
}

void importWireGuardProfileFromFile(QWidget *parent, std::function<void(const QString &)> imported)
{
    const QString path = QFileDialog::getOpenFileName(parent, translated(QT_TRANSLATE_NOOP("WireGuardProfileImport", "Import WireGuard profile")),
        QString(), QStringLiteral("WireGuard (*.conf);;*"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        warn(parent, translated(QT_TRANSLATE_NOOP("WireGuardProfileImport", "The file could not be read: %1")).arg(file.errorString()));
        return;
    }
    const QByteArray text = file.read(static_cast<qint64>(aowis::network::maxWireGuardConfBytes) + 1);
    const aowis::network::WireGuardConfParse parsed =
        aowis::network::parseWireGuardConf(std::string_view(text.constData(), static_cast<std::size_t>(text.size())));
    if (!parsed.ok())
    {
        QStringList problems;
        for (const aowis::network::WireGuardConfDiagnostic &diagnostic : parsed.diagnostics)
        {
            if (diagnostic.severity != aowis::network::WireGuardConfSeverity::error) continue;
            const QString message = QString::fromStdString(diagnostic.message);
            problems.append(diagnostic.line > 0
                ? translated(QT_TRANSLATE_NOOP("WireGuardProfileImport", "Line %1: %2")).arg(static_cast<qulonglong>(diagnostic.line)).arg(message) : message);
            if (problems.size() == 10) break;
        }
        warn(parent, translated(QT_TRANSLATE_NOOP("WireGuardProfileImport", "The embedded WireGuard client cannot use this profile:\n%1"))
                         .arg(problems.join(QLatin1Char('\n'))));
        return;
    }
    QString profile_id;
    QString error;
    if (!importWireGuardProfile(QFileInfo(path).completeBaseName(), text, &profile_id, &error))
    {
        warn(parent, error);
        return;
    }
    imported(profile_id);
}

#endif // AOWIS_GUI_WIREGUARD_PROBE
