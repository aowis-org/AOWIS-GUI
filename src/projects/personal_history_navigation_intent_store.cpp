#include "projects/personal_history_navigation_intent_store.h"

#include "projects/dvmdb_identity.h"
#include "projects/hydraulic_history_delta.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/serialization/model_codec.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QUuid>

#include <optional>

#if defined(Q_OS_UNIX) && !defined(Q_OS_WASM)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
constexpr qint64 max_intent_size = 32 * 1024 * 1024;
constexpr qsizetype max_network_bytes = 8 * 1024 * 1024;
constexpr qsizetype max_journal_bytes = 8 * 1024 * 1024;

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool decimal(const QString &value)
{
    if (value.isEmpty() || (value.size() > 1 && value.at(0) == QLatin1Char('0')))
        return false;
    for (const QChar digit : value)
        if (digit < QLatin1Char('0') || digit > QLatin1Char('9')) return false;
    bool valid = false;
    value.toULongLong(&valid);
    return valid;
}

bool syncDirectory(const QString &path, QString *error)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_WASM)
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return fail(error, QStringLiteral("Cannot open History intent directory"));
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) return fail(error, QStringLiteral("Cannot sync History intent directory"));
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
#endif
    return true;
}

QByteArray sha256(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool valid(const PersonalHistoryNavigationIntent &intent)
{
    if (!DvmdbIdentity::isUuidV7(intent.repository_id) ||
        !DvmdbIdentity::isUuidV7(intent.draft_id) ||
        !DvmdbIdentity::isUuidV7(intent.project_id) ||
        !DvmdbIdentity::isUuidV7(intent.network_id) ||
        !DvmdbIdentity::isUuidV7(intent.session_id) ||
        !DvmdbIdentity::isUuidV7(intent.operation_id) ||
        (!intent.base_revision.isEmpty() && !DvmdbIdentity::isUuidV7(intent.base_revision)) ||
        !decimal(intent.source_generation) ||
        intent.source_cursor.isEmpty() || intent.target_coordinate.isEmpty() ||
        intent.source_network.uuid.toString(QUuid::WithoutBraces) != intent.network_id ||
        intent.journal_events.size() > 4096 ||
        HydraulicHistoryDelta::digest(intent.source_network).isEmpty())
        return false;
    const QString kind = intent.target_coordinate.value(QStringLiteral("selectionKind")).toString();
    const QString position = intent.target_coordinate.value(QStringLiteral("actionPosition")).toString();
    bool positionValid = false;
    const qulonglong actionPosition = position.toULongLong(&positionValid);
    if (!positionValid || !decimal(position) || actionPosition > 4096 ||
        !((kind == QStringLiteral("draft") &&
           intent.target_coordinate.value(QStringLiteral("revisionId")).isNull()) ||
          (kind == QStringLiteral("archived") &&
           DvmdbIdentity::isUuidV7(intent.target_coordinate.value(
               QStringLiteral("revisionId")).toString()))))
        return false;
    qsizetype journalBytes = 0;
    for (const QByteArray &event : intent.journal_events)
    {
        if (event.isEmpty() || event.size() > max_journal_bytes - journalBytes ||
            !aowis::model::ModelEditTransactionCodec::decode(event).has_value())
            return false;
        journalBytes += event.size();
    }
    const QByteArray network = aowis::model::ModelCodec<NetworkHydraulic>::encode(intent.source_network);
    return !network.isEmpty() && network.size() <= max_network_bytes;
}

QByteArray encode(const PersonalHistoryNavigationIntent &intent)
{
    const QByteArray network = aowis::model::ModelCodec<NetworkHydraulic>::encode(intent.source_network);
    QJsonArray events;
    for (const QByteArray &event : intent.journal_events)
        events.append(QString::fromLatin1(event.toBase64()));
    QJsonObject payload{
        {QStringLiteral("version"), 1},
        {QStringLiteral("repositoryId"), intent.repository_id},
        {QStringLiteral("draftId"), intent.draft_id},
        {QStringLiteral("projectId"), intent.project_id},
        {QStringLiteral("networkId"), intent.network_id},
        {QStringLiteral("sessionId"), intent.session_id},
        {QStringLiteral("baseRevision"), intent.base_revision.isEmpty()
            ? QJsonValue(QJsonValue::Null) : QJsonValue(intent.base_revision)},
        {QStringLiteral("operationId"), intent.operation_id},
        {QStringLiteral("sourceGeneration"), intent.source_generation},
        {QStringLiteral("sourceCursor"), intent.source_cursor},
        {QStringLiteral("targetCoordinate"), intent.target_coordinate},
        {QStringLiteral("sourceSha256"), HydraulicHistoryDelta::digest(intent.source_network)},
        {QStringLiteral("sourceNetworkBase64"), QString::fromLatin1(network.toBase64())},
        {QStringLiteral("journalEvents"), events}};
    const QByteArray contents = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    return QJsonDocument(QJsonObject{
        {QStringLiteral("payload"), payload},
        {QStringLiteral("payloadSha256"), QString::fromLatin1(sha256(contents))}})
        .toJson(QJsonDocument::Compact);
}

bool decode(const QByteArray &bytes, PersonalHistoryNavigationIntent &intent, QString *error)
{
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("Invalid durable History intent JSON"));
    const QJsonObject envelope = document.object();
    if (envelope.size() != 2 || !envelope.value(QStringLiteral("payload")).isObject())
        return fail(error, QStringLiteral("Invalid durable History intent envelope"));
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (envelope.value(QStringLiteral("payloadSha256")).toString() !=
        QString::fromLatin1(sha256(QJsonDocument(payload).toJson(QJsonDocument::Compact))) ||
        payload.value(QStringLiteral("version")).toInt() != 1)
        return fail(error, QStringLiteral("Durable History intent checksum mismatch"));
    const QString encoded = payload.value(QStringLiteral("sourceNetworkBase64")).toString();
    if (encoded.isEmpty() || encoded.size() > (max_network_bytes + 2) / 3 * 4 + 4)
        return fail(error, QStringLiteral("Durable History source snapshot is oversized"));
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > max_network_bytes ||
        decoded.decoded.toBase64() != encoded.toLatin1())
        return fail(error, QStringLiteral("Invalid durable History source snapshot"));
    QString codecError;
    const std::optional<NetworkHydraulic> network =
        aowis::model::ModelCodec<NetworkHydraulic>::decode(decoded.decoded, &codecError);
    if (!network || aowis::model::ModelCodec<NetworkHydraulic>::encode(*network) != decoded.decoded ||
        HydraulicHistoryDelta::digest(*network) != payload.value(QStringLiteral("sourceSha256")).toString())
        return fail(error, QStringLiteral("Invalid or noncanonical durable History MODEL snapshot"));
    if (!payload.value(QStringLiteral("journalEvents")).isArray())
        return fail(error, QStringLiteral("Missing durable History journal events"));
    const QJsonArray events = payload.value(QStringLiteral("journalEvents")).toArray();
    if (events.size() > 4096)
        return fail(error, QStringLiteral("Oversized durable History journal"));
    QList<QByteArray> frames;
    qsizetype total = 0;
    for (const QJsonValue &item : events)
    {
        if (!item.isString())
            return fail(error, QStringLiteral("Invalid durable History journal frame"));
        const QByteArray text = item.toString().toLatin1();
        if (text.isEmpty() || text.size() > (max_journal_bytes + 2) / 3 * 4 + 4)
            return fail(error, QStringLiteral("Oversized durable History journal frame"));
        const QByteArray::FromBase64Result frame = QByteArray::fromBase64Encoding(
            text, QByteArray::AbortOnBase64DecodingErrors);
        if (!frame || frame.decoded.isEmpty() || frame.decoded.toBase64() != text ||
            frame.decoded.size() > max_journal_bytes - total)
            return fail(error, QStringLiteral("Noncanonical durable History journal frame"));
        total += frame.decoded.size();
        frames.append(frame.decoded);
    }
    intent.repository_id = payload.value(QStringLiteral("repositoryId")).toString();
    intent.draft_id = payload.value(QStringLiteral("draftId")).toString();
    intent.project_id = payload.value(QStringLiteral("projectId")).toString();
    intent.network_id = payload.value(QStringLiteral("networkId")).toString();
    intent.session_id = payload.value(QStringLiteral("sessionId")).toString();
    intent.base_revision = payload.value(QStringLiteral("baseRevision")).toString();
    intent.operation_id = payload.value(QStringLiteral("operationId")).toString();
    intent.source_generation = payload.value(QStringLiteral("sourceGeneration")).toString();
    intent.source_cursor = payload.value(QStringLiteral("sourceCursor")).toObject();
    intent.target_coordinate = payload.value(QStringLiteral("targetCoordinate")).toObject();
    intent.source_network = *network;
    intent.journal_events = frames;
    return valid(intent) || fail(error, QStringLiteral("Invalid durable History navigation identity"));
}
}

