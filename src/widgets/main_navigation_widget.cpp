#include "widgets/main_navigation_widget.h"

#include <QCoreApplication>
#include <QColor>
#include <QEvent>
#include <QFocusEvent>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QStackedLayout>
#include <QToolTip>

#include <functional>

namespace
{
constexpr int navigation_width = 56;
constexpr int navigation_item_height = 56;
constexpr int navigation_icon_size = 40;
constexpr int navigation_item_margin = 4;
constexpr int navigation_corner_radius = 5;
constexpr int navigation_selection_marker_width = 3;

struct NavigationEntry
{
    int page_index = -1;
    QIcon icon;
    QString tool_tip;
    MainNavigationWidget::Placement placement = MainNavigationWidget::Placement::Top;
};
}

class MainNavigationBar : public QWidget
{
public:
    explicit MainNavigationBar(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedWidth(navigation_width);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
    }

    void addEntry(int page_index,
                  const QIcon &icon,
                  const QString &tool_tip,
                  MainNavigationWidget::Placement placement)
    {
        NavigationEntry entry;
        entry.page_index = page_index;
        entry.icon = icon;
        entry.tool_tip = tool_tip;
        entry.placement = placement;
        this->entries.append(entry);
        updateGeometry();
        update();
    }

    void setCurrentPageIndex(int page_index)
    {
        if (this->current_page_index == page_index)
            return;

        this->current_page_index = page_index;
        update();
    }

