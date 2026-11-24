#include "projects/project_clone_transfer.h"

#include "projects/dvmdb_identity.h"
#include "projects/model_codec_compatibility.h"
#include "projects/repository_probe.h"

#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
#include "projects/embedded_dvmdb_host.h"
#endif

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QHash>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace
{
constexpr qint64 json_reply_limit = 4 * 1024 * 1024;
constexpr int json_timeout_ms = 15000;
// Inactivity timeout: a large chunk keeps the transfer alive while it moves.
constexpr int bundle_timeout_ms = 120000;
const QByteArray bundle_mime = QByteArrayLiteral("application/vnd.aowis.revision-bundle-v2");

QString segment(const QString &part)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(part));
}

QString nullableId(const QJsonValue &value)
{
    return value.isString() ? value.toString() : QString();
}

bool validOptionalId(const QJsonValue &value)
{
    return value.isNull() || (value.isString() && DvmdbIdentity::isUuidV7(value.toString()));
}
}

bool ProjectClonePlan::parseHeads(const QJsonObject &page, QVector<ProjectCloneBranch> *branches,
                                  QString *error)
{
    branches->clear();
    const QJsonValue heads = page.value(QStringLiteral("heads"));
    if (!heads.isArray())
    {
        *error = QCoreApplication::translate("ProjectCloneTransfer", "The source database did not list the project's branches.");
        return false;
    }
    QSet<QString> ids;
    QSet<QString> names;
    for (const QJsonValue &value : heads.toArray())
    {
        const QJsonObject head = value.toObject();
        ProjectCloneBranch branch;
        branch.id = head.value(QStringLiteral("branchId")).toString();
        branch.name = head.value(QStringLiteral("branch")).toString();
        const QJsonValue revision = head.value(QStringLiteral("revision"));
        const QJsonValue origin = head.value(QStringLiteral("originBranchId"));
        const QJsonValue fork = head.value(QStringLiteral("forkRevisionId"));
        if (!value.isObject() || !DvmdbIdentity::isUuidV7(branch.id) || branch.name.isEmpty()
            || !validOptionalId(revision) || !validOptionalId(origin) || !validOptionalId(fork)
            || ids.contains(branch.id) || names.contains(branch.name))
        {
            *error = QCoreApplication::translate("ProjectCloneTransfer", "The source database reported an invalid branch.");
            return false;
        }
        branch.head = nullableId(revision);
        branch.origin_branch_id = nullableId(origin);
        branch.fork_revision_id = nullableId(fork);
        branch.description = head.value(QStringLiteral("description")).toString();
        for (const QJsonValue &tag : head.value(QStringLiteral("tags")).toArray())
            branch.tags.append(tag.toString());
        ids.insert(branch.id);
        names.insert(branch.name);
        branches->append(branch);
    }
    return true;
}

bool ProjectClonePlan::order(const QVector<ProjectCloneBranch> &branches,
                             QVector<ProjectCloneBranch> *ordered, QString *error)
{
    ordered->clear();
    QHash<QString, int> index;
    for (int i = 0; i < branches.size(); ++i)
        index.insert(branches.at(i).id, i);
    // 0: not visited, 1: on the current path, 2: placed.
    QVector<int> state(branches.size(), 0);
    const std::function<bool(int)> place = [&](int i) -> bool
    {
        if (state.at(i) == 2) return true;
        if (state.at(i) == 1) return false;
        state[i] = 1;
        const QString &origin = branches.at(i).origin_branch_id;
        if (!origin.isEmpty() && origin != branches.at(i).id && index.contains(origin)
            && !place(index.value(origin)))
            return false;
        state[i] = 2;
        ordered->append(branches.at(i));
        return true;
    };
    for (int i = 0; i < branches.size(); ++i)
    {
        if (!place(i))
        {
            ordered->clear();
            *error = QCoreApplication::translate("ProjectCloneTransfer", "The project's branch provenance contains a cycle.");
            return false;
        }
    }
    return true;
}

