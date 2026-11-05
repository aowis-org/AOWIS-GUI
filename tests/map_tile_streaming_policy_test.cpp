#include "map/render/map_tile_streaming_policy.h"

int main()
{
    using MapTileStreamingPolicy::uploadBudget;
    if (uploadBudget(false, true) != 2) return 1;
    if (uploadBudget(false, false) != 8) return 2;
    if (uploadBudget(true, true) != 3) return 3;
    if (uploadBudget(true, false) != 12) return 4;
    if (uploadBudget(false, true, 1) != 1) return 5;
    if (uploadBudget(true, false, 32) != 32) return 6;
    if (uploadBudget(false, true, 0) != 2) return 7;
    if (uploadBudget(false, true, 33) != 2) return 8;
    if (uploadBudget(true, true, -1) != 3) return 9;
    return 0;
}
