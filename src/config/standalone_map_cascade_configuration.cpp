#include "config/standalone_map_cascade_configuration.h"

#include "config/gui_preferences.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace
{
const QString StorageGroup = QStringLiteral("embedded_map_cascade");
constexpr int StorageVersion = 1;

bool fail(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
    return false;
}
}

bool validateStandaloneMapCascadeConfiguration(
    const StandaloneMapCascadeConfiguration &configuration, QString *error)
{
    if (error != nullptr)
        error->clear();
    if (configuration.jpeg_quality < 1 || configuration.jpeg_quality > 100)
        return fail(error, QStringLiteral("JPEG quality must be between 1 and 100."));
    if (configuration.upstreams.size() > 32)
        return fail(error, QStringLiteral("At most 32 upstream MAP servers are supported."));

    const QRegularExpression valid_name(QStringLiteral("^[a-zA-Z][a-zA-Z0-9_-]{0,63}$"));
    QSet<QString> names;
    QSet<QString> urls;
    for (const StandaloneMapUpstream &upstream : configuration.upstreams)
    {
        const QString name = upstream.name.trimmed();
        if (!valid_name.match(name).hasMatch() || names.contains(name))
            return fail(error, QStringLiteral("Invalid or duplicate upstream name: %1").arg(name));
        names.insert(name);

        const QUrl url(upstream.url.trimmed(), QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || (scheme != QStringLiteral("https") && scheme != QStringLiteral("http")) ||
            url.host().isEmpty() || !url.userInfo().isEmpty() || !url.query().isEmpty() ||
            !url.fragment().isEmpty() || (!url.path().isEmpty() && url.path() != QStringLiteral("/")) ||
            url.port(-1) == 0)
        {
            return fail(error, QStringLiteral("Invalid upstream URL for %1 (HTTP(S) origin only).").arg(name));
        }
        const QString normalized = url.toString(QUrl::RemovePath | QUrl::RemoveQuery |
                                                QUrl::RemoveFragment).toLower();
        if (urls.contains(normalized))
            return fail(error, QStringLiteral("Duplicate upstream URL: %1").arg(name));
        urls.insert(normalized);
    }
    return true;
}

bool readStandaloneMapCascadeConfiguration(
    StandaloneMapCascadeConfiguration *configuration, QString *error)
{
    if (configuration == nullptr)
        return fail(error, QStringLiteral("Missing configuration destination."));
    *configuration = StandaloneMapCascadeConfiguration();
    if (error != nullptr)
        error->clear();
    const QJsonObject stored = GuiPreferences::group(StorageGroup);
    if (stored.isEmpty())
        return true;
    if (!stored.value(QStringLiteral("version")).isDouble() ||
        stored.value(QStringLiteral("version")).toInt(-1) != StorageVersion ||
        !stored.value(QStringLiteral("upstreams")).isArray() ||
        !stored.value(QStringLiteral("allow_direct_fetch")).isBool() ||
        !stored.value(QStringLiteral("poor_internet_mode")).isBool() ||
        !stored.value(QStringLiteral("jpeg_quality")).isDouble() ||
        !stored.value(QStringLiteral("adaptive_compression")).isBool())
    {
        return fail(error, QStringLiteral("Embedded MAP cascade settings have an invalid format/version."));
    }
    StandaloneMapCascadeConfiguration parsed;
    parsed.allow_direct_fetch = stored.value(QStringLiteral("allow_direct_fetch")).toBool();
    parsed.poor_internet_mode = stored.value(QStringLiteral("poor_internet_mode")).toBool();
    parsed.jpeg_quality = stored.value(QStringLiteral("jpeg_quality")).toInt(0);
    parsed.adaptive_compression = stored.value(QStringLiteral("adaptive_compression")).toBool();
    const QJsonArray entries = stored.value(QStringLiteral("upstreams")).toArray();
    for (const QJsonValue &value : entries)
    {
        if (!value.isObject())
            return fail(error, QStringLiteral("Invalid embedded MAP upstream entry."));
        const QJsonObject entry = value.toObject();
        if (!entry.value(QStringLiteral("name")).isString() ||
            !entry.value(QStringLiteral("url")).isString() ||
            !entry.value(QStringLiteral("api_key")).isString())
            return fail(error, QStringLiteral("Embedded MAP upstream entry has invalid fields."));
        StandaloneMapUpstream upstream;
        upstream.name = entry.value(QStringLiteral("name")).toString();
        upstream.url = entry.value(QStringLiteral("url")).toString();
        upstream.api_key = entry.value(QStringLiteral("api_key")).toString();
        parsed.upstreams.append(upstream);
    }
    if (!validateStandaloneMapCascadeConfiguration(parsed, error))
        return false;
    *configuration = parsed;
    return true;
}

bool saveStandaloneMapCascadeConfiguration(
    const StandaloneMapCascadeConfiguration &configuration, QString *error)
{
    if (!validateStandaloneMapCascadeConfiguration(configuration, error))
        return false;

    QJsonArray upstreams;
    for (const StandaloneMapUpstream &upstream : configuration.upstreams)
    {
        upstreams.append(QJsonObject{
            {QStringLiteral("name"), upstream.name.trimmed()},
            {QStringLiteral("url"), upstream.url.trimmed()},
            {QStringLiteral("api_key"), upstream.api_key}});
    }
    const QJsonObject stored{
        {QStringLiteral("version"), StorageVersion},
        {QStringLiteral("upstreams"), upstreams},
        {QStringLiteral("allow_direct_fetch"), configuration.allow_direct_fetch},
        {QStringLiteral("poor_internet_mode"), configuration.poor_internet_mode},
        {QStringLiteral("jpeg_quality"), configuration.jpeg_quality},
        {QStringLiteral("adaptive_compression"), configuration.adaptive_compression}};
    if (!GuiPreferences::update(StorageGroup, stored))
        return fail(error, QStringLiteral("Could not save Embedded MAP settings to AOWIS-CONFIG."));
    if (error != nullptr)
        error->clear();
    return true;
}
