#pragma once

// One canonical GUI-side mapping for the 18 persisted hydraulic entity
// collections. The storage kind and membership ordinal are DVMDB's V2 draft
// contract, while MODEL owns the type names and their canonical binary codec.
//
// Capture and event materialization MUST use this same registry. Network-root
// metadata is not an entity collection and belongs to HC-03/HC-04.
#define AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(X) \
    X(pipe_materials, HydraulicPipeMaterial, 8, 0) \
    X(patterns_time, HydraulicPatternTime, 9, 1) \
    X(curves_tank_volume, HydraulicCurveTankVolume, 10, 2) \
    X(curves_pump_head, HydraulicCurvePumpHead, 11, 3) \
    X(curves_pump_efficiency, HydraulicCurvePumpEfficiency, 12, 4) \
    X(curves_valve_headloss, HydraulicCurveValveHeadloss, 13, 5) \
    X(curves_valve_characteristic, HydraulicCurveValveCharacteristic, 14, 6) \
    X(curves_generic, HydraulicCurveGeneric, 15, 7) \
    X(nodes_reservoirs, HydraulicNodeReservoir, 2, 8) \
    X(nodes_junctions, HydraulicNodeJunction, 1, 9) \
    X(nodes_tanks, HydraulicNodeTank, 3, 10) \
    X(demand_points, HydraulicDemandPoint, 7, 11) \
    X(links_pipes, HydraulicLinkPipe, 4, 12) \
    X(links_pumps, HydraulicLinkPump, 5, 13) \
    X(links_valves, HydraulicLinkValve, 6, 14) \
    X(controls_simple, HydraulicControlSimple, 16, 15) \
    X(controls_rules, HydraulicControlRule, 17, 16) \
    X(map_labels, HydraulicMapLabel, 18, 17)
