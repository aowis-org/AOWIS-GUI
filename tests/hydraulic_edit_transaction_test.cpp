#include "network/hydraulic_data.h"
#include "network/hydraulic_edit_transaction_capture.h"
#include "network/hydraulic_network_editor.h"
#include "projects/personal_edit_queue.h"
#include "projects/hydraulic_draft_event_adapter.h"
#include "projects/hydraulic_draft_entity_registry.h"
#include "projects/personal_draft_edit_stage.h"
#include "test_harness.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/serialization/model_codec.h>

#include <QCoreApplication>
#include <QObject>
#include <QTemporaryDir>
#include <aowis/model/uuid.h>

#include <utility>

namespace
{
AowisTestHarness test_harness;

CoordinateWGS84 coordinate(double latitude, double longitude)
{
    CoordinateWGS84 result;
    result.latitude_deg = latitude;
    result.longitude_deg = longitude;
    return result;
}

std::optional<aowis::model::ModelEditTransaction> decoded(const QByteArray &bytes)
{
    QString error;
    const std::optional<aowis::model::ModelEditTransaction> transaction =
        aowis::model::ModelEditTransactionCodec::decode(bytes, &error);
    test_harness.expectTrue(transaction.has_value() && error.isEmpty(),
                            "transaction decodes with the canonical MODEL codec");
    return transaction;
}

void testCaptureOnlyTouchedEntities()
{
    NetworkHydraulic network;
    HydraulicNetworkEditor editor(network);
    HydraulicEditTransactionCapture create(QStringLiteral("hydraulic.junction.create"));
    const QUuid junction = editor.addJunction(coordinate(50.1, 8.4));
    test_harness.expectTrue(!junction.isNull(), "creates a junction");
    test_harness.expectTrue(create.watchCreated(junction), "captures entity creation");
    const std::optional<QByteArray> created = create.finish(network);
    test_harness.expectTrue(created.has_value(), "creation emits a reversible event");
    if (!created.has_value())
        return;
    const std::optional<QByteArray> repeated = create.finish(network);
    test_harness.expectTrue(repeated.has_value() && repeated.value() == created.value(),
                            "re-encoding same captured operation keeps UUID and exact bytes");
    const std::optional<aowis::model::ModelEditTransaction> transaction = decoded(created.value());
    if (!transaction.has_value() || transaction->edits.size() != 1)
    {
        test_harness.expectTrue(false, "creation is one edit");
        return;
    }
    const aowis::model::ModelEntityEdit &edit = transaction->edits.first();
    test_harness.expectTrue(edit.entityId == junction && !edit.before.has_value() &&
                            edit.after.has_value(), "creation has only after image");
    if (edit.after.has_value())
    {
        const std::optional<HydraulicNodeJunction> decoded_entity =
            aowis::model::ModelCodec<HydraulicNodeJunction>::decode(edit.after.value());
        test_harness.expectTrue(decoded_entity.has_value() && decoded_entity->uuid == junction,
                                "creation contains typed canonical junction bytes");
    }

    HydraulicEditTransactionCapture no_change(QStringLiteral("hydraulic.junction.rename"));
    test_harness.expectTrue(no_change.watch(network, junction), "watch existing junction");
    test_harness.expectTrue(!no_change.finish(network).has_value(),
                            "unchanged entity emits no edit");

    HydraulicEditTransactionCapture update(QStringLiteral("hydraulic.junction.rename"));
    update.watch(network, junction);
    test_harness.expectTrue(editor.setNodeId(junction, QStringLiteral("J-17")), "rename succeeds");
    const std::optional<QByteArray> updated = update.finish(network);
    test_harness.expectTrue(updated.has_value(), "mutation emits an event");
    if (updated.has_value())
    {
        const std::optional<aowis::model::ModelEditTransaction> update_tx = decoded(updated.value());
        test_harness.expectTrue(update_tx.has_value() && update_tx->edits.size() == 1 &&
                                update_tx->edits.first().before.has_value() &&
                                update_tx->edits.first().after.has_value(),
                                "mutation has both before and after images");
    }

    HydraulicEditTransactionCapture duplicate(QStringLiteral("hydraulic.junction.create"));
    duplicate.watch(network, junction);
    test_harness.expectTrue(!duplicate.watchCreated(junction), "duplicate UUID watch is rejected");
    QString error;
    test_harness.expectTrue(!duplicate.finish(network, &error).has_value() && !error.isEmpty(),
                            "invalid capture fails explicitly");
}

void expectCollection(bool condition, const char *type, const char *detail)
{
    const QByteArray message = QStringLiteral("HC-01 %1: %2")
        .arg(QString::fromLatin1(type), QString::fromLatin1(detail)).toUtf8();
    test_harness.expectTrue(condition, message.constData());
}

template<typename T>
void testCollectionCapture(QList<T> NetworkHydraulic::*collection,
                           const char *type, quint16 kind, quint16 ordinal)
{
    NetworkHydraulic before;
    before.uuid = createUuidV7();
    before.id = QStringLiteral("HC-01 network");
    NetworkHydraulic live = before;
    NetworkHydraulic staged = before;
    T entity;
    entity.uuid = createUuidV7();
    entity.id = QStringLiteral("HC01-original");
    const QUuid entity_id = entity.uuid;
    const QByteArray entity_bytes = aowis::model::ModelCodec<T>::encode(entity);
    expectCollection(!entity_bytes.isEmpty(), type, "canonical entity image is available");
    if (entity_bytes.isEmpty()) return;

    // Creation: before-image is absent, after-image carries the exact MODEL
    // type and canonical bytes, with one corresponding DVMDB membership intent.
    HydraulicEditTransactionCapture creation(QStringLiteral("hc01.create"));
    expectCollection(creation.watchCreated(entity_id), type, "watchCreated accepts UUIDv7");
    (live.*collection).append(entity);
    QString error;
    const std::optional<QByteArray> create_bytes = creation.finish(live, &error);
    expectCollection(create_bytes.has_value() && error.isEmpty(), type, "create transaction encoded");
    if (!create_bytes.has_value()) return;
    const std::optional<aowis::model::ModelEditTransaction> created = decoded(*create_bytes);
    const bool correct_creation = created.has_value() && created->edits.size() == 1 &&
        created->edits.first().entityId == entity_id &&
        created->edits.first().entityType == QString::fromLatin1(type) &&
        !created->edits.first().before.has_value() &&
        created->edits.first().after == std::optional<QByteArray>(entity_bytes);
    expectCollection(correct_creation, type, "create contains the correct typed canonical after-image");

    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    const bool added = HydraulicDraftEventAdapter::prepareWithMembership(
        *create_bytes, staged, records, membership, &error);
    expectCollection(added, type, "create replays against the canonical draft");
    if (!added) return;
    expectCollection(records.size() == 1 && records.first().kind == kind &&
        records.first().payload == entity_bytes && !records.first().deleted &&
        membership.size() == 1 && membership.first().kind == kind &&
        membership.first().collection_index == ordinal && membership.first().inserted &&
        membership.first().entity_id == entity_id && membership.first().network_id == before.uuid,
        type, "create maps to the exact DVMDB record kind and collection ordinal");
    expectCollection(aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                     aowis::model::ModelCodec<NetworkHydraulic>::encode(live),
                     type, "create replay equals the edited network");

    // No-op must produce neither a new event nor a changed membership.
    HydraulicEditTransactionCapture noop(QStringLiteral("hc01.noop"));
    expectCollection(noop.watch(live, entity_id), type, "existing entity can be watched");
    error.clear();
    expectCollection(!noop.finish(live, &error).has_value() && error.isEmpty(),
                     type, "unchanged entity produces no transaction");

    // Updates must contain *both* images; replay must reject a stale repeat.
    HydraulicEditTransactionCapture update(QStringLiteral("hc01.update"));
    expectCollection(update.watch(live, entity_id), type, "update captures before-image");
    (live.*collection).first().id = QStringLiteral("HC01-updated");
    const QByteArray updated_bytes = aowis::model::ModelCodec<T>::encode((live.*collection).first());
    const std::optional<QByteArray> update_bytes = update.finish(live, &error);
    expectCollection(update_bytes.has_value() && error.isEmpty(), type, "update transaction encoded");
    if (!update_bytes.has_value()) return;
    const std::optional<aowis::model::ModelEditTransaction> changed = decoded(*update_bytes);
    expectCollection(changed.has_value() && changed->edits.size() == 1 &&
        changed->edits.first().entityId == entity_id &&
        changed->edits.first().entityType == QString::fromLatin1(type) &&
        changed->edits.first().before == std::optional<QByteArray>(entity_bytes) &&
        changed->edits.first().after == std::optional<QByteArray>(updated_bytes),
        type, "update has canonical before/after images");
    const bool updated = HydraulicDraftEventAdapter::prepareWithMembership(
        *update_bytes, staged, records, membership, &error);
    expectCollection(updated && membership.isEmpty() && records.size() == 1 &&
        records.first().kind == kind && !records.first().deleted &&
        records.first().payload == updated_bytes,
        type, "update replays without a membership mutation");
    if (!updated) return;
    const QByteArray staged_after_update = aowis::model::ModelCodec<NetworkHydraulic>::encode(staged);
    const QList<PersonalWorkingCopyChange> old_records = records;
    const QList<HydraulicDraftMembershipEdit> old_membership = membership;
    error.clear();
    expectCollection(!HydraulicDraftEventAdapter::prepareWithMembership(
        *update_bytes, staged, records, membership, &error) && !error.isEmpty() &&
        aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) == staged_after_update &&
        records.size() == old_records.size() &&
        membership.size() == old_membership.size(),
        type, "stale repeat is rejected without modifying staged model or outputs");
    expectCollection(staged_after_update == aowis::model::ModelCodec<NetworkHydraulic>::encode(live),
                     type, "update replay equals the edited network");

