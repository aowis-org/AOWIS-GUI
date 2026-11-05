#pragma once

#include <aowis/model/hydraulic/network_hydraulic.h>

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// The source snapshot and the exact operation identity are written BEFORE
// sending /history/navigate. The intent is local to the personal repository
// and draft, not to a branch on an unrelated source server. On a process
// restart the snapshot is validated and used only for hash-checked replay.
struct PersonalHistoryNavigationIntent final
{
    QString repository_id;
    QString draft_id;
    QString project_id;
    QString network_id;
    QString session_id;
    QString base_revision;
    QString operation_id;
    QString source_generation;
    QJsonObject source_cursor;
    QJsonObject target_coordinate;
    NetworkHydraulic source_network;
    QList<QByteArray> journal_events;
};

class PersonalHistoryNavigationIntentStore final
{
public:
    enum class ReadResult { Missing, Found, Error };

    explicit PersonalHistoryNavigationIntentStore(const QString &queue_root);
    ReadResult read(const QString &repository_id, const QString &draft_id,
                    PersonalHistoryNavigationIntent &intent,
                    QString *error = nullptr) const;
    bool create(const PersonalHistoryNavigationIntent &intent,
                QString *error = nullptr) const;
    bool clear(const QString &repository_id, const QString &draft_id,
               const QString &operation_id, QString *error = nullptr) const;

private:
    QString path(const QString &repository_id, const QString &draft_id) const;
    QString root_;
};
