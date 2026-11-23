#pragma once

#include <QPointF>
#include <QRect>
#include <QSize>

// Select/rectangle dragging is a screen-space interaction. Geographic
// coordinates are correct for tile-cache regions, but not for hit-testing
// the exact sprites currently painted by bgfx during a pointer gesture.
namespace MapScreenSelectionRectangle
{
inline QRect bounded(const QPointF &start, const QPointF &current,
                     const QSize &viewport_size)
{
    if (viewport_size.isEmpty())
        return QRect();
    // QRect has inclusive right/bottom endpoints. Normalizing a rectangle
    // constructed with reversed QPoint corners can shift its edges by one
    // pixel. Sort the corners before constructing the QRect so a drag has
    // exactly the same bounds in either direction.
    const QPoint start_point = start.toPoint();
    const QPoint current_point = current.toPoint();
    const QPoint top_left(qMin(start_point.x(), current_point.x()),
                          qMin(start_point.y(), current_point.y()));
    const QPoint bottom_right(qMax(start_point.x(), current_point.x()),
                              qMax(start_point.y(), current_point.y()));
    return QRect(top_left, bottom_right)
        .intersected(QRect(QPoint(0, 0), viewport_size));
}
}
