#include "projects/model_codec_compatibility.h"
#include "projects/project_source_reader.h"
#include "projects/dvmdb_identity.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
#include "projects/embedded_dvmdb_host.h"
#endif

namespace
{
constexpr int graph_page_size = 256;
constexpr int maximum_projects = 10000;
constexpr int maximum_branches = 25000;
constexpr int maximum_revisions_per_project = 25000;
constexpr int maximum_total_revisions = 60000;
constexpr int maximum_json_bytes = 4 * 1024 * 1024;
constexpr int maximum_cursor_bytes = 180000;

QString segment(const QString &part)
{
    return QString::fromLatin1(part.toUtf8().toPercentEncoding());
}


// Canonical DVMDB P0: the transport always addresses revisions by UUIDv7.
// SHA-256 values authenticate their immutable records and parent bindings.
bool parseRevision(const QJsonValue &value, SourceRevision &revision)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    revision.id = object.value(QStringLiteral("id")).toString();
    revision.record_sha256 = object.value(QStringLiteral("recordHash")).toString();
    revision.message = object.value(QStringLiteral("message")).toString();
    revision.original_message = object.value(QStringLiteral("originalMessage")).toString();
    if ((!object.value(QStringLiteral("message")).isUndefined()
         && !object.value(QStringLiteral("message")).isString())
        || (!object.value(QStringLiteral("originalMessage")).isUndefined()
            && !object.value(QStringLiteral("originalMessage")).isString())
        || revision.message.toUtf8().size() > 65536) return false;
    const QJsonValue parents = object.value(QStringLiteral("parents"));
    const QJsonValue hashes = object.value(QStringLiteral("parentRecordHashes"));
    if (!DvmdbIdentity::isUuidV7(revision.id)
        || !DvmdbIdentity::isLegacySha256(revision.record_sha256)
        || !parents.isArray() || !hashes.isArray()
        || parents.toArray().size() != hashes.toArray().size()
        || parents.toArray().size() > 2) return false;
    QSet<QString> unique_parents;
    for (int i = 0; i < parents.toArray().size(); ++i)
    {
        const QString parent = parents.toArray().at(i).toString();
        const QString digest = hashes.toArray().at(i).toString();
        if (!DvmdbIdentity::isUuidV7(parent)
            || !DvmdbIdentity::isLegacySha256(digest)
            || parent == revision.id || unique_parents.contains(parent)) return false;
        unique_parents.insert(parent);
        revision.parents.append(parent);
        revision.parent_record_sha256.append(digest);
    }
    return true;
}

// A graph is published only if every captured branch HEAD and every parent
// reference is actually present. A partial graph cannot masquerade as complete.
bool validateCompleteGraph(const SourceProject &project)
{
    QHash<QString, QString> hashes;
    QHash<QString, QStringList> parent_links;
    QSet<QString> branch_ids;
    QSet<QString> branch_names;
    for (const SourceRevision &revision : project.revisions)
    {
        if (!DvmdbIdentity::isUuidV7(revision.id)
            || !DvmdbIdentity::isLegacySha256(revision.record_sha256)
            || hashes.contains(revision.id)
            || revision.parents.size() != revision.parent_record_sha256.size()) return false;
        hashes.insert(revision.id, revision.record_sha256);
        parent_links.insert(revision.id, revision.parents);
    }
    for (const SourceBranch &branch : project.branches)
    {
        if (branch.name.isEmpty() || branch_names.contains(branch.name)
            || !DvmdbIdentity::isUuidV7(branch.id) || branch_ids.contains(branch.id)
            || (branch.head.isEmpty()
                ? !branch.record_sha256.isEmpty()
                : (!DvmdbIdentity::isUuidV7(branch.head)
                   || hashes.value(branch.head) != branch.record_sha256))) return false;
        if (!branch.fork_revision_id.isEmpty())
        {
            if (branch.origin_branch_id.isEmpty()
                || !DvmdbIdentity::isUuidV7(branch.fork_revision_id)
                || !hashes.contains(branch.fork_revision_id)) return false;
            // Fork provenance is metadata, but it must identify a genuine
            // ancestor of this HEAD. Never turn it into a graph parent edge.
            QSet<QString> visited;
            QStringList pending{branch.head};
            bool reached_fork = false;
            while (!pending.isEmpty())
            {
                const QString revision = pending.takeLast();
                if (visited.contains(revision)) continue;
                visited.insert(revision);
                if (revision == branch.fork_revision_id)
                {
                    reached_fork = true;
                    break;
                }
                pending.append(parent_links.value(revision));
            }
            if (!reached_fork) return false;
        }
        branch_names.insert(branch.name);
        branch_ids.insert(branch.id);
    }
    QHash<QString, int> remaining_parents;
    QHash<QString, QStringList> children;
    QStringList roots;
    for (const SourceRevision &revision : project.revisions)
    {
        remaining_parents.insert(revision.id, revision.parents.size());
        if (revision.parents.isEmpty()) roots.append(revision.id);
        for (int i = 0; i < revision.parents.size(); ++i)
        {
            const QString parent = revision.parents.at(i);
            if (hashes.value(parent) != revision.parent_record_sha256.at(i)) return false;
            children[parent].append(revision.id);
        }
    }
    int consumed = 0;
    while (!roots.isEmpty())
    {
        const QString id = roots.takeLast();
        ++consumed;
        for (const QString &child : children.value(id))
        {
            const int count = remaining_parents.value(child) - 1;
            remaining_parents.insert(child, count);
            if (count == 0) roots.append(child);
        }
    }
    return consumed == project.revisions.size();
}

}