    // Deletion: carries the typed before-image and a removal intent.
    HydraulicEditTransactionCapture deletion(QStringLiteral("hc01.delete"));
    expectCollection(deletion.watch(live, entity_id), type, "delete captures before-image");
    (live.*collection).clear();
    error.clear();
    const std::optional<QByteArray> delete_bytes = deletion.finish(live, &error);
    expectCollection(delete_bytes.has_value() && error.isEmpty(), type, "delete transaction encoded");
    if (!delete_bytes.has_value()) return;
    const std::optional<aowis::model::ModelEditTransaction> removed = decoded(*delete_bytes);
    expectCollection(removed.has_value() && removed->edits.size() == 1 &&
        removed->edits.first().entityType == QString::fromLatin1(type) &&
        removed->edits.first().before == std::optional<QByteArray>(updated_bytes) &&
        !removed->edits.first().after.has_value(),
        type, "delete retains canonical before-image only");
    const bool deleted = HydraulicDraftEventAdapter::prepareWithMembership(
        *delete_bytes, staged, records, membership, &error);
    expectCollection(deleted && records.size() == 1 && records.first().deleted &&
        records.first().kind == kind && membership.size() == 1 &&
        !membership.first().inserted && membership.first().collection_index == ordinal,
        type, "delete replays with the exact membership removal");
    if (deleted)
        expectCollection(aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                         aowis::model::ModelCodec<NetworkHydraulic>::encode(before),
                         type, "create/update/delete returns to the canonical base");
}

