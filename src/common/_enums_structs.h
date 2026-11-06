#ifndef _ENUMS_STRUCTS_H
#define _ENUMS_STRUCTS_H

#include <optional>
#include <cstdint>

#include <QObject>


enum MapServerMode
{
    REST,
    Standalone
};

// MapProvider and MapViewMode live in AOWIS-GUI-Render with MapModel.
#include "map/core/map_view_types.h"

enum StatusColorCode
{
    None,
    Green,
    Yellow,
    Red
};

enum CanvasMode
{
    Edit,
    Monitor
};



enum class VisualNode
{
    None,
    Elevation,
    BaseDemand,
    TotalDemand,
    DemandDeficit,
    EmitterFlow,
    Leakage,
    Head,
    Pressure,
    Chlorine,
    RiverWater,
    LakeWater,
    WaterAge
};
enum class VisualLink
{
    None,
    Diameter,
    Length,
    Roughness,
    FlowRate,
    Velocity,
    HeadLoss,
    Leakage,
    Chlorine,
    RiverWater,
    LakeWater,
    WaterAge
};
enum class VisualHeatmap
{
    None,
    Elevation,
    BaseDemand,
    TotalDemand,
    DemandDeficit,
    EmitterFlow,
    Leakage,
    Head,
    Pressure,
    Chlorine,
    RiverWater,
    LakeWater,
    WaterAge
};



enum class HistoryVisibility
{
    Visible,
    Archived,
    Deleted
};

enum class MapEditTool
{
    Select          = 100,
    
    Pipe            = 1,
    Junction        = 2,
    Valve           = 3,
    Demand_Point    = 4,
    Pump            = 5,
    Tank            = 6,
    Power           = 7,
    Reservoir       = 8,
    Note            = 9
};
enum MapEditToolSub
{
    Tool_1 = 1,
    Tool_2 = 2,
    Tool_3 = 3,
    Tool_4 = 4,
    Tool_5 = 5
};

enum class InfrastructureEntity : std::uint8_t
{
    Unknown = 0,
    
    // Hydraulic / EPANET-style node entities
    Junction,
    Reservoir,
    Tank,
    
    // Hydraulic / EPANET-style link entities
    Pipe,
    Pump,
    Valve,
    
    // AOWIS hydraulic extensions
    DemandPoint,
    
    // Electric node/storage/generation entities
    ElectricJunction,
    Battery,
    Generator,
    SolarPanel,
    Inverter,
    Transformer,
    
    // Electric link/control entities
    Cable,
    Switch,
    Fuse,
    CircuitBreaker,
    
    // Non-simulation / UI entities
    Note
};

enum class JunctionPhysicalKind
{
    Physical,
    Virtual
};

enum class HeadlossFormula
{
    None           = 0x0,
    HazenWilliams  = 0x1,
    DarcyWeisbach  = 0x2,
    ChezyManning   = 0x4
};

Q_DECLARE_FLAGS(HeadlossFormulas, HeadlossFormula)
Q_DECLARE_OPERATORS_FOR_FLAGS(HeadlossFormulas)

Q_DECLARE_METATYPE(HeadlossFormula)
Q_DECLARE_METATYPE(HeadlossFormulas)

#endif // _ENUMS_STRUCTS_H
