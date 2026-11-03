#pragma once

#include <QJsonObject>
#include <QString>

// One durable Commit intent per private repository / project / branch.
// A local crash or lost HTTP reply must never create a second operation UUID.
struct PersonalCommitIntent final
{
    QString repository_id;
    QString project_id;
    QString branch_id;
    QString endpoint;
    QString operation_id;
    QJsonObject body;
    QString state = QStringLiteral("pending");
    QString revision;
    QString reason;
};

class PersonalCommitIntentStore final
{
public:
    enum class ReadResult { Missing, Found, Error };

    explicit PersonalCommitIntentStore(const QString &root = defaultRoot());

    static QString defaultRoot();
    ReadResult read(const QString &repository_id, const QString &project_id,
                    const QString &branch_id, PersonalCommitIntent &intent,
                    QString *error = nullptr) const;
    bool create(const PersonalCommitIntent &intent, QString *error = nullptr) const;
    bool resolve(const PersonalCommitIntent &intent, const QString &state,
                 const QString &revision, const QString &reason,
                 QString *error = nullptr) const;
    // Only a persisted terminal result may be acknowledged and removed.
    bool acknowledge(const PersonalCommitIntent &intent, QString *error = nullptr) const;

private:
    QString filePath(const QString &repository_id, const QString &project_id,
                     const QString &branch_id) const;
    QString root_;
};
