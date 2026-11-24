#pragma once

#include "config/project_server_profile_storage.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class DvmdbConnection;
class QNetworkReply;

// One branch of the project being cloned, as DVMDB reports it.
struct ProjectCloneBranch
{
    QString id;               // branch UUIDv7, kept by the clone
    QString name;
    QString head;             // empty: unborn (no commits)
    QString origin_branch_id; // fork provenance, if recorded
    QString fork_revision_id;
    QString description;
    QStringList tags;
};

// Pure planning helpers (no network), shared with tests.
namespace ProjectClonePlan
{
// Branch heads from a revision-graph page ({"heads": [...]}). False with an
// error for a malformed or incomplete head.
bool parseHeads(const QJsonObject &page, QVector<ProjectCloneBranch> *branches, QString *error);
// Every branch after the branch it was forked from, when that branch is part
// of the project; otherwise source order. False with an error on a cycle.
bool order(const QVector<ProjectCloneBranch> &branches, QVector<ProjectCloneBranch> *ordered,
           QString *error);
// The largest bundle both databases accept; 0 when either does not
// advertise transferLimits.maxBundleBytes (an older DVMDB).
qint64 chunkLimit(const QJsonObject &source_repository, const QJsonObject &target_repository);
}

// Clones a whole DVMDB project into another database ("Clone project", then
// "Paste clone" on the target). The clone keeps the project, branch and
// revision UUIDv7s, so it is the same project on both databases.
//
// Steps: verify both repository identities, MODEL codecs and the target's
// role; provision the target project (clone-target); then per branch, in
// fork order: recreate an unborn branch by UUIDv7 with its metadata, or
// create a forked branch at its fork revision and send the history as
// size-bounded bundle chunks (GET .../bundle?maxBytes=), each imported
// before the next is requested; finally copy branch description and tags.
// Every target write carries X-AOWIS-Expected-Repository-Id.
//
// Every step is idempotent, and a branch the target already has continues
// from the target's HEAD. After a failure or a cancel, starting the same
// clone again continues where it stopped; on a finished clone it brings the
// branches up to date. Import only fast-forwards: a branch that changed on
// both databases is reported as a warning and left alone. Drafts are never
// copied.
//
// Push (startPush) is the same transfer for one branch, from the personal
// database to a database that already has the project: everything up to and
// including one revision of the branch that the target does not have yet is
// sent (GET .../bundle?maxBytes=&upTo=), and the target branch ends exactly
// there; later revisions stay where they are. A target branch that is further
// or changed on its own is refused, never overwritten. A branch the target
// does not have is created there with its identity (and metadata).
class ProjectCloneTransfer final : public QObject
{
    Q_DECLARE_TR_FUNCTIONS(ProjectCloneTransfer)

public:
    using ConnectionFor = std::function<std::shared_ptr<DvmdbConnection>(const ProjectServerProfile &)>;
    struct Progress
    {
        QString branch;
        int branch_index = 0; // 1-based branch being copied
        int branch_count = 0;
        int chunks = 0;       // bundle chunks imported so far, all branches
        qint64 bytes = 0;     // bundle bytes imported so far
    };
    using ProgressCallback = std::function<void(const Progress &)>;
    // error empty on success. warnings: branches that could not be copied
    // (the rest of the project was).
    using Completion = std::function<void(const QString &error, const QStringList &warnings)>;

    explicit ProjectCloneTransfer(QObject *parent = nullptr, ConnectionFor connection_for = {});
    ~ProjectCloneTransfer() override;
    bool isBusy() const;
    void start(const ProjectServerProfile &source, const QString &project_id,
               const ProjectServerProfile &target, ProgressCallback progress, Completion completion);
    // DVMDB project, branch and revision UUIDv7s; an empty up_to pushes the
    // branch HEAD. A push that cannot fast-forward the target is an error;
    // only branch metadata of a newly created branch can end as a warning.
    void startPush(const ProjectServerProfile &source, const QString &project_id,
                   const QString &branch_id, const QString &up_to,
                   const ProjectServerProfile &target, ProgressCallback progress, Completion completion);
    // Aborts the request in flight; the completion reports the cancel.
    void cancel();
    // Whether the last clone changed the target at all, so a failure can say
    // truthfully whether anything is there to continue from.
    bool wroteAnything() const;

private:
    struct Endpoint
    {
        ProjectServerProfile profile;
        QString root; // .../dvmdb/v1
        std::shared_ptr<DvmdbConnection> connection;
    };
    struct Response
    {
        int status = 0;
        QByteArray body;
        QString transport_error;
        QByteArray bundle_head;
        QByteArray bundle_complete;
        QJsonObject json() const;
        QString errorMessage() const; // DVMDB error.message, or empty
    };
    using Handler = std::function<void(const Response &)>;

    void begin(const ProjectServerProfile &source, const QString &project_id,
               const ProjectServerProfile &target, ProgressCallback progress, Completion completion);
    bool resolve(const ProjectServerProfile &profile, Endpoint *endpoint, QString *error) const;

    void send(const Endpoint &endpoint, const QByteArray &method, const QString &path,
              const QByteArray &body, const QByteArray &content_type, bool pinned,
              int timeout_ms, qint64 max_bytes, Handler handler);
    QString failure(const QString &what, const Response &response) const;
    QString projectPath() const;
    // The revision the branch's transfer ends at: HEAD, or the pushed revision.
    QString tipOf(const ProjectCloneBranch &branch) const;
    QString cancelledMessage() const;
    QString branchPath(const ProjectCloneBranch &branch) const;

    void readSourceRepository();
    void readTargetRole();
    void readSourceProject();
    void readSourceBranches();
    void readTargetProject();
    void provisionTarget();
    void nextBranch();
    void readTargetBranch();
    void copyUnbornBranch(const QJsonObject &existing, bool exists);
    void startHistory(const QJsonObject &existing, bool exists);
    void createForkedBranch();
    void requestChunk();
    void importChunk(const QByteArray &bundle, const QString &head, bool complete);
    void copyMetadata(const QJsonObject &existing);
    void branchDone();
    void skipBranch(const QString &warning);
    void reportProgress();
    void finish(const QString &error);

    ConnectionFor connection_for;
    QNetworkAccessManager manager;
    QPointer<QNetworkReply> reply;
    ProgressCallback progress;
    Completion completion;
    Endpoint source;
    Endpoint target;
    QString project_id;
    QString project_name;
    qint64 chunk_limit = 0;
    QVector<ProjectCloneBranch> branches;
    int branch_index = -1;
    QString base; // recipient base of the next chunk, empty for a full bundle
    QSet<QString> copied_branches; // branch UUIDv7s whose history the target has
    QStringList warnings;
    Progress state;
    bool pushing = false;
    QString push_branch_id;
    QString push_up_to;
    bool push_created = false; // the push created the branch on the target
    bool busy = false;
    bool cancelled = false;
    bool wrote = false;
};
