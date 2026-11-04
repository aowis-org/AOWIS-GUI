#include "projects/model_codec_compatibility.h"
#include "projects/personal_working_copy_client.h"
#include "projects/personal_commit_intent_store.h"
#include "projects/dvmdb_identity.h"
#include "projects/personal_edit_queue.h"
#include "projects/personal_draft_edit_session.h"
#include "network/hydraulic_network_editor.h"
#include "network/hydraulic_edit_transaction_capture.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_root_edit.h>
#include <aowis/model/serialization/model_codec.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHash>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QThread>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdio>
#include <functional>

namespace
{
const QString repository_id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6371");
const QString different_repository = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6372");
const QString project_id = QStringLiteral("019a47b1-2041-7af9-8d1c-2670badb6371");
const QString branch_id = QStringLiteral("019a47b1-2042-7af9-8d1c-2670badb6371");
const QString draft_id = QStringLiteral("019a47b1-2043-7af9-8d1c-2670badb6371");
const QString head_id = QStringLiteral("019a47b1-2044-7af9-8d1c-2670badb6371");
const QString lease_token = QStringLiteral("019a47b1-2045-7af9-8d1c-2670badb6371");
const QString entity_id = QStringLiteral("019a47b1-2046-7af9-8d1c-2670badb6371");

bool check(bool value, const char *message)
{
    if (!value) std::fprintf(stderr, "working_copy_client: %s\n", message);
    return value;
}

// DVMDB's real error shape; the client must surface `message`.
QJsonObject dvmdbError(const QString &code, const QString &message)
{
    return QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

bool until(QCoreApplication &application, const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000)
    {
        application.processEvents();
        if (ready()) return true;
        QThread::msleep(5);
    }
    return ready();
}

class Stub final : public QObject
{
public:
    QTcpServer server;
    QString identity = repository_id;
    QString role = QStringLiteral("private");
    bool incompatible_codec = false;
    bool is_source = false;
    QString source_head = head_id;
    QString bundle_head = head_id;
    QString bundle_project = project_id;
    QString bundle_branch = branch_id;
    int provision_count = 0;
    int bundle_export_count = 0;
    int bundle_import_count = 0;
    int unborn_checkout_count = 0;
    QJsonObject last_unborn_checkout;
    bool source_has_origin = false;
    QJsonObject last_provision;
    int listing_count = 0;
    QList<QJsonObject> listing_records;
    QStringList listing_paths;
    int listing_fault = 0; // 1: bad cursor, 2: repeat page, 3: missing cursor, 4: wrong project
    int creating_count = 0;
    int open_count = 0;
    QJsonObject last_create;
    QJsonObject last_save;
    QJsonArray saved_changes;
    QJsonArray saved_events;
    int event_save_count = 0;
    QJsonObject last_event_save;
    bool drop_next_event_acknowledgement = false;
    bool reject_next_event_save = false;
    bool corrupt_event_reopen = false;
    QString lease_session;
    int save_count = 0;
    int release_count = 0;
    int generation = 0;
    bool malformed_overlay = false;
    bool corrupt_network_payload = false;
    int network_read_count = 0;
    int network_summary_count = 0;
    QString saved_network_id;
    int archive_page_reads = 0;
    int branch_history_reads = 0;
    bool invalid_branch_history_page = false;
    QStringList branch_history_paths;
    bool invalid_archive_page = false;
    QStringList archive_paths;
    int history_cursor_reads = 0;
    int history_navigation_requests = 0;
    bool history_require_committed_head = false;
    QJsonObject last_history_navigation;
    int network_import_count = 0;
    QJsonObject last_network_import;
    bool drop_next_save_acknowledgement = false;
    bool drop_next_commit_acknowledgement = false;
    bool fail_next_commit_status_lookup = false;
    bool drop_before_commit_record = false;
    bool reject_next_commit = false;
    bool reject_next_unrecorded_commit = false;
    QSet<QString> unrecorded_rejections;
    int commit_post_count = 0;
    QHash<QString, QJsonObject> commit_ledger;
    QHash<QString, QJsonObject> commit_requests;

    QByteArray bundlePayload() const
    {
        QByteArray result(96, '\0');
        result.replace(0, 8, "AOWREV21");
        result.replace(8, 16, QUuid(this->bundle_project).toRfc4122());
        result.replace(24, 16, QUuid(this->bundle_branch).toRfc4122());
        result.replace(40, 16, QUuid(this->bundle_head).toRfc4122());
        return result;
    }

