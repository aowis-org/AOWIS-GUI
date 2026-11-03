#include "projects/model_codec_compatibility.h"
#include "projects/project_source_reader.h"
#include "projects/dvmdb_identity.h"

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
    if (!require(server.listen(QHostAddress::LocalHost, 0), "cannot start REST mock")) return 1;
    const QString project_id = QStringLiteral("018f84c0-0000-7000-8000-000000000900");
    const QString repository_id = QStringLiteral("018f84c0-0000-7000-8000-000000000999");
    const QString a = QStringLiteral("018f84c0-0000-7000-8000-000000000001");
    const QString b = QStringLiteral("018f84c0-0000-7000-8000-000000000002");
    const QString c = QStringLiteral("018f84c0-0000-7000-8000-000000000003");
    const QString d = QStringLiteral("018f84c0-0000-7000-8000-000000000004");
    const QString e = QStringLiteral("018f84c0-0000-7000-8000-000000000005");
    const QString branch_main = QStringLiteral("018f84c0-0000-7000-8000-000000000101");
    const QString branch_feature = QStringLiteral("018f84c0-0000-7000-8000-000000000102");
    const QString branch_alias = QStringLiteral("018f84c0-0000-7000-8000-000000000103");
    const QString branch_history = QStringLiteral("018f84c0-0000-7000-8000-000000000104");
    const QHash<QString,QString> digests = {{a,QString(64,QLatin1Char('a'))},
        {b,QString(64,QLatin1Char('b'))}, {c,QString(64,QLatin1Char('c'))},
        {d,QString(64,QLatin1Char('d'))}, {e,QString(64,QLatin1Char('e'))}};

    const auto record = [&digests](const QString &id, const QStringList &parents, bool invalid = false)
    {
        QJsonArray links;
        QJsonArray hashes;
        for (const QString &parent : parents)
        {
            links.append(parent);
            hashes.append(invalid ? QString(64, QLatin1Char('f')) : digests.value(parent));
        }
        return QJsonObject{{QStringLiteral("id"), id},
            {QStringLiteral("recordHash"), digests.value(id)},
            {QStringLiteral("parents"), links},
            {QStringLiteral("parentRecordHashes"), hashes}};
    };
    const auto head = [&digests](const QString &branch, const QString &branch_id,
                                  const QString &revision, const QString &origin = {},
                                  const QString &fork = {})
    {
        return QJsonObject{{QStringLiteral("branch"), branch},
            {QStringLiteral("branchId"), branch_id},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("recordHash"), digests.value(revision)},
            {QStringLiteral("originBranchId"), origin.isEmpty()
                ? QJsonValue(QJsonValue::Null) : QJsonValue(origin)},
            {QStringLiteral("forkRevisionId"), fork.isEmpty()
                ? QJsonValue(QJsonValue::Null) : QJsonValue(fork)}};
    };
    QStringList requests;
    bool change_pinned_head = false;
    bool corrupt_parent_hash = false;
    bool corrupt_branch_id = false;
    bool corrupt_origin_id = false;
    bool corrupt_fork_id = false;
    bool change_pinned_fork = false;
    bool unreachable_fork = false;
    bool unborn_project = false;
    bool first_commit_project = false;
    bool invalid_unborn_digest = false;
    bool invalid_repository_id = false;
    bool invalid_codec_fingerprint = false;
    bool large_catalogue = false;
    bool many_branches = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]
    {
        while (server.hasPendingConnections())
        {
            QTcpSocket *socket = server.nextPendingConnection();
            const std::shared_ptr<QByteArray> buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                [socket, buffer, &requests, &change_pinned_head, &corrupt_parent_hash,
                 &corrupt_branch_id, &corrupt_origin_id, &corrupt_fork_id, &change_pinned_fork,
                 &unreachable_fork,
                 &unborn_project, &first_commit_project,
                 &invalid_unborn_digest, &invalid_repository_id, &invalid_codec_fingerprint,
                 &large_catalogue, &many_branches, repository_id,
                 project_id, a, b, c, d, e, branch_main,
                 branch_feature, branch_alias, branch_history, record, head]
            {
                buffer->append(socket->readAll());
                if (!buffer->contains("\r\n\r\n")) return;
                const QByteArray path = buffer->split(' ').value(1);
                requests.append(QString::fromLatin1(path));
                QByteArray body;
                if (path == "/dvmdb/v1/repository")
                    body = QJsonDocument(QJsonObject{
                        {QStringLiteral("repositoryId"), invalid_repository_id
                            ? QStringLiteral("invalid") : repository_id},
                        {QStringLiteral("modelCodecFingerprint"), invalid_codec_fingerprint
                            ? QString(64, QLatin1Char('0'))
                            : ModelCodecCompatibility::fingerprint()}}).toJson();
                else if (path == "/dvmdb/v1/projects")
                {
                    QJsonArray projects;
                    if (large_catalogue)
                    {
                        for (int index = 0; index < 105; ++index)
                            projects.append(QJsonObject{
                                {QStringLiteral("id"), QStringLiteral("018f84c0-0000-7000-8000-%1")
                                    .arg(index, 12, 16, QLatin1Char('0'))},
                                {QStringLiteral("name"), QStringLiteral("Waterworks %1").arg(index)}});
                    }
                    else projects.append(QJsonObject{{QStringLiteral("id"), project_id},
                                    {QStringLiteral("name"), QStringLiteral("Waterworks")}});
                    body = QJsonDocument(QJsonObject{{QStringLiteral("projects"), projects}}).toJson();
                }
                else if (large_catalogue && path.startsWith("/dvmdb/v1/projects/") &&
                         path.contains("/revision-graph?limit=256"))
                {
                    const QString requested = QString::fromLatin1(path.split('/').value(4));
                    const QJsonObject unborn{
                        {QStringLiteral("branch"), QStringLiteral("main")},
                        {QStringLiteral("branchId"), QStringLiteral("018f84c0-0000-7000-8001-%1")
                            .arg(requested.right(12))},
                        {QStringLiteral("revision"), QJsonValue(QJsonValue::Null)},
                        {QStringLiteral("recordHash"), QJsonValue(QJsonValue::Null)}};
                    body = QJsonDocument(QJsonObject{{QStringLiteral("heads"), QJsonArray{unborn}},
                        {QStringLiteral("revisions"), QJsonArray{}},
                        {QStringLiteral("complete"), true},
                        {QStringLiteral("nextCursor"), QJsonValue(QJsonValue::Null)}}).toJson();
                }
                else if (many_branches && path.startsWith(
                    QStringLiteral("/dvmdb/v1/projects/%1/revision-graph?limit=256").arg(project_id).toLatin1()))
                {
                    QJsonArray heads;
                    for (int index = 0; index < 151; ++index)
                        heads.append(QJsonObject{
                            {QStringLiteral("branch"), QStringLiteral("branch-%1").arg(index)},
                            {QStringLiteral("branchId"), QStringLiteral("018f84c0-0000-7000-8000-%1")
                                .arg(index + 2048, 12, 16, QLatin1Char('0'))},
                            {QStringLiteral("revision"), QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("recordHash"), QJsonValue(QJsonValue::Null)}});
                    body = QJsonDocument(QJsonObject{{QStringLiteral("heads"), heads},
                        {QStringLiteral("revisions"), QJsonArray{}},
                        {QStringLiteral("complete"), true},
                        {QStringLiteral("nextCursor"), QJsonValue(QJsonValue::Null)}}).toJson();
                }
                else if (path.startsWith(QStringLiteral("/dvmdb/v1/projects/%1/revision-graph?limit=256")
                                            .arg(project_id).toUtf8()))
                {
                    const bool second = path.contains("&cursor=page-2");
                    QJsonArray heads{head(QStringLiteral("main"), branch_main, e),
                                     head(QStringLiteral("Feature Branch"), branch_feature, d, branch_main, b),
                                     head(QStringLiteral("alias"), branch_alias, e, branch_feature, d),
                                     head(QStringLiteral("archive"), branch_history, b, branch_main, a)};
                    if (first_commit_project)
                        heads = QJsonArray{head(QStringLiteral("main"), branch_main, a)};
                    if (unborn_project)
                    {
                        heads = QJsonArray{QJsonObject{{QStringLiteral("branch"), QStringLiteral("main")},
                            {QStringLiteral("branchId"), branch_main},
                            {QStringLiteral("revision"), QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("recordHash"), invalid_unborn_digest
                                ? QJsonValue(QString(64, QLatin1Char('a')))
                                : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("originBranchId"), QJsonValue(QJsonValue::Null)}}};
                    }
                    if (second && change_pinned_head)
                        heads[0] = head(QStringLiteral("main"), branch_main, c);
                    if (second && change_pinned_fork)
                        heads[1] = head(QStringLiteral("Feature Branch"), branch_feature, d, branch_main, a);
                    if (unreachable_fork)
                        heads[3] = head(QStringLiteral("archive"), branch_history, b, branch_main, c);
                    if (corrupt_branch_id)
                    {
                        QJsonObject invalid = heads.at(0).toObject();
                        invalid.remove(QStringLiteral("branchId"));
                        heads[0] = invalid;
                    }
                    if (corrupt_origin_id)
                    {
                        QJsonObject invalid = heads.at(1).toObject();
                        invalid.insert(QStringLiteral("originBranchId"), 42);
                        heads[1] = invalid;
                    }
                    if (corrupt_fork_id)
                    {
                        QJsonObject invalid = heads.at(1).toObject();
                        invalid.insert(QStringLiteral("forkRevisionId"), 42);
                        heads[1] = invalid;
                    }
                    const QJsonArray revisions = unborn_project ? QJsonArray{}
                        : first_commit_project ? QJsonArray{record(a, {})} : second
                        ? QJsonArray{record(b, {a}), record(a, {})}
                        : QJsonArray{record(e, {c,d}, corrupt_parent_hash),
                                     record(c, {b}), record(d, {b})};
                    body = QJsonDocument(QJsonObject{{QStringLiteral("heads"), heads},
                        {QStringLiteral("revisions"), revisions},
                        {QStringLiteral("complete"), unborn_project || first_commit_project || second},
                        {QStringLiteral("nextCursor"), (unborn_project || first_commit_project || second) ? QJsonValue(QJsonValue::Null)
                            : QJsonValue(QStringLiteral("page-2"))}}).toJson();
                }
                else body = R"({"error":"unknown route"})";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                    + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        }
    });

    ProjectServerProfile profile;
    profile.id = QStringLiteral("test");
    profile.connection_type = QStringLiteral("remote");
    profile.name = QStringLiteral("Test DB");
    profile.auth_mode = QStringLiteral("None");
    profile.endpoint = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    ProjectSourceReader reader;
    bool finished = false;
    SourceSnapshot received;
    QString failure;
    const auto load = [&]
    {
        finished = false;
        reader.load(profile, [&](const SourceSnapshot &snapshot, const QString &error)
        {
            received = snapshot;
            failure = error;
            finished = true;
        });
        return waitUntil(app, [&] { return finished; });
    };
    if (!require(load(), "canonical REST reader timed out")) return 1;
    if (!require(failure.isEmpty() && received.projects.size() == 1
                 && received.repository_id == repository_id,
                 "canonical graph refresh lost its repository UUID")) return 1;
    const SourceProject &project = received.projects.first();
    if (!require(project.graph_complete && project.id == project_id
        && project.branches.size() == 4 && project.revisions.size() == 5
        && project.branches.first().id == branch_main && project.branches.first().head == e
        && project.branches.at(2).head == e && project.branches.at(2).id == branch_alias
        && project.branches.at(1).name == QStringLiteral("Feature Branch")
        && project.branches.at(1).origin_branch_id == branch_main
        && project.branches.at(2).origin_branch_id == branch_feature
        && project.branches.at(1).fork_revision_id == b
        && project.branches.at(2).fork_revision_id == d
        && project.branches.at(3).fork_revision_id == a
        && project.branches.first().fork_revision_id.isEmpty()
        && project.branches.at(3).head == b
        && project.revisions.first().id == e
        && project.revisions.first().parents == QStringList({c,d})
        && project.revisions.first().parent_record_sha256 == QStringList({digests.value(c),digests.value(d)})
        && project.revisions.first().record_sha256 == digests.value(e),
        "UUIDv7 branch/merge identities or SHA record binding lost")) return 1;
    if (!require(requests.size() == 4
        && requests.at(0) == QStringLiteral("/dvmdb/v1/repository")
        && requests.at(1) == QStringLiteral("/dvmdb/v1/projects")
        && requests.at(2) == QStringLiteral("/dvmdb/v1/projects/%1/revision-graph?limit=256").arg(project_id)
        && requests.at(3) == QStringLiteral("/dvmdb/v1/projects/%1/revision-graph?limit=256&cursor=page-2").arg(project_id),
        "pinned graph pagination did not use the canonical route")) return 1;

    invalid_repository_id = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty()
                 && received.repository_id.isEmpty(),
                 "reader accepted an invalid repository identity")) return 1;
    invalid_repository_id = false;
    invalid_codec_fingerprint = true;
    if (!require(load() && failure.contains(QStringLiteral("MODEL codec mismatch"))
                 && received.projects.isEmpty(),
                 "reader did not reject a different MODEL codec before loading projects")) return 1;
    invalid_codec_fingerprint = false;

    change_pinned_head = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty(),
                 "graph accepted changed branch HEAD during pagination")) return 1;
    change_pinned_head = false;
    change_pinned_fork = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty(),
                 "graph accepted changed immutable fork revision during pagination")) return 1;
    change_pinned_fork = false;
    corrupt_parent_hash = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty(),
                 "graph accepted mismatched parent record SHA")) return 1;
    corrupt_parent_hash = false;
    corrupt_branch_id = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty(),
                 "graph accepted missing branch UUIDv7")) return 1;
    corrupt_branch_id = false;
    corrupt_origin_id = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty(),
                 "graph accepted malformed branch creation provenance")) return 1;
    corrupt_origin_id = false;
    corrupt_fork_id = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty(),
                 "graph accepted malformed immutable fork revision")) return 1;
    corrupt_fork_id = false;
    unreachable_fork = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty(),
                 "graph accepted a fork revision outside the branch ancestry")) return 1;
    unreachable_fork = false;

    unborn_project = true;
    if (!require(load() && failure.isEmpty() && received.projects.size() == 1
        && received.projects.first().graph_complete
        && received.projects.first().branches.size() == 1
        && received.projects.first().branches.first().id == branch_main
        && received.projects.first().branches.first().head.isEmpty()
        && received.projects.first().branches.first().record_sha256.isEmpty()
        && received.projects.first().branches.first().fork_revision_id.isEmpty()
        && received.projects.first().revisions.isEmpty(),
        "unborn project with null HEAD was not accepted")) return 1;
    // An unborn graph must stay empty across refreshes, including when a
    // previous load of the same project contained committed revisions.
    const auto verify_unborn_reload = [&]()
    {
        if (!load() || !failure.isEmpty() || received.projects.size() != 1)
            return false;
        const SourceProject &empty = received.projects.first();
        return empty.graph_complete && empty.id == project_id
            && empty.branches.size() == 1
            && empty.branches.first().id == branch_main
            && empty.branches.first().head.isEmpty()
            && empty.branches.first().record_sha256.isEmpty()
            && empty.revisions.isEmpty();
    };
    if (!require(verify_unborn_reload() && verify_unborn_reload(),
                 "refresh of unborn project produced a revision or changed HEAD")) return 1;
    // The first commit must publish exactly one parentless UUIDv7 revision
    // while retaining the UUID identity of the original unborn main branch.
    unborn_project = false;
    first_commit_project = true;
    const auto verify_first_commit = [&]()
    {
        if (!load() || !failure.isEmpty() || received.projects.size() != 1)
            return false;
        const SourceProject &committed = received.projects.first();
        return committed.graph_complete && committed.id == project_id
            && committed.branches.size() == 1
            && committed.branches.first().name == QStringLiteral("main")
            && committed.branches.first().id == branch_main
            && committed.branches.first().head == a
            && committed.branches.first().record_sha256 == digests.value(a)
            && committed.revisions.size() == 1
            && committed.revisions.first().id == a
            && committed.revisions.first().parents.isEmpty()
            && committed.revisions.first().parent_record_sha256.isEmpty()
            && committed.revisions.first().record_sha256 == digests.value(a);
    };
    if (!require(verify_first_commit() && verify_first_commit(),
                 "first commit from unborn main did not persist across graph refresh")) return 1;
    first_commit_project = false;
    if (!require(load() && failure.isEmpty() && received.projects.size() == 1
        && received.projects.first().revisions.size() == 5
        && received.projects.first().branches.first().head == e,
        "committed graph did not replace previously loaded unborn graph")) return 1;
    unborn_project = true;
    if (!require(verify_unborn_reload(),
                 "switching back to unborn project retained committed revision data")) return 1;
    invalid_unborn_digest = true;
    if (!require(load() && !failure.isEmpty() && received.projects.isEmpty(),
        "unborn branch accepted a non-null record digest")) return 1;
    invalid_unborn_digest = false;
    unborn_project = false;

    // INT-03: a catalogue larger than the old hard limit of 100 projects
    // must be delivered completely, not silently truncated or rejected.
    large_catalogue = true;
    requests.clear();
    if (!require(load() && failure.isEmpty() && received.projects.size() == 105
        && received.projects.first().graph_complete
        && received.projects.last().graph_complete
        && received.projects.last().branches.size() == 1
        && requests.size() == 107,
        "large multi-project catalogue was hidden or only partly loaded")) return 1;
    large_catalogue = false;
    // Likewise, >150 branch HEADs must survive the snapshot graph reader.
    many_branches = true;
    if (!require(load() && failure.isEmpty() && received.projects.size() == 1
        && received.projects.first().graph_complete
        && received.projects.first().branches.size() == 151
        && received.projects.first().revisions.isEmpty(),
        "large branch catalogue was rejected by obsolete GUI cap")) return 1;
    many_branches = false;

    if (!require(DvmdbIdentity::isUuidV7(a)
        && !DvmdbIdentity::isUuidV7(QStringLiteral("018f84c0-0000-4000-8000-000000000001"))
        && DvmdbIdentity::revisionFormat(a) == DvmdbIdentity::RevisionFormat::UuidV7,
        "UUIDv7 identity parser regression")) return 1;
    profile.auth_mode = QStringLiteral("API token");
    if (!require(load() && failure.contains(QStringLiteral("not implemented")),
                 "unsupported REST authentication was not rejected")) return 1;
    profile.auth_mode = QStringLiteral("None");
    bool cancelled_callback = false;
    reader.load(profile, [&](const SourceSnapshot &, const QString &) { cancelled_callback = true; });
    reader.cancel();
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 150) app.processEvents(QEventLoop::AllEvents, 20);
    if (!require(!cancelled_callback, "cancelled graph read returned stale data")) return 1;
    return 0;
}
