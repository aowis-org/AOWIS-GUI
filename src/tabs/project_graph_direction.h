#pragma once

#include <QPointF>
#include <QRectF>

#include <cmath>

// Cards remain upright in either orientation; only the direction of flow
// and the side of the card used for an edge change.
namespace ProjectGraphDirection
{
inline QPointF advance(bool top_down, double distance)
{
    return top_down ? QPointF(0.0, distance) : QPointF(distance, 0.0);
}

inline double extent(bool top_down, const QRectF &bounds)
{
    return top_down ? bounds.height() : bounds.width();
}

inline double along(bool top_down, const QPointF &point)
{
    return top_down ? point.y() : point.x();
}

inline bool aligned(bool top_down, const QPointF &a, const QPointF &b)
{
    return std::abs(top_down ? a.x() - b.x() : a.y() - b.y()) < 0.01;
}
}
