#include "projects/personal_edit_queue.h"

#include <aowis/model/edit/model_edit_transaction.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QtEndian>

#if defined(Q_OS_UNIX) && !defined(Q_OS_WASM)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
constexpr char queue_magic[] = {'A', 'O', 'W', 'Q', '0', '0', '0', '1'};
constexpr qsizetype header_size = 8 + 8 + 4 + 32;
constexpr qsizetype max_transaction_size = 8 * 1024 * 1024 + 64;
constexpr qsizetype max_pending_entries = 100000;
constexpr quint64 max_pending_bytes = 128ull * 1024ull * 1024ull;

bool fail(QString *error, const QString &reason)
{
    if (error)
        *error = reason;
    return false;
}

bool durableDirectory(const QString &path, QString *error)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_WASM)
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return fail(error, QStringLiteral("Cannot open editing queue directory for sync"));
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0)
        return fail(error, QStringLiteral("Cannot sync editing queue directory"));
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
#endif
    return true;
}

QByteArray encodeFrame(quint64 sequence, const QByteArray &transaction)
{
    QByteArray bytes(queue_magic, 8);
    char integer[8];
    qToBigEndian<quint64>(sequence, reinterpret_cast<uchar *>(integer));
    bytes.append(integer, 8);
    qToBigEndian<quint32>(static_cast<quint32>(transaction.size()),
                          reinterpret_cast<uchar *>(integer));
    bytes.append(integer, 4);
    bytes.append(QCryptographicHash::hash(transaction, QCryptographicHash::Sha256));
    bytes.append(transaction);
    return bytes;
}

bool decodeFrame(const QByteArray &bytes, quint64 expected_sequence,
                 const QUuid &expected_id, QByteArray &transaction, QString *error)
{
    if (bytes.size() < header_size ||
        std::memcmp(bytes.constData(), queue_magic, sizeof(queue_magic)) != 0)
        return fail(error, QStringLiteral("Invalid editing queue frame"));
    const uchar *input = reinterpret_cast<const uchar *>(bytes.constData());
    const quint64 sequence = qFromBigEndian<quint64>(input + 8);
    const quint32 length = qFromBigEndian<quint32>(input + 16);
    if (sequence != expected_sequence || length == 0 ||
        length > static_cast<quint32>(max_transaction_size) ||
        static_cast<qsizetype>(length) != bytes.size() - header_size)
        return fail(error, QStringLiteral("Editing queue frame length/sequence mismatch"));
    transaction = bytes.mid(header_size);
    if (QCryptographicHash::hash(transaction, QCryptographicHash::Sha256) !=
        bytes.mid(20, 32))
        return fail(error, QStringLiteral("Editing queue frame checksum mismatch"));
    QString codec_error;
    const std::optional<aowis::model::ModelEditTransaction> decoded =
        aowis::model::ModelEditTransactionCodec::decode(transaction, &codec_error);
    if (!decoded.has_value() || decoded->transactionId != expected_id)
        return fail(error, QStringLiteral("Editing queue transaction identity/content mismatch: %1")
                    .arg(codec_error));
    return true;
}

QString normalizedUuid(const QUuid &id)
{
    return id.toString(QUuid::WithoutBraces).toLower();
}

bool uuidV7(const QUuid &id)
{
    const QByteArray bytes = id.toRfc4122();
    return bytes.size() == 16 && (static_cast<uchar>(bytes.at(6)) >> 4) == 7 &&
        ((static_cast<uchar>(bytes.at(8)) & 0xc0) == 0x80);
}
}

PersonalEditQueue::PersonalEditQueue() = default;

PersonalEditQueue::~PersonalEditQueue()
{
    close();
}

QString PersonalEditQueue::fileName(quint64 sequence, const QUuid &id)
{
    return QStringLiteral("%1_%2.txn")
        .arg(sequence, 16, 16, QLatin1Char('0'))
        .arg(normalizedUuid(id));
}

