#ifndef MAP_CORE_MAP_ANNOTATION_DISPLAY_SCALE_H
#define MAP_CORE_MAP_ANNOTATION_DISPLAY_SCALE_H

#include <QtGlobal>

// Pure presentation state: never written into MODEL, DVMDB, or a revision.
// The first slider position is Off; all other positions are a useful size.
namespace MapAnnotationDisplayScale
{
inline constexpr int DefaultPercent = 100;
inline constexpr int MinimumPercent = 25;
inline constexpr int MaximumPercent = 250;
inline constexpr int MaximumSliderPosition = MaximumPercent - MinimumPercent + 1;

inline int percentFromPosition(int position)
{
    return position <= 0 ? 0 : qBound(MinimumPercent, position + MinimumPercent - 1,
                                       MaximumPercent);
}
inline int positionFromPercent(int percent)
{
    return percent <= 0 ? 0 : qBound(MinimumPercent, percent, MaximumPercent)
                                - MinimumPercent + 1;
}
inline double factor(int percent)
{
    return double(qBound(0, percent, MaximumPercent)) / 100.0;
}
} // namespace MapAnnotationDisplayScale

#endif