ProjectSourceReader::ProjectSourceReader(QObject *parent)
    : QObject(parent), manager(this)
{
}

void ProjectSourceReader::cancel()
{
    ++this->generation;
    this->completion = {};
    if (this->reply)
    {
        QNetworkReply *previous = this->reply;
        this->reply.clear();
        previous->abort();
    }
    this->manager.clearConnectionCache();
}

void ProjectSourceReader::load(const ProjectServerProfile &source_profile, Completion completion)
{
    cancel();
    ProjectServerProfile profile = source_profile;
    // Standalone's local catalogues are owned by one in-process service.
    // The same bounded read routes are used for both local and remote sources.
    if (profile.connection_type == QStringLiteral("local"))
    {
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        QString error;
        const QString endpoint = embeddedDvmdbEndpoint(profile.local_path, profile.personal,
                                                       nullptr, &error);
        if (endpoint.isEmpty())
        {
            if (completion) completion({}, error.isEmpty()
                ? tr("Embedded DVMDB could not start.") : error);
            return;
        }
        profile.connection_type = QStringLiteral("remote");
        profile.endpoint = endpoint;
#else
        if (completion) completion({}, tr("This build does not include embedded DVMDB support."));
        return;
#endif
    }
    this->profile = profile;
    this->completion = std::move(completion);
    this->snapshot = {};
    this->step = Step::Repository;
    this->project_index = 0;
    this->total_branches = 0;
    this->total_revisions = 0;
    this->next_cursor.clear();
    this->used_cursors.clear();
    this->revisions_seen.clear();
    const quint64 requested = this->generation;
    if (profile.connection_type != QStringLiteral("remote"))
    {
        complete(requested, tr("Unsupported database connection type."));
        return;
    }
    if (profile.auth_mode != QStringLiteral("None") && !profile.auth_mode.isEmpty())
    {
        complete(requested, tr("DVMDB REST authentication is not implemented; select 'None' to connect."));
        return;
    }
    const QUrl url(profile.endpoint);
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
        || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
    {
        complete(requested, tr("Invalid REST database endpoint."));
        return;
    }
    nextRestRequest(requested);
}

