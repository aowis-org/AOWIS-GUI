#include "network/hydraulic_data.h"
#include "network/hydraulic_edit_transaction_capture.h"
#include "network/hydraulic_network_editor.h"
#include "projects/personal_edit_queue.h"
#include "projects/hydraulic_draft_event_adapter.h"
#include "projects/hydraulic_draft_entity_registry.h"
#include "projects/personal_draft_edit_stage.h"
#include "test_harness.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_root_edit.h>
#include <aowis/model/serialization/model_codec.h>

#include <QCoreApplication>
#include <QObject>
#include <QTemporaryDir>
#include <aowis/model/uuid.h>

#include <limits>
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

void testTerrainRelativeNodeCreationIsAtomic()
{
    HydraulicData data;
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    data.setLogicalEditCaptureEnabled(true);

    const QUuid junction_uuid = data.addJunction(coordinate(50.0, 8.0), 0.0);
    const std::optional<HydraulicNodeJunction> junction = data.junction(junction_uuid);
    test_harness.expectTrue(junction.has_value() &&
        junction->elevation_input_type == HydraulicNodeElevationInputType::TerrainElevationAndOffset &&
        junction->terrain_elevation_m == 0.0 && junction->elevation_offset_m == 0.0,
        "3D junction accepts sea-level DEM and starts in terrain + zero-offset mode");

    const QUuid tank_uuid = data.addTank(coordinate(50.1, 8.1), 152.25);
    const std::optional<HydraulicNodeTank> tank = data.tank(tank_uuid);
    test_harness.expectTrue(tank.has_value() &&
        tank->elevation_input_type == HydraulicNodeTankElevationInputType::TerrainElevationAndOffset &&
        tank->terrain_elevation_m == 152.25 && tank->bottom_offset_m == 0.0,
        "3D tank bottom uses sampled terrain without creating a separate visual height");

    const QUuid reservoir_uuid = data.addReservoir(coordinate(50.2, 8.2), -4.5);
    const std::optional<HydraulicNodeReservoir> reservoir = data.reservoir(reservoir_uuid);
    test_harness.expectTrue(reservoir.has_value() &&
        reservoir->head_input_type == HydraulicNodeElevationInputType::TerrainElevationAndOffset &&
        reservoir->terrain_elevation_m == -4.5 && reservoir->hydraulic_head_offset_m == 0.0,
        "3D reservoir uses the terrain-relative hydraulic-head rule");

    test_harness.expectTrue(events.size() == 3,
        "each 3D node is created in exactly one hydraulic logical edit");
    for (const QByteArray &bytes : events)
    {
        const std::optional<aowis::model::ModelEditTransaction> event = decoded(bytes);
        test_harness.expectTrue(event.has_value() && event->edits.size() == 1 &&
            !event->edits.first().before.has_value() &&
            event->edits.first().after.has_value(),
            "terrain-relative node creation is one reversible insertion");
    }

    const int previous_events = events.size();
    const double invalid_height = std::numeric_limits<double>::quiet_NaN();
    test_harness.expectTrue(data.addJunction(coordinate(51.0, 9.0), invalid_height).isNull() &&
        data.addReservoir(coordinate(51.0, 9.0), invalid_height).isNull() &&
        data.addTank(coordinate(51.0, 9.0), invalid_height).isNull() &&
        events.size() == previous_events,
        "nonfinite DEM heights reject the entire creation without partial writes");

    const QUuid flat_uuid = data.addJunction(coordinate(51.0, 9.0));
    const std::optional<HydraulicNodeJunction> flat = data.junction(flat_uuid);
    test_harness.expectTrue(flat.has_value() &&
        flat->elevation_input_type != HydraulicNodeElevationInputType::TerrainElevationAndOffset,
        "existing 2D junction creation retains its original elevation mode");
}

void testTerrainAwareNodeMovementIsAtomic()
{
    HydraulicData data;
    const QUuid junction_uuid = data.addJunction(coordinate(50.0, 8.0), 100.0);
    const QUuid tank_uuid = data.addTank(coordinate(50.1, 8.1), 120.0);
    const QUuid reservoir_uuid = data.addReservoir(coordinate(50.2, 8.2), 140.0);
    const QUuid fixed_uuid = data.addJunction(coordinate(50.3, 8.3));
    const QUuid pipe_uuid = data.addPipe(junction_uuid, fixed_uuid, {});
    const std::optional<HydraulicLinkPipe> original_pipe = data.pipe(pipe_uuid);
    test_harness.expectTrue(!pipe_uuid.isNull() && original_pipe.has_value(),
        "connected pipe exists before the 3D node move");
    test_harness.expectTrue(!junction_uuid.isNull() && !tank_uuid.isNull() &&
        !reservoir_uuid.isNull() && !fixed_uuid.isNull(),
        "created terrain-relative and absolute nodes for move tests");

    test_harness.expectTrue(data.setJunctionElevationOffsetM(junction_uuid, 3.5) &&
        data.setTankBottomOffsetM(tank_uuid, 8.0) &&
        data.setReservoirHeadOffsetM(reservoir_uuid, 12.0) &&
        data.setJunctionElevationM(fixed_uuid, 175.0),
        "established offsets and absolute elevation before moving");

    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    data.setLogicalEditCaptureEnabled(true);

    HydraulicGeometryBatch batch;
    batch.node_coordinates.insert(junction_uuid, coordinate(51.0, 9.0));
    batch.node_terrain_elevations_m.insert(junction_uuid, 160.0);
    batch.node_coordinates.insert(tank_uuid, coordinate(51.1, 9.1));
    batch.node_terrain_elevations_m.insert(tank_uuid, 180.0);
    batch.node_coordinates.insert(reservoir_uuid, coordinate(51.2, 9.2));
    batch.node_terrain_elevations_m.insert(reservoir_uuid, 200.0);
    batch.node_coordinates.insert(fixed_uuid, coordinate(51.3, 9.3));
    test_harness.expectTrue(data.applyGeometryBatch(batch),
        "one hydraulic geometry batch updates coordinates and sampled terrain");

    const std::optional<HydraulicNodeJunction> junction = data.junction(junction_uuid);
    const std::optional<HydraulicNodeTank> tank = data.tank(tank_uuid);
    const std::optional<HydraulicNodeReservoir> reservoir = data.reservoir(reservoir_uuid);
    const std::optional<HydraulicNodeJunction> fixed = data.junction(fixed_uuid);
    test_harness.expectTrue(junction.has_value() &&
        junction->coordinate_wgs84.longitude_deg == 9.0 &&
        junction->terrain_elevation_m == 160.0 && junction->elevation_offset_m == 3.5 &&
        junction->elevation_input_type == HydraulicNodeElevationInputType::TerrainElevationAndOffset,
        "junction follows new terrain but preserves offset and input mode");
    test_harness.expectTrue(tank.has_value() &&
        tank->coordinate_wgs84.longitude_deg == 9.1 &&
        tank->terrain_elevation_m == 180.0 && tank->bottom_offset_m == 8.0,
        "tank follows new terrain but preserves bottom offset");
    test_harness.expectTrue(reservoir.has_value() &&
        reservoir->coordinate_wgs84.longitude_deg == 9.2 &&
        reservoir->terrain_elevation_m == 200.0 && reservoir->hydraulic_head_offset_m == 12.0,
        "reservoir terrain component follows move without changing head offset");
    test_harness.expectTrue(fixed.has_value() &&
        fixed->coordinate_wgs84.longitude_deg == 9.3 &&
        fixed->elevation_input_type == HydraulicNodeElevationInputType::TotalElevation &&
        fixed->elevation_m == 175.0,
        "absolute elevation remains locked during horizontal movement");
    const std::optional<HydraulicLinkPipe> moved_pipe = data.pipe(pipe_uuid);
    test_harness.expectTrue(original_pipe.has_value() && moved_pipe.has_value() &&
        original_pipe->length_calculated_m != moved_pipe->length_calculated_m,
        "a node move recalculates its connected pipe length");
    test_harness.expectTrue(events.size() == 1,
        "a multi-node geometry and terrain move emits exactly one edit transaction");
    if (events.size() == 1)
    {
        const std::optional<aowis::model::ModelEditTransaction> event = decoded(events.first());
        test_harness.expectTrue(event.has_value() && event->edits.size() == 5,
            "one transaction captures four nodes and the connected pipe");
    }

    HydraulicGeometryBatch invalid;
    invalid.node_coordinates.insert(junction_uuid, coordinate(52.0, 10.0));
    invalid.node_terrain_elevations_m.insert(junction_uuid,
        std::numeric_limits<double>::quiet_NaN());
    test_harness.expectTrue(!data.applyGeometryBatch(invalid) &&
        data.junction(junction_uuid)->coordinate_wgs84.longitude_deg == 9.0 &&
        data.junction(junction_uuid)->terrain_elevation_m == 160.0 && events.size() == 1,
        "invalid DEM height aborts the entire move without any partial changes");

    HydraulicGeometryBatch wrong_mode;
    wrong_mode.node_coordinates.insert(fixed_uuid, coordinate(52.3, 10.3));
    wrong_mode.node_terrain_elevations_m.insert(fixed_uuid, 195.0);
    test_harness.expectTrue(!data.applyGeometryBatch(wrong_mode) &&
        data.junction(fixed_uuid)->elevation_m == 175.0 &&
        data.junction(fixed_uuid)->coordinate_wgs84.longitude_deg == 9.3 &&
        events.size() == 1,
        "DEM edits cannot silently alter an absolute-elevation node");

    HydraulicGeometryBatch zero_terrain;
    zero_terrain.node_coordinates.insert(junction_uuid, coordinate(53.0, 11.0));
    zero_terrain.node_terrain_elevations_m.insert(junction_uuid, 0.0);
    test_harness.expectTrue(data.applyGeometryBatch(zero_terrain) &&
        data.junction(junction_uuid)->terrain_elevation_m == 0.0 &&
        data.junction(junction_uuid)->elevation_offset_m == 3.5 && events.size() == 2,
        "zero-metre DEM is valid and preserves the node offset");
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
// The private draft stage and DVMDB append a created entity to the end of its
// collection. Every editor operation must do the same, or the live model and
// the replayed candidate differ in order (and private editing blocks). A pipe
// split of a pipe that is not the last one is the case that used to differ.
void testPipeSplitKeepsCanonicalCollectionOrder()
{
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("Split order");
    HydraulicData data;
    data.replaceNetworkHydraulic(base, {});
    const QUuid first = data.addJunction(coordinate(50.1, 8.4));
    const QUuid middle = data.addJunction(coordinate(50.2, 8.5));
    const QUuid last = data.addJunction(coordinate(50.3, 8.6));
    QList<CoordinateWGS84> vertices;
    vertices.append(coordinate(50.15, 8.45));
    const QUuid split_pipe = data.addPipe(first, middle, vertices);
    const QUuid trailing_pipe = data.addPipe(middle, last, {});
    test_harness.expectTrue(!split_pipe.isNull() && !trailing_pipe.isNull() &&
                            data.networkHydraulic().links_pipes.size() == 2 &&
                            data.networkHydraulic().links_pipes.first().uuid == split_pipe,
                            "split pipe is not the last pipe");

    const NetworkHydraulic verified = data.networkHydraulic();
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    data.setLogicalEditCaptureEnabled(true);
    const QUuid inserted = data.addJunction(coordinate(50.15, 8.45));
    const QUuid created_pipe = data.splitPipeAtVertex(split_pipe, 0, inserted);
    test_harness.expectTrue(!created_pipe.isNull() && events.size() == 2,
                            "junction and split are two captured events");
    test_harness.expectTrue(data.networkHydraulic().links_pipes.size() == 3 &&
                            data.networkHydraulic().links_pipes.last().uuid == created_pipe,
                            "the pipe created by a split is appended");

    NetworkHydraulic staged = verified;
    for (const QByteArray &event : events)
    {
        QList<PersonalWorkingCopyChange> records;
        QList<HydraulicDraftMembershipEdit> membership;
        QString error;
        test_harness.expectTrue(HydraulicDraftEventAdapter::prepareWithMembership(
                                    event, staged, records, membership, &error),
                                "captured split events replay on the verified candidate");
    }
    test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()),
                            "replayed candidate equals the live model byte for byte after a split");
}

