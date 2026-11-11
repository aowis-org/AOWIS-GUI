#include <aowis/model/units/quantity.h>
#include "config/unit_profile_storage.h"

#include "config/gui_config_store.h"
#include "config/gui_preferences.h"

#include <aowis/config/object_store.h>

#include <QDebug>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace
{
const char *const unit_profiles_collection = "unit-profiles";

QString selection_group()
{
    return QStringLiteral("units");
}
constexpr int unit_profile_version = 1;

QString errorText(const aowis::config::Error &error)
{
    return QString::fromStdString(error.message);
}

std::optional<aowis::config::ObjectCollection> openUnitProfiles(QString *error)
{
    const aowis::config::OwnerStore *store = guiConfigStore(error);
    if (store == nullptr) return std::nullopt;
    aowis::config::Result<aowis::config::ObjectCollection> opened = store->collection(unit_profiles_collection);
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

// Records this build understands; everything else is left alone.
bool ownRecord(const QJsonObject &record)
{
    return record.value(QStringLiteral("unit_profile_version")).toInt(0) == unit_profile_version
        && !record.value(QStringLiteral("profile_id")).toString().trimmed().isEmpty();
}

QString fieldKey(const aowis::units::UnitField &field)
{
    return QString::fromLatin1(field.key.data(), static_cast<qsizetype>(field.key.size()));
}

std::string serializeProfile(const aowis::units::UnitProfile &profile, int position)
{
    QJsonObject overrides;
    for (const aowis::units::UnitField &field : aowis::units::unit_fields)
    {
        const aowis::units::UnitSelections::const_iterator choice = profile.overrides.find(field.key);
        if (choice != profile.overrides.end())
            overrides.insert(fieldKey(field), QString::fromStdString(choice->second));
    }
    const QJsonObject record{
        {QStringLiteral("unit_profile_version"), unit_profile_version},
        {QStringLiteral("profile_id"), QString::fromStdString(profile.id)},
        {QStringLiteral("name"), QString::fromStdString(profile.name)},
        {QStringLiteral("position"), position},
        {QStringLiteral("overrides"), overrides}};
    // Sorted keys: unchanged profiles serialize byte-identically, so saving
    // them writes nothing.
    return QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString();
}

struct LoadedProfile
{
    aowis::units::UnitProfile profile;
    int position = 0;
};
}

UnitProfileSnapshot ConfigUnitProfileStorage::load() const
{
    UnitProfileSnapshot result;
    const QJsonObject selection = GuiPreferences::group(selection_group());
    result.active_id = selection.value(QStringLiteral("selected_profile_id")).toString();
    result.legacy_active_name = selection.value(QStringLiteral("selected_profile"))
                                    .toString(QStringLiteral("AOWIS Canonical"));

    QString error;
    const std::optional<aowis::config::ObjectCollection> collection = openUnitProfiles(&error);
    if (!collection.has_value())
    {
        qWarning() << "Unable to load custom unit profiles:" << error;
        return result;
    }
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection->readAll();
    if (!snapshot)
    {
        qWarning() << "Unable to load custom unit profiles:" << errorText(snapshot.error());
        return result;
    }
    for (const aowis::config::UnreadableObject &damaged : snapshot.value().unreadable)
        qWarning() << "Skipping unreadable unit profile record" << QString::fromStdString(damaged.filename)
                   << errorText(damaged.error);

    std::vector<LoadedProfile> loaded;
    for (const aowis::config::StoredObject &stored : snapshot.value().objects)
    {
        const QJsonObject record = payloadObject(stored);
        if (!ownRecord(record))
        {
            qWarning() << "Skipping unit profile record of an unsupported version"
                       << QString::fromStdString(stored.info.id.value());
            continue;
        }
        LoadedProfile entry;
        entry.position = record.value(QStringLiteral("position")).toInt(0);
        entry.profile.id = record.value(QStringLiteral("profile_id")).toString().trimmed().toStdString();
        entry.profile.name = record.value(QStringLiteral("name")).toString().trimmed().toStdString();
        entry.profile.builtin = false;
        const QJsonObject overrides = record.value(QStringLiteral("overrides")).toObject();
        int accepted = 0;
        for (const aowis::units::UnitField &field : aowis::units::unit_fields)
        {
            const QJsonValue value = overrides.value(fieldKey(field));
            if (!value.isString()) continue;
            const std::string unit = value.toString().toStdString();
            if (!aowis::units::isAllowedUnit(field.key, unit)) continue;
            // Inserted as stored: an explicit canonical choice stays explicit.
            entry.profile.overrides.emplace(std::string(field.key), unit);
            ++accepted;
        }
        if (accepted != overrides.size())
            qWarning() << "Ignoring unknown or invalid unit choices in profile"
                       << QString::fromStdString(entry.profile.name);
        loaded.push_back(std::move(entry));
    }
    std::stable_sort(loaded.begin(), loaded.end(), [](const LoadedProfile &left, const LoadedProfile &right)
    {
        return left.position < right.position;
    });

    // Names are case-sensitive and unique, also against built-in profiles;
    // ids are unique. A colliding id gets a fresh one (the record keeps the
    // old one until the next save).
    QSet<QString> names;
    QSet<QString> ids;
    for (const aowis::units::UnitProfile &profile : aowis::units::predefinedProfiles())
    {
        names.insert(QString::fromStdString(profile.name));
        ids.insert(QString::fromStdString(profile.id));
    }
    for (LoadedProfile &entry : loaded)
    {
        const QString name = QString::fromStdString(entry.profile.name);
        if (name.isEmpty() || names.contains(name))
        {
            qWarning() << "Skipping unit profile with an empty or duplicate name";
            continue;
        }
        QString id = QString::fromStdString(entry.profile.id);
        if (ids.contains(id))
        {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            entry.profile.id = id.toStdString();
        }
        names.insert(name);
        ids.insert(id);
        result.custom_profiles.push_back(std::move(entry.profile));
    }
    return result;
}

bool ConfigUnitProfileStorage::save(const UnitProfileSnapshot &snapshot) const
{
    bool saved = true;
    QString error;
    const std::optional<aowis::config::ObjectCollection> collection = openUnitProfiles(&error);
    if (!collection.has_value())
    {
        qWarning() << "Unable to save custom unit profiles:" << error;
        saved = false;
    }
    std::optional<aowis::config::OwnerSession> session;
    if (saved)
    {
        // One owner session: another GUI instance cannot interleave between
        // reading the current records and replacing them.
        aowis::config::Result<aowis::config::OwnerSession> locked = collection->store().lock();
        if (!locked)
        {
            qWarning() << "Unable to save custom unit profiles:" << errorText(locked.error());
            saved = false;
        }
        else
        {
            session.emplace(std::move(locked.value()));
        }
    }
    std::optional<aowis::config::ObjectSnapshot> current;
    if (saved)
    {
        aowis::config::Result<aowis::config::ObjectSnapshot> read = collection->readAll(*session);
        if (!read)
        {
            qWarning() << "Unable to save custom unit profiles:" << errorText(read.error());
            saved = false;
        }
        else
        {
            current.emplace(std::move(read.value()));
        }
    }
    if (saved)
    {
        std::vector<const aowis::config::StoredObject *> own;
        QHash<QString, const aowis::config::StoredObject *> by_profile;
        for (const aowis::config::StoredObject &stored : current->objects)
        {
            const QJsonObject record = payloadObject(stored);
            if (!ownRecord(record)) continue;
            own.push_back(&stored);
            const QString profile_id = record.value(QStringLiteral("profile_id")).toString().trimmed();
            if (!by_profile.contains(profile_id)) by_profile.insert(profile_id, &stored);
        }
        QSet<QString> kept;
        int position = 0;
        for (const aowis::units::UnitProfile &profile : snapshot.custom_profiles)
        {
            if (!saved) break;
            if (profile.builtin || profile.id.empty()) continue;
            const std::string payload = serializeProfile(profile, position++);
            const aowis::config::StoredObject *stored = by_profile.value(QString::fromStdString(profile.id), nullptr);
            if (stored != nullptr && !kept.contains(QString::fromStdString(stored->info.id.value())))
            {
                kept.insert(QString::fromStdString(stored->info.id.value()));
                if (stored->json == payload) continue;
                const aowis::config::Result<aowis::config::StoredObject> updated =
                    collection->update(*session, stored->info.id, payload, stored->info.revision);
                if (!updated)
                {
                    qWarning() << "Unable to update unit profile" << errorText(updated.error());
                    saved = false;
                }
                continue;
            }
            const aowis::config::Result<aowis::config::StoredObject> created = collection->create(*session, payload);
            if (!created)
            {
                qWarning() << "Unable to create unit profile" << errorText(created.error());
                saved = false;
                continue;
            }
            kept.insert(QString::fromStdString(created.value().info.id.value()));
        }
        // Delete only after every kept/new record is written, so a failure
        // never leaves fewer profiles than before. Own records that are no
        // longer listed (removed profiles, duplicates of one profile_id) go;
        // foreign and unreadable records are left alone.
        for (const aowis::config::StoredObject *stored : own)
        {
            if (!saved) break;
            if (kept.contains(QString::fromStdString(stored->info.id.value()))) continue;
            const aowis::config::Result<bool> erased = collection->erase(*session, stored->info.id, stored->info.revision);
            if (!erased)
            {
                qWarning() << "Unable to delete unit profile" << errorText(erased.error());
                saved = false;
            }
        }
    }
    const bool selected = GuiPreferences::update(selection_group(), QJsonObject{
        {QStringLiteral("selected_profile_id"), snapshot.active_id},
        {QStringLiteral("selected_profile"), snapshot.legacy_active_name}});
    return saved && selected;
}
