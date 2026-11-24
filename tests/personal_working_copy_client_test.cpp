#include "projects/model_codec_compatibility.h"
#include "projects/personal_working_copy_client.h"
#include "projects/personal_commit_intent_store.h"
#include "projects/personal_history_navigation_intent_store.h"
#include "projects/dvmdb_identity.h"
#include "projects/personal_edit_queue.h"
#include "projects/personal_draft_edit_session.h"
#include "projects/hydraulic_history_delta.h"
#include "projects/map_annotation_history_delta.h"
#include "network/hydraulic_network_editor.h"
#include "network/hydraulic_edit_transaction_capture.h"
#include "network/map_annotation_layer_id.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_distribution_root_edit.h>
#include <aowis/model/serialization/model_codec.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHash>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QRandomGenerator>
#include <QUrlQuery>
#include <QThread>
#include <QtEndian>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>

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

bool until(QCoreApplication &application, const std::function<bool()> &ready,
           int max_ms = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < max_ms)
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
    QString current_lease_token = lease_token;
    int lease_acquisitions = 0;
    bool rotate_lease_on_reacquire = false;
    int save_count = 0;
    int release_count = 0;
    int generation = 0;
    bool malformed_overlay = false;
    bool corrupt_network_payload = false;
    // First-network reply: 0 = as DVMDB (network root + empty annotation
    // layer root), 1 = network root only, 2 = layer with a wrong UUID,
    // 3 = an extra record, 4 = layer referencing a wrong network,
    // 5 = layer root of a wrong kind.
    int initialize_reply = 0;
    int network_read_count = 0;
    int network_summary_count = 0;
    int annotation_layer_read_count = 0;
    // The draft annotation layer, served in pages of `layer_chunk_size`
    // members per membership chunk (DVMDB: up to 256).
    QList<MapAnnotation> layer_annotations;
    int layer_chunk_size = 256;
    QStringList annotation_layer_paths;
    bool layer_generation_changes_after_first_page = false;
    bool corrupt_layer_next_chunk = false;
    bool duplicate_layer_member_across_pages = false;
    // History replies carry an annotationDelta (D2); with
    // history_annotation_rename the first annotation's text changes.
    bool history_annotation_delta = true;
    bool history_annotation_rename = false;
    QString saved_network_id;
    int archive_page_reads = 0;
    int branch_history_reads = 0;
    bool invalid_branch_history_page = false;
    bool forked_branch_history = false;
    QStringList branch_history_paths;
    bool invalid_archive_page = false;
    QStringList archive_paths;
    int history_cursor_reads = 0;
    int history_reconcile_reads = 0;
    QString history_selected_position = QStringLiteral("0");
    bool corrupt_history_reconcile_cursor = false;
    bool corrupt_history_reconcile_digest = false;
    bool history_network_change = false;
    int discard_history_before_acceptance = 0;
    bool history_selected_archived = false;
    bool history_can_branch_from_archived = false;
    int history_navigation_requests = 0;
    bool history_require_committed_head = false;
    QJsonObject last_history_navigation;
    int network_import_count = 0;
    QJsonObject last_network_import;
    bool drop_next_history_acknowledgement = false;
    int drop_history_acknowledgements = 0;
    bool reject_next_history_navigation = false;
    QJsonObject acknowledged_history_request;
    QJsonObject acknowledged_history_reply;
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
                        // Match DVMDB's current wire contract: every Commit
                        // supplies a logical parent, including an explicit null
                        // for the oldest action checkpoint. Unlike DAG parents,
                        // this link can skip abandoned Redo history.
                        const std::function<QJsonObject(const QString &, const QJsonArray &,
                            const QString &, const QJsonArray &, const QJsonValue &)> commit =
                            [](const QString &id, const QJsonArray &parents,
                               const QString &message, const QJsonArray &actions,
                               const QJsonValue &logical_parent)
                        {
                            return QJsonObject{
                                {QStringLiteral("revision"), QJsonObject{
                                    {QStringLiteral("id"), id},
                                    {QStringLiteral("parents"), parents},
                                    {QStringLiteral("message"), message}}},
                                {QStringLiteral("actionCount"), actions.size()},
                                {QStringLiteral("actions"), actions},
                                {QStringLiteral("logicalParentRevisionId"), logical_parent},
                                {QStringLiteral("checkpointOnly"), actions.isEmpty()},
                                {QStringLiteral("operationId"), QJsonValue(QJsonValue::Null)}};
                        };
                        if (after.isEmpty())
                        {
                            const QJsonObject latest = commit(head_id, QJsonArray{lease_token},
                                QStringLiteral("Latest water edit"), QJsonArray{draft_id},
                                this->forked_branch_history ? QJsonValue(entity_id)
                                                            : QJsonValue(lease_token));
                            const QJsonObject previous = this->forked_branch_history
                                ? commit(entity_id, {}, QStringLiteral("Original layout"), {},
                                         QJsonValue(QJsonValue::Null))
                                : commit(lease_token, QJsonArray{entity_id},
                                         QStringLiteral("Previous checkpoint"), {},
                                         QJsonValue(entity_id));
                            document = QJsonObject{
                                {QStringLiteral("commits"), QJsonArray{latest, previous}},
                                {QStringLiteral("nextCursor"), this->invalid_branch_history_page
                                    ? QJsonValue(draft_id) : this->forked_branch_history
                                        ? QJsonValue(QJsonValue::Null) : QJsonValue(lease_token)},
                                {QStringLiteral("draft"), QJsonValue(QJsonValue::Null)}};
                        }
                        else
                        {
                            document = QJsonObject{
                                {QStringLiteral("commits"), QJsonArray{
                                    commit(entity_id, {}, QStringLiteral("Original layout"), {},
                                           QJsonValue(QJsonValue::Null))}},
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
                        ++this->lease_acquisitions;
                        if (this->rotate_lease_on_reacquire && this->lease_acquisitions > 1)
                            this->current_lease_token = PersonalWorkingCopyClient::newSessionUuidV7();
                        document = QJsonObject{{QStringLiteral("token"), this->current_lease_token},
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
                            {QStringLiteral("baseRevision"), this->history_require_committed_head
                                ? QJsonValue(head_id) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("journalCount"), QString::number(this->saved_events.size())},
                            {QStringLiteral("actionPosition"), this->history_selected_archived
                                ? QStringLiteral("0") : this->history_selected_position},
                            {QStringLiteral("archivedActionPosition"), this->history_selected_archived
                                ? QJsonValue(this->history_selected_position) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("selectionKind"), this->history_selected_archived
                                ? QStringLiteral("archived") : QStringLiteral("draft")},
                            {QStringLiteral("archivedRevisionId"), this->history_selected_archived
                                ? QJsonValue(head_id) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("canBranchFromHistory"),
                                this->history_selected_archived &&
                                this->history_can_branch_from_archived},
                            {QStringLiteral("previousAction"), QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("nextAction"), QJsonObject{
                                {QStringLiteral("selectionKind"), QStringLiteral("draft")},
                                {QStringLiteral("revisionId"), QJsonValue(QJsonValue::Null)},
                                {QStringLiteral("actionPosition"), QStringLiteral("1")}}}};
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/working-copies/") &&
                             request_line.contains("/history/reconcile?"))
                    {
                        ++this->history_reconcile_reads;
                        const QString path = QString::fromLatin1(request_line.split(' ').value(1));
                        const QUrlQuery query(QUrl(QStringLiteral("http://localhost") + path));
                        const QString networkId = query.queryItemValue(QStringLiteral("networkId"));
                        NetworkHydraulicDistribution network;
                        network.uuid = QUuid(networkId);
                        network.id = QStringLiteral("loaded draft");
                        if (this->history_network_change &&
                            !this->acknowledged_history_request.isEmpty())
                            network.title_line_1 = QStringLiteral("navigated");
                        const QJsonObject last = this->acknowledged_history_request;
                        document = QJsonObject{
                            {QStringLiteral("draftId"), draft_id},
                            {QStringLiteral("sessionId"), this->lease_session},
                            {QStringLiteral("networkId"), networkId},
                            {QStringLiteral("networkSha256"), this->corrupt_history_reconcile_digest
                                ? QString(64, QLatin1Char('a')) : HydraulicHistoryDelta::digest(network)},
                            {QStringLiteral("generation"), QString::number(this->generation)},
                            {QStringLiteral("baseRevision"), this->history_require_committed_head
                                ? QJsonValue(head_id) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("journalCount"), QString::number(this->saved_events.size())},
                            {QStringLiteral("selectionKind"), this->history_selected_archived
                                ? QStringLiteral("archived") : QStringLiteral("draft")},
                            {QStringLiteral("revisionId"), this->history_selected_archived
                                ? QJsonValue(head_id) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("actionPosition"), this->corrupt_history_reconcile_cursor
                                ? QStringLiteral("999") : this->history_selected_position},
                            {QStringLiteral("lastNavigationOperationId"), last.isEmpty()
                                ? QJsonValue(QJsonValue::Null) : last.value(QStringLiteral("operationId"))},
                            {QStringLiteral("lastNavigationSourceGeneration"), last.isEmpty()
                                ? QJsonValue(QJsonValue::Null) : last.value(QStringLiteral("expectedGeneration"))}};
                    }
                    else if (request_line.startsWith("POST /dvmdb/v1/working-copies/") &&
                             request_line.contains("/history/navigate "))
                    {
                        ++this->history_navigation_requests;
                        this->last_history_navigation = request_body;
                        if (this->discard_history_before_acceptance > 0)
                        {
                            --this->discard_history_before_acceptance;
                            socket->disconnectFromHost();
                            return;
                        }
                        if (this->reject_next_history_navigation)
                        {
                            this->reject_next_history_navigation = false;
                            status = 409;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("conflict"), QStringLiteral("stale History cursor")));
                        }
                        else if (!this->acknowledged_history_request.isEmpty() &&
                            request_body.value(QStringLiteral("operationId")) ==
                                this->acknowledged_history_request.value(QStringLiteral("operationId")) &&
                            request_body.value(QStringLiteral("sessionId")) ==
                                this->acknowledged_history_request.value(QStringLiteral("sessionId")) &&
                            request_body.value(QStringLiteral("expectedGeneration")) ==
                                this->acknowledged_history_request.value(QStringLiteral("expectedGeneration")) &&
                            request_body.value(QStringLiteral("actionPosition")) ==
                                this->acknowledged_history_request.value(QStringLiteral("actionPosition")) &&
                            request_body.value(QStringLiteral("expectedHead")) ==
                                this->acknowledged_history_request.value(QStringLiteral("expectedHead")) &&
                            request_body.value(QStringLiteral("revisionId")) ==
                                this->acknowledged_history_request.value(QStringLiteral("revisionId")) &&
                            request_body.value(QStringLiteral("networkId")) ==
                                this->acknowledged_history_request.value(QStringLiteral("networkId")) &&
                            request_body.value(QStringLiteral("fencingToken")).toString() ==
                                this->current_lease_token)
                        {
                            // An accepted UUID can be replayed after the same
                            // writer re-leases with a rotated fencing token.
                            document = this->acknowledged_history_reply;
                        }
                        else if (request_body.value(QStringLiteral("expectedGeneration")).toString() !=
                                QString::number(this->generation) ||
                            (this->history_require_committed_head
                                ? request_body.value(QStringLiteral("expectedHead")).toString() != head_id ||
                                  (request_body.contains(QStringLiteral("revisionId")) &&
                                   request_body.value(QStringLiteral("revisionId")).toString() != head_id)
                                : !request_body.value(QStringLiteral("expectedHead")).isNull()) ||
                            request_body.value(QStringLiteral("fencingToken")).toString() !=
                                this->current_lease_token)
                        {
                            status = 409;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("conflict"), QStringLiteral("stale History cursor")));
                        }
                        else
                        {
                            ++this->generation;
                            this->history_selected_archived = request_body.contains(
                                QStringLiteral("revisionId"));
                            this->history_selected_position = request_body.value(
                                QStringLiteral("actionPosition")).toString();
                            document = draftRecord(this->lease_session);
                            // Navigation is a state-preserving fixture. The
                            // protocol must still return a verifiable sparse
                            // delta, rather than making the session fetch a
                            // whole replacement network.
                            const QString requested = request_body.value(
                                QStringLiteral("networkId")).toString();
                            if (!requested.isEmpty())
                            {
                                NetworkHydraulicDistribution network;
                                network.uuid = QUuid(requested);
                                network.id = QStringLiteral("loaded draft");
                                NetworkHydraulicDistribution destination = network;
                                QJsonArray edits;
                                if (this->history_network_change)
                                {
                                    destination.title_line_1 = QStringLiteral("navigated");
                                    edits.append(QJsonObject{
                                        {QStringLiteral("type"), QString::fromLatin1(
                                            aowis::model::hydraulicDistributionRootEditType)},
                                        {QStringLiteral("id"), requested},
                                        {QStringLiteral("beforeBase64"), QString::fromLatin1(
                                            aowis::model::hydraulicDistributionRootImage(network).toBase64())},
                                        {QStringLiteral("afterBase64"), QString::fromLatin1(
                                            aowis::model::hydraulicDistributionRootImage(destination).toBase64())}});
                                }
                                document.insert(QStringLiteral("historyDelta"), QJsonObject{
                                    {QStringLiteral("version"), 2},
                                    {QStringLiteral("networkId"), requested},
                                    {QStringLiteral("fromSha256"),
                                     HydraulicHistoryDelta::orderDigest(network, {})},
                                    {QStringLiteral("toSha256"),
                                     HydraulicHistoryDelta::orderDigest(destination, {})},
                                    {QStringLiteral("orderedTypes"), QJsonArray{}},
                                    {QStringLiteral("edits"), edits},
                                    {QStringLiteral("moves"), QJsonArray{}}});
                                const QUuid layer = distributionMapAnnotationLayerId(QUuid(requested));
                                QJsonArray annotation_edits;
                                if (this->history_annotation_rename && !this->layer_annotations.isEmpty())
                                {
                                    MapAnnotation renamed = this->layer_annotations.first();
                                    renamed.text = renamed.text + QStringLiteral(" (History)");
                                    annotation_edits.append(QJsonObject{
                                        {QStringLiteral("id"), renamed.uuid.toString(QUuid::WithoutBraces)},
                                        {QStringLiteral("beforeBase64"), QString::fromLatin1(
                                            aowis::model::ModelCodec<MapAnnotation>::encode(
                                                this->layer_annotations.first()).toBase64())},
                                        {QStringLiteral("afterBase64"), QString::fromLatin1(
                                            aowis::model::ModelCodec<MapAnnotation>::encode(renamed).toBase64())}});
                                    this->layer_annotations[0] = renamed;
                                }
                                if (this->history_annotation_delta)
                                    document.insert(QStringLiteral("annotationDelta"), QJsonObject{
                                        {QStringLiteral("version"), 1},
                                        {QStringLiteral("layerId"), layer.toString(QUuid::WithoutBraces)},
                                        {QStringLiteral("networkId"), requested},
                                        {QStringLiteral("orderChanged"), false},
                                        {QStringLiteral("fromSha256"),
                                         MapAnnotationHistoryDelta::orderDigest(layer, nullptr)},
                                        {QStringLiteral("toSha256"),
                                         MapAnnotationHistoryDelta::orderDigest(layer, nullptr)},
                                        {QStringLiteral("edits"), annotation_edits},
                                        {QStringLiteral("insertions"), QJsonArray{}},
                                        {QStringLiteral("orderBase64"), QJsonValue(QJsonValue::Null)}});
                            }
                            this->acknowledged_history_request = request_body;
                            this->acknowledged_history_reply = document;
                        }
                    }
                    else if (request_line.startsWith("GET /dvmdb/v1/working-copies/") &&
                             request_line.contains("/annotation-layers/"))
                    {
                        ++this->annotation_layer_read_count;
                        const QByteArray target_path = request_line.split(' ').value(1);
                        this->annotation_layer_paths.append(QString::fromLatin1(target_path));
                        const qsizetype query_start = target_path.indexOf('?');
                        const QByteArray path = query_start < 0 ? target_path : target_path.left(query_start);
                        const QUrlQuery query(query_start < 0 ? QString()
                            : QString::fromLatin1(target_path.mid(query_start + 1)));
                        const QString requested_layer = QString::fromLatin1(
                            path.mid(path.lastIndexOf('/') + 1));
                        const QString expected_layer = distributionMapAnnotationLayerId(
                            QUuid(this->saved_network_id)).toString(QUuid::WithoutBraces);
                        if (this->saved_network_id.isEmpty() ||
                            requested_layer != expected_layer)
                        {
                            status = 404;
                            document.insert(QStringLiteral("error"), dvmdbError(
                                QStringLiteral("not_found"),
                                QStringLiteral("annotation layer not found in working copy")));
                        }
                        else
                        {
                            // Model DVMDB's independent, generation-pinned
                            // annotation layer, read in pages of chunks.
                            const int size = std::max(1, this->layer_chunk_size);
                            const int chunk_count = static_cast<int>(
                                (this->layer_annotations.size() + size - 1) / size);
                            const int from = query.hasQueryItem(QStringLiteral("fromChunk"))
                                ? query.queryItemValue(QStringLiteral("fromChunk")).toInt() : 0;
                            const int limit = query.hasQueryItem(QStringLiteral("chunkLimit"))
                                ? query.queryItemValue(QStringLiteral("chunkLimit")).toInt() : chunk_count;
                            const int next = std::min(chunk_count, from + limit);
                            QJsonArray members;
                            for (qsizetype index = qsizetype(from) * size;
                                 index < qsizetype(next) * size && index < this->layer_annotations.size();
                                 ++index)
                            {
                                const MapAnnotation &item = this->layer_annotations.at(index);
                                members.append(QJsonObject{
                                    {QStringLiteral("uuid"), item.uuid.toString(QUuid::WithoutBraces)},
                                    {QStringLiteral("payloadBase64"), QString::fromLatin1(
                                        aowis::model::ModelCodec<MapAnnotation>::encode(item).toBase64())}});
                            }
                            if (this->duplicate_layer_member_across_pages && from > 0 &&
                                !this->layer_annotations.isEmpty())
                                members.replace(0, QJsonObject{
                                    {QStringLiteral("uuid"), this->layer_annotations.first().uuid
                                        .toString(QUuid::WithoutBraces)},
                                    {QStringLiteral("payloadBase64"), QString::fromLatin1(
                                        aowis::model::ModelCodec<MapAnnotation>::encode(
                                            this->layer_annotations.first()).toBase64())}});
                            document = QJsonObject{
                                {QStringLiteral("draftId"), draft_id},
                                {QStringLiteral("generation"), QString::number(this->generation)},
                                {QStringLiteral("id"), expected_layer},
                                {QStringLiteral("domain"), QStringLiteral("distribution")},
                                {QStringLiteral("ownerNetworkId"), this->saved_network_id},
                                {QStringLiteral("count"), members.size()},
                                {QStringLiteral("chunkCount"), chunk_count},
                                {QStringLiteral("fromChunk"), from},
                                {QStringLiteral("nextChunk"), next < chunk_count
                                    ? QJsonValue(this->corrupt_layer_next_chunk ? next + 1 : next)
                                    : QJsonValue(QJsonValue::Null)},
                                {QStringLiteral("annotations"), members}};
                            if (this->layer_generation_changes_after_first_page) ++this->generation;
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
                        NetworkHydraulicDistribution network;
                        network.uuid = QUuid(network_id);
                        network.id = QStringLiteral("loaded draft");
                        if (this->corrupt_network_payload) network.uuid = QUuid(entity_id);
                        document = QJsonObject{
                            {QStringLiteral("draftId"), draft_id},
                            {QStringLiteral("networkId"), network_id},
                            {QStringLiteral("generation"), QString::number(this->generation)},
                            {QStringLiteral("payloadBase64"), QString::fromLatin1(
                                aowis::model::ModelCodec<NetworkHydraulicDistribution>::encode(network).toBase64())}};
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
                            const QString other_id = QStringLiteral("018f0000-0000-7000-8000-000000000001");
                            const QString layer_id = distributionMapAnnotationLayerId(
                                QUuid(this->initialize_reply == 2 ? other_id : network_id))
                                    .toString(QUuid::WithoutBraces);
                            const QString owner_id = this->initialize_reply == 4 ? other_id : network_id;
                            this->saved_changes = QJsonArray{QJsonObject{
                                {QStringLiteral("uuid"), network_id},
                                {QStringLiteral("kind"), 100},
                                {QStringLiteral("references"), QJsonArray{}},
                                {QStringLiteral("payloadBase64"), QStringLiteral("AQID")}}};
                            // DVMDB lists candidate records in key order; the
                            // layer is listed first here so order is not relied on.
                            if (this->initialize_reply != 1)
                                this->saved_changes.prepend(QJsonObject{
                                    {QStringLiteral("uuid"), layer_id},
                                    {QStringLiteral("kind"), this->initialize_reply == 5 ? 111 : 110},
                                    {QStringLiteral("references"), QJsonArray{owner_id}},
                                    {QStringLiteral("payloadBase64"), QStringLiteral("BAUG")}});
                            if (this->initialize_reply == 3)
                                this->saved_changes.append(QJsonObject{
                                    {QStringLiteral("uuid"), other_id},
                                    {QStringLiteral("kind"), 100},
                                    {QStringLiteral("references"), QJsonArray{}},
                                    {QStringLiteral("payloadBase64"), QStringLiteral("AQID")}});
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
                        const std::optional<NetworkHydraulicDistribution> network =
                            aowis::model::ModelCodec<NetworkHydraulicDistribution>::decode(payload);
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
                        if (request_body.value(QStringLiteral("fencingToken")).toString() !=
                                this->current_lease_token
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
                        request_line.contains("/history/navigate ") &&
                        (this->drop_next_history_acknowledgement ||
                         this->drop_history_acknowledgements > 0) && status == 200)
                    {
                        this->drop_next_history_acknowledgement = false;
                        if (this->drop_history_acknowledgements > 0)
                            --this->drop_history_acknowledgements;
                        socket->disconnectFromHost();
                        return;
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
            {QStringLiteral("baseRevision"), this->history_require_committed_head
                ? QJsonValue(head_id) : QJsonValue(QJsonValue::Null)},
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

// Sparse History transport must apply all changed images and exact membership
// positions without fetching or accepting a full replacement network.
namespace
{
QString historyUuid(const QUuid &id) { return id.toString(QUuid::WithoutBraces); }

QJsonValue historyImage(const std::optional<QByteArray> &image)
{
    return image ? QJsonValue(QString::fromLatin1(image->toBase64())) : QJsonValue(QJsonValue::Null);
}

QJsonObject historyEdit(const QString &type, const QUuid &id,
                        const std::optional<QByteArray> &before,
                        const std::optional<QByteArray> &after)
{
    return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("id"), historyUuid(id)},
                       {QStringLiteral("beforeBase64"), historyImage(before)},
                       {QStringLiteral("afterBase64"), historyImage(after)}};
}

QJsonObject historyJunctionEdit(const std::optional<HydraulicNodeJunction> &before,
                                const std::optional<HydraulicNodeJunction> &after)
{
    const QUuid id = before ? before->uuid : after->uuid;
    return historyEdit(QStringLiteral("HydraulicNodeJunction"), id,
        before ? std::optional<QByteArray>(
                     aowis::model::ModelCodec<HydraulicNodeJunction>::encode(*before))
               : std::nullopt,
        after ? std::optional<QByteArray>(
                    aowis::model::ModelCodec<HydraulicNodeJunction>::encode(*after))
              : std::nullopt);
}

// Independent byte-for-byte rendering of the version 2 digest from
// docs/history-transport.md, so a wrong orderDigest() cannot test itself.
QString historyDigestReference(const NetworkHydraulicDistribution &network, bool with_junctions)
{
    QByteArray bytes("AOWIS-HISTORY-DELTA-2");
    const QByteArray settings = aowis::model::hydraulicDistributionRootImage(network);
    QByteArray length(4, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(settings.size()), length.data());
    bytes.append(length).append(settings);
    if (with_junctions)
    {
        const QByteArray name("HydraulicNodeJunction");
        qToBigEndian<quint32>(static_cast<quint32>(name.size()), length.data());
        bytes.append(length).append(name);
        qToBigEndian<quint32>(static_cast<quint32>(network.nodes_junctions.size()), length.data());
        bytes.append(length);
        for (const HydraulicNodeJunction &junction : network.nodes_junctions)
            bytes.append(junction.uuid.toRfc4122());
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

HydraulicNodeJunction historyJunction(const QString &name)
{
    HydraulicNodeJunction junction;
    junction.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    junction.id = name;
    return junction;
}

QJsonObject historyDeltaV2(const NetworkHydraulicDistribution &source,
                           const NetworkHydraulicDistribution &desired,
                           const QStringList &ordered_types,
                           const QJsonArray &edits, const QJsonArray &moves)
{
    return QJsonObject{
        {QStringLiteral("version"), 2},
        {QStringLiteral("networkId"), historyUuid(source.uuid)},
        {QStringLiteral("fromSha256"), HydraulicHistoryDelta::orderDigest(source, ordered_types)},
        {QStringLiteral("toSha256"), HydraulicHistoryDelta::orderDigest(desired, ordered_types)},
        {QStringLiteral("orderedTypes"), QJsonArray::fromStringList(ordered_types)},
        {QStringLiteral("edits"), edits},
        {QStringLiteral("moves"), moves}};
}
}

bool testVerifiedHistoryDelta()
{
    NetworkHydraulicDistribution source;
    source.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    source.id = QStringLiteral("History network");
    const HydraulicNodeJunction original = historyJunction(QStringLiteral("Original"));
    source.nodes_junctions.append(original);
    const HydraulicNodeJunction added = historyJunction(QStringLiteral("Inserted first"));
    NetworkHydraulicDistribution desired = source;
    desired.title_line_1 = QStringLiteral("Changed root metadata");
    desired.nodes_junctions.prepend(added);
    const QStringList junctions{QStringLiteral("HydraulicNodeJunction")};
    if (!check(HydraulicHistoryDelta::orderDigest(source, junctions) ==
                   historyDigestReference(source, true) &&
               HydraulicHistoryDelta::orderDigest(desired, {}) ==
                   historyDigestReference(desired, false),
               "History v2 digest differs from the documented byte layout")) return false;
    const QJsonObject delta = historyDeltaV2(source, desired, junctions, QJsonArray{
            historyEdit(QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
                        source.uuid, aowis::model::hydraulicDistributionRootImage(source),
                        aowis::model::hydraulicDistributionRootImage(desired)),
            historyJunctionEdit(std::nullopt, added)},
        QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("HydraulicNodeJunction")},
            {QStringLiteral("id"), historyUuid(added.uuid)},
            {QStringLiteral("position"), 0}}});
    NetworkHydraulicDistribution actual;
    QString error;
    if (!check(HydraulicHistoryDelta::apply(delta, source, actual, &error) &&
               HydraulicHistoryDelta::digest(actual) ==
                   HydraulicHistoryDelta::digest(desired) &&
               actual.nodes_junctions.first().uuid == added.uuid &&
               actual.nodes_junctions.last().uuid == original.uuid,
               "History delta did not reconstruct canonical structural/root state")) return false;
    QJsonObject stale = delta;
    stale.insert(QStringLiteral("fromSha256"), QString(64, QLatin1Char('0')));
    actual = source;
    if (!check(!HydraulicHistoryDelta::apply(stale, source, actual, &error) &&
               HydraulicHistoryDelta::digest(actual) ==
                   HydraulicHistoryDelta::digest(source),
               "History delta accepted a stale source or partially mutated local state")) return false;
    QJsonObject wrongImage = delta;
    QJsonArray edits = delta.value(QStringLiteral("edits")).toArray();
    QJsonObject insertion = edits.last().toObject();
    insertion.insert(QStringLiteral("afterBase64"), QStringLiteral("AQID"));
    edits.replace(edits.size() - 1, insertion);
    wrongImage.insert(QStringLiteral("edits"), edits);
    if (!check(!HydraulicHistoryDelta::apply(wrongImage, source, actual, &error) &&
               HydraulicHistoryDelta::digest(actual) ==
                   HydraulicHistoryDelta::digest(source),
               "History delta accepted a malformed entity after-image")) return false;

    // Version 1 (whole-network digest) is no longer accepted.
    QJsonObject version1 = delta;
    version1.remove(QStringLiteral("orderedTypes"));
    version1.insert(QStringLiteral("version"), 1);
    version1.insert(QStringLiteral("fromSha256"), HydraulicHistoryDelta::digest(source));
    version1.insert(QStringLiteral("toSha256"), HydraulicHistoryDelta::digest(desired));
    if (!check(!HydraulicHistoryDelta::apply(version1, source, actual, &error),
               "History delta accepted version 1")) return false;

    // A membership change must be listed in orderedTypes (otherwise its
    // final order would be unchecked), and so must a move.
    QJsonObject unlisted = historyDeltaV2(source, desired, {}, QJsonArray{
        historyJunctionEdit(std::nullopt, added)}, QJsonArray{});
    if (!check(!HydraulicHistoryDelta::apply(unlisted, source, actual, &error),
               "History delta accepted an insertion outside orderedTypes")) return false;
    QJsonObject unlistedMove = historyDeltaV2(source, source, {}, QJsonArray{}, QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("HydraulicNodeJunction")},
                    {QStringLiteral("id"), historyUuid(original.uuid)},
                    {QStringLiteral("position"), 0}}});
    if (!check(!HydraulicHistoryDelta::apply(unlistedMove, source, actual, &error),
               "History delta accepted a move outside orderedTypes")) return false;

    // orderedTypes: known names in canonical order, each once.
    const QList<QJsonArray> badTypes{
        QJsonArray{QStringLiteral("NoSuchCollection")},
        QJsonArray{QStringLiteral("HydraulicNodeJunction"), QStringLiteral("HydraulicNodeJunction")},
        QJsonArray{QStringLiteral("HydraulicLinkPipe"), QStringLiteral("HydraulicNodeJunction")},
        QJsonArray{1}};
    for (const QJsonArray &types : badTypes)
    {
        QJsonObject bad = delta;
        bad.insert(QStringLiteral("orderedTypes"), types);
        if (!check(!HydraulicHistoryDelta::apply(bad, source, actual, &error),
                   "History delta accepted invalid orderedTypes")) return false;
    }

    // A wrong final order is caught by the destination digest.
    QJsonObject wrongOrder = delta;
    wrongOrder.insert(QStringLiteral("moves"), QJsonArray{});
    if (!check(!HydraulicHistoryDelta::apply(wrongOrder, source, actual, &error) &&
               error.contains(QStringLiteral("destination digest")),
               "History delta accepted a wrong final member order")) return false;
    return true;
}

