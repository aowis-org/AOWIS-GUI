#include "projects/project_source_writer.h"
#include "projects/repository_probe.h"
#include "projects/dvmdb_identity.h"
#include "projects/model_codec_compatibility.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <stdexcept>

#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
#include "projects/embedded_dvmdb_host.h"
#endif

namespace
{
QString segment(const QString &part)
{
    return QString::fromLatin1(part.toUtf8().toPercentEncoding());
}

QString validateProfile(const ProjectServerProfile &profile)
{
    if (profile.pending || !DvmdbIdentity::isUuidV7(profile.id))
        return QCoreApplication::translate("ProjectSourceWriter", "Select a verified database with a UUIDv7 repository identity.");
    if (profile.connection_type == QStringLiteral("local"))
    {
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        if (!QDir::isAbsolutePath(profile.local_path))
            return QCoreApplication::translate("ProjectSourceWriter", "An absolute embedded DVMDB directory is required.");
        return {};
#else
        return QCoreApplication::translate("ProjectSourceWriter", "This build does not include embedded DVMDB support.");
#endif
    }
    if (profile.connection_type != QStringLiteral("remote"))
        return QCoreApplication::translate("ProjectSourceWriter", "Unsupported database connection type.");
    if (profile.auth_mode != QStringLiteral("None") && !profile.auth_mode.isEmpty())
        return QCoreApplication::translate("ProjectSourceWriter", "DVMDB REST authentication is not implemented; select 'None'.");
    const QUrl url(profile.endpoint);
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
        || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return QCoreApplication::translate("ProjectSourceWriter", "Invalid REST database endpoint.");
    return {};
}

} // namespace

ProjectSourceWriter::ProjectSourceWriter(QObject *parent, ConnectionFor connection_for)
    : QObject(parent), connection_for(std::move(connection_for)), manager(this)
{
}

