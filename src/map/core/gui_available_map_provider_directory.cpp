#include "map/core/gui_available_map_provider_directory.h"

GuiAvailableMapProviderDirectory::GuiAvailableMapProviderDirectory(
    MapProviderCatalog &local, MapInheritedProviderClient &inherited, QObject *parent)
    : QObject(parent), local_catalog(&local), inherited_catalog(&inherited)
{
    connect(&local, &MapProviderCatalog::catalogChanged,
            this, &GuiAvailableMapProviderDirectory::rebuild);
    connect(&inherited, &MapInheritedProviderClient::catalogChanged,
            this, &GuiAvailableMapProviderDirectory::rebuild);
    rebuild();
}

void GuiAvailableMapProviderDirectory::rebuild()
{
    QVector<MapProviderEntry> combined = this->local_catalog->providers();
    QSet<QString> ids;
    for (const MapProviderEntry &entry : combined)
        ids.insert(entry.id);

    QSet<QString> inherited_ids;
    QHash<QString, QStringList> upstreams;
    for (const MapInheritedProviderEntry &entry : this->inherited_catalog->providers())
    {
        // A local definition takes precedence. The MAP server checks that
        // duplicate UUIDs advertise the same imagery source.
        if (ids.contains(entry.id)) continue;
        ids.insert(entry.id);
        MapProviderEntry available;
        available.id = entry.id;
        available.name = entry.name;
        available.url = entry.url;
        available.minimum_zoom = entry.minimum_zoom;
        available.maximum_zoom = entry.maximum_zoom;
        available.enabled = entry.enabled;
        available.remote_fetch_enabled = true;
        combined.append(available);
        inherited_ids.insert(entry.id);
        upstreams.insert(entry.id, entry.upstreams);
    }
    // Catalog refresh is periodic. Avoid dropping the rendered tile cache
    // every 15 minutes when the server advertises identical definitions.
    bool changed = this->entries.size() != combined.size() ||
        this->inherited_ids != inherited_ids ||
        this->inherited_upstreams != upstreams;
    if (!changed)
    {
        for (qsizetype index = 0; index < combined.size(); ++index)
        {
            const MapProviderEntry &before = this->entries.at(index);
            const MapProviderEntry &after = combined.at(index);
            if (before.id != after.id || before.name != after.name ||
                before.url != after.url || before.minimum_zoom != after.minimum_zoom ||
                before.maximum_zoom != after.maximum_zoom || before.enabled != after.enabled ||
                before.remote_fetch_enabled != after.remote_fetch_enabled ||
                before.coverage != after.coverage)
            {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;
    this->entries = combined;
    this->inherited_ids = inherited_ids;
    this->inherited_upstreams = upstreams;
    emit catalogChanged();
}

const MapProviderEntry *GuiAvailableMapProviderDirectory::provider(const QString &id) const
{
    for (const MapProviderEntry &entry : this->entries)
        if (entry.id == id) return &entry;
    return nullptr;
}

bool GuiAvailableMapProviderDirectory::isInherited(const QString &id) const
{
    return this->inherited_ids.contains(id);
}

QStringList GuiAvailableMapProviderDirectory::upstreams(const QString &id) const
{
    return this->inherited_upstreams.value(id);
}

bool GuiAvailableMapProviderDirectory::isProviderEnabled(const QString &id) const
{
    const MapProviderEntry *entry = provider(id);
    return entry != nullptr && entry->enabled;
}

QString GuiAvailableMapProviderDirectory::firstEnabledProviderId() const
{
    for (const MapProviderEntry &entry : this->entries)
        if (entry.enabled) return entry.id;
    return {};
}

QString GuiAvailableMapProviderDirectory::sourceForZoom(const QString &id, int zoom) const
{
    if (!isInherited(id))
        return this->local_catalog->sourceForZoom(id, zoom);
    const MapProviderEntry *entry = provider(id);
    if (entry == nullptr || !entry->enabled || zoom < entry->minimum_zoom)
        return {};
    // Match Render's local-provider behavior: above the provider's native
    // maximum zoom, the imagery renderer enlarges its highest available tile.
    return id;
}

int GuiAvailableMapProviderDirectory::maximumSourceZoom(const QString &id) const
{
    if (!isInherited(id)) return this->local_catalog->maximumSourceZoom(id);
    const MapProviderEntry *entry = provider(id);
    return entry != nullptr && entry->enabled ? entry->maximum_zoom : 0;
}

bool GuiAvailableMapProviderDirectory::sourceZoomRange(
    const QString &id, int *minimum_zoom, int *maximum_zoom) const
{
    if (!isInherited(id))
        return this->local_catalog->sourceZoomRange(id, minimum_zoom, maximum_zoom);
    const MapProviderEntry *entry = provider(id);
    if (entry == nullptr || minimum_zoom == nullptr || maximum_zoom == nullptr)
        return false;
    *minimum_zoom = entry->minimum_zoom;
    *maximum_zoom = entry->maximum_zoom;
    return true;
}

QString GuiAvailableMapProviderDirectory::builtinProviderId(MapProvider provider) const
{
    return this->local_catalog->builtinProviderId(provider);
}

void GuiAvailableMapProviderDirectory::refresh()
{
    this->inherited_catalog->refresh();
}
