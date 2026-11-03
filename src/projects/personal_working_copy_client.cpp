#include "projects/model_codec_compatibility.h"
#include "projects/personal_working_copy_client.h"
#include "projects/dvmdb_identity.h"
#include "projects/personal_edit_queue.h"
#include "projects/personal_commit_intent_store.h"
#include "config/gui_configuration.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_root_edit.h>
#include <aowis/model/serialization/model_codec.h>

#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <limits>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QUrl>

#include <memory>
#include <utility>

namespace
{
QString endpointRoot(const QString &endpoint)
{
    QString url = endpoint;
    while (url.endsWith(QLatin1Char('/'))) url.chop(1);
    if (!url.endsWith(QStringLiteral("/dvmdb/v1"))) url += QStringLiteral("/dvmdb/v1");
    return url;
}

QString responseError(QNetworkReply *reply, const QByteArray &bytes)
{
    // DVMDB reports failures as {"error":{"code":"...","message":"..."}}.
    // Prefer its message (it names the rejected precondition) over Qt's
    // generic transport text; fall back to the latter for non-DVMDB replies.
    const QJsonObject error = QJsonDocument::fromJson(bytes).object()
        .value(QStringLiteral("error")).toObject();
    const QString message = error.value(QStringLiteral("message")).toString();
    const QString code = error.value(QStringLiteral("code")).toString();
    if (!message.isEmpty())
        return code.isEmpty() ? message : QStringLiteral("%1 (%2)").arg(message, code);
    return reply->errorString();
}

bool modelKeyUuid(const QString &value)
{
    // Model keys are canonical 16-byte UUIDs; older imported Model data may
    // have UUIDs other than v7, which the native DVMDB model API preserves.
    if (value.size() != 36) return false;
    const QUuid parsed(value);
    return !parsed.isNull() && parsed.toString(QUuid::WithoutBraces) == value;
}

bool decimal(const QString &value)
{
    if (value.isEmpty() || value.size() > 20) return false;
    for (const QChar digit : value)
        if (digit < QLatin1Char('0') || digit > QLatin1Char('9')) return false;
    return true;
}

// Commit authorship must name the same person on every Commit. Until the
// security concept defines user identities, this installation keeps one
// UUIDv7, created on first use, and the operating-system account name.
QSettings *openAuthorSettings()
{
#ifdef __EMSCRIPTEN__
    // Browser-persistent storage; the WASM ini path is a read-only resource.
    return new QSettings(QStringLiteral("AOWIS"), QStringLiteral("AOWIS-SERVER-GUI"));
#else
    return new QSettings(guiConfigurationFilePath(), QSettings::IniFormat);
#endif
}

QString commitAuthorId()
{
    const std::unique_ptr<QSettings> settings(openAuthorSettings());
    const QString key = QStringLiteral("dvmdb/commit_author_id");
    QString id = settings->value(key).toString();
    if (!DvmdbIdentity::isUuidV7(id))
    {
        id = DvmdbIdentity::newUuidV7();
        settings->setValue(key, id);
        settings->sync();
    }
    return id;
}

QString commitAuthorName()
{
    QString name = qEnvironmentVariable("USER").trimmed();
    if (name.isEmpty())
        name = qEnvironmentVariable("USERNAME").trimmed();
    // DVMDB accepts 1..200 bytes of UTF-8 without control characters.
    bool printable = true;
    for (const QChar character : name)
        if (character.category() == QChar::Other_Control) printable = false;
    if (name.isEmpty() || name.toUtf8().size() > 128 || !printable)
        name = QStringLiteral("AOWIS user");
    return name;
}

QJsonArray encodeChanges(const QList<PersonalWorkingCopyChange> &changes)
{
    QJsonArray batch;
    for (const PersonalWorkingCopyChange &change : changes)
    {
        if (change.deleted)
        {
            batch.append(QJsonObject{{QStringLiteral("uuid"), change.uuid},
                                     {QStringLiteral("delete"), true}});
            continue;
        }
        QJsonArray references;
        for (const QString &reference : change.references)
            references.append(reference);
        batch.append(QJsonObject{{QStringLiteral("uuid"), change.uuid},
                                 {QStringLiteral("kind"), static_cast<int>(change.kind)},
                                 {QStringLiteral("references"), references},
                                 {QStringLiteral("payloadBase64"),
                                  QString::fromLatin1(change.payload.toBase64())}});
    }
    return batch;
}

QJsonArray encodeMembership(const QList<HydraulicDraftMembershipEdit> &membership)
{
    QJsonArray batch;
    for (const HydraulicDraftMembershipEdit &intent : membership)
        batch.append(QJsonObject{
            {QStringLiteral("networkId"), intent.network_id.toString(QUuid::WithoutBraces)},
            {QStringLiteral("entityId"), intent.entity_id.toString(QUuid::WithoutBraces)},
            {QStringLiteral("kind"), static_cast<int>(intent.kind)},
            {QStringLiteral("collectionIndex"), static_cast<int>(intent.collection_index)},
            {QStringLiteral("inserted"), intent.inserted}});
    return batch;
}

// Mirrors DVMDB's /save-events preconditions so that a batch DVMDB would
// reject never leaves the GUI: every created/removed entity in the events has
// exactly one intent with the matching direction, every intent describes such
// an edit, and the typed record agrees (inserted: record with the event's
// after-image; removed: tombstone). Updates need no intent.
QString validateMembership(const QList<aowis::model::ModelEditTransaction> &events,
                           const QList<PersonalWorkingCopyChange> &changes,
                           const QList<HydraulicDraftMembershipEdit> &membership)
{
    if (membership.size() > 128)
        return QStringLiteral("Membership batch exceeds 128 intents.");
    QHash<QUuid, const HydraulicDraftMembershipEdit *> intents;
    for (const HydraulicDraftMembershipEdit &intent : membership)
    {
        if (intent.network_id.isNull() || intent.entity_id.isNull() ||
            intent.entity_id == intent.network_id || intent.kind == 0 ||
            intent.collection_index >= 18 || intents.contains(intent.entity_id))
            return QStringLiteral("Invalid or duplicate hydraulic membership intent.");
        intents.insert(intent.entity_id, &intent);
    }
    QSet<QUuid> described;
    QSet<QString> eventRecords;
    for (const aowis::model::ModelEditTransaction &event : events)
    {
        for (const aowis::model::ModelEntityEdit &edit : event.edits)
        {
            if (edit.entityType == QString::fromLatin1(aowis::model::hydraulicRootEditType))
            {
                if (events.size() != 1 ||
                    !edit.before.has_value() || !edit.after.has_value() ||
                    !aowis::model::decodeHydraulicRootImage(*edit.before) ||
                    !aowis::model::decodeHydraulicRootImage(*edit.after))
                    return QStringLiteral("Invalid network-root edit in event batch.");
                continue;
            }
            // All normal entity edits must have matching native candidates,
            // including updates that do not change collection membership.
            bool recordMatched = false;
            for (const PersonalWorkingCopyChange &record : changes)
                if (record.uuid == edit.entityId.toString(QUuid::WithoutBraces) &&
                    record.deleted == !edit.after.has_value() &&
                    (record.deleted || record.payload == edit.after.value()))
                {
                    recordMatched = true;
                    break;
                }
            if (!recordMatched)
                return QStringLiteral("MODEL event is missing its typed candidate record.");
            eventRecords.insert(edit.entityId.toString(QUuid::WithoutBraces));
            const bool created = !edit.before.has_value() && edit.after.has_value();
            const bool removed = edit.before.has_value() && !edit.after.has_value();
            const HydraulicDraftMembershipEdit *intent = intents.value(edit.entityId, nullptr);
            if (!created && !removed)
            {
                if (intent != nullptr)
                    return QStringLiteral("Membership intent for entity %1 does not describe a "
                        "creation or removal.").arg(edit.entityId.toString(QUuid::WithoutBraces));
                continue;
            }
            if (intent == nullptr || intent->inserted != created)
                return QStringLiteral("Created or removed entity %1 has no matching membership intent.")
                    .arg(edit.entityId.toString(QUuid::WithoutBraces));
            if (described.contains(edit.entityId))
                return QStringLiteral("Entity %1 is created or removed twice in one batch.")
                    .arg(edit.entityId.toString(QUuid::WithoutBraces));
            described.insert(edit.entityId);
            const QString uuid = edit.entityId.toString(QUuid::WithoutBraces);
            bool record_matches = false;
            for (const PersonalWorkingCopyChange &change : changes)
            {
                if (change.uuid != uuid) continue;
                record_matches = created
                    ? !change.deleted && change.kind == intent->kind && change.payload == *edit.after
                    : change.deleted;
                break;
            }
            if (!record_matches)
                return QStringLiteral("Typed record for membership entity %1 is missing or differs "
                                      "from the MODEL event.").arg(uuid);
        }
    }
    if (described.size() != intents.size())
        return QStringLiteral("Membership intent without a matching creation or removal event.");
    // Do not permit a root-only event to carry unrelated candidate model
    // records. DVMDB derives the root and chunk writes from logical events.
    for (const PersonalWorkingCopyChange &change : changes)
        if (!eventRecords.contains(change.uuid))
            return QStringLiteral("Candidate record has no matching MODEL entity edit.");
    return {};
}

bool matchingChanges(const QList<PersonalWorkingCopyChange> &stored,
                     const QList<PersonalWorkingCopyChange> &sent)
{
    for (const PersonalWorkingCopyChange &change : sent)
    {
        bool found = false;
        for (const PersonalWorkingCopyChange &item : stored)
        {
            if (item.uuid != change.uuid) continue;
            if (item.deleted != change.deleted ||
                (!item.deleted && (item.kind != change.kind ||
                 item.references != change.references || item.payload != change.payload)))
                return false;
            found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

bool decimalValue(const QString &value, qulonglong &number)
{
    if (!decimal(value)) return false;
    bool ok = false;
    number = value.toULongLong(&ok);
    return ok;
}

QString validateSaveRequest(const QString &draft_id, const QString &session_id,
                            const QString &fencing_token,
                            const QString &expected_generation,
                            const QList<PersonalWorkingCopyChange> &changes, bool allow_empty = false)
{
    bool parsed = false;
    const qulonglong generation = expected_generation.toULongLong(&parsed);
    if (!DvmdbIdentity::isUuidV7(draft_id) || !DvmdbIdentity::isUuidV7(session_id)
        || !DvmdbIdentity::isUuidV7(fencing_token) || !decimal(expected_generation)
        || !parsed || generation == std::numeric_limits<qulonglong>::max()
        || (!allow_empty && changes.isEmpty()) || changes.size() > 128)
        return QStringLiteral("Invalid draft save identifiers, generation or batch size.");
    QSet<QString> seen;
    for (const PersonalWorkingCopyChange &change : changes)
    {
        if (!modelKeyUuid(change.uuid) || seen.contains(change.uuid)
            || change.references.size() > 65535 || change.payload.size() > 8 * 1024 * 1024)
            return QStringLiteral("Invalid or duplicate model-record UUID in draft Save.");
        seen.insert(change.uuid);
        if (change.deleted) continue;
        for (const QString &reference : change.references)
            if (!modelKeyUuid(reference))
                return QStringLiteral("Invalid model-record reference UUID.");
    }
    return {};
}
}

PersonalWorkingCopyClient::PersonalWorkingCopyClient(QObject *parent)
    : QObject(parent), manager(this)
{
}

QString PersonalWorkingCopyClient::newSessionUuidV7()
{
    return DvmdbIdentity::newUuidV7();
}

bool PersonalWorkingCopyClient::parseDraft(const QJsonObject &object,
                                            PersonalWorkingCopyInfo &draft)
{
    draft.id = object.value(QStringLiteral("id")).toString();
    draft.project_id = object.value(QStringLiteral("projectId")).toString();
    draft.branch_id = object.value(QStringLiteral("branchId")).toString();
    draft.session_id = object.value(QStringLiteral("sessionId")).toString();
    draft.generation = object.value(QStringLiteral("generation")).toString();
    const QJsonValue base = object.value(QStringLiteral("baseRevision"));
    draft.base_revision = base.isNull() ? QString() : base.toString();
    if (!DvmdbIdentity::isUuidV7(draft.id) || !DvmdbIdentity::isUuidV7(draft.project_id)
        || !DvmdbIdentity::isUuidV7(draft.branch_id)
        || !DvmdbIdentity::isUuidV7(draft.session_id)
        || (!base.isNull() && !DvmdbIdentity::isUuidV7(draft.base_revision))
        || !decimal(draft.generation)
        || !object.value(QStringLiteral("dirty")).isBool()) return false;
    draft.dirty = object.value(QStringLiteral("dirty")).toBool();
    draft.changes.clear();
    const QJsonValue candidate = object.value(QStringLiteral("changes"));
    if (!candidate.isUndefined())
    {
        if (!candidate.isArray() || candidate.toArray().size() > 20000) return false;
        QSet<QString> seen;
        for (const QJsonValue &entry : candidate.toArray())
        {
            if (!entry.isObject()) return false;
            const QJsonObject item = entry.toObject();
            PersonalWorkingCopyChange change;
            change.uuid = item.value(QStringLiteral("uuid")).toString();
            if (!modelKeyUuid(change.uuid) || seen.contains(change.uuid)) return false;
            seen.insert(change.uuid);
            if (item.value(QStringLiteral("delete")) == QJsonValue(true))
            {
                if (item.size() != 2) return false;
                change.deleted = true;
            }
            else
            {
                if (item.size() != 4 || !item.value(QStringLiteral("kind")).isDouble()
                    || !item.value(QStringLiteral("references")).isArray()
                    || !item.value(QStringLiteral("payloadBase64")).isString()) return false;
                const double kind = item.value(QStringLiteral("kind")).toDouble(-1);
                if (kind < 0 || kind > 65535 || kind != static_cast<int>(kind)) return false;
                change.kind = static_cast<quint16>(kind);
                const QJsonArray references = item.value(QStringLiteral("references")).toArray();
                if (references.size() > 65535) return false;
                for (const QJsonValue &reference : references)
                {
                    if (!reference.isString() || !modelKeyUuid(reference.toString()))
                        return false;
                    change.references.append(reference.toString());
                }
                const QByteArray encoded = item.value(QStringLiteral("payloadBase64")).toString().toLatin1();
                const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
                    encoded, QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded || decoded.decoded.toBase64() != encoded) return false;
                change.payload = decoded.decoded;
            }
            draft.changes.append(change);
        }
    }
    draft.events.clear();
    draft.event_count = object.value(QStringLiteral("eventCount")).toString();
    if (object.contains(QStringLiteral("eventCount")))
    {
        qulonglong count = 0;
        if (!decimalValue(draft.event_count, count)) return false;
        if (object.contains(QStringLiteral("events")) &&
            static_cast<qulonglong>(object.value(QStringLiteral("events")).toArray().size()) != count)
            return false;
    }
    if (object.contains(QStringLiteral("events")))
    {
        if (!object.value(QStringLiteral("events")).isArray()) return false;
        const QJsonArray journal = object.value(QStringLiteral("events")).toArray();
        if (journal.size() > 10000) return false;
        QSet<QString> identities;
        for (const QJsonValue &value : journal)
        {
            if (!value.isObject()) return false;
            const QJsonObject item = value.toObject();
            if (item.size() != 2 || !item.value(QStringLiteral("id")).isString() ||
                !item.value(QStringLiteral("payloadBase64")).isString()) return false;
            const QString id = item.value(QStringLiteral("id")).toString();
            const QByteArray text = item.value(QStringLiteral("payloadBase64")).toString().toLatin1();
            const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
                text, QByteArray::AbortOnBase64DecodingErrors);
            if (!DvmdbIdentity::isUuidV7(id) || identities.contains(id) || !decoded ||
                decoded.decoded.toBase64() != text) return false;
            QString codec_error;
            const std::optional<aowis::model::ModelEditTransaction> event =
                aowis::model::ModelEditTransactionCodec::decode(decoded.decoded, &codec_error);
            if (!event.has_value() ||
                event->transactionId.toString(QUuid::WithoutBraces) != id)
                return false;
            identities.insert(id);
            draft.events.append({id, decoded.decoded});
        }
    }
    return true;
}

void PersonalWorkingCopyClient::request(const PersonalRepositoryTarget &target,
                                        const QByteArray &method, const QString &path,
                                        const QJsonObject &body, Response completion)
{
    if (target.kind != PersonalRepositoryTarget::Kind::Http &&
        target.kind != PersonalRepositoryTarget::Kind::Embedded)
    {
        completion({}, QStringLiteral("Select a personal database in Projects first."));
        return;
    }
    const PersonalRepositoryProfile profile{target.endpoint, target.repository_id};
    QString invalid;
    if (!validatePersonalRepositoryProfile(profile, &invalid))
    {
        completion({}, invalid);
        return;
    }
    const QString root = endpointRoot(target.endpoint);
    QNetworkRequest verify(QUrl(root + QStringLiteral("/repository/tracking")));
    verify.setTransferTimeout(8000);
    QNetworkReply *identity = this->manager.get(verify);
    connect(identity, &QNetworkReply::finished, this,
            [this, identity, root, target, method, path, body,
             completion = std::move(completion)]() mutable
    {
        const QByteArray bytes = identity->readAll();
        const bool ok = identity->error() == QNetworkReply::NoError
            && identity->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        identity->deleteLater();
        if (!ok)
        {
            completion({}, QStringLiteral("Personal DVMDB identity lookup failed."));
            return;
        }
        const QJsonObject repository = QJsonDocument::fromJson(bytes).object();
        const QString codec_error = ModelCodecCompatibility::error(repository);
        if (!codec_error.isEmpty())
        {
            completion({}, codec_error);
            return;
        }
        if (repository.value(QStringLiteral("repositoryId")).toString() != target.repository_id
            || repository.value(QStringLiteral("role")).toString() != QStringLiteral("private"))
        {
            completion({}, QStringLiteral("Configured repository UUID or private role does not match."));
            return;
        }
        QNetworkRequest call(QUrl(root + path));
        call.setTransferTimeout(10000);
        call.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply *reply = method == "GET"
            ? this->manager.get(call)
            : this->manager.post(call, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this,
                [reply, completion = std::move(completion)]() mutable
        {
            const QByteArray data = reply->readAll();
            const bool success = reply->error() == QNetworkReply::NoError
                && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 200
                && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() < 300;
            const QString error = success ? QString() : responseError(reply, data);
            reply->deleteLater();
            if (!success) { completion({}, error); return; }
            QJsonParseError parsing;
            const QJsonDocument document = QJsonDocument::fromJson(data, &parsing);
            if (parsing.error != QJsonParseError::NoError || !document.isObject())
            {
                completion({}, QStringLiteral("Invalid personal DVMDB response."));
                return;
            }
            completion(document.object(), QString());
        });
    });
}

namespace
{
constexpr int working_copy_page_size = 100;
constexpr int working_copy_max_pages = 1000;
constexpr int working_copy_max_drafts = working_copy_page_size * working_copy_max_pages;
}

void PersonalWorkingCopyClient::list(const PersonalRepositoryTarget &target,
                                     ListCompletion completion)
{
    list(target, {}, std::move(completion));
}

void PersonalWorkingCopyClient::list(const PersonalRepositoryTarget &target,
                                     const QString &project_id, ListCompletion completion)
{
    if (!project_id.isEmpty() && !DvmdbIdentity::isUuidV7(project_id))
    {
        completion({}, QStringLiteral("Invalid working-copy project UUIDv7."));
        return;
    }
    listPage(target, project_id, {}, 0, {}, std::move(completion));
}

void PersonalWorkingCopyClient::listPage(const PersonalRepositoryTarget &target,
                                         const QString &project_id, const QString &after,
                                         int page_count, QList<PersonalWorkingCopyInfo> collected,
                                         ListCompletion completion)
{
    if (page_count >= working_copy_max_pages)
    {
        completion({}, QStringLiteral("Working-copy discovery exceeded the safe page limit."));
        return;
    }
    QString path = QStringLiteral("/working-copies?limit=%1").arg(working_copy_page_size);
    if (!project_id.isEmpty()) path += QStringLiteral("&projectId=") + project_id;
    if (!after.isEmpty()) path += QStringLiteral("&after=") + after;
    request(target, "GET", path, {},
            [this, target, project_id, after, page_count, collected = std::move(collected),
             completion = std::move(completion)](const QJsonObject &body,
                                                  const QString &error) mutable
    {
        if (!error.isEmpty()) { completion({}, error); return; }
        const QJsonValue records = body.value(QStringLiteral("workingCopies"));
        const QJsonValue cursor = body.value(QStringLiteral("nextCursor"));
        if (!records.isArray() || records.toArray().size() > working_copy_page_size ||
            (!cursor.isNull() && !cursor.isString()))
        {
            completion({}, QStringLiteral("Invalid paginated working-copy response."));
            return;
        }
        QString last_id = after;
        for (const QJsonValue &value : records.toArray())
        {
            PersonalWorkingCopyInfo draft;
            if (!value.isObject() || !parseDraft(value.toObject(), draft) ||
                (!project_id.isEmpty() && draft.project_id != project_id) ||
                (!last_id.isEmpty() && draft.id <= last_id))
            {
                completion({}, QStringLiteral("Invalid, repeated, or out-of-order draft metadata."));
                return;
            }
            last_id = draft.id;
            collected.append(std::move(draft));
            if (collected.size() > working_copy_max_drafts)
            {
                completion({}, QStringLiteral("Working-copy discovery exceeds the safe draft limit."));
                return;
            }
        }
        if (cursor.isNull())
        {
            completion(collected, QString());
            return;
        }
        const QString next = cursor.toString();
        // DVMDB's cursor is exactly the final UUID in the previous page.
        // Reject missing progress, a forged offset, and false completion.
        if (records.toArray().isEmpty() || !DvmdbIdentity::isUuidV7(next) ||
            next != last_id || collected.size() == working_copy_max_drafts)
        {
            completion({}, QStringLiteral("DVMDB working-copy cursor did not advance correctly."));
            return;
        }
        // Queue the next request instead of deepening the network callback stack.
        QTimer::singleShot(0, this,
            [this, target, project_id, next, page_count, collected = std::move(collected),
             completion = std::move(completion)]() mutable
        {
            listPage(target, project_id, next, page_count + 1,
                     std::move(collected), std::move(completion));
        });
    });
}

void PersonalWorkingCopyClient::create(const PersonalRepositoryTarget &target,
                                       const QString &project_id, const QString &branch_id,
                                       const QString &base_revision, DraftCompletion completion)
{
    if (!DvmdbIdentity::isUuidV7(project_id) || !DvmdbIdentity::isUuidV7(branch_id)
        || (!base_revision.isEmpty() && !DvmdbIdentity::isUuidV7(base_revision)))
    {
        completion({}, QStringLiteral("Invalid project, branch or checkout revision UUIDv7."));
        return;
    }
    // Draft and session identities are fixed before sending: a resent
    // request (lost response, or Qt < 6.10 replaying the POST) names the
    // same draft, which DVMDB returns instead of creating a second one.
    const QString draft_id = DvmdbIdentity::newUuidV7();
    const QString session_id = newSessionUuidV7();
    request(target, "POST", QStringLiteral("/working-copies"),
            QJsonObject{{QStringLiteral("id"), draft_id},
                        {QStringLiteral("projectId"), project_id},
                        {QStringLiteral("branchId"), branch_id},
                        {QStringLiteral("baseRevision"), base_revision.isEmpty()
                            ? QJsonValue(QJsonValue::Null) : QJsonValue(base_revision)},
                        {QStringLiteral("sessionId"), session_id}},
            [completion = std::move(completion), draft_id, project_id, branch_id,
             base_revision, session_id](const QJsonObject &body, const QString &error)
    {
        if (!error.isEmpty()) { completion({}, error); return; }
        PersonalWorkingCopyInfo draft;
        if (!parseDraft(body, draft) || draft.id != draft_id || draft.project_id != project_id
            || draft.branch_id != branch_id || draft.base_revision != base_revision
            || draft.session_id != session_id)
        {
            completion({}, QStringLiteral("Invalid created-draft response."));
            return;
        }
        completion(draft, QString());
    });
}

void PersonalWorkingCopyClient::reopen(const PersonalRepositoryTarget &target,
                                       const QString &draft_id, DraftCompletion completion)
{
    if (!DvmdbIdentity::isUuidV7(draft_id))
    {
        completion({}, QStringLiteral("Invalid draft UUIDv7."));
        return;
    }
    request(target, "GET", QStringLiteral("/working-copies/") + draft_id, {},
            [completion = std::move(completion)](const QJsonObject &body,
                                                 const QString &error)
    {
        if (!error.isEmpty()) { completion({}, error); return; }
        PersonalWorkingCopyInfo draft;
        if (!parseDraft(body, draft))
        {
            completion({}, QStringLiteral("Invalid reopened-draft response."));
            return;
        }
        completion(draft, QString());
    });
}

void PersonalWorkingCopyClient::acquireLease(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &session_id, int seconds, LeaseCompletion completion)
{
    if (!DvmdbIdentity::isUuidV7(draft_id) || !DvmdbIdentity::isUuidV7(session_id)
        || seconds < 1 || seconds > 120)
    {
        completion({}, QStringLiteral("Invalid draft/session UUIDv7 or lease duration."));
        return;
    }
    request(target, "POST", QStringLiteral("/working-copies/") + draft_id
                + QStringLiteral("/lease"),
            QJsonObject{{QStringLiteral("sessionId"), session_id},
                        {QStringLiteral("seconds"), seconds}},
            [completion = std::move(completion)](const QJsonObject &response,
                                                 const QString &error)
    {
        if (!error.isEmpty()) { completion({}, error); return; }
        PersonalWorkingCopyLease lease;
        lease.token = response.value(QStringLiteral("token")).toString();
        lease.generation = response.value(QStringLiteral("generation")).toString();
        lease.expires_at_ms = response.value(QStringLiteral("expiresAtMs")).toString();
        if (!DvmdbIdentity::isUuidV7(lease.token) || !decimal(lease.generation)
            || !decimal(lease.expires_at_ms))
        {
            completion({}, QStringLiteral("Invalid writer-lease response."));
            return;
        }
        completion(lease, {});
    });
}

QString PersonalWorkingCopyClient::recordedCommitState(
    const PersonalRepositoryTarget &target, const QString &project_id,
    const QString &branch_id, QString *error)
{
    PersonalCommitIntentStore store;
    PersonalCommitIntent intent;
    const PersonalCommitIntentStore::ReadResult result =
        store.read(target.repository_id, project_id, branch_id, intent, error);
    return result == PersonalCommitIntentStore::ReadResult::Found ? intent.state : QString();
}

bool PersonalWorkingCopyClient::acknowledgeRecordedCommit(
    const PersonalRepositoryTarget &target, const QString &project_id,
    const QString &branch_id, QString *error)
{
    PersonalCommitIntentStore store;
    PersonalCommitIntent intent;
    if (store.read(target.repository_id, project_id, branch_id, intent, error)
        != PersonalCommitIntentStore::ReadResult::Found)
        return false;
    return store.acknowledge(intent, error);
}

void PersonalWorkingCopyClient::reconcileRecordedCommit(
    const PersonalRepositoryTarget &target, const QString &project_id,
    const QString &branch_id, CommitCompletion completion)
{
    PersonalCommitIntentStore store;
    PersonalCommitIntent intent;
    QString disk_error;
    if (store.read(target.repository_id, project_id, branch_id, intent, &disk_error)
        != PersonalCommitIntentStore::ReadResult::Found) {
        completion({}, disk_error.isEmpty()
            ? QStringLiteral("No recorded Commit exists for this branch.") : disk_error);
        return;
    }
    if (intent.endpoint != target.endpoint) {
        completion({}, QStringLiteral("Commit %1 belongs to a different endpoint; select its original "
                                      "personal database before reconciliation.").arg(intent.operation_id));
        return;
    }
    if (intent.state == QStringLiteral("succeeded")) {
        completion(intent.revision, {});
        return;
    }
    if (intent.state == QStringLiteral("rejected")) {
        completion({}, QStringLiteral("Commit %1 was rejected: %2")
            .arg(intent.operation_id, intent.reason));
        return;
    }
    request(target, "GET", QStringLiteral("/local-commits/") + intent.operation_id, {},
            [this, target, intent, completion = std::move(completion)](
                const QJsonObject &status, const QString &lookup_error) mutable
    {
        const std::function<void(const QJsonObject &, const QString &)> finish = [intent, completion = std::move(completion)](
            const QJsonObject &result, const QString &error) mutable
        {
            PersonalCommitIntentStore pending;
            const QString state = result.value(QStringLiteral("state")).toString();
            const QString revision = result.value(QStringLiteral("revision")).toString();
            const QString reason = result.value(QStringLiteral("reason")).toString();
            if (!error.isEmpty()) {
                completion({}, QStringLiteral("Commit %1 outcome unknown: %2. "
                    "The saved operation is retained; retry reconciliation, not a new Commit.")
                    .arg(intent.operation_id, error));
                return;
            }
            if (state == QStringLiteral("pending")) {
                completion({}, QStringLiteral("Commit %1 is still pending on DVMDB; "
                    "the saved operation remains locked.").arg(intent.operation_id));
                return;
            }
            if (state != QStringLiteral("succeeded") && state != QStringLiteral("rejected")) {
                completion({}, QStringLiteral("Commit %1 returned an unknown status; "
                    "the operation remains locked.").arg(intent.operation_id));
                return;
            }
            QString disk_error;
            if (!pending.resolve(intent, state, revision, reason, &disk_error)) {
                completion({}, QStringLiteral("Commit %1 outcome could not be persisted: %2")
                    .arg(intent.operation_id, disk_error));
                return;
            }
            if (state == QStringLiteral("succeeded")) completion(revision, {});
            else completion({}, QStringLiteral("Commit %1 was rejected: %2")
                .arg(intent.operation_id, reason));
        };
        // A confirmed 404 means that DVMDB never recorded this operation.
        // Replay precisely the persisted body and UUID, never synthesize a new ID.
        if (lookup_error.endsWith(QStringLiteral("(not_found)"))) {
            request(target, "POST", QStringLiteral("/local-commits"), intent.body,
                [this, target, intent, finish](const QJsonObject &reply,
                                               const QString &post_error) mutable
            {
                if (post_error.isEmpty() && reply.value(QStringLiteral("state")).toString()
                    == QStringLiteral("succeeded")) { finish(reply, {}); return; }
                request(target, "GET", QStringLiteral("/local-commits/") + intent.operation_id,
                        {}, [finish, post_error](const QJsonObject &resolved,
                                                  const QString &status_error) mutable {
                    // A definitive rejection with an authoritative missing
                    // ledger entry is terminal. Otherwise keep the intent:
                    // a 503/timeout must never be treated as a rejection.
                    if (status_error.endsWith(QStringLiteral("(not_found)"))
                        && (post_error.endsWith(QStringLiteral("(conflict)"))
                            || post_error.endsWith(QStringLiteral("(invalid_request)"))
                            || post_error.endsWith(QStringLiteral("(not_found)")))) {
                        finish(QJsonObject{{QStringLiteral("state"), QStringLiteral("rejected")},
                            {QStringLiteral("reason"), post_error}}, {});
                        return;
                    }
                    finish(resolved, status_error.isEmpty() ? QString()
                        : post_error.isEmpty() ? status_error
                                               : post_error + QStringLiteral("; status: ") + status_error);
                });
            });
            return;
        }
        finish(status, lookup_error);
    });
}

void PersonalWorkingCopyClient::commitSavedDraft(
    const PersonalRepositoryTarget &target, const PersonalWorkingCopyInfo &draft,
    const QString &session_id, const PersonalWorkingCopyLease &lease,
    const QString &message, CommitCompletion completion)
{
    if (!DvmdbIdentity::isUuidV7(draft.id) || !DvmdbIdentity::isUuidV7(draft.project_id)
        || !DvmdbIdentity::isUuidV7(draft.branch_id)
        || !DvmdbIdentity::isUuidV7(session_id) || !DvmdbIdentity::isUuidV7(lease.token)
        || !decimal(lease.generation) || !draft.dirty || draft.changes.isEmpty()
        || message.trimmed().isEmpty() || message.toUtf8().size() > 65536
        || !decimal(draft.generation) || draft.generation != lease.generation) {
        completion({}, QStringLiteral("Commit requires an unchanged, dirty saved draft and a nonempty message."));
        return;
    }
    PersonalCommitIntentStore store;
    PersonalCommitIntent prior;
    QString prior_error;
    const PersonalCommitIntentStore::ReadResult prior_state = store.read(
        target.repository_id, draft.project_id, draft.branch_id, prior, &prior_error);
    if (prior_state != PersonalCommitIntentStore::ReadResult::Missing) {
        completion({}, prior_state == PersonalCommitIntentStore::ReadResult::Error
            ? prior_error : QStringLiteral("Commit %1 is already recorded (%2). "
                "Reconcile or acknowledge it before starting another Commit.")
                .arg(prior.operation_id, prior.state));
        return;
    }
    const QString operation_id = DvmdbIdentity::newUuidV7();
    const QJsonObject body{
        {QStringLiteral("operationId"), operation_id},
        {QStringLiteral("draftId"), draft.id},
        {QStringLiteral("sessionId"), session_id},
        {QStringLiteral("fencingToken"), lease.token},
        {QStringLiteral("expectedGeneration"), draft.generation},
        {QStringLiteral("expectedHead"), draft.base_revision.isEmpty()
            ? QJsonValue(QJsonValue::Null) : QJsonValue(draft.base_revision)},
        {QStringLiteral("author"), QJsonObject{{QStringLiteral("id"), commitAuthorId()},
                                               {QStringLiteral("name"), commitAuthorName()}}},
        {QStringLiteral("message"), message}};
    PersonalCommitIntent intent;
    intent.repository_id = target.repository_id;
    intent.project_id = draft.project_id;
    intent.branch_id = draft.branch_id;
    intent.endpoint = target.endpoint;
    intent.operation_id = operation_id;
    intent.body = body;
    QString disk_error;
    if (!store.create(intent, &disk_error)) {
        completion({}, QStringLiteral("Commit not sent: %1").arg(disk_error));
        return;
    }
    // The durable intent is on disk before the first write to DVMDB.
    request(target, "POST", QStringLiteral("/local-commits"), body,
            [this, target, intent, completion = std::move(completion)](
                const QJsonObject &response, const QString &error) mutable
    {
        const QString state = response.value(QStringLiteral("state")).toString();
        const QString revision = response.value(QStringLiteral("revision")).toString();
        if (error.isEmpty() && state == QStringLiteral("succeeded")
            && DvmdbIdentity::isUuidV7(revision)) {
            PersonalCommitIntentStore store;
            QString disk_error;
            if (!store.resolve(intent, state, revision, {}, &disk_error))
                completion({}, QStringLiteral("Commit %1 succeeded, but its result could not be "
                    "persisted locally: %2").arg(intent.operation_id, disk_error));
            else completion(revision, {});
            return;
        }
        // Even a rejected POST can have a durable server-side ledger entry.
        // Resolve from the authoritative status endpoint, using the same UUID.
        reconcileRecordedCommit(target, intent.project_id, intent.branch_id,
            [completion = std::move(completion), error](const QString &resolved,
                                                         const QString &status_error) mutable {
            completion(resolved, status_error.isEmpty() ? QString()
                : error.isEmpty() ? status_error : error + QStringLiteral("; ") + status_error);
        });
    });
}

void PersonalWorkingCopyClient::importNetworkIntoEmptyDraft(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &session_id, const QString &fencing_token,
    const QString &generation, const NetworkHydraulic &network, DraftCompletion completion)
{
    if (!DvmdbIdentity::isUuidV7(draft_id) || !DvmdbIdentity::isUuidV7(session_id)
        || !DvmdbIdentity::isUuidV7(fencing_token) || !decimal(generation)
        || !DvmdbIdentity::isUuidV7(network.uuid.toString(QUuid::WithoutBraces))) {
        completion({}, QStringLiteral("Invalid private draft, lease, generation or network UUIDv7."));
        return;
    }
    const QByteArray encoded = aowis::model::ModelCodec<NetworkHydraulic>::encode(network);
    if (encoded.isEmpty() || encoded.size() > 5 * 1024 * 1024) {
        completion({}, QStringLiteral("Imported network exceeds the supported canonical HTTP payload size."));
        return;
    }
    request(target, "POST", QStringLiteral("/working-copies/") + draft_id
                + QStringLiteral("/import-network"),
            QJsonObject{{QStringLiteral("sessionId"), session_id},
                        {QStringLiteral("fencingToken"), fencing_token},
                        {QStringLiteral("expectedGeneration"), generation},
                        {QStringLiteral("payloadBase64"), QString::fromLatin1(encoded.toBase64())}},
            [completion = std::move(completion), draft_id, generation, network_id = network.uuid]
            (const QJsonObject &body, const QString &error) {
        if (!error.isEmpty()) {
            completion({}, error + QStringLiteral(" Reopen the saved draft before retrying; "
                "the import may already have completed."));
            return;
        }
        PersonalWorkingCopyInfo saved;
        if (!parseDraft(body, saved) || saved.id != draft_id || !saved.dirty
            || saved.generation != QString::number(generation.toULongLong() + 1)) {
            completion({}, QStringLiteral("Invalid imported-draft acknowledgement. Reopen before retrying."));
            return;
        }
        bool found = false;
        const QString id = network_id.toString(QUuid::WithoutBraces);
        for (const PersonalWorkingCopyChange &change : saved.changes)
            if (!change.deleted && change.kind == 100 && change.uuid == id) found = true;
        if (!found) {
            completion({}, QStringLiteral("Imported network identity missing from saved draft."));
            return;
        }
        completion(saved, {});
    });
}

void PersonalWorkingCopyClient::summarizeDraftNetworks(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    SummaryCompletion completion)
{
    if (!DvmdbIdentity::isUuidV7(draft_id)) {
        completion({}, QStringLiteral("Invalid saved draft UUIDv7."));
        return;
    }
    request(target, "GET", QStringLiteral("/working-copies/") + draft_id
                + QStringLiteral("/networks/summary"), {},
            [completion = std::move(completion), draft_id]
            (const QJsonObject &response, const QString &error) {
        if (!error.isEmpty()) { completion({}, error); return; }
        const QString generation = response.value(QStringLiteral("generation")).toString();
        const QJsonArray networks = response.value(QStringLiteral("networks")).toArray();
        if (response.value(QStringLiteral("scope")).toString() != QStringLiteral("working-copy")
            || response.value(QStringLiteral("draftId")).toString() != draft_id
            || !decimal(generation)
            || !response.value(QStringLiteral("networks")).isArray()
            || !response.value(QStringLiteral("networkCount")).isDouble()
            || response.value(QStringLiteral("networkCount")).toInt(-1) != networks.size()) {
            completion({}, QStringLiteral("Invalid saved draft network summary response."));
            return;
        }
        completion(response, {});
    });
}

void PersonalWorkingCopyClient::loadDraftNetwork(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &network_id, NetworkCompletion completion)
{
    if (!DvmdbIdentity::isUuidV7(draft_id) || !DvmdbIdentity::isUuidV7(network_id))
    {
        completion({}, {}, QStringLiteral("Invalid draft or network UUIDv7."));
        return;
    }
    request(target, "GET", QStringLiteral("/working-copies/") + draft_id
                + QStringLiteral("/networks/") + network_id, {},
            [completion = std::move(completion), draft_id, network_id]
            (const QJsonObject &response, const QString &error)
    {
        if (!error.isEmpty()) { completion({}, {}, error); return; }
        const QString generation = response.value(QStringLiteral("generation")).toString();
        const QString encoded = response.value(QStringLiteral("payloadBase64")).toString();
        if (response.size() != 4 || response.value(QStringLiteral("draftId")).toString() != draft_id ||
            response.value(QStringLiteral("networkId")).toString() != network_id ||
            !decimal(generation) || !response.value(QStringLiteral("payloadBase64")).isString() ||
            encoded.size() > 12 * 1024 * 1024)
        {
            completion({}, {}, QStringLiteral("Invalid draft network identity, generation or payload."));
            return;
        }
        const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
            encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.toBase64() != encoded.toLatin1())
        {
            completion({}, {}, QStringLiteral("Invalid canonical draft network base64."));
            return;
        }
        QString codec_error;
        const std::optional<NetworkHydraulic> network =
            aowis::model::ModelCodec<NetworkHydraulic>::decode(decoded.decoded, &codec_error);
        if (!network || network->uuid.toString(QUuid::WithoutBraces) != network_id ||
            aowis::model::ModelCodec<NetworkHydraulic>::encode(*network) != decoded.decoded)
        {
            completion({}, {}, QStringLiteral("Invalid canonical draft network: %1").arg(codec_error));
            return;
        }
        completion(*network, generation, {});
    });
}