qint64 ProjectClonePlan::chunkLimit(const QJsonObject &source_repository,
                                    const QJsonObject &target_repository)
{
    const qint64 source_limit = source_repository.value(QStringLiteral("transferLimits")).toObject()
        .value(QStringLiteral("maxBundleBytes")).toInteger(0);
    const qint64 target_limit = target_repository.value(QStringLiteral("transferLimits")).toObject()
        .value(QStringLiteral("maxBundleBytes")).toInteger(0);
    if (source_limit <= 0 || target_limit <= 0) return 0;
    return std::min(source_limit, target_limit);
}

QJsonObject ProjectCloneTransfer::Response::json() const
{
    const QJsonDocument document = QJsonDocument::fromJson(this->body);
    return document.isObject() ? document.object() : QJsonObject();
}

QString ProjectCloneTransfer::Response::errorMessage() const
{
    return json().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
}

ProjectCloneTransfer::ProjectCloneTransfer(QObject *parent, ConnectionFor connection_for)
    : QObject(parent), connection_for(std::move(connection_for)), manager(this)
{
}

ProjectCloneTransfer::~ProjectCloneTransfer()
{
    // Never call back into a destroyed owner.
    if (this->reply)
    {
        this->reply->disconnect(this);
        this->reply->abort();
    }
}

bool ProjectCloneTransfer::isBusy() const
{
    return this->busy;
}

bool ProjectCloneTransfer::resolve(const ProjectServerProfile &profile, Endpoint *endpoint,
                                   QString *error) const
{
    endpoint->profile = profile;
    endpoint->connection.reset();
    if (profile.pending || !DvmdbIdentity::isUuidV7(profile.id))
    {
        *error = tr("'%1' is not a verified database.").arg(profile.name);
        return false;
    }
    QString url = profile.endpoint;
    if (profile.connection_type == QStringLiteral("local"))
    {
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        QString start_error;
        url = embeddedDvmdbEndpoint(profile.local_path, profile.personal, nullptr, &start_error);
        if (url.isEmpty())
        {
            *error = start_error.isEmpty() ? tr("The embedded database '%1' could not start.").arg(profile.name)
                                           : start_error;
            return false;
        }
#else
        *error = tr("This build does not include embedded DVMDB support.");
        return false;
#endif
    }
    else
    {
        if (!profile.auth_mode.isEmpty() && profile.auth_mode != QStringLiteral("None"))
        {
            *error = tr("DVMDB REST authentication is not implemented; select 'None' for '%1'.").arg(profile.name);
            return false;
        }
        if (this->connection_for)
            endpoint->connection = this->connection_for(profile);
        if (endpoint->connection ? !endpoint->connection->isOpen()
                                 : !profile.wireguard_profile_id.isEmpty())
        {
            *error = tr("The connection to '%1' is not open. Refresh it and try again.").arg(profile.name);
            return false;
        }
    }
    QString root = url.trimmed();
    while (root.endsWith(QLatin1Char('/'))) root.chop(1);
    if (!root.endsWith(QStringLiteral("/dvmdb/v1"))) root += QStringLiteral("/dvmdb/v1");
    const QUrl parsed(root);
    if (!parsed.isValid() || (parsed.scheme() != QStringLiteral("http") && parsed.scheme() != QStringLiteral("https"))
        || parsed.host().isEmpty())
    {
        *error = tr("'%1' has an invalid database endpoint.").arg(profile.name);
        return false;
    }
    endpoint->root = root;
    return true;
}

