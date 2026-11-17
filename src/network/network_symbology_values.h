#ifndef NETWORK_SYMBOLOGY_VALUES_H
#define NETWORK_SYMBOLOGY_VALUES_H

#include <cmath>
#include <limits>

#include <QtGlobal>
#include <QUuid>

#include <aowis/model/hydraulic/network_hydraulic_distribution.h>
#include <aowis/model/hydraulic/hydraulic_simulation_results.h>
#include <aowis/model/hydraulic/water_quality_simulation_results.h>

#include "common/_enums_structs.h"

inline double resolvedSymbologyElevationM(const HydraulicNodeJunction &junction)
{
    if (junction.elevation_input_type ==
        HydraulicNodeElevationInputType::TerrainElevationAndOffset)
    {
        return junction.terrain_elevation_m + junction.elevation_offset_m;
    }

    return junction.elevation_m;
}

inline double resolvedSymbologyElevationM(const HydraulicNodeReservoir &reservoir)
{
    if (reservoir.head_input_type ==
        HydraulicNodeElevationInputType::TerrainElevationAndOffset)
    {
        return reservoir.terrain_elevation_m + reservoir.hydraulic_head_offset_m;
    }

    return reservoir.hydraulic_head_m;
}

inline double resolvedSymbologyElevationM(const HydraulicNodeTank &tank)
{
    if (tank.elevation_input_type ==
        HydraulicNodeTankElevationInputType::TerrainElevationAndOffset)
    {
        return tank.terrain_elevation_m + tank.bottom_offset_m;
    }

    return tank.bottom_elevation_m;
}

inline double resolvedSymbologyElevationM(const HydraulicDemandPoint &)
{
    return std::numeric_limits<double>::quiet_NaN();
}



// Symbology values are produced by visitors: visit(const QUuid &uuid,
// double value) is called once per entity that has a value, in the order of
// the source lists. Nothing is collected here, so a time step never builds
// a hash table: the map resolves the uuids to render ids with an
// order-cached lookup (MapNetworkSymbologyResolver) and the ranges are
// minimum / maximum folds over the same visit (HydraulicData::symbologyRanges()).

template <typename Visitor>
void visitNetworkNodeSymbologyValues(
    const NetworkHydraulicDistribution &network_hydraulic_distribution, VisualNode visual_node, Visitor &&visit)
{
    switch (visual_node)
    {
    case VisualNode::Elevation:
        for (const HydraulicNodeJunction &junction : network_hydraulic_distribution.nodes_junctions)
            visit(junction.uuid, resolvedSymbologyElevationM(junction));
        for (const HydraulicNodeReservoir &reservoir : network_hydraulic_distribution.nodes_reservoirs)
            visit(reservoir.uuid, resolvedSymbologyElevationM(reservoir));
        for (const HydraulicNodeTank &tank : network_hydraulic_distribution.nodes_tanks)
            visit(tank.uuid, resolvedSymbologyElevationM(tank));
        break;
    case VisualNode::BaseDemand:
        for (const HydraulicNodeJunction &junction : network_hydraulic_distribution.nodes_junctions)
        {
            double base_demand_m3_per_h = 0.0;
            for (const HydraulicDemand &demand : junction.demands)
                base_demand_m3_per_h += demand.base_demand_m3_per_h;
            visit(junction.uuid, base_demand_m3_per_h);
        }
        for (const HydraulicDemandPoint &demand_point : network_hydraulic_distribution.demand_points)
        {
            double base_demand_m3_per_h = 0.0;
            for (const HydraulicDemand &demand : demand_point.demands)
                base_demand_m3_per_h += demand.base_demand_m3_per_h;
            visit(demand_point.uuid, base_demand_m3_per_h);
        }
        break;
    case VisualNode::None:
    case VisualNode::TotalDemand:
    case VisualNode::DemandDeficit:
    case VisualNode::EmitterFlow:
    case VisualNode::Leakage:
    case VisualNode::Head:
    case VisualNode::Pressure:
    case VisualNode::Chlorine:
    case VisualNode::RiverWater:
    case VisualNode::LakeWater:
    case VisualNode::WaterAge:
        break;
    }
}

