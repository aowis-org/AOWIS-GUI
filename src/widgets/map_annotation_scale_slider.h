#ifndef WIDGETS_MAP_ANNOTATION_SCALE_SLIDER_H
#define WIDGETS_MAP_ANNOTATION_SCALE_SLIDER_H

#include "map/core/map_annotation_display_scale.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSlider>

// Same leftmost Off detent and right-click-to-reset behavior as the existing
// monitor node/flow-size controls, shared by Editor and Monitor tool panels.
class MapAnnotationScaleSlider final : public QSlider
{
public:
    explicit MapAnnotationScaleSlider(QWidget *parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        setObjectName(QStringLiteral("mapAnnotationScaleSlider"));
        setRange(0, MapAnnotationDisplayScale::MaximumSliderPosition);
        setSingleStep(1);
        setPageStep(10);
        setValue(MapAnnotationDisplayScale::positionFromPercent(
            MapAnnotationDisplayScale::DefaultPercent));
        connect(this, &QSlider::valueChanged, this, [this](int) { updateTip(); });
        updateTip();
    }

    int percent() const
    {
        return MapAnnotationDisplayScale::percentFromPosition(value());
    }

    void setPercent(int percent)
    {
        const QSignalBlocker blocker(this);
        setValue(MapAnnotationDisplayScale::positionFromPercent(percent));
        updateTip();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QSlider::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            updateTip();
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton)
        {
            setValue(MapAnnotationDisplayScale::positionFromPercent(
                MapAnnotationDisplayScale::DefaultPercent));
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        setValue(MapAnnotationDisplayScale::positionFromPercent(
            MapAnnotationDisplayScale::DefaultPercent));
        event->accept();
    }

private:
    void updateTip()
    {
        const QString value = percent() == 0
            ? QCoreApplication::translate("MapAnnotationScaleSlider", "Off")
            : QStringLiteral("%1%").arg(percent());
        setToolTip(QCoreApplication::translate("MapAnnotationScaleSlider",
            "Annotation display size: %1. Far left hides all annotations; "
            "other positions scale their map size. Right-click to reset.").arg(value));
    }
};

#endif
