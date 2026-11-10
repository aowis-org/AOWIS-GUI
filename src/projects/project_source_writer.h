#pragma once

#include "config/project_server_profile_storage.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

class DvmdbConnection;

class QNetworkReply;
class QJsonObject;

// Pinned project and branch metadata/history writes. Exactly one write may be in flight. The writer
// chooses the UUIDv7 of what it creates, so a request resent by the transport
// (Qt before 6.10 replays a POST whose connection dropped before the reply)
// names the same object and DVMDB answers it idempotently. A lost response
// still does not prove that the server rejected the write: callers must
// refresh before starting another creation, which would get a new UUIDv7.
class ProjectSourceWriter final : public QObject
{
    Q_DECLARE_TR_FUNCTIONS(ProjectSourceWriter)

public:
    using Completion = std::function<void(const QString &created_id, const QString &error)>;

    using ConnectionFor = std::function<std::shared_ptr<DvmdbConnection>(const ProjectServerProfile &)>;
    explicit ProjectSourceWriter(QObject *parent = nullptr, ConnectionFor connection_for = {});
    bool isBusy() const;
    void createProject(const ProjectServerProfile &profile, const QString &name, Completion completion);
    // Rename a project persistently. The expected name prevents overwriting a
    // concurrent rename from another client.
    void renameProject(const ProjectServerProfile &profile, const QString &project_id,
                       const QString &expected_name, const QString &name, Completion completion);
    // Create an independent unborn HEAD belonging to the selected project.
    void createUnbornBranch(const ProjectServerProfile &profile, const QString &project_id,
                            const QString &name, const QStringList &tags,
                            const QString &description, Completion completion);
    void deleteBranch(const ProjectServerProfile &profile, const QString &project_id,
                      const QString &branch_id, const QString &expected_head, Completion completion);
    void truncateBranch(const ProjectServerProfile &profile, const QString &project_id,
                        const QString &branch_id, const QString &expected_head,
                        const QString &first_removed, Completion completion);
    // Both operations use compare-and-swap preconditions and refresh after
    // uncertain replies; the UUIDv7 addresses never change on rename.
    void updateBranchProperties(const ProjectServerProfile &profile, const QString &project_id,
                                const QString &branch_id, const QString &expected_name,
                                const QString &name, const QStringList &tags,
                                const QString &description, Completion completion);
    void updateRevisionMessage(const ProjectServerProfile &profile, const QString &project_id,
                               const QString &revision_id, const QString &expected_message,
                               const QString &message, Completion completion);
    // The source branch is addressed by UUIDv7, never by its renamable name.
    void createBranch(const ProjectServerProfile &profile, const QString &project_id,
                      const QString &name, const QString &from_branch_id,
                      const QString &from_revision, Completion completion);

private:
    enum class Operation { Project, Branch, UnbornBranch };
    void start(const ProjectServerProfile &profile, Operation operation,
               const QString &project_id, const QString &name, const QString &from_branch_id,
               const QString &from_revision, const QStringList &tags,
               const QString &description, Completion completion);
    void finish(const QString &id, const QString &error);
    void sendPinned(const ProjectServerProfile &profile, const QString &endpoint,
                    const QByteArray &method, const QString &path, const QJsonObject &body,
                    std::function<void(QNetworkReply *)> completed);
    void destructiveWrite(const ProjectServerProfile &profile, const QString &path,
                          const QJsonObject &body, Completion completion);
    void patchProperties(const ProjectServerProfile &profile, const QString &path,
                         const QJsonObject &body, const QString &expected_response_key,
                         const QString &expected_value, Completion completion);

    ConnectionFor connection_for;
    std::shared_ptr<DvmdbConnection> active_connection;
    QNetworkAccessManager manager;
    QPointer<QNetworkReply> reply;
    Completion completion;
    bool busy = false;
};