// The shapes DVMDB sends: updates only (collection not listed), and a listed
// collection with deletion + update + insertion + reorder in one delta.
bool testVerifiedHistoryDeltaCollections()
{
    NetworkHydraulicDistribution source;
    source.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    source.id = QStringLiteral("History collections");
    QList<HydraulicNodeJunction> members;
    for (int index = 0; index < 6; ++index)
        members.append(historyJunction(QStringLiteral("J%1").arg(index)));
    source.nodes_junctions = members;
    QString error;
    NetworkHydraulicDistribution actual;

    // Update without membership change: no orderedTypes, no moves.
    NetworkHydraulicDistribution renamed = source;
    renamed.nodes_junctions[2].id = QStringLiteral("J2 renamed");
    const QJsonObject update = historyDeltaV2(source, renamed, {}, QJsonArray{
        historyJunctionEdit(source.nodes_junctions.at(2), renamed.nodes_junctions.at(2))},
        QJsonArray{});
    if (!check(HydraulicHistoryDelta::apply(update, source, actual, &error) &&
               HydraulicHistoryDelta::digest(actual) == HydraulicHistoryDelta::digest(renamed),
               "History delta did not apply an update-only change")) return false;
    // The before-image still protects content outside the digest.
    NetworkHydraulicDistribution drifted = source;
    drifted.nodes_junctions[2].id = QStringLiteral("local drift");
    if (!check(!HydraulicHistoryDelta::apply(update, drifted, actual, &error) &&
               error.contains(QStringLiteral("before-image")),
               "History delta ignored a stale before-image")) return false;

    // Delete J1, update J3, insert N at the front, swap J4/J5.
    const HydraulicNodeJunction inserted = historyJunction(QStringLiteral("N"));
    NetworkHydraulicDistribution desired = source;
    desired.nodes_junctions.removeAt(1);
    desired.nodes_junctions[2].id = QStringLiteral("J3 changed"); // was index 3
    desired.nodes_junctions.prepend(inserted);
    desired.nodes_junctions.move(4, 5);                          // J4 <-> J5
    // Server order: deletions/updates in source order, insertions in
    // destination order; moves after appending, in final index order.
    const QJsonArray edits{
        historyJunctionEdit(source.nodes_junctions.at(1), std::nullopt),
        historyJunctionEdit(source.nodes_junctions.at(3), desired.nodes_junctions.at(3)),
        historyJunctionEdit(std::nullopt, inserted)};
    QList<QUuid> projected;
    for (const HydraulicNodeJunction &junction : source.nodes_junctions)
        if (junction.uuid != members.at(1).uuid) projected.append(junction.uuid);
    projected.append(inserted.uuid);
    QJsonArray moves;
    for (qsizetype index = 0; index < desired.nodes_junctions.size(); ++index)
    {
        const QUuid wanted = desired.nodes_junctions.at(index).uuid;
        if (projected.at(index) == wanted) continue;
        projected.move(projected.indexOf(wanted, index + 1), index);
        moves.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("HydraulicNodeJunction")},
                                 {QStringLiteral("id"), historyUuid(wanted)},
                                 {QStringLiteral("position"), static_cast<int>(index)}});
    }
    const QStringList junctions{QStringLiteral("HydraulicNodeJunction")};
    const QJsonObject mixed = historyDeltaV2(source, desired, junctions, edits, moves);
    if (!check(HydraulicHistoryDelta::apply(mixed, source, actual, &error) &&
               HydraulicHistoryDelta::digest(actual) == HydraulicHistoryDelta::digest(desired),
               "History delta did not apply delete + update + insert + reorder")) return false;

    // Removing a missing member, or inserting an existing one, is refused.
    if (!check(!HydraulicHistoryDelta::apply(historyDeltaV2(desired, desired, junctions,
                   QJsonArray{historyJunctionEdit(members.at(1), std::nullopt)}, QJsonArray{}),
                   desired, actual, &error),
               "History delta removed a missing member")) return false;
    if (!check(!HydraulicHistoryDelta::apply(historyDeltaV2(source, source, junctions,
                   QJsonArray{historyJunctionEdit(std::nullopt, members.at(0))}, QJsonArray{}),
                   source, actual, &error),
               "History delta inserted an existing member")) return false;

    // Duplicate UUIDs in the local collection are refused when touched.
    NetworkHydraulicDistribution duplicate = source;
    duplicate.nodes_junctions.append(duplicate.nodes_junctions.at(2));
    const QJsonObject touchDuplicate = historyDeltaV2(duplicate, duplicate, {}, QJsonArray{
        historyJunctionEdit(duplicate.nodes_junctions.at(2), renamed.nodes_junctions.at(2))},
        QJsonArray{});
    return check(!HydraulicHistoryDelta::apply(touchDuplicate, duplicate, actual, &error) &&
                 error.contains(QStringLiteral("Duplicate")),
                 "History delta updated a duplicated local member");
}