// An event DVMDB can never accept (here 130 created junctions in one
// operation) must be refused before it reaches the durable queue; otherwise
// every reopen would replay it and fail to save it again.
void testDraftStageRefusesUnsaveableEvent()
{
    QTemporaryDir root;
    const QUuid repository = createUuidV7();
    const QUuid draft = createUuidV7();
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("Bulk guard");
    NetworkHydraulic live = base;
    HydraulicNetworkEditor editor(live);
    HydraulicEditTransactionCapture bulk(QStringLiteral("hydraulic.junction.bulk"));
    for (int index = 0; index < 130; ++index)
    {
        const QUuid junction = editor.addJunction(coordinate(50.0 + index * 0.001, 8.4));
        bulk.watchCreated(junction);
    }
    const std::optional<QByteArray> event = bulk.finish(live);
    test_harness.expectTrue(event.has_value(), "bulk operation is one captured event");
    if (!event.has_value())
        return;
    QString error;
    PersonalDraftEditStage stage;
    test_harness.expectTrue(stage.open(root.path(), repository, draft, base, {}, &error),
                            "stage opens on the verified candidate");
    test_harness.expectTrue(!stage.append(*event, &error) &&
                            error.contains(QStringLiteral("at most 128")),
                            "an event beyond the DVMDB save bounds is refused");
    test_harness.expectTrue(stage.pendingCount() == 0 && !stage.isBlocked(),
                            "the refused event is not queued");
    stage.close();
    PersonalEditQueue raw;
    test_harness.expectTrue(raw.open(root.path(), repository, draft, &error) &&
                            raw.pending().isEmpty(),
                            "nothing durable refers to the refused event");
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

// HC-02.4: library UUID references cannot name missing or wrong-type records.
// Invalid attempts must leave canonical bytes unchanged and emit no edit.
void testHydraulicLibraryReferenceValidation()
{
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("library-reference-guard");
    const QUuid missing = createUuidV7();

    HydraulicPipeMaterial material;
    material.uuid = createUuidV7();
    material.id = QStringLiteral("material");
    base.pipe_materials.append(material);
    HydraulicPatternTime pattern;
    pattern.uuid = createUuidV7();
    pattern.id = QStringLiteral("pattern");
    base.patterns_time.append(pattern);
    HydraulicCurveTankVolume tank_curve;
    tank_curve.uuid = createUuidV7();
    tank_curve.id = QStringLiteral("tank-curve");
    base.curves_tank_volume.append(tank_curve);
    HydraulicCurvePumpEfficiency efficiency_curve;
    efficiency_curve.uuid = createUuidV7();
    efficiency_curve.id = QStringLiteral("efficiency-curve");
    base.curves_pump_efficiency.append(efficiency_curve);
    HydraulicCurveValveHeadloss headloss_curve;
    headloss_curve.uuid = createUuidV7();
    headloss_curve.id = QStringLiteral("headloss-curve");
    base.curves_valve_headloss.append(headloss_curve);
    HydraulicCurveValveCharacteristic characteristic_curve;
    characteristic_curve.uuid = createUuidV7();
    characteristic_curve.id = QStringLiteral("characteristic-curve");
    base.curves_valve_characteristic.append(characteristic_curve);

    HydraulicNodeJunction junction;
    junction.uuid = createUuidV7();
    junction.id = QStringLiteral("J-1");
    junction.demands.append(HydraulicDemand{});
    base.nodes_junctions.append(junction);
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    reservoir.id = QStringLiteral("R-1");
    base.nodes_reservoirs.append(reservoir);
    HydraulicNodeTank tank;
    tank.uuid = createUuidV7();
    tank.id = QStringLiteral("T-1");
    base.nodes_tanks.append(tank);
    HydraulicDemandPoint demand_point;
    demand_point.uuid = createUuidV7();
    demand_point.id = QStringLiteral("DP-1");
    demand_point.demands.append(HydraulicDemand{});
    base.demand_points.append(demand_point);
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.id = QStringLiteral("P-1");
    pipe.node_uuid_from = junction.uuid;
    pipe.node_uuid_to = reservoir.uuid;
    base.links_pipes.append(pipe);
    HydraulicLinkPump pump;
    pump.uuid = createUuidV7();
    pump.id = QStringLiteral("PU-1");
    pump.node_uuid_from = reservoir.uuid;
    pump.node_uuid_to = tank.uuid;
    base.links_pumps.append(pump);
    HydraulicLinkValve valve;
    valve.uuid = createUuidV7();
    valve.id = QStringLiteral("V-1");
    valve.node_uuid_from = tank.uuid;
    valve.node_uuid_to = junction.uuid;
    base.links_valves.append(valve);

    HydraulicData data;
    data.replaceNetworkHydraulic(base, {});
    QList<QByteArray> events;
    QString capture_error;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded, &data,
                     [&events](const QByteArray &event) { events.append(event); });
    QObject::connect(&data, &HydraulicData::signalLogicalEditCaptureFailed, &data,
                     [&capture_error](const QString &message) { capture_error = message; });
    data.setLogicalEditCaptureEnabled(true);
    const QByteArray before = aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic());
    const bool rejected =
        !data.setDemandPointDemandPatternUuid(demand_point.uuid, 0, missing) &&
        !data.setJunctionDemandPatternUuid(junction.uuid, 0, missing) &&
        !data.setNodeQualitySourcePatternUuid(junction.uuid, missing) &&
        !data.setReservoirHeadPatternUuid(reservoir.uuid, missing) &&
        !data.setTankVolumeCurveUuid(tank.uuid, missing) &&
        !data.setPipeMaterialUuid(pipe.uuid, missing) &&
        !data.setPumpSpeedPatternUuid(pump.uuid, missing) &&
        !data.setPumpEfficiencyInput(pump.uuid, HydraulicLinkPumpEfficiencyInputType{}, missing) &&
        !data.setPumpEnergyPriceInput(pump.uuid, HydraulicLinkPumpEnergyPriceInputType{}, missing) &&
        !data.setValveHeadLossCurveUuid(valve.uuid, missing) &&
        !data.setValveCharacteristicCurveUuid(valve.uuid, missing);
    test_harness.expectTrue(rejected && events.isEmpty() && capture_error.isEmpty() &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()) == before,
                            "all eleven missing library references are rejected without mutation or autosave");

    // A known UUID from the wrong library must not accidentally pass.
    test_harness.expectTrue(!data.setTankVolumeCurveUuid(tank.uuid, efficiency_curve.uuid) &&
                            !data.setValveHeadLossCurveUuid(valve.uuid, characteristic_curve.uuid) &&
                            events.isEmpty(),
                            "references to the wrong hydraulic library type are rejected");

    const bool accepted =
        data.setPipeMaterialUuid(pipe.uuid, material.uuid) &&
        data.setPumpSpeedPatternUuid(pump.uuid, pattern.uuid) &&
        data.setTankVolumeCurveUuid(tank.uuid, tank_curve.uuid) &&
        data.setValveHeadLossCurveUuid(valve.uuid, headloss_curve.uuid) &&
        data.setValveCharacteristicCurveUuid(valve.uuid, characteristic_curve.uuid);
    test_harness.expectTrue(accepted && events.size() == 5 && capture_error.isEmpty(),
                            "five valid library references produce five canonical autosave transactions");
    if (!accepted || events.size() != 5)
        return;

    NetworkHydraulic staged = base;
    QString error;
    bool replayed = true;
    for (const QByteArray &event : events)
    {
        QList<PersonalWorkingCopyChange> records;
        QList<HydraulicDraftMembershipEdit> membership;
        if (!HydraulicDraftEventAdapter::prepareWithMembership(
                event, staged, records, membership, &error) || records.size() != 1 || !membership.isEmpty())
        {
            replayed = false;
            break;
        }
    }
    test_harness.expectTrue(replayed && error.isEmpty() &&
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()),
                            "valid library reference edits replay to exactly the live canonical network");
}

