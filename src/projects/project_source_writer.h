#pragma once

#include "config/project_server_profile_storage.h"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>

#include <functional>

class QNetworkReply;

// Project and branch creation. Exactly one write may be in flight. The writer
// chooses the UUIDv7 of what it creates, so a request resent by the transport
// (Qt before 6.10 replays a POST whose connection dropped before the reply)
// names the same object and DVMDB answers it idempotently. A lost response
// still does not prove that the server rejected the write: callers must
// refresh before starting another creation, which would get a new UUIDv7.
class ProjectSourceWriter final : public QObject
{
public:
    using Completion = std::function<void(const QString &created_id, const QString &error)>;

    explicit ProjectSourceWriter(QObject *parent = nullptr);
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

    QNetworkAccessManager manager;
    QPointer<QNetworkReply> reply;
    Completion completion;
    bool busy = false;
};