void ProjectSourceReader::nextRestRequest(quint64 requested)
{
    if (requested != this->generation) return;
    if (this->step == Step::Graph && this->project_index >= this->snapshot.projects.size())
    {
        complete(requested);
        return;
    }
    QString path = this->step == Step::Repository
        ? QStringLiteral("/repository") : QStringLiteral("/projects");
    if (this->step == Step::Graph)
    {
        path += QStringLiteral("/") + segment(this->snapshot.projects[this->project_index].id)
            + QStringLiteral("/revision-graph?limit=%1").arg(graph_page_size);
        if (!this->next_cursor.isEmpty())
            path += QStringLiteral("&cursor=") + segment(this->next_cursor);
    }
    QString root = this->profile.endpoint.trimmed();
    while (root.endsWith(QLatin1Char('/'))) root.chop(1);
    if (!root.endsWith(QStringLiteral("/dvmdb/v1"))) root += QStringLiteral("/dvmdb/v1");
    QNetworkRequest request(QUrl(root + path));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(12000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::ManualRedirectPolicy);
    this->reply = this->manager.get(request);
    QNetworkReply *pending = this->reply;
    const QPointer<QNetworkReply> deadline_reply(pending);
    QTimer::singleShot(15000, this, [this, requested, deadline_reply]
    {
        if (requested == this->generation && deadline_reply) deadline_reply->abort();
    });
    connect(pending, &QNetworkReply::finished, this, [this, requested, pending]
    {
        if (requested != this->generation) { pending->deleteLater(); return; }
        this->reply.clear();
        pending->deleteLater();
        if (pending->error() != QNetworkReply::NoError)
        {
            complete(requested, tr("DVMDB request failed: %1").arg(pending->errorString()));
            return;
        }
        if (pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        {
            complete(requested, tr("DVMDB request returned HTTP %1")
                .arg(pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
            return;
        }
        if (pending->bytesAvailable() > maximum_json_bytes)
        {
            complete(requested, tr("DVMDB response exceeds the size limit."));
            return;
        }
        QString error;
        if (!processRestDocument(pending->readAll(), error))
        {
            complete(requested, error);
            return;
        }
        // Avoid deep recursive callbacks for large paginated histories.
        QTimer::singleShot(0, this, [this, requested] { nextRestRequest(requested); });
    });
}

bool ProjectSourceReader::processRestDocument(const QByteArray &json, QString &error)
{
    QJsonParseError parsing;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parsing);
    if (parsing.error != QJsonParseError::NoError || !document.isObject())
    {
        error = tr("Invalid DVMDB JSON response.");
        return false;
    }
    const QJsonObject object = document.object();
    if (this->step == Step::Repository)
    {
        error = ModelCodecCompatibility::error(object);
        if (!error.isEmpty()) return false;
        this->snapshot.repository_id = object.value(QStringLiteral("repositoryId")).toString();
        if (!DvmdbIdentity::isUuidV7(this->snapshot.repository_id))
        {
            error = tr("Invalid DVMDB repository UUIDv7.");
            return false;
        }
        this->step = Step::Projects;
        return true;
    }
    if (this->step == Step::Projects)
    {
        const QJsonValue projects = object.value(QStringLiteral("projects"));
        if (!projects.isArray() || projects.toArray().size() > maximum_projects)
        { error = tr("Invalid or oversized DVMDB project list."); return false; }
        QSet<QString> project_ids;
        for (const QJsonValue &value : projects.toArray())
        {
            if (!value.isObject()) { error = tr("Invalid project record."); return false; }
            const QJsonObject item = value.toObject();
            SourceProject project;
            project.id = item.value(QStringLiteral("id")).toString();
            project.name = item.value(QStringLiteral("name")).toString();
            if (!DvmdbIdentity::isUuidV7(project.id) || project.name.isEmpty()
                || project_ids.contains(project.id))
            { error = tr("Invalid or duplicate project identity."); return false; }
            project_ids.insert(project.id);
            this->snapshot.projects.append(project);
        }
        this->step = Step::Graph;
        return true;
    }
    const QJsonValue heads = object.value(QStringLiteral("heads"));
    const QJsonValue revisions = object.value(QStringLiteral("revisions"));
    const QJsonValue complete = object.value(QStringLiteral("complete"));
    const QJsonValue cursor = object.value(QStringLiteral("nextCursor"));
    if (!heads.isArray() || !revisions.isArray() || !complete.isBool()
        || revisions.toArray().size() > graph_page_size || heads.toArray().size() > maximum_branches
        || (cursor.isString() && cursor.toString().toUtf8().size() > maximum_cursor_bytes)
        || (!cursor.isString() && !cursor.isNull()))
    { error = tr("Invalid DVMDB revision graph page."); return false; }

    SourceProject &project = this->snapshot.projects[this->project_index];
    QList<SourceBranch> page_heads;
    QSet<QString> names;
    for (const QJsonValue &value : heads.toArray())
    {
        if (!value.isObject()) { error = tr("Invalid DVMDB graph HEAD."); return false; }
        const QJsonObject head = value.toObject();
        SourceBranch branch;
        branch.name = head.value(QStringLiteral("branch")).toString();
        const QJsonValue head_revision = head.value(QStringLiteral("revision"));
        const QJsonValue head_digest = head.value(QStringLiteral("recordHash"));
        if ((!head_revision.isString() && !head_revision.isNull())
            || (!head_digest.isString() && !head_digest.isNull())
            || head_revision.isNull() != head_digest.isNull())
        { error = tr("Invalid nullable DVMDB branch HEAD."); return false; }
        branch.head = head_revision.toString();
        branch.id = head.value(QStringLiteral("branchId")).toString();
        branch.record_sha256 = head_digest.toString();
        const QJsonValue metadata_description = head.value(QStringLiteral("description"));
        const QJsonValue metadata_tags = head.value(QStringLiteral("tags"));
        if ((!metadata_description.isUndefined() && !metadata_description.isString())
            || (!metadata_tags.isUndefined() && !metadata_tags.isArray()))
        { error = tr("Invalid DVMDB branch metadata."); return false; }
        branch.description = metadata_description.toString();
        if (branch.description.toUtf8().size() > 512)
        { error = tr("Branch description exceeds the size limit."); return false; }
        if (metadata_tags.isArray())
        {
            if (metadata_tags.toArray().size() > 12)
            { error = tr("Too many DVMDB branch tags."); return false; }
            for (const QJsonValue &tag : metadata_tags.toArray())
            {
                if (!tag.isString() || tag.toString().toUtf8().size() > 32
                    || tag.toString().isEmpty() || branch.tags.contains(tag.toString()))
                { error = tr("Invalid DVMDB branch tag."); return false; }
                branch.tags.append(tag.toString());
            }
        }
        const QJsonValue origin = head.value(QStringLiteral("originBranchId"));
        if (!origin.isUndefined() && !origin.isNull())
        {
            if (!origin.isString())
            { error = tr("Invalid DVMDB branch creation origin."); return false; }
            branch.origin_branch_id = origin.toString();
            if (!DvmdbIdentity::isUuidV7(branch.origin_branch_id)
                || branch.origin_branch_id == branch.id)
            { error = tr("Invalid DVMDB branch creation origin."); return false; }
        }
        const QJsonValue fork = head.value(QStringLiteral("forkRevisionId"));
        if (!fork.isUndefined() && !fork.isNull())
        {
            if (!fork.isString())
            { error = tr("Invalid DVMDB branch fork revision."); return false; }
            branch.fork_revision_id = fork.toString();
            if (!DvmdbIdentity::isUuidV7(branch.fork_revision_id)
                || branch.origin_branch_id.isEmpty())
            { error = tr("Invalid DVMDB branch fork revision."); return false; }
        }
        if (branch.name.isEmpty() || !DvmdbIdentity::isUuidV7(branch.id)
            || (branch.head.isEmpty() ? !branch.record_sha256.isEmpty()
                : (!DvmdbIdentity::isUuidV7(branch.head)
                   || !DvmdbIdentity::isLegacySha256(branch.record_sha256)))
            || names.contains(branch.name))
        { error = tr("Invalid or repeated DVMDB branch HEAD."); return false; }
        names.insert(branch.name);
        page_heads.append(branch);
    }
    if (this->next_cursor.isEmpty())
    {
        this->total_branches += page_heads.size();
        if (this->total_branches > maximum_branches)
        { error = tr("Too many branches to display."); return false; }
        project.branches = page_heads;
    }
    else
    {
        if (page_heads.size() != project.branches.size())
        { error = tr("DVMDB graph HEADs changed during pagination."); return false; }
        for (int index = 0; index < page_heads.size(); ++index)
            if (page_heads[index].name != project.branches[index].name
                || page_heads[index].id != project.branches[index].id
                || page_heads[index].head != project.branches[index].head
                || page_heads[index].record_sha256 != project.branches[index].record_sha256
                || page_heads[index].origin_branch_id != project.branches[index].origin_branch_id
                || page_heads[index].fork_revision_id != project.branches[index].fork_revision_id
                || page_heads[index].description != project.branches[index].description
                || page_heads[index].tags != project.branches[index].tags)
            { error = tr("DVMDB graph HEADs changed during pagination."); return false; }
    }
    if (revisions.toArray().isEmpty() && !complete.toBool())
    { error = tr("DVMDB returned an empty intermediate graph page."); return false; }
    for (const QJsonValue &value : revisions.toArray())
    {
        SourceRevision revision;
        if (!parseRevision(value, revision) || this->revisions_seen.contains(revision.id))
        { error = tr("Invalid or repeated DVMDB graph revision."); return false; }
        this->revisions_seen.insert(revision.id, project.revisions.size());
        project.revisions.append(revision);
        ++this->total_revisions;
        if (project.revisions.size() > maximum_revisions_per_project
            || this->total_revisions > maximum_total_revisions)
        { error = tr("Revision graph exceeds the GUI's safe display limit."); return false; }
    }
    if (complete.toBool())
    {
        if (!cursor.isNull() || !validateCompleteGraph(project))
        { error = tr("DVMDB returned an incomplete revision graph."); return false; }
        project.graph_complete = true;
        ++this->project_index;
        this->next_cursor.clear();
        this->used_cursors.clear();
        this->revisions_seen.clear();
    }
    else
    {
        const QString next = cursor.toString();
        if (next.isEmpty() || this->used_cursors.contains(next))
        { error = tr("DVMDB revision graph cursor did not advance."); return false; }
        this->used_cursors.insert(next);
        this->next_cursor = next;
    }
    return true;
}

void ProjectSourceReader::complete(quint64 requested, const QString &error)
{
    if (requested != this->generation) return;
    Completion done = std::move(this->completion);
    this->completion = {};
    if (done) done(error.isEmpty() ? this->snapshot : SourceSnapshot{}, error);
}
