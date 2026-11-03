#pragma once

#include "config/personal_repository_storage.h"

#include <aowis/model/hydraulic/network_hydraulic.h>

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QStringList>
#include <QUuid>

#include <functional>

// Draft identities are local to the personal repository. This client NEVER
// falls back to a project-source profile and NEVER changes a committed HEAD.
struct PersonalWorkingCopyChange
{
    QString uuid;
    bool deleted = false;
    quint16 kind = 0;
    QStringList references;
    QByteArray payload;
};

// Logical membership intent for one hydraulic entity that a MODEL event
// creates (inserted) or removes. DVMDB materializes the physical membership
// chunks itself; the GUI never encodes DVMDB storage internals. Every create
// or delete edit in an event batch needs exactly one intent, and DVMDB rejects
// a batch that creates or removes an entity without one.
struct HydraulicDraftMembershipEdit final
{
    QUuid network_id;
    QUuid entity_id;
    quint16 kind = 0;
    quint16 collection_index = 0;
    bool inserted = false;
};

struct PersonalWorkingCopyEvent
{
    QString id;
    QByteArray encoded;
};

struct PersonalWorkingCopyInfo
{
    QString id;
    QString project_id;
    QString branch_id;
    QString base_revision; // empty == explicit unborn HEAD
    QString session_id;
    QString generation; // decimal, not a lossy JSON number
    bool dirty = false;
    QList<PersonalWorkingCopyChange> changes; // populated by reopen(), not list()
    QString event_count; // decimal journal count, when supplied by server
    QList<PersonalWorkingCopyEvent> events; // populated by reopen(), not list()
};

struct PersonalWorkingCopyLease
{
    QString token;
    QString generation;
    QString expires_at_ms;
};

class PersonalEditQueue;

struct PersonalWorkingCopyEventSaveResult
{
    QString draft_id;
    QString generation;
    QString event_count;
    QStringList accepted_ids;
};

class PersonalWorkingCopyClient final : public QObject
{
public:
    using DraftCompletion = std::function<void(const PersonalWorkingCopyInfo &, const QString &)>;
    using ListCompletion = std::function<void(const QList<PersonalWorkingCopyInfo> &, const QString &)>;
    using LeaseCompletion = std::function<void(const PersonalWorkingCopyLease &, const QString &)>;
    using Completion = std::function<void(const QString &)>;

    explicit PersonalWorkingCopyClient(QObject *parent = nullptr);

    // Read *all* pages before publishing the list. Project-scoped calls are
    // filtered by DVMDB, not merely by the Inspector after retrieval.
    void list(const PersonalRepositoryTarget &target, ListCompletion completion);
    void list(const PersonalRepositoryTarget &target, const QString &project_id,
              ListCompletion completion);
    // The branch is addressed by UUIDv7 (DG-01). The client chooses the new
    // draft's UUIDv7, so a resent request returns the same draft.
    void create(const PersonalRepositoryTarget &target, const QString &project_id,
                const QString &branch_id, const QString &base_revision,
                DraftCompletion completion);
    void reopen(const PersonalRepositoryTarget &target, const QString &draft_id,
                DraftCompletion completion);
    // Every write is explicitly scoped to the personal repository. A save
    // acknowledges only the exact next generation; uncertain network failures
    // must be resolved by reopen(), never by blindly replaying a batch.
    // Commit an exact saved draft using a private DVMDB's durable, UUID-keyed
    // local-commit coordinator; an uncertain reply is reconciled by operation ID.
    using CommitCompletion = std::function<void(const QString &revision, const QString &error)>;
    // Empty means no recorded operation; an error means corrupt/unreadable
    // local state and MUST block a fresh operation.
    static QString recordedCommitState(const PersonalRepositoryTarget &target,
                                       const QString &project_id, const QString &branch_id,
                                       QString *error = nullptr);
    void reconcileRecordedCommit(const PersonalRepositoryTarget &target,
                                 const QString &project_id, const QString &branch_id,
                                 CommitCompletion completion);
    static bool acknowledgeRecordedCommit(const PersonalRepositoryTarget &target,
                                          const QString &project_id, const QString &branch_id,
                                          QString *error = nullptr);
    void commitSavedDraft(const PersonalRepositoryTarget &target,
                          const PersonalWorkingCopyInfo &draft, const QString &session_id,
                          const PersonalWorkingCopyLease &lease, const QString &message,
                          CommitCompletion completion);
    void acquireLease(const PersonalRepositoryTarget &target, const QString &draft_id,
                      const QString &session_id, int seconds, LeaseCompletion completion);
    // Create the first hydraulic network in an unborn private draft. The
    // DVMDB server owns the native manifest bytes. This does not commit or
    // advance HEAD. On an uncertain response, reopen before proceeding; the
    // same request is safely retryable by the same fenced writer.
    // Hydrate a *read-only* consistent candidate from its immutable base and
    // exact saved overlay. The server validates the native manifest, chunks,
    // canonical entity codecs and references before returning MODEL bytes.
    // This operation does not acquire a lease or enable unsaved editor edits.
    using NetworkCompletion = std::function<void(
        const NetworkHydraulic &, const QString &generation, const QString &error)>;
    // Lightweight summary of the exact SAVED candidate generation. Unlike
    // committed branch-tip counts, this includes unpublished unborn HEADs.
    using SummaryCompletion = std::function<void(const QJsonObject &, const QString &)>;
    void summarizeDraftNetworks(const PersonalRepositoryTarget &target,
                                const QString &draft_id, SummaryCompletion completion);
    // Atomically import a complete canonical network into an untouched unborn
    // private draft, never an already populated or committed HEAD.
    void importNetworkIntoEmptyDraft(const PersonalRepositoryTarget &target,
                                     const QString &draft_id, const QString &session_id,
                                     const QString &fencing_token, const QString &generation,
                                     const NetworkHydraulic &network, DraftCompletion completion);
    void loadDraftNetwork(const PersonalRepositoryTarget &target,
                          const QString &draft_id, const QString &network_id,
                          NetworkCompletion completion);
    void initializeEmptyNetwork(const PersonalRepositoryTarget &target,
                                const QString &draft_id, const QString &session_id,
                                const QString &fencing_token,
                                const QString &expected_generation,
                                const QString &network_id, const QString &name,
                                DraftCompletion completion);
    void saveChanges(const PersonalRepositoryTarget &target, const QString &draft_id,
                     const QString &session_id, const QString &fencing_token,
                     const QString &expected_generation,
                     const QList<PersonalWorkingCopyChange> &changes,
                     DraftCompletion completion);
    // A lost HTTP acknowledgement does not mean the Save failed. On any
    // unconfirmed result, reopen the authoritative draft and accept recovery
    // only if it contains the exact proposed records at exactly generation+1.
    // No second Save is issued; the caller keeps its local edits on failure.
    using RecoveringSaveCompletion = std::function<void(
        const PersonalWorkingCopyInfo &, bool recovered, const QString &)>;
    void saveChangesRecovering(const PersonalRepositoryTarget &target,
                               const QString &draft_id, const QString &session_id,
                               const QString &fencing_token,
                               const QString &expected_generation,
                               const QList<PersonalWorkingCopyChange> &changes,
                               RecoveringSaveCompletion completion);