void testAllHydraulicEntityCollectionCapture()
{
    constexpr int collection_count = 0
#define COUNT_HYDRAULIC_COLLECTION(name, type, kind, ordinal) + 1
        AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(COUNT_HYDRAULIC_COLLECTION)
#undef COUNT_HYDRAULIC_COLLECTION
        ;
    static_assert(collection_count == 18, "Hydraulic capture must cover every MODEL entity collection");
#define TEST_HYDRAULIC_COLLECTION(name, type, kind, ordinal) \
    testCollectionCapture<type>(&NetworkHydraulic::name, #type, kind, ordinal);
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(TEST_HYDRAULIC_COLLECTION)
#undef TEST_HYDRAULIC_COLLECTION
}

void testAmbiguousAndNullHydraulicCapture()
{
    NetworkHydraulic network;
    network.uuid = createUuidV7();
    network.id = QStringLiteral("HC01-ambiguous");
    const QUuid shared = createUuidV7();
    HydraulicNodeJunction junction;
    junction.uuid = shared;
    junction.id = QStringLiteral("junction");
    HydraulicPipeMaterial material;
    material.uuid = shared;
    material.id = QStringLiteral("material");
    network.nodes_junctions.append(junction);
    network.pipe_materials.append(material);
    HydraulicEditTransactionCapture ambiguous(QStringLiteral("hc01.ambiguous"));
    QString error;
    test_harness.expectTrue(!ambiguous.watch(network, shared) &&
                            !ambiguous.finish(network, &error).has_value() && !error.isEmpty(),
                            "HC-01 duplicated UUID across different collections fails closed");
    HydraulicEditTransactionCapture created(QStringLiteral("hc01.duplicate"));
    test_harness.expectTrue(created.watchCreated(shared), "HC-01 creation watch is initially valid");
    test_harness.expectTrue(!created.finish(network, &error).has_value() &&
                            error.contains(QStringLiteral("Duplicate")),
                            "HC-01 duplicate UUID detected even for a created entity");
    network.pipe_materials.clear();
    network.nodes_junctions.append(junction);
    HydraulicEditTransactionCapture same_type(QStringLiteral("hc01.same-type-duplicate"));
    test_harness.expectTrue(!same_type.watch(network, shared),
                            "HC-01 duplicate UUID within one collection is rejected");
    HydraulicEditTransactionCapture null_uuid(QStringLiteral("hc01.null"));
    test_harness.expectTrue(!null_uuid.watchCreated(QUuid()) &&
                            !null_uuid.finish(network, &error).has_value() && !error.isEmpty(),
                            "HC-01 null UUID never produces an event");
}

void testHydraulicDataLogicalOperations()
{
    HydraulicData data;
    QList<QByteArray> events;
    QString capture_error;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    QObject::connect(&data, &HydraulicData::signalLogicalEditCaptureFailed,
                     &data, [&capture_error](const QString &error) { capture_error = error; });

    test_harness.expectTrue(!data.logicalEditCaptureEnabled(),
                            "example browsing does not record history by default");
    data.setLogicalEditCaptureEnabled(true);
    const QUuid first = data.addJunction(coordinate(50.1, 8.4));
    const QUuid second = data.addJunction(coordinate(50.2, 8.5));
    test_harness.expectTrue(!first.isNull() && !second.isNull() && events.size() == 2,
                            "junction creation produces two separate user actions");

    QList<CoordinateWGS84> vertices;
    vertices.append(coordinate(50.15, 8.45));
    const QUuid pipe_uuid = data.addPipe(first, second, vertices);
    test_harness.expectTrue(!pipe_uuid.isNull() && events.size() == 3,
                            "pipe creation produces one event");
    test_harness.expectTrue(data.setPipeDiameterMm(pipe_uuid, 220.0),
                            "pipe property mutation succeeds");
    test_harness.expectTrue(events.size() == 4, "property edit produces one event");

    HydraulicGeometryBatch batch;
    batch.node_coordinates.insert(first, coordinate(50.12, 8.42));
    batch.node_coordinates.insert(second, coordinate(50.22, 8.52));
    test_harness.expectTrue(data.applyGeometryBatch(batch), "geometry batch succeeds");
    test_harness.expectTrue(events.size() == 5, "multi-node geometry batch produces one undo step");
    if (events.size() == 5)
    {
        const std::optional<aowis::model::ModelEditTransaction> batch_tx = decoded(events.last());
        test_harness.expectTrue(batch_tx.has_value() && batch_tx->edits.size() >= 2,
                                "geometry batch groups both node changes");
    }

    const QUuid inserted = data.addJunction(coordinate(50.15, 8.45));
    const int before_split = events.size();
    const QUuid second_pipe = data.splitPipeAtVertex(pipe_uuid, 0, inserted);
    test_harness.expectTrue(!second_pipe.isNull() && events.size() == before_split + 1,
                            "pipe split produces exactly one logical event");
    if (events.size() == before_split + 1)
    {
        const std::optional<aowis::model::ModelEditTransaction> split_tx = decoded(events.last());
        test_harness.expectTrue(split_tx.has_value() && split_tx->edits.size() == 2,
                                "pipe split contains original and newly created pipe");
    }
    const int before_invalid = events.size();
    test_harness.expectTrue(!data.setPipeDiameterMm(QUuid::createUuid(), 150.0),
                            "invalid pipe edit is rejected");
    test_harness.expectTrue(events.size() == before_invalid,
                            "failed operation is not journaled");
    test_harness.expectTrue(data.deletePipe(second_pipe), "deleting a pipe succeeds");
    test_harness.expectTrue(events.size() == before_invalid + 1,
                            "deletion produces one logical event");
    if (events.size() == before_invalid + 1)
    {
        const std::optional<aowis::model::ModelEditTransaction> deletion = decoded(events.last());
        test_harness.expectTrue(deletion.has_value() && deletion->edits.size() == 1 &&
                                deletion->edits.first().before.has_value() &&
                                !deletion->edits.first().after.has_value(),
                                "deletion contains a reversible before image");
    }
    test_harness.expectTrue(capture_error.isEmpty(), "no encoding errors in covered operations");
    data.replaceNetworkHydraulic(NetworkHydraulic{}, {});
    test_harness.expectTrue(!data.logicalEditCaptureEnabled(),
                            "switching model disables history capture until new working copy is bound");
}
void testPropertyCaptureAndNoop()
{
    HydraulicData data;
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &event) { events.append(event); });
    data.setLogicalEditCaptureEnabled(true);
    const QUuid first = data.addJunction(coordinate(50.0, 8.0));
    const QUuid second = data.addJunction(coordinate(50.1, 8.1));
    const QUuid pipe = data.addPipe(first, second, {});
    const QUuid demand_point = data.addDemandPoint(coordinate(50.05, 8.05));
    test_harness.expectTrue(!first.isNull() && !second.isNull() &&
                            !pipe.isNull() && !demand_point.isNull(),
                            "entities for property capture created");
    if (pipe.isNull() || demand_point.isNull())
        return;

    const int baseline = events.size();
    test_harness.expectTrue(data.setNodeId(first, QStringLiteral("A-17")),
                            "node ID change succeeds");
    test_harness.expectTrue(events.size() == baseline + 1,
                            "node ID produces one reversible transaction");
    if (events.size() > baseline)
    {
        const std::optional<aowis::model::ModelEditTransaction> tx = decoded(events.last());
        test_harness.expectTrue(tx.has_value() && tx->edits.size() == 1 &&
                                tx->edits.first().entityId == first &&
                                tx->edits.first().before.has_value() &&
                                tx->edits.first().after.has_value(),
                                "node ID records only changed node");
    }
    const int after_rename = events.size();
    test_harness.expectTrue(data.setNodeId(first, QStringLiteral("A-17")),
                            "same value accepted by editor");
    test_harness.expectTrue(events.size() == after_rename,
                            "no-op property change creates no undo step");

    test_harness.expectTrue(data.setDemandPointDescription(demand_point,
                                                           QStringLiteral("Street demand")),
                            "demand point description succeeds");
    test_harness.expectTrue(events.size() == after_rename + 1,
                            "demand point property captured");
    if (events.size() > after_rename)
    {
        const std::optional<aowis::model::ModelEditTransaction> tx = decoded(events.last());
        test_harness.expectTrue(tx.has_value() && tx->edits.size() == 1 &&
                                tx->edits.first().entityId == demand_point,
                                "demand point property journaled under its own UUID");
    }

    const int before_pipe = events.size();
    test_harness.expectTrue(data.setPipeMinorLoss(pipe, 0.25),
                            "pipe minor loss succeeds");
    test_harness.expectTrue(events.size() == before_pipe + 1,
                            "previously uncovered pipe property captured");
    const int before_failed = events.size();
    test_harness.expectTrue(!data.setNodeId(QUuid::createUuid(), QStringLiteral("missing")),
                            "invalid node change fails");
    test_harness.expectTrue(events.size() == before_failed && data.logicalEditCaptureEnabled(),
                            "failed mutation is not journaled and capture remains enabled");
}