// HC-02.5: exercise the real HydraulicData CRUD endpoints, rather than
// constructing MODEL edits directly as the HC-01 registry tests do.
template<typename Entity>
void testLibraryCrud(const char *name, quint16 kind, quint16 ordinal,
                     QList<Entity> NetworkHydraulic::*collection,
                     QUuid (HydraulicData::*create)(Entity),
                     bool (HydraulicData::*update)(const Entity &),
                     bool (HydraulicData::*remove)(const QUuid &))
{
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("HC025 library");
    HydraulicData data;
    data.replaceNetworkHydraulic(base, {});
    QList<QByteArray> events;
    QString capture_error;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded, &data,
                     [&events](const QByteArray &bytes) { events.append(bytes); });
    QObject::connect(&data, &HydraulicData::signalLogicalEditCaptureFailed, &data,
                     [&capture_error](const QString &message) { capture_error = message; });
    data.setLogicalEditCaptureEnabled(true);
    Entity original;
    original.id = QStringLiteral("Library original");
    original.uuid = createUuidV7(); // add must replace this with a fresh ID
    const QUuid created = (data.*create)(original);
    expectCollection(!created.isNull() && created != original.uuid &&
                     (created.toRfc4122().at(6) & 0xf0) == 0x70 && events.size() == 1 &&
                     capture_error.isEmpty(), name, "create assigns UUIDv7 and emits one event");
    if (events.size() != 1 || created.isNull()) return;
    NetworkHydraulic staged = base;
    QString error;
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    const bool inserted = HydraulicDraftEventAdapter::prepareWithMembership(
        events.at(0), staged, records, membership, &error);
    expectCollection(inserted && records.size() == 1 && !records.first().deleted &&
                     records.first().kind == kind && membership.size() == 1 &&
                     membership.first().inserted && membership.first().kind == kind &&
                     membership.first().collection_index == ordinal &&
                     membership.first().entity_id == created &&
                     (staged.*collection).size() == 1,
                     name, "create replays with correct membership insertion");
    if (!inserted) return;
    Entity changed = (data.networkHydraulic().*collection).first();
    changed.id = QStringLiteral("Library updated");
    const bool updated = (data.*update)(changed);
    expectCollection(updated && events.size() == 2 && capture_error.isEmpty(),
                     name, "update emits one canonical edit");
    if (events.size() != 2) return;
    const bool replayed_update = HydraulicDraftEventAdapter::prepareWithMembership(
        events.at(1), staged, records, membership, &error);
    expectCollection(replayed_update && records.size() == 1 && !records.first().deleted &&
                     records.first().kind == kind && membership.isEmpty() &&
                     aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                     aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()),
                     name, "update preserves membership and matches canonical live network");
    const int count_before_noop = events.size();
    const bool noop = (data.*update)(changed);
    expectCollection(noop && events.size() == count_before_noop,
                     name, "no-op update emits nothing");
    Entity wrong_identity = changed;
    wrong_identity.uuid = createUuidV7();
    expectCollection(!(data.*update)(wrong_identity) && events.size() == count_before_noop,
                     name, "update rejects an unknown UUID without an event");
    const bool removed = (data.*remove)(created);
    expectCollection(removed && events.size() == 3 && capture_error.isEmpty(),
                     name, "delete emits one canonical event");
    if (events.size() != 3) return;
    const bool replayed_delete = HydraulicDraftEventAdapter::prepareWithMembership(
        events.at(2), staged, records, membership, &error);
    expectCollection(replayed_delete && records.size() == 1 && records.first().deleted &&
                     records.first().kind == kind && membership.size() == 1 &&
                     !membership.first().inserted && membership.first().collection_index == ordinal &&
                     aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                     aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()) &&
                     aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                     aowis::model::ModelCodec<NetworkHydraulic>::encode(base),
                     name, "delete removes membership and reconstructs the original network");
    expectCollection(!(data.*remove)(created) && events.size() == 3,
                     name, "second delete is rejected without event");
}

void testHydraulicLibraryCrud()
{
    testLibraryCrud<HydraulicPipeMaterial>("HydraulicPipeMaterial", 8, 0,
        &NetworkHydraulic::pipe_materials, &HydraulicData::addPipeMaterial,
        &HydraulicData::updatePipeMaterial, &HydraulicData::removePipeMaterial);
    testLibraryCrud<HydraulicPatternTime>("HydraulicPatternTime", 9, 1,
        &NetworkHydraulic::patterns_time, &HydraulicData::addPatternTime,
        &HydraulicData::updatePatternTime, &HydraulicData::removePatternTime);
    testLibraryCrud<HydraulicCurveTankVolume>("HydraulicCurveTankVolume", 10, 2,
        &NetworkHydraulic::curves_tank_volume, &HydraulicData::addCurveTankVolume,
        &HydraulicData::updateCurveTankVolume, &HydraulicData::removeCurveTankVolume);
    testLibraryCrud<HydraulicCurvePumpHead>("HydraulicCurvePumpHead", 11, 3,
        &NetworkHydraulic::curves_pump_head, &HydraulicData::addCurvePumpHead,
        &HydraulicData::updateCurvePumpHead, &HydraulicData::removeCurvePumpHead);
    testLibraryCrud<HydraulicCurvePumpEfficiency>("HydraulicCurvePumpEfficiency", 12, 4,
        &NetworkHydraulic::curves_pump_efficiency, &HydraulicData::addCurvePumpEfficiency,
        &HydraulicData::updateCurvePumpEfficiency, &HydraulicData::removeCurvePumpEfficiency);
    testLibraryCrud<HydraulicCurveValveHeadloss>("HydraulicCurveValveHeadloss", 13, 5,
        &NetworkHydraulic::curves_valve_headloss, &HydraulicData::addCurveValveHeadloss,
        &HydraulicData::updateCurveValveHeadloss, &HydraulicData::removeCurveValveHeadloss);
    testLibraryCrud<HydraulicCurveValveCharacteristic>("HydraulicCurveValveCharacteristic", 14, 6,
        &NetworkHydraulic::curves_valve_characteristic, &HydraulicData::addCurveValveCharacteristic,
        &HydraulicData::updateCurveValveCharacteristic, &HydraulicData::removeCurveValveCharacteristic);
    testLibraryCrud<HydraulicCurveGeneric>("HydraulicCurveGeneric", 15, 7,
        &NetworkHydraulic::curves_generic, &HydraulicData::addCurveGeneric,
        &HydraulicData::updateCurveGeneric, &HydraulicData::removeCurveGeneric);
}