void PersonalWorkingCopyClient::initializeEmptyNetwork(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &session_id, const QString &fencing_token,
    const QString &expected_generation, const QString &network_id,
    const QString &name, DraftCompletion completion)
{
    bool generation_valid = false;
    const qulonglong expected = expected_generation.toULongLong(&generation_valid);
    if (!DvmdbIdentity::isUuidV7(draft_id) ||
        !DvmdbIdentity::isUuidV7(session_id) ||
        !DvmdbIdentity::isUuidV7(fencing_token) ||
        !DvmdbIdentity::isUuidV7(network_id) || !decimal(expected_generation) ||
        !generation_valid || expected == std::numeric_limits<qulonglong>::max() ||
        name.trimmed().isEmpty() || name.size() > 255)
    {
        completion({}, QStringLiteral("Invalid first-network initialization request."));
        return;
    }
    request(target, "POST", QStringLiteral("/working-copies/") + draft_id
                + QStringLiteral("/initialize-network"),
            QJsonObject{{QStringLiteral("sessionId"), session_id},
                        {QStringLiteral("fencingToken"), fencing_token},
                        {QStringLiteral("expectedGeneration"), expected_generation},
                        {QStringLiteral("networkId"), network_id},
                        {QStringLiteral("name"), name}},
            [completion = std::move(completion), draft_id, network_id, expected]
            (const QJsonObject &response, const QString &error)
    {
        if (!error.isEmpty())
        {
            completion({}, error + QStringLiteral(
                " Reopen the draft before retrying initialization; the first network may already exist."));
            return;
        }
        PersonalWorkingCopyInfo draft;
        if (!parseDraft(response, draft) || draft.id != draft_id ||
            !draft.base_revision.isEmpty() || !draft.dirty ||
            draft.generation != QString::number(expected + 1) ||
            draft.changes.size() != 1 ||
            draft.changes.first().uuid != network_id ||
            draft.changes.first().deleted || draft.changes.first().kind != 100 ||
            !draft.changes.first().references.isEmpty() ||
            !draft.events.isEmpty())
        {
            completion({}, QStringLiteral("Invalid first-network draft response. Reopen before retrying."));
            return;
        }
        completion(draft, {});
    });
}