// DVMDB's forward placement (docs/history-transport.md): move each desired
// member in turn from its later index to its final index.
QJsonArray forwardMoves(QList<QUuid> projected, const QList<QUuid> &desired)
{
    QJsonArray moves;
    for (qsizetype index = 0; index < desired.size(); ++index)
    {
        if (projected.at(index) == desired.at(index)) continue;
        projected.move(projected.indexOf(desired.at(index), index + 1), index);
        moves.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("HydraulicNodeJunction")},
                                 {QStringLiteral("id"), historyUuid(desired.at(index))},
                                 {QStringLiteral("position"), static_cast<int>(index)}});
    }
    return moves;
}

QJsonObject junctionMove(const QUuid &id, int position)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("HydraulicNodeJunction")},
                       {QStringLiteral("id"), historyUuid(id)},
                       {QStringLiteral("position"), position}};
}

// D5: moves of one collection are applied in one pass. Every forward
// placement gives the same result as applying its moves one by one; other
// move lists are refused; a large reversal stays fast.
bool testHistoryDeltaForwardMoves()
{
    const QStringList junctions{QStringLiteral("HydraulicNodeJunction")};
    QRandomGenerator random(20261123);
    QString error;
    NetworkHydraulicDistribution actual;
    for (int round = 0; round < 300; ++round)
    {
        NetworkHydraulicDistribution source;
        source.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
        const int count = static_cast<int>(random.bounded(1, 14));
        for (int index = 0; index < count; ++index)
            source.nodes_junctions.append(historyJunction(QStringLiteral("R%1").arg(index)));
        NetworkHydraulicDistribution desired = source;
        const int shuffles = static_cast<int>(random.bounded(0, 4));
        for (int step = 0; step < shuffles; ++step)
            desired.nodes_junctions.move(random.bounded(count), random.bounded(count));
        if (round % 5 == 0)
            for (qsizetype index = desired.nodes_junctions.size() - 1; index > 0; --index)
                desired.nodes_junctions.swapItemsAt(index, random.bounded(static_cast<int>(index) + 1));
        QList<QUuid> from;
        QList<QUuid> to;
        for (const HydraulicNodeJunction &junction : source.nodes_junctions) from.append(junction.uuid);
        for (const HydraulicNodeJunction &junction : desired.nodes_junctions) to.append(junction.uuid);
        const QJsonObject delta = historyDeltaV2(source, desired, junctions, QJsonArray{},
                                                 forwardMoves(from, to));
        if (!check(HydraulicHistoryDelta::apply(delta, source, actual, &error) &&
                   HydraulicHistoryDelta::digest(actual) == HydraulicHistoryDelta::digest(desired),
                   "a forward placement was not applied like its moves one by one")) return false;
    }

    // A, B, C, D: moving B to 2, then A to 3 is valid one by one ([C, B, D, A]),
    // but A already sits before its final position: not a forward placement.
    NetworkHydraulicDistribution source;
    source.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    for (int index = 0; index < 4; ++index)
        source.nodes_junctions.append(historyJunction(QStringLiteral("F%1").arg(index)));
    const QList<HydraulicNodeJunction> &f = source.nodes_junctions;
    NetworkHydraulicDistribution backward = source;
    backward.nodes_junctions = {f.at(2), f.at(1), f.at(3), f.at(0)};
    const std::function<bool(const NetworkHydraulicDistribution &, const QJsonArray &)> refused =
        [&](const NetworkHydraulicDistribution &desired, const QJsonArray &moves)
    {
        NetworkHydraulicDistribution untouched = source;
        const bool applied = HydraulicHistoryDelta::apply(
            historyDeltaV2(source, desired, junctions, QJsonArray{}, moves), source, untouched, &error);
        return !applied && HydraulicHistoryDelta::digest(untouched) == HydraulicHistoryDelta::digest(source);
    };
    NetworkHydraulicDistribution swapped = source;
    swapped.nodes_junctions.swapItemsAt(1, 2);
    if (!check(refused(backward, QJsonArray{junctionMove(f.at(1).uuid, 2), junctionMove(f.at(0).uuid, 3)}) &&
               error.contains(QStringLiteral("forward placement")) &&
               refused(swapped, QJsonArray{junctionMove(f.at(2).uuid, 1), junctionMove(f.at(2).uuid, 2)}) &&
               refused(swapped, QJsonArray{junctionMove(f.at(2).uuid, 2), junctionMove(f.at(3).uuid, 1)}) &&
               refused(swapped, QJsonArray{junctionMove(f.at(2).uuid, 4)}) &&
               refused(swapped, QJsonArray{junctionMove(
                   QUuid(PersonalWorkingCopyClient::newSessionUuidV7()), 1)}),
               "a move list that is not a forward placement was applied")) return false;
    // The valid list for the same swap still works.
    if (!check(HydraulicHistoryDelta::apply(historyDeltaV2(source, swapped, junctions, QJsonArray{},
                   QJsonArray{junctionMove(f.at(2).uuid, 1)}), source, actual, &error) &&
               HydraulicHistoryDelta::digest(actual) == HydraulicHistoryDelta::digest(swapped),
               "a one-move forward placement was refused")) return false;

    // 50,000 members reversed: 49,999 moves. One by one this was quadratic.
    NetworkHydraulicDistribution large;
    large.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    for (int index = 0; index < 50000; ++index)
        large.nodes_junctions.append(historyJunction(QStringLiteral("L%1").arg(index)));
    NetworkHydraulicDistribution reversed = large;
    std::reverse(reversed.nodes_junctions.begin(), reversed.nodes_junctions.end());
    QJsonArray reversal;
    for (qsizetype index = 0; index + 1 < reversed.nodes_junctions.size(); ++index)
        reversal.append(junctionMove(reversed.nodes_junctions.at(index).uuid, static_cast<int>(index)));
    QElapsedTimer timer;
    timer.start();
    const bool reversedOk = HydraulicHistoryDelta::apply(
        historyDeltaV2(large, reversed, junctions, QJsonArray{}, reversal), large, actual, &error);
    const qint64 elapsed = timer.elapsed();
    return check(reversedOk &&
                     HydraulicHistoryDelta::digest(actual) == HydraulicHistoryDelta::digest(reversed) &&
                     elapsed < 10000,
                 "a 50,000-member reversal was not applied in one pass");
}

