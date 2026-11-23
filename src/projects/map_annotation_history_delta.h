#pragma once

#include <aowis/model/map/map_annotation.h>
#include <aowis/model/serialization/model_codec.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QString>
#include <QUuid>

#include <optional>
#include <utility>

// annotationDelta version 1 (DVMDB docs/history-transport.md): the changed
// annotations of one network's Distribution annotation layer between the
// History source and destination drafts. Like the hydraulic historyDelta it
// is a transport, not a MODEL journal transaction. Apply it to a COPY of the
// verified layer; nothing is published until every before-image, the source
// order digest and the destination order digest have been checked.
namespace MapAnnotationHistoryDelta
{
// DVMDB stores at most 4096 chunks of 256 members per layer.
inline constexpr qsizetype max_members = 4096 * 256;
// At most 20,000 changes per draft, so a delta between two drafts has at
// most 40,000 annotation edits.
inline constexpr qsizetype max_edits = 40000;

inline bool error(QString *message, const QString &detail)
{
    if (message) *message = detail;
    return false;
}

// SHA-256 (hex) of ASCII "AOWIS-ANNOTATION-DELTA-1", the 16-byte RFC 4122
// layer UUID, then only when the member order changed (`order` non-null):
// u32be member count and the 16-byte UUID of every member in layer order.
inline QString orderDigest(const QUuid &layer_id, const QList<MapAnnotation> *order)
{
    QByteArray bytes("AOWIS-ANNOTATION-DELTA-1");
    bytes.append(layer_id.toRfc4122());
    if (order)
    {
        const quint32 count = static_cast<quint32>(order->size());
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.append(static_cast<char>((count >> shift) & 0xffU));
        for (const MapAnnotation &member : *order) bytes.append(member.uuid.toRfc4122());
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

inline bool canonicalUuid(const QJsonValue &value, QUuid &uuid)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    uuid = QUuid(text);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == text;
}

inline bool decodeImage(const QJsonValue &value, std::optional<QByteArray> &decoded,
                        QString *message)
{
    decoded.reset();
    if (value.isNull()) return true;
    if (!value.isString()) return error(message, QStringLiteral("Invalid annotation History image field"));
    const QByteArray text = value.toString().toLatin1();
    if (text.isEmpty() || text.size() > 1024 * 1024)
        return error(message, QStringLiteral("Invalid annotation History image size"));
    const QByteArray::FromBase64Result result = QByteArray::fromBase64Encoding(
        text, QByteArray::AbortOnBase64DecodingErrors);
    if (!result || result.decoded.isEmpty() || result.decoded.toBase64() != text)
        return error(message, QStringLiteral("Noncanonical annotation History base64 image"));
    decoded = result.decoded;
    return true;
}

inline bool apply(const QJsonObject &delta, const QUuid &layer_id, const QUuid &network_id,
                  const QList<MapAnnotation> &source, QList<MapAnnotation> &destination,
                  QString *message = nullptr)
{
    if (message) message->clear();
    QUuid delta_layer;
    QUuid delta_network;
    if (delta.size() != 9 || delta.value(QStringLiteral("version")) != QJsonValue(1) ||
        !canonicalUuid(delta.value(QStringLiteral("layerId")), delta_layer) ||
        delta_layer != layer_id ||
        !canonicalUuid(delta.value(QStringLiteral("networkId")), delta_network) ||
        delta_network != network_id ||
        !delta.value(QStringLiteral("orderChanged")).isBool() ||
        !delta.value(QStringLiteral("fromSha256")).isString() ||
        !delta.value(QStringLiteral("toSha256")).isString() ||
        !delta.value(QStringLiteral("edits")).isArray() ||
        !delta.value(QStringLiteral("insertions")).isArray() ||
        !delta.contains(QStringLiteral("orderBase64")) ||
        (!delta.value(QStringLiteral("orderBase64")).isNull() &&
         !delta.value(QStringLiteral("orderBase64")).isString()))
        return error(message, QStringLiteral("Invalid annotation History delta envelope"));
    const bool order_changed = delta.value(QStringLiteral("orderChanged")).toBool();
    const QJsonArray edits = delta.value(QStringLiteral("edits")).toArray();
    const QJsonArray insertions = delta.value(QStringLiteral("insertions")).toArray();
    const QJsonValue order_value = delta.value(QStringLiteral("orderBase64"));
    if (!order_changed && (!insertions.isEmpty() || !order_value.isNull()))
        return error(message, QStringLiteral("Annotation History order change without orderChanged"));
    if (!order_value.isNull() && !insertions.isEmpty())
        return error(message, QStringLiteral("Annotation History delta has both insertions and an order"));
    if (edits.size() > max_edits || insertions.size() > max_edits)
        return error(message, QStringLiteral("Annotation History delta exceeds its edit limit"));
    const QString from = delta.value(QStringLiteral("fromSha256")).toString();
    const QString to = delta.value(QStringLiteral("toSha256")).toString();
    if (from.size() != 64 || to.size() != 64 ||
        orderDigest(layer_id, order_changed ? &source : nullptr) != from)
        return error(message, QStringLiteral("Annotation History source digest mismatch"));

    QHash<QUuid, qsizetype> index;
    index.reserve(source.size());
    for (qsizetype position = 0; position < source.size(); ++position)
    {
        if (index.contains(source.at(position).uuid))
            return error(message, QStringLiteral("Duplicate annotation UUID in the verified layer"));
        index.insert(source.at(position).uuid, position);
    }
    QList<MapAnnotation> next = source;
    QSet<qsizetype> removed;
    QHash<QUuid, MapAnnotation> created;
    QSet<QUuid> seen;
    for (const QJsonValue &value : edits)
    {
        if (!value.isObject())
            return error(message, QStringLiteral("Annotation History edit is not an object"));
        const QJsonObject edit = value.toObject();
        QUuid id;
        if (edit.size() != 3 || !canonicalUuid(edit.value(QStringLiteral("id")), id) ||
            !edit.contains(QStringLiteral("beforeBase64")) ||
            !edit.contains(QStringLiteral("afterBase64")) || seen.contains(id))
            return error(message, QStringLiteral("Malformed or duplicate annotation History edit"));
        seen.insert(id);
        std::optional<QByteArray> before;
        std::optional<QByteArray> after;
        if (!decodeImage(edit.value(QStringLiteral("beforeBase64")), before, message) ||
            !decodeImage(edit.value(QStringLiteral("afterBase64")), after, message) ||
            (!before && !after))
            return error(message, QStringLiteral("Annotation History images are invalid"));
        if ((!before || !after) && !order_changed)
            return error(message, QStringLiteral("Annotation membership change without orderChanged"));
        const qsizetype found = index.value(id, -1);
        if ((found < 0) != !before.has_value())
            return error(message, QStringLiteral("Annotation History membership before-image mismatch"));
        if (found >= 0 && aowis::model::ModelCodec<MapAnnotation>::encode(next.at(found)) != *before)
            return error(message, QStringLiteral("Stale annotation History before-image"));
        if (!after)
        {
            removed.insert(found);
            continue;
        }
        QString codec_error;
        const std::optional<MapAnnotation> decoded =
            aowis::model::ModelCodec<MapAnnotation>::decode(*after, &codec_error);
        if (!decoded || decoded->uuid != id ||
            aowis::model::ModelCodec<MapAnnotation>::encode(*decoded) != *after)
            return error(message, QStringLiteral("Noncanonical annotation History after-image: ") +
                                      codec_error);
        if (found < 0) created.insert(id, *decoded);
        else next[found] = *decoded;
    }
    QList<MapAnnotation> kept;
    kept.reserve(next.size() - removed.size());
    for (qsizetype position = 0; position < next.size(); ++position)
        if (!removed.contains(position)) kept.append(std::move(next[position]));
    const qsizetype total = kept.size() + created.size();
    if (total > max_members)
        return error(message, QStringLiteral("Annotation History result exceeds the layer limit"));

    QList<MapAnnotation> ordered;
    ordered.reserve(total);
    if (!order_value.isNull())
    {
        // Members that stay changed their relative order: arrange every
        // member as listed by the complete destination order.
        const QByteArray text = order_value.toString().toLatin1();
        const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
            text, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.toBase64() != text ||
            decoded.decoded.size() != total * 16)
            return error(message, QStringLiteral("Invalid annotation History order"));
        QHash<QUuid, qsizetype> kept_index;
        kept_index.reserve(kept.size());
        for (qsizetype position = 0; position < kept.size(); ++position)
            kept_index.insert(kept.at(position).uuid, position);
        QSet<QUuid> placed;
        for (qsizetype offset = 0; offset < decoded.decoded.size(); offset += 16)
        {
            const QUuid id = QUuid::fromRfc4122(decoded.decoded.mid(offset, 16));
            if (placed.contains(id))
                return error(message, QStringLiteral("Annotation History order lists a member twice"));
            placed.insert(id);
            const QHash<QUuid, MapAnnotation>::iterator fresh = created.find(id);
            if (fresh != created.end())
            {
                ordered.append(std::move(fresh.value()));
                continue;
            }
            const qsizetype position = kept_index.value(id, -1);
            if (position < 0)
                return error(message, QStringLiteral("Annotation History order lists an unknown member"));
            ordered.append(std::move(kept[position]));
        }
    }
    else
    {
        // Members that stay keep their order; creations go to their final
        // positions, ascending.
        if (insertions.size() != created.size())
            return error(message, QStringLiteral("Annotation History creations without positions"));
        qsizetype taken = 0;
        qsizetype previous = -1;
        for (const QJsonValue &value : insertions)
        {
            const QJsonObject insertion = value.toObject();
            QUuid id;
            const double requested = insertion.value(QStringLiteral("position")).toDouble(-1);
            if (!value.isObject() || insertion.size() != 2 ||
                !canonicalUuid(insertion.value(QStringLiteral("id")), id) ||
                !insertion.value(QStringLiteral("position")).isDouble() ||
                requested < 0 || requested >= static_cast<double>(total) ||
                requested != static_cast<double>(static_cast<qsizetype>(requested)) ||
                static_cast<qsizetype>(requested) <= previous)
                return error(message, QStringLiteral("Invalid annotation History insertion"));
            const qsizetype position = static_cast<qsizetype>(requested);
            previous = position;
            const QHash<QUuid, MapAnnotation>::iterator fresh = created.find(id);
            if (fresh == created.end())
                return error(message, QStringLiteral("Annotation History insertion of an unknown member"));
            while (ordered.size() < position)
            {
                if (taken >= kept.size())
                    return error(message, QStringLiteral("Annotation History insertion beyond the layer"));
                ordered.append(std::move(kept[taken++]));
            }
            ordered.append(std::move(fresh.value()));
            created.erase(fresh);
        }
        while (taken < kept.size()) ordered.append(std::move(kept[taken++]));
    }
    if (ordered.size() != total ||
        orderDigest(layer_id, order_changed ? &ordered : nullptr) != to)
        return error(message, QStringLiteral("Annotation History destination digest mismatch"));
    destination = std::move(ordered);
    return true;
}
}
