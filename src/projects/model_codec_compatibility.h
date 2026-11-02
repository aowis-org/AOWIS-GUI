#pragma once

#include "aowis_model_codec_fingerprint.h"

#include <QJsonObject>
#include <QString>

// The generated checksum is derived from MODEL source *contents*, including
// uncommitted changes. Only a matching, explicitly advertised fingerprint
// permits decoding or writing MODEL records against a DVMDB process.
namespace ModelCodecCompatibility
{
inline QString fingerprint()
{
    return QString::fromLatin1(AOWIS_MODEL_CODEC_FINGERPRINT);
}

inline QString error(const QJsonObject &repository)
{
    const QString remote = repository.value(QStringLiteral("modelCodecFingerprint")).toString();
    if (remote == fingerprint()) return {};
    if (remote.isEmpty())
        return QStringLiteral("MODEL codec compatibility cannot be verified: DVMDB does not "
                              "advertise a fingerprint. Rebuild and restart DVMDB.");
    return QStringLiteral("MODEL codec mismatch between GUI and DVMDB "
                          "(GUI %1, DVMDB %2). Rebuild both against identical "
                          "AOWIS-SERVER-MODEL sources and restart both processes.")
        .arg(fingerprint(), remote);
}
}