// An archived selection is writable only after the server verifies that a
// new MODEL edit can safely branch the active History at this coordinate.
// Navigation and cursor refresh must never invent a synthetic MODEL event.
bool testArchivedHistoryCaptureGate(QCoreApplication &application)
{
    Stub stub;
    if (!check(stub.listen(), "archived cursor fixture could not listen")) return false;
    QTemporaryDir root;
    if (!check(root.isValid(), "archived cursor queue root invalid")) return false;
    const QString network_id = PersonalWorkingCopyClient::newSessionUuidV7();
    stub.saved_network_id = network_id;
    stub.history_selected_archived = true;
    stub.history_require_committed_head = true;
    MapAnnotation label;
    label.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    label.text = QStringLiteral("Archived label");
    label.position_wgs84.longitude_deg = 14.25;
    label.position_wgs84.latitude_deg = 11.5;
    stub.layer_annotations.append(label);
    PersonalDraftEditSession session;
    bool opened = false;
    bool navigation_complete = false;
    bool writable_at_navigation_completion = false;
    QString problem;
    QObject::connect(&session, &PersonalDraftEditSession::ready, &session,
                     [&] { opened = true; });
    QObject::connect(&session, &PersonalDraftEditSession::historyNavigationCompleted,
                     &session, [&](const NetworkHydraulicDistribution &)
    {
        navigation_complete = true;
        writable_at_navigation_completion = session.canCaptureHistoryEdits();
    });
    QObject::connect(&session, &PersonalDraftEditSession::blocked, &session,
                     [&](const QString &reason) { problem = reason; });
    session.start(stub.target(), draft_id, network_id, root.path());
    if (!check(until(application, [&] {
                    return session.historyCursor().value(QStringLiteral("selectionKind")).toString()
                        == QStringLiteral("archived") || !problem.isEmpty();
               }) && opened && problem.isEmpty() && session.isReady() &&
               !session.canCaptureHistoryEdits() && session.canNavigateHistory() &&
               stub.annotation_layer_read_count == 1,
               "archived History allowed editing without a verified fork coordinate")) return false;
    // A newly verified archived coordinate is reached through the same
    // public, fenced History navigation used by the editor. Do not bypass the
    // session state machine by calling its private cursor-refresh helper.
    // The fixture contains two archived actions; moving to action 1 must
    // preserve editor capture once the authoritative cursor permits a fork.
    stub.history_can_branch_from_archived = true;
    // First POST reaches DVMDB but the response disappears. The client must
    // retry the exact operation UUID and apply its sparse delta once.
    stub.drop_next_history_acknowledgement = true;
    const int initialNetworkReads = stub.network_read_count;
    const int initialSummaryReads = stub.network_summary_count;
    session.navigateHistoryTo(QJsonObject{
        {QStringLiteral("selectionKind"), QStringLiteral("archived")},
        {QStringLiteral("revisionId"), head_id},
        {QStringLiteral("actionPosition"), QStringLiteral("1")}});
    if (!check(until(application, [&] {
                    return session.canCaptureHistoryEdits() || !problem.isEmpty();
               }) && problem.isEmpty() && session.canCaptureHistoryEdits() &&
               session.historyCursor().value(QStringLiteral("canBranchFromHistory")).toBool() &&
               stub.history_cursor_reads >= 2 && stub.history_navigation_requests == 2 &&
               stub.acknowledged_history_request.value(QStringLiteral("operationId")) ==
                   stub.last_history_navigation.value(QStringLiteral("operationId")) &&
               stub.network_read_count == initialNetworkReads &&
               stub.network_summary_count == initialSummaryReads &&
               stub.annotation_layer_read_count == 1 &&
               session.visibleAnnotations().size() == 1 &&
               session.visibleAnnotations().first().text == label.text &&
               stub.last_history_navigation.value(QStringLiteral("networkId")) == network_id,
               "archived History attempted a full-network, summary or annotation reload")) return false;

    // D2: the annotation changes of the navigation arrive with its reply.
    navigation_complete = false;
    writable_at_navigation_completion = true;
    stub.history_annotation_rename = true;
    session.navigateHistoryTo(QJsonObject{
        {QStringLiteral("selectionKind"), QStringLiteral("draft")},
        {QStringLiteral("revisionId"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("actionPosition"), QStringLiteral("0")}});
    if (!check(until(application, [&] {
                    return (navigation_complete && session.canCaptureHistoryEdits()) ||
                        !problem.isEmpty();
               }) && problem.isEmpty() && session.canCaptureHistoryEdits() &&
               session.historyCursor().value(QStringLiteral("selectionKind")).toString()
                   == QStringLiteral("draft") && !writable_at_navigation_completion &&
               stub.history_navigation_requests == 3 && stub.saved_events.isEmpty() &&
               stub.network_read_count == initialNetworkReads &&
               stub.network_summary_count == initialSummaryReads &&
               stub.annotation_layer_read_count == 1 &&
               session.visibleAnnotations().size() == 1 &&
               session.visibleAnnotations().first().text ==
                   label.text + QStringLiteral(" (History)"),
               "return from archived History reloaded the network or its annotations")) return false;
    // Without an annotationDelta (layer missing at a draft, older DVMDB) the
    // layer is reloaded at the published generation.
    stub.history_annotation_rename = true;
    stub.history_annotation_delta = false;
    navigation_complete = false;
    session.navigateHistoryTo(QJsonObject{
        {QStringLiteral("selectionKind"), QStringLiteral("archived")},
        {QStringLiteral("revisionId"), head_id},
        {QStringLiteral("actionPosition"), QStringLiteral("1")}});
    if (!check(until(application, [&] {
                    return (navigation_complete && session.canCaptureHistoryEdits()) ||
                        !problem.isEmpty();
               }) && problem.isEmpty() &&
               stub.history_navigation_requests == 4 &&
               stub.annotation_layer_read_count == 2 &&
               session.visibleAnnotations().size() == 1 &&
               session.visibleAnnotations().first().text ==
                   label.text + QStringLiteral(" (History) (History)"),
               "History without annotationDelta did not reload the annotation layer")) return false;
    bool stopped = false;
    QObject::connect(&session, &PersonalDraftEditSession::stopped, &session,
                     [&](const QString &) { stopped = true; });
    session.stop();
    return check(until(application, [&] { return stopped; }),
                 "archived cursor fixture could not release its lease");
}

// D1: a large draft annotation layer is read in pages of 39 membership
// chunks, all of one generation, joined in layer order.
bool testAnnotationLayerPages(QCoreApplication &application)
{
    Stub stub;
    if (!check(stub.listen(), "annotation page fixture could not listen")) return false;
    const QString network_id = PersonalWorkingCopyClient::newSessionUuidV7();
    const QUuid network(network_id);
    const QUuid layer = distributionMapAnnotationLayerId(network);
    stub.saved_network_id = network_id;
    stub.generation = 7;
    stub.layer_chunk_size = 2; // 100 members in 50 chunks: two pages
    for (int number = 0; number < 100; ++number)
    {
        MapAnnotation item;
        item.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
        item.text = QStringLiteral("Paged label %1").arg(number);
        item.position_wgs84.longitude_deg = 14.0 + number * 0.001;
        item.position_wgs84.latitude_deg = 11.0;
        stub.layer_annotations.append(item);
    }
    PersonalWorkingCopyClient client;
    const std::function<bool(QList<MapAnnotation> &, QString &, QString &)> load =
        [&](QList<MapAnnotation> &annotations, QString &generation, QString &failure)
    {
        bool done = false;
        client.loadDraftAnnotationLayer(stub.target(), draft_id, layer, network,
            [&](const QList<MapAnnotation> &loaded, const QUuid &, const QString &loaded_generation,
                const QString &error)
        {
            annotations = loaded;
            generation = loaded_generation;
            failure = error;
            done = true;
        });
        return until(application, [&] { return done; });
    };
    QList<MapAnnotation> annotations;
    QString generation;
    QString failure;
    bool same_order = true;
    if (!check(load(annotations, generation, failure) && failure.isEmpty() &&
               generation == QStringLiteral("7") && annotations.size() == 100,
               "paged annotation layer was not read completely")) return false;
    for (qsizetype index = 0; index < annotations.size(); ++index)
        if (annotations.at(index).uuid != stub.layer_annotations.at(index).uuid ||
            annotations.at(index).text != stub.layer_annotations.at(index).text)
            same_order = false;
    if (!check(same_order && stub.annotation_layer_paths.size() == 2 &&
               stub.annotation_layer_paths.at(0).endsWith(
                   QStringLiteral("?fromChunk=0&chunkLimit=39")) &&
               stub.annotation_layer_paths.at(1).endsWith(
                   QStringLiteral("?fromChunk=39&chunkLimit=39")),
               "annotation layer pages were not requested and joined in layer order")) return false;

    // An empty layer is one page.
    stub.annotation_layer_paths.clear();
    const QList<MapAnnotation> members = stub.layer_annotations;
    stub.layer_annotations.clear();
    if (!check(load(annotations, generation, failure) && failure.isEmpty() &&
               annotations.isEmpty() && stub.annotation_layer_paths.size() == 1,
               "an empty annotation layer was not read in one page")) return false;
    stub.layer_annotations = members;

    // A Save between two pages, a forged continuation and a member listed by
    // two pages are refused.
    stub.layer_generation_changes_after_first_page = true;
    if (!check(load(annotations, generation, failure) && !failure.isEmpty() && annotations.isEmpty(),
               "annotation pages of different generations were joined")) return false;
    stub.layer_generation_changes_after_first_page = false;
    stub.corrupt_layer_next_chunk = true;
    if (!check(load(annotations, generation, failure) && !failure.isEmpty(),
               "a forged annotation page continuation was accepted")) return false;
    stub.corrupt_layer_next_chunk = false;
    stub.duplicate_layer_member_across_pages = true;
    if (!check(load(annotations, generation, failure) && !failure.isEmpty(),
               "an annotation listed by two pages was accepted")) return false;
    stub.duplicate_layer_member_across_pages = false;
    return check(load(annotations, generation, failure) && failure.isEmpty() &&
                     annotations.size() == 100,
                 "annotation page fixture did not recover");
}

MapAnnotation deltaAnnotation(const QString &text)
{
    MapAnnotation item;
    item.uuid = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    item.text = text;
    item.position_wgs84.longitude_deg = 13.5;
    item.position_wgs84.latitude_deg = 52.5;
    return item;
}

QJsonValue annotationImage(const std::optional<MapAnnotation> &item)
{
    if (!item) return QJsonValue(QJsonValue::Null);
    return QString::fromLatin1(aowis::model::ModelCodec<MapAnnotation>::encode(*item).toBase64());
}

QJsonObject annotationEdit(const QUuid &id, const std::optional<MapAnnotation> &before,
                           const std::optional<MapAnnotation> &after)
{
    return QJsonObject{{QStringLiteral("id"), id.toString(QUuid::WithoutBraces)},
                       {QStringLiteral("beforeBase64"), annotationImage(before)},
                       {QStringLiteral("afterBase64"), annotationImage(after)}};
}

QJsonObject annotationDeltaV1(const QUuid &layer, const QUuid &network,
                              const QList<MapAnnotation> &source,
                              const QList<MapAnnotation> &desired, bool order_changed,
                              const QJsonArray &edits, const QJsonArray &insertions,
                              const QJsonValue &order = QJsonValue(QJsonValue::Null))
{
    return QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("layerId"), layer.toString(QUuid::WithoutBraces)},
        {QStringLiteral("networkId"), network.toString(QUuid::WithoutBraces)},
        {QStringLiteral("orderChanged"), order_changed},
        {QStringLiteral("fromSha256"),
         MapAnnotationHistoryDelta::orderDigest(layer, order_changed ? &source : nullptr)},
        {QStringLiteral("toSha256"),
         MapAnnotationHistoryDelta::orderDigest(layer, order_changed ? &desired : nullptr)},
        {QStringLiteral("edits"), edits},
        {QStringLiteral("insertions"), insertions},
        {QStringLiteral("orderBase64"), order}};
}