    bool listen()
    {
        connect(&this->server, &QTcpServer::newConnection, this, [this]
        {
            while (this->server.hasPendingConnections())
            {
                QTcpSocket *socket = this->server.nextPendingConnection();
                QByteArray *buffer = new QByteArray();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]
                {
                    buffer->append(socket->readAll());
                    const int headers_end = buffer->indexOf("\r\n\r\n");
                    if (headers_end < 0) return;
                    const QByteArray header = buffer->left(headers_end);
                    int content_length = 0;
                    for (const QByteArray &line : header.split('\n'))
                    {
                        const QByteArray trimmed = line.trimmed();
                        if (trimmed.toLower().startsWith("content-length:"))
                            content_length = trimmed.mid(15).trimmed().toInt();
                    }
                    if (buffer->size() < headers_end + 4 + content_length) return;
                    const QByteArray request_line = header.left(header.indexOf("\r\n"));
                    const QJsonObject request_body = QJsonDocument::fromJson(
                        buffer->mid(headers_end + 4, content_length)).object();
                    QJsonObject document;
                    int status = 200;
                    bool binary_response = false;
                    if (request_line.startsWith("GET /dvmdb/v1/repository "))
                    {
                        document.insert(QStringLiteral("repositoryId"), this->identity);
                        document.insert(QStringLiteral("modelCodecFingerprint"),
                            this->incompatible_codec ? QString(64, QLatin1Char('0'))
                                                     : ModelCodecCompatibility::fingerprint());
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/projects/") &&
                             request_line.contains("/action-history?"))
                    {
                        ++this->branch_history_reads;
                        const QString path = QString::fromLatin1(request_line.split(' ').value(1));
                        this->branch_history_paths.append(path);
                        const QUrlQuery query(QUrl(QStringLiteral("http://localhost") + path));
                        const QString after = query.queryItemValue(QStringLiteral("after"));
                        const std::function<QJsonObject(const QString &, const QJsonArray &,
                            const QString &, const QJsonArray &)> commit =
                            [](const QString &id, const QJsonArray &parents,
                               const QString &message, const QJsonArray &actions)
                        {
                            return QJsonObject{
                                {QStringLiteral("revision"), QJsonObject{
                                    {QStringLiteral("id"), id},
                                    {QStringLiteral("parents"), parents},
                                    {QStringLiteral("message"), message}}},
                                {QStringLiteral("actionCount"), actions.size()},
                                {QStringLiteral("actions"), actions},
                                {QStringLiteral("checkpointOnly"), actions.isEmpty()},
                                {QStringLiteral("operationId"), QJsonValue(QJsonValue::Null)}};
                        };
                        if (after.isEmpty())
                        {
                            document = QJsonObject{
                                {QStringLiteral("commits"), QJsonArray{
                                    commit(head_id, QJsonArray{lease_token},
                                        QStringLiteral("Latest water edit"), QJsonArray{draft_id}),
                                    commit(lease_token, QJsonArray{entity_id},
                                        QStringLiteral("Previous checkpoint"), QJsonArray{})}},
                                {QStringLiteral("nextCursor"), this->invalid_branch_history_page
                                    ? QJsonValue(draft_id) : QJsonValue(lease_token)},
                                {QStringLiteral("draft"), QJsonValue(QJsonValue::Null)}};
                        }
                        else
                        {
                            document = QJsonObject{
                                {QStringLiteral("commits"), QJsonArray{
                                    commit(entity_id, {}, QStringLiteral("Original layout"), {})}},
                                {QStringLiteral("nextCursor"), QJsonValue(QJsonValue::Null)},
                                {QStringLiteral("draft"), QJsonValue(QJsonValue::Null)}};
                        }
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/projects/") &&
                             request_line.contains("/action-history/") &&
                             request_line.contains("/events?"))
                    {
                        ++this->archive_page_reads;
                        const QString path = QString::fromLatin1(request_line.split(' ').value(1));
                        this->archive_paths.append(path);
                        const QUrlQuery query(QUrl(QStringLiteral("http://localhost") + path));
                        const int offset = query.queryItemValue(QStringLiteral("offset")).toInt();
                        const QString action = offset == 0 ? entity_id : lease_token;
                        document = QJsonObject{
                            {QStringLiteral("revisionId"), this->invalid_archive_page
                                ? draft_id : head_id},
                            {QStringLiteral("total"), 2},
                            {QStringLiteral("nextOffset"), offset == 0
                                ? QJsonValue(1) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("actions"), QJsonArray{
                                QJsonObject{{QStringLiteral("id"), action},
                                    {QStringLiteral("label"), offset == 0
                                        ? QStringLiteral("Lay pipe") : QStringLiteral("Move valve")},
                                    {QStringLiteral("operation"), QStringLiteral("hydraulic.update")},
                                    {QStringLiteral("editCount"), 1},
                                    {QStringLiteral("frameBase64"), QStringLiteral("AA==")}}}}};
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/projects/") &&
                             request_line.contains("/branch-ids/"))
                    {
                        // DVMDB's branch-by-UUIDv7 record (source and, after
                        // import, the personal repository).
                        const QByteArray requested = request_line.split(' ').value(1);
                        if (!requested.endsWith(branch_id.toLatin1()))
                        {
                            status = 404;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("not_found"), QStringLiteral("branch UUIDv7 not found")));
                        }
                        else
                            document = QJsonObject{{QStringLiteral("branch"), QStringLiteral("main")},
                                                   {QStringLiteral("branchId"), branch_id},
                                                   {QStringLiteral("revision"), this->source_head.isEmpty()
                                                       ? QJsonValue(QJsonValue::Null) : QJsonValue(this->source_head)},
                                                   {QStringLiteral("originBranchId"), this->source_has_origin
                                                       ? QJsonValue(draft_id) : QJsonValue(QJsonValue::Null)},
                                                   {QStringLiteral("forkRevisionId"), QJsonValue(QJsonValue::Null)}};
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/projects/") &&
                             request_line.contains("/bundle ") && this->is_source)
                    {
                        ++this->bundle_export_count;
                        binary_response = true;
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/projects/clone-target ") &&
                             !this->is_source)
                    {
                        ++this->provision_count;
                        this->last_provision = request_body;
                        document.insert(QStringLiteral("id"), project_id);
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/projects/") &&
                             request_line.contains("/unborn-checkout ") && !this->is_source)
                    {
                        ++this->unborn_checkout_count;
                        this->last_unborn_checkout = request_body;
                        document = QJsonObject{{QStringLiteral("branchId"), request_body.value(QStringLiteral("branchId"))},
                            {QStringLiteral("branch"), request_body.value(QStringLiteral("name"))},
                            {QStringLiteral("revision"), QJsonValue(QJsonValue::Null)}};
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/projects/") &&
                             request_line.contains("/bundle ") && !this->is_source)
                    {
                        if (buffer->mid(headers_end + 4, content_length) == bundlePayload())
                        {
                            ++this->bundle_import_count;
                            status = 201;
                            document.insert(QStringLiteral("head"), head_id);
                        }
                        else status = 400;
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/repository/tracking "))
                    {
                        document.insert(QStringLiteral("repositoryId"), this->identity);
                        document.insert(QStringLiteral("modelCodecFingerprint"),
                            this->incompatible_codec ? QString(64, QLatin1Char('0'))
                                                     : ModelCodecCompatibility::fingerprint());
                        document.insert(QStringLiteral("role"), this->role);
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/local-commits "))
                    {
                        ++this->commit_post_count;
                        const QString id = request_body.value(QStringLiteral("operationId")).toString();
                        if (this->reject_next_unrecorded_commit)
                        {
                            this->unrecorded_rejections.insert(id);
                            this->reject_next_unrecorded_commit = false;
                        }
                        if (this->unrecorded_rejections.contains(id))
                        {
                            status = 409;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("conflict"), QStringLiteral("stale writer")));
                        }
                        else if (!this->drop_before_commit_record)
                        {
                            if (this->commit_ledger.contains(id))
                            {
                                if (this->commit_requests.value(id) != request_body)
                                {
                                    status = 409;
                                    document.insert(QStringLiteral("error"), dvmdbError(
                                        QStringLiteral("conflict"), QStringLiteral("operation body changed")));
                                }
                                else document = this->commit_ledger.value(id);
                            }
                            else
                            {
                                document = QJsonObject{{QStringLiteral("operationId"), id},
                                    {QStringLiteral("state"), this->reject_next_commit
                                        ? QStringLiteral("rejected") : QStringLiteral("succeeded")},
                                    {QStringLiteral("revision"), this->reject_next_commit
                                        ? QJsonValue(QJsonValue::Null) : QJsonValue(head_id)},
                                    {QStringLiteral("reason"), this->reject_next_commit
                                        ? QStringLiteral("conflict") : QString()}};
                                this->commit_ledger.insert(id, document);
                                this->commit_requests.insert(id, request_body);
                                if (this->reject_next_commit) status = 409;
                                this->reject_next_commit = false;
                            }
                        }
                        else this->drop_before_commit_record = false;
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/local-commits/"))
                    {
                        if (this->fail_next_commit_status_lookup)
                        {
                            // A dropped GET connection may be transparently retried by Qt.
                            // Return 503 to deterministically exercise an unresolved outcome.
                            this->fail_next_commit_status_lookup = false;
                            status = 503;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("service_unavailable"),
                                QStringLiteral("Commit status temporarily unavailable")));
                        }
                        else
                        {
                            const QByteArray path = request_line.split(' ').value(1);
                            const QString id = QString::fromLatin1(path.mid(path.lastIndexOf('/') + 1));
                            if (this->commit_ledger.contains(id)) document = this->commit_ledger.value(id);
                            else
                            {
                                status = 404;
                                document.insert(QStringLiteral("error"), dvmdbError(
                                    QStringLiteral("not_found"), QStringLiteral("local Commit operation not found")));
                            }
                        }
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/working-copies?"))
                    {
                        ++this->listing_count;
                        const QString path = QString::fromLatin1(request_line.split(' ').value(1));
                        this->listing_paths.append(path);
                        const QUrlQuery query(QUrl(QStringLiteral("http://localhost") + path));
                        const QString after = query.queryItemValue(QStringLiteral("after"));
                        const QString project_filter = query.queryItemValue(QStringLiteral("projectId"));
                        const int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
                        QJsonArray records;
                        bool has_more = false;
                        if (this->listing_records.isEmpty())
                            records.append(this->draftRecord(QString()));
                        else
                        {
                            for (const QJsonObject &draft : this->listing_records)
                            {
                                if (this->listing_fault != 2 &&
                                    draft.value(QStringLiteral("id")).toString() <= after) continue;
                                if (!project_filter.isEmpty() &&
                                    draft.value(QStringLiteral("projectId")).toString() != project_filter)
                                    continue;
                                if (records.size() == limit) { has_more = true; break; }
                                records.append(draft);
                            }
                        }
                        if (this->listing_fault == 4 && !project_filter.isEmpty() && !records.isEmpty())
                        {
                            QJsonObject wrong = records.first().toObject();
                            wrong.insert(QStringLiteral("projectId"), different_repository);
                            records.replace(0, wrong);
                        }
                        const QString last = records.isEmpty() ? QString()
                            : records.last().toObject().value(QStringLiteral("id")).toString();
                        const QString cursor = has_more ? last : QString();
                        document.insert(QStringLiteral("workingCopies"), records);
                        if (this->listing_fault != 3)
                            document.insert(QStringLiteral("nextCursor"),
                                this->listing_fault == 1 && has_more
                                    ? QJsonValue(draft_id)
                                    : cursor.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                       : QJsonValue(cursor));
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies "))
                    {
                        ++this->creating_count;
                        this->last_create = request_body;
                        document = draftRecord(request_body.value(QStringLiteral("sessionId")).toString());
                        // DVMDB creates the draft under the caller-chosen id.
                        document.insert(QStringLiteral("id"), request_body.value(QStringLiteral("id")));
                        document.insert(QStringLiteral("branchId"), request_body.value(QStringLiteral("branchId")));
                        document.insert(QStringLiteral("baseRevision"), request_body.value(QStringLiteral("baseRevision")));
                        status = 201;
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                             request_line.contains("/lease "))
                    {
                        this->lease_session = request_body.value(QStringLiteral("sessionId")).toString();
                        document = QJsonObject{{QStringLiteral("token"), lease_token},
                            {QStringLiteral("generation"), QString::number(this->generation)},
                            {QStringLiteral("expiresAtMs"), QStringLiteral("1800000000000")}};
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/working-copies/") &&
                             request_line.contains("/history/cursor "))
                    {
                        ++this->history_cursor_reads;
                        document = QJsonObject{
                            {QStringLiteral("draftId"), draft_id},
                            {QStringLiteral("generation"), QString::number(this->generation)},
                            {QStringLiteral("baseRevision"), QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("journalCount"), QStringLiteral("1")},
                            {QStringLiteral("actionPosition"), QStringLiteral("0")},
                            {QStringLiteral("selectionKind"), QStringLiteral("draft")},
                            {QStringLiteral("previousAction"), QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("nextAction"), QJsonObject{
                                {QStringLiteral("selectionKind"), QStringLiteral("draft")},
                                {QStringLiteral("revisionId"), QJsonValue(QJsonValue::Null)},
                                {QStringLiteral("actionPosition"), QStringLiteral("1")}}}};
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                             request_line.contains("/history/navigate "))
                    {
                        ++this->history_navigation_requests;
                        this->last_history_navigation = request_body;
                        if (request_body.value(QStringLiteral("expectedGeneration")).toString() !=
                                QString::number(this->generation) ||
                            (this->history_require_committed_head
                                ? request_body.value(QStringLiteral("expectedHead")).toString() != head_id ||
                                  request_body.value(QStringLiteral("revisionId")).toString() != head_id
                                : !request_body.value(QStringLiteral("expectedHead")).isNull()) ||
                            request_body.value(QStringLiteral("fencingToken")).toString() != lease_token)
                        {
                            status = 409;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("conflict"), QStringLiteral("stale History cursor")));
                        }
                        else
                        {
                            ++this->generation;
                            document = draftRecord(this->lease_session);
                        }
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/working-copies/") &&
                             request_line.contains("/networks/summary "))
                    {
                        ++this->network_summary_count;
                        const QString id = this->saved_network_id;
                        const QJsonArray networks = id.isEmpty() ? QJsonArray{} : QJsonArray{
                            QJsonObject{{QStringLiteral("id"), id},
                                        {QStringLiteral("counts"), QJsonObject{}},
                                        {QStringLiteral("entityCount"), 0}}};
                        document = QJsonObject{{QStringLiteral("scope"), QStringLiteral("working-copy")},
                            {QStringLiteral("draftId"), draft_id},
                            {QStringLiteral("generation"), QString::number(this->generation)},
                            {QStringLiteral("networkCount"), networks.size()},
                            {QStringLiteral("networks"), networks}};
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/working-copies/") &&
                             request_line.contains("/networks/"))
                    {
                        ++this->network_read_count;
                        const QByteArray path = request_line.split(' ').value(1);
                        const QString network_id = QString::fromLatin1(path.mid(path.lastIndexOf('/') + 1));
                        NetworkHydraulic network;
                        network.uuid = QUuid(network_id);
                        network.id = QStringLiteral("loaded draft");
                        if (this->corrupt_network_payload) network.uuid = QUuid(entity_id);
                        document = QJsonObject{
                            {QStringLiteral("draftId"), draft_id},
                            {QStringLiteral("networkId"), network_id},
                            {QStringLiteral("generation"), QString::number(this->generation)},
                            {QStringLiteral("payloadBase64"), QString::fromLatin1(
                                aowis::model::ModelCodec<NetworkHydraulic>::encode(network).toBase64())}};
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                             request_line.contains("/initialize-network "))
                    {
                        const QString network_id = request_body.value(QStringLiteral("networkId")).toString();
                        if (request_body.value(QStringLiteral("sessionId")).toString() != this->lease_session ||
                            request_body.value(QStringLiteral("fencingToken")).toString() != lease_token ||
                            request_body.value(QStringLiteral("expectedGeneration")).toString()
                                != QString::number(this->generation) || this->generation != 0)
                        {
                            status = 409;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("conflict"), QStringLiteral("first network requires empty draft")));
                        }
                        else
                        {
                            this->generation = 1;
                            this->saved_changes = QJsonArray{QJsonObject{
                                {QStringLiteral("uuid"), network_id},
                                {QStringLiteral("kind"), 100},
                                {QStringLiteral("references"), QJsonArray{}},
                                {QStringLiteral("payloadBase64"), QStringLiteral("AQID")}}};
                            document = draftRecord(this->lease_session);
                            document.insert(QStringLiteral("changes"), this->saved_changes);
                            document.insert(QStringLiteral("events"), QJsonArray{});
                        }
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                             request_line.contains("/import-network "))
                    {
                        ++this->network_import_count;
                        this->last_network_import = request_body;
                        const QByteArray payload = QByteArray::fromBase64(
                            request_body.value(QStringLiteral("payloadBase64")).toString().toLatin1());
                        const std::optional<NetworkHydraulic> network =
                            aowis::model::ModelCodec<NetworkHydraulic>::decode(payload);
                        if (!network || this->generation != 0
                            || request_body.value(QStringLiteral("sessionId")).toString() != this->lease_session
                            || request_body.value(QStringLiteral("fencingToken")).toString() != lease_token
                            || request_body.value(QStringLiteral("expectedGeneration")).toString()
                                != QString::number(this->generation)) {
                            status = 409;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("conflict"), QStringLiteral("empty unborn draft required")));
                        } else {
                            ++this->generation;
                            this->saved_changes = QJsonArray{QJsonObject{
                                {QStringLiteral("uuid"), network->uuid.toString(QUuid::WithoutBraces)},
                                {QStringLiteral("kind"), 100},
                                {QStringLiteral("references"), QJsonArray{}},
                                {QStringLiteral("payloadBase64"), QString::fromLatin1(payload.toBase64())}}};
                            document = draftRecord(this->lease_session);
                            document.insert(QStringLiteral("changes"), this->saved_changes);
                            document.insert(QStringLiteral("events"), QJsonArray{});
                        }
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                             request_line.contains("/save-events "))
                    {
                        ++this->event_save_count;
                        this->last_event_save = request_body;
                        const QString expected = request_body.value(QStringLiteral("expectedGeneration")).toString();
                        // Same precondition as DVMDB: creating or removing an
                        // entity without a membership array is a 400.
                        bool unlisted = false;
                        if (!request_body.contains(QStringLiteral("membership")))
                            for (const QJsonValue &encoded : request_body.value(QStringLiteral("events")).toArray())
                            {
                                const std::optional<aowis::model::ModelEditTransaction> decoded =
                                    aowis::model::ModelEditTransactionCodec::decode(
                                        QByteArray::fromBase64(encoded.toString().toLatin1()));
                                if (decoded.has_value())
                                    for (const aowis::model::ModelEntityEdit &item : decoded->edits)
                                        unlisted |= !item.before.has_value() || !item.after.has_value();
                            }
                        if (unlisted)
                        {
                            status = 400;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("invalid_request"),
                                QStringLiteral("missing membership for created or removed entity")));
                        }
                        else if (this->reject_next_event_save || expected != QString::number(this->generation) ||
                            request_body.value(QStringLiteral("fencingToken")).toString() != lease_token)
                        {
                            this->reject_next_event_save = false;
                            status = 409;
                            document.insert(QStringLiteral("error"), dvmdbError(QStringLiteral("conflict"),
                                                               QStringLiteral("stale event writer")));
                        }
                        else
                        {
                            bool duplicate = false;
                            for (const QJsonValue &encoded : request_body.value(QStringLiteral("events")).toArray())
                            {
                                const std::optional<aowis::model::ModelEditTransaction> decoded =
                                    aowis::model::ModelEditTransactionCodec::decode(
                                        QByteArray::fromBase64(encoded.toString().toLatin1()));
                                for (const QJsonValue &existing : this->saved_events)
                                    if (existing.toObject().value(QStringLiteral("id")).toString() ==
                                        decoded->transactionId.toString(QUuid::WithoutBraces))
                                        duplicate = true;
                            }
                            if (duplicate)
                            {
                                status = 409;
                                document.insert(QStringLiteral("error"), dvmdbError(QStringLiteral("conflict"),
                                                                   QStringLiteral("event already persisted")));
                            }
                            else
                            {
                            this->saved_changes = request_body.value(QStringLiteral("changes")).toArray();
                            QJsonArray accepted;
                            for (const QJsonValue &value : request_body.value(QStringLiteral("events")).toArray())
                            {
                                const QByteArray bytes = QByteArray::fromBase64(value.toString().toLatin1());
                                const std::optional<aowis::model::ModelEditTransaction> event =
                                    aowis::model::ModelEditTransactionCodec::decode(bytes);
                                const QString id = event->transactionId.toString(QUuid::WithoutBraces);
                                accepted.append(id);
                                this->saved_events.append(QJsonObject{
                                    {QStringLiteral("id"), id},
                                    {QStringLiteral("payloadBase64"), value.toString()}});
                            }
                            ++this->generation;
                            document = QJsonObject{
                                {QStringLiteral("id"), draft_id},
                                {QStringLiteral("generation"), QString::number(this->generation)},
                                {QStringLiteral("eventCount"), QString::number(this->saved_events.size())},
                                {QStringLiteral("acceptedEventIds"), accepted}};
                            }
                        }
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                             request_line.contains("/save "))
                    {
                        this->last_save = request_body;
                        if (request_body.value(QStringLiteral("sessionId")).toString() != this->lease_session
                            || request_body.value(QStringLiteral("fencingToken")).toString() != lease_token
                            || request_body.value(QStringLiteral("expectedGeneration")).toString()
                                != QString::number(this->generation))
                        {
                            status = 409;
                            document.insert(QStringLiteral("error"), dvmdbError(QStringLiteral("conflict"),
                                                               QStringLiteral("stale writer")));
                        }
                        else
                        {
                            ++this->save_count;
                            ++this->generation;
                            this->saved_changes = request_body.value(QStringLiteral("changes")).toArray();
                            document = draftRecord(this->lease_session);
                        }
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                             request_line.contains("/release "))
                    {
                        if (request_body.value(QStringLiteral("fencingToken")).toString() != lease_token
                            || request_body.value(QStringLiteral("sessionId")).toString() != this->lease_session)
                            status = 409;
                        else
                        {
                            ++this->release_count;
                            this->lease_session.clear();
                            document.insert(QStringLiteral("released"), true);
                        }
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/working-copies/"))
                    {
                        ++this->open_count;
                        document = draftRecord(QString());
                        document.insert(QStringLiteral("changes"), this->saved_changes);
                        document.insert(QStringLiteral("events"), this->saved_events);
                        if (this->corrupt_event_reopen && !this->saved_events.isEmpty())
                        {
                            QJsonArray tampered = this->saved_events;
                            QJsonObject modified = tampered.last().toObject();
                            modified.insert(QStringLiteral("payloadBase64"), QStringLiteral("not-base64!!"));
                            tampered.replace(tampered.size() - 1, modified);
                            document.insert(QStringLiteral("events"), tampered);
                        }
                        if (this->malformed_overlay)
                            document.insert(QStringLiteral("changes"), QJsonArray{
                                QJsonObject{{QStringLiteral("uuid"), entity_id},
                                    {QStringLiteral("kind"), 4},
                                    {QStringLiteral("references"), QJsonArray{}},
                                    {QStringLiteral("payloadBase64"), QStringLiteral("@@@")}}});
                    }
                    else
                    {
                        status = 404;
                        document.insert(QStringLiteral("error"), dvmdbError(QStringLiteral("not_found"),
                                                           QStringLiteral("Not found")));
                    }
                    if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                        request_line.contains("/save-events ") &&
                        this->drop_next_event_acknowledgement && status == 200)
                    {
                        this->drop_next_event_acknowledgement = false;
                        socket->disconnectFromHost();
                        return;
                    }
                    if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                        request_line.contains("/save ") &&
                        this->drop_next_save_acknowledgement && status == 200)
                    {
                        this->drop_next_save_acknowledgement = false;
                        socket->disconnectFromHost();
                        return;
                    }
                    if (request_line.startsWith("POST /dvmdb/v1/local-commits ") &&
                        this->drop_next_commit_acknowledgement)
                    {
                        this->drop_next_commit_acknowledgement = false;
                        socket->disconnectFromHost();
                        return;
                    }
                    const QByteArray payload = binary_response ? bundlePayload() :
                        QJsonDocument(document).toJson(QJsonDocument::Compact);
                    const QByteArray contentType = binary_response ?
                        QByteArray("application/vnd.aowis.revision-bundle-v2") : QByteArray("application/json");
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  " OK\r\nContent-Type: " + contentType +
                                  "\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, [socket, buffer]
                {
                    delete buffer;
                    socket->deleteLater();
                });
            }
        });
        return this->server.listen(QHostAddress::LocalHost);
    }

    QJsonObject draftRecord(const QString &session) const
    {
        return QJsonObject{
            {QStringLiteral("id"), draft_id},
            {QStringLiteral("projectId"), project_id},
            {QStringLiteral("branchId"), branch_id},
            {QStringLiteral("baseRevision"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("sessionId"), session.isEmpty() ? head_id : session},
            {QStringLiteral("generation"), QString::number(this->generation)},
            {QStringLiteral("dirty"), this->generation != 0},
            {QStringLiteral("eventCount"), QString::number(this->saved_events.size())}};
    }

    PersonalRepositoryTarget target() const
    {
        PersonalRepositoryTarget result;
        result.kind = PersonalRepositoryTarget::Kind::Http;
        result.endpoint = QStringLiteral("http://127.0.0.1:%1").arg(this->server.serverPort());
        result.repository_id = repository_id;
        return result;
    }
};
}

