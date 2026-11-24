// Whole-project clone between two DVMDB databases (ProjectCloneTransfer),
// against two scripted loopback servers: identity, codec and role checks,
// fork-ordered branches, unborn branches with metadata, forked branches
// created at their fork revision, chunked history with the smaller transfer
// limit, repository pins on every write, resume after a failure, cancel,
// and the pure planning helpers.
#include "projects/project_clone_transfer.h"
#include "projects/dvmdb_identity.h"
#include "projects/model_codec_compatibility.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>
#include <functional>
#include <memory>

namespace
{
int failures = 0;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "project_clone_transfer: %s\n", message);
        ++failures;
    }
}

bool waitUntil(const std::function<bool()> &condition, int timeout_ms = 10000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout_ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

QString uuid(int n)
{
    return QStringLiteral("018f84c0-0000-7000-8000-%1").arg(n, 12, 10, QLatin1Char('0'));
}

struct Request
{
    QByteArray method;
    QString path;
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(this->body).object(); }
};

struct Reply
{
    int status = 200;
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    static Reply json(int status, const QJsonObject &object)
    {
        return Reply{status, QJsonDocument(object).toJson(QJsonDocument::Compact), {}};
    }
    static Reply error(int status, const QString &message)
    {
        return json(status, QJsonObject{{QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), QStringLiteral("test")}, {QStringLiteral("message"), message}}}});
    }
};

// Minimal HTTP/1.1 server: one request per connection.
class FakeHttp
{
public:
    std::function<Reply(const Request &)> handler;