bool PersonalEditQueue::open(const QString &root, const QUuid &repository_id,
                             const QUuid &draft_id, QString *error)
{
    close();
    if (error)
        error->clear();
    if (root.isEmpty() || !uuidV7(repository_id) || !uuidV7(draft_id))
        return fail(error, QStringLiteral("Editing queue requires a root and UUIDv7 repository/draft"));
    const QString path = QDir(root).filePath(normalizedUuid(repository_id) +
                                             QLatin1Char('/') + normalizedUuid(draft_id));
    if (!QDir().mkpath(path))
        return fail(error, QStringLiteral("Cannot create personal editing queue directory"));
    this->directory_ = QFileInfo(path).absoluteFilePath();
    this->repository_id_ = repository_id;
    this->draft_id_ = draft_id;
    this->lock_ = std::make_unique<QLockFile>(QDir(this->directory_).filePath(QStringLiteral(".queue.lock")));
    if (!this->lock_->tryLock(0))
    {
        close();
        return fail(error, QStringLiteral("Personal draft edit queue is already in use"));
    }
    if (!readExisting(error))
    {
        close();
        return false;
    }
    return true;
}

void PersonalEditQueue::close()
{
    if (this->lock_)
        this->lock_->unlock();
    this->lock_.reset();
    this->directory_.clear();
    this->repository_id_ = QUuid();
    this->draft_id_ = QUuid();
    this->entries_.clear();
    this->next_sequence_ = 1;
    this->pending_bytes_ = 0;
}

