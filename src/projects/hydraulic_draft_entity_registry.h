#pragma once

// One canonical GUI-side mapping for the 17 persisted hydraulic entity
// collections. The storage kind and membership ordinal are DVMDB's native draft
// contract (hydraulic members 201..217), while MODEL owns the type names and their canonical binary codec.
//
// Capture and event materialization MUST use this same registry. Network-root
// metadata is not an entity collection and belongs to HC-03/HC-04.
#define AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(X) \
    X(pipe_materials, HydraulicPipeMaterial, 208, 0) \
    X(patterns_time, HydraulicPatternTime, 209, 1) \
    X(curves_tank_volume, HydraulicCurveTankVolume, 210, 2) \
    X(curves_pump_head, HydraulicCurvePumpHead, 211, 3) \
    X(curves_pump_efficiency, HydraulicCurvePumpEfficiency, 212, 4) \
    X(curves_valve_headloss, HydraulicCurveValveHeadloss, 213, 5) \
    X(curves_valve_characteristic, HydraulicCurveValveCharacteristic, 214, 6) \
    X(curves_generic, HydraulicCurveGeneric, 215, 7) \
    X(nodes_reservoirs, HydraulicNodeReservoir, 202, 8) \
    X(nodes_junctions, HydraulicNodeJunction, 201, 9) \
    X(nodes_tanks, HydraulicNodeTank, 203, 10) \
    X(demand_points, HydraulicDemandPoint, 207, 11) \
    X(links_pipes, HydraulicLinkPipe, 204, 12) \
    X(links_pumps, HydraulicLinkPump, 205, 13) \
    X(links_valves, HydraulicLinkValve, 206, 14) \
    X(controls_simple, HydraulicControlSimple, 216, 15) \
    X(controls_rules, HydraulicControlRule, 217, 16)
