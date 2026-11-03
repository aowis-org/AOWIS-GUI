#include "projects/personal_commit_intent_store.h"
#include "projects/dvmdb_identity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

#if defined(Q_OS_UNIX) && !defined(Q_OS_WASM)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
bool fail(QString *error, const QString &message)
{
    if (error != nullptr) *error = message;
    return false;
}

bool syncDirectory(const QString &path, QString *error)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_WASM)
    const QByteArray name = QFile::encodeName(path);
    const int descriptor = ::open(name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
        return fail(error, QStringLiteral("Cannot open Commit intent directory for sync"));
    const int result = ::fsync(descriptor);
    ::close(descriptor);
    if (result != 0)
        return fail(error, QStringLiteral("Cannot sync Commit intent directory"));
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
#endif
    return true;
}

QByteArray digest(const QJsonObject &body)
{
    return QCryptographicHash::hash(QJsonDocument(body).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256).toHex();
}

bool valid(const PersonalCommitIntent &intent)
{
    return DvmdbIdentity::isUuidV7(intent.repository_id)
        && DvmdbIdentity::isUuidV7(intent.project_id)
        && DvmdbIdentity::isUuidV7(intent.branch_id)
        && DvmdbIdentity::isUuidV7(intent.operation_id)
        && DvmdbIdentity::isUuidV7(intent.body.value(QStringLiteral("draftId")).toString())
        && DvmdbIdentity::isUuidV7(intent.body.value(QStringLiteral("sessionId")).toString())
        && DvmdbIdentity::isUuidV7(intent.body.value(QStringLiteral("fencingToken")).toString())
        && DvmdbIdentity::isUuidV7(intent.body.value(QStringLiteral("author")).toObject()
                                     .value(QStringLiteral("id")).toString())
        && intent.body.value(QStringLiteral("expectedGeneration")).isString()
        && !intent.body.value(QStringLiteral("expectedGeneration")).toString().isEmpty()
        && (intent.body.value(QStringLiteral("expectedHead")).isNull()
            || DvmdbIdentity::isUuidV7(intent.body.value(QStringLiteral("expectedHead")).toString()))
        && intent.body.value(QStringLiteral("operationId")).toString() == intent.operation_id
        && !intent.endpoint.isEmpty()
        && (intent.state == QStringLiteral("pending")
            || intent.state == QStringLiteral("succeeded")
            || intent.state == QStringLiteral("rejected"));
}

QJsonObject encode(const PersonalCommitIntent &intent)
{
    return QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("repositoryId"), intent.repository_id},
        {QStringLiteral("projectId"), intent.project_id},
        {QStringLiteral("branchId"), intent.branch_id},
        {QStringLiteral("endpoint"), intent.endpoint},
        {QStringLiteral("operationId"), intent.operation_id},
        {QStringLiteral("body"), intent.body},
        {QStringLiteral("bodySha256"), QString::fromLatin1(digest(intent.body))},
        {QStringLiteral("state"), intent.state},
        {QStringLiteral("revision"), intent.revision},
        {QStringLiteral("reason"), intent.reason}};
}

bool decode(const QJsonObject &data, PersonalCommitIntent &intent)
{
    if (data.value(QStringLiteral("schema")).toInt() != 1 ||
        !data.value(QStringLiteral("body")).isObject()) return false;
    intent.repository_id = data.value(QStringLiteral("repositoryId")).toString();
    intent.project_id = data.value(QStringLiteral("projectId")).toString();
    intent.branch_id = data.value(QStringLiteral("branchId")).toString();
    intent.endpoint = data.value(QStringLiteral("endpoint")).toString();
    intent.operation_id = data.value(QStringLiteral("operationId")).toString();
    intent.body = data.value(QStringLiteral("body")).toObject();
    intent.state = data.value(QStringLiteral("state")).toString();
    intent.revision = data.value(QStringLiteral("revision")).toString();
    intent.reason = data.value(QStringLiteral("reason")).toString();
    return valid(intent)
        && data.value(QStringLiteral("bodySha256")).toString()
            == QString::fromLatin1(digest(intent.body))
        && (intent.state != QStringLiteral("succeeded")
            || DvmdbIdentity::isUuidV7(intent.revision))
        && (intent.state != QStringLiteral("pending") || intent.revision.isEmpty());
}

bool writeIntent(const QString &path, const PersonalCommitIntent &intent, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("Cannot open durable Commit intent for writing"));
    const QByteArray bytes = QJsonDocument(encode(intent)).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("Cannot persist durable Commit intent"));
#if defined(Q_OS_UNIX) && !defined(Q_OS_WASM)
    // QSaveFile makes the replacement atomic. Explicitly sync the resulting
    // inode and its directory before permitting the corresponding HTTP POST.
    QFile persisted(path);
    if (!persisted.open(QIODevice::ReadOnly) || ::fsync(persisted.handle()) != 0)
        return fail(error, QStringLiteral("Cannot sync durable Commit intent file"));
#endif
    return syncDirectory(QFileInfo(path).absolutePath(), error);
}

bool takeLock(const QString &path, QLockFile &lock, QString *error)
{
    Q_UNUSED(path);
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(500))
        return fail(error, QStringLiteral("Another GUI process is updating this Commit intent"));
    return true;
}
}