// Preflight the *selected* repository and then let the server enforce the
// same identity on the mutation itself. This prevents replacement of the
// endpoint between the two HTTP requests from silently redirecting a write.
void ProjectSourceWriter::sendPinned(const ProjectServerProfile &profile,
                                     const QString &endpoint, const QByteArray &method,
                                     const QString &path, const QJsonObject &body,
                                     std::function<void(QNetworkReply *)> completed)
{
    QString root = endpoint.trimmed();
    while (root.endsWith(QLatin1Char('/'))) root.chop(1);
    if (!root.endsWith(QStringLiteral("/dvmdb/v1"))) root += QStringLiteral("/dvmdb/v1");
    QNetworkRequest verify(QUrl(root + QStringLiteral("/repository/tracking")));
    verify.setRawHeader("Accept", "application/json");
    verify.setTransferTimeout(8000);
    verify.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                        QNetworkRequest::ManualRedirectPolicy);
    if (profile.connection_type == QStringLiteral("remote") && this->connection_for)
        this->active_connection = this->connection_for(profile);
    // A GUI remote write must stay on the selected catalogue session for both requests.
    if (this->active_connection)
        this->reply = this->active_connection->get(verify);
    else if (profile.connection_type == QStringLiteral("local")
             || (!this->connection_for && profile.wireguard_profile_path.isEmpty()))
        this->reply = this->manager.get(verify);
    if (!this->reply)
    {
        this->finish({}, tr("DVMDB repository identity could not be verified. No write was sent."));
        return;
    }
    QNetworkReply *identity = this->reply;
    const QPointer<QNetworkReply> deadline(identity);
    QTimer::singleShot(10000, this, [deadline] { if (deadline) deadline->abort(); });
    connect(identity, &QNetworkReply::finished, this,
            [this, identity, root, profile, method, path, body, completed = std::move(completed)]() mutable
    {
        if (this->reply == identity) this->reply.clear();
        const int status = identity->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = identity->isReadable() ? identity->readAll() : QByteArray();
        const bool transport_ok = status == 200 && identity->error() == QNetworkReply::NoError;
        identity->deleteLater();
        if (!transport_ok || payload.size() > 1024 * 1024)
        {
            QString message = tr("DVMDB repository identity could not be verified. No write was sent.");
            if (!transport_ok)
                message += QStringLiteral(" (%1)").arg(status > 0 ? QStringLiteral("HTTP %1").arg(status)
                                                                 : identity->errorString());
            finish({}, message);
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (!document.isObject())
        {
            finish({}, tr("DVMDB repository identity reply is malformed. No write was sent."));
            return;
        }
        const QJsonObject repository = document.object();
        const QString codec_error = ModelCodecCompatibility::error(repository);
        if (!codec_error.isEmpty())
        {
            finish({}, codec_error);
            return;
        }
        if (repository.value(QStringLiteral("repositoryId")).toString() != profile.id)
        {
            finish({}, tr("Repository identity changed. No metadata write was sent."));
            return;
        }
        const QString role = repository.value(QStringLiteral("role")).toString();
        // Project and branch metadata can be created before a new DVMDB has
        // chosen its one-time deployment role. Never silently initialize it
        // as shared: the user may still want to designate it as personal.
        // Editing drafts still require an explicitly private repository.
        if (profile.personal && role != QStringLiteral("private"))
        {
            finish({}, tr("DVMDB repository role is not private. Configure this database "
                          "as personal in the Server Inspector before writing. No write was sent."));
            return;
        }
        if (!profile.personal && role != QStringLiteral("shared")
            && role != QStringLiteral("unconfigured"))
        {
            finish({}, role == QStringLiteral("private")
                ? tr("DVMDB repository role is private, but this database is not designated "
                     "as personal. Select it in the Server Inspector before writing. No write was sent.")
                : tr("DVMDB reported an unknown repository role. No write was sent."));
            return;
        }
        QNetworkRequest call(QUrl(root + path));
        call.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        call.setRawHeader("Accept", "application/json");
        call.setRawHeader("X-AOWIS-Expected-Repository-Id", profile.id.toUtf8());
        call.setTransferTimeout(12000);
        call.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                          QNetworkRequest::ManualRedirectPolicy);
        const QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
        if (this->active_connection)
            this->reply = this->active_connection->write(call, method, data);
        else
            this->reply = method == "PATCH" ? this->manager.sendCustomRequest(call, method, data)
                                            : this->manager.post(call, data);
        if (!this->reply)
        {
            this->finish({}, tr("DVMDB repository identity could not be verified. No write was sent."));
            return;
        }
        QNetworkReply *pending = this->reply;
        const QPointer<QNetworkReply> write_deadline(pending);
        QTimer::singleShot(15000, this, [write_deadline] {
            if (write_deadline) write_deadline->abort();
        });
        connect(pending, &QNetworkReply::finished, this, [pending, completed = std::move(completed)]() mutable {
            completed(pending);
        });
    });
}

bool ProjectSourceWriter::isBusy() const
{
    return this->busy;
}

void ProjectSourceWriter::createProject(const ProjectServerProfile &profile, const QString &name,
                                       Completion completion)
{
    start(profile, Operation::Project, {}, name, {}, {}, {}, {}, std::move(completion));
}

void ProjectSourceWriter::createBranch(const ProjectServerProfile &profile,
                                      const QString &project_id, const QString &name,
                                      const QString &from_branch_id, const QString &from_revision,
                                      Completion completion)
{
    start(profile, Operation::Branch, project_id, name, from_branch_id, from_revision,
          {}, {}, std::move(completion));
}

void ProjectSourceWriter::createUnbornBranch(const ProjectServerProfile &profile,
                                              const QString &project_id, const QString &name,
                                              const QStringList &tags, const QString &description,
                                              Completion completion)
{
    start(profile, Operation::UnbornBranch, project_id, name, {}, {}, tags, description,
          std::move(completion));
}