bool sameAnnotations(const QList<MapAnnotation> &first, const QList<MapAnnotation> &second)
{
    if (first.size() != second.size()) return false;
    for (qsizetype index = 0; index < first.size(); ++index)
        if (aowis::model::ModelCodec<MapAnnotation>::encode(first.at(index)) !=
            aowis::model::ModelCodec<MapAnnotation>::encode(second.at(index)))
            return false;
    return true;
}

// D2: annotationDelta version 1 is applied to a copy and verified completely.
bool testVerifiedAnnotationHistoryDelta()
{
    const QUuid network(PersonalWorkingCopyClient::newSessionUuidV7());
    const QUuid layer = distributionMapAnnotationLayerId(network);
    const MapAnnotation a = deltaAnnotation(QStringLiteral("A"));
    const MapAnnotation b = deltaAnnotation(QStringLiteral("B"));
    const MapAnnotation c = deltaAnnotation(QStringLiteral("C"));
    const MapAnnotation d = deltaAnnotation(QStringLiteral("D"));
    MapAnnotation b_renamed = b;
    b_renamed.text = QStringLiteral("B renamed");
    const QList<MapAnnotation> source{a, b, c};

    // Documented digest layout.
    QByteArray bytes("AOWIS-ANNOTATION-DELTA-1");
    bytes.append(layer.toRfc4122());
    const QString unchanged = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    bytes.append(QByteArray::fromHex("00000003"));
    for (const MapAnnotation &item : source) bytes.append(item.uuid.toRfc4122());
    const QString ordered = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (!check(MapAnnotationHistoryDelta::orderDigest(layer, nullptr) == unchanged &&
               MapAnnotationHistoryDelta::orderDigest(layer, &source) == ordered,
               "annotationDelta digest differs from the documented byte layout")) return false;

    QList<MapAnnotation> result;
    QString problem;
    // Update only: order unchanged.
    const QList<MapAnnotation> updated{a, b_renamed, c};
    if (!check(MapAnnotationHistoryDelta::apply(annotationDeltaV1(layer, network, source, updated, false,
                   QJsonArray{annotationEdit(b.uuid, b, b_renamed)}, QJsonArray{}),
                   layer, network, source, result, &problem) &&
               sameAnnotations(result, updated),
               "annotationDelta update was not applied")) return false;
    // Deletion, creation at a position, update.
    const QList<MapAnnotation> mixed{a, d, b_renamed};
    if (!check(MapAnnotationHistoryDelta::apply(annotationDeltaV1(layer, network, source, mixed, true,
                   QJsonArray{annotationEdit(b.uuid, b, b_renamed),
                              annotationEdit(c.uuid, c, std::nullopt),
                              annotationEdit(d.uuid, std::nullopt, d)},
                   QJsonArray{QJsonObject{{QStringLiteral("id"), d.uuid.toString(QUuid::WithoutBraces)},
                                          {QStringLiteral("position"), 1}}}),
                   layer, network, source, result, &problem) &&
               sameAnnotations(result, mixed),
               "annotationDelta deletion/insertion/update was not applied")) return false;
    // Members that stay change their relative order: the complete order.
    const QList<MapAnnotation> reordered{b, c, a, d};
    QByteArray order;
    for (const MapAnnotation &item : reordered) order.append(item.uuid.toRfc4122());
    const QJsonObject reorder = annotationDeltaV1(layer, network, source, reordered, true,
        QJsonArray{annotationEdit(d.uuid, std::nullopt, d)}, QJsonArray{},
        QString::fromLatin1(order.toBase64()));
    if (!check(MapAnnotationHistoryDelta::apply(reorder, layer, network, source, result, &problem) &&
               sameAnnotations(result, reordered),
               "annotationDelta complete order was not applied")) return false;

    // Refusals: the result must stay untouched.
    const QList<MapAnnotation> untouched = result;
    const std::function<bool(const QJsonObject &, const QList<MapAnnotation> &)> refused =
        [&](const QJsonObject &delta, const QList<MapAnnotation> &from)
    {
        QString message;
        const bool applied = MapAnnotationHistoryDelta::apply(delta, layer, network, from,
                                                              result, &message);
        return !applied && !message.isEmpty() && sameAnnotations(result, untouched);
    };
    QJsonObject stale = annotationDeltaV1(layer, network, source, updated, false,
        QJsonArray{annotationEdit(b.uuid, b_renamed, b)}, QJsonArray{});
    QJsonObject wrongSource = annotationDeltaV1(layer, network, QList<MapAnnotation>{b, a, c},
        QList<MapAnnotation>{a, b}, true,
        QJsonArray{annotationEdit(c.uuid, c, std::nullopt)}, QJsonArray{});
    QJsonObject wrongDestination = annotationDeltaV1(layer, network, source, reordered, true,
        QJsonArray{annotationEdit(c.uuid, c, std::nullopt)}, QJsonArray{});
    QJsonObject unlisted = annotationDeltaV1(layer, network, source, mixed, false,
        QJsonArray{annotationEdit(c.uuid, c, std::nullopt)}, QJsonArray{});
    QJsonObject missingPosition = annotationDeltaV1(layer, network, source, QList<MapAnnotation>{a, b, c, d},
        true, QJsonArray{annotationEdit(d.uuid, std::nullopt, d)}, QJsonArray{});
    QJsonObject existingCreation = annotationDeltaV1(layer, network, source, source, true,
        QJsonArray{annotationEdit(a.uuid, std::nullopt, a)},
        QJsonArray{QJsonObject{{QStringLiteral("id"), a.uuid.toString(QUuid::WithoutBraces)},
                               {QStringLiteral("position"), 0}}});
    QJsonObject duplicateEdit = annotationDeltaV1(layer, network, source, updated, false,
        QJsonArray{annotationEdit(b.uuid, b, b_renamed), annotationEdit(b.uuid, b, b_renamed)},
        QJsonArray{});
    QByteArray shortOrder = order.left(48);
    QJsonObject badOrder = annotationDeltaV1(layer, network, source, reordered, true,
        QJsonArray{annotationEdit(d.uuid, std::nullopt, d)}, QJsonArray{},
        QString::fromLatin1(shortOrder.toBase64()));
    QJsonObject bothOrders = reorder;
    bothOrders.insert(QStringLiteral("insertions"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), d.uuid.toString(QUuid::WithoutBraces)}, {QStringLiteral("position"), 3}}});
    QJsonObject otherLayer = reorder;
    otherLayer.insert(QStringLiteral("layerId"),
                      QUuid(PersonalWorkingCopyClient::newSessionUuidV7()).toString(QUuid::WithoutBraces));
    QJsonObject extraField = reorder;
    extraField.insert(QStringLiteral("moves"), QJsonArray{});
    QJsonObject otherVersion = reorder;
    otherVersion.insert(QStringLiteral("version"), 2);
    return check(refused(stale, source) && refused(wrongSource, source) &&
                     refused(wrongDestination, source) && refused(unlisted, source) &&
                     refused(missingPosition, source) && refused(existingCreation, source) &&
                     refused(duplicateEdit, source) && refused(badOrder, source) &&
                     refused(bothOrders, source) && refused(otherLayer, source) &&
                     refused(extraField, source) && refused(otherVersion, source) &&
                     refused(reorder, QList<MapAnnotation>{a, a, b, c}),
                 "an inconsistent annotationDelta was applied");
}