void ProjectCloneTransfer::send(const Endpoint &endpoint, const QByteArray &method, const QString &path,
                                const QByteArray &body, const QByteArray &content_type, bool pinned,
                                int timeout_ms, qint64 max_bytes, Handler handler)
{
    QNetworkRequest request(QUrl(endpoint.root + path));
    request.setRawHeader("Accept", method == "GET" && content_type == bundle_mime
        ? bundle_mime : QByteArrayLiteral("application/json"));
    if (method != "GET")
        request.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(content_type));
    if (pinned)
        request.setRawHeader("X-AOWIS-Expected-Repository-Id", endpoint.profile.id.toUtf8());
    request.setTransferTimeout(timeout_ms);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *pending = nullptr;
    if (endpoint.connection)
        pending = method == "GET" ? endpoint.connection->get(request)
                                  : endpoint.connection->write(request, method, body);
    else
        pending = method == "GET" ? this->manager.get(request)
                                  : this->manager.sendCustomRequest(request, method, body);
    if (pending == nullptr)
    {
        Response response;
        response.transport_error = tr("the connection to '%1' is not open").arg(endpoint.profile.name);
        handler(response);
        return;
    }
    this->reply = pending;
    // Stop reading a reply that is larger than it may be.
    const std::shared_ptr<bool> too_large = std::make_shared<bool>(false);
    connect(pending, &QNetworkReply::downloadProgress, this,
            [pending, max_bytes, too_large](qint64 received, qint64)
    {
        if (received <= max_bytes || *too_large) return;
        *too_large = true;
        pending->abort();
    });
    connect(pending, &QNetworkReply::finished, this,
            [this, pending, max_bytes, too_large, handler = std::move(handler)]()
    {
        if (this->reply == pending) this->reply.clear();
        pending->deleteLater();
        Response response;
        response.status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = pending->readAll();
        response.bundle_head = pending->rawHeader("X-AOWIS-Bundle-Head");
        response.bundle_complete = pending->rawHeader("X-AOWIS-Bundle-Complete");
        if (this->cancelled)
        {
            finish(tr("The clone was cancelled."));
            return;
        }
        if (*too_large || response.body.size() > max_bytes)
        {
            response.status = 0;
            response.body.clear();
            response.transport_error = tr("the reply is larger than expected");
        }
        else if (pending->error() != QNetworkReply::NoError
                 && (response.status == 0 || pending->error() == QNetworkReply::OperationCanceledError))
        {
            // A reply cut short is a transport failure even after its status line.
            response.status = 0;
            response.body.clear();
            response.transport_error = pending->errorString();
        }
        handler(response);
    });
}

QString ProjectCloneTransfer::failure(const QString &what, const Response &response) const
{
    if (response.status == 0)
        return tr("%1: %2.").arg(what, response.transport_error);
    const QString message = response.errorMessage();
    return message.isEmpty() ? tr("%1 (HTTP %2).").arg(what).arg(response.status)
                             : tr("%1 (HTTP %2): %3").arg(what).arg(response.status).arg(message);
}

QString ProjectCloneTransfer::projectPath() const
{
    return QStringLiteral("/projects/") + segment(this->project_id);
}

QString ProjectCloneTransfer::branchPath(const ProjectCloneBranch &branch) const
{
    return projectPath() + QStringLiteral("/branches/") + segment(branch.name);
}

void ProjectCloneTransfer::start(const ProjectServerProfile &source, const QString &project_id,
                                 const ProjectServerProfile &target, ProgressCallback progress,
                                 Completion completion)
{
    if (this->busy)
    {
        if (completion) completion(tr("Another clone is in progress."), {});
        return;
    }
    this->progress = std::move(progress);
    this->completion = std::move(completion);
    this->busy = true;
    this->cancelled = false;
    this->project_id = project_id;
    this->project_name.clear();
    this->chunk_limit = 0;
    this->branches.clear();
    this->branch_index = -1;
    this->base.clear();
    this->copied_branches.clear();
    this->warnings.clear();
    this->state = Progress{};
    if (!DvmdbIdentity::isUuidV7(project_id))
    {
        finish(tr("The copied project has no DVMDB identity."));
        return;
    }
    if (source.id == target.id)
    {
        finish(tr("A project cannot be cloned into the database it comes from."));
        return;
    }
    QString error;
    if (!resolve(source, &this->source, &error) || !resolve(target, &this->target, &error))
    {
        finish(error);
        return;
    }
    readSourceRepository();
}

