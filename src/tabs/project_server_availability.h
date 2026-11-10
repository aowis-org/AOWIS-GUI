#pragma once

#include <QColor>
#include <QRectF>
#include <QString>

// Presentation snapshot of the ProjectsWidget-owned source session. This is
// never a separate network probe: the existing HTTP health monitor is the
// sole source of remote liveness for the sidebar, inspector and graph.
struct ProjectServerAvailability
{
    QColor color;
    QString status;
    QString details;
};

namespace ProjectServerBadge
{
// Shared graph-space geometry for the QPainter and bgfx card renderers.
// Leave the badge outside the title's reserved right inset.
inline QRectF indicatorRect(const QRectF &card)
{
    return QRectF(card.right() - 25.0, card.top() + 12.0, 12.0, 12.0);
}
}
