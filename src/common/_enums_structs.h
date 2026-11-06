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



// VisualNode, VisualLink, VisualHeatmap and InfrastructureEntity live in
// AOWIS-GUI-Render with the network render contract.
#include "map/core/map_network_types.h"



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