void ProjectCloneTransfer::cancel()
{
    if (!this->busy) return;
    this->cancelled = true;
    if (this->reply) this->reply->abort();
}

void ProjectCloneTransfer::readSourceRepository()
{
    send(this->source, "GET", QStringLiteral("/repository"), {}, {}, false, json_timeout_ms,
         json_reply_limit, [this](const Response &response)
    {
        const QJsonObject repository = response.json();
        if (response.status != 200)
        {
            finish(failure(tr("Reading the source database identity failed"), response));
            return;
        }
        if (repository.value(QStringLiteral("repositoryId")).toString() != this->source.profile.id)
        {
            finish(tr("The source database identity changed. Nothing was copied."));
            return;
        }
        const QString codec = ModelCodecCompatibility::error(repository);
        if (!codec.isEmpty())
        {
            finish(codec);
            return;
        }
        const QJsonObject source_repository = repository;
        send(this->target, "GET", QStringLiteral("/repository"), {}, {}, false, json_timeout_ms,
             json_reply_limit, [this, source_repository](const Response &target_response)
        {
            const QJsonObject target_repository = target_response.json();
            if (target_response.status != 200)
            {
                finish(failure(tr("Reading the target database identity failed"), target_response));
                return;
            }
            if (target_repository.value(QStringLiteral("repositoryId")).toString() != this->target.profile.id)
            {
                finish(tr("The target database identity changed. Nothing was copied."));
                return;
            }
            const QString codec = ModelCodecCompatibility::error(target_repository);
            if (!codec.isEmpty())
            {
                finish(codec);
                return;
            }
            this->chunk_limit = ProjectClonePlan::chunkLimit(source_repository, target_repository);
            if (this->chunk_limit <= 0)
            {
                finish(tr("A database does not support chunked project transfers. "
                          "Update DVMDB on both databases and try again."));
                return;
            }
            readTargetRole();
        });
    });
}

void ProjectCloneTransfer::readTargetRole()
{
    // The same role rules as every other GUI write: a personal database must
    // be private, any other database shared (or not yet configured).
    send(this->target, "GET", QStringLiteral("/repository/tracking"), {}, {}, false, json_timeout_ms,
         json_reply_limit, [this](const Response &response)
    {
        const QJsonObject tracking = response.json();
        if (response.status != 200)
        {
            finish(failure(tr("Reading the target database role failed"), response));
            return;
        }
        if (tracking.value(QStringLiteral("repositoryId")).toString() != this->target.profile.id)
        {
            finish(tr("The target database identity changed. Nothing was copied."));
            return;
        }
        const QString role = tracking.value(QStringLiteral("role")).toString();
        if (this->target.profile.personal && role != QStringLiteral("private"))
        {
            finish(tr("The target database is your personal database, but DVMDB does not report it as "
                      "private. Configure it in the Server Inspector. Nothing was copied."));
            return;
        }
        if (!this->target.profile.personal && role != QStringLiteral("shared")
            && role != QStringLiteral("unconfigured"))
        {
            finish(role == QStringLiteral("private")
                ? tr("The target database is private, but it is not your personal database. "
                     "Nothing was copied.")
                : tr("The target database reported an unknown role. Nothing was copied."));
            return;
        }
        readSourceProject();
    });
}

void ProjectCloneTransfer::readSourceProject()
{
    send(this->source, "GET", projectPath(), {}, {}, false, json_timeout_ms, json_reply_limit,
         [this](const Response &response)
    {
        const QJsonObject project = response.json();
        if (response.status != 200)
        {
            finish(failure(tr("Reading the project from the source database failed"), response));
            return;
        }
        this->project_name = project.value(QStringLiteral("name")).toString();
        if (project.value(QStringLiteral("id")).toString() != this->project_id || this->project_name.isEmpty())
        {
            finish(tr("The source database returned a different project. Nothing was copied."));
            return;
        }
        readSourceBranches();
    });
}