bool PersonalEditQueue::readExisting(QString *error)
{
    const QRegularExpression pattern(QStringLiteral("^([0-9a-f]{16})_([0-9a-f-]{36})\\.txn$"));
    const QRegularExpression abandoned_temp(
        QStringLiteral("^\\.?[0-9a-f]{16}_[0-9a-f-]{36}\\.txn\\.[A-Za-z0-9]{6}$"));
    const QFileInfoList files = QDir(this->directory_).entryInfoList(
        QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    quint64 previous_sequence = 0;
    QSet<QUuid> seen_ids;
    for (const QFileInfo &info : files)
    {
        const QString name = info.fileName();
        if (name == QStringLiteral(".queue.lock"))
            continue;
        // A QSaveFile temporary file that was never committed is not an edit.
        if (abandoned_temp.match(name).hasMatch())
            continue;
        const QRegularExpressionMatch match = pattern.match(name);
        if (!match.hasMatch() || info.isSymLink() || info.size() > max_transaction_size + header_size)
            return fail(error, QStringLiteral("Unexpected or oversized editing queue record: %1").arg(name));
        bool good_sequence = false;
        const quint64 sequence = match.captured(1).toULongLong(&good_sequence, 16);
        const QUuid transaction_id(match.captured(2));
        if (!good_sequence || sequence <= previous_sequence || !uuidV7(transaction_id) ||
            seen_ids.contains(transaction_id) ||
            this->entries_.size() >= max_pending_entries)
            return fail(error, QStringLiteral("Invalid editing queue sequence or UUID: %1").arg(name));
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            return fail(error, QStringLiteral("Cannot read editing queue record: %1").arg(name));
        const QByteArray frame = file.readAll();
        QByteArray encoded;
        if (!decodeFrame(frame, sequence, transaction_id, encoded, error))
            return false;
        if (static_cast<quint64>(encoded.size()) > max_pending_bytes - this->pending_bytes_)
            return fail(error, QStringLiteral("Local editing queue exceeds 128 MiB"));
        this->pending_bytes_ += static_cast<quint64>(encoded.size());
        seen_ids.insert(transaction_id);
        this->entries_.append({sequence, transaction_id, encoded});
        previous_sequence = sequence;
    }
    if (previous_sequence == std::numeric_limits<quint64>::max())
        return fail(error, QStringLiteral("Editing queue sequence exhausted"));
    this->next_sequence_ = previous_sequence + 1;
    return true;
}

bool PersonalEditQueue::append(const QByteArray &encoded_transaction, QString *error)
{
    if (error)
        error->clear();
    if (!isOpen())
        return fail(error, QStringLiteral("Personal editing queue is not open"));
    QString codec_error;
    const std::optional<aowis::model::ModelEditTransaction> decoded =
        aowis::model::ModelEditTransactionCodec::decode(encoded_transaction, &codec_error);
    if (!decoded.has_value())
        return fail(error, QStringLiteral("Invalid MODEL editing transaction: %1").arg(codec_error));
    for (const Entry &existing : this->entries_)
    {
        if (existing.transaction_id == decoded->transactionId)
            return existing.encoded_transaction == encoded_transaction ||
                fail(error, QStringLiteral("Conflicting reuse of editing transaction UUID"));
    }
    if (this->entries_.size() >= max_pending_entries ||
        static_cast<quint64>(encoded_transaction.size()) > max_pending_bytes - this->pending_bytes_ ||
        this->next_sequence_ == std::numeric_limits<quint64>::max())
        return fail(error, QStringLiteral("Personal editing queue capacity exceeded; stop editing until synchronized"));
    const QString file_path = QDir(this->directory_).filePath(
        fileName(this->next_sequence_, decoded->transactionId));
    if (QFileInfo::exists(file_path))
        return fail(error, QStringLiteral("Personal editing queue sequence collision"));
    QSaveFile file(file_path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("Cannot write editing transaction to local disk"));
    const QByteArray frame = encodeFrame(this->next_sequence_, encoded_transaction);
    if (file.write(frame) != frame.size() || !file.commit())
        return fail(error, QStringLiteral("Cannot durably commit editing transaction to local disk"));
    if (!durableDirectory(this->directory_, error))
    {
        // Sync status is uncertain; reopen and recover the visible record.
        close();
        return false;
    }
    this->entries_.append({this->next_sequence_, decoded->transactionId, encoded_transaction});
    this->pending_bytes_ += static_cast<quint64>(encoded_transaction.size());
    ++this->next_sequence_;
    return true;
}

bool PersonalEditQueue::acknowledgePersisted(const QUuid &transaction_id, QString *error)
{
    if (error)
        error->clear();
    if (!isOpen())
        return fail(error, QStringLiteral("Personal editing queue is not open"));
    if (this->entries_.isEmpty() || this->entries_.first().transaction_id != transaction_id)
        return fail(error, QStringLiteral("Only the oldest pending transaction may be acknowledged"));
    const Entry &first = this->entries_.first();
    const QString path = QDir(this->directory_).filePath(
        fileName(first.sequence, transaction_id));
    if (!QFile::remove(path))
        return fail(error, QStringLiteral("Cannot remove acknowledged editing transaction"));
    if (!durableDirectory(this->directory_, error))
    {
        close();
        return false;
    }
    this->pending_bytes_ -= static_cast<quint64>(first.encoded_transaction.size());
    this->entries_.removeFirst();
    return true;
}

QString PersonalEditQueue::defaultRoot()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty())
        return {};
    return QDir(directory).filePath(QStringLiteral("personal-draft-edit-queues"));
}

int PersonalEditQueue::pendingTransactionFileCount(const QString &root, const QUuid &repository_id,
                                                   const QUuid &draft_id)
{
    if (root.isEmpty() || repository_id.isNull() || draft_id.isNull())
        return -1;
    const QDir directory(QDir(root).filePath(normalizedUuid(repository_id) +
                                             QLatin1Char('/') + normalizedUuid(draft_id)));
    if (!directory.exists())
        return 0;
    const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{16}_[0-9a-f-]{36}\\.txn$"));
    int count = 0;
    for (const QString &name : directory.entryList(QDir::Files | QDir::Hidden | QDir::System))
        if (pattern.match(name).hasMatch())
            ++count;
    return count;
}

QList<PersonalEditQueue::Entry> PersonalEditQueue::pending() const
{
    return this->entries_;
}

bool PersonalEditQueue::isOpen() const
{
    return this->lock_ && this->lock_->isLocked();
}

QString PersonalEditQueue::directoryPath() const
{
    return this->directory_;
}

QUuid PersonalEditQueue::repositoryId() const
{
    return this->repository_id_;
}

QUuid PersonalEditQueue::draftId() const
{
    return this->draft_id_;
}
