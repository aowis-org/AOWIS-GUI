#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QUuid>

// DVMDB's stable Distribution annotation-layer identity. This identifier is
// NOT a user-created UUIDv7. It is derived from the network root UUID and is
// independent of the current branch/revision. Keep this exact byte contract
// in sync with DVMDB's annotations::distributionLayerUuid().
inline QUuid distributionMapAnnotationLayerId(const QUuid &network_uuid)
{
    if (network_uuid.isNull())
        return QUuid();
    QByteArray seed("aowis-distribution-annotation-layer-v1");
    seed.append(network_uuid.toRfc4122());
    QByteArray digest = QCryptographicHash::hash(seed, QCryptographicHash::Sha256).left(16);
    digest[6] = static_cast<char>((static_cast<unsigned char>(digest[6]) & 0x0fU) | 0x80U);
    digest[8] = static_cast<char>((static_cast<unsigned char>(digest[8]) & 0x3fU) | 0x80U);
    return QUuid::fromRfc4122(digest);
}