PersonalHistoryNavigationIntentStore::PersonalHistoryNavigationIntentStore(const QString &queue_root)
    : root_(queue_root)
{
}

QString PersonalHistoryNavigationIntentStore::path(
    const QString &repository_id, const QString &draft_id) const
{
    return QDir(this->root_).filePath(repository_id + QLatin1Char('/') +
                                      draft_id + QStringLiteral(".history-intent.json"));
}

PersonalHistoryNavigationIntentStore::ReadResult PersonalHistoryNavigationIntentStore::read(
    const QString &repository_id, const QString &draft_id,
    PersonalHistoryNavigationIntent &intent, QString *error) const
{
    if (error) error->clear();
    if (this->root_.isEmpty() || !DvmdbIdentity::isUuidV7(repository_id) ||
        !DvmdbIdentity::isUuidV7(draft_id))
    {
        fail(error, QStringLiteral("Invalid durable History intent scope"));
        return ReadResult::Error;
    }
    const QString filePath = path(repository_id, draft_id);
    const QFileInfo info(filePath);
    if (!info.exists() && !info.isSymLink()) return ReadResult::Missing;
    if (info.isSymLink() || !info.isFile() || info.size() > max_intent_size)
    {
        fail(error, QStringLiteral("Invalid durable History intent file"));
        return ReadResult::Error;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) ||
        !decode(file.read(max_intent_size + 1), intent, error) ||
        intent.repository_id != repository_id || intent.draft_id != draft_id)
    {
        if (error && error->isEmpty())
            *error = QStringLiteral("Durable History intent scope mismatch");
        return ReadResult::Error;
    }
    return ReadResult::Found;
}