template <typename Visitor>
void visitNetworkLinkSymbologyValues(
    const NetworkHydraulicDistribution &network_hydraulic_distribution, VisualLink visual_link, Visitor &&visit)
{
    switch (visual_link)
    {
    case VisualLink::Diameter:
        for (const HydraulicLinkPipe &pipe : network_hydraulic_distribution.links_pipes)
            visit(pipe.uuid, pipe.diameter_mm);
        for (const HydraulicLinkValve &valve : network_hydraulic_distribution.links_valves)
            visit(valve.uuid, valve.diameter_mm);
        break;
    case VisualLink::Length:
        for (const HydraulicLinkPipe &pipe : network_hydraulic_distribution.links_pipes)
            visit(pipe.uuid, pipe.length_measured_m.value_or(pipe.length_calculated_m));
        break;
    case VisualLink::Roughness:
        for (const HydraulicLinkPipe &pipe : network_hydraulic_distribution.links_pipes)
        {
            switch (network_hydraulic_distribution.options_hydraulic.headloss_formula)
            {
            case HydraulicHeadlossFormula::HazenWilliams:
                visit(pipe.uuid, pipe.roughness_hazen_williams);
                break;
            case HydraulicHeadlossFormula::DarcyWeisbach:
                visit(pipe.uuid, pipe.roughness_darcy_weisbach_mm);
                break;
            case HydraulicHeadlossFormula::ChezyManning:
                visit(pipe.uuid, pipe.roughness_chezy_manning);
                break;
            }
        }
        break;
    case VisualLink::None:
    case VisualLink::FlowRate:
    case VisualLink::Velocity:
    case VisualLink::HeadLoss:
    case VisualLink::Leakage:
    case VisualLink::Chlorine:
    case VisualLink::RiverWater:
    case VisualLink::LakeWater:
    case VisualLink::WaterAge:
        break;
    }
}

// The node metric a network-model heatmap shows (None: no network value).
inline VisualNode networkHeatmapNodeVisual(VisualHeatmap visual_heatmap)
{
    switch (visual_heatmap)
    {
    case VisualHeatmap::Elevation:
        return VisualNode::Elevation;
    case VisualHeatmap::BaseDemand:
        return VisualNode::BaseDemand;
    case VisualHeatmap::TotalDemand:
        return VisualNode::TotalDemand;
    case VisualHeatmap::DemandDeficit:
        return VisualNode::DemandDeficit;
    case VisualHeatmap::EmitterFlow:
        return VisualNode::EmitterFlow;
    case VisualHeatmap::Leakage:
        return VisualNode::Leakage;
    case VisualHeatmap::Head:
        return VisualNode::Head;
    case VisualHeatmap::Pressure:
        return VisualNode::Pressure;
    case VisualHeatmap::Chlorine:
        return VisualNode::Chlorine;
    case VisualHeatmap::RiverWater:
        return VisualNode::RiverWater;
    case VisualHeatmap::LakeWater:
        return VisualNode::LakeWater;
    case VisualHeatmap::WaterAge:
    case VisualHeatmap::None:
        break;
    }

    return VisualNode::None;
}

inline bool nodeVisualUsesHydraulicSimulationResult(VisualNode visual_node)
{
    switch (visual_node)
    {
    case VisualNode::TotalDemand:
    case VisualNode::DemandDeficit:
    case VisualNode::EmitterFlow:
    case VisualNode::Leakage:
    case VisualNode::Head:
    case VisualNode::Pressure:
        return true;
    case VisualNode::None:
    case VisualNode::Elevation:
    case VisualNode::BaseDemand:
    case VisualNode::Chlorine:
    case VisualNode::RiverWater:
    case VisualNode::LakeWater:
    case VisualNode::WaterAge:
        return false;
    }

    return false;
}

inline bool linkVisualUsesHydraulicSimulationResult(VisualLink visual_link)
{
    switch (visual_link)
    {
    case VisualLink::FlowRate:
    case VisualLink::Velocity:
    case VisualLink::HeadLoss:
    case VisualLink::Leakage:
        return true;
    case VisualLink::None:
    case VisualLink::Diameter:
    case VisualLink::Length:
    case VisualLink::Roughness:
    case VisualLink::Chlorine:
    case VisualLink::RiverWater:
    case VisualLink::LakeWater:
    case VisualLink::WaterAge:
        return false;
    }

    return false;
}