// A real HTTP session/controller test, with the same private identity,
// generation, writer fencing and event-save contracts as the existing fixture.
// This does NOT assert that the Distribution Editor has been bound to autosave.
bool testDraftEditSession(QCoreApplication &application)
{
    Stub stub;
    if (!check(stub.listen(), "controller fixture could not listen")) return false;
    QTemporaryDir root;
    if (!check(root.isValid(), "controller fixture queue root invalid")) return false;
    const QString network_id = PersonalWorkingCopyClient::newSessionUuidV7();
    PersonalDraftEditSession session;
    bool opened = false;
    QString problem;
    QObject::connect(&session, &PersonalDraftEditSession::ready,
                     &session, [&] { opened = true; });
    QObject::connect(&session, &PersonalDraftEditSession::blocked,
                     &session, [&](const QString &message) { problem = message; });
    session.start(stub.target(), draft_id, network_id, root.path());
    if (!check(until(application, [&] { return opened || !problem.isEmpty(); }) &&
               opened && problem.isEmpty() && session.isReady() &&
               session.currentGeneration() == QStringLiteral("0") &&
               session.pendingCount() == 0 &&
               session.visibleNetwork().uuid == QUuid(network_id),
               "session did not verify, hydrate and fence the private draft")) return false;

    NetworkHydraulic live = session.visibleNetwork();
    HydraulicNetworkEditor editor(live);
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.junction.create"));
    CoordinateWGS84 point;
    point.latitude_deg = 50.1;
    point.longitude_deg = 8.4;
    const QUuid junction = editor.addJunction(point);
    if (!check(!junction.isNull() && capture.watchCreated(junction),
               "could not prepare a hydraulic edit for session")) return false;
    const std::optional<QByteArray> encoded = capture.finish(live);
    if (!check(encoded.has_value(), "could not encode hydraulic edit")) return false;

    bool confirmed = false;
    bool recovered = false;
    QString accepted_generation;
    QObject::connect(&session, &PersonalDraftEditSession::saveConfirmed,
                     &session, [&](const QString &generation, bool was_recovered)
    {
        confirmed = true;
        recovered = was_recovered;
        accepted_generation = generation;
    });
    // DVMDB stores the event but the HTTP acknowledgement never reaches Qt.
    // The controller must resolve it by authoritative reopen, NOT resend it.
    stub.drop_next_event_acknowledgement = true;
    QString append_error;
    if (!check(session.appendTransaction(*encoded, &append_error) && append_error.isEmpty(),
               "session failed to durably append the MODEL event")) return false;
    if (!check(until(application, [&] { return confirmed || !problem.isEmpty(); }) &&
               confirmed && problem.isEmpty() && recovered &&
               accepted_generation == QStringLiteral("1") &&
               session.pendingCount() == 0 &&
               stub.event_save_count == 1 && stub.saved_events.size() == 1 &&
               session.visibleNetwork().nodes_junctions.size() == 1,
               "lost acknowledgement did not confirm exactly one durable Save")) return false;

    // A GUI-detected uncaptured mutation must fail closed even when the
    // server acknowledged all earlier events. A blocked session never accepts
    // additional edits and its already-confirmed model remains intact.
    session.blockEditing(QStringLiteral("Uncaptured hydraulic mutation"));
    if (!check(session.isBlocked() && !session.isReady() &&
               !session.appendTransaction(*encoded, &append_error) &&
               session.pendingCount() == 0 && stub.saved_events.size() == 1,
               "unsafe live mutation did not block the draft writer")) return false;

    bool stopped = false;
    QString stop_error;
    QObject::connect(&session, &PersonalDraftEditSession::stopped,
                     &session, [&](const QString &failure)
    {
        stopped = true;
        stop_error = failure;
    });
    session.stop();
    if (!check(until(application, [&] { return stopped; }) && stop_error.isEmpty() &&
               stub.release_count == 1 && !session.isReady(),
               "session failed to release fenced writer on stop")) return false;

    // A rejected Save cannot remove the pending local event. Restarting the
    // session from the unchanged authoritative draft must replay and save it.
    Stub rejected;
    if (!check(rejected.listen(), "rejection fixture could not listen")) return false;
    QTemporaryDir failed_root;
    const QString failed_network = PersonalWorkingCopyClient::newSessionUuidV7();
    PersonalDraftEditSession first_attempt;
    bool failed_open = false;
    QString failure;
    QObject::connect(&first_attempt, &PersonalDraftEditSession::ready,
                     &first_attempt, [&] { failed_open = true; });
    QObject::connect(&first_attempt, &PersonalDraftEditSession::blocked,
                     &first_attempt, [&](const QString &message) { failure = message; });
    first_attempt.start(rejected.target(), draft_id, failed_network, failed_root.path());
    if (!check(until(application, [&] { return failed_open || !failure.isEmpty(); }) &&
               failed_open, "rejected Save fixture session did not start")) return false;
    NetworkHydraulic retry_live = first_attempt.visibleNetwork();
    HydraulicNetworkEditor retry_editor(retry_live);
    HydraulicEditTransactionCapture retry_capture(QStringLiteral("hydraulic.junction.create"));
    const QUuid retry_junction = retry_editor.addJunction(point);
    retry_capture.watchCreated(retry_junction);
    const std::optional<QByteArray> retry_event = retry_capture.finish(retry_live);
    if (!check(retry_event.has_value(), "rejected Save fixture event missing")) return false;
    rejected.reject_next_event_save = true;
    if (!check(first_attempt.appendTransaction(*retry_event, &append_error),
               "rejected Save fixture could not append event")) return false;
    if (!check(until(application, [&] { return !failure.isEmpty(); }) &&
               first_attempt.isBlocked() && first_attempt.pendingCount() == 1 &&
               rejected.saved_events.isEmpty(),
               "rejected Save incorrectly acknowledged or dropped queued event")) return false;
    bool first_stopped = false;
    QObject::connect(&first_attempt, &PersonalDraftEditSession::stopped,
                     &first_attempt, [&](const QString &) { first_stopped = true; });
    first_attempt.stop();
    if (!check(until(application, [&] { return first_stopped; }),
               "rejected writer failed to stop")) return false;

    PersonalDraftEditSession restarted;
    bool restart_ready = false;
    bool restart_saved = false;
    QString restart_failure;
    QObject::connect(&restarted, &PersonalDraftEditSession::ready,
                     &restarted, [&] { restart_ready = true; });
    QObject::connect(&restarted, &PersonalDraftEditSession::blocked,
                     &restarted, [&](const QString &message) { restart_failure = message; });
    QObject::connect(&restarted, &PersonalDraftEditSession::saveConfirmed,
                     &restarted, [&](const QString &, bool) { restart_saved = true; });
    restarted.start(rejected.target(), draft_id, failed_network, failed_root.path());
    if (!check(until(application, [&] { return restart_saved || !restart_failure.isEmpty(); }) &&
               restart_ready && restart_saved && restart_failure.isEmpty() &&
               restarted.pendingCount() == 0 && rejected.saved_events.size() == 1 &&
               rejected.event_save_count == 2,
               "session restart did not replay the same queued event exactly once")) return false;
    bool restart_stopped = false;
    QObject::connect(&restarted, &PersonalDraftEditSession::stopped,
                     &restarted, [&](const QString &) { restart_stopped = true; });
    restarted.stop();
    return check(until(application, [&] { return restart_stopped; }),
                 "restarted writer failed to stop");
}