bool PersonalHistoryNavigationIntentStore::create(
    const PersonalHistoryNavigationIntent &intent, QString *error) const
{
    if (error) error->clear();
    if (this->root_.isEmpty() || !valid(intent))
        return fail(error, QStringLiteral("Invalid durable History navigation intent"));
    const QString filePath = path(intent.repository_id, intent.draft_id);
    const QString directory = QFileInfo(filePath).absolutePath();
    if (!QDir().mkpath(directory) || QFileInfo::exists(filePath) ||
        QFileInfo(filePath).isSymLink())
        return fail(error, QStringLiteral("History intent already exists or its directory is unavailable"));
    const QByteArray bytes = encode(intent);
    if (bytes.size() > max_intent_size)
        return fail(error, QStringLiteral("History recovery snapshot exceeds 32 MiB"));
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("Cannot write durable History navigation intent"));
#if defined(Q_OS_UNIX) && !defined(Q_OS_WASM)
    QFile persisted(filePath);
    if (!persisted.open(QIODevice::ReadOnly) || ::fsync(persisted.handle()) != 0)
        return fail(error, QStringLiteral("Cannot sync durable History navigation intent"));
#endif
    return syncDirectory(directory, error);
}

bool PersonalHistoryNavigationIntentStore::clear(
    const QString &repository_id, const QString &draft_id,
    const QString &operation_id, QString *error) const
{
    if (error) error->clear();
    PersonalHistoryNavigationIntent stored;
    const ReadResult result = read(repository_id, draft_id, stored, error);
    if (result != ReadResult::Found || stored.operation_id != operation_id)
        return fail(error, QStringLiteral("Cannot clear a missing or different History operation"));
    const QString filePath = path(repository_id, draft_id);
    if (!QFile::remove(filePath))
        return fail(error, QStringLiteral("Cannot remove acknowledged History intent"));
    return syncDirectory(QFileInfo(filePath).absolutePath(), error);
}