void testCascadingNodeDeletionIsOneTransaction()
{
    HydraulicData data;
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &event) { events.append(event); });
    data.setLogicalEditCaptureEnabled(true);
    const QUuid first = data.addJunction(coordinate(50.0, 8.0));
    const QUuid second = data.addJunction(coordinate(50.1, 8.1));
    const QUuid pipe = data.addPipe(first, second, {});
    const QUuid point = data.addDemandPoint(coordinate(50.02, 8.02));
    test_harness.expectTrue(data.attachDemandPointToPipe(point, pipe, 0.5),
                            "demand point attaches to soon-deleted pipe");
    const int before_delete = events.size();
    test_harness.expectTrue(data.deleteJunction(first), "delete junction succeeds");
    test_harness.expectTrue(events.size() == before_delete + 1,
                            "cascading node deletion creates one logical undo step");
    if (events.size() != before_delete + 1)
        return;
    const std::optional<aowis::model::ModelEditTransaction> tx = decoded(events.last());
    if (!tx.has_value())
        return;
    test_harness.expectTrue(tx->edits.size() == 3,
                            "node, connected pipe and attachment are one atomic event");
    bool found_node = false;
    bool found_pipe = false;
    bool found_point = false;
    for (const aowis::model::ModelEntityEdit &edit : tx->edits)
    {
        if (edit.entityId == first)
            found_node = edit.before.has_value() && !edit.after.has_value();
        if (edit.entityId == pipe)
            found_pipe = edit.before.has_value() && !edit.after.has_value();
        if (edit.entityId == point)
            found_point = edit.before.has_value() && edit.after.has_value();
    }
    test_harness.expectTrue(found_node && found_pipe && found_point,
                            "delete event preserves all reversible before/after images");
}

