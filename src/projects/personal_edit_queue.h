#ifndef AOWIS_PERSONAL_EDIT_QUEUE_H
#define AOWIS_PERSONAL_EDIT_QUEUE_H

#include <memory>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUuid>

class QLockFile;

// Crash-recoverable *pending* editing transactions, stored on this GUI device.
// Each event is written atomically before it can be offered to a remote DVMDB.
// This is not the durable server-side undo/redo history; never remove an event
// merely because the server acknowledged a candidate-model overlay save.
class PersonalEditQueue final
{
public:
    struct Entry final
    {
        quint64 sequence = 0;
        QUuid transaction_id;
        QByteArray encoded_transaction;
    };

    PersonalEditQueue();
    ~PersonalEditQueue();
    PersonalEditQueue(const PersonalEditQueue &) = delete;
    PersonalEditQueue &operator=(const PersonalEditQueue &) = delete;

    // Root should be QStandardPaths::AppLocalDataLocation / "edit-queue".
    // An exclusive process lock prevents two GUIs from allocating the same
    // sequence or modifying a draft's pending transactions concurrently.
    bool open(const QString &root, const QUuid &repository_id,
              const QUuid &draft_id, QString *error = nullptr);
    void close();

    // Duplicate transaction IDs with identical bytes are idempotent.
    // A conflicting reuse of an ID is rejected without modifying the queue.
    bool append(const QByteArray &encoded_transaction, QString *error = nullptr);

    // Call ONLY after DVMDB has durably recorded the matching transaction
    // *and* its candidate-model mutation as one atomic operation. VC-04's
    // existing overlay-only Save response is NOT such an acknowledgement.
    bool acknowledgePersisted(const QUuid &transaction_id, QString *error = nullptr);

    QList<Entry> pending() const;
    bool isOpen() const;

    // The queue root every private draft session uses on this device.
    static QString defaultRoot();
    // Number of queued transaction files of one draft, read without taking
    // the lock (a running session may hold it). -1 if the directory cannot
    // be listed. Used to refuse a Commit that would leave queued edits out.
    static int pendingTransactionFileCount(const QString &root, const QUuid &repository_id,
                                           const QUuid &draft_id);
    QString directoryPath() const;
    QUuid repositoryId() const;
    QUuid draftId() const;

private:
    bool readExisting(QString *error);
    static QString fileName(quint64 sequence, const QUuid &id);

    QString directory_;
    QUuid repository_id_;
    QUuid draft_id_;
    std::unique_ptr<QLockFile> lock_;
    QList<Entry> entries_;
    quint64 next_sequence_ = 1;
    quint64 pending_bytes_ = 0;
};

#endif // AOWIS_PERSONAL_EDIT_QUEUE_H