    void setPageActivationCallback(const std::function<void(int)> &callback)
    {
        this->page_activation_callback = callback;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // Keep the navigation rail visually separate from the page surface.
        // The selected item is painted with QPalette::Window below, so it
        // visually opens the rail into the active page.
        painter.fillRect(rect(), palette().color(QPalette::Base));

        const QColor divider = palette().color(QPalette::Mid);
        painter.setPen(divider);
        painter.drawLine(width() - 1, 0, width() - 1, height());

        for (int entry_index = 0; entry_index < this->entries.size(); ++entry_index)
        {
            const NavigationEntry &entry = this->entries.at(entry_index);
            const QRect item_rect = entryRect(entry_index);
            if (!item_rect.isValid())
                continue;

            const bool selected = entry.page_index == this->current_page_index;
            const bool hovered = entry_index == this->hovered_entry_index;
            const bool focused = hasFocus() && entry_index == this->focused_entry_index;

            const QRect background_rect = item_rect.adjusted(
                navigation_item_margin,
                navigation_item_margin,
                -navigation_item_margin,
                -navigation_item_margin);

            if (selected)
            {
                // Paint the active navigation cell as an attached tab rather than an
                // inset button. Filling through the right edge also covers the rail's
                // divider for this row, so the selected cell visually opens into the
                // active page.
                painter.setPen(Qt::NoPen);
                painter.setBrush(palette().color(QPalette::Window));
                painter.drawRect(item_rect);

                const QColor marker = palette().color(QPalette::Highlight);
                painter.setBrush(marker);
                painter.drawRoundedRect(
                    QRect(0,
                          item_rect.top() + navigation_item_margin,
                          navigation_selection_marker_width,
                          item_rect.height() - (2 * navigation_item_margin)),
                    navigation_selection_marker_width,
                    navigation_selection_marker_width);
            }
            else if (hovered)
            {
                QColor background = palette().color(QPalette::Highlight);
                background.setAlpha(24);
                painter.setPen(Qt::NoPen);
                painter.setBrush(background);
                painter.drawRoundedRect(background_rect,
                                        navigation_corner_radius,
                                        navigation_corner_radius);
            }

            const int icon_size = qMin(navigation_icon_size, qMax(24, item_rect.height() - 12));
            const QRect icon_rect(
                item_rect.center().x() - (icon_size / 2),
                item_rect.center().y() - (icon_size / 2),
                icon_size,
                icon_size);

            entry.icon.paint(&painter,
                             icon_rect,
                             Qt::AlignCenter,
                             hovered ? QIcon::Active : QIcon::Normal,
                             selected ? QIcon::On : QIcon::Off);

            if (focused)
            {
                QPen focus_pen(palette().color(QPalette::Highlight));
                focus_pen.setStyle(Qt::DotLine);
                painter.setPen(focus_pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(background_rect.adjusted(1, 1, -1, -1),
                                        navigation_corner_radius,
                                        navigation_corner_radius);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int entry_index = entryAt(event->position().toPoint());
        if (entry_index != this->hovered_entry_index)
        {
            this->hovered_entry_index = entry_index;
            if (entry_index >= 0)
                setCursor(Qt::PointingHandCursor);
            else
                unsetCursor();
            update();
        }

        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        this->hovered_entry_index = -1;
        unsetCursor();
        update();
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            QWidget::mousePressEvent(event);
            return;
        }

        const int entry_index = entryAt(event->position().toPoint());
        if (entry_index < 0)
        {
            QWidget::mousePressEvent(event);
            return;
        }

        this->focused_entry_index = entry_index;
        setFocus(Qt::MouseFocusReason);
        activateEntry(entry_index);
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (this->entries.isEmpty())
        {
            QWidget::keyPressEvent(event);
            return;
        }

        int entry_index = this->focused_entry_index;
        if (entry_index < 0)
            entry_index = entryIndexForPage(this->current_page_index);
        if (entry_index < 0)
            entry_index = 0;

        switch (event->key())
        {
        case Qt::Key_Up:
            entry_index = qMax(0, entry_index - 1);
            break;
        case Qt::Key_Down:
            entry_index = qMin(this->entries.size() - 1, entry_index + 1);
            break;
        case Qt::Key_Home:
            entry_index = 0;
            break;
        case Qt::Key_End:
            entry_index = this->entries.size() - 1;
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            activateEntry(entry_index);
            event->accept();
            return;
        default:
            QWidget::keyPressEvent(event);
            return;
        }

        this->focused_entry_index = entry_index;
        ensureHoveredEntryMatchesFocus();
        update();
        event->accept();
    }

    void focusInEvent(QFocusEvent *event) override
    {
        if (this->focused_entry_index < 0)
            this->focused_entry_index = entryIndexForPage(this->current_page_index);
        update();
        QWidget::focusInEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        update();
        QWidget::focusOutEvent(event);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip)
        {
            QHelpEvent *help_event = static_cast<QHelpEvent *>(event);
            const int entry_index = entryAt(help_event->pos());
            if (entry_index >= 0)
            {
                const NavigationEntry &entry = this->entries.at(entry_index);
                if (!entry.tool_tip.isEmpty())
                {
                    QToolTip::showText(help_event->globalPos(),
                                       QCoreApplication::translate("MainNavigationWidget", entry.tool_tip.toUtf8().constData()),
                                       this,
                                       entryRect(entry_index));
                    return true;
                }
            }

            QToolTip::hideText();
            event->ignore();
            return true;
        }

        return QWidget::event(event);
    }

private:
    QRect entryRect(int entry_index) const
    {
        if (entry_index < 0 || entry_index >= this->entries.size())
            return QRect();

        const int item_height = effectiveItemHeight();
        const NavigationEntry &entry = this->entries.at(entry_index);
        if (entry.placement == MainNavigationWidget::Placement::Top)
        {
            int top_position = 0;
            for (int index = 0; index < entry_index; ++index)
            {
                if (this->entries.at(index).placement == MainNavigationWidget::Placement::Top)
                    ++top_position;
            }

            return QRect(0,
                         top_position * item_height,
                         navigation_width,
                         item_height);
        }

        int bottom_count = 0;
        int bottom_position = 0;
        for (int index = 0; index < this->entries.size(); ++index)
        {
            if (this->entries.at(index).placement != MainNavigationWidget::Placement::Bottom)
                continue;

            if (index < entry_index)
                ++bottom_position;
            ++bottom_count;
        }

        const int bottom_group_top = height() - (bottom_count * item_height);
        return QRect(0,
                     bottom_group_top + (bottom_position * item_height),
                     navigation_width,
                     item_height);
    }

    int entryAt(const QPoint &position) const
    {
        for (int entry_index = 0; entry_index < this->entries.size(); ++entry_index)
        {
            if (entryRect(entry_index).contains(position))
                return entry_index;
        }

        return -1;
    }

    int entryIndexForPage(int page_index) const
    {
        for (int entry_index = 0; entry_index < this->entries.size(); ++entry_index)
        {
            if (this->entries.at(entry_index).page_index == page_index)
                return entry_index;
        }

        return -1;
    }

    int effectiveItemHeight() const
    {
        if (this->entries.isEmpty())
            return navigation_item_height;

        const int fitted_height = height() / this->entries.size();
        return qMax(32, qMin(navigation_item_height, fitted_height));
    }

    void activateEntry(int entry_index)
    {
        if (entry_index < 0 || entry_index >= this->entries.size())
            return;

        if (!this->page_activation_callback)
            return;

        this->page_activation_callback(this->entries.at(entry_index).page_index);
    }

    void ensureHoveredEntryMatchesFocus()
    {
        this->hovered_entry_index = -1;
        unsetCursor();
    }

    QList<NavigationEntry> entries;
    int current_page_index = -1;
    int hovered_entry_index = -1;
    int focused_entry_index = -1;
    std::function<void(int)> page_activation_callback;
};

MainNavigationWidget::MainNavigationWidget(QWidget *parent)
    : QWidget(parent)
{
    QHBoxLayout *root_layout = new QHBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    this->navigation_bar = new MainNavigationBar(this);
    this->page_host = new QWidget(this);
    this->page_layout = new QStackedLayout(this->page_host);
    this->page_layout->setContentsMargins(0, 0, 0, 0);
    this->page_layout->setSpacing(0);

    root_layout->addWidget(this->navigation_bar);
    root_layout->addWidget(this->page_host, 1);

    this->navigation_bar->setPageActivationCallback([this](int page_index)
    {
        setCurrentIndex(page_index);
    });
}

int MainNavigationWidget::addPage(QWidget *page,
                                  const QIcon &icon,
                                  const QString &tool_tip,
                                  Placement placement)
{
    if (!page)
        return -1;

    const int page_index = this->page_layout->addWidget(page);
    this->navigation_bar->addEntry(page_index, icon, tool_tip, placement);

    if (this->page_layout->currentIndex() == page_index)
        this->navigation_bar->setCurrentPageIndex(page_index);

    return page_index;
}

int MainNavigationWidget::count() const
{
    return this->page_layout->count();
}

int MainNavigationWidget::currentIndex() const
{
    return this->page_layout->currentIndex();
}

QWidget *MainNavigationWidget::currentWidget() const
{
    return this->page_layout->currentWidget();
}

void MainNavigationWidget::setNavigationBarVisible(bool visible)
{
    this->navigation_bar->setVisible(visible);
}

void MainNavigationWidget::setCurrentIndex(int index)
{
    if (index < 0 || index >= this->page_layout->count())
        return;

    const int previous_index = this->page_layout->currentIndex();
    this->page_layout->setCurrentIndex(index);
    this->navigation_bar->setCurrentPageIndex(index);

    if (previous_index != index)
        emit currentChanged(index);
}
