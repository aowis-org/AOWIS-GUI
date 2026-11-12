#ifndef GUI_AVAILABLE_MAP_PROVIDER_DIRECTORY_H
#define GUI_AVAILABLE_MAP_PROVIDER_DIRECTORY_H

#include "map/core/map_imagery_provider_directory.h"
#include "map/core/map_provider_catalog.h"
#include "services/map_inherited_provider_client.h"

#include <QObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

// View-only union of the MAP server's local providers and its inherited
// providers. The editable MapProviderCatalog remains local-only. The renderer
// sees this through MapImageryProviderDirectory; no MAP configuration is read
// or written by this adapter.
class GuiAvailableMapProviderDirectory final : public QObject, public MapImageryProviderDirectory
{
    Q_OBJECT
public:
    GuiAvailableMapProviderDirectory(MapProviderCatalog &local,
                                     MapInheritedProviderClient &inherited,
                                     QObject *parent = nullptr);

    const QVector<MapProviderEntry> &providers() const { return this->entries; }
    const MapProviderEntry *provider(const QString &id) const;
    bool isInherited(const QString &id) const;
    QStringList upstreams(const QString &id) const;

    bool isProviderEnabled(const QString &id) const override;
    QString firstEnabledProviderId() const override;
    QString sourceForZoom(const QString &id, int zoom) const override;
    int maximumSourceZoom(const QString &id) const override;
    bool sourceZoomRange(const QString &id, int *minimum_zoom, int *maximum_zoom) const override;
    QString builtinProviderId(MapProvider provider) const override;

    // Refresh only the read-only available catalog, not the editable local
    // provider definitions. The last validated result is retained offline.
    void refresh();

signals:
    void catalogChanged();

private:
    void rebuild();

    MapProviderCatalog *local_catalog = nullptr;
    MapInheritedProviderClient *inherited_catalog = nullptr;
    QVector<MapProviderEntry> entries;
    QSet<QString> inherited_ids;
    QHash<QString, QStringList> inherited_upstreams;
};

// Process-wide directory used by the MapModel and navigation controls.
// Constructed after the existing local MapProviderCatalog is configured.
GuiAvailableMapProviderDirectory &guiAvailableMapProviderDirectory();

#endif // GUI_AVAILABLE_MAP_PROVIDER_DIRECTORY_H
