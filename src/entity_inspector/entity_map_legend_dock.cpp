#include "config/live_ui_text.h"
#include "entity_inspector/entity_map_legend_dock.h"

#include "config/gui_configuration.h"
#include "config/unit_aware_view.h"
#include "config/unit_profile_manager.h"
#include "entity_inspector/map_legend_unit_presentation.h"
#include "network/network_symbology_rendering.h"
#ifdef Q_OS_WASM
#include "widgets/wasm_popup_menu.h"
#endif

#include <array>
#include <cmath>
#include <functional>

#include <QAction>
#include <QColor>
#include <QFontMetricsF>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QWidgetAction>

namespace
{
const std::array<NetworkSymbologyPalette, NetworkSymbologyPaletteCount> palette_choices = {{
    NetworkSymbologyPalette::Viridis,
    NetworkSymbologyPalette::Cividis,
    NetworkSymbologyPalette::Plasma,
    NetworkSymbologyPalette::Inferno,
    NetworkSymbologyPalette::Magma,
    NetworkSymbologyPalette::Batlow,
    NetworkSymbologyPalette::Turbo,
    NetworkSymbologyPalette::CoolWarm,
    NetworkSymbologyPalette::RedBlue
}};

QString legendGroupTitle(const QString &scope, const QString &metric, const QString &unit)
{
    if (unit.isEmpty())
        return QStringLiteral("%1 · %2").arg(scope, metric);

    return QStringLiteral("%1 · %2 [%3]").arg(scope, metric, unit);
}

QIcon palettePreviewIcon(NetworkSymbologyPalette palette)
{
    QPixmap pixmap(112, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    QLinearGradient gradient(0.0, 0.0, pixmap.width(), 0.0);
    const std::array<QColor, NetworkSymbologyRampColorCount> &colors =
        networkSymbologyPaletteColors(palette);
    for (int index = 0; index < int(colors.size()); ++index)
    {
        gradient.setColorAt(
            double(index) / double(colors.size() - 1), colors.at(index));
    }
    painter.fillRect(pixmap.rect(), gradient);
    painter.setPen(QPen(QColor(0, 0, 0, 120), 1.0));
    painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    return QIcon(pixmap);
}

using LegendDescriptor = MapLegendMetrics::Descriptor;

MapLegendMetrics::RoughnessStyle roughnessStyle(HydraulicHeadlossFormula formula)
{
    switch (formula)
    {
    case HydraulicHeadlossFormula::HazenWilliams:
        return MapLegendMetrics::RoughnessStyle::HazenWilliams;
    case HydraulicHeadlossFormula::DarcyWeisbach:
        return MapLegendMetrics::RoughnessStyle::DarcyWeisbach;
    case HydraulicHeadlossFormula::ChezyManning:
        return MapLegendMetrics::RoughnessStyle::ChezyManning;
    }
    return MapLegendMetrics::RoughnessStyle::HazenWilliams;
}
}

class MapSymbologyHoverSwatch final : public QWidget
{
public:
    explicit MapSymbologyHoverSwatch(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFixedSize(96, 52);
        hide();
    }

    void setDisplay(const QColor &color, const QString &text)
    {
        this->color = color;
        this->text = text;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
        painter.setBrush(this->color);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4.0, 4.0);

        QFont value_font = painter.font();
        value_font.setBold(true);
        value_font.setPointSizeF(value_font.pointSizeF() + 1.0);
        painter.setFont(value_font);

        const QRect text_rect = rect().adjusted(4, 2, -4, -2);
        painter.setPen(QColor(0, 0, 0, 180));
        painter.drawText(text_rect.translated(1, 1), Qt::AlignCenter, this->text);
        painter.setPen(Qt::white);
        painter.drawText(text_rect, Qt::AlignCenter, this->text);
    }

private:
    QColor color;
    QString text;
};

class MapSymbologyRampWidget final : public QWidget
{
public:
    using PaletteChangedCallback = std::function<void(NetworkSymbologyPalette, bool)>;

