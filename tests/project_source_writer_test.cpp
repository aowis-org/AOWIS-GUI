#include "projects/project_source_writer.h"
#include "projects/dvmdb_identity.h"
#include "projects/model_codec_compatibility.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>
#include <functional>
#include <memory>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

bool waitUntil(QCoreApplication &app, const std::function<bool()> &predicate)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 5000)
        app.processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTcpServer server;
    if (!require(server.listen(QHostAddress::LocalHost, 0), "cannot listen on loopback")) return 1;
    QStringList requested_paths;
    QStringList pinned_headers;
    int identity_lookups = 0;
    const QString repository_id = QStringLiteral("018f84c0-0000-7000-8000-0000000000ac");
    QString observed_id = repository_id;
    QString observed_role = QStringLiteral("shared");
    QString observed_fingerprint = ModelCodecCompatibility::fingerprint();
    QList<QJsonObject> request_bodies;
    const QString root_revision = QStringLiteral("018f84c0-0000-7000-8000-000000000001");
    const QString branch_revision = QStringLiteral("018f84c0-0000-7000-8000-000000000002");
    const QString main_identity = QStringLiteral("018f84c0-0000-7000-8000-000000000003");
    const QString project_identity = QStringLiteral("018f84c0-0000-7000-8000-000000000004");
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]
    {
        while (server.hasPendingConnections())
        {
            QTcpSocket *socket = server.nextPendingConnection();
            const std::shared_ptr<QByteArray> buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                [socket, buffer, &requested_paths, &request_bodies, &pinned_headers,
                 &identity_lookups, &observed_id, &observed_role, &observed_fingerprint,
                 branch_revision, project_identity]
            {
                buffer->append(socket->readAll());
                const qsizetype marker = buffer->indexOf("\r\n\r\n");
                if (marker < 0) return;
                const QByteArray header = buffer->left(marker);
                qint64 length = 0;
                for (const QByteArray &line : header.split('\n'))
                    if (line.trimmed().toLower().startsWith("content-length:"))
                        length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                if (buffer->size() < marker + 4 + length) return;
                const QByteArray request_line = header.split('\n').first().trimmed();
                const QByteArray path = request_line.split(' ').value(1);
                const QByteArray method = request_line.split(' ').value(0);
                if (method == "GET" && path == "/dvmdb/v1/repository/tracking")
                {
                    ++identity_lookups;
                    const QByteArray payload = QJsonDocument(QJsonObject{
                        {QStringLiteral("repositoryId"), observed_id},
                        {QStringLiteral("role"), observed_role},
                        {QStringLiteral("modelCodecFingerprint"), observed_fingerprint}})
                        .toJson(QJsonDocument::Compact);
                    socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Connection: close\r\nContent-Length: ") + QByteArray::number(payload.size())
                        + "\r\n\r\n" + payload);
                    socket->disconnectFromHost();
                    return;
                }
                requested_paths.append(QString::fromUtf8(path));
                QByteArray pinned;
                for (const QByteArray &line : header.split('\n'))
                    if (line.trimmed().toLower().startsWith("x-aowis-expected-repository-id:"))
                        pinned = line.mid(line.indexOf(':') + 1).trimmed();
                pinned_headers.append(QString::fromLatin1(pinned));
                const QJsonDocument json = QJsonDocument::fromJson(buffer->mid(marker + 4, length));
                request_bodies.append(json.object());
                QByteArray payload;
                int status = 201;
                // Like DVMDB, answer with the caller-chosen identities.
                if (path == "/dvmdb/v1/projects")
                    payload = QJsonDocument(QJsonObject{
                        {QStringLiteral("id"), json.object().value(QStringLiteral("id"))},
                        {QStringLiteral("name"), QStringLiteral("Water Project")}}).toJson(QJsonDocument::Compact);
                else if (method == "PATCH" && path == QStringLiteral("/dvmdb/v1/projects/%1")
                    .arg(project_identity).toUtf8())
                {
                    status = 200;
                    payload = QJsonDocument(QJsonObject{
                        {QStringLiteral("id"), project_identity},
                        {QStringLiteral("name"), json.object().value(QStringLiteral("name"))}})
                        .toJson(QJsonDocument::Compact);
                }
                else if (path == QStringLiteral("/dvmdb/v1/projects/%1/branches")
                    .arg(project_identity).toUtf8()
                    && json.object().value(QStringLiteral("name")) != QStringLiteral("duplicate"))
                    payload = QJsonDocument(QJsonObject{
                        {QStringLiteral("id"), json.object().value(QStringLiteral("unborn")).toBool()
                            ? QJsonValue(QJsonValue::Null) : QJsonValue(branch_revision)},
                        {QStringLiteral("branch"), json.object().value(QStringLiteral("name"))},
                        {QStringLiteral("branchId"), json.object().value(QStringLiteral("id"))},
                        {QStringLiteral("description"), json.object().value(QStringLiteral("description"))},
                        {QStringLiteral("tags"), json.object().value(QStringLiteral("tags"))}}).toJson(QJsonDocument::Compact);
                else if (method == "POST" &&
                    (path.endsWith("/delete") || path.endsWith("/truncate")))
                {
                    status = 200;
                    payload = R"({"deleted":true,"branchDeleted":true})";
                }
                else
                {
                    status = 409;
                    payload = R"({"error":{"code":"conflict","message":"duplicate branch"}})";
                }
                socket->write(QByteArray("HTTP/1.1 ") + QByteArray::number(status)
                    + (status == 201 ? " Created\r\n" : status == 200 ? " OK\r\n" : " Conflict\r\n")
                    + "Content-Type: application/json\r\nConnection: close\r\nContent-Length: "
                    + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    });

    ProjectServerProfile profile;
    profile.id = repository_id;
    profile.connection_type = QStringLiteral("remote");
    profile.endpoint = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    profile.auth_mode = QStringLiteral("None");
    ProjectSourceWriter writer;
    bool done = false;
    QString created;
    QString failure;
    writer.createProject(profile, QStringLiteral("Water Project"), [&](const QString &id, const QString &error)
    {
        created = id;
        failure = error;
        done = true;
    });
    if (!require(writer.isBusy(), "writer must be busy during POST")) return 1;
    if (!require(waitUntil(app, [&] { return done; }), "project POST did not finish")) return 1;
    // The writer chose the project UUIDv7 (idempotent creation on resend).
    if (!require(!writer.isBusy() && failure.isEmpty() && DvmdbIdentity::isUuidV7(created)
                 && request_bodies.first().value(QStringLiteral("id")).toString() == created,
                 "project POST did not use and return a caller-chosen UUIDv7")) return 1;
    if (!require(identity_lookups == 1 && pinned_headers.size() == 1
                 && pinned_headers.first() == repository_id,
                 "create project was not preflighted and pinned to repository UUID")) return 1;
    if (!require(requested_paths.size() == 1 && requested_paths.first() == QStringLiteral("/dvmdb/v1/projects")
                 && request_bodies.first().value(QStringLiteral("name")) == QStringLiteral("Water Project")
                 && request_bodies.first().value(QStringLiteral("unborn")).isBool()
                 && request_bodies.first().value(QStringLiteral("unborn")).toBool(),
                 "project POST did not match DVMDB contract")) return 1;

    done = false;
    writer.createBranch(profile, project_identity, QStringLiteral("feature"),
                        main_identity, root_revision,
                        [&](const QString &id, const QString &error)
    {
        created = id;
        failure = error;
        done = true;
    });
    if (!require(waitUntil(app, [&] { return done; }), "branch POST did not finish")) return 1;
    if (!require(failure.isEmpty() && DvmdbIdentity::isUuidV7(created)
        && request_bodies.last().value(QStringLiteral("id")).toString() == created
        && requested_paths.size() == 2
        && requested_paths.last() == QStringLiteral("/dvmdb/v1/projects/%1/branches").arg(project_identity)
        && request_bodies.last().value(QStringLiteral("name")) == QStringLiteral("feature")
        && request_bodies.last().value(QStringLiteral("fromBranchId")) == main_identity
        && !request_bodies.last().contains(QStringLiteral("fromBranch"))
        && request_bodies.last().value(QStringLiteral("revision")) == root_revision,
        "branch-from-revision POST does not match DVMDB contract")) return 1;

    // VC-02: historical forks must preserve their exact revision UUID and
    // arbitrary UTF-8 branch labels, including spaces, over the REST boundary.
    const QString historical_name = QString::fromUtf8("Prüfung Süd 2026");
    done = false;
    const QString first_branch = created;
    writer.createBranch(profile, project_identity, historical_name,
                        main_identity, root_revision,
                        [&](const QString &id, const QString &error)
    {
        created = id;
        failure = error;
        done = true;
    });
    if (!require(waitUntil(app, [&] { return done; }), "historical UTF-8 branch POST did not finish")) return 1;
    if (!require(failure.isEmpty() && DvmdbIdentity::isUuidV7(created) && created != first_branch
                 && requested_paths.size() == 3
                 && request_bodies.last().value(QStringLiteral("name")) == historical_name
                 && request_bodies.last().value(QStringLiteral("fromBranchId")) == main_identity
                 && request_bodies.last().value(QStringLiteral("revision")) == root_revision,
                 "historical fork lost its exact revision or UTF-8 branch name")) return 1;

    done = false;
    writer.createBranch(profile, project_identity, QStringLiteral("duplicate"),
                        main_identity, {}, [&](const QString &id, const QString &error)
    {
        created = id;
        failure = error;
        done = true;
    });
    if (!require(waitUntil(app, [&] { return done; }), "conflict POST did not finish")) return 1;
    if (!require(created.isEmpty() && failure.contains(QStringLiteral("HTTP 409"))
                 && failure.contains(QStringLiteral("duplicate branch")),
        "structured branch conflict was lost")) return 1;

    const int before_rejected = requested_paths.size();
    done = false;
    writer.createBranch(profile, project_identity, QStringLiteral("badsha"),
                        main_identity, QString(64, QLatin1Char('a')),
                        [&](const QString &, const QString &error)
    { failure = error; done = true; });
    if (!require(done && failure.contains(QStringLiteral("UUIDv7"))
        && requested_paths.size() == before_rejected,
        "legacy SHA revision was sent to the canonical UUIDv7 writer")) return 1;

    // A branch name is not an identity: the source must be a UUIDv7.
    const int before_named_source = requested_paths.size();
    done = false;
    writer.createBranch(profile, project_identity, QStringLiteral("named"),
                        QStringLiteral("main"), {}, [&](const QString &, const QString &error)
    { failure = error; done = true; });
    if (!require(done && failure.contains(QStringLiteral("UUIDv7"))
        && requested_paths.size() == before_named_source,
        "branch source given by name reached DVMDB")) return 1;

    const int before_missing_source = requested_paths.size();
    done = false;
    writer.createBranch(profile, project_identity, QStringLiteral("unsourced"), {}, {},
                        [&](const QString &id, const QString &error)
    {
        created = id;
        failure = error;
        done = true;
    });
    if (!require(done && created.isEmpty() && !failure.isEmpty() && !writer.isBusy()
                 && requested_paths.size() == before_missing_source,
                 "branch without an explicit committed source reached DVMDB")) return 1;

    done = false;
    const QStringList branch_tags{QStringLiteral("hydraulic"), QStringLiteral("experiment")};
    const QString branch_description = QString::fromUtf8("Independent Prüfzweig");
    writer.createUnbornBranch(profile, project_identity, QStringLiteral("alternative"),
                              branch_tags, branch_description,
                              [&](const QString &id, const QString &error)
    {
        created = id;
        failure = error;
        done = true;
    });
    if (!require(waitUntil(app, [&] { return done; }), "unborn branch POST did not finish")) return 1;
    if (!require(failure.isEmpty() && DvmdbIdentity::isUuidV7(created)
        && requested_paths.size() == 5
        && requested_paths.last() == QStringLiteral("/dvmdb/v1/projects/%1/branches").arg(project_identity)
        && request_bodies.last().value(QStringLiteral("id")).toString() == created
        && request_bodies.last().value(QStringLiteral("unborn")).toBool()
        && !request_bodies.last().contains(QStringLiteral("fromBranchId"))
        && !request_bodies.last().contains(QStringLiteral("revision"))
        && request_bodies.last().value(QStringLiteral("description")).toString() == branch_description
        && request_bodies.last().value(QStringLiteral("tags")).toArray().size() == 2,
        "unborn branch POST lost the origin-free HEAD or its metadata")) return 1;

    done = false;
    writer.renameProject(profile, project_identity, QStringLiteral("Water Project"),
                         QStringLiteral("Renamed Water Project"),
                         [&](const QString &id, const QString &error)
    {
        created = id;
        failure = error;
        done = true;
    });
    if (!require(waitUntil(app, [&] { return done; }), "project PATCH did not finish")) return 1;
    if (!require(failure.isEmpty() && created == project_identity &&
                 requested_paths.last() == QStringLiteral("/dvmdb/v1/projects/%1")
                     .arg(project_identity) &&
                 request_bodies.last().value(QStringLiteral("expectedName"))
                     == QStringLiteral("Water Project") &&
                 request_bodies.last().value(QStringLiteral("name"))
                     == QStringLiteral("Renamed Water Project"),
                 "project name PATCH did not preserve its UUID or optimistic name check")) return 1;

    // The inspector's confirmed actions must actually send the destructive
    // REST operation, including CAS identity and explicit draft discard.
    done = false;
    writer.deleteBranch(profile, project_identity, main_identity, root_revision,
        [&](const QString &id, const QString &error) {
            created = id; failure = error; done = true;
        });
    if (!require(waitUntil(app, [&] { return done; }) && failure.isEmpty() &&
        created == QStringLiteral("deleted") &&
        requested_paths.last() == QStringLiteral("/dvmdb/v1/projects/%1/branch-ids/%2/delete")
            .arg(project_identity, main_identity) &&
        request_bodies.last().value(QStringLiteral("expectedHead")) == root_revision &&
        request_bodies.last().value(QStringLiteral("discardWorkingCopies")).toBool(),
        "confirmed branch deletion did not send the proper DVMDB request")) return 1;
    done = false;
    writer.deleteBranch(profile, project_identity, main_identity, {},
        [&](const QString &id, const QString &error) {
            created = id; failure = error; done = true;
        });
    if (!require(waitUntil(app, [&] { return done; }) && failure.isEmpty() &&
        request_bodies.last().value(QStringLiteral("expectedHead")).isNull() &&
        request_bodies.last().value(QStringLiteral("discardWorkingCopies")).toBool(),
        "unborn HEAD deletion did not send null expectedHead")) return 1;
    done = false;
    writer.truncateBranch(profile, project_identity, main_identity, branch_revision,
                          root_revision, [&](const QString &id, const QString &error) {
        created = id; failure = error; done = true;
    });
    if (!require(waitUntil(app, [&] { return done; }) && failure.isEmpty() &&
        requested_paths.last() == QStringLiteral("/dvmdb/v1/projects/%1/branch-ids/%2/truncate")
            .arg(project_identity, main_identity) &&
        request_bodies.last().value(QStringLiteral("expectedHead")) == branch_revision &&
        request_bodies.last().value(QStringLiteral("firstRemoved")) == root_revision &&
        request_bodies.last().value(QStringLiteral("discardWorkingCopies")).toBool(),
        "confirmed revision truncation did not send correct CAS and draft discard")) return 1;
    const int before_invalid_delete = requested_paths.size();
    done = false;
    writer.truncateBranch(profile, project_identity, main_identity, {}, root_revision,
        [&](const QString &, const QString &error) { failure = error; done = true; });
    if (!require(done && !failure.isEmpty() && requested_paths.size() == before_invalid_delete,
        "invalid revision truncation reached server")) return 1;

    // Every metadata mutation must refuse a replaced endpoint before sending
    // a write; the same guard also rejects mismatched roles and codecs.
    const int before_identity_rejection = requested_paths.size();
    observed_id = QStringLiteral("018f84c0-0000-7000-8000-0000000000ad");
    done = false;
    writer.createProject(profile, QStringLiteral("Wrong endpoint"),
                         [&](const QString &, const QString &error) { failure = error; done = true; });
    if (!require(waitUntil(app, [&] { return done; }) && failure.contains("identity changed")
                 && requested_paths.size() == before_identity_rejection,
                 "writer sent a write to a replaced repository")) return 1;
    observed_id = repository_id;
    observed_role = QStringLiteral("private");
    done = false;
    writer.renameProject(profile, project_identity, QStringLiteral("Renamed Water Project"),
                         QStringLiteral("Should not happen"),
                         [&](const QString &, const QString &error) { failure = error; done = true; });
    if (!require(waitUntil(app, [&] { return done; }) && failure.contains("role")
                 && requested_paths.size() == before_identity_rejection,
                 "writer sent a write to a repository with the wrong role")) return 1;
    observed_role = QStringLiteral("shared");
    observed_fingerprint = QStringLiteral("incorrect");
    done = false;
    writer.createUnbornBranch(profile, project_identity, QStringLiteral("No codec"), {}, {},
                              [&](const QString &, const QString &error) { failure = error; done = true; });
    if (!require(waitUntil(app, [&] { return done; }) && failure.contains("codec")
                 && requested_paths.size() == before_identity_rejection,
                 "writer sent a write to a repository with the wrong MODEL codec")) return 1;
    observed_fingerprint = ModelCodecCompatibility::fingerprint();
    const ProjectServerProfile verified_profile = profile;
    profile.pending = true;
    done = false;
    writer.createProject(profile, QStringLiteral("Pending"),
                         [&](const QString &, const QString &error) { failure = error; done = true; });
    if (!require(done && failure.contains("verified")
                 && requested_paths.size() == before_identity_rejection,
                 "unverified profile was allowed to mutate a repository")) return 1;
    profile = verified_profile;

    const int requests_before_auth_rejection = requested_paths.size();
    profile.auth_mode = QStringLiteral("API token");
    done = false;
    writer.createProject(profile, QStringLiteral("invalid"), [&](const QString &, const QString &error)
    {
        failure = error;
        done = true;
    });
    if (!require(done && !writer.isBusy() && failure.contains(QStringLiteral("not implemented"))
                 && requested_paths.size() == requests_before_auth_rejection,
        "unsupported authentication was not rejected before POST")) return 1;
    return 0;
}