void ProjectCloneTransfer::readSourceBranches()
{
    // Every page lists all branch heads; one revision is enough.
    send(this->source, "GET", projectPath() + QStringLiteral("/revision-graph?limit=1"), {}, {}, false,
         json_timeout_ms, json_reply_limit, [this](const Response &response)
    {
        if (response.status != 200)
        {
            finish(failure(tr("Reading the project's branches failed"), response));
            return;
        }
        QVector<ProjectCloneBranch> listed;
        QString error;
        if (!ProjectClonePlan::parseHeads(response.json(), &listed, &error)
            || !ProjectClonePlan::order(listed, &this->branches, &error))
        {
            finish(error);
            return;
        }
        this->state.branch_count = this->branches.size();
        readTargetProject();
    });
}

void ProjectCloneTransfer::readTargetProject()
{
    send(this->target, "GET", projectPath(), {}, {}, false, json_timeout_ms, json_reply_limit,
         [this](const Response &response)
    {
        if (response.status == 200)
        {
            // An earlier, interrupted clone: continue. Its name may differ.
            if (response.json().value(QStringLiteral("id")).toString() != this->project_id)
            {
                finish(tr("The target database returned a different project. Nothing was copied."));
                return;
            }
            nextBranch();
            return;
        }
        if (response.status != 404)
        {
            finish(failure(tr("Checking the target database for the project failed"), response));
            return;
        }
        provisionTarget();
    });
}

void ProjectCloneTransfer::provisionTarget()
{
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("projectId"), this->project_id},
                                                      {QStringLiteral("name"), this->project_name}})
                                .toJson(QJsonDocument::Compact);
    send(this->target, "POST", QStringLiteral("/projects/clone-target"), body, "application/json", true,
         json_timeout_ms, json_reply_limit, [this](const Response &response)
    {
        if (response.status != 200 || response.json().value(QStringLiteral("id")).toString() != this->project_id)
        {
            finish(failure(tr("Creating the project on the target database failed"), response));
            return;
        }
        nextBranch();
    });
}

void ProjectCloneTransfer::nextBranch()
{
    ++this->branch_index;
    this->base.clear();
    if (this->branch_index >= this->branches.size())
    {
        finish({});
        return;
    }
    this->state.branch = this->branches.at(this->branch_index).name;
    this->state.branch_index = this->branch_index + 1;
    reportProgress();
    readTargetBranch();
}

void ProjectCloneTransfer::readTargetBranch()
{
    const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
    send(this->target, "GET", projectPath() + QStringLiteral("/branch-ids/") + segment(branch.id), {}, {},
         false, json_timeout_ms, json_reply_limit, [this](const Response &response)
    {
        const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
        const bool exists = response.status == 200;
        if (!exists && response.status != 404)
        {
            finish(failure(tr("Checking branch '%1' on the target database failed").arg(branch.name), response));
            return;
        }
        const QJsonObject existing = exists ? response.json() : QJsonObject();
        if (exists && existing.value(QStringLiteral("branch")).toString() != branch.name)
        {
            skipBranch(tr("Branch '%1' is named '%2' on the target database and was not updated.")
                .arg(branch.name, existing.value(QStringLiteral("branch")).toString()));
            return;
        }
        if (branch.head.isEmpty())
            copyUnbornBranch(existing, exists);
        else
            startHistory(existing, exists);
    });
}

