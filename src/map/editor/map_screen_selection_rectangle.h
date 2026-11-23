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
    return QRect(start.toPoint(), current.toPoint()).normalized()
        .intersected(QRect(QPoint(0, 0), viewport_size));
}
}
