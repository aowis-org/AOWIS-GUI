#include "network/hydraulic_network_editor.h"
#include "network/network_render_snapshot_builder.h"
#include "test_harness.h"

#include <QDate>

namespace
{
AowisTestHarness test_harness;

void expectTrue(bool condition, const char *message)
{
    test_harness.expectTrue(condition, message);
}

void expectNear(double actual, double expected, double tolerance, const char *message)
{
    test_harness.expectNear(actual, expected, tolerance, message);
}

void testDemandPointPipeAllocationModel()
{
    HydraulicDemandPointAttachment attachment;
    expectTrue(
        attachment.pipe_allocation_mode
            == HydraulicDemandPointPipeAllocationMode::InterpolateByPosition,
        "new demand-point pipe attachments default to native AOWIS positional allocation");
    expectTrue(attachment.pipe_assigned_junction_uuid.isNull(),
               "new demand-point pipe attachments have no assigned junction by default");

    const QUuid pipe_uuid = QUuid::createUuid();
    const QUuid assigned_junction_uuid = QUuid::createUuid();
    attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    attachment.pipe_uuid = pipe_uuid;
    attachment.pipe_position = 0.73;
    attachment.pipe_allocation_mode = HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    attachment.pipe_assigned_junction_uuid = assigned_junction_uuid;

    const HydraulicDemandPointAttachment copied_attachment = attachment;
    expectTrue(copied_attachment.type == HydraulicDemandPointAttachmentType::Pipe,
               "pipe attachment type survives value copying");
    expectTrue(copied_attachment.pipe_uuid == pipe_uuid,
               "pipe attachment UUID survives value copying");
    expectNear(copied_attachment.pipe_position, 0.73, 1e-12,
               "pipe attachment position survives value copying");
    expectTrue(
        copied_attachment.pipe_allocation_mode
            == HydraulicDemandPointPipeAllocationMode::AssignedJunction,
        "assigned-junction allocation mode survives value copying");
    expectTrue(copied_attachment.pipe_assigned_junction_uuid == assigned_junction_uuid,
               "assigned junction UUID survives value copying");
}

void testDemandPointAttachmentRemappingOnPipeSplit()
{
    NetworkHydraulicDistribution network;
    HydraulicNetworkEditor editor(network);

    CoordinateWGS84 from_coordinate;
    from_coordinate.latitude_deg = 0.0;
    from_coordinate.longitude_deg = 0.0;
    const QUuid from_uuid = editor.addJunction(from_coordinate);

    CoordinateWGS84 to_coordinate;
    to_coordinate.latitude_deg = 0.0;
    to_coordinate.longitude_deg = 2.0;
    const QUuid to_uuid = editor.addJunction(to_coordinate);

    CoordinateWGS84 midpoint_coordinate;
    midpoint_coordinate.latitude_deg = 0.0;
    midpoint_coordinate.longitude_deg = 1.0;
    const QUuid pipe_uuid = editor.addPipe(from_uuid, to_uuid, {midpoint_coordinate});
    expectTrue(!pipe_uuid.isNull(), "split-remapping pipe receives a UUID");

    CoordinateWGS84 demand_coordinate;
    demand_coordinate.latitude_deg = 0.2;
    demand_coordinate.longitude_deg = 1.0;
    const QUuid before_uuid = editor.addDemandPoint(demand_coordinate);
    const QUuid at_uuid = editor.addDemandPoint(demand_coordinate);
    const QUuid after_uuid = editor.addDemandPoint(demand_coordinate);

    expectTrue(editor.attachDemandPointToPipe(before_uuid, pipe_uuid, 0.25),
               "pre-split demand point attaches before split position");
    expectTrue(editor.attachDemandPointToPipe(at_uuid, pipe_uuid, 0.50),
               "pre-split demand point attaches at split position");
    expectTrue(editor.attachDemandPointToPipe(after_uuid, pipe_uuid, 0.75),
               "pre-split demand point attaches after split position");

    const QUuid split_junction_uuid = editor.addJunction(midpoint_coordinate);
    const QUuid second_pipe_uuid = editor.splitPipeAtVertex(pipe_uuid, 0, split_junction_uuid);
    expectTrue(!second_pipe_uuid.isNull(), "pipe can be split while demand points are attached");

    const std::optional<HydraulicDemandPoint> before_split = editor.demandPoint(before_uuid);
    const std::optional<HydraulicDemandPoint> at_split = editor.demandPoint(at_uuid);
    const std::optional<HydraulicDemandPoint> after_split = editor.demandPoint(after_uuid);
    expectTrue(before_split.has_value() && at_split.has_value() && after_split.has_value(),
               "all demand points survive pipe split");
    if (before_split.has_value())
    {
        expectTrue(before_split->attachment.type == HydraulicDemandPointAttachmentType::Pipe,
                   "attachment before split remains on a pipe");
        expectTrue(before_split->attachment.pipe_uuid == pipe_uuid,
                   "attachment before split remains on first pipe");
        expectNear(before_split->attachment.pipe_position, 0.50, 1e-9,
                   "attachment before split is renormalized on first pipe");
    }
    if (at_split.has_value())
    {
        expectTrue(at_split->attachment.type == HydraulicDemandPointAttachmentType::Junction,
                   "attachment exactly at split becomes a junction attachment");
        expectTrue(at_split->attachment.junction_uuid == split_junction_uuid,
                   "attachment exactly at split targets the new junction");
    }
    if (after_split.has_value())
    {
        expectTrue(after_split->attachment.type == HydraulicDemandPointAttachmentType::Pipe,
                   "attachment after split remains on a pipe");
        expectTrue(after_split->attachment.pipe_uuid == second_pipe_uuid,
                   "attachment after split moves to second pipe");
        expectNear(after_split->attachment.pipe_position, 0.50, 1e-9,
                   "attachment after split is renormalized on second pipe");
    }

    expectTrue(editor.undoPipeSplit(pipe_uuid, second_pipe_uuid, split_junction_uuid),
               "pipe split can be undone with demand-point attachments");
    const std::optional<HydraulicDemandPoint> before_undo = editor.demandPoint(before_uuid);
    const std::optional<HydraulicDemandPoint> at_undo = editor.demandPoint(at_uuid);
    const std::optional<HydraulicDemandPoint> after_undo = editor.demandPoint(after_uuid);
    if (before_undo.has_value())
    {
        expectTrue(before_undo->attachment.pipe_uuid == pipe_uuid,
                   "undo restores pre-split attachment to original pipe");
        expectNear(before_undo->attachment.pipe_position, 0.25, 1e-9,
                   "undo restores pre-split normalized position");
    }
    if (at_undo.has_value())
    {
        expectTrue(at_undo->attachment.type == HydraulicDemandPointAttachmentType::Pipe,
                   "undo converts split-junction attachment back to pipe attachment");
        expectTrue(at_undo->attachment.pipe_uuid == pipe_uuid,
                   "undo restores split-point attachment to original pipe");
        expectNear(at_undo->attachment.pipe_position, 0.50, 1e-9,
                   "undo restores split-point normalized position");
    }
    if (after_undo.has_value())
    {
        expectTrue(after_undo->attachment.pipe_uuid == pipe_uuid,
                   "undo restores post-split attachment to original pipe");
        expectNear(after_undo->attachment.pipe_position, 0.75, 1e-9,
                   "undo restores post-split normalized position");
    }
}

void testAssignedDemandPointAttachmentTopologyEdits()
{
    NetworkHydraulicDistribution network;
    HydraulicNetworkEditor editor(network);

    CoordinateWGS84 from_coordinate;
    from_coordinate.latitude_deg = 0.0;
    from_coordinate.longitude_deg = 0.0;
    const QUuid from_uuid = editor.addJunction(from_coordinate);

    CoordinateWGS84 to_coordinate;
    to_coordinate.latitude_deg = 0.0;
    to_coordinate.longitude_deg = 2.0;
    const QUuid to_uuid = editor.addJunction(to_coordinate);

    CoordinateWGS84 midpoint_coordinate;
    midpoint_coordinate.latitude_deg = 0.0;
    midpoint_coordinate.longitude_deg = 1.0;
    const QUuid pipe_uuid = editor.addPipe(from_uuid, to_uuid, {midpoint_coordinate});
    expectTrue(!pipe_uuid.isNull(), "assigned-mode split test pipe receives a UUID");

    CoordinateWGS84 demand_coordinate;
    demand_coordinate.latitude_deg = 0.2;
    demand_coordinate.longitude_deg = 1.0;
    const QUuid before_uuid = editor.addDemandPoint(demand_coordinate);
    const QUuid at_uuid = editor.addDemandPoint(demand_coordinate);
    const QUuid after_uuid = editor.addDemandPoint(demand_coordinate);

    expectTrue(editor.attachDemandPointToPipe(before_uuid, pipe_uuid, 0.40),
               "assigned-mode point before split attaches to pipe");
    expectTrue(editor.attachDemandPointToPipe(at_uuid, pipe_uuid, 0.50),
               "assigned-mode point at split attaches to pipe");
    expectTrue(editor.attachDemandPointToPipe(after_uuid, pipe_uuid, 0.60),
               "assigned-mode point after split attaches to pipe");
    expectTrue(editor.setDemandPointPipeAllocationMode(
                   before_uuid, HydraulicDemandPointPipeAllocationMode::AssignedJunction),
               "before-split point enables assigned-junction mode");
    expectTrue(editor.setDemandPointPipeAllocationMode(
                   at_uuid, HydraulicDemandPointPipeAllocationMode::AssignedJunction),
               "at-split point enables assigned-junction mode");
    expectTrue(editor.setDemandPointPipeAllocationMode(
                   after_uuid, HydraulicDemandPointPipeAllocationMode::AssignedJunction),
               "after-split point enables assigned-junction mode");

    std::optional<HydraulicDemandPoint> before = editor.demandPoint(before_uuid);
    std::optional<HydraulicDemandPoint> at = editor.demandPoint(at_uuid);
    std::optional<HydraulicDemandPoint> after = editor.demandPoint(after_uuid);
    if (before.has_value())
    {
        expectTrue(before->attachment.pipe_assigned_junction_uuid == from_uuid,
                   "assigned point before split initially uses nearer from junction");
    }
    if (at.has_value())
    {
        expectTrue(at->attachment.pipe_assigned_junction_uuid == from_uuid,
                   "assigned point at midpoint uses deterministic from-junction tie break");
    }
    if (after.has_value())
    {
        expectTrue(after->attachment.pipe_assigned_junction_uuid == to_uuid,
                   "assigned point after split initially uses nearer to junction");
    }

    const QUuid split_junction_uuid = editor.addJunction(midpoint_coordinate);
    const QUuid second_pipe_uuid = editor.splitPipeAtVertex(pipe_uuid, 0, split_junction_uuid);
    expectTrue(!second_pipe_uuid.isNull(),
               "assigned-junction pipe can be split while customer points are attached");

    before = editor.demandPoint(before_uuid);
    at = editor.demandPoint(at_uuid);
    after = editor.demandPoint(after_uuid);
    if (before.has_value())
    {
        expectTrue(before->attachment.type == HydraulicDemandPointAttachmentType::Pipe,
                   "assigned point before split remains pipe-attached");
        expectTrue(before->attachment.pipe_uuid == pipe_uuid,
                   "assigned point before split remains on first half");
        expectNear(before->attachment.pipe_position, 0.80, 1e-9,
                   "assigned point before split is renormalized on first half");
        expectTrue(before->attachment.pipe_assigned_junction_uuid == split_junction_uuid,
                   "assigned point before split reallocates to nearer junction of first half");
    }
    if (at.has_value())
    {
        expectTrue(at->attachment.type == HydraulicDemandPointAttachmentType::Pipe,
                   "assigned point exactly at split stays pipe-attached");
        expectTrue(at->attachment.pipe_uuid == pipe_uuid,
                   "assigned point exactly at split deterministically stays on first half");
        expectNear(at->attachment.pipe_position, 1.0, 1e-9,
                   "assigned point exactly at split maps to end of first half");
        expectTrue(at->attachment.pipe_assigned_junction_uuid == split_junction_uuid,
                   "assigned point exactly at split reallocates to split junction");
    }
    if (after.has_value())
    {
        expectTrue(after->attachment.type == HydraulicDemandPointAttachmentType::Pipe,
                   "assigned point after split remains pipe-attached");
        expectTrue(after->attachment.pipe_uuid == second_pipe_uuid,
                   "assigned point after split moves to second half");
        expectNear(after->attachment.pipe_position, 0.20, 1e-9,
                   "assigned point after split is renormalized on second half");
        expectTrue(after->attachment.pipe_assigned_junction_uuid == split_junction_uuid,
                   "assigned point after split reallocates to nearer junction of second half");
    }

    expectTrue(editor.undoPipeSplit(pipe_uuid, second_pipe_uuid, split_junction_uuid),
               "assigned-junction pipe split can be undone");
    before = editor.demandPoint(before_uuid);
    at = editor.demandPoint(at_uuid);
    after = editor.demandPoint(after_uuid);
    if (before.has_value())
    {
        expectTrue(before->attachment.pipe_uuid == pipe_uuid,
                   "undo restores assigned point before split to original pipe");
        expectNear(before->attachment.pipe_position, 0.40, 1e-9,
                   "undo restores assigned point before split position");
        expectTrue(before->attachment.pipe_assigned_junction_uuid == from_uuid,
                   "undo reallocates assigned point before split to nearer original endpoint");
    }
    if (at.has_value())
    {
        expectTrue(at->attachment.type == HydraulicDemandPointAttachmentType::Pipe,
                   "undo keeps assigned midpoint point pipe-attached");
        expectTrue(at->attachment.pipe_uuid == pipe_uuid,
                   "undo restores assigned midpoint point to original pipe");
        expectNear(at->attachment.pipe_position, 0.50, 1e-9,
                   "undo restores assigned midpoint position");
        expectTrue(at->attachment.pipe_assigned_junction_uuid == from_uuid,
                   "undo uses deterministic from-junction tie break at midpoint");
    }
    if (after.has_value())
    {
        expectTrue(after->attachment.pipe_uuid == pipe_uuid,
                   "undo restores assigned point after split to original pipe");
        expectNear(after->attachment.pipe_position, 0.60, 1e-9,
                   "undo restores assigned point after split position");
        expectTrue(after->attachment.pipe_assigned_junction_uuid == to_uuid,
                   "undo reallocates assigned point after split to nearer original endpoint");
    }

    expectTrue(editor.deleteJunction(to_uuid),
               "endpoint junction with assigned-junction customer points can be deleted");
    before = editor.demandPoint(before_uuid);
    at = editor.demandPoint(at_uuid);
    after = editor.demandPoint(after_uuid);
    expectTrue(before.has_value() && at.has_value() && after.has_value(),
               "assigned-junction demand points survive deletion of a connected junction");
    if (before.has_value())
    {
        expectTrue(before->attachment.type == HydraulicDemandPointAttachmentType::None,
                   "deleting connected junction detaches assigned point from deleted pipe");
    }
    if (at.has_value())
    {
        expectTrue(at->attachment.type == HydraulicDemandPointAttachmentType::None,
                   "deleting connected junction detaches midpoint assigned point");
    }
    if (after.has_value())
    {
        expectTrue(after->attachment.type == HydraulicDemandPointAttachmentType::None,
                   "deleting assigned junction detaches its pipe-attached demand point");
        expectTrue(after->attachment.pipe_assigned_junction_uuid.isNull(),
                   "deleting assigned junction clears stale assigned-junction UUID");
    }

    CoordinateWGS84 replacement_from_coordinate;
    replacement_from_coordinate.latitude_deg = 1.0;
    replacement_from_coordinate.longitude_deg = 0.0;
    const QUuid replacement_from_uuid = editor.addJunction(replacement_from_coordinate);
    CoordinateWGS84 replacement_to_coordinate;
    replacement_to_coordinate.latitude_deg = 1.0;
    replacement_to_coordinate.longitude_deg = 1.0;
    const QUuid replacement_to_uuid = editor.addJunction(replacement_to_coordinate);
    const QUuid replacement_pipe_uuid =
        editor.addPipe(replacement_from_uuid, replacement_to_uuid, {});
    expectTrue(editor.attachDemandPointToPipe(before_uuid, replacement_pipe_uuid, 0.75),
               "assigned point can be reattached before pipe-deletion check");
    expectTrue(editor.setDemandPointPipeAllocationMode(
                   before_uuid, HydraulicDemandPointPipeAllocationMode::AssignedJunction),
               "reattached point can restore assigned-junction mode");
    expectTrue(editor.deletePipe(replacement_pipe_uuid),
               "pipe carrying an assigned-junction demand point can be deleted");
    before = editor.demandPoint(before_uuid);
    if (before.has_value())
    {
        expectTrue(before->attachment.type == HydraulicDemandPointAttachmentType::None,
                   "deleting pipe detaches assigned-junction demand point");
        expectTrue(before->attachment.pipe_assigned_junction_uuid.isNull(),
                   "deleting pipe clears assigned-junction UUID");
    }
}
}

int main()
{
    test_harness.runCase("pipe allocation model", testDemandPointPipeAllocationModel);
    test_harness.runCase("pipe split remapping", testDemandPointAttachmentRemappingOnPipeSplit);
    test_harness.runCase("assigned-junction topology edits", testAssignedDemandPointAttachmentTopologyEdits);
    return test_harness.finish();
}
