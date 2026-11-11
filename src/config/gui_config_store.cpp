#include "config/gui_config_store.h"

#include <aowis/config/config_root.h>
#include <aowis/config/owner_id.h>

#include <QDebug>
#include <QDir>
#include <QStandardPaths>

#include <filesystem>
#include <mutex>
#include <optional>

namespace
{
aowis::config::Result<aowis::config::ConfigRoot> guiConfigRoot()
{
    const aowis::config::Result<aowis::config::OwnerId> owner = aowis::config::OwnerId::parse("gui");
    if (!owner)
        return aowis::config::Result<aowis::config::ConfigRoot>::failure(owner.error());
#ifdef __EMSCRIPTEN__
    return aowis::config::ConfigRoot::forCurrentUser(owner.value());
#else
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty() || !QDir::isAbsolutePath(base))
        return aowis::config::Result<aowis::config::ConfigRoot>::failure(
            {aowis::config::ErrorCode::missing_environment, "No writable user configuration location"});
    const std::filesystem::path namespace_root =
        std::filesystem::path(QDir::cleanPath(base).toStdString()) / "aowis";
    return aowis::config::ConfigRoot::fromNamespaceRoot(owner.value(), namespace_root);
#endif
}

}

QString guiConfigStoreLocation()
{
#ifdef __EMSCRIPTEN__
    return QStringLiteral("this browser's site storage");
#else
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir::toNativeSeparators(QDir(base).filePath(QStringLiteral("aowis/gui")));
#endif
}

const aowis::config::OwnerStore *guiConfigStore(QString *error)
{
    // Kept for the process lifetime once opened. A failure is not cached:
    // opening verifies the root under the owner lock, so a transient
    // lock_timeout (another GUI instance writing at startup) must not leave
    // this process without configuration until it is restarted.
    static std::mutex guard;
    static std::optional<aowis::config::OwnerStore> store;
    const std::lock_guard<std::mutex> locked(guard);
    if (store.has_value()) return &*store;
    QString reason;
    const aowis::config::Result<aowis::config::ConfigRoot> root = guiConfigRoot();
    if (!root)
    {
        reason = QString::fromStdString(root.error().message);
    }
    else
    {
        aowis::config::Result<aowis::config::OwnerStore> opened = aowis::config::OwnerStore::open(root.value());
        if (opened)
        {
            store.emplace(std::move(opened.value()));
            return &*store;
        }
        reason = QString::fromStdString(opened.error().message);
    }
    qWarning() << "AOWIS-CONFIG GUI store unavailable:" << reason;
    if (error != nullptr) *error = reason;
    return nullptr;
}
