#pragma once

#include "projects/personal_working_copy_client.h"

#include <aowis/model/hydraulic/network_hydraulic_distribution.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUuid>

// Converts one canonical, reversible MODEL edit event into the typed records
// required by DVMDB /save-events. `staged` is the network state immediately
// BEFORE the event, never the editor's possibly newer live network.
//
// Success advances staged by exactly one logical operation; errors leave both
// staged and output untouched. Replay queued events in order from a verified
// base plus the authoritative saved draft, NOT from today's editor snapshot.
//
// DVMDB owns the physical manifest and membership chunks. The adapter emits
// logical membership intents for its server-side draft materializer; it never
// encodes DVMDB storage internals or pretends entity records alone are enough.
// HydraulicDraftMembershipEdit is declared with the event-save client, which
// transports it unchanged to DVMDB.
class HydraulicDraftEventAdapter final
{
public:
    // Full logical preparation: includes typed record changes AND explicit
    // membership intents. Neither is applied to the caller until validation.
    // DVMDB must atomically materialize the membership intents with records.
    static bool prepareWithMembership(
                        const QByteArray &encoded_event,
                        NetworkHydraulicDistribution &staged,
                        QList<PersonalWorkingCopyChange> &records,
                        QList<HydraulicDraftMembershipEdit> &membership,
                        QString *error = nullptr);

    // Existing record-only API intentionally refuses membership mutations:
    // they cannot safely be submitted to /save-events as entity records alone.
    static bool prepare(const QByteArray &encoded_event,
                        NetworkHydraulicDistribution &staged,
                        QList<PersonalWorkingCopyChange> &records,
                        QString *error = nullptr);
};
