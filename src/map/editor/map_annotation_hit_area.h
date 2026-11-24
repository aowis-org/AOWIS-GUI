#ifndef MAP_EDITOR_MAP_ANNOTATION_HIT_AREA_H
#define MAP_EDITOR_MAP_ANNOTATION_HIT_AREA_H

#include <aowis/model/map/map_annotation.h>
#include "map/core/map_annotation_display_scale.h"
#include "map/editor/map_annotation_callout.h"

#include <QPointF>
#include <QRectF>

// Match the bgfx callout's on-screen geometry (MapAnnotationCallout, the
// same layout the overlay draws). In particular, the textless draft is
// 92x34 pixels ABOVE its anchor; the old generic label rectangle missed much
// of this visible placeholder and caused a click on it to create another
// annotation instead of selecting it. The area reaches down to the anchor
// pin.
namespace MapAnnotationHitArea
{
inline QRectF rect(const MapAnnotation &annotation, const QPointF &anchor,
                   int scale_percent = MapAnnotationDisplayScale::DefaultPercent)
{
    const MapAnnotationCallout::Layout callout =
        MapAnnotationCallout::layout(annotation, anchor, scale_percent);
    if (!callout.visible)
        return QRectF();
    if (callout.blank)
        return QRectF(callout.box.x(), callout.box.y(), callout.box.width(),
                      46.0 * callout.scale);
    return QRectF(callout.box.x(), callout.box.y(), callout.box.width(),
                  callout.box.height() + 8.0 * callout.scale);
}
}

#endif