void PersonalWorkingCopyClient::saveChanges(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &session_id, const QString &fencing_token,
    const QString &expected_generation,
    const QList<PersonalWorkingCopyChange> &changes, DraftCompletion completion)
{
    const QString invalid = validateSaveRequest(draft_id, session_id, fencing_token,
                                                expected_generation, changes);
    if (!invalid.isEmpty()) { completion({}, invalid); return; }
    const qulonglong generation = expected_generation.toULongLong();
    QJsonArray batch;
    for (const PersonalWorkingCopyChange &change : changes)
    {
        if (change.deleted)
        {
            batch.append(QJsonObject{{QStringLiteral("uuid"), change.uuid},
                                     {QStringLiteral("delete"), true}});
            continue;
        }
        QJsonArray references;
        for (const QString &reference : change.references)
            references.append(reference);
        batch.append(QJsonObject{{QStringLiteral("uuid"), change.uuid},
                                 {QStringLiteral("kind"), static_cast<int>(change.kind)},
                                 {QStringLiteral("references"), references},
                                 {QStringLiteral("payloadBase64"),
                                  QString::fromLatin1(change.payload.toBase64())}});
    }
    request(target, "POST", QStringLiteral("/working-copies/") + draft_id
                + QStringLiteral("/save"),
            QJsonObject{{QStringLiteral("sessionId"), session_id},
                        {QStringLiteral("fencingToken"), fencing_token},
                        {QStringLiteral("expectedGeneration"), expected_generation},
                        {QStringLiteral("changes"), batch}},
            [completion = std::move(completion), draft_id, generation]
            (const QJsonObject &response, const QString &error)
    {
        if (!error.isEmpty())
        {
            completion({}, error + QStringLiteral(
                " Check the saved draft and its generation before retrying; "
                "the server may have completed this Save."));
            return;
        }
        PersonalWorkingCopyInfo draft;
        if (!parseDraft(response, draft) || draft.id != draft_id
            || draft.generation != QString::number(generation + 1) || !draft.dirty)
        {
            completion({}, QStringLiteral("Save response has an unexpected draft identity or generation. "
                "Reopen the draft before any further Save."));
            return;
        }
        completion(draft, {});
    });
}