// A deletion must be refused if the library UUID is still used anywhere in
// the NetworkHydraulic model (including the network-root options).
void testHydraulicLibraryDeleteReferences()
{
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("HC025 references");
    HydraulicPipeMaterial material;
    material.uuid = createUuidV7();
    base.pipe_materials.append(material);
    HydraulicPatternTime pattern;
    pattern.uuid = createUuidV7();
    base.patterns_time.append(pattern);
    HydraulicCurveTankVolume tank_curve;
    tank_curve.uuid = createUuidV7();
    base.curves_tank_volume.append(tank_curve);
    HydraulicCurvePumpHead head_curve;
    head_curve.uuid = createUuidV7();
    base.curves_pump_head.append(head_curve);
    HydraulicCurvePumpEfficiency efficiency;
    efficiency.uuid = createUuidV7();
    base.curves_pump_efficiency.append(efficiency);
    HydraulicCurveValveHeadloss head_loss;
    head_loss.uuid = createUuidV7();
    base.curves_valve_headloss.append(head_loss);
    HydraulicCurveValveCharacteristic characteristic;
    characteristic.uuid = createUuidV7();
    base.curves_valve_characteristic.append(characteristic);
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.material_uuid = material.uuid;
    base.links_pipes.append(pipe);
    HydraulicNodeTank tank;
    tank.uuid = createUuidV7();
    tank.volume_curve_uuid = tank_curve.uuid;
    base.nodes_tanks.append(tank);
    HydraulicLinkPump pump;
    pump.uuid = createUuidV7();
    pump.head_curve_uuid = head_curve.uuid;
    pump.efficiency_curve_uuid = efficiency.uuid;
    pump.speed_pattern_uuid = pattern.uuid;
    base.links_pumps.append(pump);
    HydraulicLinkValve valve;
    valve.uuid = createUuidV7();
    valve.head_loss_curve_uuid = head_loss.uuid;
    valve.characteristic_curve_uuid = characteristic.uuid;
    base.links_valves.append(valve);
    HydraulicData data;
    data.replaceNetworkHydraulic(base, {});
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    data.setLogicalEditCaptureEnabled(true);
    const QByteArray unchanged = aowis::model::ModelCodec<NetworkHydraulic>::encode(base);
    const bool refused =
        !data.removePipeMaterial(material.uuid) &&
        !data.removePatternTime(pattern.uuid) &&
        !data.removeCurveTankVolume(tank_curve.uuid) &&
        !data.removeCurvePumpHead(head_curve.uuid) &&
        !data.removeCurvePumpEfficiency(efficiency.uuid) &&
        !data.removeCurveValveHeadloss(head_loss.uuid) &&
        !data.removeCurveValveCharacteristic(characteristic.uuid);
    test_harness.expectTrue(refused && events.isEmpty() &&
        aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()) == unchanged,
        "HC02.5 refuses all seven referenced library deletions without dirtying model");
    // Network-root references cannot yet be saved by HC-02, but they must
    // still prevent deletion of a pattern in HC-02.5.
    base.links_pumps.clear();
    base.options_hydraulic.default_demand_pattern_uuid = pattern.uuid;
    data.replaceNetworkHydraulic(base, {});
    data.setLogicalEditCaptureEnabled(true);
    test_harness.expectTrue(!data.removePatternTime(pattern.uuid) && events.isEmpty(),
                            "HC02.5 network-root default-demand pattern blocks deletion");
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

void testPumpSimpleControlCaptureAndCascades()
{
    NetworkHydraulic initial;
    initial.uuid = createUuidV7();
    initial.id = QStringLiteral("HC02 controls");
    HydraulicData data;
    data.replaceNetworkHydraulic(std::move(initial), {});
    const QUuid trigger = data.addJunction(coordinate(50.0, 8.0));
    const QUuid other = data.addJunction(coordinate(50.1, 8.1));
    const QUuid pump = data.addPump(trigger, other, coordinate(50.05, 8.05));
    test_harness.expectTrue(!trigger.isNull() && !other.isNull() && !pump.isNull(),
                            "HC02 creates a valid pump and trigger node");
    if (pump.isNull())
        return;
    NetworkHydraulic staged = data.networkHydraulic();
    QList<QByteArray> events;
    QString capture_error;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    QObject::connect(&data, &HydraulicData::signalLogicalEditCaptureFailed,
                     &data, [&capture_error](const QString &error) { capture_error = error; });
    data.setLogicalEditCaptureEnabled(true);

    const QUuid control_uuid = data.addPumpSimpleControl(
        pump, HydraulicControlSimpleType::LowLevel, trigger);
    test_harness.expectTrue(!control_uuid.isNull() && events.size() == 1,
                            "HC02 adding a pump control records one transaction");
    if (control_uuid.isNull() || events.size() != 1)
        return;
    const QByteArray control_id_bytes = control_uuid.toRfc4122();
    test_harness.expectTrue(control_id_bytes.size() == 16 &&
                            ((static_cast<unsigned char>(control_id_bytes.at(6)) >> 4) == 7),
                            "HC02 new pump controls use UUIDv7");
    const std::optional<aowis::model::ModelEditTransaction> added = decoded(events.last());
    test_harness.expectTrue(added.has_value() && added->edits.size() == 1 &&
                            added->edits.first().entityId == control_uuid &&
                            added->edits.first().entityType == QStringLiteral("HydraulicControlSimple") &&
                            !added->edits.first().before.has_value() &&
                            added->edits.first().after.has_value(),
                            "HC02 control creation captures the control, not its pump");

    HydraulicControlSimple changed = data.networkHydraulic().controls_simple.first();
    changed.id = QStringLiteral("C-modified");
    test_harness.expectTrue(data.setPumpSimpleControl(pump, changed) && events.size() == 2,
                            "HC02 control modification records a transaction");
    if (events.size() != 2) return;
    const std::optional<aowis::model::ModelEditTransaction> changed_tx = decoded(events.last());
    test_harness.expectTrue(changed_tx.has_value() && changed_tx->edits.size() == 1 &&
                            changed_tx->edits.first().entityId == control_uuid &&
                            changed_tx->edits.first().before.has_value() &&
                            changed_tx->edits.first().after.has_value(),
                            "HC02 control update records typed before/after images");
    test_harness.expectTrue(data.setPumpSimpleControl(pump, changed) && events.size() == 2,
                            "HC02 unchanged control does not create a spurious event");
    test_harness.expectTrue(data.removePumpSimpleControl(pump, control_uuid) && events.size() == 3,
                            "HC02 deleting control records one event");
    if (events.size() != 3) return;
    const std::optional<aowis::model::ModelEditTransaction> removed = decoded(events.last());
    test_harness.expectTrue(removed.has_value() && removed->edits.size() == 1 &&
                            removed->edits.first().entityId == control_uuid &&
                            removed->edits.first().before.has_value() &&
                            !removed->edits.first().after.has_value(),
                            "HC02 control deletion is reversible");

    const QUuid next_control = data.addPumpSimpleControl(
        pump, HydraulicControlSimpleType::HighLevel, trigger);
    const int before_pump_delete = events.size();
    test_harness.expectTrue(!next_control.isNull() && data.deletePump(pump) &&
                            events.size() == before_pump_delete + 1 &&
                            data.networkHydraulic().controls_simple.isEmpty(),
                            "HC02 pump deletion cascades to its controls in one transaction");
    if (events.size() != before_pump_delete + 1) return;
    const std::optional<aowis::model::ModelEditTransaction> deleted_pump = decoded(events.last());
    bool pump_removed = false;
    bool control_removed = false;
    if (deleted_pump.has_value())
    {
        for (const aowis::model::ModelEntityEdit &edit : deleted_pump->edits)
        {
            if (edit.entityId == pump)
                pump_removed = edit.before.has_value() && !edit.after.has_value();
            if (edit.entityId == next_control)
                control_removed = edit.before.has_value() && !edit.after.has_value();
        }
    }
    test_harness.expectTrue(deleted_pump.has_value() && deleted_pump->edits.size() == 2 &&
                            pump_removed && control_removed,
                            "HC02 pump and dependent control share one logical transaction");

    const QUuid second_pump = data.addPump(trigger, other, coordinate(50.05, 8.05));
    const QUuid triggered_control = data.addPumpSimpleControl(
        second_pump, HydraulicControlSimpleType::LowLevel, trigger);
    const int before_node_delete = events.size();
    test_harness.expectTrue(!second_pump.isNull() && !triggered_control.isNull() &&
                            data.deleteJunction(trigger) &&
                            events.size() == before_node_delete + 1 &&
                            data.networkHydraulic().controls_simple.isEmpty() &&
                            data.networkHydraulic().links_pumps.isEmpty(),
                            "HC02 node deletion cascades pump and control together");
    if (events.size() != before_node_delete + 1) return;
    const std::optional<aowis::model::ModelEditTransaction> deleted_node = decoded(events.last());
    test_harness.expectTrue(deleted_node.has_value() && deleted_node->edits.size() == 3,
                            "HC02 node cascade contains node, pump and simple control");

    // Replay every captured operation against a separate canonical stage,
    // including membership inserts/removals and the dependent control cascades.
    bool replay_successful = true;
    QString replay_error;
    for (const QByteArray &bytes : events)
    {
        QList<PersonalWorkingCopyChange> records;
        QList<HydraulicDraftMembershipEdit> membership;
        if (!HydraulicDraftEventAdapter::prepareWithMembership(
                bytes, staged, records, membership, &replay_error))
        {
            replay_successful = false;
            break;
        }
    }
    const QByteArray replay_message =
        (QStringLiteral("HC02 replay of control edits succeeds: ") + replay_error).toUtf8();
    test_harness.expectTrue(replay_successful, replay_message.constData());
    if (replay_successful)
        test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                                aowis::model::ModelCodec<NetworkHydraulic>::encode(
                                    data.networkHydraulic()),
                                "HC02 control, pump and node cascades replay to exact MODEL bytes");
    test_harness.expectTrue(capture_error.isEmpty() && data.logicalEditCaptureEnabled(),
                            "HC02 controls never silently disable capture");
}

