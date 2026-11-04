#pragma once

#include "projects/hydraulic_draft_event_adapter.h"
#include "projects/personal_edit_queue.h"

#include <aowis/model/hydraulic/network_hydraulic.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUuid>

// Strict local boundary between the live hydraulic model and the private
// DVMDB writer. All pending events are replayed against the last VERIFIED
// server candidate, never against the latest (possibly further-ahead) editor
// state. This class does not send HTTP requests or claim remote durability.
//
// The caller must obtain an authoritative draft and canonical network before
// open(). On restart, last_journal_event may be passed ONLY if it was read
// from that same authoritative draft. A matching queue head is then known to
// have been saved before the GUI lost its acknowledgement.
class PersonalDraftEditStage final
{
public:
    bool open(const QString &queue_root, const QUuid &repository_id,
              const QUuid &draft_id, const NetworkHydraulic &verified_candidate,
              const QByteArray &last_journal_event = QByteArray(),
              QString *error = nullptr);
    void close();
    // After a verified server-side History navigation: the queue must be empty
    // and its lock remains held throughout the replacement.
    bool resetVerifiedNetwork(const NetworkHydraulic &network, QString *error = nullptr);

    // Stage a newly captured MODEL event, then write it to the local durable
    // queue. The visible staged snapshot advances only after local durability.
    // If append fails, the caller MUST stop editor capture: its live model
    // might already contain a mutation that is not durably queued.
    bool append(const QByteArray &encoded_event, QString *error = nullptr);

    // Prepare precisely the oldest pending event for /save-events. This
    // operation NEVER mutates either snapshot or the queue. The next snapshot
    // can be accepted only after DVMDB has confirmed the event and candidate
    // changes atomically, including recovery of an uncertain acknowledgement.
    bool prepareOldest(NetworkHydraulic &next_candidate,
                       QList<PersonalWorkingCopyChange> &records,
                       QList<HydraulicDraftMembershipEdit> &membership,
                       QString *error = nullptr) const;

    // Call only after the server has authoritatively confirmed the exact
    // oldest event. Removing its local queue file is part of acknowledgement;
    // an I/O failure blocks this stage and requires a fresh authoritative
    // reload instead of risking duplicate application.
    bool acknowledgeOldest(const QByteArray &confirmed_event, QString *error = nullptr);

    bool isOpen() const;
    bool isBlocked() const;
    int pendingCount() const;
    const NetworkHydraulic &visibleNetwork() const;
    const NetworkHydraulic &persistedNetwork() const;
    const PersonalEditQueue &queue() const;

private:
    PersonalEditQueue queue_;
    NetworkHydraulic persisted_;
    NetworkHydraulic visible_;
    bool blocked_ = false;
};