void PersonalWorkingCopyClient::saveChangesRecovering(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &session_id, const QString &fencing_token,
    const QString &expected_generation,
    const QList<PersonalWorkingCopyChange> &changes,
    RecoveringSaveCompletion completion)
{
    // Reject invalid commands BEFORE any recovery read: an invalid request
    // cannot be reclassified as a previously successful Save.
    const QString invalid = validateSaveRequest(draft_id, session_id, fencing_token,
                                                expected_generation, changes);
    if (!invalid.isEmpty()) { completion({}, false, invalid); return; }
    // Do not use a network failure as permission to retry a possibly
    // completed write: a generation-checked save must execute at most once.
    saveChanges(target, draft_id, session_id, fencing_token, expected_generation,
                changes, [this, target, draft_id, expected_generation, changes,
                          completion = std::move(completion)]
                (const PersonalWorkingCopyInfo &confirmed, const QString &error)
    {
        if (error.isEmpty())
        {
            completion(confirmed, false, QString());
            return;
        }
        bool parsed = false;
        const qulonglong expected = expected_generation.toULongLong(&parsed);
        if (!parsed || expected == std::numeric_limits<qulonglong>::max())
        {
            completion({}, false, error);
            return;
        }
        reopen(target, draft_id, [draft_id, expected, changes, error,
                                  completion = std::move(completion)]
               (const PersonalWorkingCopyInfo &persisted, const QString &reopen_error)
        {
            if (!reopen_error.isEmpty())
            {
                completion({}, false, error + QStringLiteral(" Recovery lookup failed: ")
                    + reopen_error);
                return;
            }
            if (persisted.id != draft_id ||
                persisted.generation != QString::number(expected + 1))
            {
                completion(persisted, false,
                    error + QStringLiteral(" The authoritative draft generation differs; "
                                           "keep local edits and resolve the conflict."));
                return;
            }
            // A persisted overlay can include unrelated earlier edits.
            // Every proposed record must match exactly, including reference
            // order, deletion state, kind and canonical codec payload bytes.
            QHash<QString, PersonalWorkingCopyChange> by_id;
            for (const PersonalWorkingCopyChange &change : persisted.changes)
                by_id.insert(change.uuid, change);
            for (const PersonalWorkingCopyChange &proposed : changes)
            {
                if (!by_id.contains(proposed.uuid))
                {
                    completion(persisted, false,
                        error + QStringLiteral(" The saved overlay is missing a proposed record."));
                    return;
                }
                const PersonalWorkingCopyChange stored = by_id.value(proposed.uuid);
                if (stored.deleted != proposed.deleted ||
                    (!proposed.deleted &&
                     (stored.kind != proposed.kind ||
                      stored.references != proposed.references ||
                      stored.payload != proposed.payload)))
                {
                    completion(persisted, false,
                        error + QStringLiteral(" The saved overlay differs from the proposed changes."));
                    return;
                }
            }
            completion(persisted, true, QString());
        });
    });
}

