#include "config/gui_preferences.h"

#include "config/gui_config_store.h"

#include <aowis/config/object_store.h>

#include <QDebug>
#include <QHash>
#include <QJsonDocument>

#include <mutex>
#include <optional>
#include <string>

namespace
{
const char *const preferences_collection = "preferences";
constexpr int preferences_version = 1;

std::mutex &cacheGuard()
{
    static std::mutex guard;
    return guard;
}

std::optional<QHash<QString, QJsonObject>> &cache()
{
    static std::optional<QHash<QString, QJsonObject>> groups;
    return groups;
}

QString errorText(const aowis::config::Error &error)
{
    return QString::fromStdString(error.message);
}

std::optional<aowis::config::ObjectCollection> openPreferences(QString *error)
{
    const aowis::config::OwnerStore *store = guiConfigStore(error);
    if (store == nullptr) return std::nullopt;
    aowis::config::Result<aowis::config::ObjectCollection> opened = store->collection(preferences_collection);
    if (!opened)
    {
        if (error != nullptr) *error = errorText(opened.error());
        return std::nullopt;
    }
    return std::move(opened.value());
}

QJsonObject payloadObject(const aowis::config::StoredObject &stored)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(stored.json)).object();
}

QString recordGroup(const QJsonObject &record)
{
    if (record.value(QStringLiteral("preferences_version")).toInt(0) != preferences_version) return QString();
    return record.value(QStringLiteral("group")).toString();
}

// The record of each group: the first one in ID (creation) order.
QHash<QString, const aowis::config::StoredObject *> recordsByGroup(const aowis::config::ObjectSnapshot &snapshot)
{
    QHash<QString, const aowis::config::StoredObject *> groups;
    for (const aowis::config::StoredObject &stored : snapshot.objects)
    {
        const QString group = recordGroup(payloadObject(stored));
        if (!group.isEmpty() && !groups.contains(group)) groups.insert(group, &stored);
    }
    return groups;
}

std::string serializeGroup(const QString &group, const QJsonObject &values)
{
    const QJsonObject record{
        {QStringLiteral("preferences_version"), preferences_version},
        {QStringLiteral("group"), group},
        {QStringLiteral("values"), values}};
    return QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString();
}

QHash<QString, QJsonObject> loadAll()
{
    QHash<QString, QJsonObject> groups;
    QString error;
    const std::optional<aowis::config::ObjectCollection> collection = openPreferences(&error);
    if (!collection.has_value())
    {
        qWarning() << "Unable to load GUI preferences, using defaults:" << error;
        return groups;
    }
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection->readAll();
    if (!snapshot)
    {
        qWarning() << "Unable to load GUI preferences, using defaults:" << errorText(snapshot.error());
        return groups;
    }
    for (const aowis::config::UnreadableObject &damaged : snapshot.value().unreadable)
        qWarning() << "Skipping unreadable preferences record" << QString::fromStdString(damaged.filename)
                   << errorText(damaged.error);
    const QHash<QString, const aowis::config::StoredObject *> records = recordsByGroup(snapshot.value());
    for (auto record = records.constBegin(); record != records.constEnd(); ++record)
        groups.insert(record.key(), payloadObject(*record.value()).value(QStringLiteral("values")).toObject());
    return groups;
}

const QHash<QString, QJsonObject> &loadedLocked()
{
    std::optional<QHash<QString, QJsonObject>> &groups = cache();
    if (!groups.has_value()) groups = loadAll();
    return *groups;
}
}

namespace GuiPreferences
{
QJsonObject group(const QString &name)
{
    const std::lock_guard<std::mutex> locked(cacheGuard());
    return loadedLocked().value(name);
}

QJsonValue value(const QString &group_name, const QString &key)
{
    return group(group_name).value(key);
}

bool update(const QString &group_name, const QJsonObject &changes, const QStringList &removed)
{
    const std::lock_guard<std::mutex> locked(cacheGuard());
    (void)loadedLocked();
    QString error;
    const std::optional<aowis::config::ObjectCollection> collection = openPreferences(&error);
    if (!collection.has_value())
    {
        qWarning() << "Unable to save GUI preferences:" << error;
        return false;
    }
    // Read-modify-write in one owner session: keys another instance stored
    // in the same group meanwhile are kept.
    aowis::config::Result<aowis::config::OwnerSession> session = collection->store().lock();
    if (!session)
    {
        qWarning() << "Unable to save GUI preferences:" << errorText(session.error());
        return false;
    }
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection->readAll(session.value());
    if (!snapshot)
    {
        qWarning() << "Unable to save GUI preferences:" << errorText(snapshot.error());
        return false;
    }
    const aowis::config::StoredObject *stored = recordsByGroup(snapshot.value()).value(group_name, nullptr);
    QJsonObject values = stored != nullptr
        ? payloadObject(*stored).value(QStringLiteral("values")).toObject() : QJsonObject();
    for (auto change = changes.constBegin(); change != changes.constEnd(); ++change)
        values.insert(change.key(), change.value());
    for (const QString &key : removed)
        values.remove(key);
    const std::string payload = serializeGroup(group_name, values);
    if (stored != nullptr && stored->json != payload)
    {
        const aowis::config::Result<aowis::config::StoredObject> updated =
            collection->update(session.value(), stored->info.id, payload, stored->info.revision);
        if (!updated)
        {
            qWarning() << "Unable to save GUI preferences" << group_name << errorText(updated.error());
            return false;
        }
    }
    else if (stored == nullptr && !values.isEmpty())
    {
        const aowis::config::Result<aowis::config::StoredObject> created =
            collection->create(session.value(), payload);
        if (!created)
        {
            qWarning() << "Unable to save GUI preferences" << group_name << errorText(created.error());
            return false;
        }
    }
    (*cache())[group_name] = values;
    return true;
}

void reload()
{
    const std::lock_guard<std::mutex> locked(cacheGuard());
    cache().reset();
}
}