void testOrderedHydraulicDraftRecordMapping()
{
    HydraulicData data;
    // A checked-out hydraulic model always has a persistent network identity.
    // An empty HydraulicData instance is only an editor scratch model and is
    // intentionally rejected by MODEL's dependency validator.
    NetworkHydraulic initial_network;
    initial_network.uuid = createUuidV7();
    initial_network.id = QStringLiteral("draft-adapter-regression");
    data.replaceNetworkHydraulic(std::move(initial_network), {});
    const QUuid a = data.addJunction(coordinate(50.0, 8.0));
    const QUuid b = data.addJunction(coordinate(50.1, 8.1));
    const QUuid pipe = data.addPipe(a, b, {});
    test_harness.expectTrue(!a.isNull() && !b.isNull() && !pipe.isNull(),
                            "initial hydraulic model exists without history capture");
    if (pipe.isNull())
        return;
    NetworkHydraulic staged = data.networkHydraulic();
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded, &data,
                     [&events](const QByteArray &bytes) { events.append(bytes); });
    data.setLogicalEditCaptureEnabled(true);
    test_harness.expectTrue(data.setNodeId(a, QStringLiteral("A-draft")),
                            "renaming real junction emits a MODEL event");
    test_harness.expectTrue(data.setPipeDiameterMm(pipe, 225.0),
                            "changing real pipe diameter emits a MODEL event");
    test_harness.expectTrue(events.size() == 2, "two logical edits captured in order");
    if (events.size() != 2)
        return;

    QList<PersonalWorkingCopyChange> records;
    QString error;
    const bool first_prepared = HydraulicDraftEventAdapter::prepare(
        events.at(0), staged, records, &error);
    const QByteArray first_message =
        (QStringLiteral("first event maps to a canonical DVMDB model record: ") + error).toUtf8();
    test_harness.expectTrue(first_prepared, first_message.constData());
    if (!first_prepared)
        return;
    test_harness.expectTrue(records.size() == 1 &&
                            records.at(0).uuid == a.toString(QUuid::WithoutBraces) &&
                            records.at(0).kind == 1 && records.at(0).references.isEmpty(),
                            "junction uses native DVMDB kind and no outgoing entity references");
    const QByteArray first_payload = records.isEmpty() ? QByteArray() : records.first().payload;
    const bool second_prepared = HydraulicDraftEventAdapter::prepare(
        events.at(1), staged, records, &error);
    const QByteArray second_message =
        (QStringLiteral("second event maps after advancing the staged candidate: ") + error).toUtf8();
    test_harness.expectTrue(second_prepared, second_message.constData());
    if (!second_prepared)
        return;
    QStringList endpoints{a.toString(QUuid::WithoutBraces), b.toString(QUuid::WithoutBraces)};
    endpoints.sort(Qt::CaseSensitive);
    test_harness.expectTrue(records.size() == 1 &&
                            records.at(0).uuid == pipe.toString(QUuid::WithoutBraces) &&
                            records.at(0).kind == 4 && records.at(0).references == endpoints,
                            "pipe's canonical references are sorted endpoint UUIDs");
    test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()),
                            "ordered event replay reaches the exact edited network");

    QByteArray corrupted = events.at(1);
    corrupted[corrupted.size() - 1] = static_cast<char>(
        corrupted.at(corrupted.size() - 1) ^ 1);
    NetworkHydraulic corrupted_staged = staged;
    test_harness.expectTrue(!HydraulicDraftEventAdapter::prepare(
                                corrupted, corrupted_staged, records, &error) && !error.isEmpty(),
                            "corrupt event digest fails before any candidate update");

    NetworkHydraulic stale = staged;
    const QList<PersonalWorkingCopyChange> old_records = records;
    test_harness.expectTrue(!HydraulicDraftEventAdapter::prepare(events.at(0), stale, records, &error) &&
                            !error.isEmpty(),
                            "replayed stale event is rejected, not applied twice");
    test_harness.expectTrue(records.size() == old_records.size() &&
                            !records.isEmpty() && !old_records.isEmpty() &&
                            records.first().payload == old_records.first().payload &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(stale) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(staged),
                            "failed replay leaves candidate model and caller records untouched");

    // A new member also needs a DVMDB membership-chunk update; the adapter
    // must not silently acknowledge a record that a revision loader cannot see.
    const QUuid added = data.addJunction(coordinate(50.3, 8.3));
    test_harness.expectTrue(!added.isNull() && events.size() == 3,
                            "entity creation is recorded as a reversible event");
    if (events.size() == 3)
    {
        test_harness.expectTrue(!HydraulicDraftEventAdapter::prepare(events.at(2), staged, records, &error) &&
                                error.contains(QStringLiteral("membership")),
                                "new entities cannot bypass DVMDB network membership");
    }
    test_harness.expectTrue(!first_payload.isEmpty(), "first record contains canonical MODEL bytes");
}

