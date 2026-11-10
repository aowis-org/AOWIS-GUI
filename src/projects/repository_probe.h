#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

class QObject;
class QNetworkRequest;

struct RepositoryProbeResult
{
    QByteArray body;
    int status = 0;
    QString error;
    bool transport_ok = false;
};

void probeRepository(const QNetworkRequest &request, const QString &profile, QObject *context,
                     std::function<void(RepositoryProbeResult)> finished);