// A rule's premises and THEN/ELSE actions can reference different hydraulic
// entities. Deleting a referenced link or a node with connected links must
// remove the *entire* dependent rule in the same persisted MODEL event.
void testRuleControlDeletionCascades()
{
    for (int scenario = 0; scenario < 6; ++scenario)
    {
        NetworkHydraulic baseline;
        baseline.uuid = createUuidV7();
        baseline.id = QStringLiteral("HC02-rule-cascade");
        HydraulicNetworkEditor seed(baseline);
        const QUuid first = scenario == 4 ? seed.addReservoir(coordinate(50.0, 8.0))
            : scenario == 5 ? seed.addTank(coordinate(50.0, 8.0))
            : seed.addJunction(coordinate(50.0, 8.0));
        const QUuid second = seed.addJunction(coordinate(50.1, 8.1));
        const QUuid stable_a = seed.addJunction(coordinate(50.2, 8.2));
        const QUuid stable_b = seed.addJunction(coordinate(50.3, 8.3));
        const QUuid stable_link = seed.addPipe(stable_a, stable_b, {});
        const QUuid target_link = scenario == 1
            ? seed.addPump(first, second, coordinate(50.05, 8.05))
            : scenario == 2
                ? seed.addValve(first, second, coordinate(50.05, 8.05))
                : seed.addPipe(first, second, {});
        test_harness.expectTrue(!first.isNull() && !target_link.isNull() &&
                                !stable_link.isNull(),
                                "HC02 rule cascade fixture has valid nodes and links");
        if (target_link.isNull() || stable_link.isNull()) continue;

        HydraulicControlRulePremise time_premise;
        time_premise.object = HydraulicControlRuleObject::System;
        time_premise.variable = HydraulicControlRuleVariable::Time;
        time_premise.elapsed_time_s = 3600;
        HydraulicControlRulePremise link_premise;
        link_premise.object = HydraulicControlRuleObject::Link;
        link_premise.object_uuid = target_link;
        link_premise.variable = HydraulicControlRuleVariable::Flow;
        link_premise.flow_m3_per_h = 1.0;
        HydraulicControlRulePremise node_premise;
        node_premise.object = HydraulicControlRuleObject::Node;
        node_premise.object_uuid = first;
        node_premise.variable = HydraulicControlRuleVariable::Pressure;
        node_premise.pressure_head_m = 1.0;
        HydraulicControlRulePremise stable_premise = node_premise;
        stable_premise.object_uuid = stable_a;

        HydraulicControlRuleAction target_action;
        target_action.link_uuid = target_link;
        target_action.status = HydraulicControlRuleStatus::Open;
        HydraulicControlRuleAction stable_action;
        stable_action.link_uuid = stable_link;
        stable_action.status = HydraulicControlRuleStatus::Open;

        HydraulicControlRule then_rule;
        then_rule.id = QStringLiteral("then");
        then_rule.uuid = createUuidV7();
        then_rule.premises.append(time_premise);
        then_rule.actions_then.append(target_action);
        HydraulicControlRule else_rule;
        else_rule.id = QStringLiteral("else");
        else_rule.uuid = createUuidV7();
        else_rule.premises.append(time_premise);
        else_rule.actions_then.append(stable_action);
        else_rule.actions_else.append(target_action);
        HydraulicControlRule link_rule;
        link_rule.id = QStringLiteral("link-premise");
        link_rule.uuid = createUuidV7();
        link_rule.premises.append(link_premise);
        link_rule.actions_then.append(stable_action);
        HydraulicControlRule node_rule;
        node_rule.id = QStringLiteral("node-premise");
        node_rule.uuid = createUuidV7();
        node_rule.premises.append(node_premise);
        node_rule.actions_then.append(stable_action);
        HydraulicControlRule unrelated_rule;
        unrelated_rule.id = QStringLiteral("unrelated");
        unrelated_rule.uuid = createUuidV7();
        unrelated_rule.premises.append(stable_premise);
        unrelated_rule.actions_then.append(stable_action);
        baseline.controls_rules = {then_rule, else_rule, link_rule, node_rule, unrelated_rule};
        HydraulicMapLabel target_label;
        target_label.id = QStringLiteral("label-of-target-node");
        target_label.uuid = createUuidV7();
        target_label.text = QStringLiteral("Anchor survives detachment");
        target_label.coordinate_wgs84 = coordinate(50.0, 8.0);
        target_label.anchor_node_uuid = first;
        HydraulicMapLabel stable_label;
        stable_label.id = QStringLiteral("independent-label");
        stable_label.uuid = createUuidV7();
        stable_label.text = QStringLiteral("Unchanged");
        stable_label.coordinate_wgs84 = coordinate(50.2, 8.2);
        stable_label.anchor_node_uuid = stable_a;
        baseline.map_labels = {target_label, stable_label};

        HydraulicData data;
        data.replaceNetworkHydraulic(baseline, {});
        NetworkHydraulic staged = baseline;
        QList<QByteArray> events;
        QString capture_error;
        QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                         &data, [&events](const QByteArray &bytes) { events.append(bytes); });
        QObject::connect(&data, &HydraulicData::signalLogicalEditCaptureFailed,
                         &data, [&capture_error](const QString &message) { capture_error = message; });
        data.setLogicalEditCaptureEnabled(true);
        const bool is_node = scenario >= 3;
        const bool deleted = scenario == 0 ? data.deletePipe(target_link)
            : scenario == 1 ? data.deletePump(target_link)
            : scenario == 2 ? data.deleteValve(target_link)
            : scenario == 3 ? data.deleteJunction(first)
            : scenario == 4 ? data.deleteReservoir(first)
            : data.deleteTank(first);
        const int expected_rule_count = is_node ? 1 : 2;
        test_harness.expectTrue(deleted && events.size() == 1 &&
                                data.networkHydraulic().controls_rules.size() == expected_rule_count,
                                "HC02 rule cascade deletion emits exactly one transaction");
        if (!deleted || events.size() != 1) continue;
        bool unrelated_kept = false;
        bool node_rule_kept = false;
        for (const HydraulicControlRule &rule : data.networkHydraulic().controls_rules)
        {
            unrelated_kept = unrelated_kept || rule.uuid == unrelated_rule.uuid;
            node_rule_kept = node_rule_kept || rule.uuid == node_rule.uuid;
        }
        test_harness.expectTrue(unrelated_kept && node_rule_kept == !is_node,
                                "HC02 rule cascade retains only independent rules");
        bool target_label_preserved = false;
        bool stable_label_preserved = false;
        for (const HydraulicMapLabel &label : data.networkHydraulic().map_labels)
        {
            if (label.uuid == target_label.uuid)
                target_label_preserved = label.text == target_label.text &&
                    label.coordinate_wgs84.latitude_deg == target_label.coordinate_wgs84.latitude_deg &&
                    label.anchor_node_uuid == (is_node ? QUuid() : first);
            if (label.uuid == stable_label.uuid)
                stable_label_preserved = label.anchor_node_uuid == stable_a &&
                    label.text == stable_label.text;
        }
        test_harness.expectTrue(target_label_preserved && stable_label_preserved &&
                                data.networkHydraulic().map_labels.size() == 2,
                                "HC02 node deletion detaches only dependent map label anchors");

        const std::optional<aowis::model::ModelEditTransaction> transaction = decoded(events.first());
        bool removed_then = false;
        bool removed_else = false;
        bool removed_premise = false;
        bool removed_node_premise = false;
        bool updated_label = false;
        if (transaction.has_value())
        {
            for (const aowis::model::ModelEntityEdit &edit : transaction->edits)
            {
                if (edit.entityId == then_rule.uuid)
                    removed_then = edit.before.has_value() && !edit.after.has_value();
                if (edit.entityId == else_rule.uuid)
                    removed_else = edit.before.has_value() && !edit.after.has_value();
                if (edit.entityId == link_rule.uuid)
                    removed_premise = edit.before.has_value() && !edit.after.has_value();
                if (edit.entityId == node_rule.uuid)
                    removed_node_premise = edit.before.has_value() && !edit.after.has_value();
                if (edit.entityId == target_label.uuid)
                    updated_label = edit.before.has_value() && edit.after.has_value() &&
                        edit.before != edit.after;
            }
        }
        test_harness.expectTrue(transaction.has_value() &&
                                transaction->edits.size() == (is_node ? 7 : 4) &&
                                removed_then && removed_else && removed_premise &&
                                removed_node_premise == is_node && updated_label == is_node,
                                "HC02 rule dependencies and their owner are removed atomically");
        QList<PersonalWorkingCopyChange> records;
        QList<HydraulicDraftMembershipEdit> membership;
        QString error;
        const bool replayed = HydraulicDraftEventAdapter::prepareWithMembership(
            events.first(), staged, records, membership, &error);
        const QByteArray message = (QStringLiteral("HC02 rule cascade replays: ") + error).toUtf8();
        test_harness.expectTrue(replayed, message.constData());
        if (replayed)
            test_harness.expectTrue(
                aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()) &&
                membership.size() == (is_node ? 6 : 4) &&
                records.size() == (is_node ? 7 : 4),
                "HC02 rule and map-label cascade replays with exact membership and canonical images");
        test_harness.expectTrue(capture_error.isEmpty() && data.logicalEditCaptureEnabled(),
                                "HC02 dependent rule removal does not disable edit capture");
    }
}

