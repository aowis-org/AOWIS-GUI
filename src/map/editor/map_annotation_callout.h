#ifndef MAP_EDITOR_MAP_ANNOTATION_CALLOUT_H
#define MAP_EDITOR_MAP_ANNOTATION_CALLOUT_H

#include <aowis/model/map/map_annotation.h>
#include "map/core/map_annotation_display_scale.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <algorithm>

// On-screen geometry of one map annotation callout, shared by the bgfx
// overlay (what is drawn) and hit testing (what is clicked), so the two can
// never disagree.
//
// The text is ONE line, shaped once at the annotation's base point size and
// drawn from the signed-distance glyph atlas scaled by text_scale
// (MapBgfxOverlayGlyphs::line()). The base font never changes with the
// display scale, so zooming the annotations reuses the same shaped line and
// the same glyphs; only the quads move.
//
// A callout with text: the text line padded by 9 px left/right and 5 px
// top/bottom, its bottom-left 5 px left of and 4 px above the anchor.
// A blank callout (a placement not yet typed): a fixed 92 x 34 px box, 7 px
// left of and 46 px above the anchor. All paddings scale with the display
// scale; text is never drawn smaller than MinimumPointSize.
namespace MapAnnotationCallout
{
inline constexpr double MinimumPointSize = 6.0;
inline constexpr double MaximumTextWidthPx = 330.0;

struct Layout
{
    // false: nothing is drawn (hidden, or display size Off).
    bool visible = false;
    // Placement not yet typed: GPU placeholder, no text.
    bool blank = false;
    double scale = 1.0;
    // Unscaled font of the line; glyph scale on screen.
    QFont font;
    double text_scale = 1.0;
    // Elided single line (empty when blank).
    QString text;
    // Callout rectangle and the top-left of the text line box (logical px).
    QRectF box;
    QPointF text_origin;
};

inline int basePointSize(MapAnnotationTextSize size)
{
    return size == MapAnnotationTextSize::Small ? 10
        : size == MapAnnotationTextSize::Large ? 16 : 12;
}

inline Layout layout(const MapAnnotation &annotation, const QPointF &anchor,
                     int scale_percent = MapAnnotationDisplayScale::DefaultPercent)
{
    Layout result;
    if (!annotation.style.visible || scale_percent <= 0)
        return result;
    result.visible = true;
    result.scale = MapAnnotationDisplayScale::factor(scale_percent);
    const double scale = result.scale;
    result.blank = annotation.text.trimmed().isEmpty();
    if (result.blank)
    {
        result.box = QRectF(anchor.x() - 7.0 * scale, anchor.y() - 46.0 * scale,
                            92.0 * scale, 34.0 * scale);
        return result;
    }
    const double point_size = double(basePointSize(annotation.style.text_size));
    result.font.setPointSizeF(point_size);
    result.text_scale = std::max(scale, MinimumPointSize / point_size);
    const QFontMetricsF metrics(result.font);
    result.text = metrics.elidedText(annotation.text.simplified(), Qt::ElideRight,
                                     MaximumTextWidthPx * scale / result.text_scale);
    const double width = metrics.horizontalAdvance(result.text) * result.text_scale
        + 18.0 * scale;
    const double height = metrics.height() * result.text_scale + 10.0 * scale;
    result.box = QRectF(anchor.x() - 5.0 * scale, anchor.y() - height - 4.0 * scale,
                        width, height);
    result.text_origin = QPointF(result.box.x() + 9.0 * scale, result.box.y() + 5.0 * scale);
    return result;
}
} // namespace MapAnnotationCallout

#endif