void PersonalWorkingCopyClient::saveEventsRecovering(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &session_id, const QString &fencing_token,
    const QString &expected_generation,
    const QList<PersonalWorkingCopyChange> &changes,
    const QList<QByteArray> &encoded_events,
    const QList<HydraulicDraftMembershipEdit> &membership, EventSaveCompletion completion)
{
    const QString invalid = validateSaveRequest(draft_id, session_id, fencing_token,
                                                expected_generation, changes, true);
    qulonglong expected = 0;
    if (!invalid.isEmpty() || !decimalValue(expected_generation, expected) ||
        expected == std::numeric_limits<qulonglong>::max() ||
        encoded_events.isEmpty() || encoded_events.size() > 128)
    {
        completion({}, false, invalid.isEmpty()
            ? QStringLiteral("Invalid event batch or generation.") : invalid);
        return;
    }
    QStringList ids;
    QSet<QString> seen;
    QJsonArray events;
    QList<aowis::model::ModelEditTransaction> decoded_events;
    for (const QByteArray &encoded : encoded_events)
    {
        QString codec_error;
        const std::optional<aowis::model::ModelEditTransaction> event =
            aowis::model::ModelEditTransactionCodec::decode(encoded, &codec_error);
        if (!event.has_value())
        {
            completion({}, false, QStringLiteral("Invalid MODEL event: %1").arg(codec_error));
            return;
        }
        const QString id = event->transactionId.toString(QUuid::WithoutBraces);
        if (!DvmdbIdentity::isUuidV7(id) || seen.contains(id))
        {
            completion({}, false, QStringLiteral("Duplicate or invalid event UUIDv7."));
            return;
        }
        seen.insert(id);
        ids.append(id);
        events.append(QString::fromLatin1(encoded.toBase64()));
        decoded_events.append(event.value());
    }
    const QString membership_error = validateMembership(decoded_events, changes, membership);
    if (!membership_error.isEmpty())
    {
        completion({}, false, membership_error);
        return;
    }
    QJsonObject body{{QStringLiteral("sessionId"), session_id},
                     {QStringLiteral("fencingToken"), fencing_token},
                     {QStringLiteral("expectedGeneration"), expected_generation},
                     {QStringLiteral("changes"), encodeChanges(changes)},
                     {QStringLiteral("events"), events}};
    // DVMDB accepts `membership` only as a non-empty array; an update-only
    // batch omits it. Its derived chunk records are not echoed in `changes`,
    // so recovery below compares only the records sent here.
    if (!membership.isEmpty())
        body.insert(QStringLiteral("membership"), encodeMembership(membership));
    request(target, "POST", QStringLiteral("/working-copies/") + draft_id
                + QStringLiteral("/save-events"),
            body,
            [this, target, draft_id, expected, ids, encoded_events, changes,
             completion = std::move(completion)](const QJsonObject &reply, const QString &error)
    {
        const QString next = QString::number(expected + 1);
        if (error.isEmpty())
        {
            const QJsonValue accepted = reply.value(QStringLiteral("acceptedEventIds"));
            QStringList returned;
            if (accepted.isArray())
                for (const QJsonValue &value : accepted.toArray())
                    returned.append(value.isString() ? value.toString() : QString());
            qulonglong event_count = 0;
            if (reply.value(QStringLiteral("id")).toString() == draft_id &&
                reply.value(QStringLiteral("generation")).toString() == next &&
                decimalValue(reply.value(QStringLiteral("eventCount")).toString(), event_count) &&
                event_count >= static_cast<qulonglong>(ids.size()) && returned == ids)
            {
                completion({draft_id, next, QString::number(event_count), ids}, false, {});
                return;
            }
        }
        // A response can be lost even after an fsynced transaction. Reopen
        // once, never resend, and require exactly the next generation and an
        // exact ordered suffix of persisted event identities AND bytes.
        reopen(target, draft_id,
               [draft_id, expected, ids, encoded_events, changes,
                completion = std::move(completion), error]
               (const PersonalWorkingCopyInfo &draft, const QString &reopen_error)
        {
            const qsizetype offset = draft.events.size() - encoded_events.size();
            bool matches = reopen_error.isEmpty() && draft.id == draft_id &&
                draft.generation == QString::number(expected + 1) &&
                offset >= 0 && matchingChanges(draft.changes, changes);
            if (matches)
                for (qsizetype index = 0; index < encoded_events.size(); ++index)
                    if (draft.events.at(offset + index).id != ids.at(index) ||
                        draft.events.at(offset + index).encoded != encoded_events.at(index))
                    {
                        matches = false;
                        break;
                    }
            if (!matches)
            {
                completion({}, false, QStringLiteral(
                    "Event Save not confirmed: %1. Keep queued events until journal and candidate match.")
                    .arg(!reopen_error.isEmpty() ? reopen_error : error));
                return;
            }
            completion({draft_id, draft.generation,
                        QString::number(draft.events.size()), ids}, true, {});
        });
    });
}