// After three lost acknowledgements the server and GUI already hold the
// same canonical MODEL bytes. The SHA-256 receipt proves the operation UUID,
// generation and archived cursor; no sparse replay is necessary. The lease
// rotates without a full-network GET.
bool testHistoryReconnectAfterLostReplies(QCoreApplication &application)
{
    Stub stub;
    if (!check(stub.listen(), "History reconnect fixture could not listen")) return false;
    QTemporaryDir root;
    if (!check(root.isValid(), "History reconnect queue root invalid")) return false;
    const QString network_id = PersonalWorkingCopyClient::newSessionUuidV7();
    stub.saved_network_id = network_id;
    stub.history_selected_archived = true;
    stub.history_require_committed_head = true;
    stub.history_can_branch_from_archived = true;
    stub.rotate_lease_on_reacquire = true;
    PersonalDraftEditSession session;
    QString problem;
    int recovered = 0;
    int completed = 0;
    QObject::connect(&session, &PersonalDraftEditSession::blocked,
                     &session, [&](const QString &error) { problem = error; });
    QObject::connect(&session, &PersonalDraftEditSession::historyNavigationRecovering,
                     &session, [&] { ++recovered; });
    QObject::connect(&session, &PersonalDraftEditSession::historyNavigationCompleted,
                     &session, [&](const NetworkHydraulicDistribution &) { ++completed; });
    session.start(stub.target(), draft_id, network_id, root.path());
    if (!check(until(application, [&] {
            return session.canNavigateHistory() || !problem.isEmpty();
        }) && problem.isEmpty(), "reconnect fixture could not open History")) return false;
    const int original_reads = stub.network_read_count;
    const int original_summaries = stub.network_summary_count;
    stub.drop_history_acknowledgements = 3;
    session.navigateHistoryTo(QJsonObject{
        {QStringLiteral("selectionKind"), QStringLiteral("archived")},
        {QStringLiteral("revisionId"), head_id},
        {QStringLiteral("actionPosition"), QStringLiteral("1")}});
    if (!check(until(application, [&] {
            return (completed == 1 && session.canCaptureHistoryEdits()) ||
                !problem.isEmpty();
        }, 10000) && problem.isEmpty() && recovered == 1 && completed == 1 &&
        stub.history_navigation_requests == 3 && stub.history_reconcile_reads == 1 &&
        stub.lease_acquisitions == 2 &&
        stub.acknowledged_history_request.value(QStringLiteral("operationId")) ==
            stub.last_history_navigation.value(QStringLiteral("operationId")) &&
        stub.acknowledged_history_request.value(QStringLiteral("expectedGeneration")) ==
            stub.last_history_navigation.value(QStringLiteral("expectedGeneration")) &&
        stub.last_history_navigation.value(QStringLiteral("fencingToken")).toString() ==
            lease_token && stub.current_lease_token != lease_token &&
        stub.generation == 1 && stub.network_read_count == original_reads &&
        stub.network_summary_count == original_summaries &&
        session.currentGeneration() == QStringLiteral("1"),
        "reconnecting History did not preserve UUID/generation and avoid full reload")) return false;

    // A server conflict is definitive. It MUST NOT be turned into a
    // reconnect loop that could replay an unrelated navigation.
    stub.reject_next_history_navigation = true;
    session.navigateHistoryTo(QJsonObject{
        {QStringLiteral("selectionKind"), QStringLiteral("draft")},
        {QStringLiteral("revisionId"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("actionPosition"), QStringLiteral("0")}});
    if (!check(until(application, [&] { return !problem.isEmpty(); }) &&
               session.isBlocked() && recovered == 1 &&
               stub.history_navigation_requests == 4,
               "definitive History conflict incorrectly entered reconnect recovery")) return false;
    bool stopped = false;
    QString stop_error;
    QObject::connect(&session, &PersonalDraftEditSession::stopped,
                     &session, [&](const QString &error) { stopped = true; stop_error = error; });
    session.stop();
    return check(until(application, [&] { return stopped; }) && stop_error.isEmpty(),
                 "reconnect fixture could not stop the session");
}

// A lost request can also have failed BEFORE DVMDB published anything. The
// receipt then reports the original generation and cursor: resend the same
// UUID exactly once when connectivity recovers. Never infer success from a
// matching network hash if the cursor or operation identity disagrees.
bool testHistoryReconciliationSourceAndDivergence(QCoreApplication &application)
{
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        Stub stub;
        if (!check(stub.listen(), "reconciliation edge fixture failed to listen")) return false;
        QTemporaryDir root;
        if (!check(root.isValid(), "reconciliation edge queue invalid")) return false;
        const QString network_id = PersonalWorkingCopyClient::newSessionUuidV7();
        stub.saved_network_id = network_id;
        stub.history_selected_archived = true;
        stub.history_require_committed_head = true;
        stub.history_can_branch_from_archived = true;
        stub.rotate_lease_on_reacquire = true;
        if (scenario == 0) stub.discard_history_before_acceptance = 3;
        else
        {
            stub.drop_history_acknowledgements = 3;
            stub.corrupt_history_reconcile_cursor = scenario == 1;
            stub.corrupt_history_reconcile_digest = scenario == 2;
        }
        PersonalDraftEditSession session;
        QString problem;
        int completed = 0;
        QObject::connect(&session, &PersonalDraftEditSession::blocked,
                         &session, [&](const QString &error) { problem = error; });
        QObject::connect(&session, &PersonalDraftEditSession::historyNavigationCompleted,
                         &session, [&](const NetworkHydraulicDistribution &) { ++completed; });
        session.start(stub.target(), draft_id, network_id, root.path());
        if (!check(until(application, [&] {
                return session.canNavigateHistory() || !problem.isEmpty();
            }) && problem.isEmpty(), "reconciliation edge fixture cannot start")) return false;
        const int fullReads = stub.network_read_count;
        session.navigateHistoryTo(QJsonObject{
            {QStringLiteral("selectionKind"), QStringLiteral("archived")},
            {QStringLiteral("revisionId"), head_id},
            {QStringLiteral("actionPosition"), QStringLiteral("1")}});
        if (scenario == 0)
        {
            if (!check(until(application, [&] {
                    return (completed == 1 && session.canCaptureHistoryEdits()) ||
                        !problem.isEmpty();
                }, 10000) && problem.isEmpty() &&
                stub.history_reconcile_reads == 1 &&
                stub.history_navigation_requests == 4 && stub.generation == 1 &&
                stub.lease_acquisitions == 2 &&
                stub.network_read_count == fullReads &&
                stub.acknowledged_history_request.value(QStringLiteral("operationId")) ==
                    stub.last_history_navigation.value(QStringLiteral("operationId")),
                "unpublished navigation did not safely retry its original UUID")) return false;
        }
        else
        {
            if (!check(until(application, [&] { return !problem.isEmpty(); }, 10000) &&
                session.isBlocked() && completed == 0 &&
                stub.history_reconcile_reads == 1 &&
                stub.history_navigation_requests == (scenario == 1 ? 3 : 4) &&
                stub.lease_acquisitions == (scenario == 1 ? 1 : 2) &&
                stub.network_read_count == fullReads,
                "reconciliation mismatch incorrectly authorized History publication")) return false;
        }
        bool stopped = false;
        QObject::connect(&session, &PersonalDraftEditSession::stopped,
                         &session, [&](const QString &) { stopped = true; });
        session.stop();
        if (!check(until(application, [&] { return stopped; }),
                   "reconciliation edge fixture could not stop")) return false;
    }
    return true;
}