inline bool heatmapVisualUsesHydraulicSimulationResult(VisualHeatmap visual_heatmap)
{
    return nodeVisualUsesHydraulicSimulationResult(networkHeatmapNodeVisual(visual_heatmap));
}

template <typename Visitor>
void visitHydraulicNodeSymbologyValues(
    const HydraulicSimulationResult &result, VisualNode visual_node, Visitor &&visit)
{
    switch (visual_node)
    {
    case VisualNode::TotalDemand:
        for (const HydraulicSimulationResultNodeJunction &junction : result.nodes_junctions)
            visit(junction.uuid, junction.total_demand_m3_per_h);
        for (const HydraulicSimulationResultNodeReservoir &reservoir : result.nodes_reservoirs)
            visit(reservoir.uuid, reservoir.net_demand_m3_per_h);
        for (const HydraulicSimulationResultNodeTank &tank : result.nodes_tanks)
            visit(tank.uuid, tank.net_demand_m3_per_h);
        break;
    case VisualNode::DemandDeficit:
        for (const HydraulicSimulationResultNodeJunction &junction : result.nodes_junctions)
            visit(junction.uuid, junction.demand_deficit_m3_per_h);
        break;
    case VisualNode::EmitterFlow:
        for (const HydraulicSimulationResultNodeJunction &junction : result.nodes_junctions)
            visit(junction.uuid, junction.emitter_flow_m3_per_h);
        break;
    case VisualNode::Leakage:
        for (const HydraulicSimulationResultNodeJunction &junction : result.nodes_junctions)
            visit(junction.uuid, junction.leakage_flow_m3_per_h);
        break;
    case VisualNode::Head:
        for (const HydraulicSimulationResultNodeJunction &junction : result.nodes_junctions)
            visit(junction.uuid, junction.hydraulic_head_m);
        for (const HydraulicSimulationResultNodeReservoir &reservoir : result.nodes_reservoirs)
            visit(reservoir.uuid, reservoir.hydraulic_head_m);
        for (const HydraulicSimulationResultNodeTank &tank : result.nodes_tanks)
            visit(tank.uuid, tank.hydraulic_head_m);
        break;
    case VisualNode::Pressure:
        for (const HydraulicSimulationResultNodeJunction &junction : result.nodes_junctions)
            visit(junction.uuid, junction.pressure_head_m);
        for (const HydraulicSimulationResultNodeReservoir &reservoir : result.nodes_reservoirs)
            visit(reservoir.uuid, reservoir.pressure_head_m);
        for (const HydraulicSimulationResultNodeTank &tank : result.nodes_tanks)
            visit(tank.uuid, tank.pressure_head_m);
        break;
    case VisualNode::None:
    case VisualNode::Elevation:
    case VisualNode::BaseDemand:
    case VisualNode::Chlorine:
    case VisualNode::RiverWater:
    case VisualNode::LakeWater:
    case VisualNode::WaterAge:
        break;
    }
}

template <typename Visitor>
void visitHydraulicLinkSymbologyValues(
    const HydraulicSimulationResult &result, VisualLink visual_link, Visitor &&visit)
{
    switch (visual_link)
    {
    case VisualLink::FlowRate:
        for (const HydraulicSimulationResultLinkPipe &pipe : result.links_pipes)
            visit(pipe.uuid, pipe.flow_m3_per_h);
        for (const HydraulicSimulationResultLinkPump &pump : result.links_pumps)
            visit(pump.uuid, pump.flow_m3_per_h);
        for (const HydraulicSimulationResultLinkValve &valve : result.links_valves)
            visit(valve.uuid, valve.flow_m3_per_h);
        break;
    case VisualLink::Velocity:
        for (const HydraulicSimulationResultLinkPipe &pipe : result.links_pipes)
            visit(pipe.uuid, pipe.velocity_m_per_s);
        for (const HydraulicSimulationResultLinkPump &pump : result.links_pumps)
            visit(pump.uuid, pump.velocity_m_per_s);
        for (const HydraulicSimulationResultLinkValve &valve : result.links_valves)
            visit(valve.uuid, valve.velocity_m_per_s);
        break;
    case VisualLink::HeadLoss:
        for (const HydraulicSimulationResultLinkPipe &pipe : result.links_pipes)
            visit(pipe.uuid, pipe.head_loss_m);
        for (const HydraulicSimulationResultLinkValve &valve : result.links_valves)
            visit(valve.uuid, valve.head_loss_m);
        break;
    case VisualLink::Leakage:
        for (const HydraulicSimulationResultLinkPipe &pipe : result.links_pipes)
            visit(pipe.uuid, pipe.leakage_flow_m3_per_h);
        break;
    case VisualLink::None:
    case VisualLink::Diameter:
    case VisualLink::Length:
    case VisualLink::Roughness:
    case VisualLink::Chlorine:
    case VisualLink::RiverWater:
    case VisualLink::LakeWater:
    case VisualLink::WaterAge:
        break;
    }
}