void PersonalWorkingCopyClient::flushOldestQueuedEvent(
    const PersonalRepositoryTarget &target, PersonalEditQueue &queue,
    const QString &draft_id, const QString &session_id,
    const QString &fencing_token, const QString &expected_generation,
    const QList<PersonalWorkingCopyChange> &changes,
    const QList<HydraulicDraftMembershipEdit> &membership,
    QueueFlushCompletion completion)
{
    if (!queue.isOpen() || queue.pending().isEmpty())
    {
        completion({}, false, QStringLiteral("No open personal edit queue with pending events."));
        return;
    }
    if (target.repository_id != queue.repositoryId().toString(QUuid::WithoutBraces) ||
        draft_id != queue.draftId().toString(QUuid::WithoutBraces))
    {
        completion({}, false, QStringLiteral("Editing queue repository or draft does not match Save target."));
        return;
    }
    const PersonalEditQueue::Entry first = queue.pending().first();
    QString codec_error;
    const std::optional<aowis::model::ModelEditTransaction> event =
        aowis::model::ModelEditTransactionCodec::decode(first.encoded_transaction, &codec_error);
    if (!event.has_value() || event->transactionId != first.transaction_id)
    {
        completion({}, false, QStringLiteral("Local editing event failed integrity check."));
        return;
    }
    // Every reversible entity edit must be present with the exact resulting
    // bytes. Membership chunks are never supplied here: DVMDB derives them
    // from `membership`, which saveEventsRecovering checks against the event.
    for (const aowis::model::ModelEntityEdit &edit : event->edits)
    {
        if (edit.entityType == QString::fromLatin1(aowis::model::hydraulicRootEditType))
        {
            if (!edit.before.has_value() || !edit.after.has_value())
            {
                completion({}, false, QStringLiteral("Invalid network-root event batch."));
                return;
            }
            continue;
        }
        const QString uuid = edit.entityId.toString(QUuid::WithoutBraces);
        bool found = false;
        for (const PersonalWorkingCopyChange &change : changes)
            if (change.uuid == uuid && change.deleted == !edit.after.has_value() &&
                (change.deleted || change.payload == *edit.after))
            {
                found = true;
                break;
            }
        if (!found)
        {
            completion({}, false, QStringLiteral(
                "Candidate record missing or mismatched for editing event entity %1.").arg(uuid));
            return;
        }
    }
    // A local precondition failure must not reach the recovery path below,
    // which would read the draft over the network for a batch never sent.
    const QString membership_error = validateMembership({event.value()}, changes, membership);
    if (!membership_error.isEmpty())
    {
        completion({}, false, membership_error);
        return;
    }
    saveEventsRecovering(target, draft_id, session_id, fencing_token,
                         expected_generation, changes, {first.encoded_transaction}, membership,
                         [this, target, draft_id, changes, &queue, first,
                          completion = std::move(completion)]
                         (const PersonalWorkingCopyEventSaveResult &result,
                          bool recovered, const QString &failure)
    {
        if (!failure.isEmpty())
        {
            // After a GUI restart the local queue can still contain the
            // last event that DVMDB already acknowledged. A new lease then
            // reports the *current* generation, so expected+1 recovery alone
            // cannot recognize the old write. Confirm the exact journal tail
            // and candidate bytes before retiring it locally.
            reopen(target, draft_id, [&queue, first, changes,
                   completion = std::move(completion), failure]
                   (const PersonalWorkingCopyInfo &draft, const QString &reopen_error)
            {
                const QString id = first.transaction_id.toString(QUuid::WithoutBraces);
                const bool confirmed = reopen_error.isEmpty() &&
                    !draft.events.isEmpty() && draft.events.last().id == id &&
                    draft.events.last().encoded == first.encoded_transaction &&
                    matchingChanges(draft.changes, changes);
                if (!confirmed)
                {
                    completion({}, false, failure);
                    return;
                }
                QString queue_error;
                if (!queue.acknowledgePersisted(first.transaction_id, &queue_error))
                {
                    completion({}, true, QStringLiteral(
                        "Recovered DVMDB event, but local queue removal failed: %1")
                        .arg(queue_error));
                    return;
                }
                completion(draft.generation, true, {});
            });
            return;
        }
        QString queue_error;
        if (!queue.acknowledgePersisted(first.transaction_id, &queue_error))
        {
            completion({}, recovered, QStringLiteral(
                "DVMDB saved the event, but its local queue acknowledgement failed: %1")
                .arg(queue_error));
            return;
        }
        completion(result.generation, recovered, {});
    });
}