bool testDurableCommitRecovery(QCoreApplication &application)
{
    Stub server;
    if (!check(server.listen(), "cannot listen for Commit recovery tests")) return false;
    const QString project = DvmdbIdentity::newUuidV7();
    const QString branch = DvmdbIdentity::newUuidV7();
    PersonalWorkingCopyInfo draft;
    draft.id = DvmdbIdentity::newUuidV7();
    draft.project_id = project;
    draft.branch_id = branch;
    draft.generation = QStringLiteral("1");
    draft.dirty = true;
    draft.changes.append({entity_id, false, 1, {}, QByteArray("data")});
    PersonalWorkingCopyLease lease;
    lease.token = lease_token;
    lease.generation = draft.generation;
    const QString session = DvmdbIdentity::newUuidV7();
    PersonalWorkingCopyClient first;
    QString revision;
    QString error;
    bool done = false;
    const PersonalRepositoryTarget target = server.target();
    QString state_error;
    if (!check(PersonalWorkingCopyClient::recordedCommitState(target, project, branch,
                                                               &state_error).isEmpty() &&
               state_error.isEmpty(), "new recovery fixture already has a saved Commit")) return false;
    server.drop_next_commit_acknowledgement = true;
    // Force status lookup failure after the lost POST acknowledgement. A dropped
    // GET could be automatically retried and resolved by QNetworkAccessManager,
    // which is also correct but would not exercise the pending-intent path.
    server.fail_next_commit_status_lookup = true;
    first.commitSavedDraft(target, draft, session, lease, QStringLiteral("broken network baseline"),
        [&](const QString &value, const QString &failure) {
            revision = value; error = failure; done = true;
        });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty()
               && revision.isEmpty() && server.commit_post_count == 1
               && PersonalWorkingCopyClient::recordedCommitState(target, project, branch)
                  == QStringLiteral("pending"),
               "lost POST response and unavailable status did not preserve pending local intent")) return false;
    done = false;
    first.commitSavedDraft(target, draft, session, lease, QStringLiteral("different message"),
        [&](const QString &, const QString &failure) { error = failure; done = true; });
    if (!check(done && !error.isEmpty() && server.commit_post_count == 1,
               "a second Commit was sent despite a pending operation")) return false;
    // Destroy the request client and reconcile using a fresh one (GUI restart).
    PersonalWorkingCopyClient restarted;
    done = false;
    restarted.reconcileRecordedCommit(target, project, branch,
        [&](const QString &value, const QString &failure) {
            revision = value; error = failure; done = true;
        });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && revision == head_id && server.commit_post_count == 1
               && PersonalWorkingCopyClient::recordedCommitState(target, project, branch)
                   == QStringLiteral("succeeded"),
               "restart did not reconcile the same server-side revision")) return false;
    if (!check(PersonalWorkingCopyClient::acknowledgeRecordedCommit(target, project, branch,
                                                                     &state_error),
               "cannot acknowledge confirmed recovered Commit")) return false;

    // When the first POST never reached durable ledger, GET returns a proven
    // 404; only the exact saved request may be replayed, with the same UUID.
    server.drop_before_commit_record = true;
    server.drop_next_commit_acknowledgement = true;
    done = false;
    first.commitSavedDraft(target, draft, session, lease, QStringLiteral("retry exact request"),
        [&](const QString &value, const QString &failure) {
            revision = value; error = failure; done = true;
        });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && revision == head_id && server.commit_post_count == 3
               && server.commit_ledger.size() == 2,
               "lost before-ledger POST did not retry original operation UUID")) return false;
    if (!check(PersonalWorkingCopyClient::acknowledgeRecordedCommit(target, project, branch,
                                                                     &state_error),
               "cannot acknowledge exact replay")) return false;

    server.reject_next_commit = true;
    done = false;
    first.commitSavedDraft(target, draft, session, lease, QStringLiteral("reject this request"),
        [&](const QString &value, const QString &failure) {
            revision = value; error = failure; done = true;
        });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty()
               && revision.isEmpty() && server.commit_post_count == 4
               && PersonalWorkingCopyClient::recordedCommitState(target, project, branch)
                   == QStringLiteral("rejected"),
               "rejected operation not durably resolved")) return false;
    if (!check(PersonalWorkingCopyClient::acknowledgeRecordedCommit(target, project, branch,
                                                                     &state_error),
               "cannot acknowledge rejected Commit")) return false;
    // A 409 before creation of the server ledger followed by confirmed 404
    // is a terminal rejection, not an eternally pending operation.
    server.reject_next_unrecorded_commit = true;
    done = false;
    first.commitSavedDraft(target, draft, session, lease, QStringLiteral("invalid lease"),
        [&](const QString &value, const QString &failure) {
            revision = value; error = failure; done = true;
        });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty()
               && revision.isEmpty() && server.commit_post_count == 6
               && PersonalWorkingCopyClient::recordedCommitState(target, project, branch)
                   == QStringLiteral("rejected"),
               "pre-ledger HTTP 409 left an unresolved Commit intent")) return false;
    if (!check(PersonalWorkingCopyClient::acknowledgeRecordedCommit(target, project, branch,
                                                                     &state_error),
               "cannot acknowledge pre-ledger rejection")) return false;
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (!testDraftEditSession(application)) return 1;
    if (!testDurableCommitRecovery(application)) return 1;
    Stub stub;
    if (!check(stub.listen(), "could not start private DVMDB fixture")) return 1;
    PersonalWorkingCopyClient client;
    // Cursor coordinates must come from DVMDB, with no client-supplied native overlay.
    Stub history_stub;
    if (!check(history_stub.listen(), "could not start History navigation fixture")) return 1;
    history_stub.lease_session = head_id;
    bool history_done = false;
    QString history_error;
    QJsonObject cursor;
    client.historyCursor(history_stub.target(), draft_id,
        [&](const QJsonObject &response, const QString &failure)
    {
        cursor = response;
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) && history_error.isEmpty() &&
               cursor.value(QStringLiteral("nextAction")).isObject() &&
               history_stub.history_cursor_reads == 1,
               "History cursor was not read from private DVMDB")) return 1;
    // Full first-parent branch History is fetched independently of Projects'
    // graph preview, with a server-authored continuation cursor.
    history_done = false;
    QList<QJsonObject> branch_commits;
    QString branch_next;
    client.branchHistoryPage(history_stub.target(), project_id,
        QStringLiteral("main"), QString(),
        [&](const QList<QJsonObject> &entries, const QString &next, const QString &failure)
    {
        branch_commits = entries;
        branch_next = next;
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) && history_error.isEmpty() &&
               branch_commits.size() == 2 && branch_next == lease_token &&
               branch_commits.first().value(QStringLiteral("id")).toString() == head_id &&
               branch_commits.last().value(QStringLiteral("firstParent")).toString() == entity_id &&
               branch_commits.first().value(QStringLiteral("actionCount")).toInt() == 1 &&
               history_stub.branch_history_reads == 1 &&
               history_stub.branch_history_paths.first().contains(
                   QStringLiteral("/branches/main/action-history?limit=10")),
               "full branch History initial page was not validated or repository-scoped")) return 1;
    history_done = false;
    client.branchHistoryPage(history_stub.target(), project_id,
        QStringLiteral("main"), branch_next,
        [&](const QList<QJsonObject> &entries, const QString &next, const QString &failure)
    {
        branch_commits = entries;
        branch_next = next;
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) && history_error.isEmpty() &&
               branch_commits.size() == 1 && branch_next.isEmpty() &&
               branch_commits.first().value(QStringLiteral("id")).toString() == entity_id &&
               history_stub.branch_history_paths.last().contains(
                   QStringLiteral("&after=") + lease_token),
               "full branch History pagination did not reach its first ancestor")) return 1;
    history_stub.invalid_branch_history_page = true;
    history_done = false;
    client.branchHistoryPage(history_stub.target(), project_id,
        QStringLiteral("main"), QString(),
        [&](const QList<QJsonObject> &, const QString &, const QString &failure)
    {
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) &&
               !history_error.isEmpty(),
               "branch History accepted a forged continuation cursor")) return 1;
    history_stub.invalid_branch_history_page = false;
    // Original Commit actions are paged from the private repository and
    // validated by revision and offset. A malformed page never reaches UI.
    history_done = false;
    QList<QJsonObject> archived_actions;
    int next_archive_offset = -1;
    int archived_total = -1;
    client.archivedActionPage(history_stub.target(), project_id, QStringLiteral("main"),
        head_id, 0, [&](const QList<QJsonObject> &entries, int next, int total,
                        const QString &failure)
    {
        archived_actions = entries;
        next_archive_offset = next;
        archived_total = total;
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) && history_error.isEmpty() &&
               archived_actions.size() == 1 && next_archive_offset == 1 &&
               archived_total == 2 &&
               archived_actions.first().value(QStringLiteral("label")).toString() ==
                   QStringLiteral("Lay pipe") &&
               !archived_actions.first().contains(QStringLiteral("frameBase64")) &&
               history_stub.archive_page_reads == 1 &&
               history_stub.archive_paths.first().contains(
                   QStringLiteral("/branches/main/action-history/") + head_id +
                   QStringLiteral("/events?offset=0&limit=50")),
               "archived MODEL page was not scoped, validated and stripped of large frames")) return 1;
    history_done = false;
    client.archivedActionPage(history_stub.target(), project_id, QStringLiteral("main"),
        head_id, 1, [&](const QList<QJsonObject> &entries, int next, int total,
                        const QString &failure)
    {
        archived_actions = entries;
        next_archive_offset = next;
        archived_total = total;
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) && history_error.isEmpty() &&
               archived_actions.size() == 1 && next_archive_offset == -1 &&
               archived_total == 2 &&
               history_stub.archive_page_reads == 2,
               "archived MODEL pagination did not terminate correctly")) return 1;
    history_stub.invalid_archive_page = true;
    history_done = false;
    client.archivedActionPage(history_stub.target(), project_id, QStringLiteral("main"),
        head_id, 0, [&](const QList<QJsonObject> &, int, int, const QString &failure)
    {
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) &&
               !history_error.isEmpty(),
               "archived MODEL response accepted the wrong revision")) return 1;
    history_stub.invalid_archive_page = false;
    history_done = false;
    history_done = false;
    PersonalWorkingCopyInfo navigated;
    const QString history_operation = PersonalWorkingCopyClient::newSessionUuidV7();
    client.navigateHistory(history_stub.target(), draft_id, history_operation, head_id,
        lease_token, QStringLiteral("0"), QString(),
        cursor.value(QStringLiteral("nextAction")).toObject(),
        [&](const PersonalWorkingCopyInfo &draft, const QString &failure)
    {
        navigated = draft;
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) && history_error.isEmpty() &&
               navigated.generation == QStringLiteral("1") &&
               history_stub.history_navigation_requests == 1 &&
               history_stub.last_history_navigation.value(QStringLiteral("operationId")).toString()
                   == history_operation &&
               history_stub.last_history_navigation.value(QStringLiteral("actionPosition")).toString()
                   == QStringLiteral("1") &&
               history_stub.last_history_navigation.value(QStringLiteral("expectedHead")).isNull() &&
               !history_stub.last_history_navigation.contains(QStringLiteral("changes")),
               "History navigation did not send a fenced server-built boundary request")) return 1;
    history_done = false;
    client.navigateHistory(history_stub.target(), draft_id,
        PersonalWorkingCopyClient::newSessionUuidV7(), head_id, lease_token,
        QStringLiteral("1"), QString(),
        QJsonObject{{QStringLiteral("selectionKind"), QStringLiteral("archived")},
                    {QStringLiteral("revisionId"), QStringLiteral("not-an-id")},
                    {QStringLiteral("actionPosition"), QStringLiteral("0")}},
        [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        history_error = failure;
        history_done = true;
    });
    if (!check(history_done && !history_error.isEmpty() &&
               history_stub.history_navigation_requests == 1,
               "invalid archived target was transmitted to the server")) return 1;
    history_stub.history_require_committed_head = true;
    history_done = false;
    const QJsonObject checkpoint_target{
        {QStringLiteral("selectionKind"), QStringLiteral("archived")},
        {QStringLiteral("revisionId"), head_id},
        {QStringLiteral("actionPosition"), QStringLiteral("2")}};
    client.navigateHistory(history_stub.target(), draft_id,
        PersonalWorkingCopyClient::newSessionUuidV7(), head_id, lease_token,
        QStringLiteral("1"), head_id, checkpoint_target,
        [&](const PersonalWorkingCopyInfo &draft, const QString &failure)
    {
        navigated = draft;
        history_error = failure;
        history_done = true;
    });
    if (!check(until(application, [&] { return history_done; }) && history_error.isEmpty() &&
               navigated.generation == QStringLiteral("2") &&
               history_stub.history_navigation_requests == 2 &&
               history_stub.last_history_navigation.value(QStringLiteral("revisionId")).toString()
                   == head_id &&
               history_stub.last_history_navigation.value(QStringLiteral("expectedHead")).toString()
                   == head_id &&
               history_stub.last_history_navigation.value(QStringLiteral("actionPosition")).toString()
                   == QStringLiteral("2"),
               "direct Commit jump did not send one fenced archived navigation")) return 1;
    // Bootstrap a first network on an independent unborn draft. It must be
    // represented as kind-100 candidate data, with no synthetic commit.
    Stub bootstrap;
    if (!check(bootstrap.listen(), "could not start unborn network bootstrap fixture")) return 1;
    const QString network_id = PersonalWorkingCopyClient::newSessionUuidV7();
    bool initialized = false;
    QString init_error;
    PersonalWorkingCopyInfo initial_draft;
    const QString init_session = head_id;
    bootstrap.lease_session = init_session;
    client.initializeEmptyNetwork(bootstrap.target(), draft_id, init_session, lease_token,
        QStringLiteral("0"), network_id, QStringLiteral("water network"),
        [&](const PersonalWorkingCopyInfo &draft, const QString &failure)
        { initial_draft = draft; init_error = failure; initialized = true; });
    if (!check(until(application, [&] { return initialized; }) && init_error.isEmpty() &&
               initial_draft.generation == QStringLiteral("1") &&
               initial_draft.changes.size() == 1 &&
               initial_draft.changes.first().uuid == network_id &&
               initial_draft.changes.first().kind == 100 &&
               initial_draft.events.isEmpty(),
               "GUI first hydraulic network initialization did not persist an empty root")) return 1;
    initialized = false;
    NetworkHydraulic loaded_network;
    QString loaded_generation;
    client.loadDraftNetwork(bootstrap.target(), draft_id, network_id,
        [&](const NetworkHydraulic &network, const QString &generation, const QString &failure)
    {
        loaded_network = network;
        loaded_generation = generation;
        init_error = failure;
        initialized = true;
    });
    if (!check(until(application, [&] { return initialized; }) && init_error.isEmpty() &&
               loaded_generation == QStringLiteral("1") &&
               loaded_network.uuid == QUuid(network_id) &&
               loaded_network.id == QStringLiteral("loaded draft") &&
               bootstrap.network_read_count == 1,
               "canonical draft hydration failed")) return 1;
    // A successful Commit rebases the draft onto the new revision and clears
    // its candidate changes. The summary must still locate the inherited
    // network; using draft.changes here would incorrectly report no network.
    bootstrap.saved_network_id = network_id;
    bootstrap.saved_changes = QJsonArray{}; // Emulate a rebased, clean candidate.
    QJsonObject network_summary;
    initialized = false;
    client.summarizeDraftNetworks(bootstrap.target(), draft_id,
        [&](const QJsonObject &summary, const QString &failure)
        { network_summary = summary; init_error = failure; initialized = true; });
    if (!check(until(application, [&] { return initialized; }) && init_error.isEmpty()
               && network_summary.value(QStringLiteral("generation")).toString()
                   == QStringLiteral("1")
               && network_summary.value(QStringLiteral("networkCount")).toInt() == 1
               && network_summary.value(QStringLiteral("networks")).toArray().first().toObject()
                   .value(QStringLiteral("id")).toString() == network_id
               && bootstrap.network_summary_count == 1,
               "saved draft summary failed to discover an inherited network")) return 1;
    bootstrap.corrupt_network_payload = true;
    initialized = false;
    client.loadDraftNetwork(bootstrap.target(), draft_id, network_id,
        [&](const NetworkHydraulic &, const QString &, const QString &failure)
        { init_error = failure; initialized = true; });
    if (!check(until(application, [&] { return initialized; }) && !init_error.isEmpty() &&
               bootstrap.network_read_count == 2,
               "GUI draft hydration accepted wrong network UUID")) return 1;
    bootstrap.corrupt_network_payload = false;
    initialized = false;
    client.loadDraftNetwork(bootstrap.target(), draft_id, QStringLiteral("not-a-uuid"),
        [&](const NetworkHydraulic &, const QString &, const QString &failure)
        { init_error = failure; initialized = true; });
    if (!check(initialized && !init_error.isEmpty() && bootstrap.network_read_count == 2,
               "invalid draft hydration UUID reached HTTP")) return 1;
    initialized = false;
    client.initializeEmptyNetwork(bootstrap.target(), draft_id, init_session, lease_token,
        QStringLiteral("0"), PersonalWorkingCopyClient::newSessionUuidV7(),
        QStringLiteral("other network"),
        [&](const PersonalWorkingCopyInfo &, const QString &failure)
        { init_error = failure; initialized = true; });
    if (!check(until(application, [&] { return initialized; }) && !init_error.isEmpty() &&
               bootstrap.generation == 1,
               "GUI first hydraulic network accepted a stale second initialization")) return 1;
    // A complete imported EPANET model is sent to a specific, fenced private
    // draft; no global editor model or committed revision is touched.
    Stub import_stub;
    if (!check(import_stub.listen(), "could not start full-network import fixture")) return 1;
    import_stub.lease_session = init_session;
    NetworkHydraulic imported_network;
    imported_network.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    imported_network.id = QStringLiteral("Imported network");
    initialized = false;
    client.importNetworkIntoEmptyDraft(import_stub.target(), draft_id, init_session,
        lease_token, QStringLiteral("0"), imported_network,
        [&](const PersonalWorkingCopyInfo &draft, const QString &failure) {
            initial_draft = draft;
            init_error = failure;
            initialized = true;
        });
    if (!check(until(application, [&] { return initialized; }) && init_error.isEmpty()
            && import_stub.network_import_count == 1 &&
            import_stub.generation == 1 && initial_draft.dirty
            && initial_draft.generation == QStringLiteral("1")
            && initial_draft.changes.size() == 1
            && initial_draft.changes.first().uuid == imported_network.uuid.toString(QUuid::WithoutBraces),
            "GUI full-network import did not save to the selected private draft")) return 1;
    initialized = false;
    client.importNetworkIntoEmptyDraft(import_stub.target(), draft_id, init_session,
        lease_token, QStringLiteral("0"), imported_network,
        [&](const PersonalWorkingCopyInfo &, const QString &failure) {
            init_error = failure;
            initialized = true;
        });
    if (!check(until(application, [&] { return initialized; }) && !init_error.isEmpty()
            && import_stub.network_import_count == 2 && import_stub.generation == 1,
            "GUI full-network import accepted overwrite of a populated draft")) return 1;
    if (!check(DvmdbIdentity::isUuidV7(PersonalWorkingCopyClient::newSessionUuidV7()),
               "session generator did not produce UUIDv7")) return 1;
    bool done = false;
    QString error;
    QList<PersonalWorkingCopyInfo> listed;
    // A mismatched server is rejected before even listing drafts, and therefore
    // before any queued MODEL edit can be replayed through save-events.
    stub.incompatible_codec = true;
    client.list(stub.target(), [&](const QList<PersonalWorkingCopyInfo> &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; })
               && error.contains(QStringLiteral("MODEL codec mismatch"))
               && stub.listing_count == 0,
               "MODEL mismatch was not rejected before private DVMDB access")) return 1;
    stub.incompatible_codec = false;
    done = false;
    client.list(stub.target(), [&](const QList<PersonalWorkingCopyInfo> &drafts, const QString &failure)
    {
        listed = drafts;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && listed.size() == 1 && listed.first().id == draft_id,
               "list could not read personal working-copy metadata")) return 1;

    // Standalone routes its native in-process catalogue through the same
    // verified, loopback-only HTTP adapter. Kind::Embedded is not a bypass of
    // repository UUID/role checks or the working-copy response validation.
    PersonalRepositoryTarget embedded = stub.target();
    embedded.kind = PersonalRepositoryTarget::Kind::Embedded;
    done = false;
    client.list(embedded, [&](const QList<PersonalWorkingCopyInfo> &drafts,
                              const QString &failure)
    {
        listed = drafts;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && listed.size() == 1 && listed.first().id == draft_id
               && stub.listing_count == 2,
               "embedded personal target did not use the verified draft transport")) return 1;

    // INT-03: listing is complete, not just the first default 100 entries.
    // The server also filters by project UUID before paginating, so drafts
    // for another project cannot conceal a competing draft for this HEAD.
    const QString another_project = different_repository;
    for (int index = 0; index < 280; ++index)
    {
        QJsonObject item = stub.draftRecord(QString());
        item.insert(QStringLiteral("id"),
            QStringLiteral("019a47b1-2043-7af9-8d1c-2670%1")
                .arg(index, 8, 16, QLatin1Char('0')));
        item.insert(QStringLiteral("projectId"),
            index % 11 == 0 ? another_project : project_id);
        stub.listing_records.append(item);
    }
    const int before_paged = stub.listing_count;
    done = false;
    client.list(stub.target(), project_id,
        [&](const QList<PersonalWorkingCopyInfo> &drafts, const QString &failure)
    {
        listed = drafts;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && listed.size() == 254 && stub.listing_count == before_paged + 3
               && listed.first().project_id == project_id
               && listed.last().id == stub.listing_records.last().value(QStringLiteral("id")).toString()
               && stub.listing_paths.last().contains(QStringLiteral("projectId="))
               && stub.listing_paths.last().contains(QStringLiteral("&after=")),
               "project-scoped paginated list omitted working copies beyond the first page")) return 1;
    const int before_all = stub.listing_count;
    done = false;
    client.list(stub.target(),
        [&](const QList<PersonalWorkingCopyInfo> &drafts, const QString &failure)
    {
        listed = drafts;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && listed.size() == 280 && stub.listing_count == before_all + 3,
               "unfiltered paginated list lost drafts from another project")) return 1;
    // Failure must be atomic: malformed pages must never return the already
    // received drafts as a complete list to callers that could Commit them.
    for (int fault = 1; fault <= 4; ++fault)
    {
        stub.listing_fault = fault;
        done = false;
        client.list(stub.target(), project_id,
            [&](const QList<PersonalWorkingCopyInfo> &drafts, const QString &failure)
        {
            listed = drafts;
            error = failure;
            done = true;
        });
        if (!check(until(application, [&] { return done; })
                   && !error.isEmpty() && listed.isEmpty(),
                   "malformed draft page was accepted or returned a partial list")) return 1;
    }
    stub.listing_fault = 0;
    const int before_invalid_project = stub.listing_count;
    done = false;
    client.list(stub.target(), QStringLiteral("not-a-uuid"),
        [&](const QList<PersonalWorkingCopyInfo> &drafts, const QString &failure)
    {
        listed = drafts;
        error = failure;
        done = true;
    });
    if (!check(done && !error.isEmpty() && listed.isEmpty()
               && stub.listing_count == before_invalid_project,
               "invalid project filter reached DVMDB")) return 1;
    stub.listing_records.clear();

    done = false;
    PersonalWorkingCopyInfo created;
    client.create(stub.target(), project_id, branch_id, {},
        [&](const PersonalWorkingCopyInfo &draft, const QString &failure)
    {
        created = draft;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && DvmdbIdentity::isUuidV7(created.id)
               && created.id == stub.last_create.value(QStringLiteral("id")).toString()
               && created.base_revision.isEmpty()
               && stub.last_create.value(QStringLiteral("baseRevision")).isNull()
               && DvmdbIdentity::isUuidV7(stub.last_create.value(QStringLiteral("sessionId")).toString())
               && stub.last_create.value(QStringLiteral("branchId")).toString() == branch_id
               && !stub.last_create.contains(QStringLiteral("branch")),
               "unborn draft create did not pin exact inputs")) return 1;
    // Branch names are not identities: a name is refused before any request.
    done = false;
    const int creates_before_name = stub.creating_count;
    client.create(stub.target(), project_id, QStringLiteral("main"), {},
        [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(done && !error.isEmpty() && stub.creating_count == creates_before_name,
               "draft create accepted a branch name")) return 1;
    done = false;
    client.reopen(stub.target(), draft_id,
        [&](const PersonalWorkingCopyInfo &draft, const QString &failure)
    {
        created = draft;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && created.id == draft_id && stub.open_count == 1,
               "reopen did not load the saved draft by UUID")) return 1;

    done = false;
    client.create(stub.target(), project_id, branch_id, head_id,
        [&](const PersonalWorkingCopyInfo &draft, const QString &failure)
    {
        created = draft;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && created.base_revision == head_id
               && stub.last_create.value(QStringLiteral("baseRevision")).toString() == head_id,
               "committed draft creation did not pin its source revision")) return 1;

    done = false;
    PersonalRepositoryTarget absent;
    client.create(absent, project_id, branch_id, head_id,
        [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(done && !error.isEmpty() && stub.creating_count == 2,
               "unconfigured personal repository silently used a source profile")) return 1;

    // New GUI checkout: validate remote identity/HEAD, provision the same
    // logical project UUID, and transfer a full revision bundle privately.
    Stub source;
    source.identity = different_repository;
    source.role = QStringLiteral("shared");
    source.is_source = true;
    if (!check(source.listen(), "could not start shared source DVMDB fixture")) return 1;
    done = false;
    client.checkoutBranchPrivately(stub.target(), source.target().endpoint,
        different_repository, project_id, QStringLiteral("Network test"),
        branch_id, head_id,
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && error.isEmpty() &&
               source.bundle_export_count == 1 && stub.provision_count == 1 &&
               stub.bundle_import_count == 1 &&
               stub.last_provision.value(QStringLiteral("projectId")).toString() == project_id &&
               stub.last_provision.value(QStringLiteral("name")).toString() == QStringLiteral("Network test"),
               "checkout did not transfer the expected project, branch and HEAD")) return 1;
    done = false;
    client.checkoutBranchPrivately(stub.target(), source.target().endpoint,
        repository_id, project_id, QStringLiteral("Network test"),
        branch_id, head_id,
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty() &&
               stub.provision_count == 1 && stub.bundle_import_count == 1,
               "checkout accepted a mismatched source repository identity")) return 1;
    source.source_head = draft_id;
    done = false;
    client.checkoutBranchPrivately(stub.target(), source.target().endpoint,
        different_repository, project_id, QStringLiteral("Network test"),
        branch_id, head_id,
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty() &&
               stub.provision_count == 1 && stub.bundle_import_count == 1,
               "checkout accepted a changed source HEAD")) return 1;

    // A source commit during bundle export must not silently import a
    // different immutable HEAD into the private repository.
    source.source_head = head_id;
    source.bundle_head = draft_id;
    done = false;
    client.checkoutBranchPrivately(stub.target(), source.target().endpoint,
        different_repository, project_id, QStringLiteral("Network test"),
        branch_id, head_id,
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty() &&
               stub.bundle_import_count == 1,
               "checkout imported a bundle with the wrong immutable HEAD")) return 1;

    // The exported name may meanwhile belong to another branch (rename and
    // reuse): the bundle must carry exactly the pinned branch UUIDv7.
    source.bundle_head = head_id;
    source.bundle_branch = draft_id;
    done = false;
    client.checkoutBranchPrivately(stub.target(), source.target().endpoint,
        different_repository, project_id, QStringLiteral("Network test"),
        branch_id, head_id,
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty() &&
               stub.bundle_import_count == 1,
               "checkout imported a bundle of a different branch")) return 1;
    source.bundle_branch = branch_id;

    // Unborn checkout has no bundle, no synthetic revision, and preserves the
    // source branch UUIDv7. A second request is a safe retry.
    Stub unborn_target;
    unborn_target.source_head.clear();
    if (!check(unborn_target.listen(), "could not start unborn personal target")) return 1;
    source.source_head.clear();
    const int exports_before_unborn = source.bundle_export_count;
    done = false;
    client.checkoutBranchPrivately(unborn_target.target(), source.target().endpoint,
        different_repository, project_id, QStringLiteral("Network test"), branch_id, {},
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && error.isEmpty() &&
               unborn_target.provision_count == 1 && unborn_target.unborn_checkout_count == 1 &&
               unborn_target.bundle_import_count == 0 &&
               source.bundle_export_count == exports_before_unborn &&
               unborn_target.last_unborn_checkout.value(QStringLiteral("branchId")) == branch_id &&
               unborn_target.last_unborn_checkout.value(QStringLiteral("name")) == "main",
               "unborn checkout did not preserve branch identity without a revision")) return 1;
    done = false;
    client.checkoutBranchPrivately(unborn_target.target(), source.target().endpoint,
        different_repository, project_id, QStringLiteral("Network test"), branch_id, {},
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && error.isEmpty() &&
               unborn_target.unborn_checkout_count == 2 &&
               source.bundle_export_count == exports_before_unborn,
               "unborn checkout retry failed")) return 1;
    source.source_has_origin = true;
    done = false;
    client.checkoutBranchPrivately(unborn_target.target(), source.target().endpoint,
        different_repository, project_id, QStringLiteral("Network test"), branch_id, {},
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty() &&
               unborn_target.unborn_checkout_count == 2,
               "unborn checkout silently discarded source fork provenance")) return 1;
    source.source_has_origin = false;
    source.source_head = head_id;

    // A model-record Save must use the exact fenced lease and generation.
    PersonalWorkingCopyLease lease;
    done = false;
    client.acquireLease(stub.target(), draft_id, head_id, 90,
        [&](const PersonalWorkingCopyLease &result, const QString &failure)
    {
        lease = result;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && lease.token == lease_token && lease.generation == QStringLiteral("0"),
               "lease acquisition did not expose the fencing token and generation")) return 1;

    PersonalWorkingCopyChange update;
    update.uuid = entity_id;
    update.kind = 42;
    update.references.append(project_id);
    update.payload = QByteArray("AOWIS raw payload \0 bytes", 25);
    done = false;
    client.saveChanges(stub.target(), draft_id, head_id, lease.token, lease.generation,
                       {update}, [&](const PersonalWorkingCopyInfo &draft, const QString &failure)
    {
        created = draft;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && created.generation == QStringLiteral("1") && created.dirty
               && stub.save_count == 1 && stub.saved_changes.size() == 1
               && stub.saved_changes.first().toObject().value(QStringLiteral("kind")).toInt() == 42
               && stub.saved_changes.first().toObject().value(QStringLiteral("payloadBase64")).toString()
                  == QString::fromLatin1(update.payload.toBase64()),
               "draft Save did not preserve the typed record and exact bytes")) return 1;

    done = false;
    client.reopen(stub.target(), draft_id,
        [&](const PersonalWorkingCopyInfo &draft, const QString &failure)
    {
        created = draft;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && created.changes.size() == 1 && created.changes.first().uuid == entity_id
               && created.changes.first().payload == update.payload
               && created.changes.first().references == update.references,
               "reopened draft did not decode its saved candidate overlay")) return 1;

    // Simulate an acknowledged durable write whose HTTP response is lost.
    // The client must reopen once and verify exact bytes plus generation;
    // it must NEVER issue a duplicate Save request.
    stub.drop_next_save_acknowledgement = true;
    update.payload.append(" after interruption");
    bool recovered = false;
    done = false;
    client.saveChangesRecovering(stub.target(), draft_id, head_id, lease.token,
        QStringLiteral("1"), {update},
        [&](const PersonalWorkingCopyInfo &draft, bool from_recovery, const QString &failure)
    {
        created = draft;
        recovered = from_recovery;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && recovered && created.generation == QStringLiteral("2")
               && created.changes.size() == 1
               && created.changes.first().payload == update.payload
               && stub.save_count == 2 && stub.open_count >= 3,
               "lost Save acknowledgement was not recovered exactly once")) return 1;

    done = false;
    client.saveChanges(stub.target(), draft_id, head_id, lease.token, lease.generation,
                       {update}, [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty()
               && stub.save_count == 2,
               "stale Save overwrote a newer generation")) return 1;
    done = false;
    client.saveChanges(stub.target(), draft_id, head_id, draft_id, QStringLiteral("1"),
                       {update}, [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty()
               && stub.save_count == 2,
               "stale fencing token was accepted")) return 1;

    // A stale request that happens to propose the same data must not be
    // mistaken for recovered success: generation must be exactly expected+1.
    done = false;
    recovered = true;
    client.saveChangesRecovering(stub.target(), draft_id, head_id, lease.token,
        QStringLiteral("0"), {update},
        [&](const PersonalWorkingCopyInfo &, bool from_recovery, const QString &failure)
    {
        recovered = from_recovery;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty()
               && !recovered && stub.save_count == 2,
               "stale Save was incorrectly accepted as recovered")) return 1;

    stub.malformed_overlay = true;
    done = false;
    client.reopen(stub.target(), draft_id,
        [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty(),
               "malformed candidate model record was accepted on reopening")) return 1;
    stub.malformed_overlay = false;

    done = false;
    update.references = {QStringLiteral("not-a-uuid")};
    client.saveChanges(stub.target(), draft_id, head_id, lease.token, QStringLiteral("1"),
                       {update}, [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(done && !error.isEmpty() && stub.save_count == 2,
               "invalid model reference reached HTTP Save")) return 1;

    // Recovery cannot reinterpret an invalid command as an older completed
    // Save, even when the persisted generation and payload happen to match.
    const int reads_before_invalid = stub.open_count;
    done = false;
    recovered = true;
    client.saveChangesRecovering(stub.target(), draft_id, head_id, lease.token,
        QStringLiteral("1"), {update},
        [&](const PersonalWorkingCopyInfo &, bool from_recovery, const QString &failure)
    {
        recovered = from_recovery;
        error = failure;
        done = true;
    });
    if (!check(done && !error.isEmpty() && !recovered &&
               stub.open_count == reads_before_invalid && stub.save_count == 2,
               "invalid Save incorrectly triggered recovery or another write")) return 1;

    // Atomic MODEL event journal: small ACK, exact transaction identity,
    // dropped ACK recovery, and local queue removal only after verification.
    aowis::model::ModelEditTransaction event;
    event.transactionId = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    event.operation = QStringLiteral("hydraulic.pipe.diameter");
    aowis::model::ModelEntityEdit edit;
    edit.entityId = QUuid(entity_id);
    edit.entityType = QStringLiteral("HydraulicLinkPipe");
    // An attribute update: before and after images, so no membership change.
    edit.before = QByteArray("pipe before diameter change");
    edit.after = update.payload;
    event.edits.append(edit);
    const QByteArray encoded = aowis::model::ModelEditTransactionCodec::encode(event);
    if (!check(!encoded.isEmpty(), "could not encode MODEL edit event")) return 1;
    update.references.clear();
    done = false;
    PersonalWorkingCopyEventSaveResult journal_ack;
    recovered = true;
    client.saveEventsRecovering(stub.target(), draft_id, head_id, lease.token,
        QStringLiteral("2"), {update}, {encoded}, {},
        [&](const PersonalWorkingCopyEventSaveResult &result, bool from_recovery, const QString &failure)
    {
        journal_ack = result;
        recovered = from_recovery;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty() && !recovered &&
               journal_ack.generation == QStringLiteral("3") &&
               journal_ack.accepted_ids == QStringList{event.transactionId.toString(QUuid::WithoutBraces)} &&
               stub.event_save_count == 1 && stub.saved_events.size() == 1,
               "atomic event Save did not confirm ordered journal IDs")) return 1;

    QTemporaryDir queue_root;
    if (!check(queue_root.isValid(), "could not create temporary queue root")) return 1;
    PersonalEditQueue queue;
    QString queue_error;
    if (!check(queue.open(queue_root.path(), QUuid(repository_id), QUuid(draft_id), &queue_error),
               "could not open durable editing queue")) return 1;
    aowis::model::ModelEditTransaction queued_event = event;
    queued_event.transactionId = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    queued_event.edits.first().after = QByteArray("post reconnect bytes");
    update.payload = *queued_event.edits.first().after;
    const QByteArray queued_bytes = aowis::model::ModelEditTransactionCodec::encode(queued_event);
    if (!check(queue.append(queued_bytes, &queue_error) && queue.pending().size() == 1,
               "could not persist queued editing event")) return 1;
    done = false;
    client.flushOldestQueuedEvent(stub.target(), queue, head_id, head_id, lease.token,
        QStringLiteral("3"), {update}, {},
        [&](const QString &, bool, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(done && !error.isEmpty() && queue.pending().size() == 1 &&
               stub.event_save_count == 1,
               "wrong draft target was not rejected before queue replay")) return 1;
    stub.drop_next_event_acknowledgement = true;
    done = false;
    QString next_generation;
    recovered = false;
    client.flushOldestQueuedEvent(stub.target(), queue, draft_id, head_id, lease.token,
        QStringLiteral("3"), {update}, {},
        [&](const QString &next, bool from_recovery, const QString &failure)
    {
        next_generation = next;
        recovered = from_recovery;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty() && recovered &&
               next_generation == QStringLiteral("4") && queue.pending().isEmpty() &&
               stub.event_save_count == 2 && stub.saved_events.size() == 2,
               "dropped atomic Save acknowledgement did not recover without duplicate writes")) return 1;

    // A crash after server persistence but before deleting the local file
    // leaves the exact last event queued. Reopen confirms it without replay.
    if (!check(queue.append(queued_bytes) && queue.pending().size() == 1,
               "could not simulate a pre-acknowledgement GUI crash")) return 1;
    done = false;
    recovered = false;
    client.flushOldestQueuedEvent(stub.target(), queue, draft_id, head_id, lease.token,
        QStringLiteral("4"), {update}, {},
        [&](const QString &next, bool from_recovery, const QString &failure)
    {
        next_generation = next;
        recovered = from_recovery;
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.isEmpty() && recovered &&
               next_generation == QStringLiteral("4") && queue.pending().isEmpty() &&
               stub.saved_events.size() == 2 && stub.generation == 4,
               "restart replay did not confirm existing journal without duplicate write")) return 1;

    // Refused server writes cannot acknowledge/remove queued local work.
    queued_event.transactionId = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    queued_event.edits.first().after = QByteArray("still pending");
    update.payload = *queued_event.edits.first().after;
    if (!check(queue.append(aowis::model::ModelEditTransactionCodec::encode(queued_event)),
               "failed to enqueue rejection test")) return 1;
    stub.reject_next_event_save = true;
    done = false;
    client.flushOldestQueuedEvent(stub.target(), queue, draft_id, head_id, lease.token,
        QStringLiteral("4"), {update}, {},
        [&](const QString &, bool, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty() &&
               queue.pending().size() == 1 && stub.saved_events.size() == 2,
               "rejected server event incorrectly erased the local queue")) return 1;
    if (!check(error.contains(QStringLiteral("stale event writer (conflict)")),
               "DVMDB error message was not surfaced to the caller")) return 1;
    // A candidate payload unrelated to the journal must be rejected locally.
    update.payload = QByteArray("unrelated candidate");
    const int saves_before_mismatch = stub.event_save_count;
    done = false;
    client.flushOldestQueuedEvent(stub.target(), queue, draft_id, head_id, lease.token,
        QStringLiteral("4"), {update}, {},
        [&](const QString &, bool, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(done && !error.isEmpty() && queue.pending().size() == 1 &&
               stub.event_save_count == saves_before_mismatch,
               "mismatched event/model records were transmitted")) return 1;
    queue.close();

    // Creating an entity: the membership intent travels with the event and
    // its typed record. Without an intent nothing is sent at all.
    const QUuid network_uuid(PersonalWorkingCopyClient::newSessionUuidV7());
    const QUuid junction_uuid(PersonalWorkingCopyClient::newSessionUuidV7());
    aowis::model::ModelEditTransaction creation;
    creation.transactionId = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    creation.operation = QStringLiteral("hydraulic.junction.create");
    creation.edits.append(aowis::model::ModelEntityEdit{
        junction_uuid, QStringLiteral("HydraulicNodeJunction"), std::nullopt,
        QByteArray("junction after-image")});
    const QByteArray creation_bytes = aowis::model::ModelEditTransactionCodec::encode(creation);
    PersonalWorkingCopyChange created_record;
    created_record.uuid = junction_uuid.toString(QUuid::WithoutBraces);
    created_record.kind = 1;
    created_record.payload = *creation.edits.first().after;
    HydraulicDraftMembershipEdit insertion;
    insertion.network_id = network_uuid;
    insertion.entity_id = junction_uuid;
    insertion.kind = 1;
    insertion.collection_index = 9;
    insertion.inserted = true;
    bool batch_recovered = false;
    const std::function<QString(const QList<PersonalWorkingCopyChange> &,
                                const QList<QByteArray> &,
                                const QList<HydraulicDraftMembershipEdit> &)> saveBatch =
        [&](const QList<PersonalWorkingCopyChange> &records, const QList<QByteArray> &frames,
            const QList<HydraulicDraftMembershipEdit> &intents)
    {
        done = false;
        QString result;
        client.saveEventsRecovering(stub.target(), draft_id, head_id, lease.token,
            QString::number(stub.generation), records, frames, intents,
            [&](const PersonalWorkingCopyEventSaveResult &, bool from_recovery, const QString &failure)
        {
            result = failure;
            batch_recovered = from_recovery;
            done = true;
        });
        until(application, [&] { return done; });
        return result;
    };
    const int saves_before_membership = stub.event_save_count;
    error = saveBatch({created_record}, {creation_bytes}, {});
    if (!check(done && error.contains(QStringLiteral("no matching membership intent")) &&
               stub.event_save_count == saves_before_membership,
               "creation without membership intent was transmitted")) return 1;
    HydraulicDraftMembershipEdit removal = insertion;
    removal.inserted = false;
    error = saveBatch({created_record}, {creation_bytes}, {removal});
    if (!check(done && !error.isEmpty() && stub.event_save_count == saves_before_membership,
               "creation with a removal intent was transmitted")) return 1;
    PersonalWorkingCopyChange wrong_record = created_record;
    wrong_record.payload = QByteArray("different bytes");
    error = saveBatch({wrong_record}, {creation_bytes}, {insertion});
    if (!check(done && !error.isEmpty() && stub.event_save_count == saves_before_membership,
               "membership record differing from the after-image was transmitted")) return 1;
    HydraulicDraftMembershipEdit stray = insertion;
    stray.entity_id = QUuid(entity_id);
    error = saveBatch({update}, {aowis::model::ModelEditTransactionCodec::encode(queued_event)},
                      {stray});
    if (!check(done && !error.isEmpty() && stub.event_save_count == saves_before_membership,
               "membership intent for an attribute update was transmitted")) return 1;
    // The queue path rejects the same mismatch without any network read.
    PersonalEditQueue reopened_queue;
    if (!check(reopened_queue.open(queue_root.path(), QUuid(repository_id), QUuid(draft_id),
                                   &queue_error) && reopened_queue.pending().size() == 1,
               "could not reopen the pending editing queue")) return 1;
    PersonalWorkingCopyChange pending_record = update;
    pending_record.payload = QByteArray("still pending");
    const int reads_before_flush = stub.open_count;
    done = false;
    client.flushOldestQueuedEvent(stub.target(), reopened_queue, draft_id, head_id, lease.token,
        QString::number(stub.generation), {pending_record}, {stray},
        [&](const QString &, bool, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(done && error.contains(QStringLiteral("does not describe a creation")) &&
               reopened_queue.pending().size() == 1 && stub.open_count == reads_before_flush &&
               stub.event_save_count == saves_before_membership,
               "queued flush with a stray membership intent reached the network")) return 1;
    reopened_queue.close();
    stub.reject_next_event_save = false;
    const QString generation_before_creation = QString::number(stub.generation);
    // Lost acknowledgement: the reopened journal and sent records confirm the
    // write; DVMDB's derived membership chunks are not part of the comparison.
    stub.drop_next_event_acknowledgement = true;
    error = saveBatch({created_record}, {creation_bytes}, {insertion});
    const QJsonArray sent_membership =
        stub.last_event_save.value(QStringLiteral("membership")).toArray();
    const QJsonObject sent_intent = sent_membership.isEmpty()
        ? QJsonObject() : sent_membership.first().toObject();
    if (!check(done && error.isEmpty() && batch_recovered &&
               stub.event_save_count == saves_before_membership + 1 &&
               stub.last_event_save.value(QStringLiteral("expectedGeneration")).toString()
                   == generation_before_creation &&
               sent_membership.size() == 1 && sent_intent.size() == 5 &&
               sent_intent.value(QStringLiteral("networkId")).toString()
                   == network_uuid.toString(QUuid::WithoutBraces) &&
               sent_intent.value(QStringLiteral("entityId")).toString()
                   == junction_uuid.toString(QUuid::WithoutBraces) &&
               sent_intent.value(QStringLiteral("kind")).toInt() == 1 &&
               sent_intent.value(QStringLiteral("collectionIndex")).toInt() == 9 &&
               sent_intent.value(QStringLiteral("inserted")) == QJsonValue(true),
               "membership intent was not sent with the creation event")) return 1;
    // Update-only batches omit the field: DVMDB requires a non-empty array.
    aowis::model::ModelEditTransaction retitle = queued_event;
    retitle.transactionId = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    retitle.edits.first().after = QByteArray("update only");
    PersonalWorkingCopyChange retitled = update;
    retitled.payload = *retitle.edits.first().after;
    error = saveBatch({retitled}, {aowis::model::ModelEditTransactionCodec::encode(retitle)}, {});
    if (!check(done && error.isEmpty() &&
               !stub.last_event_save.contains(QStringLiteral("membership")),
               "update-only event batch sent a membership field")) return 1;

    // A single MODEL event is allowed to change both the network metadata
    // and a typed member. Neither the GUI nor DVMDB accepts native manifest
    // bytes from the client; the body carries only the member record and its
    // membership intent.
    NetworkHydraulic rootBefore;
    rootBefore.uuid = network_uuid;
    rootBefore.id = QStringLiteral("Network");
    NetworkHydraulic rootAfter = rootBefore;
    rootAfter.title_line_1 = QStringLiteral("Updated with member");
    const QUuid mixed_junction_uuid(PersonalWorkingCopyClient::newSessionUuidV7());
    aowis::model::ModelEditTransaction mixed = creation;
    mixed.transactionId = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    mixed.edits.first().entityId = mixed_junction_uuid;
    mixed.edits.append(aowis::model::ModelEntityEdit{
        network_uuid, QString::fromLatin1(aowis::model::hydraulicRootEditType),
        aowis::model::hydraulicRootImage(rootBefore),
        aowis::model::hydraulicRootImage(rootAfter)});
    const QByteArray mixed_bytes = aowis::model::ModelEditTransactionCodec::encode(mixed);
    if (!check(!mixed_bytes.isEmpty(), "could not encode mixed root/member event")) return 1;
    PersonalWorkingCopyChange mixed_record = created_record;
    mixed_record.uuid = mixed_junction_uuid.toString(QUuid::WithoutBraces);
    HydraulicDraftMembershipEdit mixed_insertion = insertion;
    mixed_insertion.entity_id = mixed_junction_uuid;
    const int before_mixed_save = stub.event_save_count;
    aowis::model::ModelEditTransaction unjournaled = mixed;
    unjournaled.transactionId = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    unjournaled.edits.removeFirst(); // only the root event remains
    error = saveBatch({mixed_record},
        {aowis::model::ModelEditTransactionCodec::encode(unjournaled)}, {});
    if (!check(done && error.contains(QStringLiteral("no matching MODEL entity edit")) &&
               stub.event_save_count == before_mixed_save,
               "root-only event smuggled an unjournaled typed record")) return 1;
    error = saveBatch({mixed_record}, {mixed_bytes}, {mixed_insertion});
    const QJsonArray mixed_records = stub.last_event_save.value(QStringLiteral("changes")).toArray();
    const QJsonArray mixed_membership = stub.last_event_save.value(QStringLiteral("membership")).toArray();
    const QJsonArray mixed_events = stub.last_event_save.value(QStringLiteral("events")).toArray();
    if (!check(done && error.isEmpty() &&
               stub.event_save_count == before_mixed_save + 1 &&
               mixed_records.size() == 1 && mixed_membership.size() == 1 &&
               mixed_events.size() == 1 &&
               mixed_events.first().toString() == QString::fromLatin1(mixed_bytes.toBase64()),
               "mixed root/member event was not sent as one atomic DVMDB request")) return 1;

    done = false;
    client.releaseLease(stub.target(), draft_id, head_id, lease.token,
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && error.isEmpty()
               && stub.release_count == 1, "writer lease was not released")) return 1;

    stub.identity = different_repository;
    done = false;
    client.create(stub.target(), project_id, branch_id, head_id,
        [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty()
               && stub.creating_count == 2,
               "identity change was not rejected before a draft write")) return 1;
    stub.identity = repository_id;
    stub.role = QStringLiteral("shared");
    const int listings_before_shared_role = stub.listing_count;
    done = false;
    client.list(stub.target(), [&](const QList<PersonalWorkingCopyInfo> &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; })
               && error.contains(QStringLiteral("private role"))
               && stub.listing_count == listings_before_shared_role,
               "shared repository role was not rejected before draft access")) return 1;
    return 0;
}