void ProjectSourceWriter::renameProject(const ProjectServerProfile &profile,
                                        const QString &project_id, const QString &expected_name,
                                        const QString &name, Completion completion)
{
    if (this->busy)
    {
        if (completion) completion({}, tr("Another database write is in progress."));
        return;
    }
    const QString validation = validateProfile(profile);
    if (!validation.isEmpty() || !DvmdbIdentity::isUuidV7(project_id)
        || expected_name.isEmpty() || name.trimmed().isEmpty()
        || name.toUtf8().size() > 200)
    {
        if (completion) completion({}, validation.isEmpty()
            ? tr("Invalid project ID or name.") : validation);
        return;
    }
    ProjectServerProfile request_profile = profile;
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
        request_profile.connection_type = QStringLiteral("remote");
        request_profile.endpoint = endpoint;
#else
        if (completion) completion({}, tr("Embedded DVMDB is unavailable."));
        return;
#endif
    }
    const QString path = QStringLiteral("/projects/") + segment(project_id);
    const QJsonObject body{{QStringLiteral("expectedName"), expected_name},
                           {QStringLiteral("name"), name}};
    this->busy = true;
    this->completion = std::move(completion);
    sendPinned(profile, request_profile.endpoint, "PATCH", path, body,
               [this, project_id, name](QNetworkReply *pending)
    {
        if (this->reply == pending) this->reply.clear();
        pending->deleteLater();
        const int status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (pending->bytesAvailable() > 1024 * 1024)
        {
            finish({}, tr("DVMDB rename response is too large. Refresh before retrying."));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(pending->readAll());
        const QJsonObject object = document.isObject() ? document.object() : QJsonObject();
        if (status != 200 || pending->error() != QNetworkReply::NoError)
        {
            const QString error = object.value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString();
            finish({}, status == 0
                ? tr("The rename response was lost. Refresh before retrying: %1")
                    .arg(pending->errorString())
                : error.isEmpty() ? tr("DVMDB rename failed (HTTP %1).").arg(status)
                                  : tr("DVMDB rename failed (HTTP %1): %2").arg(status).arg(error));
            return;
        }
        if (object.value(QStringLiteral("id")).toString() != project_id
            || object.value(QStringLiteral("name")).toString() != name)
        {
            finish({}, tr("Rename response did not match the project. Refresh before retrying."));
            return;
        }
        finish(project_id, {});
    });
}

void ProjectSourceWriter::updateBranchProperties(
        const ProjectServerProfile &profile, const QString &project_id,
        const QString &branch_id, const QString &expected_name, const QString &name,
        const QStringList &tags, const QString &description, Completion completion)
{
    QJsonArray tag_array;
    for (const QString &tag : tags) tag_array.append(tag);
    patchProperties(profile, QStringLiteral("/projects/") + segment(project_id)
        + QStringLiteral("/branch-ids/") + segment(branch_id),
        QJsonObject{{QStringLiteral("expectedName"), expected_name},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("description"), description},
                    {QStringLiteral("tags"), tag_array}}, QStringLiteral("branchId"), branch_id,
        std::move(completion));
}

void ProjectSourceWriter::updateRevisionMessage(
        const ProjectServerProfile &profile, const QString &project_id,
        const QString &revision_id, const QString &expected_message,
        const QString &message, Completion completion)
{
    patchProperties(profile, QStringLiteral("/projects/") + segment(project_id)
        + QStringLiteral("/revisions/") + segment(revision_id) + QStringLiteral("/message"),
        QJsonObject{{QStringLiteral("expectedMessage"), expected_message},
                    {QStringLiteral("message"), message}},
        QStringLiteral("revision"), revision_id, std::move(completion));
}