void ProjectCloneTransfer::copyUnbornBranch(const QJsonObject &existing, bool exists)
{
    const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
    if (exists)
    {
        if (!existing.value(QStringLiteral("revision")).isNull())
        {
            skipBranch(tr("Branch '%1' has no commits here but already has commits on the target database.")
                .arg(branch.name));
            return;
        }
        copyMetadata(existing);
        return;
    }
    if (!branch.origin_branch_id.isEmpty() || !branch.fork_revision_id.isEmpty())
    {
        skipBranch(tr("Branch '%1' has no commits and records the branch it came from; "
                      "DVMDB cannot recreate that.").arg(branch.name));
        return;
    }
    QJsonArray tags;
    for (const QString &tag : branch.tags) tags.append(tag);
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("name"), branch.name}, {QStringLiteral("unborn"), true},
        {QStringLiteral("id"), branch.id}, {QStringLiteral("description"), branch.description},
        {QStringLiteral("tags"), tags}}).toJson(QJsonDocument::Compact);
    send(this->target, "POST", projectPath() + QStringLiteral("/branches"), body, "application/json", true,
         json_timeout_ms, json_reply_limit, [this](const Response &response)
    {
        const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
        if (response.status != 201 || response.json().value(QStringLiteral("branchId")).toString() != branch.id)
        {
            finish(failure(tr("Creating branch '%1' on the target database failed").arg(branch.name), response));
            return;
        }
        branchDone();
    });
}

void ProjectCloneTransfer::startHistory(const QJsonObject &existing, bool exists)
{
    const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
    if (exists)
    {
        const QString target_head = nullableId(existing.value(QStringLiteral("revision")));
        if (target_head == branch.head)
        {
            this->copied_branches.insert(branch.id);
            copyMetadata(existing);
            return;
        }
        // Resume: the target already has history up to its HEAD. A target
        // that diverged is refused by the import, not overwritten.
        this->base = target_head;
        requestChunk();
        return;
    }
    if (!branch.origin_branch_id.isEmpty() && !branch.fork_revision_id.isEmpty()
        && this->copied_branches.contains(branch.origin_branch_id))
    {
        createForkedBranch();
        return;
    }
    requestChunk();
}

void ProjectCloneTransfer::createForkedBranch()
{
    // The origin branch is on the target already: create the fork at its
    // revision with the same identity, then send only what came after.
    const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("name"), branch.name}, {QStringLiteral("fromBranchId"), branch.origin_branch_id},
        {QStringLiteral("revision"), branch.fork_revision_id}, {QStringLiteral("id"), branch.id}})
        .toJson(QJsonDocument::Compact);
    send(this->target, "POST", projectPath() + QStringLiteral("/branches"), body, "application/json", true,
         json_timeout_ms, json_reply_limit, [this](const Response &response)
    {
        const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
        if (response.status == 201 && response.json().value(QStringLiteral("branchId")).toString() == branch.id)
        {
            if (branch.head == branch.fork_revision_id)
            {
                this->copied_branches.insert(branch.id);
                copyMetadata({});
                return;
            }
            this->base = branch.fork_revision_id;
            requestChunk();
            return;
        }
        // The fork revision is no longer on the origin branch there (or the
        // origin is gone): send the branch's full history instead.
        if (response.status == 400 || response.status == 404)
        {
            requestChunk();
            return;
        }
        finish(failure(tr("Creating branch '%1' on the target database failed").arg(branch.name), response));
    });
}

void ProjectCloneTransfer::requestChunk()
{
    const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("maxBytes"), QString::number(this->chunk_limit));
    if (!this->base.isEmpty()) query.addQueryItem(QStringLiteral("base"), this->base);
    send(this->source, "GET", branchPath(branch) + QStringLiteral("/bundle?") + query.toString(QUrl::FullyEncoded),
         {}, bundle_mime, false, bundle_timeout_ms, this->chunk_limit, [this](const Response &response)
    {
        const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
        const QString head = QString::fromLatin1(response.bundle_head);
        if (response.status == 413)
        {
            skipBranch(tr("Branch '%1' contains a step larger than the transfer limit of %2 MiB and was not copied.")
                .arg(branch.name).arg(double(this->chunk_limit) / (1024.0 * 1024.0), 0, 'f', 1));
            return;
        }
        if (response.status != 200)
        {
            finish(failure(tr("Reading branch '%1' from the source database failed").arg(branch.name), response));
            return;
        }
        if (!DvmdbIdentity::isUuidV7(head)
            || (response.bundle_complete != "true" && response.bundle_complete != "false")
            || response.body.isEmpty() || head == this->base)
        {
            finish(tr("The source database sent an invalid part of branch '%1'. Nothing more was copied.")
                .arg(branch.name));
            return;
        }
        importChunk(response.body, head, response.bundle_complete == "true");
    });
}