void testHydraulicDraftMembershipIntents()
{
    HydraulicData data;
    NetworkHydraulic initial;
    initial.uuid = createUuidV7();
    initial.id = QStringLiteral("membership-regression");
    data.replaceNetworkHydraulic(std::move(initial), {});
    const QUuid first = data.addJunction(coordinate(50.0, 8.0));
    const QUuid second = data.addJunction(coordinate(50.1, 8.1));
    const QUuid pipe = data.addPipe(first, second, {});
    const QUuid demand = data.addDemandPoint(coordinate(50.02, 8.02));
    if (first.isNull() || second.isNull() || pipe.isNull() || demand.isNull() ||
        !data.attachDemandPointToPipe(demand, pipe, 0.5))
    {
        test_harness.expectTrue(false, "base hydraulic model for membership test is valid");
        return;
    }
    NetworkHydraulic staged = data.networkHydraulic();
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded, &data,
                     [&events](const QByteArray &bytes) { events.append(bytes); });
    data.setLogicalEditCaptureEnabled(true);

    const QUuid new_junction = data.addJunction(coordinate(50.3, 8.3));
    test_harness.expectTrue(!new_junction.isNull() && events.size() == 1,
                            "junction creation is one logical transaction");
    if (events.size() != 1)
        return;
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    QString error;
    const bool created = HydraulicDraftEventAdapter::prepareWithMembership(
        events.first(), staged, records, membership, &error);
    const QByteArray created_message =
        (QStringLiteral("membership insertion prepares: ") + error).toUtf8();
    test_harness.expectTrue(created, created_message.constData());
    if (!created)
        return;
    test_harness.expectTrue(records.size() == 1 &&
                            !records.first().deleted && records.first().kind == 1 &&
                            records.first().uuid == new_junction.toString(QUuid::WithoutBraces) &&
                            !records.first().payload.isEmpty(),
                            "created entity has one canonical insert record");
    test_harness.expectTrue(membership.size() == 1 && membership.first().inserted &&
                            membership.first().network_id == staged.uuid &&
                            membership.first().entity_id == new_junction &&
                            membership.first().kind == 1 &&
                            membership.first().collection_index == 9,
                            "insertion carries network ownership and canonical collection ordinal");
    test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()),
                            "prepared insertion advances exactly to the editor model");

    // Entity-only /save-events cannot materialize the membership chunks.
    NetworkHydraulic incomplete = data.networkHydraulic();
    incomplete.nodes_junctions.removeLast();
    QList<PersonalWorkingCopyChange> sentinel;
    PersonalWorkingCopyChange old;
    old.uuid = QStringLiteral("sentinel");
    sentinel.append(old);
    const QByteArray before_incomplete = aowis::model::ModelCodec<NetworkHydraulic>::encode(incomplete);
    test_harness.expectTrue(!HydraulicDraftEventAdapter::prepare(
                                events.first(), incomplete, sentinel, &error) &&
                            error.contains(QStringLiteral("membership")),
                            "record-only API refuses to acknowledge insertion");
    test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(incomplete) ==
                            before_incomplete && sentinel.size() == 1 &&
                            sentinel.first().uuid == QStringLiteral("sentinel"),
                            "refused insertion leaves staged state and records intact");

    // One deletion cascades to the pipe while changing the demand attachment;
    // both memberships and the dependent entity update remain one event.
    test_harness.expectTrue(data.deleteJunction(first),
                            "cascading deletion succeeds in the hydraulic editor");
    test_harness.expectTrue(events.size() == 2,
                            "cascading deletion produces exactly one additional event");
    if (events.size() != 2)
        return;
    const bool deleted = HydraulicDraftEventAdapter::prepareWithMembership(
        events.last(), staged, records, membership, &error);
    const QByteArray deleted_message =
        (QStringLiteral("cascading membership removal prepares: ") + error).toUtf8();
    test_harness.expectTrue(deleted, deleted_message.constData());
    if (!deleted)
        return;
    bool removed_junction = false;
    bool removed_pipe = false;
    bool updated_demand = false;
    for (const PersonalWorkingCopyChange &record : records)
    {
        if (record.uuid == first.toString(QUuid::WithoutBraces))
            removed_junction = record.deleted && record.kind == 1;
        else if (record.uuid == pipe.toString(QUuid::WithoutBraces))
            removed_pipe = record.deleted && record.kind == 4;
        else if (record.uuid == demand.toString(QUuid::WithoutBraces))
            updated_demand = !record.deleted && record.kind == 7 &&
                             !record.payload.isEmpty();
    }
    test_harness.expectTrue(records.size() == 3 && removed_junction &&
                            removed_pipe && updated_demand,
                            "one event includes both tombstones and related attachment update");
    bool membership_junction = false;
    bool membership_pipe = false;
    for (const HydraulicDraftMembershipEdit &intent : membership)
    {
        if (intent.entity_id == first)
            membership_junction = !intent.inserted && intent.collection_index == 9;
        if (intent.entity_id == pipe)
            membership_pipe = !intent.inserted && intent.collection_index == 12;
    }
    test_harness.expectTrue(membership.size() == 2 && membership_junction && membership_pipe,
                            "only removed members have membership delete intents");
    test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()),
                            "cascading operation replays exactly against staged model");
}

void testCapturedOperationsEnterDurableQueue()
{
    QTemporaryDir root;
    QString error;
    PersonalEditQueue queue;
    const QUuid repository = createUuidV7();
    const QUuid draft = createUuidV7();
    test_harness.expectTrue(queue.open(root.path(), repository, draft, &error),
                            "verified draft's local edit queue opens");
    HydraulicData data;
    bool persisted = true;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded, &data,
                     [&queue, &persisted](const QByteArray &event)
    {
        QString write_error;
        if (!queue.append(event, &write_error))
            persisted = false;
    });
    // Explicit test binding only: general editing remains capture-disabled.
    data.setLogicalEditCaptureEnabled(true);
    const QUuid junction = data.addJunction(coordinate(50.1, 8.4));
    test_harness.expectTrue(!junction.isNull() && data.setNodeId(junction, QStringLiteral("J-offline")),
                            "real editor operations produce queued events");
    test_harness.expectTrue(persisted && queue.pending().size() == 2,
                            "each operation is persisted synchronously before network access");
    const QList<PersonalEditQueue::Entry> written = queue.pending();
    queue.close();
    PersonalEditQueue recovered;
    test_harness.expectTrue(recovered.open(root.path(), repository, draft, &error),
                            "draft events survive editor teardown");
    const QList<PersonalEditQueue::Entry> events = recovered.pending();
    const bool exact_recovery = events.size() == 2 && written.size() == 2 &&
        events.at(0).encoded_transaction == written.at(0).encoded_transaction &&
        events.at(1).encoded_transaction == written.at(1).encoded_transaction;
    test_harness.expectTrue(exact_recovery,
                            "the original MODEL transaction bytes are recoverable after restart");
    // No acknowledgement: DVMDB has not yet saved the logical undo journal.
}