void ProjectSourceWriter::deleteBranch(const ProjectServerProfile &profile,
        const QString &project_id, const QString &branch_id, const QString &expected_head,
        Completion completion) {
    if (!DvmdbIdentity::isUuidV7(project_id) || !DvmdbIdentity::isUuidV7(branch_id)
        || (!expected_head.isEmpty() && !DvmdbIdentity::isUuidV7(expected_head))) {
        if (completion) completion({}, tr("Invalid branch deletion identity."));
        return;
    }
    destructiveWrite(profile, QStringLiteral("/projects/%1/branch-ids/%2/delete")
        .arg(project_id, branch_id), QJsonObject{{QStringLiteral("expectedHead"),
        expected_head.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(expected_head)},
        {QStringLiteral("discardWorkingCopies"), true}},
        std::move(completion));
}

void ProjectSourceWriter::truncateBranch(const ProjectServerProfile &profile,
        const QString &project_id, const QString &branch_id, const QString &expected_head,
        const QString &first_removed, Completion completion) {
    if (!DvmdbIdentity::isUuidV7(project_id) || !DvmdbIdentity::isUuidV7(branch_id)
        || !DvmdbIdentity::isUuidV7(expected_head)
        || !DvmdbIdentity::isUuidV7(first_removed)) {
        if (completion) completion({}, tr("Invalid revision deletion identity."));
        return;
    }
    destructiveWrite(profile, QStringLiteral("/projects/%1/branch-ids/%2/truncate")
        .arg(project_id, branch_id), QJsonObject{
            {QStringLiteral("expectedHead"), expected_head},
            {QStringLiteral("firstRemoved"), first_removed},
            {QStringLiteral("discardWorkingCopies"), true}}, std::move(completion));
}

void ProjectSourceWriter::destructiveWrite(const ProjectServerProfile &profile,
        const QString &path, const QJsonObject &body, Completion completion) {
    if (this->busy) {
        if (completion) completion({}, tr("Another database write is in progress."));
        return;
    }
    const QString validation = validateProfile(profile);
    if (!validation.isEmpty()) {
        if (completion) completion({}, validation);
        return;
    }
    ProjectServerProfile request_profile = profile;
    if (profile.connection_type == QStringLiteral("local")) {
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        QString error;
        const QString endpoint = embeddedDvmdbEndpoint(profile.local_path, profile.personal,
                                                       nullptr, &error);
        if (endpoint.isEmpty()) {
            if (completion) completion({}, error);
            return;
        }
        request_profile.connection_type = QStringLiteral("remote");
        request_profile.endpoint = endpoint;
#else
        if (completion) completion({}, tr("Embedded DVMDB is unavailable."));
        return;
#endif
    }
    this->busy = true;
    this->completion = std::move(completion);
    sendPinned(profile, request_profile.endpoint, "POST", path, body,
        [this](QNetworkReply *pending) {
        if (this->reply == pending) this->reply.clear();
        const int status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray bytes = pending->readAll();
        const QString network_error = pending->errorString();
        const bool ok = pending->error() == QNetworkReply::NoError && status == 200;
        pending->deleteLater();
        if (!ok) {
            const QJsonObject data = QJsonDocument::fromJson(bytes).object();
            const QString message = data.value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString();
            finish({}, message.isEmpty() ? tr("Deletion failed (HTTP %1: %2). Refresh before retrying.")
                   .arg(status).arg(network_error) : message);
            return;
        }
        if (bytes.size() > 1024 * 1024 || !QJsonDocument::fromJson(bytes).isObject()) {
            finish({}, tr("Deletion reply was invalid. Refresh before another write."));
            return;
        }
        finish(QStringLiteral("deleted"), {});
    });
}