// Crash/restart must reuse the exact persisted UUID and canonical MODEL
// source. Neither a published Undo nor a request lost before publication may
// cause a NetworkHydraulicDistribution GET, even when the server was unavailable across
// process termination. The intent is removed only after verified completion.
bool testHistoryRestartAfterLostReply(QCoreApplication &application)
{
    for (int scenario = 0; scenario < 5; ++scenario)
    {
        Stub stub;
        if (!check(stub.listen(), "restart History fixture failed to listen")) return false;
        QTemporaryDir root;
        if (!check(root.isValid(), "restart History queue root invalid")) return false;
        const QString network_id = PersonalWorkingCopyClient::newSessionUuidV7();
        stub.saved_network_id = network_id;
        stub.history_selected_archived = true;
        stub.history_require_committed_head = true;
        stub.history_can_branch_from_archived = true;
        stub.rotate_lease_on_reacquire = true;
        stub.history_network_change = scenario == 3;
        if (scenario == 1) stub.discard_history_before_acceptance = 3;
        else stub.drop_history_acknowledgements = 3;
        QString originalOperation;
        QString originalGeneration;
        int baselineReads = 0;
        int baselineSummaries = 0;
        {
            PersonalDraftEditSession original;
            QString error;
            int recoveries = 0;
            QObject::connect(&original, &PersonalDraftEditSession::blocked,
                             &original, [&](const QString &message) { error = message; });
            QObject::connect(&original, &PersonalDraftEditSession::historyNavigationRecovering,
                             &original, [&] { ++recoveries; });
            original.start(stub.target(), draft_id, network_id, root.path());
            if (!check(until(application, [&] {
                    return original.canNavigateHistory() || !error.isEmpty();
                }) && error.isEmpty(), "restart History initial open failed")) return false;
            baselineReads = stub.network_read_count;
            baselineSummaries = stub.network_summary_count;
            original.navigateHistoryTo(QJsonObject{
                {QStringLiteral("selectionKind"), QStringLiteral("archived")},
                {QStringLiteral("revisionId"), head_id},
                {QStringLiteral("actionPosition"), QStringLiteral("1")}});
            if (!check(until(application, [&] { return recoveries == 1 || !error.isEmpty(); }, 6000)
                       && error.isEmpty(), "restart fixture did not reach uncertain navigation"))
                return false;
            PersonalHistoryNavigationIntentStore store(root.path());
            PersonalHistoryNavigationIntent saved;
            QString diskError;
            if (!check(store.read(repository_id, draft_id, saved, &diskError) ==
                       PersonalHistoryNavigationIntentStore::ReadResult::Found && diskError.isEmpty()
                       && saved.network_id == network_id && saved.source_generation == QStringLiteral("0")
                       && HydraulicHistoryDelta::digest(saved.source_network) ==
                          HydraulicHistoryDelta::digest(original.visibleNetwork()),
                       "History restart intent was not durably stored before request")) return false;
            originalOperation = saved.operation_id;
            originalGeneration = saved.source_generation;
        }
        if (scenario == 2)
        {
            // A damaged checkpoint must fail closed instead of treating it as
            // absent and silently hydrating a full network.
            const QString filePath = root.path() + QLatin1Char('/') + repository_id +
                QLatin1Char('/') + draft_id + QStringLiteral(".history-intent.json");
            QFile file(filePath);
            if (!check(file.open(QIODevice::Append) && file.write("garbage") == 7,
                       "cannot corrupt History restart fixture")) return false;
            file.close();
            PersonalDraftEditSession blocked;
            QString error;
            QObject::connect(&blocked, &PersonalDraftEditSession::blocked,
                             &blocked, [&](const QString &message) { error = message; });
            blocked.start(stub.target(), draft_id, network_id, root.path());
            if (!check(blocked.isBlocked() && error.contains(QStringLiteral("intent")) &&
                       stub.network_read_count == baselineReads &&
                       stub.history_reconcile_reads == 0,
                       "corrupted restart intent was ignored or fetched full model")) return false;
            continue;
        }
        if (scenario == 4)
        {
            // The SHA matches but the server claims a different History
            // cursor. It must not accept the result or erase the intent.
            stub.corrupt_history_reconcile_cursor = true;
            PersonalDraftEditSession rejected;
            QString error;
            QObject::connect(&rejected, &PersonalDraftEditSession::blocked,
                             &rejected, [&](const QString &message) { error = message; });
            rejected.start(stub.target(), draft_id, network_id, root.path());
            if (!check(until(application, [&] { return !error.isEmpty(); }, 10000) &&
                       rejected.isBlocked() &&
                       stub.network_read_count == baselineReads,
                       "divergent restart cursor was silently accepted")) return false;
            PersonalHistoryNavigationIntentStore store(root.path());
            PersonalHistoryNavigationIntent pending;
            if (!check(store.read(repository_id, draft_id, pending) ==
                       PersonalHistoryNavigationIntentStore::ReadResult::Found &&
                       pending.operation_id == originalOperation,
                       "divergent restart erased its recovery intent")) return false;
            stub.corrupt_history_reconcile_cursor = false;
        }
        PersonalDraftEditSession restarted;
        QString error;
        int readyCount = 0;
        QObject::connect(&restarted, &PersonalDraftEditSession::ready,
                         &restarted, [&] { ++readyCount; });
        QObject::connect(&restarted, &PersonalDraftEditSession::blocked,
                         &restarted, [&](const QString &message) { error = message; });
        restarted.start(stub.target(), draft_id, network_id, root.path());
        if (!check(until(application, [&] {
                return (readyCount == 1 && restarted.canCaptureHistoryEdits()) || !error.isEmpty();
            }, 10000) && error.isEmpty() && readyCount == 1 &&
            stub.network_read_count == baselineReads &&
            stub.network_summary_count == baselineSummaries &&
            stub.history_reconcile_reads == (scenario == 4 ? 2 : 1) &&
            stub.generation == 1 &&
            restarted.currentGeneration() == QStringLiteral("1") &&
            restarted.visibleNetwork().title_line_1 == (scenario == 3
                ? QStringLiteral("navigated") : QString()) &&
            stub.last_history_navigation.value(QStringLiteral("operationId")).toString() == originalOperation &&
            stub.last_history_navigation.value(QStringLiteral("expectedGeneration")).toString() ==
                originalGeneration &&
            (scenario != 1 || stub.history_navigation_requests == 4),
            "GUI restart did not reconcile the same Undo without full-network download"))
            return false;
        PersonalHistoryNavigationIntentStore store(root.path());
        PersonalHistoryNavigationIntent saved;
        if (!check(store.read(repository_id, draft_id, saved) ==
                   PersonalHistoryNavigationIntentStore::ReadResult::Missing,
                   "acknowledged History restart intent was not removed")) return false;
        bool stopped = false;
        QObject::connect(&restarted, &PersonalDraftEditSession::stopped,
                         &restarted, [&](const QString &) { stopped = true; });
        restarted.stop();
        if (!check(until(application, [&] { return stopped; }),
                   "restarted History session could not stop")) return false;
    }
    return true;
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
    stub.saved_network_id = network_id;
    PersonalDraftEditSession session;
    bool opened = false;
    bool editable_before_cursor = false;
    QString problem;
    QObject::connect(&session, &PersonalDraftEditSession::ready,
                     &session, [&]
    {
        opened = true;
        editable_before_cursor = session.canCaptureHistoryEdits();
    });
    QObject::connect(&session, &PersonalDraftEditSession::blocked,
                     &session, [&](const QString &message) { problem = message; });
    session.start(stub.target(), draft_id, network_id, root.path());
    if (!check(until(application, [&] { return opened || !problem.isEmpty(); }) &&
               opened && problem.isEmpty() && session.isReady() &&
               session.currentGeneration() == QStringLiteral("0") &&
               session.pendingCount() == 0 &&
               session.visibleNetwork().uuid == QUuid(network_id) &&
               !editable_before_cursor,
               "session enabled capture before the authoritative History cursor")) return false;
    if (!check(until(application, [&] {
                    return session.canCaptureHistoryEdits() || !problem.isEmpty();
               }) && session.canCaptureHistoryEdits() && problem.isEmpty() &&
               stub.history_cursor_reads >= 1,
               "session did not authorize edit capture from the live draft cursor")) return false;

    NetworkHydraulicDistribution live = session.visibleNetwork();
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
    rejected.saved_network_id = failed_network;
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
    if (!check(until(application, [&] { return first_attempt.canCaptureHistoryEdits(); }),
               "rejected Save fixture did not verify a writable draft cursor")) return false;
    NetworkHydraulicDistribution retry_live = first_attempt.visibleNetwork();
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
    // The commit author identity is a GUI preference in the AOWIS-CONFIG
    // store; never read or write the developer's real configuration.
    QTemporaryDir configuration_root;
    if (!check(configuration_root.isValid(), "temporary configuration root unavailable")) return 1;
    qputenv("XDG_CONFIG_HOME", configuration_root.path().toUtf8());
    qputenv("XDG_DATA_HOME", configuration_root.path().toUtf8());
    QCoreApplication application(argc, argv);
    if (!testVerifiedHistoryDelta()) return 1;
    if (!testVerifiedHistoryDeltaCollections()) return 1;
    if (!testHistoryDeltaForwardMoves()) return 1;
    if (!testVerifiedAnnotationHistoryDelta()) return 1;
    if (!testAnnotationLayerPages(application)) return 1;
    if (!testArchivedHistoryCaptureGate(application)) return 1;
    if (!testHistoryReconnectAfterLostReplies(application)) return 1;
    if (!testHistoryReconciliationSourceAndDivergence(application)) return 1;
    if (!testHistoryRestartAfterLostReply(application)) return 1;
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
    // A new edit after archived Undo can skip a committed checkpoint in
    // the logical History while leaving its immutable DAG parent intact.
    history_stub.forked_branch_history = true;
    history_done = false;
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
               branch_commits.size() == 2 && branch_next.isEmpty() &&
               branch_commits.first().value(QStringLiteral("id")).toString() == head_id &&
               branch_commits.first().value(QStringLiteral("firstParent")).toString() == entity_id &&
               branch_commits.last().value(QStringLiteral("id")).toString() == entity_id &&
               branch_commits.last().value(QStringLiteral("firstParent")).toString().isEmpty(),
               "branch History resurrected an abandoned Redo checkpoint")) return 1;
    history_stub.forked_branch_history = false;
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
    const QString initial_layer_id = distributionMapAnnotationLayerId(QUuid(network_id))
        .toString(QUuid::WithoutBraces);
    if (!check(until(application, [&] { return initialized; }) && init_error.isEmpty() &&
               initial_draft.generation == QStringLiteral("1") &&
               initial_draft.changes.size() == 2 &&
               std::any_of(initial_draft.changes.cbegin(), initial_draft.changes.cend(),
                   [&](const PersonalWorkingCopyChange &change)
                   { return change.uuid == network_id && change.kind == 100; }) &&
               std::any_of(initial_draft.changes.cbegin(), initial_draft.changes.cend(),
                   [&](const PersonalWorkingCopyChange &change)
                   { return change.uuid == initial_layer_id && change.kind == 110; }) &&
               initial_draft.events.isEmpty(),
               "GUI first hydraulic network initialization refused the DVMDB reply "
               "(network root + empty annotation layer)")) return 1;
    // Any other first-network reply is still refused.
    for (int reply = 1; reply <= 5; ++reply)
    {
        Stub bad_init;
        if (!check(bad_init.listen(), "could not start first-network reply fixture")) return 1;
        bad_init.lease_session = init_session;
        bad_init.initialize_reply = reply;
        bool refused_done = false;
        QString refused_error;
        client.initializeEmptyNetwork(bad_init.target(), draft_id, init_session, lease_token,
            QStringLiteral("0"), network_id, QStringLiteral("water network"),
            [&](const PersonalWorkingCopyInfo &, const QString &failure)
            { refused_error = failure; refused_done = true; });
        if (!check(until(application, [&] { return refused_done; }) &&
                   refused_error.startsWith(QStringLiteral("Invalid first-network draft response")),
                   "GUI accepted a malformed first-network reply")) return 1;
    }
    initialized = false;
    NetworkHydraulicDistribution loaded_network;
    QString loaded_generation;
    client.loadDraftNetwork(bootstrap.target(), draft_id, network_id,
        [&](const NetworkHydraulicDistribution &network, const QString &generation, const QString &failure)
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
        [&](const NetworkHydraulicDistribution &, const QString &, const QString &failure)
        { init_error = failure; initialized = true; });
    if (!check(until(application, [&] { return initialized; }) && !init_error.isEmpty() &&
               bootstrap.network_read_count == 2,
               "GUI draft hydration accepted wrong network UUID")) return 1;
    bootstrap.corrupt_network_payload = false;
    initialized = false;
    client.loadDraftNetwork(bootstrap.target(), draft_id, QStringLiteral("not-a-uuid"),
        [&](const NetworkHydraulicDistribution &, const QString &, const QString &failure)
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
    NetworkHydraulicDistribution imported_network;
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

    // The source is read only through the transport Projects passes (the
    // source's WireGuard connection). A closed connection fails the
    // checkout and is never replaced by a direct request.
    int transported = 0;
    const int provisions_before_transport = stub.provision_count;
    QNetworkAccessManager source_manager;
    client.setSourceTransport([&](const QNetworkRequest &request) -> QNetworkReply *
    {
        ++transported;
        return source_manager.get(request);
    });
    done = false;
    // An unexpected source UUID: rejected after exactly the identity GET.
    client.checkoutBranchPrivately(stub.target(), source.target().endpoint,
        DvmdbIdentity::newUuidV7(), project_id, QStringLiteral("Network test"), branch_id, head_id,
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(until(application, [&] { return done; }) && !error.isEmpty() && transported == 1
                   && stub.provision_count == provisions_before_transport,
               "checkout did not read the source through the given transport")) return 1;
    const int exports_before_closed = source.bundle_export_count;
    client.setSourceTransport([&](const QNetworkRequest &) -> QNetworkReply *
    {
        ++transported;
        return nullptr; // e.g. the source's WireGuard tunnel is down
    });
    done = false;
    client.checkoutBranchPrivately(stub.target(), source.target().endpoint,
        different_repository, project_id, QStringLiteral("Network test"), branch_id, head_id,
        [&](const QString &failure) { error = failure; done = true; });
    if (!check(done && error.contains(QStringLiteral("WireGuard")) && transported == 2
                   && source.bundle_export_count == exports_before_closed
                   && stub.provision_count == provisions_before_transport,
               "closed source connection did not fail the checkout")) return 1;
    client.setSourceTransport({});

    // A personal repository configured for WireGuard is never reached
    // directly: in this build (no embedded engine) every request fails.
    PersonalRepositoryTarget tunnelled = stub.target();
    tunnelled.wireguard_profile_id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6399");
    const int creating_before_tunnel = stub.creating_count;
    done = false;
    client.create(tunnelled, project_id, branch_id, head_id,
        [&](const PersonalWorkingCopyInfo &, const QString &failure)
    {
        error = failure;
        done = true;
    });
    if (!check(until(application, [&] { return done; }) && error.contains(QStringLiteral("WireGuard"))
                   && stub.creating_count == creating_before_tunnel,
               "WireGuard personal repository was reached directly")) return 1;

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
    created_record.kind = 201;
    created_record.payload = *creation.edits.first().after;
    HydraulicDraftMembershipEdit insertion;
    insertion.network_id = network_uuid;
    insertion.entity_id = junction_uuid;
    insertion.kind = 201;
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
               sent_intent.value(QStringLiteral("kind")).toInt() == 201 &&
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
    NetworkHydraulicDistribution rootBefore;
    rootBefore.uuid = network_uuid;
    rootBefore.id = QStringLiteral("Network");
    NetworkHydraulicDistribution rootAfter = rootBefore;
    rootAfter.title_line_1 = QStringLiteral("Updated with member");
    const QUuid mixed_junction_uuid(PersonalWorkingCopyClient::newSessionUuidV7());
    aowis::model::ModelEditTransaction mixed = creation;
    mixed.transactionId = QUuid(PersonalWorkingCopyClient::newSessionUuidV7());
    mixed.edits.first().entityId = mixed_junction_uuid;
    mixed.edits.append(aowis::model::ModelEntityEdit{
        network_uuid, QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
        aowis::model::hydraulicDistributionRootImage(rootBefore),
        aowis::model::hydraulicDistributionRootImage(rootAfter)});
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