// Rejoining a split pipe removes the second link UUID. The GUI must watch
// dependent controls before the mutation; otherwise the model can change
// without matching DVMDB draft events and/or retain dangling references.
void testPipeRejoinCapturesControlCascades()
{
    NetworkHydraulic baseline;
    baseline.uuid = createUuidV7();
    baseline.id = QStringLiteral("HC02 pipe rejoin dependencies");
    HydraulicNetworkEditor seed(baseline);
    const QUuid first_node = seed.addJunction(coordinate(50.00, 8.00));
    const QUuid joining_node = seed.addJunction(coordinate(50.01, 8.01));
    const QUuid last_node = seed.addJunction(coordinate(50.02, 8.02));
    QList<CoordinateWGS84> vertices;
    vertices.append(coordinate(50.01, 8.01));
    const QUuid first_pipe = seed.addPipe(first_node, last_node, vertices);
    const QUuid second_pipe = seed.splitPipeAtVertex(first_pipe, 0, joining_node);
    const QUuid demand = seed.addDemandPoint(coordinate(50.015, 8.015));
    const bool attached = seed.attachDemandPointToPipe(demand, second_pipe, 0.5);
    test_harness.expectTrue(!first_pipe.isNull() && !second_pipe.isNull() &&
                            !demand.isNull() && attached,
                            "HC02 pipe rejoin fixture includes two pipes and an attached demand point");
    if (first_pipe.isNull() || second_pipe.isNull() || !attached)
        return;

    HydraulicControlSimple target_control;
    target_control.uuid = createUuidV7();
    target_control.id = QStringLiteral("dependent-control");
    target_control.link_uuid = second_pipe;
    target_control.trigger_node_uuid = first_node;
    baseline.controls_simple.append(target_control);

    HydraulicControlRulePremise premise;
    premise.object = HydraulicControlRuleObject::System;
    premise.variable = HydraulicControlRuleVariable::Time;
    premise.elapsed_time_s = 3600;
    HydraulicControlRulePremise link_premise;
    link_premise.object = HydraulicControlRuleObject::Link;
    link_premise.object_uuid = second_pipe;
    link_premise.variable = HydraulicControlRuleVariable::Flow;
    link_premise.flow_m3_per_h = 1.0;
    HydraulicControlRuleAction affected_action;
    affected_action.link_uuid = second_pipe;
    affected_action.status = HydraulicControlRuleStatus::Open;
    HydraulicControlRuleAction independent_action;
    independent_action.link_uuid = first_pipe;
    independent_action.status = HydraulicControlRuleStatus::Open;

    HydraulicControlRule then_rule;
    then_rule.uuid = createUuidV7();
    then_rule.id = QStringLiteral("dependent-then");
    then_rule.premises.append(premise);
    then_rule.actions_then.append(affected_action);
    HydraulicControlRule else_rule;
    else_rule.uuid = createUuidV7();
    else_rule.id = QStringLiteral("dependent-else");
    else_rule.premises.append(premise);
    else_rule.actions_then.append(independent_action);
    else_rule.actions_else.append(affected_action);
    HydraulicControlRule premise_rule;
    premise_rule.uuid = createUuidV7();
    premise_rule.id = QStringLiteral("dependent-premise");
    premise_rule.premises.append(link_premise);
    premise_rule.actions_then.append(independent_action);
    HydraulicControlRule independent_rule;
    independent_rule.uuid = createUuidV7();
    independent_rule.id = QStringLiteral("independent-rule");
    independent_rule.premises.append(premise);
    independent_rule.actions_then.append(independent_action);
    baseline.controls_rules = {then_rule, else_rule, premise_rule, independent_rule};

    HydraulicData data;
    data.replaceNetworkHydraulic(baseline, {});
    NetworkHydraulic staged = baseline;
    QList<QByteArray> events;
    QString capture_error;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    QObject::connect(&data, &HydraulicData::signalLogicalEditCaptureFailed,
                     &data, [&capture_error](const QString &error) { capture_error = error; });
    data.setLogicalEditCaptureEnabled(true);
    test_harness.expectTrue(!data.undoPipeSplit(first_pipe, createUuidV7(), joining_node) &&
                            events.isEmpty(),
                            "HC02 rejected pipe rejoin does not produce a logical transaction");
    const bool joined = data.undoPipeSplit(first_pipe, second_pipe, joining_node);
    test_harness.expectTrue(joined && events.size() == 1 &&
                            data.networkHydraulic().controls_simple.isEmpty() &&
                            data.networkHydraulic().controls_rules.size() == 1 &&
                            data.networkHydraulic().controls_rules.first().uuid == independent_rule.uuid &&
                            data.networkHydraulic().links_pipes.size() == 1 &&
                            data.networkHydraulic().demand_points.first().attachment.pipe_uuid == first_pipe,
                            "HC02 rejoin removes dependent controls and preserves independent rules and demand");
    if (!joined || events.size() != 1)
        return;
    const std::optional<aowis::model::ModelEditTransaction> tx = decoded(events.first());
    bool second_removed = false;
    bool simple_removed = false;
    bool then_removed = false;
    bool else_removed = false;
    bool premise_removed = false;
    bool first_updated = false;
    bool demand_updated = false;
    if (tx.has_value())
    {
        for (const aowis::model::ModelEntityEdit &edit : tx->edits)
        {
            const bool removed = edit.before.has_value() && !edit.after.has_value();
            const bool updated = edit.before.has_value() && edit.after.has_value() &&
                                 edit.before != edit.after;
            if (edit.entityId == second_pipe) second_removed = removed;
            if (edit.entityId == target_control.uuid) simple_removed = removed;
            if (edit.entityId == then_rule.uuid) then_removed = removed;
            if (edit.entityId == else_rule.uuid) else_removed = removed;
            if (edit.entityId == premise_rule.uuid) premise_removed = removed;
            if (edit.entityId == first_pipe) first_updated = updated;
            if (edit.entityId == demand) demand_updated = updated;
        }
    }
    test_harness.expectTrue(tx.has_value() && tx->edits.size() == 7 &&
                            second_removed && simple_removed && then_removed &&
                            else_removed && premise_removed && first_updated && demand_updated,
                            "HC02 rejoin captures five removals and two before/after updates atomically");
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    QString error;
    const bool replayed = HydraulicDraftEventAdapter::prepareWithMembership(
        events.first(), staged, records, membership, &error);
    const QByteArray replay_message =
        (QStringLiteral("HC02 pipe rejoin replay: ") + error).toUtf8();
    test_harness.expectTrue(replayed, replay_message.constData());
    if (replayed)
    {
        int deletions = 0;
        for (const HydraulicDraftMembershipEdit &entry : membership)
        {
            if (!entry.inserted) ++deletions;
        }
        test_harness.expectTrue(records.size() == 7 && membership.size() == 5 &&
                                deletions == 5 &&
                                aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
                                aowis::model::ModelCodec<NetworkHydraulic>::encode(
                                    data.networkHydraulic()),
                                "HC02 rejoin reconstructs exact canonical network and membership removals");
    }
    test_harness.expectTrue(capture_error.isEmpty() && data.logicalEditCaptureEnabled(),
                            "HC02 pipe rejoin retains an active, error-free edit capture");
}