void ProjectSourceWriter::patchProperties(const ProjectServerProfile &profile,
        const QString &path, const QJsonObject &body, const QString &expected_response_key,
        const QString &expected_value, Completion completion)
{
    if (this->busy) {
        if (completion) completion({}, tr("Another database write is in progress."));
        return;
    }
    const QString validation = validateProfile(profile);
    if (!validation.isEmpty()) {
        if (completion) completion({}, validation);
        return;
    }
    ProjectServerProfile request_profile = profile;
    if (profile.connection_type == QStringLiteral("local")) {
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        QString error;
        const QString endpoint = embeddedDvmdbEndpoint(profile.local_path, profile.personal,
                                                       nullptr, &error);
        if (endpoint.isEmpty()) {
            if (completion) completion({}, error);
            return;
        }
        request_profile.connection_type = QStringLiteral("remote");
        request_profile.endpoint = endpoint;
#else
        if (completion) completion({}, tr("Embedded DVMDB is unavailable."));
        return;
#endif
    }
    this->busy = true;
    this->completion = std::move(completion);
    sendPinned(profile, request_profile.endpoint, "PATCH", path, body,
               [this, expected_response_key, expected_value](QNetworkReply *pending)
    {
        if (this->reply == pending) this->reply.clear();
        const QByteArray bytes = pending->readAll();
        const int status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = pending->error() == QNetworkReply::NoError && status == 200;
        const QString network_error = pending->errorString();
        pending->deleteLater();
        if (bytes.size() > 1024 * 1024) {
            finish({}, tr("DVMDB metadata reply exceeded the size limit. Refresh before retrying."));
            return;
        }
        const QJsonObject object = QJsonDocument::fromJson(bytes).object();
        if (!ok) {
            const QString error = object.value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString();
            finish({}, error.isEmpty() ? tr("Metadata update failed (HTTP %1: %2). Refresh before retrying.")
                .arg(status).arg(network_error) : error);
            return;
        }
        if (object.value(expected_response_key).toString() != expected_value) {
            finish({}, tr("DVMDB metadata reply has the wrong identity. Refresh before retrying."));
            return;
        }
        finish(expected_value, {});
    });
}

void ProjectSourceWriter::finish(const QString &id, const QString &error)
{
    this->active_connection.reset();
    this->busy = false;
    Completion done = std::move(this->completion);
    this->completion = {};
    if (done) done(id, error);
}