inline qint8 hydraulicFlowDirection(double flow_m3_per_h)
{
    // EPANET always solves internally in US customary units (cubic feet per
    // second) regardless of the network's declared display units, and treats
    // any flow below its own internal TINY threshold (1.E-6 cfs, see
    // external/epanet/src/types.h) as numerically meaningless solver noise
    // rather than a real flow (see e.g. hydstatus.c's pump/tank status
    // checks). Converted to this field's m3/h units, that noise floor is
    // roughly 1.E-6 * 101.94 =~ 1.E-4 m3/h. A network that cannot pass any
    // real flow (e.g. a dead-ended branch, or a tank that cannot accept
    // inflow) still converges with residual flows on that order, which used
    // to render as a confident directional arrow. Use a dead band comfortably
    // above that noise floor so only flow EPANET itself considers meaningful
    // is shown as directional.
    constexpr double FlowDirectionEpsilonM3PerH = 1.0e-3;
    if (!std::isfinite(flow_m3_per_h) ||
        std::abs(flow_m3_per_h) <= FlowDirectionEpsilonM3PerH)
    {
        return 0;
    }

    return flow_m3_per_h > 0.0 ? qint8(1) : qint8(-1);
}

// visit(uuid, direction) for every link of the result (-1, 0, +1).
template <typename Visitor>
void visitHydraulicLinkFlowDirections(const HydraulicSimulationResult &result, Visitor &&visit)
{
    for (const HydraulicSimulationResultLinkPipe &pipe : result.links_pipes)
        visit(pipe.uuid, hydraulicFlowDirection(pipe.flow_m3_per_h));
    for (const HydraulicSimulationResultLinkPump &pump : result.links_pumps)
        visit(pump.uuid, hydraulicFlowDirection(pump.flow_m3_per_h));
    for (const HydraulicSimulationResultLinkValve &valve : result.links_valves)
        visit(valve.uuid, hydraulicFlowDirection(valve.flow_m3_per_h));
}

template <typename Visitor>
void visitWaterAgeNodeSymbologyValues(const WaterQualitySimulationResult &result, Visitor &&visit)
{
    for (const WaterQualitySimulationResultNodeJunction &junction : result.nodes_junctions)
        visit(junction.uuid, junction.water_age_h);
    for (const WaterQualitySimulationResultNodeReservoir &reservoir : result.nodes_reservoirs)
        visit(reservoir.uuid, reservoir.water_age_h);
    for (const WaterQualitySimulationResultNodeTank &tank : result.nodes_tanks)
        visit(tank.uuid, tank.water_age_h);
}

template <typename Visitor>
void visitWaterAgeLinkSymbologyValues(const WaterQualitySimulationResult &result, Visitor &&visit)
{
    for (const WaterQualitySimulationResultLinkPipe &pipe : result.links_pipes)
        visit(pipe.uuid, pipe.water_age_h);
    for (const WaterQualitySimulationResultLinkPump &pump : result.links_pumps)
        visit(pump.uuid, pump.water_age_h);
    for (const WaterQualitySimulationResultLinkValve &valve : result.links_valves)
        visit(valve.uuid, valve.water_age_h);
}

#endif // NETWORK_SYMBOLOGY_VALUES_H
