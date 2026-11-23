#pragma once

#include "projects/hydraulic_draft_entity_registry.h"

#include <aowis/model/edit/hydraulic_distribution_root_edit.h>
#include <aowis/model/serialization/model_codec.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// A transport-only delta, not another MODEL journal transaction. DVMDB has
// already published and verified the destination before sending it. Apply it
// to a COPY of the currently verified snapshot; neither the stage nor the
// live editor is mutated until every before-image, membership move, and final
// canonical digest has been checked.
//
// `digest()` is the complete-network SHA-256 (reconcile receipt, navigation
// intents). The delta itself (version 2) uses `orderDigest()`, which needs
// no whole-network encoding.
namespace HydraulicHistoryDelta
{
inline bool error(QString *message, const QString &detail)
{
    if (message) *message = detail;
    return false;
}

inline QString digest(const NetworkHydraulicDistribution &network)
{
    const QByteArray canonical = aowis::model::ModelCodec<NetworkHydraulicDistribution>::encode(network);
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

// Canonical position of a collection type (AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES
// order, which is also the DVMDB collection order); nullopt when unknown.
inline std::optional<int> typeOrdinal(const QString &type)
{
#define HISTORY_TYPE_ORDINAL(name, entity_type, storage_kind, ordinal) \
    if (type == QStringLiteral(#entity_type)) return ordinal;
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(HISTORY_TYPE_ORDINAL)
#undef HISTORY_TYPE_ORDINAL
    return std::nullopt;
}

// Member UUIDs of one collection, in collection order.
inline std::optional<QList<QUuid>> collectionOrder(const NetworkHydraulicDistribution &network,
                                                   const QString &type)
{
#define HISTORY_COLLECTION_ORDER(name, entity_type, storage_kind, ordinal) \
    if (type == QStringLiteral(#entity_type)) \
    { \
        QList<QUuid> members; \
        members.reserve(network.name.size()); \
        for (qsizetype index = 0; index < network.name.size(); ++index) \
            members.append(network.name.at(index).uuid); \
        return members; \
    }
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(HISTORY_COLLECTION_ORDER)
#undef HISTORY_COLLECTION_ORDER
    return std::nullopt;
}

// historyDelta version 2 digest (docs/history-transport.md in DVMDB):
// SHA-256 of "AOWIS-HISTORY-DELTA-2" + u32be-length root settings image +
// for each listed type: u32be-length UTF-8 name, u32be count, 16-byte RFC
// 4122 UUIDs in collection order. Empty for an unknown type. It covers only
// settings and member order; changed content is checked via before-images.
inline QString orderDigest(const NetworkHydraulicDistribution &network,
                           const QStringList &ordered_types)
{
    QByteArray bytes("AOWIS-HISTORY-DELTA-2");
    const std::function<void(quint32)> u32 = [&bytes](quint32 value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.append(static_cast<char>((value >> shift) & 0xffU));
    };
    const QByteArray settings = aowis::model::hydraulicDistributionRootImage(network);
    u32(static_cast<quint32>(settings.size()));
    bytes.append(settings);
    for (const QString &type : ordered_types)
    {
        const std::optional<QList<QUuid>> members = collectionOrder(network, type);
        if (!members) return {};
        const QByteArray name = type.toUtf8();
        u32(static_cast<quint32>(name.size()));
        bytes.append(name);
        u32(static_cast<quint32>(members->size()));
        for (const QUuid &member : *members) bytes.append(member.toRfc4122());
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

struct EntityEdit
{
    QString type;
    QUuid id;
    std::optional<QByteArray> before;
    std::optional<QByteArray> after;
};

// Applies all edits of ONE collection with a UUID lookup table: updates in
// place, then deletions, then insertions appended in edit order. Same result
// as applying them one by one, without a linear search per edit.
template<typename T>
inline bool applyEntities(QList<T> &collection, const std::vector<const EntityEdit *> &edits,
                          QString *message)
{
    if (edits.empty()) return true;
    QHash<QUuid, qsizetype> index;
    QSet<QUuid> duplicated;
    index.reserve(collection.size());
    for (qsizetype position = 0; position < collection.size(); ++position)
    {
        const QUuid &id = collection.at(position).uuid;
        if (index.contains(id)) duplicated.insert(id);
        else index.insert(id, position);
    }
    QSet<qsizetype> removed;
    QList<T> inserted;
    for (const EntityEdit *edit : edits)
    {
        if (duplicated.contains(edit->id))
            return error(message, QStringLiteral("Duplicate History entity UUID"));
        const qsizetype found = index.value(edit->id, -1);
        if ((found < 0) != !edit->before.has_value())
            return error(message, QStringLiteral("History membership before-image mismatch"));
        if (found >= 0 && aowis::model::ModelCodec<T>::encode(collection.at(found)) != *edit->before)
            return error(message, QStringLiteral("Stale History entity before-image"));
        if (!edit->after)
        {
            removed.insert(found);
            continue;
        }
        QString codecError;
        const std::optional<T> value = aowis::model::ModelCodec<T>::decode(*edit->after, &codecError);
        if (!value || value->uuid != edit->id ||
            aowis::model::ModelCodec<T>::encode(*value) != *edit->after)
            return error(message, QStringLiteral("Noncanonical History entity after-image: ") + codecError);
        if (found < 0) inserted.append(*value);
        else collection[found] = *value;
    }
    if (!removed.isEmpty())
    {
        QList<T> kept;
        kept.reserve(collection.size() - removed.size());
        for (qsizetype position = 0; position < collection.size(); ++position)
            if (!removed.contains(position)) kept.append(std::move(collection[position]));
        collection = std::move(kept);
    }
    collection.append(inserted);
    return true;
}

struct MembershipMove
{
    QUuid id;
    qsizetype position = 0;
};

// Applies the moves of ONE collection in one pass (D5). DVMDB places members
// forward: positions ascend, and each move takes a member that no earlier
// position holds yet. For such a list the result equals applying the moves
// one by one; any other list is refused. A moved member is fetched through a
// UUID table instead of a linear search per move.
template<typename T>
inline bool applyMoves(QList<T> &collection, const std::vector<MembershipMove> &moves,
                       QString *message)
{
    if (moves.empty()) return true;
    const qsizetype count = collection.size();
    QHash<QUuid, qsizetype> location;
    location.reserve(count);
    for (qsizetype index = 0; index < count; ++index)
    {
        if (location.contains(collection.at(index).uuid))
            return error(message, QStringLiteral("Duplicate History move entity UUID"));
        location.insert(collection.at(index).uuid, index);
    }
    QHash<QUuid, qsizetype> order; // moved member -> index of its move
    order.reserve(static_cast<qsizetype>(moves.size()));
    qsizetype previous = -1;
    for (std::size_t index = 0; index < moves.size(); ++index)
    {
        const MembershipMove &move = moves[index];
        if (move.position <= previous || move.position >= count)
            return error(message, QStringLiteral("Invalid History membership position"));
        if (!location.contains(move.id))
            return error(message, QStringLiteral("History move refers to missing entity"));
        if (order.contains(move.id))
            return error(message, QStringLiteral("Duplicate History move entity UUID"));
        order.insert(move.id, static_cast<qsizetype>(index));
        previous = move.position;
    }
    QList<T> result;
    result.reserve(count);
    qsizetype next = 0;
    // Fill up to `limit` with members not moved, skipping members already
    // moved (or moved now, `current`). A member whose move comes later sits
    // before its final position: not a forward placement.
    const std::function<bool(qsizetype, qsizetype)> fill =
        [&collection, &order, &result, &next, count](qsizetype limit, qsizetype current)
    {
        while (result.size() < limit)
        {
            while (next < count)
            {
                const qsizetype moved = order.value(collection.at(next).uuid, -1);
                if (moved < 0 || moved > current) break;
                ++next;
            }
            if (next >= count || order.contains(collection.at(next).uuid)) return false;
            result.append(std::move(collection[next]));
            ++next;
        }
        return true;
    };
    for (std::size_t index = 0; index < moves.size(); ++index)
    {
        if (!fill(moves[index].position, static_cast<qsizetype>(index)))
            return error(message, QStringLiteral("History moves are not a forward placement"));
        // Copied: the skip loop above may still read this member's UUID.
        result.append(collection.at(location.value(moves[index].id)));
    }
    if (!fill(count, static_cast<qsizetype>(moves.size())))
        return error(message, QStringLiteral("History moves are not a forward placement"));
    collection = std::move(result);
    return true;
}

inline bool apply(const QJsonObject &delta, const NetworkHydraulicDistribution &source,
                  NetworkHydraulicDistribution &destination, QString *message = nullptr)
{
    if (message) message->clear();
    if (delta.size() != 7 || delta.value(QStringLiteral("version")) != QJsonValue(2) ||
        delta.value(QStringLiteral("networkId")).toString() !=
            source.uuid.toString(QUuid::WithoutBraces) ||
        !delta.value(QStringLiteral("fromSha256")).isString() ||
        !delta.value(QStringLiteral("toSha256")).isString() ||
        !delta.value(QStringLiteral("orderedTypes")).isArray() ||
        !delta.value(QStringLiteral("edits")).isArray() ||
        !delta.value(QStringLiteral("moves")).isArray())
        return error(message, QStringLiteral("Invalid History delta envelope"));

    // Collections whose membership changed: known types, canonical order,
    // no duplicates. Only these may gain, lose or reorder members.
    QStringList orderedTypes;
    int lastOrdinal = -1;
    for (const QJsonValue &value : delta.value(QStringLiteral("orderedTypes")).toArray())
    {
        const std::optional<int> ordinal = value.isString()
            ? typeOrdinal(value.toString()) : std::optional<int>();
        if (!ordinal || *ordinal <= lastOrdinal)
            return error(message, QStringLiteral("Invalid History orderedTypes"));
        lastOrdinal = *ordinal;
        orderedTypes.append(value.toString());
    }
    const QSet<QString> membershipTypes(orderedTypes.cbegin(), orderedTypes.cend());

    const QString from = delta.value(QStringLiteral("fromSha256")).toString();
    const QString to = delta.value(QStringLiteral("toSha256")).toString();
    if (from.size() != 64 || to.size() != 64 || orderDigest(source, orderedTypes) != from)
        return error(message, QStringLiteral("History delta source digest mismatch"));
    const QJsonArray edits = delta.value(QStringLiteral("edits")).toArray();
    const QJsonArray moves = delta.value(QStringLiteral("moves")).toArray();
    // DVMDB holds at most 20,000 changes per draft, so a delta between two
    // drafts has at most 40,000 entity edits plus the root edit. A move
    // targets a final index, which is bounded below.
    if (edits.size() > 40001 || moves.size() > 2000000)
        return error(message, QStringLiteral("History delta exceeds entity limit"));

    NetworkHydraulicDistribution next = source;
    QSet<QString> seenEdits;
    std::vector<EntityEdit> parsed;
    parsed.reserve(static_cast<std::size_t>(edits.size()));
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
        if (type == QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType))
        {
            if (id != next.uuid || !before || !after ||
                aowis::model::hydraulicDistributionRootImage(next) != *before)
                return error(message, QStringLiteral("History root before-image mismatch"));
            QString rootError;
            const std::optional<NetworkHydraulicDistribution> metadata =
                aowis::model::decodeHydraulicDistributionRootImage(*after, &rootError);
            if (!metadata || aowis::model::hydraulicDistributionRootImage(*metadata) != *after ||
                !aowis::model::applyHydraulicDistributionRootMetadata(next, *metadata))
                return error(message, QStringLiteral("Invalid History root after-image: ") + rootError);
            continue;
        }
        if (!typeOrdinal(type))
            return error(message, QStringLiteral("Unknown History entity type: ") + type);
        if ((!before || !after) && !membershipTypes.contains(type))
            return error(message, QStringLiteral("History membership change outside orderedTypes"));
        parsed.push_back(EntityEdit{type, id, std::move(before), std::move(after)});
    }
#define APPLY_HISTORY_ENTITIES(name, entity_type, storage_kind, ordinal) \
    { \
        std::vector<const EntityEdit *> collectionEdits; \
        for (const EntityEdit &edit : parsed) \
            if (edit.type == QStringLiteral(#entity_type)) collectionEdits.push_back(&edit); \
        if (!applyEntities(next.name, collectionEdits, message)) return false; \
    }
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(APPLY_HISTORY_ENTITIES)
#undef APPLY_HISTORY_ENTITIES
    std::map<QString, std::vector<MembershipMove>> movesByType;
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
        if (!membershipTypes.contains(type))
            return error(message, QStringLiteral("History move outside orderedTypes"));
        if (!typeOrdinal(type))
            return error(message, QStringLiteral("Unknown History membership kind"));
        const double requested = move.value(QStringLiteral("position")).toDouble(-1);
        if (requested < 0 || requested > 2000000 ||
            requested != static_cast<int>(requested))
            return error(message, QStringLiteral("Invalid History membership index"));
        movesByType[type].push_back(MembershipMove{id, static_cast<qsizetype>(requested)});
    }
    // Collections are independent, so each one's moves apply in one pass.
#define MOVE_HISTORY_ENTITIES(name, entity_type, storage_kind, ordinal) \
    { \
        const std::map<QString, std::vector<MembershipMove>>::const_iterator found = \
            movesByType.find(QStringLiteral(#entity_type)); \
        if (found != movesByType.end() && !applyMoves(next.name, found->second, message)) \
            return false; \
    }
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(MOVE_HISTORY_ENTITIES)
#undef MOVE_HISTORY_ENTITIES
    if (orderDigest(next, orderedTypes) != to)
        return error(message, QStringLiteral("History destination digest mismatch"));
    destination = std::move(next);
    return true;
}
}
