#ifndef MAP_EDITOR_MAP_ANNOTATION_HIT_AREA_H
#define MAP_EDITOR_MAP_ANNOTATION_HIT_AREA_H

#include <aowis/model/map/map_annotation.h>
#include "map/core/map_annotation_display_scale.h"

#include <QFont>
#include <QFontMetrics>
#include <QPointF>
#include <QRectF>

// Match the bgfx callout's on-screen geometry. In particular, the
// textless draft is 92x34 pixels ABOVE its anchor; the old generic label
// rectangle missed much of this visible placeholder and caused a click on
// it to create another annotation instead of selecting it.
namespace MapAnnotationHitArea
{
inline QRectF rect(const MapAnnotation &annotation, const QPointF &anchor,
                   int scale_percent = MapAnnotationDisplayScale::DefaultPercent)
{
    if (!annotation.style.visible || scale_percent <= 0)
        return QRectF();
    const double scale = MapAnnotationDisplayScale::factor(scale_percent);
    if (annotation.text.trimmed().isEmpty())
        return QRectF(anchor.x() - 7.0 * scale, anchor.y() - 46.0 * scale,
                      92.0 * scale, 46.0 * scale);
    QFont font;
    const int base_size = annotation.style.text_size == MapAnnotationTextSize::Small ? 10
        : annotation.style.text_size == MapAnnotationTextSize::Large ? 16 : 12;
    font.setPointSize(qMax(6, qRound(base_size * scale)));
    const QFontMetrics metrics(font);
    const QString text = metrics.elidedText(annotation.text.simplified(), Qt::ElideRight,
                                            qRound(330.0 * scale));
    const double width = double(metrics.horizontalAdvance(text)) + qRound(18.0 * scale);
    const double height = double(metrics.height()) + qRound(10.0 * scale);
    return QRectF(anchor.x() - 5.0 * scale, anchor.y() - height - 4.0 * scale,
                  width, height + 8.0 * scale);
}
}

#endif