void ProjectCloneTransfer::importChunk(const QByteArray &bundle, const QString &head, bool complete)
{
    const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
    const qint64 size = bundle.size();
    send(this->target, "POST", branchPath(branch) + QStringLiteral("/bundle"), bundle, bundle_mime, true,
         bundle_timeout_ms, json_reply_limit, [this, head, complete, size](const Response &response)
    {
        const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
        const bool wrong_target = response.json().value(QStringLiteral("error")).toObject()
            .value(QStringLiteral("code")).toString() == QStringLiteral("repository_mismatch");
        if (response.status == 409 && wrong_target)
        {
            finish(tr("The target database identity changed. The clone stopped."));
            return;
        }
        if (response.status == 409)
        {
            // Both databases committed to this branch: fast-forward only.
            skipBranch(tr("Branch '%1' has changed on both databases and was left as it is on the target.")
                .arg(branch.name));
            return;
        }
        if ((response.status != 200 && response.status != 201)
            || response.json().value(QStringLiteral("head")).toString() != head)
        {
            finish(failure(tr("Copying branch '%1' to the target database failed").arg(branch.name), response));
            return;
        }
        ++this->state.chunks;
        this->state.bytes += size;
        reportProgress();
        if (complete)
        {
            // A commit made on the source during the clone is included: the
            // last chunk ends at the HEAD at the time of that request.
            this->copied_branches.insert(branch.id);
            copyMetadata({});
            return;
        }
        this->base = head;
        requestChunk();
    });
}

void ProjectCloneTransfer::copyMetadata(const QJsonObject &existing)
{
    const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
    QStringList existing_tags;
    for (const QJsonValue &tag : existing.value(QStringLiteral("tags")).toArray())
        existing_tags.append(tag.toString());
    const bool known = !existing.isEmpty();
    const bool equal = known ? existing.value(QStringLiteral("description")).toString() == branch.description
                                   && existing_tags == branch.tags
                             : branch.description.isEmpty() && branch.tags.isEmpty();
    if (equal)
    {
        branchDone();
        return;
    }
    QJsonArray tags;
    for (const QString &tag : branch.tags) tags.append(tag);
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("expectedName"), branch.name}, {QStringLiteral("name"), branch.name},
        {QStringLiteral("description"), branch.description}, {QStringLiteral("tags"), tags}})
        .toJson(QJsonDocument::Compact);
    send(this->target, "PATCH", projectPath() + QStringLiteral("/branch-ids/") + segment(branch.id), body,
         "application/json", true, json_timeout_ms, json_reply_limit, [this](const Response &response)
    {
        const ProjectCloneBranch &branch = this->branches.at(this->branch_index);
        if (response.status != 200)
        {
            skipBranch(tr("The description and tags of branch '%1' were not copied: %2")
                .arg(branch.name, response.errorMessage().isEmpty()
                    ? QStringLiteral("HTTP %1").arg(response.status) : response.errorMessage()));
            return;
        }
        branchDone();
    });
}

void ProjectCloneTransfer::branchDone()
{
    nextBranch();
}

void ProjectCloneTransfer::skipBranch(const QString &warning)
{
    this->warnings.append(warning);
    nextBranch();
}

void ProjectCloneTransfer::reportProgress()
{
    if (this->progress) this->progress(this->state);
}

void ProjectCloneTransfer::finish(const QString &error)
{
    if (!this->busy) return;
    this->busy = false;
    this->cancelled = false;
    const Completion done = std::move(this->completion);
    this->completion = {};
    this->progress = {};
    const QStringList reported = this->warnings;
    if (done) done(error, reported);
}