void PersonalWorkingCopyClient::releaseLease(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &session_id, const QString &fencing_token, Completion completion)
{
    if (!DvmdbIdentity::isUuidV7(draft_id) || !DvmdbIdentity::isUuidV7(session_id)
        || !DvmdbIdentity::isUuidV7(fencing_token))
    {
        completion(QStringLiteral("Invalid draft, session or fencing UUIDv7."));
        return;
    }
    request(target, "POST", QStringLiteral("/working-copies/") + draft_id
                + QStringLiteral("/release"),
            QJsonObject{{QStringLiteral("sessionId"), session_id},
                        {QStringLiteral("fencingToken"), fencing_token}},
            [completion = std::move(completion)](const QJsonObject &response,
                                                 const QString &error)
    {
        if (!error.isEmpty()) { completion(error); return; }
        completion(response.value(QStringLiteral("released")) == QJsonValue(true)
            ? QString() : QStringLiteral("Invalid lease-release response."));
    });
}

void PersonalWorkingCopyClient::branchHead(
    const PersonalRepositoryTarget &target, const QString &project_id,
    const QString &branch_id, BranchHeadCompletion completion)
{
    if (!DvmdbIdentity::isUuidV7(project_id) || !DvmdbIdentity::isUuidV7(branch_id))
    {
        completion({}, QStringLiteral("Invalid project or branch UUIDv7."));
        return;
    }
    request(target, "GET", QStringLiteral("/projects/") + project_id +
                QStringLiteral("/branch-ids/") + branch_id, {},
            [branch_id, completion = std::move(completion)](const QJsonObject &branch,
                                                             const QString &error)
    {
        if (!error.isEmpty()) { completion({}, error); return; }
        const QJsonValue revision = branch.value(QStringLiteral("revision"));
        if (branch.value(QStringLiteral("branchId")).toString() != branch_id ||
            !(revision.isNull() || DvmdbIdentity::isUuidV7(revision.toString())))
        {
            completion({}, QStringLiteral("Invalid personal branch HEAD response."));
            return;
        }
        completion(revision.isNull() ? QString() : revision.toString(), {});
    });
}

void PersonalWorkingCopyClient::discardDraft(
    const PersonalRepositoryTarget &target, const QString &draft_id,
    const QString &session_id, const QString &fencing_token,
    const QString &expected_generation, Completion completion)
{
    if (!DvmdbIdentity::isUuidV7(draft_id) || !DvmdbIdentity::isUuidV7(session_id) ||
        !DvmdbIdentity::isUuidV7(fencing_token) || !decimal(expected_generation))
    {
        completion(QStringLiteral("Invalid draft discard request."));
        return;
    }
    request(target, "POST", QStringLiteral("/working-copies/") + draft_id +
                QStringLiteral("/discard"),
            QJsonObject{{QStringLiteral("sessionId"), session_id},
                        {QStringLiteral("fencingToken"), fencing_token},
                        {QStringLiteral("expectedGeneration"), expected_generation}},
            [this, target, draft_id, completion = std::move(completion)](
                const QJsonObject &response, const QString &error) mutable
    {
        if (error.isEmpty() && response.value(QStringLiteral("discarded")) == QJsonValue(true))
        {
            completion({});
            return;
        }
        // A repeated discard after success answers 404, and so may the replay
        // Qt before 6.10 sends after a dropped connection: only the draft's
        // absence proves the deletion.
        reopen(target, draft_id, [error, completion = std::move(completion)](
                   const PersonalWorkingCopyInfo &, const QString &reopen_error) mutable
        {
            if (reopen_error.endsWith(QStringLiteral("(not_found)")))
                completion({});
            else
                completion(error.isEmpty() ? QStringLiteral("Invalid draft discard response.") : error);
        });
    });
}