void testVerifiedDraftStageReplayAndAck()
{
    QTemporaryDir root;
    const QUuid repository = createUuidV7();
    const QUuid draft = createUuidV7();
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("Draft Network");
    NetworkHydraulic live = base;
    HydraulicNetworkEditor editor(live);
    QString error;

    HydraulicEditTransactionCapture create(QStringLiteral("hydraulic.junction.create"));
    const QUuid junction = editor.addJunction(coordinate(50.1, 8.4));
    test_harness.expectTrue(!junction.isNull() && create.watchCreated(junction),
                            "draft stage test creates a junction");
    const std::optional<QByteArray> created = create.finish(live, &error);
    test_harness.expectTrue(created.has_value() && error.isEmpty(),
                            "first transaction captured with canonical MODEL codec");
    if (!created.has_value()) return;

    HydraulicEditTransactionCapture rename(QStringLiteral("hydraulic.junction.rename"));
    test_harness.expectTrue(rename.watch(live, junction), "capture before-image of junction");
    test_harness.expectTrue(editor.setNodeId(junction, QStringLiteral("J-local")),
                            "update the same junction while creation is not yet saved remotely");
    const std::optional<QByteArray> updated = rename.finish(live, &error);
    test_harness.expectTrue(updated.has_value() && error.isEmpty(),
                            "second transaction captured");
    if (!updated.has_value()) return;

    PersonalDraftEditStage stage;
    test_harness.expectTrue(stage.open(root.path(), repository, draft, base, {}, &error),
                            "open staging boundary against a verified remote candidate");
    test_harness.expectTrue(stage.append(*created, &error) && stage.append(*updated, &error),
                            "two events durably queued before remote submission");
    test_harness.expectTrue(stage.pendingCount() == 2 && stage.persistedNetwork().nodes_junctions.isEmpty(),
                            "server candidate remains unchanged while live model has pending edits");
    test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(stage.visibleNetwork()) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(live),
                            "local replay reaches the exact live edited network");

    NetworkHydraulic first_candidate;
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    test_harness.expectTrue(stage.prepareOldest(first_candidate, records, membership, &error),
                            "prepare oldest event against server candidate rather than live model");
    test_harness.expectTrue(first_candidate.nodes_junctions.size() == 1 &&
                            first_candidate.nodes_junctions.first().id != QStringLiteral("J-local") &&
                            records.size() == 1 && membership.size() == 1 && membership.first().inserted,
                            "first Save proposes only the junction creation and membership");
    test_harness.expectTrue(!stage.acknowledgeOldest(*updated, &error) &&
                            stage.pendingCount() == 2,
                            "a confirmation for a newer transaction cannot delete the oldest event");

    // Simulated power cut after DVMDB has persisted the first transaction but
    // before the GUI deleted the locally durable event.
    stage.close();
    PersonalDraftEditStage recovered;
    test_harness.expectTrue(recovered.open(root.path(), repository, draft,
                                            first_candidate, *created, &error),
                            "authoritative journal tail allows recovery of a lost Save acknowledgement");
    test_harness.expectTrue(recovered.pendingCount() == 1 &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(recovered.visibleNetwork()) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(live),
                            "recovery drops only saved event and replays later pending edits");
    NetworkHydraulic second_candidate;
    test_harness.expectTrue(recovered.prepareOldest(second_candidate, records, membership, &error),
                            "prepare pending update from verified post-create candidate");
    test_harness.expectTrue(membership.isEmpty() && records.size() == 1 &&
                            !second_candidate.nodes_junctions.isEmpty() &&
                            second_candidate.nodes_junctions.first().id == QStringLiteral("J-local"),
                            "second Save is a property update without duplicate membership insertion");
    test_harness.expectTrue(recovered.acknowledgeOldest(*updated, &error) &&
                            recovered.pendingCount() == 0,
                            "confirmed second journal event advances remote candidate and empties local queue");
    test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(recovered.persistedNetwork()) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(live),
                            "verified and live models converge after both acknowledgements");
}

void testStructuralDraftRecoveryAcrossRestart()
{
    QTemporaryDir root;
    test_harness.expectTrue(root.isValid(), "structural draft queue storage exists");
    if (!root.isValid())
        return;

    const QUuid repository = createUuidV7();
    const QUuid draft = createUuidV7();
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("structural-draft-restart");
    HydraulicData live;
    live.replaceNetworkHydraulic(base, {});
    QList<QByteArray> events;
    QString capture_error;
    QObject::connect(&live, &HydraulicData::signalLogicalEditRecorded, &live,
                     [&events](const QByteArray &event) { events.append(event); });
    QObject::connect(&live, &HydraulicData::signalLogicalEditCaptureFailed, &live,
                     [&capture_error](const QString &error) { capture_error = error; });
    live.setLogicalEditCaptureEnabled(true);

    const QUuid first = live.addJunction(coordinate(50.1, 8.4));
    const QUuid second = live.addJunction(coordinate(50.2, 8.5));
    const QUuid pipe = live.addPipe(first, second, {});
    const bool renamed = live.setNodeId(second, QStringLiteral("J-after-restart"));
    const bool deleted = live.deleteJunction(first);
    test_harness.expectTrue(!first.isNull() && !second.isNull() && !pipe.isNull() &&
                            renamed && deleted && capture_error.isEmpty() && events.size() == 5,
                            "five real editor operations capture creation, connection, update and cascade deletion");
    if (events.size() != 5 || !capture_error.isEmpty())
        return;

    PersonalDraftEditStage stage;
    QString error;
    test_harness.expectTrue(stage.open(root.path(), repository, draft, base, {}, &error),
                            "stage structural edits against empty verified network");
    if (!stage.isOpen())
        return;
    for (const QByteArray &event : events)
    {
        if (!stage.append(event, &error))
        {
            test_harness.expectTrue(false, "all captured structural edits must be durably queued");
            return;
        }
    }
    const QByteArray expected = aowis::model::ModelCodec<NetworkHydraulic>::encode(
        live.networkHydraulic());
    test_harness.expectTrue(stage.pendingCount() == 5 &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(
                                stage.visibleNetwork()) == expected,
                            "offline replay reproduces final network including cascade deletion");

    // Simulate two successful server Saves, with later local events still pending.
    NetworkHydraulic candidate;
    QList<PersonalWorkingCopyChange> changes;
    QList<HydraulicDraftMembershipEdit> membership;
    for (int index = 0; index < 2; ++index)
    {
        if (!stage.prepareOldest(candidate, changes, membership, &error))
        {
            test_harness.expectTrue(false, "prepare structural creation for remote Save");
            return;
        }
        test_harness.expectTrue(changes.size() == 1 && membership.size() == 1 &&
                                membership.first().inserted &&
                                membership.first().collection_index == 9,
                                "each created junction has one atomic membership insertion");
        if (!stage.acknowledgeOldest(events.at(index), &error))
        {
            test_harness.expectTrue(false, "acknowledge verified junction Save");
            return;
        }
    }
    test_harness.expectTrue(stage.pendingCount() == 3,
                            "only acknowledged events are removed before restart");
    const NetworkHydraulic verified_after_two = stage.persistedNetwork();
    stage.close();

    PersonalDraftEditStage restarted;
    test_harness.expectTrue(restarted.open(root.path(), repository, draft,
                                            verified_after_two, events.at(1), &error),
                            "restart replays pending pipe creation, rename and cascade removal");
    if (!restarted.isOpen())
        return;
    test_harness.expectTrue(restarted.pendingCount() == 3 &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(
                                restarted.visibleNetwork()) == expected,
                            "restart reconstructs identical complete network from persisted candidate and queue");
    if (!restarted.prepareOldest(candidate, changes, membership, &error))
    {
        test_harness.expectTrue(false, "prepare pipe creation after restart");
        return;
    }
    const QString first_reference = first.toString(QUuid::WithoutBraces);
    const QString second_reference = second.toString(QUuid::WithoutBraces);
    test_harness.expectTrue(changes.size() == 1 && changes.first().kind == 4 &&
                            changes.first().references.contains(first_reference) &&
                            changes.first().references.contains(second_reference) &&
                            membership.size() == 1 && membership.first().inserted &&
                            membership.first().collection_index == 12,
                            "pipe Save retains canonical endpoint references and atomic membership insertion");

    // Simulate DVMDB persisting the pipe and the process dying before the
    // local acknowledgement: only the exact journal-tail event can be retired.
    restarted.close();
    PersonalDraftEditStage recovered;
    test_harness.expectTrue(recovered.open(root.path(), repository, draft,
                                            candidate, events.at(2), &error),
                            "lost pipe acknowledgement is reconciled from authoritative journal tail");
    if (!recovered.isOpen())
        return;
    test_harness.expectTrue(recovered.pendingCount() == 2 &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(
                                recovered.visibleNetwork()) == expected,
                            "recovery retires only confirmed pipe event and retains later edits");
    if (!recovered.prepareOldest(candidate, changes, membership, &error))
    {
        test_harness.expectTrue(false, "prepare junction rename after restart");
        return;
    }
    test_harness.expectTrue(changes.size() == 1 && membership.isEmpty(),
                            "property-only Save does not mutate membership");
    if (!recovered.acknowledgeOldest(events.at(3), &error) ||
        !recovered.prepareOldest(candidate, changes, membership, &error))
    {
        test_harness.expectTrue(false, "prepare final cascade deletion");
        return;
    }
    int removals = 0;
    for (const HydraulicDraftMembershipEdit &entry : membership)
        if (!entry.inserted)
            ++removals;
    test_harness.expectTrue(changes.size() == 2 && membership.size() == 2 && removals == 2,
                            "cascading junction/pipe deletion sends two atomic membership removals");
    test_harness.expectTrue(recovered.acknowledgeOldest(events.at(4), &error) &&
                            recovered.pendingCount() == 0 &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(
                                recovered.persistedNetwork()) == expected,
                            "all five remote acknowledgements converge to exact live hydraulic model");
    const NetworkHydraulic authoritative_final = recovered.persistedNetwork();
    recovered.close();
    PersonalDraftEditStage final_reopen;
    test_harness.expectTrue(final_reopen.open(root.path(), repository, draft,
                                              authoritative_final, events.at(4), &error) &&
                            final_reopen.pendingCount() == 0 &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(
                                final_reopen.visibleNetwork()) == expected,
                            "second restart leaves no duplicates, ghost links or unsaved transactions");
}

