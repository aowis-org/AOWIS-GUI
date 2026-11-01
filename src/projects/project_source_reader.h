#pragma once

#include "config/project_server_profile_storage.h"

#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkReply;

// Immutable revision objects are owned by a project, not by a branch. A
// branch is a named HEAD reference, possibly sharing a revision with others.
struct SourceRevision
{
    QString id;
    QStringList parents;
    QString record_sha256; // SHA-256 of canonical immutable revision record.
    QStringList parent_record_sha256; // Ordered integrity bindings for parent UUIDs.
};

struct SourceBranch
{
    QString name;
    QString id; // Real backend branch UUIDv7 when available; never synthesized.
    QString head;
    QString record_sha256; // Expected canonical record digest of HEAD revision.
    QString origin_branch_id; // Persisted creation-source UUIDv7, if known.
    QString fork_revision_id; // Immutable creation revision UUIDv7; metadata, not a parent.
    QString description;
    QStringList tags;
};

struct SourceProject
{
    QString id;
    QString name;
    QList<SourceBranch> branches;
    QList<SourceRevision> revisions;
    // Successful loads are complete. In-progress pages are never published.
    bool graph_complete = false;
};

struct SourceSnapshot
{
    // Identity of the DVMDB root that owns these independently mutable HEADs.
    // This is graph/tracking metadata, not authorization or a profile label.
    QString repository_id;
    QList<SourceProject> projects;
};

// A single active generation: cancelling/refreshing aborts REST replies and
// discards superseded in-flight HTTP responses.
// Results are delivered on the GUI thread only after a complete snapshot.
class ProjectSourceReader final : public QObject
{
public:
    using Completion = std::function<void(const SourceSnapshot &, const QString &)>;

    explicit ProjectSourceReader(QObject *parent = nullptr);
    void load(const ProjectServerProfile &profile, Completion completion);
    void cancel();

private:
    enum class Step { Repository, Projects, Graph };
    void nextRestRequest(quint64 generation);
    void complete(quint64 generation, const QString &error = QString());
    bool processRestDocument(const QByteArray &json, QString &error);

    QNetworkAccessManager manager;
    QPointer<QNetworkReply> reply;
    Completion completion;
    ProjectServerProfile profile;
    SourceSnapshot snapshot;
    Step step = Step::Projects;
    int project_index = 0;
    int total_branches = 0;
    int total_revisions = 0;
    QString next_cursor;
    QSet<QString> used_cursors;
    QHash<QString, int> revisions_seen;
    quint64 generation = 0;
};