    // Send logical MODEL transactions, typed model records and membership
    // intents atomically to the private journal. `membership` must hold
    // exactly one intent per created/removed entity in `encoded_events` (empty
    // when the batch only updates entities); a mismatch is rejected locally
    // before any request. Validate exact IDs, generation and count in the small
    // acknowledgement. For lost/malformed responses, reopen and compare the
    // authoritative last journal batch and candidate bytes before confirming.
    // Never resend an uncertain batch automatically.
    using EventSaveCompletion = std::function<void(
        const PersonalWorkingCopyEventSaveResult &, bool recovered, const QString &)>;
    void saveEventsRecovering(const PersonalRepositoryTarget &target,
                             const QString &draft_id, const QString &session_id,
                             const QString &fencing_token, const QString &expected_generation,
                             const QList<PersonalWorkingCopyChange> &changes,
                             const QList<QByteArray> &encoded_events,
                             const QList<HydraulicDraftMembershipEdit> &membership,
                             EventSaveCompletion completion);

    // Drain precisely the oldest durable local event. The caller supplies the
    // matching typed candidate records and membership intents, both as
    // produced by HydraulicDraftEventAdapter::prepareWithMembership(); only an
    // authoritative DVMDB journal acknowledgement permits deleting the local
    // event. The queue must remain alive and open until completion; call
    // serially per draft.
    using QueueFlushCompletion = std::function<void(
        const QString &next_generation, bool recovered, const QString &)>;
    void flushOldestQueuedEvent(const PersonalRepositoryTarget &target,
                               PersonalEditQueue &queue, const QString &draft_id,
                               const QString &session_id, const QString &fencing_token,
                               const QString &expected_generation,
                               const QList<PersonalWorkingCopyChange> &changes,
                               const QList<HydraulicDraftMembershipEdit> &membership,
                               QueueFlushCompletion completion);

    void releaseLease(const PersonalRepositoryTarget &target, const QString &draft_id,
                      const QString &session_id, const QString &fencing_token,
                      Completion completion);

    // Current HEAD of a branch in the personal repository, addressed by its
    // UUIDv7. An empty revision means the branch is unborn.
    using BranchHeadCompletion = std::function<void(const QString &revision, const QString &error)>;
    void branchHead(const PersonalRepositoryTarget &target, const QString &project_id,
                    const QString &branch_id, BranchHeadCompletion completion);

    // Delete a draft under a live lease of `session_id`, fenced like a Save.
    // A lost reply is resolved by reopening: "not found" confirms deletion.
    void discardDraft(const PersonalRepositoryTarget &target, const QString &draft_id,
                      const QString &session_id, const QString &fencing_token,
                      const QString &expected_generation, Completion completion);

    // Checkout a pinned source branch into a distinct private HTTP repository.
    // A committed branch transfers a verified full bundle. An unborn root branch
    // is provisioned with its original UUID and null HEAD, without a bundle.
    // Neither path creates a draft or a commit.
    using CheckoutCompletion = std::function<void(const QString &)>;
    void checkoutBranchPrivately(const PersonalRepositoryTarget &target,
        const QString &source_endpoint, const QString &source_repository_id,
        const QString &project_id, const QString &project_name,
        const QString &branch_id, const QString &expected_head,
        CheckoutCompletion completion);

    static QString newSessionUuidV7();

private:
    using Response = std::function<void(const QJsonObject &, const QString &)>;
    void request(const PersonalRepositoryTarget &target, const QByteArray &method,
                 const QString &path, const QJsonObject &body, Response completion);
    static bool parseDraft(const QJsonObject &object, PersonalWorkingCopyInfo &draft);
    void listPage(const PersonalRepositoryTarget &target, const QString &project_id,
                  const QString &after, int page_count,
                  QList<PersonalWorkingCopyInfo> collected, ListCompletion completion);
    QNetworkAccessManager manager;
};