    bool start()
    {
        if (!this->server.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&this->server, &QTcpServer::newConnection, &this->server, [this]
        {
            while (this->server.hasPendingConnections())
            {
                QTcpSocket *socket = this->server.nextPendingConnection();
                const std::shared_ptr<QByteArray> buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]
                {
                    buffer->append(socket->readAll());
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QList<QByteArray> lines = buffer->left(end).split('\n');
                    Request request;
                    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                    request.method = first.value(0);
                    const QUrl url(QString::fromLatin1(first.value(1)));
                    request.path = url.path(QUrl::FullyEncoded);
                    request.query = QUrlQuery(url.query(QUrl::FullyEncoded));
                    qint64 length = 0;
                    for (int i = 1; i < lines.size(); ++i)
                    {
                        const QByteArray line = lines.at(i).trimmed();
                        const qsizetype colon = line.indexOf(':');
                        if (colon <= 0) continue;
                        const QByteArray name = line.left(colon).trimmed().toLower();
                        const QByteArray value = line.mid(colon + 1).trimmed();
                        request.headers.insert(name, value);
                        if (name == "content-length") length = value.toLongLong();
                    }
                    if (buffer->size() - end - 4 < length) return;
                    request.body = buffer->mid(end + 4, length);
                    buffer->clear();
                    const Reply reply = this->handler(request);
                    QByteArray head = "HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\n"
                        "Connection: close\r\nContent-Length: " + QByteArray::number(reply.body.size()) + "\r\n";
                    for (const QPair<QByteArray, QByteArray> &header : reply.headers)
                        head += header.first + ": " + header.second + "\r\n";
                    socket->write(head + "\r\n" + reply.body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
        return true;
    }
    QString endpoint() const { return QStringLiteral("http://127.0.0.1:%1").arg(this->server.serverPort()); }

private:
    QTcpServer server;
};

struct Branch
{
    QString id;
    QString name;
    QString head; // empty: unborn
    QString origin;
    QString fork;
    QString description;
    QStringList tags;
    QStringList chain; // source only: revisions this branch adds, oldest first
};

QJsonValue nullable(const QString &value)
{
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

QJsonObject branchJson(const Branch &branch)
{
    QJsonArray tags;
    for (const QString &tag : branch.tags) tags.append(tag);
    return QJsonObject{{QStringLiteral("branch"), branch.name}, {QStringLiteral("branchId"), branch.id},
                       {QStringLiteral("revision"), nullable(branch.head)},
                       {QStringLiteral("recordHash"), branch.head.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                                            : QJsonValue(QString(64, QLatin1Char('a')))},
                       {QStringLiteral("originBranchId"), nullable(branch.origin)},
                       {QStringLiteral("forkRevisionId"), nullable(branch.fork)},
                       {QStringLiteral("description"), branch.description},
                       {QStringLiteral("tags"), tags}};
}

// A DVMDB as far as cloning sees it. Bundles are "BUNDLE <branchId> <head>"
// texts; a source chunk carries at most chunk_revisions revisions.
class FakeDvmdb
{
public:
    QString repository_id;
    QString role = QStringLiteral("shared");
    qint64 max_bundle_bytes = 8 * 1024 * 1024;
    bool advertise_limits = true;
    QString codec = ModelCodecCompatibility::fingerprint();
    QMap<QString, QString> projects;               // id -> name
    QMap<QString, QList<Branch>> branches;         // project id -> branches
    int chunk_revisions = 2;
    bool reject_fork_creation = false;
    QString diverged_branch;                       // imports into it answer 409 conflict
    int fail_import_number = -1;                   // 1-based import to answer with 500
    QList<QByteArray> writes;                      // "METHOD path" of every write
    QList<QByteArray> unpinned_writes;
    QList<QUrlQuery> bundle_queries;
    int imports = 0;

    bool start()
    {
        this->http.handler = [this](const Request &request) { return this->handle(request); };
        return this->http.start();
    }
    QString endpoint() const { return this->http.endpoint(); }
    Branch *branch(const QString &project, const QString &id)
    {
        for (Branch &entry : this->branches[project])
            if (entry.id == id) return &entry;
        return nullptr;
    }
    Branch *branchNamed(const QString &project, const QString &name)
    {
        for (Branch &entry : this->branches[project])
            if (entry.name == name) return &entry;
        return nullptr;
    }

private:
    FakeHttp http;

    Reply handle(const Request &request)
    {
        const QString prefix = QStringLiteral("/dvmdb/v1");
        if (!request.path.startsWith(prefix)) return Reply::error(404, QStringLiteral("no route"));
        const QString path = request.path.mid(prefix.size());
        if (request.method != "GET")
        {
            this->writes.append(request.method + ' ' + path.toUtf8());
            if (request.headers.value("x-aowis-expected-repository-id") != this->repository_id.toUtf8())
            {
                this->unpinned_writes.append(request.method + ' ' + path.toUtf8());
                return Reply::error(409, QStringLiteral("repository identity changed"));
            }
        }
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (path == QStringLiteral("/repository"))
        {
            QJsonObject repository{{QStringLiteral("repositoryId"), this->repository_id},
                                   {QStringLiteral("modelCodecFingerprint"), this->codec}};
            if (this->advertise_limits)
                repository.insert(QStringLiteral("transferLimits"),
                                  QJsonObject{{QStringLiteral("maxBundleBytes"), this->max_bundle_bytes}});
            return Reply::json(200, repository);
        }
        if (path == QStringLiteral("/repository/tracking"))
            return Reply::json(200, QJsonObject{{QStringLiteral("repositoryId"), this->repository_id},
                {QStringLiteral("role"), this->role}, {QStringLiteral("modelCodecFingerprint"), this->codec},
                {QStringLiteral("remotes"), QJsonArray{}}, {QStringLiteral("tracking"), QJsonArray{}}});
        if (path == QStringLiteral("/projects/clone-target") && request.method == "POST")
        {
            const QString id = request.json().value(QStringLiteral("projectId")).toString();
            const QString name = request.json().value(QStringLiteral("name")).toString();
            if (this->projects.contains(id) && this->projects.value(id) != name)
                return Reply::error(409, QStringLiteral("different name"));
            this->projects.insert(id, name);
            this->branches[id];
            return Reply::json(200, QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), name}});
        }
        if (parts.size() < 2 || parts.at(0) != QStringLiteral("projects"))
            return Reply::error(404, QStringLiteral("no route"));
        const QString project = parts.at(1);
        if (!this->projects.contains(project)) return Reply::error(404, QStringLiteral("project not found"));
        if (parts.size() == 2 && request.method == "GET")
            return Reply::json(200, QJsonObject{{QStringLiteral("id"), project},
                                                {QStringLiteral("name"), this->projects.value(project)}});
        if (parts.size() == 3 && parts.at(2) == QStringLiteral("revision-graph"))
        {
            QJsonArray heads;
            for (const Branch &entry : this->branches.value(project)) heads.append(branchJson(entry));
            return Reply::json(200, QJsonObject{{QStringLiteral("heads"), heads},
                {QStringLiteral("revisions"), QJsonArray{}}, {QStringLiteral("complete"), false}});
        }
        if (parts.size() == 4 && parts.at(2) == QStringLiteral("branch-ids"))
        {
            Branch *entry = branch(project, parts.at(3));
            if (entry == nullptr) return Reply::error(404, QStringLiteral("branch UUIDv7 not found"));
            if (request.method == "GET") return Reply::json(200, branchJson(*entry));
            const QJsonObject body = request.json();
            if (body.value(QStringLiteral("expectedName")).toString() != entry->name)
                return Reply::error(409, QStringLiteral("name changed"));
            entry->description = body.value(QStringLiteral("description")).toString();
            entry->tags.clear();
            for (const QJsonValue &tag : body.value(QStringLiteral("tags")).toArray()) entry->tags.append(tag.toString());
            return Reply::json(200, QJsonObject{{QStringLiteral("branchId"), entry->id}});
        }
        if (parts.size() == 3 && parts.at(2) == QStringLiteral("branches") && request.method == "POST")
        {
            const QJsonObject body = request.json();
            Branch created;
            created.id = body.value(QStringLiteral("id")).toString();
            created.name = body.value(QStringLiteral("name")).toString();
            if (Branch *existing = branch(project, created.id))
                return Reply::json(201, QJsonObject{{QStringLiteral("branchId"), existing->id}});
            if (body.value(QStringLiteral("unborn")).toBool())
            {
                created.description = body.value(QStringLiteral("description")).toString();
                for (const QJsonValue &tag : body.value(QStringLiteral("tags")).toArray())
                    created.tags.append(tag.toString());
            }
            else
            {
                if (this->reject_fork_creation)
                    return Reply::error(400, QStringLiteral("revision is not reachable from source branch"));
                created.origin = body.value(QStringLiteral("fromBranchId")).toString();
                created.fork = body.value(QStringLiteral("revision")).toString();
                created.head = created.fork;
                if (branch(project, created.origin) == nullptr)
                    return Reply::error(404, QStringLiteral("source branch UUIDv7 not found"));
            }
            this->branches[project].append(created);
            return Reply::json(201, QJsonObject{{QStringLiteral("branchId"), created.id}});
        }
        if (parts.size() == 5 && parts.at(2) == QStringLiteral("branches") && parts.at(4) == QStringLiteral("bundle"))
        {
            const QString name = QUrl::fromPercentEncoding(parts.at(3).toUtf8());
            if (request.method == "GET") return exportChunk(project, name, request.query);
            return importBundle(project, name, request.body);
        }
        return Reply::error(404, QStringLiteral("no route"));
    }

    Reply exportChunk(const QString &project, const QString &name, const QUrlQuery &query)
    {
        this->bundle_queries.append(query);
        Branch *entry = branchNamed(project, name);
        if (entry == nullptr || entry->head.isEmpty()) return Reply::error(404, QStringLiteral("branch not found"));
        const QString base = query.queryItemValue(QStringLiteral("base"));
        // upTo (push): the transfer ends at that revision of the branch.
        const QString up_to = query.queryItemValue(QStringLiteral("upTo"));
        const int last = up_to.isEmpty() ? int(entry->chain.size()) - 1 : int(entry->chain.indexOf(up_to));
        if (last < 0) return Reply::error(400, QStringLiteral("the pushed revision is not on the branch"));
        int start = 0;
        if (!base.isEmpty())
        {
            const int position = entry->chain.indexOf(base);
            if (position < 0 && base != entry->fork) return Reply::error(404, QStringLiteral("base not published"));
            start = position + 1;
        }
        if (start > last) return Reply::error(409, QStringLiteral("base not an ancestor"));
        const int tip = std::min<int>(start + this->chunk_revisions - 1, last);
        const QString head = entry->chain.at(tip);
        Reply reply{200, ("BUNDLE " + entry->id + ' ' + head).toUtf8(), {}};
        reply.headers.append(QPair<QByteArray, QByteArray>{"Content-Type", "application/vnd.aowis.revision-bundle-v2"});
        reply.headers.append(QPair<QByteArray, QByteArray>{"X-AOWIS-Bundle-Head", head.toUtf8()});
        reply.headers.append(QPair<QByteArray, QByteArray>{"X-AOWIS-Bundle-Complete", tip == last ? "true" : "false"});
        return reply;
    }

    Reply importBundle(const QString &project, const QString &name, const QByteArray &body)
    {
        ++this->imports;
        if (this->imports == this->fail_import_number)
            return Reply::error(500, QStringLiteral("injected failure"));
        const QList<QByteArray> fields = body.split(' ');
        if (fields.size() == 3 && QString::fromUtf8(fields.at(1)) == this->diverged_branch)
            return Reply::error(409, QStringLiteral("local HEAD is not an ancestor of the bundle HEAD"));
        if (fields.size() != 3 || fields.at(0) != "BUNDLE")
            return Reply::error(400, QStringLiteral("corrupt bundle"));
        const QString id = QString::fromUtf8(fields.at(1));
        const QString head = QString::fromUtf8(fields.at(2));
        if (Branch *entry = branch(project, id))
        {
            entry->head = head;
            return Reply::json(200, QJsonObject{{QStringLiteral("status"), QStringLiteral("fast_forward")},
                                                {QStringLiteral("head"), head}});
        }
        Branch created;
        created.id = id;
        created.name = name;
        created.head = head;
        this->branches[project].append(created);
        return Reply::json(201, QJsonObject{{QStringLiteral("status"), QStringLiteral("created")},
                                            {QStringLiteral("head"), head}});
    }
};

ProjectServerProfile profile(const FakeDvmdb &server, const QString &name)
{
    ProjectServerProfile result;
    result.id = server.repository_id;
    result.name = name;
    result.endpoint = server.endpoint();
    result.auth_mode = QStringLiteral("None");
    return result;
}

const QString project_id = uuid(100);
const QString main_id = uuid(201);
const QString feature_id = uuid(202);
const QString planned_id = uuid(203);

// main: r1..r5; feature forked at r3 with f1..f3; planned unborn.
void seedSource(FakeDvmdb &source)
{
    source.repository_id = uuid(1);
    source.projects.insert(project_id, QStringLiteral("Pipeline Nord"));
    Branch main{main_id, QStringLiteral("main"), uuid(305), {}, {}, QStringLiteral("Trunk"),
                {QStringLiteral("approved")}, {uuid(301), uuid(302), uuid(303), uuid(304), uuid(305)}};
    Branch feature{feature_id, QStringLiteral("valve study"), uuid(313), main_id, uuid(303), {}, {},
                   {uuid(311), uuid(312), uuid(313)}};
    Branch planned{planned_id, QStringLiteral("planned"), {}, {}, {}, QStringLiteral("Next phase"),
                   {QStringLiteral("draft"), QStringLiteral("phase-2")}, {}};
    // The fork is listed first: the clone must still copy main first.
    source.branches.insert(project_id, {feature, planned, main});
}

struct Outcome
{
    bool done = false;
    QString error;
    QStringList warnings;
    QList<ProjectCloneTransfer::Progress> progress;
};

Outcome runClone(ProjectCloneTransfer &transfer, const ProjectServerProfile &from,
                 const ProjectServerProfile &to, const std::function<void(ProjectCloneTransfer &)> &during = {})
{
    std::shared_ptr<Outcome> outcome = std::make_shared<Outcome>();
    transfer.start(from, project_id, to,
        [outcome, &transfer, during](const ProjectCloneTransfer::Progress &progress)
        {
            outcome->progress.append(progress);
            if (during) during(transfer);
        },
        [outcome](const QString &error, const QStringList &warnings)
        {
            outcome->done = true;
            outcome->error = error;
            outcome->warnings = warnings;
        });
    waitUntil([&] { return outcome->done; });
    return *outcome;
}

Outcome runPush(ProjectCloneTransfer &transfer, const ProjectServerProfile &from, const QString &branch,
                const QString &up_to, const ProjectServerProfile &to)
{
    std::shared_ptr<Outcome> outcome = std::make_shared<Outcome>();
    transfer.startPush(from, project_id, branch, up_to, to,
        [outcome](const ProjectCloneTransfer::Progress &progress) { outcome->progress.append(progress); },
        [outcome](const QString &error, const QStringList &warnings)
        {
            outcome->done = true;
            outcome->error = error;
            outcome->warnings = warnings;
        });
    waitUntil([&] { return outcome->done; });
    return *outcome;
}

// The shared database already has the project with main at r2.
void seedPushTarget(FakeDvmdb &target)
{
    target.repository_id = uuid(2);
    target.projects.insert(project_id, QStringLiteral("Pipeline Nord"));
    target.branches.insert(project_id, {Branch{main_id, QStringLiteral("main"), uuid(302), {}, {},
                                               QStringLiteral("Shared trunk"), {}, {}}});
}

void testPlanning()
{
    QVector<ProjectCloneBranch> parsed;
    QString error;
    const QJsonObject page{{QStringLiteral("heads"), QJsonArray{
        branchJson(Branch{main_id, QStringLiteral("main"), uuid(305), {}, {}, QStringLiteral("d"), {QStringLiteral("t")}, {}}),
        branchJson(Branch{planned_id, QStringLiteral("planned"), {}, {}, {}, {}, {}, {}})}}};
    require(ProjectClonePlan::parseHeads(page, &parsed, &error) && parsed.size() == 2
                && parsed.at(0).head == uuid(305) && parsed.at(0).description == QStringLiteral("d")
                && parsed.at(0).tags == QStringList{QStringLiteral("t")} && parsed.at(1).head.isEmpty(),
            "valid heads were not parsed");
    QJsonArray duplicate = page.value(QStringLiteral("heads")).toArray();
    duplicate.append(branchJson(Branch{uuid(209), QStringLiteral("main"), {}, {}, {}, {}, {}, {}}));
    require(!ProjectClonePlan::parseHeads(QJsonObject{{QStringLiteral("heads"), duplicate}}, &parsed, &error),
            "two branches with one name were accepted");
    require(!ProjectClonePlan::parseHeads(QJsonObject{{QStringLiteral("heads"), QJsonArray{
                branchJson(Branch{QStringLiteral("not-a-uuid"), QStringLiteral("x"), {}, {}, {}, {}, {}, {}})}}},
                &parsed, &error),
            "an invalid branch UUID was accepted");
    require(!ProjectClonePlan::parseHeads(QJsonObject{}, &parsed, &error), "a missing head list was accepted");

    // Forks after their origin, transitively; unknown origins keep source order.
    const ProjectCloneBranch a{uuid(1), QStringLiteral("a"), uuid(9), uuid(2), uuid(8), {}, {}};
    const ProjectCloneBranch b{uuid(2), QStringLiteral("b"), uuid(8), uuid(3), uuid(7), {}, {}};
    const ProjectCloneBranch c{uuid(3), QStringLiteral("c"), uuid(7), {}, {}, {}, {}};
    const ProjectCloneBranch d{uuid(4), QStringLiteral("d"), uuid(6), uuid(99), uuid(5), {}, {}};
    QVector<ProjectCloneBranch> ordered;
    require(ProjectClonePlan::order({a, d, b, c}, &ordered, &error) && ordered.size() == 4
                && ordered.at(0).name == QStringLiteral("c") && ordered.at(1).name == QStringLiteral("b")
                && ordered.at(2).name == QStringLiteral("a") && ordered.at(3).name == QStringLiteral("d"),
            "branches are not ordered after the branches they were forked from");
    const ProjectCloneBranch loop_a{uuid(1), QStringLiteral("a"), uuid(9), uuid(2), uuid(8), {}, {}};
    const ProjectCloneBranch loop_b{uuid(2), QStringLiteral("b"), uuid(8), uuid(1), uuid(7), {}, {}};
    require(!ProjectClonePlan::order({loop_a, loop_b}, &ordered, &error) && ordered.isEmpty(),
            "a provenance cycle was accepted");

    const QJsonObject small{{QStringLiteral("transferLimits"), QJsonObject{{QStringLiteral("maxBundleBytes"), 1000}}}};
    const QJsonObject large{{QStringLiteral("transferLimits"), QJsonObject{{QStringLiteral("maxBundleBytes"), 5000}}}};
    require(ProjectClonePlan::chunkLimit(small, large) == 1000 && ProjectClonePlan::chunkLimit(large, small) == 1000,
            "the chunk limit is not the smaller of the two databases");
    require(ProjectClonePlan::chunkLimit(QJsonObject{}, large) == 0 && ProjectClonePlan::chunkLimit(large, QJsonObject{}) == 0,
            "a database without transfer limits was accepted");
}

void testCompleteClone()
{
    FakeDvmdb source;
    FakeDvmdb target;
    seedSource(source);
    target.repository_id = uuid(2);
    target.max_bundle_bytes = 3 * 1024 * 1024; // smaller than the source's 8 MiB
    require(source.start() && target.start(), "fake servers did not start");
    ProjectCloneTransfer transfer;
    const Outcome outcome = runClone(transfer, profile(source, QStringLiteral("Local")),
                                     profile(target, QStringLiteral("Shared")));
    require(outcome.done && outcome.error.isEmpty() && outcome.warnings.isEmpty(), "the clone did not succeed");
    require(!transfer.isBusy(), "the transfer is still busy after completing");
    require(target.projects.value(project_id) == QStringLiteral("Pipeline Nord"),
            "the project was not created with the source UUIDv7 and name");
    require(target.unpinned_writes.isEmpty() && !target.writes.isEmpty(), "a target write was not pinned");
    require(source.writes.isEmpty(), "the source database was written to");

    const Branch *main = target.branch(project_id, main_id);
    const Branch *feature = target.branch(project_id, feature_id);
    const Branch *planned = target.branch(project_id, planned_id);
    require(main != nullptr && main->name == QStringLiteral("main") && main->head == uuid(305)
                && main->description == QStringLiteral("Trunk") && main->tags == QStringList{QStringLiteral("approved")},
            "main was not copied with its HEAD and metadata");
    require(feature != nullptr && feature->name == QStringLiteral("valve study") && feature->head == uuid(313)
                && feature->origin == main_id && feature->fork == uuid(303),
            "the forked branch lost its HEAD or provenance");
    require(planned != nullptr && planned->head.isEmpty() && planned->description == QStringLiteral("Next phase")
                && planned->tags == QStringList({QStringLiteral("draft"), QStringLiteral("phase-2")}),
            "the unborn branch was not recreated by UUIDv7 with its metadata");

    // main before the fork; 3 main chunks (2+2+1), feature continues from
    // its fork revision with 2 chunks; every request asks for the smaller limit.
    require(source.bundle_queries.size() == 5, "unexpected number of bundle chunks");
    for (const QUrlQuery &query : source.bundle_queries)
        require(query.queryItemValue(QStringLiteral("maxBytes")) == QString::number(3 * 1024 * 1024),
                "a chunk did not request the smaller transfer limit");
    if (source.bundle_queries.size() == 5)
    {
        require(!source.bundle_queries.at(0).hasQueryItem(QStringLiteral("base")),
                "the first main chunk was not a full bundle");
        require(source.bundle_queries.at(1).queryItemValue(QStringLiteral("base")) == uuid(302)
                    && source.bundle_queries.at(2).queryItemValue(QStringLiteral("base")) == uuid(304),
                "main chunks do not continue from the previous chunk head");
        require(source.bundle_queries.at(3).queryItemValue(QStringLiteral("base")) == uuid(303)
                    && source.bundle_queries.at(4).queryItemValue(QStringLiteral("base")) == uuid(312),
                "the fork does not continue from its fork revision");
    }
    const int fork_creation = target.writes.indexOf("POST /projects/" + project_id.toUtf8() + "/branches");
    require(target.writes.value(0) == "POST /projects/clone-target", "the project was not provisioned first");
    require(fork_creation > 0, "branches were not created by POST");
    require(target.imports == 5, "unexpected number of imported chunks");
    require(!outcome.progress.isEmpty() && outcome.progress.last().chunks == 5
                && outcome.progress.last().branch_count == 3 && outcome.progress.last().bytes > 0,
            "progress did not count branches, chunks and bytes");

    // Pasting again is a no-op that writes nothing but already equal metadata.
    const int writes_before = target.writes.size();
    const Outcome again = runClone(transfer, profile(source, QStringLiteral("Local")),
                                   profile(target, QStringLiteral("Shared")));
    require(again.error.isEmpty() && target.writes.size() == writes_before && target.imports == 5,
            "repeating a finished clone wrote to the target again");
}

void testResumeAfterFailure()
{
    FakeDvmdb source;
    FakeDvmdb target;
    seedSource(source);
    target.repository_id = uuid(2);
    target.fail_import_number = 2; // the second main chunk
    require(source.start() && target.start(), "fake servers did not start");
    ProjectCloneTransfer transfer;
    const Outcome failed = runClone(transfer, profile(source, QStringLiteral("Local")),
                                    profile(target, QStringLiteral("Shared")));
    require(failed.done && failed.error.contains(QStringLiteral("injected failure"))
                && failed.error.contains(QStringLiteral("main")),
            "a failed import was not reported with the branch and server message");
    require(target.branch(project_id, main_id) != nullptr && target.branch(project_id, main_id)->head == uuid(302),
            "the first chunk was not kept on the target");
    const int clone_targets = int(target.writes.count("POST /projects/clone-target"));
    const qsizetype queries_before = source.bundle_queries.size();

    const Outcome resumed = runClone(transfer, profile(source, QStringLiteral("Local")),
                                     profile(target, QStringLiteral("Shared")));
    require(resumed.error.isEmpty(), "pasting again did not finish the clone");
    require(target.writes.count("POST /projects/clone-target") == clone_targets,
            "the resumed clone provisioned the project again");
    require(target.branch(project_id, main_id)->head == uuid(305)
                && target.branch(project_id, feature_id) != nullptr
                && target.branch(project_id, feature_id)->head == uuid(313),
            "the resumed clone did not reach the source HEADs");
    require(source.bundle_queries.size() > queries_before
                && source.bundle_queries.at(queries_before).queryItemValue(QStringLiteral("base")) == uuid(302),
            "the resumed clone did not continue from the target HEAD");
}

void testRefusals()
{
    const std::function<void(FakeDvmdb &, const Outcome &, const char *)> expectNoWrites =
        [](FakeDvmdb &target, const Outcome &outcome, const char *message)
    {
        require(outcome.done && !outcome.error.isEmpty() && target.writes.isEmpty(), message);
    };
    {
        FakeDvmdb source;
        FakeDvmdb target;
        seedSource(source);
        target.repository_id = uuid(2);
        require(source.start() && target.start(), "fake servers did not start");
        ProjectCloneTransfer transfer;
        ProjectServerProfile wrong = profile(target, QStringLiteral("Shared"));
        wrong.id = uuid(3);
        expectNoWrites(target, runClone(transfer, profile(source, QStringLiteral("Local")), wrong),
                       "a changed target identity was not refused");
        require(!transfer.wroteAnything(), "a refused clone claims it wrote to the target");
        target.role = QStringLiteral("mirror");
        expectNoWrites(target, runClone(transfer, profile(source, QStringLiteral("Local")),
                                        profile(target, QStringLiteral("Shared"))),
                       "a target with an unknown role was not refused");
        target.role = QStringLiteral("shared");
        target.codec = QString(64, QLatin1Char('0'));
        const Outcome codec = runClone(transfer, profile(source, QStringLiteral("Local")),
                                       profile(target, QStringLiteral("Shared")));
        expectNoWrites(target, codec, "a MODEL codec mismatch was not refused");
        require(codec.error.contains(QStringLiteral("MODEL codec")), "the codec refusal does not say why");
        target.codec = ModelCodecCompatibility::fingerprint();
        target.advertise_limits = false;
        expectNoWrites(target, runClone(transfer, profile(source, QStringLiteral("Local")),
                                        profile(target, QStringLiteral("Shared"))),
                       "a target without chunked transfers was not refused");
        const Outcome same = runClone(transfer, profile(source, QStringLiteral("Local")),
                                      profile(source, QStringLiteral("Local")));
        require(same.done && !same.error.isEmpty() && source.writes.isEmpty(),
                "cloning into the source database was not refused");
        ProjectServerProfile pending = profile(target, QStringLiteral("Pending"));
        pending.pending = true;
        expectNoWrites(target, runClone(transfer, profile(source, QStringLiteral("Local")), pending),
                       "an unverified target was not refused");
    }
}

// Every pairing of personal and non-personal databases clones, whatever the
// DVMDB role: into the personal database (private), into a former personal
// database (private, no longer personal), into a shared or unconfigured one,
// and out of the personal database.
void testAnyRolePairing()
{
    struct Case
    {
        const char *name;
        bool source_personal;
        QString source_role;
        bool target_personal;
        QString target_role;
    };
    const QVector<Case> cases{
        {"personal to shared", true, QStringLiteral("private"), false, QStringLiteral("shared")},
        {"shared to personal", false, QStringLiteral("shared"), true, QStringLiteral("private")},
        {"personal to former personal", true, QStringLiteral("private"), false, QStringLiteral("private")},
        {"former personal to shared", false, QStringLiteral("private"), false, QStringLiteral("shared")},
        {"shared to unconfigured", false, QStringLiteral("shared"), false, QStringLiteral("unconfigured")},
    };
    for (const Case &pairing : cases)
    {
        FakeDvmdb source;
        FakeDvmdb target;
        seedSource(source);
        source.role = pairing.source_role;
        target.repository_id = uuid(2);
        target.role = pairing.target_role;
        require(source.start() && target.start(), "fake servers did not start");
        ProjectServerProfile from = profile(source, QStringLiteral("From"));
        from.personal = pairing.source_personal;
        ProjectServerProfile to = profile(target, QStringLiteral("To"));
        to.personal = pairing.target_personal;
        ProjectCloneTransfer transfer;
        const Outcome outcome = runClone(transfer, from, to);
        const bool copied = outcome.error.isEmpty() && transfer.wroteAnything()
            && target.branch(project_id, main_id) != nullptr
            && target.branch(project_id, main_id)->head == uuid(305)
            && target.branch(project_id, feature_id) != nullptr;
        if (!copied)
            std::fprintf(stderr, "project_clone_transfer: %s: %s\n", pairing.name, qPrintable(outcome.error));
        require(copied, "a clone between two configured databases was refused");
    }
}

void testForkFallbackAndWarnings()
{
    FakeDvmdb source;
    FakeDvmdb target;
    seedSource(source);
    // An unborn branch that records its origin cannot be recreated.
    source.branches[project_id].append(Branch{uuid(204), QStringLiteral("orphan"), {}, main_id, {}, {}, {}, {}});
    target.repository_id = uuid(2);
    target.reject_fork_creation = true;
    require(source.start() && target.start(), "fake servers did not start");
    ProjectCloneTransfer transfer;
    const Outcome outcome = runClone(transfer, profile(source, QStringLiteral("Local")),
                                     profile(target, QStringLiteral("Shared")));
    require(outcome.error.isEmpty() && outcome.warnings.size() == 1
                && outcome.warnings.first().contains(QStringLiteral("orphan")),
            "an uncopyable branch was not reported as a warning while the rest was copied");
    require(target.branch(project_id, uuid(204)) == nullptr, "the uncopyable branch was created anyway");
    const Branch *feature = target.branch(project_id, feature_id);
    require(feature != nullptr && feature->head == uuid(313), "the fork fallback did not copy the branch");
    bool full_fork_chunk = false;
    for (int i = 0; i < source.bundle_queries.size(); ++i)
        full_fork_chunk = full_fork_chunk || (i >= 3 && !source.bundle_queries.at(i).hasQueryItem(QStringLiteral("base")));
    require(full_fork_chunk, "a refused fork creation did not fall back to a full bundle");
}

void testDivergedBranch()
{
    FakeDvmdb source;
    FakeDvmdb target;
    seedSource(source);
    target.repository_id = uuid(2);
    target.diverged_branch = main_id;
    require(source.start() && target.start(), "fake servers did not start");
    ProjectCloneTransfer transfer;
    const Outcome outcome = runClone(transfer, profile(source, QStringLiteral("Local")),
                                     profile(target, QStringLiteral("Shared")));
    require(outcome.error.isEmpty() && outcome.warnings.size() == 1
                && outcome.warnings.first().contains(QStringLiteral("'main'"))
                && outcome.warnings.first().contains(QStringLiteral("both databases")),
            "a diverged branch did not become a warning");
    require(target.branch(project_id, main_id) == nullptr, "the diverged branch was changed");
    require(target.branch(project_id, feature_id) != nullptr && target.branch(project_id, planned_id) != nullptr,
            "a diverged branch stopped the rest of the clone");
}

// Pushing a revision sends it and every earlier revision the target lacks,
// in chunks; the target branch ends exactly there. Pushing HEAD sends the rest.
void testPushUpToRevision()
{
    FakeDvmdb source;
    FakeDvmdb target;
    seedSource(source);
    seedPushTarget(target);
    require(source.start() && target.start(), "fake servers did not start");
    ProjectCloneTransfer transfer;
    const Outcome middle = runPush(transfer, profile(source, QStringLiteral("Personal")), main_id, uuid(304),
                                   profile(target, QStringLiteral("Team")));
    require(middle.done && middle.error.isEmpty() && middle.warnings.isEmpty(), "a partial push failed");
    require(target.branch(project_id, main_id)->head == uuid(304), "the target branch did not end at the pushed revision");
    require(source.bundle_queries.size() == 1
                && source.bundle_queries.first().queryItemValue(QStringLiteral("base")) == uuid(302)
                && source.bundle_queries.first().queryItemValue(QStringLiteral("upTo")) == uuid(304),
            "a partial push did not continue from the target branch up to the pushed revision");
    // Commits only: no project provisioning, no branch metadata overwritten.
    require(target.writes == QList<QByteArray>{"POST /projects/" + project_id.toUtf8() + "/branches/main/bundle"}
                && target.unpinned_writes.isEmpty() && source.writes.isEmpty(),
            "a push wrote more than the pinned bundle import");
    require(target.branch(project_id, main_id)->description == QStringLiteral("Shared trunk"),
            "a push overwrote the target branch metadata");

    const Outcome rest = runPush(transfer, profile(source, QStringLiteral("Personal")), main_id, {},
                                 profile(target, QStringLiteral("Team")));
    require(rest.error.isEmpty() && target.branch(project_id, main_id)->head == uuid(305)
                && !source.bundle_queries.last().hasQueryItem(QStringLiteral("upTo")),
            "pushing HEAD did not send the rest");
    // Pushing what is there already writes nothing.
    const int writes = target.writes.size();
    const Outcome again = runPush(transfer, profile(source, QStringLiteral("Personal")), main_id, uuid(305),
                                  profile(target, QStringLiteral("Team")));
    require(again.error.isEmpty() && target.writes.size() == writes, "a repeated push wrote again");
}

void testPushChunksAndNewBranch()
{
    FakeDvmdb source;
    FakeDvmdb target;
    seedSource(source);
    seedPushTarget(target);
    target.branch(project_id, main_id)->head = uuid(305);
    source.chunk_revisions = 1;
    require(source.start() && target.start(), "fake servers did not start");
    ProjectCloneTransfer transfer;
    // The fork is created at its fork revision, then f1..f2 follow one by one.
    const Outcome outcome = runPush(transfer, profile(source, QStringLiteral("Personal")), feature_id, uuid(312),
                                    profile(target, QStringLiteral("Team")));
    const Branch *feature = target.branch(project_id, feature_id);
    require(outcome.error.isEmpty() && feature != nullptr && feature->head == uuid(312)
                && feature->origin == main_id && feature->fork == uuid(303),
            "a pushed new branch was not created with its identity at the pushed revision");
    require(target.imports == 2 && !outcome.progress.isEmpty() && outcome.progress.last().chunks == 2
                && outcome.progress.last().branch_count == 1,
            "a push was not sent in chunks with progress");
    require(target.branch(project_id, main_id)->head == uuid(305), "a push of one branch changed another");
}

void testPushRefusals()
{
    FakeDvmdb source;
    FakeDvmdb target;
    FakeDvmdb empty;
    seedSource(source);
    seedPushTarget(target);
    target.branch(project_id, main_id)->head = uuid(305);
    empty.repository_id = uuid(3);
    require(source.start() && target.start() && empty.start(), "fake servers did not start");
    ProjectCloneTransfer transfer;
    const ProjectServerProfile personal = profile(source, QStringLiteral("Personal"));

    // The target is further than the pushed revision: refused, untouched.
    const Outcome older = runPush(transfer, personal, main_id, uuid(303), profile(target, QStringLiteral("Team")));
    require(older.error.contains(QStringLiteral("Update the clone")) && target.writes.isEmpty()
                && target.branch(project_id, main_id)->head == uuid(305),
            "a push older than the target was not refused");
    // The target branch has history of its own.
    target.branch(project_id, main_id)->head = uuid(399);
    const Outcome apart = runPush(transfer, personal, main_id, {}, profile(target, QStringLiteral("Team")));
    require(apart.error.contains(QStringLiteral("Nothing was overwritten")) && target.writes.isEmpty(),
            "a push onto a branch with history of its own was not refused");
    // Changed during the push (the import is refused).
    target.branch(project_id, main_id)->head = uuid(302);
    target.diverged_branch = main_id;
    const Outcome during = runPush(transfer, personal, main_id, {}, profile(target, QStringLiteral("Team")));
    require(during.error.contains(QStringLiteral("during the push")) && during.warnings.isEmpty(),
            "a refused import was not a push error");
    target.diverged_branch.clear();

    const Outcome missing = runPush(transfer, personal, main_id, {}, profile(empty, QStringLiteral("Empty")));
    require(missing.error.contains(QStringLiteral("Clone the project there first")) && empty.writes.isEmpty(),
            "a push to a database without the project did not ask for a clone");
    const Outcome unborn = runPush(transfer, personal, planned_id, {}, profile(target, QStringLiteral("Team")));
    require(unborn.error.contains(QStringLiteral("no commits")), "an unborn branch was pushed");
    const Outcome gone = runPush(transfer, personal, uuid(299), {}, profile(target, QStringLiteral("Team")));
    require(gone.error.contains(QStringLiteral("no longer exists")), "a missing branch was pushed");
    const Outcome off = runPush(transfer, personal, main_id, uuid(312), profile(target, QStringLiteral("Team")));
    require(!off.error.isEmpty() && target.branch(project_id, main_id)->head == uuid(302),
            "a revision of another branch was pushed");
    const Outcome self = runPush(transfer, personal, main_id, {}, personal);
    require(self.error.contains(QStringLiteral("database it is in")), "a push into its own database was accepted");
    const Outcome invalid = runPush(transfer, personal, QStringLiteral("main"), {}, profile(target, QStringLiteral("Team")));
    require(!invalid.error.isEmpty() && !transfer.isBusy(), "a push without branch identity was accepted");
    // A clone after a push is a clone again (no push settings left over).
    const Outcome clone = runClone(transfer, personal, profile(empty, QStringLiteral("Empty")));
    require(clone.error.isEmpty() && empty.branch(project_id, planned_id) != nullptr,
            "a clone after a push did not copy the whole project");
}

void testCancel()
{
    FakeDvmdb source;
    FakeDvmdb target;
    seedSource(source);
    target.repository_id = uuid(2);
    source.chunk_revisions = 1;
    require(source.start() && target.start(), "fake servers did not start");
    ProjectCloneTransfer transfer;
    bool cancelled = false;
    const Outcome outcome = runClone(transfer, profile(source, QStringLiteral("Local")),
        profile(target, QStringLiteral("Shared")), [&cancelled](ProjectCloneTransfer &running)
        {
            if (!cancelled) { cancelled = true; running.cancel(); }
        });
    require(outcome.done && outcome.error.contains(QStringLiteral("cancelled")), "a cancel was not reported");
    require(!transfer.isBusy() && target.imports < 8, "the transfer kept copying after the cancel");
    // A cancelled clone resumes.
    const Outcome resumed = runClone(transfer, profile(source, QStringLiteral("Local")),
                                     profile(target, QStringLiteral("Shared")));
    require(resumed.error.isEmpty() && target.branch(project_id, feature_id) != nullptr
                && target.branch(project_id, feature_id)->head == uuid(313),
            "a cancelled clone did not resume");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testPlanning();
    testCompleteClone();
    testResumeAfterFailure();
    testRefusals();
    testAnyRolePairing();
    testForkFallbackAndWarnings();
    testDivergedBranch();
    testCancel();
    testPushUpToRevision();
    testPushChunksAndNewBranch();
    testPushRefusals();
    if (failures != 0)
    {
        std::fprintf(stderr, "%d project clone transfer check(s) failed\n", failures);
        return 1;
    }
    std::puts("Project clone transfer tests passed");
    return 0;
}