void PersonalWorkingCopyClient::checkoutBranchPrivately(
    const PersonalRepositoryTarget &target, const QString &source_endpoint,
    const QString &source_repository_id, const QString &project_id,
    const QString &project_name, const QString &branch_id,
    const QString &expected_head, CheckoutCompletion completion)
{
    if ((target.kind != PersonalRepositoryTarget::Kind::Http &&
         target.kind != PersonalRepositoryTarget::Kind::Embedded) ||
        !DvmdbIdentity::isUuidV7(source_repository_id) ||
        !DvmdbIdentity::isUuidV7(project_id) ||
        !DvmdbIdentity::isUuidV7(branch_id) ||
        (!expected_head.isEmpty() && !DvmdbIdentity::isUuidV7(expected_head)) ||
        project_name.trimmed().isEmpty() ||
        source_repository_id == target.repository_id)
    {
        completion(QStringLiteral("Checkout requires two distinct DVMDB "
                                  "repositories and valid project, branch and HEAD identities."));
        return;
    }
    const QUrl sourceUrl(source_endpoint);
    if (!sourceUrl.isValid() ||
        (sourceUrl.scheme() != QStringLiteral("http") &&
         sourceUrl.scheme() != QStringLiteral("https")) ||
        sourceUrl.host().isEmpty() || !sourceUrl.userInfo().isEmpty() ||
        sourceUrl.hasQuery() || sourceUrl.hasFragment())
    {
        completion(QStringLiteral("Source REST endpoint is not valid for checkout."));
        return;
    }
    const QString sourceRoot = endpointRoot(source_endpoint);
    QNetworkRequest identify(QUrl(sourceRoot + QStringLiteral("/repository")));
    identify.setTransferTimeout(10000);
    QNetworkReply *sourceIdentity = this->manager.get(identify);
    connect(sourceIdentity, &QNetworkReply::finished, this,
        [this, sourceIdentity, sourceRoot, source_repository_id, target,
         project_id, project_name, branch_id, expected_head,
         completion = std::move(completion)]() mutable
    {
        const QByteArray identityBytes = sourceIdentity->readAll();
        const bool validIdentity = sourceIdentity->error() == QNetworkReply::NoError &&
            sourceIdentity->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        sourceIdentity->deleteLater();
        const QJsonObject source_info = QJsonDocument::fromJson(identityBytes).object();
        const QString codec_error = ModelCodecCompatibility::error(source_info);
        if (validIdentity && !codec_error.isEmpty())
        {
            completion(codec_error);
            return;
        }
        if (!validIdentity || source_info.value(QStringLiteral("repositoryId")).toString()
            != source_repository_id)
        {
            completion(QStringLiteral("Source repository UUID changed or is unavailable."));
            return;
        }
        // Pin the branch by UUIDv7 (DG-01): its current, possibly renamed,
        // name and HEAD. A concurrent source commit is never silently treated
        // as the revision selected in the Projects graph.
        QNetworkRequest branchRequest(QUrl(sourceRoot + QStringLiteral("/projects/") + project_id +
                                           QStringLiteral("/branch-ids/") + branch_id));
        branchRequest.setTransferTimeout(10000);
        QNetworkReply *branchReply = this->manager.get(branchRequest);
        connect(branchReply, &QNetworkReply::finished, this,
            [this, branchReply, sourceRoot, target, project_id, project_name, branch_id,
             expected_head, completion = std::move(completion)]() mutable
        {
            const QJsonObject branch = QJsonDocument::fromJson(branchReply->readAll()).object();
            const bool branchOk = branchReply->error() == QNetworkReply::NoError &&
                branchReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
            branchReply->deleteLater();
            const QString branch_name = branch.value(QStringLiteral("branch")).toString();
            if (!branchOk || branch.value(QStringLiteral("branchId")).toString() != branch_id ||
                branch_name.isEmpty())
            {
                completion(QStringLiteral("Source branch is not available by its UUIDv7."));
                return;
            }
            const QJsonValue sourceRevision = branch.value(QStringLiteral("revision"));
            if ((expected_head.isEmpty() && !sourceRevision.isNull()) ||
                (!expected_head.isEmpty() && sourceRevision.toString() != expected_head))
            {
                completion(QStringLiteral("Source branch HEAD changed. Refresh Projects before checkout."));
                return;
            }
            // A source branch forked from another unborn branch cannot be
            // faithfully recreated in isolation: preserve provenance or fail.
            if (expected_head.isEmpty() &&
                (!branch.value(QStringLiteral("originBranchId")).isNull() ||
                 !branch.value(QStringLiteral("forkRevisionId")).isNull()))
            {
                completion(QStringLiteral("Unborn checkout requires a root branch with no fork provenance."));
                return;
            }
            const QString branchPath = QStringLiteral("/projects/") + project_id +
                QStringLiteral("/branches/") +
                QString::fromLatin1(QUrl::toPercentEncoding(branch_name));
            request(target, "POST", QStringLiteral("/projects/clone-target"),
                QJsonObject{{QStringLiteral("projectId"), project_id},
                            {QStringLiteral("name"), project_name}},
                [this, sourceRoot, branchPath, branch_name, target, project_id, branch_id, expected_head,
                 completion = std::move(completion)](const QJsonObject &, const QString &error) mutable
            {
                if (!error.isEmpty()) { completion(QStringLiteral("Private project provisioning failed: ") + error); return; }
                if (expected_head.isEmpty())
                {
                    // No revision exists to bundle. Reproduce the verified
                    // unborn root branch identity in the empty clone target.
                    request(target, "POST", QStringLiteral("/projects/") + project_id +
                                QStringLiteral("/unborn-checkout"),
                        QJsonObject{{QStringLiteral("branchId"), branch_id},
                                    {QStringLiteral("name"), branch_name}},
                        [this, target, project_id, branch_id,
                         completion = std::move(completion)](const QJsonObject &created,
                                                              const QString &error) mutable
                    {
                        if (!error.isEmpty() ||
                            created.value(QStringLiteral("branchId")).toString() != branch_id ||
                            !created.value(QStringLiteral("revision")).isNull())
                        {
                            completion(QStringLiteral("Private unborn checkout failed: ") +
                                       (error.isEmpty() ? QStringLiteral("invalid response") : error));
                            return;
                        }
                        request(target, "GET", QStringLiteral("/projects/") + project_id +
                                    QStringLiteral("/branch-ids/") + branch_id, {},
                            [branch_id, completion = std::move(completion)]
                            (const QJsonObject &local, const QString &readError) mutable
                        {
                            if (!readError.isEmpty() ||
                                local.value(QStringLiteral("branchId")).toString() != branch_id ||
                                !local.value(QStringLiteral("revision")).isNull())
                            {
                                completion(QStringLiteral("Private unborn branch identity or HEAD "
                                                          "does not match the selected source."));
                                return;
                            }
                            completion(QString());
                        });
                    });
                    return;
                }
                QNetworkRequest exportRequest(QUrl(sourceRoot + branchPath + QStringLiteral("/bundle")));
                exportRequest.setTransferTimeout(60000);
                QNetworkReply *download = this->manager.get(exportRequest);
                constexpr qint64 maxCheckoutBytes = 64 * 1024 * 1024;
                connect(download, &QNetworkReply::downloadProgress, download,
                    [download](qint64 received, qint64 total)
                {
                    if (received > maxCheckoutBytes || total > maxCheckoutBytes) download->abort();
                });
                connect(download, &QNetworkReply::finished, this,
                    [this, download, branchPath, target, project_id, branch_id, expected_head,
                     completion = std::move(completion)]() mutable
                {
                    const QByteArray bundle = download->readAll();
                    const bool downloaded = download->error() == QNetworkReply::NoError &&
                        download->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                        download->header(QNetworkRequest::ContentTypeHeader).toString()
                            .startsWith(QStringLiteral("application/vnd.aowis.revision-bundle-v2")) &&
                        !bundle.isEmpty() && bundle.size() <= maxCheckoutBytes;
                    const QString fetchError = download->errorString();
                    download->deleteLater();
                    if (!downloaded)
                    {
                        completion(QStringLiteral("Revision bundle download failed or exceeded 64 MiB: ") + fetchError);
                        return;
                    }
                    // AOWREV21/23 full bundles carry the project UUID at offset
                    // 8, the branch UUID at 24 and the immutable tip at 40.
                    // The name used to export may have been renamed or reused
                    // since the lookup above: accept only the pinned branch at
                    // the selected HEAD.
                    const bool fullBundle = bundle.size() >= 96 &&
                        (bundle.startsWith("AOWREV21") || bundle.startsWith("AOWREV23"));
                    const std::function<QString(int)> uuidAt = [&bundle](int offset)
                    {
                        return QUuid::fromRfc4122(bundle.mid(offset, 16)).toString(QUuid::WithoutBraces);
                    };
                    if (!fullBundle || uuidAt(8) != project_id || uuidAt(24) != branch_id ||
                        uuidAt(40) != expected_head)
                    {
                        completion(QStringLiteral("Exported bundle is not the selected branch at the selected "
                                                  "revision. Refresh Projects before checkout."));
                        return;
                    }
                    QNetworkRequest upload(QUrl(endpointRoot(target.endpoint) + branchPath +
                                                QStringLiteral("/bundle")));
                    upload.setTransferTimeout(60000);
                    upload.setHeader(QNetworkRequest::ContentTypeHeader,
                                     QStringLiteral("application/vnd.aowis.revision-bundle-v2"));
                    QNetworkReply *imported = this->manager.post(upload, bundle);
                    connect(imported, &QNetworkReply::finished, this,
                        [this, imported, target, project_id, branch_id, expected_head,
                         completion = std::move(completion)]() mutable
                    {
                        const QByteArray response = imported->readAll();
                        const int status = imported->attribute(
                            QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        const bool success = imported->error() == QNetworkReply::NoError &&
                            (status == 200 || status == 201);
                        const QString failure = responseError(imported, response);
                        imported->deleteLater();
                        if (!success) { completion(QStringLiteral("Private bundle import failed: ") + failure); return; }
                        // Confirm by identity what the personal repository now
                        // holds: the same branch UUIDv7 at the selected HEAD.
                        request(target, "GET", QStringLiteral("/projects/") + project_id +
                                    QStringLiteral("/branch-ids/") + branch_id, {},
                            [branch_id, expected_head, completion = std::move(completion)]
                            (const QJsonObject &local, const QString &error) mutable
                        {
                            if (!error.isEmpty() ||
                                local.value(QStringLiteral("branchId")).toString() != branch_id ||
                                local.value(QStringLiteral("revision")).toString() != expected_head)
                            {
                                completion(QStringLiteral("Imported branch differs from the selected branch "
                                                          "or revision. Refresh Projects before creating a draft."));
                                return;
                            }
                            completion(QString());
                        });
                    });
                });
            });
        });
    });
}