    MapSymbologyRampWidget(QWidget *hover_parent, GroupBoxCollapsible *group,
                          const char *scope, const char *empty_title)
        : QWidget(group), group(group), scope(scope), empty_title(empty_title),
          hover_parent(hover_parent),
          hover_swatch(new MapSymbologyHoverSwatch(hover_parent))
    {
        setMouseTracking(true);
        LiveUiText::bind(this, [this] {
            refreshTitle();
            updateToolTip();
            update();
        });
        setFixedHeight(54);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    ~MapSymbologyRampWidget() override
    {
        delete this->hover_swatch.data();
    }

    QSize sizeHint() const override
    {
        return QSize(220, 54);
    }

    void setPaletteChangedCallback(const PaletteChangedCallback &callback)
    {
        this->palette_changed_callback = callback;
    }

    void setDefaultPaletteSelection(NetworkSymbologyPalette palette, bool flipped)
    {
        this->default_palette = palette;
        this->default_palette_flipped = flipped;
    }

    void setPaletteSelection(NetworkSymbologyPalette palette, bool flipped)
    {
        if (this->palette == palette && this->palette_flipped == flipped)
            return;

        this->palette = palette;
        this->palette_flipped = flipped;
        hideHoverSwatch();
        updateToolTip();
        update();
    }

    void clearMetric()
    {
        this->metric = nullptr;
        refreshTitle();
    }

    void setRange(const LegendDescriptor &metric, double minimum, double maximum, const QString &unit)
    {
        this->value_minimum = minimum;
        this->value_maximum = maximum;
        this->metric = &metric;
        this->unit = unit;
        refreshTitle();
        hideHoverSwatch();
        updateToolTip();
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF ramp_rect = rampRect();
        const QColor border_color = QWidget::palette().color(QPalette::Mid);
        const QColor text_color = QWidget::palette().color(QPalette::Text);
        const bool finite = std::isfinite(this->value_minimum) && std::isfinite(this->value_maximum);
        const bool uniform = finite && qFuzzyCompare(this->value_minimum + 1.0, this->value_maximum + 1.0);

        painter.setPen(QPen(border_color, 1.0));

        if (!finite)
        {
            painter.setBrush(QWidget::palette().brush(QPalette::AlternateBase));
            painter.drawRoundedRect(ramp_rect, 4.0, 4.0);
            painter.setPen(text_color);
            painter.drawText(rect(), Qt::AlignCenter, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "No finite values")));
            return;
        }

        if (uniform)
        {
            painter.setBrush(rampColor(0.5));
        }
        else
        {
            QLinearGradient gradient(ramp_rect.topLeft(), ramp_rect.topRight());
            for (int index = 0; index < NetworkSymbologyRampColorCount; ++index)
            {
                const double fraction = double(index) / double(NetworkSymbologyRampColorCount - 1);
                gradient.setColorAt(fraction, rampColor(fraction));
            }
            painter.setBrush(gradient);
        }

        painter.drawRoundedRect(ramp_rect, 4.0, 4.0);
        drawTicksAndLabels(painter, ramp_rect, text_color, uniform);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QRectF ramp_rect = rampRect();
        const bool finite = std::isfinite(this->value_minimum) && std::isfinite(this->value_maximum);

        if (finite && ramp_rect.contains(event->position()))
        {
            const double fraction = qBound(0.0, (event->position().x() - ramp_rect.left()) / ramp_rect.width(), 1.0);
            const bool uniform = qFuzzyCompare(this->value_minimum + 1.0, this->value_maximum + 1.0);
            showHoverSwatch(fraction, uniform);
            setCursor(Qt::CrossCursor);
        }
        else
        {
            hideHoverSwatch();
            unsetCursor();
        }

        QWidget::mouseMoveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rampRect().contains(event->position()))
        {
            hideHoverSwatch();
            showPaletteMenu();
            event->accept();
            return;
        }

        QWidget::mousePressEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        hideHoverSwatch();
        unsetCursor();
        QWidget::leaveEvent(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        hideHoverSwatch();
        QWidget::hideEvent(event);
    }

