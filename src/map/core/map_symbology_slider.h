#ifndef MAP_SYMBOLOGY_SLIDER_H
#define MAP_SYMBOLOGY_SLIDER_H

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QString>

class MapSymbologySlider final : public QSlider
{
public:
    MapSymbologySlider(int value_minimum, int value_maximum, int value_default,
                       const QString &description, const QString &unit_suffix,
                       QWidget *parent = nullptr, double display_scale = 1.0,
                       int display_decimals = 0)
        : QSlider(Qt::Horizontal, parent),
          value_default(value_default),
          description(description),
          unit_suffix(unit_suffix),
          display_scale(display_scale),
          display_decimals(display_decimals)
    {
        setRange(value_minimum, value_maximum);
        setValue(this->value_default);
        connect(this, &QSlider::valueChanged, this, [this]
        {
            updateToolTip();
        });
        updateToolTip();
    }

    // Update tooltip units without changing any slider value or firing signals.
    void setDescription(const QString &text)
    {
        this->description = text;
        updateToolTip();
    }

    void setDisplayPresentation(const QString &suffix, double scale, int decimals)
    {
        this->unit_suffix = suffix;
        this->display_scale = scale;
        this->display_decimals = decimals;
        updateToolTip();
    }

    void setConfiguration(int value_minimum, int value_maximum, int value_default,
                          int value, const QString &description, const QString &unit_suffix,
                          double display_scale = 1.0, int display_decimals = 0)
    {
        const QSignalBlocker blocker(this);
        this->value_default = value_default;
        this->description = description;
        this->unit_suffix = unit_suffix;
        this->display_scale = display_scale;
        this->display_decimals = display_decimals;
        setRange(value_minimum, value_maximum);
        setValue(value);
        updateToolTip();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton)
        {
            setValue(this->value_default);
            event->accept();
            return;
        }

        QSlider::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        setValue(this->value_default);
        event->accept();
    }

private:
    QString displayValue(int raw_value) const
    {
        return QString::number(raw_value * this->display_scale, 'f', this->display_decimals);
    }

    void updateToolTip()
    {
        setToolTip(QCoreApplication::translate("MapSymbologySlider",
            "%1\nCurrent: %2%3\nRange: %4%3 to %5%3\nDefault: %6%3\nRight-click to reset.")
            .arg(this->description)
            .arg(displayValue(value()))
            .arg(this->unit_suffix)
            .arg(displayValue(minimum()))
            .arg(displayValue(maximum()))
            .arg(displayValue(this->value_default)));
    }

    int value_default = 0;
    QString description;
    QString unit_suffix;
    double display_scale = 1.0;
    int display_decimals = 0;
};

#endif // MAP_SYMBOLOGY_SLIDER_H