void testRuleAndMapLabelCrud()
{
    NetworkHydraulic baseline;
    baseline.uuid = createUuidV7();
    baseline.id = QStringLiteral("HC02 rule/map-label editing");
    HydraulicNodeJunction node_from;
    node_from.uuid = createUuidV7();
    node_from.id = QStringLiteral("J1");
    node_from.coordinate_wgs84 = coordinate(50.0, 8.0);
    HydraulicNodeJunction node_to;
    node_to.uuid = createUuidV7();
    node_to.id = QStringLiteral("J2");
    node_to.coordinate_wgs84 = coordinate(50.1, 8.1);
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.id = QStringLiteral("P1");
    pipe.node_uuid_from = node_from.uuid;
    pipe.node_uuid_to = node_to.uuid;
    baseline.nodes_junctions = {node_from, node_to};
    baseline.links_pipes = {pipe};

    HydraulicData data;
    data.replaceNetworkHydraulic(baseline, {});
    NetworkHydraulic staged = baseline;
    QList<QByteArray> events;
    QString capture_error;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    QObject::connect(&data, &HydraulicData::signalLogicalEditCaptureFailed,
                     &data, [&capture_error](const QString &error) { capture_error = error; });
    data.setLogicalEditCaptureEnabled(true);

    HydraulicControlRulePremise premise;
    premise.object = HydraulicControlRuleObject::Node;
    premise.object_uuid = node_from.uuid;
    premise.variable = HydraulicControlRuleVariable::Pressure;
    premise.comparison = HydraulicControlRuleOperator::Below;
    premise.pressure_head_m = 12.5;
    HydraulicControlRuleAction action;
    action.link_uuid = pipe.uuid;
    action.status = HydraulicControlRuleStatus::Open;
    HydraulicControlRule rule;
    rule.id = QStringLiteral("R1");
    rule.premises.append(premise);
    rule.actions_then.append(action);
    HydraulicMapLabel label;
    label.id = QStringLiteral("L1");
    label.text = QStringLiteral("Label one");
    label.coordinate_wgs84 = coordinate(50.01, 8.02);
    label.anchor_node_uuid = node_from.uuid;

    HydraulicControlRule invalid_rule = rule;
    invalid_rule.premises[0].object_uuid = pipe.uuid;
    test_harness.expectTrue(data.addRuleControl(invalid_rule).isNull() && events.isEmpty(),
                            "HC02 rules reject a link UUID used as a node premise");
    invalid_rule = rule;
    invalid_rule.actions_then[0].link_uuid = node_from.uuid;
    test_harness.expectTrue(data.addRuleControl(invalid_rule).isNull() && events.isEmpty(),
                            "HC02 rules reject node UUID used as link action");
    invalid_rule = rule;
    invalid_rule.actions_then[0].link_uuid = createUuidV7();
    test_harness.expectTrue(data.addRuleControl(invalid_rule).isNull() && events.isEmpty(),
                            "HC02 rules reject missing action links");
    invalid_rule = rule;
    invalid_rule.premises.clear();
    test_harness.expectTrue(data.addRuleControl(invalid_rule).isNull() && events.isEmpty(),
                            "HC02 rules reject missing premises");
    invalid_rule = rule;
    invalid_rule.actions_then.clear();
    test_harness.expectTrue(data.addRuleControl(invalid_rule).isNull() && events.isEmpty(),
                            "HC02 rules reject missing THEN actions");
    HydraulicMapLabel invalid_label = label;
    invalid_label.anchor_node_uuid = pipe.uuid;
    test_harness.expectTrue(data.addMapLabel(invalid_label).isNull() && events.isEmpty(),
                            "HC02 labels reject a link UUID as a node anchor");
    invalid_label = label;
    invalid_label.coordinate_wgs84.latitude_deg = 1000.0;
    test_harness.expectTrue(data.addMapLabel(invalid_label).isNull() && events.isEmpty(),
                            "HC02 labels reject coordinates outside WGS84 bounds");

    const QUuid rule_uuid = data.addRuleControl(rule);
    const QUuid label_uuid = data.addMapLabel(label);
    test_harness.expectTrue(!rule_uuid.isNull() && !label_uuid.isNull() &&
                            (rule_uuid.toRfc4122().at(6) & 0xf0) == 0x70 &&
                            (label_uuid.toRfc4122().at(6) & 0xf0) == 0x70 &&
                            rule_uuid != label_uuid && events.size() == 2 &&
                            data.networkHydraulic().controls_rules.size() == 1 &&
                            data.networkHydraulic().map_labels.size() == 1,
                            "HC02 creates rule and label with captured, distinct UUIDv7 identities");
    if (rule_uuid.isNull() || label_uuid.isNull() || events.size() != 2)
        return;
    rule.uuid = rule_uuid;
    label.uuid = label_uuid;
    test_harness.expectTrue(data.updateRuleControl(rule) && data.updateMapLabel(label) &&
                            events.size() == 2,
                            "HC02 unchanged rule and label produce no events");
    rule.priority = 2.5;
    rule.actions_else.append(action);
    label.text = QStringLiteral("Edited label");
    label.anchor_node_uuid = QUuid();
    label.coordinate_wgs84.longitude_deg = 8.25;
    test_harness.expectTrue(data.updateRuleControl(rule) && data.updateMapLabel(label) &&
                            events.size() == 4,
                            "HC02 updates rule branches and label text, anchor, coordinates");
    invalid_rule = rule;
    invalid_rule.premises[0].object_uuid = createUuidV7();
    invalid_label = label;
    invalid_label.anchor_node_uuid = createUuidV7();
    test_harness.expectTrue(!data.updateRuleControl(invalid_rule) &&
                            !data.updateMapLabel(invalid_label) && events.size() == 4,
                            "HC02 rejected updates do not produce events");
    test_harness.expectTrue(!data.removeRuleControl(node_from.uuid) &&
                            !data.removeMapLabel(pipe.uuid) && events.size() == 4,
                            "HC02 refuse deleting entities of other collections");
    test_harness.expectTrue(data.removeRuleControl(rule_uuid) &&
                            data.removeMapLabel(label_uuid) && events.size() == 6 &&
                            data.networkHydraulic().controls_rules.isEmpty() &&
                            data.networkHydraulic().map_labels.isEmpty(),
                            "HC02 deletes rule and label with two durable events");
    if (events.size() != 6)
        return;

    const quint16 expected_kinds[] = {17, 18, 17, 18, 17, 18};
    const bool expected_inserted[] = {true, true, false, false, false, false};
    for (int index = 0; index < events.size(); ++index)
    {
        QString error;
        QList<PersonalWorkingCopyChange> records;
        QList<HydraulicDraftMembershipEdit> membership;
        const bool replayed = HydraulicDraftEventAdapter::prepareWithMembership(
            events.at(index), staged, records, membership, &error);
        const std::optional<aowis::model::ModelEditTransaction> tx = decoded(events.at(index));
        const bool correct_images = tx.has_value() && tx->edits.size() == 1 &&
            tx->edits.first().entityId == ((index % 2 == 0) ? rule_uuid : label_uuid) &&
            tx->edits.first().before.has_value() == (index >= 2) &&
            tx->edits.first().after.has_value() == (index < 4);
        const bool membership_expected = (index < 2 || index >= 4);
        const bool correct_membership = membership_expected ?
            (membership.size() == 1 && membership.first().kind == expected_kinds[index] &&
             membership.first().inserted == expected_inserted[index]) : membership.isEmpty();
        const QByteArray message = QStringLiteral("HC02 rule/map replay event %1: %2")
            .arg(index).arg(error).toUtf8();
        test_harness.expectTrue(replayed && correct_images && correct_membership &&
                                records.size() == 1, message.constData());
        if (!replayed) return;
    }
    test_harness.expectTrue(
        aowis::model::ModelCodec<NetworkHydraulic>::encode(staged) ==
            aowis::model::ModelCodec<NetworkHydraulic>::encode(data.networkHydraulic()) &&
        capture_error.isEmpty() && data.logicalEditCaptureEnabled(),
        "HC02 rule/map-label create/update/delete replays to exact canonical network");
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

    // A candidate in which the junction exists with *different* content is
    // neither the event's before state nor its result: keep the event.
    NetworkHydraulic diverged = live;
    HydraulicNetworkEditor diverged_editor(diverged);
    test_harness.expectTrue(diverged_editor.setNodeId(junction, QStringLiteral("J-elsewhere")),
                            "prepare a diverged candidate");
    PersonalDraftEditStage reopened;
    test_harness.expectTrue(!reopened.open(root.path(), repository, draft, diverged, {}, &error) &&
                            error.contains(QStringLiteral("replay"), Qt::CaseInsensitive),
                            "stale server journal cannot silently replay event against an already changed model");
    PersonalEditQueue raw;
    test_harness.expectTrue(raw.open(root.path(), repository, draft, &error) &&
                            raw.pending().size() == 1,
                            "rejected replay never removes the locally durable transaction");
    raw.close();

    // A candidate that already *is* the event's result (the Save reached
    // DVMDB, its acknowledgement was lost and a Commit then cleared the
    // journal): the oldest event is retired locally, not replayed twice.
    PersonalDraftEditStage recovered;
    test_harness.expectTrue(recovered.open(root.path(), repository, draft, live, {}, &error) &&
                            recovered.pendingCount() == 0,
                            "an already applied oldest event is acknowledged on reopen");
    test_harness.expectTrue(aowis::model::ModelCodec<NetworkHydraulic>::encode(recovered.visibleNetwork()) ==
                            aowis::model::ModelCodec<NetworkHydraulic>::encode(live),
                            "the recovered candidate is used unchanged");
}