void ProjectSourceWriter::start(const ProjectServerProfile &profile, Operation operation,
                                const QString &project_id, const QString &name,
                                const QString &from_branch_id, const QString &from_revision,
                                const QStringList &tags, const QString &description,
                                Completion completion)
{
    if (this->busy)
    {
        if (completion) completion({}, tr("Another database write is in progress."));
        return;
    }
    const QString validation = validateProfile(profile);
    if (!validation.isEmpty())
    {
        if (completion) completion({}, validation);
        return;
    }
    if (name.trimmed().isEmpty()
        || (operation != Operation::Project && project_id.isEmpty())
        || (operation == Operation::Branch && from_branch_id.isEmpty()))
    {
        if (completion) completion({}, tr("Missing project or branch name/source."));
        return;
    }

    if (operation == Operation::Branch
        && (!DvmdbIdentity::isUuidV7(project_id) || !DvmdbIdentity::isUuidV7(from_branch_id)
            || (!from_revision.isEmpty() && !DvmdbIdentity::isUuidV7(from_revision))))
    {
        if (completion) completion({}, tr("Canonical DVMDB writes require UUIDv7 project, source branch and revision IDs."));
        return;
    }
    if (operation == Operation::UnbornBranch) {
        if (!DvmdbIdentity::isUuidV7(project_id)) {
            if (completion) completion({}, tr("Canonical DVMDB branch creation requires a UUIDv7 project ID."));
            return;
        }
        QSet<QString> distinct;
        if (description.toUtf8().size() > 512 || tags.size() > 12) {
            if (completion) completion({}, tr("Branch description or tags exceed the DVMDB limits."));
            return;
        }
        for (const QString &tag : tags) {
            if (tag.isEmpty() || tag.toUtf8().size() > 32 || distinct.contains(tag)) {
                if (completion) completion({}, tr("Invalid or duplicate branch tag."));
                return;
            }
            distinct.insert(tag);
        }
    }
    // Local Standalone sources must use the same long-lived catalogue owner
    // as private drafts. Opening another ProjectService would fail its lock.
    ProjectServerProfile request_profile = profile;
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
        request_profile.connection_type = QStringLiteral("remote");
        request_profile.endpoint = endpoint;
#else
        if (completion) completion({}, tr("Embedded DVMDB is unavailable."));
        return;
#endif
    }
    // The caller supplies a stable UUIDv7 in the request. If the reply is
    // lost, the user can refresh to verify the result before retrying.
    const QString created_id = DvmdbIdentity::newUuidV7();
    this->busy = true;
    this->completion = std::move(completion);

    QString path = QStringLiteral("/projects");
    QJsonObject body{{QStringLiteral("id"), created_id}, {QStringLiteral("name"), name}};
    if (operation == Operation::Project)
        body.insert(QStringLiteral("unborn"), true);
    if (operation != Operation::Project)
    {
        path += QStringLiteral("/") + segment(project_id) + QStringLiteral("/branches");
        if (operation == Operation::UnbornBranch) {
            body.insert(QStringLiteral("unborn"), true);
            body.insert(QStringLiteral("description"), description);
            QJsonArray tag_array;
            for (const QString &tag : tags) tag_array.append(tag);
            body.insert(QStringLiteral("tags"), tag_array);
        } else {
            body.insert(QStringLiteral("fromBranchId"), from_branch_id);
            if (!from_revision.isEmpty()) body.insert(QStringLiteral("revision"), from_revision);
        }
    }
    sendPinned(profile, request_profile.endpoint, "POST", path, body,
               [this, operation, name, created_id, tags, description](QNetworkReply *pending)
    {
        if (this->reply == pending) this->reply.clear();
        pending->deleteLater();
        const int status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (pending->bytesAvailable() > 1024 * 1024)
        {
            finish({}, tr("DVMDB write response exceeds the size limit. Refresh before retrying."));
            return;
        }
        const QByteArray payload = pending->readAll();
        const QJsonDocument document = QJsonDocument::fromJson(payload);
        const QJsonObject object = document.isObject() ? document.object() : QJsonObject();
        if (status != 201)
        {
            const QString server_error = object.value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString();
            if (status == 0)
                finish({}, tr("The write response was lost: %1. The operation may have succeeded. Refresh before retrying.")
                    .arg(pending->errorString()));
            else
                finish({}, server_error.isEmpty()
                    ? tr("DVMDB rejected the write (HTTP %1).").arg(status)
                    : tr("DVMDB rejected the write (HTTP %1): %2").arg(status).arg(server_error));
            return;
        }
        if (pending->error() != QNetworkReply::NoError || !document.isObject())
        {
            finish({}, tr("Invalid success response. The operation may have succeeded. Refresh before retrying."));
            return;
        }
        if (operation == Operation::Project)
        {
            const QString id = object.value(QStringLiteral("id")).toString();
            if (id != created_id || object.value(QStringLiteral("name")).toString() != name)
            {
                finish({}, tr("Invalid project creation response. Refresh before retrying."));
                return;
            }
            finish(id, {});
            return;
        }
        if ((operation == Operation::UnbornBranch
                ? !object.value(QStringLiteral("id")).isNull()
                : !DvmdbIdentity::isUuidV7(object.value(QStringLiteral("id")).toString()))
            || object.value(QStringLiteral("branchId")).toString() != created_id
            || object.value(QStringLiteral("branch")).toString() != name)
        {
            finish({}, tr("Invalid branch creation response. Refresh before retrying."));
            return;
        }
        if (operation == Operation::UnbornBranch) {
            if (object.value(QStringLiteral("description")).toString() != description
                || !object.value(QStringLiteral("tags")).isArray()) {
                finish({}, tr("Branch metadata was not confirmed by DVMDB. Refresh before retrying."));
                return;
            }
            QStringList returned_tags;
            for (const QJsonValue &tag : object.value(QStringLiteral("tags")).toArray())
                returned_tags.append(tag.toString());
            if (returned_tags != tags) {
                finish({}, tr("Branch tags were not persisted as requested. Refresh before retrying."));
                return;
            }
        }
        finish(object.value(QStringLiteral("branchId")).toString(), {});
    });
}