private:
    double value_minimum = 0.0;
    double value_maximum = 0.0;
    GroupBoxCollapsible *group;
    const char *scope;
    const char *empty_title;
    const LegendDescriptor *metric = nullptr; // Refers to the constexpr descriptor table.
    QString unit;

    void refreshTitle()
    {
        this->group->setTitle(this->metric == nullptr ? LiveUiText::translate(this->empty_title)
            : legendGroupTitle(QCoreApplication::translate("MapBgfxLegendContent", this->scope),
                               MapLegendUnitPresentation::label(*this->metric), this->unit));
    }

    QWidget *hover_parent = nullptr;
    QPointer<MapSymbologyHoverSwatch> hover_swatch;
    NetworkSymbologyPalette palette = NetworkSymbologyPalette::Viridis;
    bool palette_flipped = false;
    NetworkSymbologyPalette default_palette = NetworkSymbologyPalette::Viridis;
    bool default_palette_flipped = false;
    PaletteChangedCallback palette_changed_callback;

    QRectF rampRect() const
    {
        const QRectF available_rect = QRectF(rect()).adjusted(7.0, 4.0, -7.0, -4.0);
        return QRectF(available_rect.left(), available_rect.top(), available_rect.width(), 16.0);
    }

    QColor rampColor(double fraction) const
    {
        return networkSymbologyInterpolatedRampColor(
            fraction, this->palette, this->palette_flipped);
    }

    void showPaletteMenu()
    {
#ifdef Q_OS_WASM
        WasmPopupMenu *menu = new WasmPopupMenu(this);
        menu->setDeleteOnClose(true);

        const QFontMetrics menu_font_metrics(menu->font());
        int palette_label_width = 0;
        for (const NetworkSymbologyPalette palette_choice : palette_choices)
        {
            palette_label_width = qMax(
                palette_label_width,
                menu_font_metrics.horizontalAdvance(
                    networkSymbologyPaletteName(palette_choice)));
        }
        palette_label_width += 8;

        constexpr int preview_width = 124;
        constexpr int preview_height = 36;

        {
            QLabel *section = menu->addSection(QString());
            LiveUiText::bind(menu, [section] { section->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Sequential"))); });
        }

        for (const NetworkSymbologyPalette palette_choice : palette_choices)
        {
            if (palette_choice == NetworkSymbologyPalette::CoolWarm)
            {
                QLabel *section = menu->addSection(QString());
                LiveUiText::bind(menu, [section] { section->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Diverging"))); });
            }

            QWidget *row = new QWidget(menu);
            QHBoxLayout *row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(6, 3, 6, 3);
            row_layout->setSpacing(8);

            QLabel *palette_label = new QLabel(
                networkSymbologyPaletteName(palette_choice), row);
            palette_label->setFixedWidth(palette_label_width);
            palette_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

            QPushButton *palette_button = new QPushButton(row);
            palette_button->setIcon(palettePreviewIcon(palette_choice));
            palette_button->setIconSize(QSize(112, 16));
            palette_button->setCheckable(true);
            palette_button->setChecked(this->palette == palette_choice);
            palette_button->setFixedSize(preview_width, preview_height);
            LiveUiText::bind(palette_button, [palette_button, palette_choice] {
                palette_button->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Use %1 colors")).arg(networkSymbologyPaletteName(palette_choice)));
            });

            QPushButton *flip_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Flip"), row);
            flip_button->setCheckable(true);
            flip_button->setChecked(
                this->palette == palette_choice && this->palette_flipped);
            flip_button->setFixedHeight(preview_height);
            LiveUiText::bind(flip_button, [flip_button] { flip_button->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Flip color direction"))); });

            row_layout->addWidget(palette_label);
            row_layout->addStretch(1);
            row_layout->addWidget(palette_button);
            row_layout->addWidget(flip_button);
            menu->addWidget(row);

            connect(palette_button, &QPushButton::clicked, menu,
                    [this, palette_choice, menu]
            {
                selectPalette(palette_choice, false);
                menu->closeAll();
            });
            connect(flip_button, &QPushButton::clicked, menu,
                    [this, palette_choice, menu]
            {
                const bool flipped = this->palette == palette_choice
                    ? !this->palette_flipped
                    : true;
                selectPalette(palette_choice, flipped);
                menu->closeAll();
            });
        }

        menu->addSeparator();
        QPushButton *reset_button = menu->addAction(
            QString(),
            [this]
            {
                selectPalette(this->default_palette, this->default_palette_flipped);
            });
        LiveUiText::bind(reset_button, [reset_button] {
            reset_button->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset")));
            reset_button->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset colors to the default palette")));
        });

        const QPoint popup_position = mapToGlobal(
            QPoint(0, qRound(rampRect().bottom()) + 4));
        menu->popup(popup_position);
#else
        QMenu *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);

        const QFontMetrics menu_font_metrics(menu->font());
        int palette_label_width = 0;
        for (const NetworkSymbologyPalette palette_choice : palette_choices)
        {
            palette_label_width = qMax(
                palette_label_width,
                menu_font_metrics.horizontalAdvance(
                    networkSymbologyPaletteName(palette_choice)));
        }
        palette_label_width += 8;

        constexpr int preview_width = 124;
        constexpr int preview_height = 26;

        {
            QAction *section = menu->addSection(QString());
            LiveUiText::bind(menu, [section] { section->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Sequential"))); });
        }

        for (const NetworkSymbologyPalette palette_choice : palette_choices)
        {
            if (palette_choice == NetworkSymbologyPalette::CoolWarm)
            {
                QAction *section = menu->addSection(QString());
                LiveUiText::bind(menu, [section] { section->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Diverging"))); });
            }
            QWidget *row = new QWidget(menu);
            QHBoxLayout *row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(6, 3, 6, 3);
            row_layout->setSpacing(8);

            QLabel *palette_label = new QLabel(
                networkSymbologyPaletteName(palette_choice), row);
            palette_label->setFixedWidth(palette_label_width);
            palette_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

            QPushButton *palette_button = new QPushButton(row);
            palette_button->setIcon(palettePreviewIcon(palette_choice));
            palette_button->setIconSize(QSize(112, 16));
            palette_button->setCheckable(true);
            palette_button->setChecked(this->palette == palette_choice);
            palette_button->setFixedSize(preview_width, preview_height);
            LiveUiText::bind(palette_button, [palette_button, palette_choice] {
                palette_button->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Use %1 colors")).arg(networkSymbologyPaletteName(palette_choice)));
            });

            QPushButton *flip_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Flip"), row);
            flip_button->setCheckable(true);
            flip_button->setChecked(
                this->palette == palette_choice && this->palette_flipped);
            flip_button->setFixedHeight(preview_height);
            LiveUiText::bind(flip_button, [flip_button] { flip_button->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Flip color direction"))); });

            row_layout->addWidget(palette_label);
            row_layout->addStretch(1);
            row_layout->addWidget(palette_button);
            row_layout->addWidget(flip_button);

            QWidgetAction *row_action = new QWidgetAction(menu);
            row_action->setDefaultWidget(row);
            menu->addAction(row_action);

            connect(palette_button, &QPushButton::clicked, menu,
                    [this, palette_choice, menu]
            {
                selectPalette(palette_choice, false);
                menu->close();
            });
            connect(flip_button, &QPushButton::clicked, menu,
                    [this, palette_choice, menu]
            {
                const bool flipped = this->palette == palette_choice
                    ? !this->palette_flipped
                    : true;
                selectPalette(palette_choice, flipped);
                menu->close();
            });
        }

        menu->addSeparator();
        QAction *reset_action = menu->addAction(QString());
        LiveUiText::bind(menu, [reset_action] {
            reset_action->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset")));
            reset_action->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset colors to the default palette")));
        });
        connect(reset_action, &QAction::triggered, menu, [this, menu]
        {
            selectPalette(this->default_palette, this->default_palette_flipped);
            menu->close();
        });

        const QPoint popup_position = mapToGlobal(
            QPoint(0, qRound(rampRect().bottom()) + 4));
        menu->popup(popup_position);
#endif
    }

    void selectPalette(NetworkSymbologyPalette palette, bool flipped)
    {
        setPaletteSelection(palette, flipped);
        if (this->palette_changed_callback)
            this->palette_changed_callback(palette, flipped);
    }

    void updateToolTip()
    {
        const QString minimum_text = formatValue(this->value_minimum);
        const QString maximum_text = formatValue(this->value_maximum);
        const QString unit_suffix = this->unit.isEmpty()
            ? QString()
            : QStringLiteral(" %1").arg(this->unit);
        const QString palette_text = this->palette_flipped
            ? LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "%1 · flipped")).arg(networkSymbologyPaletteName(this->palette))
            : networkSymbologyPaletteName(this->palette);
        setToolTip(
            LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Minimum: %1%3\nMaximum: %2%3\nColors: %4\nClick to choose colors"))
                .arg(minimum_text, maximum_text, unit_suffix, palette_text));
    }

    void drawTicksAndLabels(QPainter &painter, const QRectF &ramp_rect, const QColor &text_color, bool uniform) const
    {
        const qreal tick_top = ramp_rect.bottom() + 2.0;
        const qreal tick_bottom = tick_top + 4.0;

        painter.setPen(text_color);
        QFont label_font = painter.font();
        label_font.setPointSizeF(qMax(6.0, label_font.pointSizeF() - 3.0));
        painter.setFont(label_font);

        const int label_count = labelCountForWidth(label_font, ramp_rect.width());
        for (int index = 0; index < label_count; ++index)
        {
            const double fraction = static_cast<double>(index) / static_cast<double>(label_count - 1);
            const qreal x = ramp_rect.left() + (ramp_rect.width() * fraction);
            painter.drawLine(QPointF(x, tick_top), QPointF(x, tick_bottom));
        }

        const QRectF label_area(ramp_rect.left(), tick_bottom + 2.0, ramp_rect.width(), height() - tick_bottom - 4.0);

        if (uniform)
        {
            painter.drawText(label_area, Qt::AlignHCenter | Qt::AlignTop, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "%1 · uniform")).arg(formatValue(this->value_minimum)));
            return;
        }

        const QFontMetricsF font_metrics(label_font);
        for (int index = 0; index < label_count; ++index)
        {
            const double fraction = static_cast<double>(index) / static_cast<double>(label_count - 1);
            const double value = this->value_minimum + ((this->value_maximum - this->value_minimum) * fraction);
            const QString text = formatValue(value);
            const qreal text_width = font_metrics.horizontalAdvance(text) + 2.0;
            const qreal label_center_x = ramp_rect.left() + (ramp_rect.width() * fraction);
            qreal label_left = label_center_x - (text_width * 0.5);
            Qt::Alignment alignment = Qt::AlignHCenter | Qt::AlignTop;

            if (index == 0)
            {
                label_left = ramp_rect.left();
                alignment = Qt::AlignLeft | Qt::AlignTop;
            }
            else if (index + 1 == label_count)
            {
                label_left = ramp_rect.right() - text_width;
                alignment = Qt::AlignRight | Qt::AlignTop;
            }

            const QRectF label_rect(label_left, label_area.top(), text_width, label_area.height());
            painter.drawText(label_rect, alignment, text);
        }
    }

    int labelCountForWidth(const QFont &font, qreal ramp_width) const
    {
        if (!std::isfinite(this->value_minimum) || !std::isfinite(this->value_maximum)
            || qFuzzyCompare(this->value_minimum + 1.0, this->value_maximum + 1.0))
        {
            return 7;
        }

        const QFontMetricsF font_metrics(font);
        const qreal minimum_gap = qMax(6.0, font_metrics.horizontalAdvance(QStringLiteral("  ")));
        constexpr std::array<int, 3> label_counts = {{7, 5, 3}};

        for (const int label_count : label_counts)
        {
            qreal previous_right = -minimum_gap;
            bool fits = true;

            for (int index = 0; index < label_count; ++index)
            {
                const double fraction = static_cast<double>(index) / static_cast<double>(label_count - 1);
                const double value = this->value_minimum + ((this->value_maximum - this->value_minimum) * fraction);
                const qreal text_width = font_metrics.horizontalAdvance(formatValue(value)) + 2.0;
                const qreal label_center_x = ramp_width * fraction;
                qreal label_left = label_center_x - (text_width * 0.5);
                qreal label_right = label_center_x + (text_width * 0.5);

                if (index == 0)
                {
                    label_left = 0.0;
                    label_right = text_width;
                }
                else if (index + 1 == label_count)
                {
                    label_left = ramp_width - text_width;
                    label_right = ramp_width;
                }

                if (label_left < 0.0 || label_right > ramp_width
                    || (index > 0 && label_left - previous_right < minimum_gap))
                {
                    fits = false;
                    break;
                }

                previous_right = label_right;
            }

            if (fits)
                return label_count;
        }

        return 3;
    }

    void showHoverSwatch(double fraction, bool uniform)
    {
        if (!this->hover_parent || !this->hover_swatch)
            return;

        const QRectF ramp_rect = rampRect();
        const qreal local_x = ramp_rect.left() + (ramp_rect.width() * fraction);
        const QPoint anchor = mapTo(this->hover_parent, QPoint(qRound(local_x), qRound(ramp_rect.top())));

        int swatch_x = anchor.x() - (this->hover_swatch->width() / 2);
        int swatch_y = anchor.y() - this->hover_swatch->height() - 4;
        swatch_x = qBound(2, swatch_x, qMax(2, this->hover_parent->width() - this->hover_swatch->width() - 2));

        if (swatch_y < 2)
            swatch_y = anchor.y() + qRound(ramp_rect.height()) + 4;

        const double value = uniform ? this->value_minimum : this->value_minimum + ((this->value_maximum - this->value_minimum) * fraction);
        this->hover_swatch->setDisplay(rampColor(uniform ? 0.5 : fraction), formatValue(value));
        this->hover_swatch->move(swatch_x, swatch_y);
        this->hover_swatch->show();
        this->hover_swatch->raise();
    }

    void hideHoverSwatch()
    {
        if (this->hover_swatch)
            this->hover_swatch->hide();
    }

    QString formatValue(double value) const
    {
        if (!std::isfinite(value))
            return QStringLiteral("—");

        const QLocale locale;
        const double absolute_value = std::abs(value);
        if (absolute_value == 0.0)
            return locale.toString(0.0, 'f', 2);

        const int decimal_exponent = static_cast<int>(std::floor(std::log10(absolute_value)));
        if (decimal_exponent >= 3 || decimal_exponent <= -3)
            return locale.toString(value, 'e', 2);

        const int decimal_places = qMax(0, 2 - decimal_exponent);
        const double decimal_factor = std::pow(10.0, decimal_places);
        const double rounded_value = std::round(value * decimal_factor) / decimal_factor;
        if (std::abs(rounded_value) >= 1000.0)
            return locale.toString(value, 'e', 2);

        return locale.toString(value, 'f', decimal_places);
    }
};

EntityMapLegendDock::EntityMapLegendDock(HydraulicData *hydraulic_data, QWidget *parent)
    : QDockWidget(parent),
      hydraulic_data(hydraulic_data)
{
    LiveUiText::bind(this, [this] { setWindowTitle(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Map Symbology Legend"))); });
    setMinimumWidth(Sizes::SidebarRightWidth);
    resize(Sizes::SidebarRightWidth, height());
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    this->content = new QWidget(this);
    this->content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    this->layout = new QVBoxLayout(this->content);
    this->layout->setContentsMargins(5, 5, 5, 5);
    this->layout->setSpacing(5);
    setWidget(this->content);

    addGroupNode();
    addGroupLink();
    addGroupHeatmap();

    this->legend_node->setDefaultPaletteSelection(
        NetworkSymbologyDefaultNodePalette, false);
    this->legend_link->setDefaultPaletteSelection(
        NetworkSymbologyDefaultLinkPalette, false);
    this->legend_heat->setDefaultPaletteSelection(
        NetworkSymbologyDefaultHeatmapPalette, false);

    const GuiSymbologyPaletteConfiguration &palette_configuration =
        guiConfiguration().symbology_palettes;
    setNodePalette(
        palette_configuration.node_palette, palette_configuration.node_palette_flipped);
    setLinkPalette(
        palette_configuration.link_palette, palette_configuration.link_palette_flipped);
    setHeatmapPalette(
        palette_configuration.heatmap_palette, palette_configuration.heatmap_palette_flipped);

    this->legend_node->setPaletteChangedCallback(
        [this](NetworkSymbologyPalette palette, bool flipped)
    {
        this->node_palette = palette;
        this->node_palette_flipped = flipped;
        emit signalNodePaletteSelected(palette, flipped);
    });
    this->legend_link->setPaletteChangedCallback(
        [this](NetworkSymbologyPalette palette, bool flipped)
    {
        this->link_palette = palette;
        this->link_palette_flipped = flipped;
        emit signalLinkPaletteSelected(palette, flipped);
    });
    this->legend_heat->setPaletteChangedCallback(
        [this](NetworkSymbologyPalette palette, bool flipped)
    {
        this->heatmap_palette = palette;
        this->heatmap_palette_flipped = flipped;
        emit signalHeatmapPaletteSelected(palette, flipped);
    });

    this->group_node->setCollapsed(true);
    this->group_link->setCollapsed(true);
    this->group_heat->setCollapsed(true);

    const std::array<GroupBoxCollapsible *, 3> groups = {{this->group_node, this->group_link, this->group_heat}};
    for (GroupBoxCollapsible *group : groups)
    {
        connect(group, &GroupBoxCollapsible::signalCollapsed, this, [this](GroupBoxCollapsible *)
        {
            scheduleDockHeightUpdate();
        });
        connect(group, &GroupBoxCollapsible::signalExpanded, this, [this](GroupBoxCollapsible *)
        {
            scheduleDockHeightUpdate();
        });
    }

    connect(this, &QDockWidget::topLevelChanged, this, [this](bool)
    {
        scheduleDockHeightUpdate();
    });
    connect(this, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea)
    {
        scheduleDockHeightUpdate();
    });

    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]
    {
        updateNodeLegend();
        updateLinkLegend();
        updateHeatmapLegend();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this,
        [this](InfrastructureEntity, const QUuid &)
    {
        updateNodeLegend();
        updateHeatmapLegend();
    });
    connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, this,
        [this](InfrastructureEntity, const QUuid &)
    {
        updateLinkLegend();
    });
    connect(this->hydraulic_data, &HydraulicData::signalWaterQualitySimulationResultTimelineChanged,
        this, [this](bool)
    {
        if (this->visual_node == VisualNode::WaterAge)
            updateNodeLegend();
        if (this->visual_link == VisualLink::WaterAge)
            updateLinkLegend();
        if (this->visual_heatmap == VisualHeatmap::WaterAge)
            updateHeatmapLegend();
    });
    connect(this->hydraulic_data, &HydraulicData::signalCurrentSimulationResultChanged,
        this, [this](int)
    {
        if (this->visual_node == VisualNode::WaterAge)
            updateNodeLegend();
        if (this->visual_link == VisualLink::WaterAge)
            updateLinkLegend();
        if (this->visual_heatmap == VisualHeatmap::WaterAge)
            updateHeatmapLegend();
    });

    scheduleDockHeightUpdate();
    new UnitAwareView(this, UnitProfileManager::instance().presentation(), [this] {
        updateNodeLegend();
        updateLinkLegend();
        updateHeatmapLegend();
    });
}

int EntityMapLegendDock::dockHeightPreferred() const
{
    return this->dock_height_preferred;
}

void EntityMapLegendDock::showMapLegendNode(VisualNode visual_node)
{
    this->visual_node = visual_node;
    setVisibility();

    if (visual_node == VisualNode::None)
    {
        this->legend_node->clearMetric();
        this->group_node->setCollapsed(true);
        scheduleDockHeightUpdate();
        return;
    }

    updateNodeLegend();
    this->group_node->setCollapsed(false);
    scheduleDockHeightUpdate();
}

void EntityMapLegendDock::showMapLegendLink(VisualLink visual_link)
{
    this->visual_link = visual_link;
    setVisibility();

    if (visual_link == VisualLink::None)
    {
        this->legend_link->clearMetric();
        this->group_link->setCollapsed(true);
        scheduleDockHeightUpdate();
        return;
    }

    updateLinkLegend();
    this->group_link->setCollapsed(false);
    scheduleDockHeightUpdate();
}

void EntityMapLegendDock::showMapLegendHeatmap(VisualHeatmap visual_heatmap)
{
    this->visual_heatmap = visual_heatmap;
    setVisibility();

    if (visual_heatmap == VisualHeatmap::None)
    {
        this->legend_heat->clearMetric();
        this->group_heat->setCollapsed(true);
        scheduleDockHeightUpdate();
        return;
    }

    updateHeatmapLegend();
    this->group_heat->setCollapsed(false);
    scheduleDockHeightUpdate();
}

void EntityMapLegendDock::setNodePalette(NetworkSymbologyPalette palette, bool flipped)
{
    this->node_palette = palette;
    this->node_palette_flipped = flipped;
    this->legend_node->setPaletteSelection(palette, flipped);
}

void EntityMapLegendDock::setLinkPalette(NetworkSymbologyPalette palette, bool flipped)
{
    this->link_palette = palette;
    this->link_palette_flipped = flipped;
    this->legend_link->setPaletteSelection(palette, flipped);
}

void EntityMapLegendDock::setHeatmapPalette(NetworkSymbologyPalette palette, bool flipped)
{
    this->heatmap_palette = palette;
    this->heatmap_palette_flipped = flipped;
    this->legend_heat->setPaletteSelection(palette, flipped);
}

void EntityMapLegendDock::setMapMonitorActive(bool active)
{
    this->map_monitor_active = active;
    setVisibility();
}

void EntityMapLegendDock::setVisibility()
{
    const bool has_visible_legend = this->visual_link != VisualLink::None ||
                                    this->visual_node != VisualNode::None ||
                                    this->visual_heatmap != VisualHeatmap::None;
    setVisible(this->map_monitor_active && has_visible_legend);

    if (isVisible())
        scheduleDockHeightUpdate();
}

void EntityMapLegendDock::addGroupNode()
{
    this->group_node = new GroupBoxCollapsible(this);
    QVBoxLayout *group_layout = new QVBoxLayout(this->group_node);
    group_layout->setContentsMargins(6, 5, 6, 6);
    group_layout->setSpacing(0);

    this->legend_node = new MapSymbologyRampWidget(this, this->group_node, "Node", QT_TRANSLATE_NOOP("LiveUiText", "Node Legend"));
    group_layout->addWidget(this->legend_node);
    this->layout->addWidget(this->group_node);
}

void EntityMapLegendDock::addGroupLink()
{
    this->group_link = new GroupBoxCollapsible(this);
    QVBoxLayout *group_layout = new QVBoxLayout(this->group_link);
    group_layout->setContentsMargins(6, 5, 6, 6);
    group_layout->setSpacing(0);

    this->legend_link = new MapSymbologyRampWidget(this, this->group_link, "Link", QT_TRANSLATE_NOOP("LiveUiText", "Link Legend"));
    group_layout->addWidget(this->legend_link);
    this->layout->addWidget(this->group_link);
}

void EntityMapLegendDock::addGroupHeatmap()
{
    this->group_heat = new GroupBoxCollapsible(this);
    QVBoxLayout *group_layout = new QVBoxLayout(this->group_heat);
    group_layout->setContentsMargins(6, 5, 6, 6);
    group_layout->setSpacing(0);

    this->legend_heat = new MapSymbologyRampWidget(this, this->group_heat, "Heatmap", QT_TRANSLATE_NOOP("LiveUiText", "Heatmap Overlay"));
    group_layout->addWidget(this->legend_heat);
    this->layout->addWidget(this->group_heat);
}

void EntityMapLegendDock::scheduleDockHeightUpdate()
{
    QTimer::singleShot(0, this, [this]()
    {
        updateDockHeight();
    });
}

void EntityMapLegendDock::updateDockHeight()
{
    if (!this->content || !this->layout)
        return;

    this->content->setMinimumHeight(0);
    this->content->setMaximumHeight(QWIDGETSIZE_MAX);

    this->layout->invalidate();
    this->layout->activate();
    const int content_height = qMax(this->layout->minimumSize().height(), this->layout->totalSizeHint().height());
    this->content->setFixedHeight(content_height);
    this->content->updateGeometry();

    if (QLayout *dock_layout = QDockWidget::layout())
    {
        dock_layout->invalidate();
        dock_layout->activate();
    }

    const int desired_height = qMax(content_height, QDockWidget::sizeHint().height());
    if (this->dock_height_preferred == desired_height)
        return;

    this->dock_height_preferred = desired_height;
    updateGeometry();
    emit signalDockHeightPreferredChanged(this->dock_height_preferred);
}

void EntityMapLegendDock::updateNodeLegend()
{
    this->legend_node->setPaletteSelection(
        this->node_palette, this->node_palette_flipped);
    if (this->visual_node == VisualNode::None) return;

    const LegendDescriptor &descriptor = MapLegendMetrics::node(this->visual_node);
    NetworkSymbologySettings settings;
    settings.visual_node = this->visual_node;
    const NetworkSymbologyRanges ranges = this->hydraulic_data->symbologyRanges(settings);
    const MapLegendUnitPresentation::Range displayed = MapLegendUnitPresentation::range(
        UnitProfileManager::instance().presentation(), descriptor,
        ranges.node_minimum, ranges.node_maximum);
    this->legend_node->setRange(descriptor, displayed.minimum, displayed.maximum, displayed.unit);
}

void EntityMapLegendDock::updateLinkLegend()
{
    this->legend_link->setPaletteSelection(
        this->link_palette, this->link_palette_flipped);
    if (this->visual_link == VisualLink::None) return;

    const MapLegendMetrics::RoughnessStyle roughness = roughnessStyle(
        this->hydraulic_data->networkHydraulic().options_hydraulic.headloss_formula);
    const LegendDescriptor &descriptor = MapLegendMetrics::link(this->visual_link, roughness);
    NetworkSymbologySettings settings;
    settings.visual_link = this->visual_link;
    const NetworkSymbologyRanges ranges = this->hydraulic_data->symbologyRanges(settings);
    const MapLegendUnitPresentation::Range displayed = MapLegendUnitPresentation::range(
        UnitProfileManager::instance().presentation(), descriptor,
        ranges.link_minimum, ranges.link_maximum);
    this->legend_link->setRange(descriptor, displayed.minimum, displayed.maximum, displayed.unit);
}

void EntityMapLegendDock::updateHeatmapLegend()
{
    this->legend_heat->setPaletteSelection(
        this->heatmap_palette, this->heatmap_palette_flipped);
    if (this->visual_heatmap == VisualHeatmap::None) return;

    const LegendDescriptor &descriptor = MapLegendMetrics::heatmap(this->visual_heatmap);
    NetworkSymbologySettings settings;
    settings.visual_heatmap = this->visual_heatmap;
    const NetworkSymbologyRanges ranges = this->hydraulic_data->symbologyRanges(settings);
    const MapLegendUnitPresentation::Range displayed = MapLegendUnitPresentation::range(
        UnitProfileManager::instance().presentation(), descriptor,
        ranges.heatmap_minimum, ranges.heatmap_maximum);
    this->legend_heat->setRange(descriptor, displayed.minimum, displayed.maximum, displayed.unit);
}