void testDraftStageRejectsOutOfOrderReplay()
{
    QTemporaryDir root;
    const QUuid repository = createUuidV7();
    const QUuid draft = createUuidV7();
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("Replay Guard");
    NetworkHydraulic live = base;
    HydraulicNetworkEditor editor(live);
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.junction.create"));
    const QUuid junction = editor.addJunction(coordinate(50.1, 8.4));
    capture.watchCreated(junction);
    const std::optional<QByteArray> event = capture.finish(live);
    if (!event.has_value())
    {
        test_harness.expectTrue(false, "prepare test event for replay mismatch");
        return;
    }
    QString error;
    PersonalDraftEditStage stage;
    test_harness.expectTrue(stage.open(root.path(), repository, draft, base, {}, &error) &&
                            stage.append(*event, &error),
                            "valid pending event is recorded");
    stage.close();
    PersonalDraftEditStage reopened;
    test_harness.expectTrue(!reopened.open(root.path(), repository, draft, live, {}, &error) &&
                            error.contains(QStringLiteral("replay"), Qt::CaseInsensitive),
                            "stale server journal cannot silently replay event against an already changed model");
    PersonalEditQueue raw;
    test_harness.expectTrue(raw.open(root.path(), repository, draft, &error) &&
                            raw.pending().size() == 1,
                            "rejected replay never removes the locally durable transaction");
}

}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_harness.runCase("Only watched entities", testCaptureOnlyTouchedEntities);
    test_harness.runCase("All 18 hydraulic collections", testAllHydraulicEntityCollectionCapture);
    test_harness.runCase("Hydraulic collection UUID ambiguity", testAmbiguousAndNullHydraulicCapture);
    test_harness.runCase("HydraulicData logical operations", testHydraulicDataLogicalOperations);
    test_harness.runCase("Hydraulic property capture and no-op", testPropertyCaptureAndNoop);
    test_harness.runCase("Cascading node deletion", testCascadingNodeDeletionIsOneTransaction);
    test_harness.runCase("Hydraulic event to canonical DVMDB records", testOrderedHydraulicDraftRecordMapping);
    test_harness.runCase("Hydraulic membership intent mapping", testHydraulicDraftMembershipIntents);
    test_harness.runCase("Capture to durable offline queue", testCapturedOperationsEnterDurableQueue);
    test_harness.runCase("Verified draft staging and recovery", testVerifiedDraftStageReplayAndAck);
    test_harness.runCase("Structural editing across draft restarts", testStructuralDraftRecoveryAcrossRestart);
    test_harness.runCase("Reject unverified draft replay", testDraftStageRejectsOutOfOrderReplay);
    return test_harness.finish();
}