void testNetworkRootMetadataCaptureAndReplay()
{
    NetworkHydraulic base;
    base.uuid = createUuidV7();
    base.id = QStringLiteral("Root-Network");
    base.title_line_1 = QStringLiteral("Original");
    const QUuid junction = createUuidV7();
    HydraulicNodeJunction node;
    node.uuid = junction;
    node.id = QStringLiteral("J1");
    base.nodes_junctions.append(node);
    HydraulicData data;
    data.replaceNetworkHydraulic(base, {});
    QList<QByteArray> events;
    QObject::connect(&data, &HydraulicData::signalLogicalEditRecorded,
                     &data, [&events](const QByteArray &bytes) { events.append(bytes); });
    data.setLogicalEditCaptureEnabled(true);
    data.setSimulationHeadlossFormula(HydraulicHeadlossFormula::DarcyWeisbach);
    test_harness.expectTrue(events.size() == 1,
        "headloss formula change emits one canonical network-root transaction");
    if (events.size() != 1) return;
    const std::optional<aowis::model::ModelEditTransaction> event = decoded(events.first());
    test_harness.expectTrue(event && event->edits.size() == 1 &&
        event->edits.first().entityId == base.uuid &&
        event->edits.first().entityType ==
            QString::fromLatin1(aowis::model::hydraulicRootEditType),
        "headloss event identifies only the network root");
    QList<PersonalWorkingCopyChange> records;
    QList<HydraulicDraftMembershipEdit> membership;
    QString error;
    NetworkHydraulic replayed = base;
    test_harness.expectTrue(HydraulicDraftEventAdapter::prepareWithMembership(
        events.first(), replayed, records, membership, &error) &&
        records.isEmpty() && membership.isEmpty() &&
        replayed.options_hydraulic.headloss_formula == HydraulicHeadlossFormula::DarcyWeisbach &&
        replayed.nodes_junctions.size() == 1,
        "replay updates metadata without creating client-owned manifest records");
    NetworkHydraulic stale = base;
    stale.title_line_1 = QStringLiteral("Other");
    test_harness.expectTrue(!HydraulicDraftEventAdapter::prepareWithMembership(
        events.first(), stale, records, membership, &error),
        "root replay rejects stale before-image");
    data.setSimulationHeadlossFormula(HydraulicHeadlossFormula::DarcyWeisbach);
    test_harness.expectTrue(events.size() == 1,
        "setting same network option does not create another logical edit");

    // Source changes must pass through the same durable offline queue; a
    // process restart may not drop or reinterpret a metadata-only event.
    QTemporaryDir directory;
    const QUuid repository = createUuidV7();
    const QUuid draft = createUuidV7();
    PersonalDraftEditStage stage;
    test_harness.expectTrue(directory.isValid() &&
        stage.open(directory.path(), repository, draft, base, {}, &error) &&
        stage.append(events.first(), &error) && stage.pendingCount() == 1,
        "root event entered a durable private queue");
    stage.close();
    PersonalDraftEditStage reopened;
    test_harness.expectTrue(reopened.open(directory.path(), repository, draft, base, {}, &error) &&
        reopened.pendingCount() == 1 &&
        reopened.visibleNetwork().options_hydraulic.headloss_formula ==
            HydraulicHeadlossFormula::DarcyWeisbach,
        "root metadata event replays across private draft restart");
    QList<PersonalWorkingCopyChange> stagedRecords;
    QList<HydraulicDraftMembershipEdit> stagedMembership;
    NetworkHydraulic stagedCandidate;
    test_harness.expectTrue(reopened.prepareOldest(stagedCandidate, stagedRecords,
        stagedMembership, &error) && stagedRecords.isEmpty() && stagedMembership.isEmpty() &&
        reopened.acknowledgeOldest(events.first(), &error) && reopened.pendingCount() == 0,
        "root event can be authoritatively acknowledged without a client manifest record");
    reopened.close();

    NetworkHydraulic metadata =
        aowis::model::hydraulicRootMetadata(data.networkHydraulic());
    metadata.title_line_2 = QStringLiteral("Updated network-wide metadata");
    metadata.duration_s = 7200;
    test_harness.expectTrue(data.setNetworkRootMetadata(metadata) && events.size() == 2 &&
        data.networkHydraulic().title_line_2 == metadata.title_line_2 &&
        data.networkHydraulic().duration_s == 7200,
        "generic metadata edit captures title and simulation duration together");
}

}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_harness.runCase("Network-root metadata capture and replay", testNetworkRootMetadataCaptureAndReplay);
    test_harness.runCase("Terrain-relative 3D node creation", testTerrainRelativeNodeCreationIsAtomic);
    test_harness.runCase("Atomic 3D node movement", testTerrainAwareNodeMovementIsAtomic);
    test_harness.runCase("Only watched entities", testCaptureOnlyTouchedEntities);
    test_harness.runCase("All 18 hydraulic collections", testAllHydraulicEntityCollectionCapture);
    test_harness.runCase("Hydraulic collection UUID ambiguity", testAmbiguousAndNullHydraulicCapture);
    test_harness.runCase("HydraulicData logical operations", testHydraulicDataLogicalOperations);
    test_harness.runCase("Pipe split keeps canonical collection order",
                         testPipeSplitKeepsCanonicalCollectionOrder);
    test_harness.runCase("Hydraulic property capture and no-op", testPropertyCaptureAndNoop);
    test_harness.runCase("Pump simple control capture and cascades", testPumpSimpleControlCaptureAndCascades);
    test_harness.runCase("Rule control reference cascades", testRuleControlDeletionCascades);
    test_harness.runCase("Pipe rejoin control cascades", testPipeRejoinCapturesControlCascades);
    test_harness.runCase("Rule and map-label CRUD", testRuleAndMapLabelCrud);
    test_harness.runCase("Hydraulic library reference guards", testHydraulicLibraryReferenceValidation);
    test_harness.runCase("Hydraulic library CRUD and draft replay", testHydraulicLibraryCrud);
    test_harness.runCase("Hydraulic library deletion dependency guards", testHydraulicLibraryDeleteReferences);
    test_harness.runCase("Cascading node deletion", testCascadingNodeDeletionIsOneTransaction);
    test_harness.runCase("Hydraulic event to canonical DVMDB records", testOrderedHydraulicDraftRecordMapping);
    test_harness.runCase("Hydraulic membership intent mapping", testHydraulicDraftMembershipIntents);
    test_harness.runCase("Capture to durable offline queue", testCapturedOperationsEnterDurableQueue);
    test_harness.runCase("Verified draft staging and recovery", testVerifiedDraftStageReplayAndAck);
    test_harness.runCase("Structural editing across draft restarts", testStructuralDraftRecoveryAcrossRestart);
    test_harness.runCase("Reject unverified draft replay", testDraftStageRejectsOutOfOrderReplay);
    test_harness.runCase("Refuse unsaveable draft event", testDraftStageRefusesUnsaveableEvent);
    return test_harness.finish();
}
