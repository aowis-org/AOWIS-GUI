#pragma once

#include "projects/hydraulic_draft_entity_registry.h"

#include <aowis/model/edit/hydraulic_root_edit.h>
#include <aowis/model/serialization/model_codec.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <optional>
#include <utility>

// A transport-only delta, not another MODEL journal transaction. DVMDB has
// already published and verified the destination before sending it. Apply it
// to a COPY of the currently verified snapshot; neither the stage nor the
// live editor is mutated until every before-image, membership move, and final
// canonical digest has been checked.
namespace HydraulicHistoryDelta
{
inline bool error(QString *message, const QString &detail)
{
    if (message) *message = detail;
    return false;
}

inline QString digest(const NetworkHydraulic &network)
{
    const QByteArray canonical = aowis::model::ModelCodec<NetworkHydraulic>::encode(network);
    if (canonical.isEmpty()) return {};
    return QString::fromLatin1(QCryptographicHash::hash(
        canonical, QCryptographicHash::Sha256).toHex());
}

inline bool decodeImage(const QJsonValue &value, std::optional<QByteArray> &decoded,
                        QString *message)
{
    decoded.reset();
    if (value.isNull()) return true;
    if (!value.isString()) return error(message, QStringLiteral("Invalid History image field"));
    const QByteArray text = value.toString().toLatin1();
    if (text.isEmpty() || text.size() > 16 * 1024 * 1024)
        return error(message, QStringLiteral("Invalid History image size"));
    const QByteArray::FromBase64Result result = QByteArray::fromBase64Encoding(
        text, QByteArray::AbortOnBase64DecodingErrors);
    if (!result || result.decoded.isEmpty() || result.decoded.toBase64() != text)
        return error(message, QStringLiteral("Noncanonical History base64 image"));
    decoded = result.decoded;
    return true;
}

template<typename T>
inline bool applyEntity(QList<T> &collection, const QUuid &id,
                        const std::optional<QByteArray> &before,
                        const std::optional<QByteArray> &after, QString *message)
{
    qsizetype found = -1;
    for (qsizetype index = 0; index < collection.size(); ++index)
    {
        if (collection.at(index).uuid != id) continue;
        if (found != -1)
            return error(message, QStringLiteral("Duplicate History entity UUID"));
        found = index;
    }
    if ((found < 0) != !before.has_value())
        return error(message, QStringLiteral("History membership before-image mismatch"));
    if (found >= 0 && aowis::model::ModelCodec<T>::encode(collection.at(found)) != *before)
        return error(message, QStringLiteral("Stale History entity before-image"));
    if (!after)
    {
        if (found < 0)
            return error(message, QStringLiteral("History cannot remove missing entity"));
        collection.removeAt(found);
        return true;
    }
    QString codecError;
    const std::optional<T> value = aowis::model::ModelCodec<T>::decode(*after, &codecError);
    if (!value || value->uuid != id ||
        aowis::model::ModelCodec<T>::encode(*value) != *after)
        return error(message, QStringLiteral("Noncanonical History entity after-image: ") + codecError);
    if (found < 0) collection.append(*value);
    else collection[found] = *value;
    return true;
}

template<typename T>
inline bool moveEntity(QList<T> &collection, const QUuid &id,
                       qsizetype position, QString *message)
{
    if (position < 0 || position >= collection.size())
        return error(message, QStringLiteral("Invalid History membership position"));
    qsizetype from = -1;
    for (qsizetype index = 0; index < collection.size(); ++index)
    {
        if (collection.at(index).uuid != id) continue;
        if (from != -1)
            return error(message, QStringLiteral("Duplicate History move entity UUID"));
        from = index;
    }
    if (from < 0)
        return error(message, QStringLiteral("History move refers to missing entity"));
    collection.move(from, position);
    return true;
}

inline bool apply(const QJsonObject &delta, const NetworkHydraulic &source,
                  NetworkHydraulic &destination, QString *message = nullptr)
{
    if (message) message->clear();
    if (delta.size() != 6 || delta.value(QStringLiteral("version")) != QJsonValue(1) ||
        delta.value(QStringLiteral("networkId")).toString() !=
            source.uuid.toString(QUuid::WithoutBraces) ||
        !delta.value(QStringLiteral("fromSha256")).isString() ||
        !delta.value(QStringLiteral("toSha256")).isString() ||
        !delta.value(QStringLiteral("edits")).isArray() ||
        !delta.value(QStringLiteral("moves")).isArray())
        return error(message, QStringLiteral("Invalid History delta envelope"));
    const QString from = delta.value(QStringLiteral("fromSha256")).toString();
    const QString to = delta.value(QStringLiteral("toSha256")).toString();
    if (from.size() != 64 || to.size() != 64 || digest(source) != from)
        return error(message, QStringLiteral("History delta source digest mismatch"));
    const QJsonArray edits = delta.value(QStringLiteral("edits")).toArray();
    const QJsonArray moves = delta.value(QStringLiteral("moves")).toArray();
    if (edits.size() > 20000 || moves.size() > 20000)
        return error(message, QStringLiteral("History delta exceeds entity limit"));

    NetworkHydraulic next = source;
    QSet<QString> seenEdits;
    for (const QJsonValue &value : edits)
    {
        if (!value.isObject())
            return error(message, QStringLiteral("History edit is not an object"));
        const QJsonObject edit = value.toObject();
        if (edit.size() != 4 || !edit.value(QStringLiteral("type")).isString() ||
            !edit.value(QStringLiteral("id")).isString() ||
            !edit.contains(QStringLiteral("beforeBase64")) ||
            !edit.contains(QStringLiteral("afterBase64")))
            return error(message, QStringLiteral("Malformed History entity edit"));
        const QString type = edit.value(QStringLiteral("type")).toString();
        const QString idText = edit.value(QStringLiteral("id")).toString();
        const QUuid id(idText);
        const QString typedId = type + QLatin1Char(':') + idText;
        if (id.isNull() || id.toString(QUuid::WithoutBraces) != idText ||
            seenEdits.contains(typedId))
            return error(message, QStringLiteral("Invalid or duplicate History entity identity"));
        seenEdits.insert(typedId);
        std::optional<QByteArray> before;
        std::optional<QByteArray> after;
        if (!decodeImage(edit.value(QStringLiteral("beforeBase64")), before, message) ||
            !decodeImage(edit.value(QStringLiteral("afterBase64")), after, message) ||
            (!before && !after))
            return error(message, QStringLiteral("History entity images are invalid"));
        if (type == QString::fromLatin1(aowis::model::hydraulicRootEditType))
        {
            if (id != next.uuid || !before || !after ||
                aowis::model::hydraulicRootImage(next) != *before)
                return error(message, QStringLiteral("History root before-image mismatch"));
            QString rootError;
            const std::optional<NetworkHydraulic> metadata =
                aowis::model::decodeHydraulicRootImage(*after, &rootError);
            if (!metadata || aowis::model::hydraulicRootImage(*metadata) != *after ||
                !aowis::model::applyHydraulicRootMetadata(next, *metadata))
                return error(message, QStringLiteral("Invalid History root after-image: ") + rootError);
            continue;
        }
        bool recognized = false;
#define APPLY_HISTORY_ENTITY(name, entity_type, storage_kind, ordinal) \
        if (!recognized && type == QStringLiteral(#entity_type)) { \
            recognized = true; \
            if (!applyEntity(next.name, id, before, after, message)) return false; \
        }
        AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(APPLY_HISTORY_ENTITY)
#undef APPLY_HISTORY_ENTITY
        if (!recognized)
            return error(message, QStringLiteral("Unknown History entity type: ") + type);
    }
    for (const QJsonValue &value : moves)
    {
        if (!value.isObject())
            return error(message, QStringLiteral("History move is not an object"));
        const QJsonObject move = value.toObject();
        if (move.size() != 3 || !move.value(QStringLiteral("type")).isString() ||
            !move.value(QStringLiteral("id")).isString() ||
            !move.value(QStringLiteral("position")).isDouble())
            return error(message, QStringLiteral("Malformed History membership move"));
        const QString type = move.value(QStringLiteral("type")).toString();
        const QString idText = move.value(QStringLiteral("id")).toString();
        const QUuid id(idText);
        if (id.isNull() || id.toString(QUuid::WithoutBraces) != idText)
            return error(message, QStringLiteral("Invalid History membership UUID"));
        const double requested = move.value(QStringLiteral("position")).toDouble(-1);
        if (requested < 0 || requested > 2000000 ||
            requested != static_cast<int>(requested))
            return error(message, QStringLiteral("Invalid History membership index"));
        bool recognized = false;
#define MOVE_HISTORY_ENTITY(name, entity_type, storage_kind, ordinal) \
        if (!recognized && type == QStringLiteral(#entity_type)) { \
            recognized = true; \
            if (!moveEntity(next.name, id, static_cast<qsizetype>(requested), message)) return false; \
        }
        AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(MOVE_HISTORY_ENTITY)
#undef MOVE_HISTORY_ENTITY
        if (!recognized)
            return error(message, QStringLiteral("Unknown History membership kind"));
    }
    if (digest(next) != to)
        return error(message, QStringLiteral("History destination digest mismatch"));
    destination = std::move(next);
    return true;
}
}