PersonalCommitIntentStore::PersonalCommitIntentStore(const QString &root)
    : root_(root)
{
}

QString PersonalCommitIntentStore::defaultRoot()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("personal-commit-intents"));
}

QString PersonalCommitIntentStore::filePath(const QString &repository_id,
                                            const QString &project_id,
                                            const QString &branch_id) const
{
    return QDir(this->root_).filePath(repository_id + QLatin1Char('/') + project_id
                                     + QLatin1Char('_') + branch_id + QStringLiteral(".json"));
}

PersonalCommitIntentStore::ReadResult PersonalCommitIntentStore::read(
    const QString &repository_id, const QString &project_id, const QString &branch_id,
    PersonalCommitIntent &intent, QString *error) const
{
    if (error != nullptr) error->clear();
    if (this->root_.isEmpty() || !DvmdbIdentity::isUuidV7(repository_id)
        || !DvmdbIdentity::isUuidV7(project_id) || !DvmdbIdentity::isUuidV7(branch_id)) {
        fail(error, QStringLiteral("Invalid Commit intent directory or UUIDv7 scope"));
        return ReadResult::Error;
    }
    const QString path = filePath(repository_id, project_id, branch_id);
    if (!QFileInfo::exists(path)) return ReadResult::Missing;
    QFile file(path);
    if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        fail(error, QStringLiteral("Cannot safely read stored Commit intent"));
        return ReadResult::Error;
    }
    const QByteArray bytes = file.readAll();
    QJsonParseError json_error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &json_error);
    if (json_error.error != QJsonParseError::NoError || !document.isObject()
        || !decode(document.object(), intent) || intent.repository_id != repository_id
        || intent.project_id != project_id || intent.branch_id != branch_id) {
        fail(error, QStringLiteral("Corrupt or mismatched durable Commit intent; refusing a new Commit"));
        return ReadResult::Error;
    }
    return ReadResult::Found;
}

bool PersonalCommitIntentStore::create(const PersonalCommitIntent &intent, QString *error) const
{
    if (error != nullptr) error->clear();
    if (!valid(intent) || intent.state != QStringLiteral("pending") || this->root_.isEmpty())
        return fail(error, QStringLiteral("Invalid initial Commit intent"));
    const QString path = filePath(intent.repository_id, intent.project_id, intent.branch_id);
    const QString directory = QFileInfo(path).absolutePath();
    const bool directory_exists = QDir(directory).exists();
    if (!QDir().mkpath(directory))
        return fail(error, QStringLiteral("Cannot create Commit intent directory"));
    if (!directory_exists && !syncDirectory(QFileInfo(directory).absolutePath(), error))
        return false;
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!takeLock(path, lock, error)) return false;
    // Never overwrite an existing unresolved OR unacknowledged terminal record.
    if (QFileInfo::exists(path))
        return fail(error, QStringLiteral("A Commit intent already exists for this branch; reconcile it first"));
    return writeIntent(path, intent, error);
}

bool PersonalCommitIntentStore::resolve(const PersonalCommitIntent &intent,
                                        const QString &state, const QString &revision,
                                        const QString &reason, QString *error) const
{
    if (error != nullptr) error->clear();
    if (state != QStringLiteral("succeeded") && state != QStringLiteral("rejected"))
        return fail(error, QStringLiteral("Cannot resolve Commit intent without a terminal state"));
    if (state == QStringLiteral("succeeded") && !DvmdbIdentity::isUuidV7(revision))
        return fail(error, QStringLiteral("Commit response has invalid revision UUIDv7"));
    const QString path = filePath(intent.repository_id, intent.project_id, intent.branch_id);
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!takeLock(path, lock, error)) return false;
    PersonalCommitIntent saved;
    if (read(intent.repository_id, intent.project_id, intent.branch_id, saved, error) != ReadResult::Found
        || saved.operation_id != intent.operation_id || saved.body != intent.body)
        return fail(error, QStringLiteral("Stored Commit operation changed during reconciliation"));
    if (saved.state != QStringLiteral("pending") &&
        (saved.state != state || saved.revision != revision))
        return fail(error, QStringLiteral("Conflicting terminal Commit outcome"));
    saved.state = state;
    saved.revision = revision;
    saved.reason = reason;
    return writeIntent(path, saved, error);
}

bool PersonalCommitIntentStore::acknowledge(const PersonalCommitIntent &intent, QString *error) const
{
    if (error != nullptr) error->clear();
    const QString path = filePath(intent.repository_id, intent.project_id, intent.branch_id);
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!takeLock(path, lock, error)) return false;
    PersonalCommitIntent saved;
    if (read(intent.repository_id, intent.project_id, intent.branch_id, saved, error) != ReadResult::Found
        || saved.operation_id != intent.operation_id
        || saved.state == QStringLiteral("pending"))
        return fail(error, QStringLiteral("Cannot acknowledge an unresolved Commit intent"));
    if (!QFile::remove(path))
        return fail(error, QStringLiteral("Cannot clear acknowledged Commit intent"));
    return syncDirectory(QFileInfo(path).absolutePath(), error);
}
