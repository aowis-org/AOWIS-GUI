#include "config/project_server_duplicates.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace
{
bool isLocal(const ProjectServerProfile &profile)
{
    return profile.connection_type == QStringLiteral("local");
}

void stripTrailingSlashes(QString &path)
{
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
}
}

QString normalizedProjectServerEndpoint(const QString &endpoint)
{
    const QString trimmed = endpoint.trimmed();
    const QUrl url(trimmed);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")))
        return trimmed;
    QString path = url.path(QUrl::FullyEncoded);
    stripTrailingSlashes(path);
    if (path.endsWith(QStringLiteral("/dvmdb/v1")))
        path.chop(QStringLiteral("/dvmdb/v1").size());
    stripTrailingSlashes(path);
    QUrl normalized;
    normalized.setScheme(scheme);
    normalized.setHost(url.host().toLower());
    normalized.setPort(url.port(scheme == QStringLiteral("https") ? 443 : 80));
    normalized.setPath(path, QUrl::TolerantMode);
    return normalized.toString(QUrl::FullyEncoded);
}

QString normalizedProjectServerLocalPath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(path.trimmed());
    const QString canonical = QFileInfo(cleaned).canonicalFilePath();
    return canonical.isEmpty() ? cleaned : canonical;
}

ProjectServerDuplicate findProjectServerDuplicate(const QList<ProjectServerProfile> &existing,
                                                  const ProjectServerProfile &candidate)
{
    const QString name = candidate.name.trimmed();
    if (!name.isEmpty())
        for (qsizetype i = 0; i < existing.size(); ++i)
            if (existing.at(i).name.trimmed().compare(name, Qt::CaseInsensitive) == 0)
                return {ProjectServerDuplicateKind::Name, i};

    if (isLocal(candidate))
    {
        const QString path = normalizedProjectServerLocalPath(candidate.local_path);
        if (!candidate.local_path.trimmed().isEmpty())
            for (qsizetype i = 0; i < existing.size(); ++i)
                if (isLocal(existing.at(i)) && !existing.at(i).local_path.trimmed().isEmpty()
                    && normalizedProjectServerLocalPath(existing.at(i).local_path) == path)
                    return {ProjectServerDuplicateKind::LocalPath, i};
    }
    else if (!candidate.endpoint.trimmed().isEmpty())
    {
        // The same URL behind a different WireGuard tunnel can reach another
        // server (private addresses), so the tunnel is part of the location.
        const QString endpoint = normalizedProjectServerEndpoint(candidate.endpoint);
        for (qsizetype i = 0; i < existing.size(); ++i)
            if (!isLocal(existing.at(i))
                && existing.at(i).wireguard_profile_id == candidate.wireguard_profile_id
                && !existing.at(i).endpoint.trimmed().isEmpty()
                && normalizedProjectServerEndpoint(existing.at(i).endpoint) == endpoint)
                return {ProjectServerDuplicateKind::Endpoint, i};
    }

    if (!candidate.pending && !candidate.id.isEmpty())
        for (qsizetype i = 0; i < existing.size(); ++i)
            if (!existing.at(i).pending && existing.at(i).id == candidate.id)
                return {ProjectServerDuplicateKind::Repository, i};

    return {};
}
