#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

// GUI preferences in the GUI's AOWIS-CONFIG store (config/gui_config_store.h),
// collection "preferences", one record per group:
//   {"preferences_version":1, "group":"<name>", "values":{<key>:<JSON value>}}
// Same storage natively and in the browser. Records of another
// preferences_version are never read or modified.
//
// All groups are read once per process on first use and cached. update()
// merges the given keys into the stored group inside one owner session, so
// two GUI instances changing different keys of one group keep both.
namespace GuiPreferences
{
// The cached values of a group; empty if absent or the store is unavailable.
QJsonObject group(const QString &name);
// One cached value; undefined if absent.
QJsonValue value(const QString &group, const QString &key);
// Stores changes (and deletes removed keys) in group; updates the cache only
// on success. Returns false if nothing could be stored.
bool update(const QString &group, const QJsonObject &changes, const QStringList &removed = {});
// Drops the cache; the next read reloads from storage (tests, restore).
void reload();
}
