#include "config/wireguard_profile_storage.h"

#include "config/gui_config_store.h"

#include <aowis/config/object_id.h>

#include <QObject>
#include <QSet>

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

namespace
{
QString errorText(const aowis::config::Error &error)
{
    return QString::fromStdString(error.message);
}
}

const aowis::config::WireGuardProfiles *guiWireGuardProfiles(QString *error)
{
    // Opened once on success; failures are retried (they can be transient).
    static std::mutex guard;
    static std::optional<aowis::config::WireGuardProfiles> profiles;
    const std::lock_guard<std::mutex> locked(guard);
    if (profiles.has_value()) return &*profiles;
    const aowis::config::OwnerStore *store = guiConfigStore(error);
    if (store == nullptr) return nullptr;
    aowis::config::Result<aowis::config::WireGuardProfiles> opened = aowis::config::WireGuardProfiles::open(*store);
    if (!opened)
    {
        if (error != nullptr) *error = errorText(opened.error());
        return nullptr;
    }
    profiles.emplace(std::move(opened.value()));
    return &*profiles;
}

QList<WireGuardProfileEntry> listWireGuardProfiles(QString *error)
{
    const aowis::config::WireGuardProfiles *profiles = guiWireGuardProfiles(error);
    if (profiles == nullptr) return {};
    const aowis::config::Result<aowis::config::WireGuardProfileListing> listing = profiles->list();
    if (!listing)
    {
        if (error != nullptr) *error = errorText(listing.error());
        return {};
    }
    QList<WireGuardProfileEntry> entries;
    for (const aowis::config::WireGuardProfileSummary &summary : listing.value().profiles)
        entries.append({QString::fromStdString(summary.info.id.value()), QString::fromStdString(summary.name)});
    std::sort(entries.begin(), entries.end(), [](const WireGuardProfileEntry &left, const WireGuardProfileEntry &right)
    {
        const int order = QString::compare(left.name, right.name, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : left.id < right.id;
    });
    return entries;
}

bool readWireGuardProfileText(const QString &profile_id, std::string &text, QString *error)
{
    const aowis::config::Result<aowis::config::ObjectId> id = aowis::config::ObjectId::parse(profile_id.toStdString());
    if (!id)
    {
        if (error != nullptr) *error = QObject::tr("No WireGuard profile is selected for this connection.");
        return false;
    }
    const aowis::config::WireGuardProfiles *profiles = guiWireGuardProfiles(error);
    if (profiles == nullptr) return false;
    aowis::config::Result<std::string> exported = profiles->exportText(id.value());
    if (!exported)
    {
        if (error != nullptr)
            *error = exported.error().code == aowis::config::ErrorCode::not_found
                ? QObject::tr("The selected WireGuard profile no longer exists.")
                : errorText(exported.error());
        return false;
    }
    text = std::move(exported.value());
    return true;
}

bool importWireGuardProfile(const QString &name, const QByteArray &text, QString *profile_id, QString *error)
{
    const aowis::config::WireGuardProfiles *profiles = guiWireGuardProfiles(error);
    if (profiles == nullptr) return false;
    // Name uniqueness is a GUI convention (profiles are chosen by name), so
    // check and import inside one owner session.
    aowis::config::Result<aowis::config::OwnerSession> session = profiles->collection().store().lock();
    if (!session)
    {
        if (error != nullptr) *error = errorText(session.error());
        return false;
    }
    const aowis::config::Result<aowis::config::WireGuardProfileListing> listing = profiles->list(session.value());
    if (!listing)
    {
        if (error != nullptr) *error = errorText(listing.error());
        return false;
    }
    QSet<QString> names;
    for (const aowis::config::WireGuardProfileSummary &summary : listing.value().profiles)
        names.insert(QString::fromStdString(summary.name).toCaseFolded());
    const QString base = name.trimmed().isEmpty() ? QObject::tr("WireGuard profile") : name.trimmed();
    QString unique = base;
    for (int suffix = 2; names.contains(unique.toCaseFolded()); ++suffix)
        unique = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
    const aowis::config::WireGuardProfileInput input{unique.toStdString(), std::string(), text.toStdString()};
    const aowis::config::Result<aowis::config::WireGuardProfile> imported = profiles->importText(session.value(), input);
    if (!imported)
    {
        if (error != nullptr) *error = errorText(imported.error());
        return false;
    }
    if (profile_id != nullptr) *profile_id = QString::fromStdString(imported.value().summary.info.id.value());
    return true;
}
