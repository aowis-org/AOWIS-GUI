#include "projects/personal_commit_intent_store.h"
#include "projects/dvmdb_identity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>

namespace
{
bool check(bool ok, const char *reason)
{
    if (!ok) std::fprintf(stderr, "commit_intent_store: %s\n", reason);
    return ok;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary intent store unavailable")) return 1;
    PersonalCommitIntentStore store(directory.path());
    PersonalCommitIntent intent;
    intent.repository_id = DvmdbIdentity::newUuidV7();
    intent.project_id = DvmdbIdentity::newUuidV7();
    intent.branch_id = DvmdbIdentity::newUuidV7();
    intent.operation_id = DvmdbIdentity::newUuidV7();
    intent.endpoint = QStringLiteral("http://127.0.0.1:10000");
    intent.body = QJsonObject{
        {QStringLiteral("operationId"), intent.operation_id},
        {QStringLiteral("draftId"), DvmdbIdentity::newUuidV7()},
        {QStringLiteral("sessionId"), DvmdbIdentity::newUuidV7()},
        {QStringLiteral("fencingToken"), DvmdbIdentity::newUuidV7()},
        {QStringLiteral("author"), QJsonObject{{QStringLiteral("id"), DvmdbIdentity::newUuidV7()},
                                                 {QStringLiteral("name"), QStringLiteral("test")}}},
        {QStringLiteral("expectedGeneration"), QStringLiteral("5")},
        {QStringLiteral("expectedHead"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("message"), QStringLiteral("commit test")}};
    QString error;
    if (!check(store.create(intent, &error) && error.isEmpty(), "first intent not durable")) return 1;
    if (!check(!store.create(intent, &error), "concurrent/new Commit overwrote pending operation")) return 1;
    PersonalCommitIntentStore restarted(directory.path());
    PersonalCommitIntent on_disk;
    if (!check(restarted.read(intent.repository_id, intent.project_id, intent.branch_id,
                              on_disk, &error) == PersonalCommitIntentStore::ReadResult::Found
               && on_disk.operation_id == intent.operation_id && on_disk.body == intent.body
               && on_disk.state == QStringLiteral("pending"),
               "operation UUID and immutable body not recovered after restart")) return 1;
    if (!check(!restarted.acknowledge(on_disk, &error),
               "unresolved operation was acknowledged and removed")) return 1;
    const QString revision = DvmdbIdentity::newUuidV7();
    if (!check(restarted.resolve(on_disk, QStringLiteral("succeeded"), revision, {}, &error),
               "succeeded status not persisted")) return 1;
    PersonalCommitIntent resolved;
    if (!check(store.read(intent.repository_id, intent.project_id, intent.branch_id,
                          resolved, &error) == PersonalCommitIntentStore::ReadResult::Found
               && resolved.state == QStringLiteral("succeeded") && resolved.revision == revision,
               "terminal Commit state not recovered")) return 1;
    if (!check(!restarted.resolve(on_disk, QStringLiteral("rejected"), {},
                                 QStringLiteral("conflict"), &error),
               "conflicting terminal outcome was allowed")) return 1;
    if (!check(store.acknowledge(resolved, &error)
               && store.read(intent.repository_id, intent.project_id, intent.branch_id,
                             on_disk, &error) == PersonalCommitIntentStore::ReadResult::Missing,
               "terminal acknowledgement did not clear durable intent")) return 1;
    intent.operation_id = DvmdbIdentity::newUuidV7();
    intent.body.insert(QStringLiteral("operationId"), intent.operation_id);
    if (!check(store.create(intent, &error), "new operation not allowed after acknowledgement")) return 1;
    const QString file = QDir(directory.path()).filePath(intent.repository_id + QLatin1Char('/')
        + intent.project_id + QLatin1Char('_') + intent.branch_id + QStringLiteral(".json"));
    QFile tamper(file);
    if (!check(tamper.open(QIODevice::ReadOnly), "cannot read test record")) return 1;
    QJsonObject damaged = QJsonDocument::fromJson(tamper.readAll()).object();
    tamper.close();
    QJsonObject changed_body = damaged.value(QStringLiteral("body")).toObject();
    changed_body.insert(QStringLiteral("message"), QStringLiteral("tampered message"));
    damaged.insert(QStringLiteral("body"), changed_body);
    if (!check(tamper.open(QIODevice::WriteOnly | QIODevice::Truncate),
               "cannot alter test record")) return 1;
    tamper.write(QJsonDocument(damaged).toJson(QJsonDocument::Compact));
    tamper.close();
    if (!check(restarted.read(intent.repository_id, intent.project_id, intent.branch_id,
                              on_disk, &error) == PersonalCommitIntentStore::ReadResult::Error
               && !error.isEmpty() && !store.create(intent, &error),
               "corrupt intent silently allowed a new Commit")) return 1;
    return 0;
}
