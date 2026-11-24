#include "map/core/gui_local_map_provider_catalog.h"
#include "map/core/map_provider_fallback_resolution.h"
#include "services/standalone_map_provider_catalog.h"

#include <utility>

GuiLocalMapProviderCatalog::GuiLocalMapProviderCatalog(StandaloneMapProviderCatalog &backend, QObject *parent)
    : QObject(parent), backend(backend)
{
    connect(&backend, &StandaloneMapProviderCatalog::localProvidersChanged,
            this, &GuiLocalMapProviderCatalog::refresh);
    this->refresh();
}

bool GuiLocalMapProviderCatalog::serverLoaded() const
{
    return this->backend.isCatalogValid();
}

void GuiLocalMapProviderCatalog::refresh()
{
    QVector<MapProviderEntry> entries;
    for (const Aowis::Map::MapProvider &provider : this->backend.providers())
    {
        MapProviderEntry entry;
        entry.id = provider.id;
        entry.name = provider.name;
        entry.url = provider.url_template;
        entry.coverage = provider.coverage;
        entry.enabled = provider.enabled;
        entry.remote_fetch_enabled = provider.remote_fetch_enabled;
        entry.minimum_zoom = provider.minimum_zoom;
        entry.maximum_zoom = provider.maximum_zoom;
        entries.append(entry);
    }
    this->entries = entries;
    emit catalogChanged();
    if (!this->serverLoaded()) emit errorOccurred(this->backend.errorMessage());
}

void GuiLocalMapProviderCatalog::saveProviders(const QVector<MapProviderEntry> &providers)
{
    QVector<Aowis::Map::MapProvider> proposed;
    for (const MapProviderEntry &entry : providers)
    {
        Aowis::Map::MapProvider value;
        // Keep MAP fields the GUI cannot edit (e.g. lossy transfer policy).
        for (const Aowis::Map::MapProvider &previous : this->backend.providers())
            if (previous.id == entry.id.trimmed().toLower()) { value = previous; break; }
        value.id = entry.id;
        value.name = entry.name;
        value.url_template = entry.url;
        value.coverage = entry.coverage;
        value.minimum_zoom = entry.minimum_zoom;
        value.maximum_zoom = entry.maximum_zoom;
        value.enabled = entry.enabled;
        value.remote_fetch_enabled = entry.remote_fetch_enabled;
        proposed.append(value);
    }
    QString error;
    if (this->backend.saveProviders(proposed, &error)) emit providersSaved();
    else emit errorOccurred(error);
}

void GuiLocalMapProviderCatalog::setFallbackStore(
    const QHash<QString, QString> &fallbacks, MapProviderCatalog::FallbackSave save)
{
    this->fallbacks = fallbacks;
    this->save_fallback = std::move(save);
    emit fallbackChanged();
}

const MapProviderEntry *GuiLocalMapProviderCatalog::provider(const QString &id) const
{
    return MapProviderFallbackResolution::find(this->entries, id);
}

bool GuiLocalMapProviderCatalog::setFallback(const QString &id, const QString &target_id)
{
    const MapProviderEntry *source = this->provider(id);
    if (source == nullptr || !this->save_fallback) return false;
    if (!target_id.isEmpty())
    {
        const QHash<QString, QString> candidate{{id, target_id}};
        if (MapProviderFallbackResolution::next(this->entries, candidate, source) == nullptr)
            return false;
    }
    if (this->fallback(id) == target_id) return true;
    if (!this->save_fallback(id, target_id)) return false;
    if (target_id.isEmpty()) this->fallbacks.remove(id);
    else this->fallbacks.insert(id, target_id);
    emit fallbackChanged();
    return true;
}

QString GuiLocalMapProviderCatalog::sourceForZoom(const QString &id, int zoom) const
{
    return MapProviderFallbackResolution::sourceForZoom(this->entries, this->fallbacks, id, zoom);
}

int GuiLocalMapProviderCatalog::maximumSourceZoom(const QString &id) const
{
    return MapProviderFallbackResolution::maximumSourceZoom(this->entries, this->fallbacks, id);
}

bool GuiLocalMapProviderCatalog::isProviderEnabled(const QString &id) const
{
    const MapProviderEntry *entry = this->provider(id);
    return entry != nullptr && entry->enabled;
}

QString GuiLocalMapProviderCatalog::firstEnabledProviderId() const
{
    for (const MapProviderEntry &entry : this->entries)
        if (entry.enabled) return entry.id;
    return {};
}

bool GuiLocalMapProviderCatalog::sourceZoomRange(const QString &id, int *minimum, int *maximum) const
{
    const MapProviderEntry *entry = this->provider(id);
    if (entry == nullptr || minimum == nullptr || maximum == nullptr) return false;
    *minimum = entry->minimum_zoom;
    *maximum = entry->maximum_zoom;
    return true;
}

QString GuiLocalMapProviderCatalog::builtinProviderId(MapProvider provider) const
{
    return MapProviderCatalog::builtinId(static_cast<int>(provider));
}
