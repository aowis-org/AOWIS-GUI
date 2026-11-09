#pragma once

#include "common/_sizes.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QSize>
#include <QSizePolicy>
#include <QWidget>

// Actual content widget used by TopControlBar. Extracted so the same layout
// can be exercised in a headless Qt integration test rather than testing a
// separate mock geometry function.
class TopControlBarContent : public QWidget
{
public:
    explicit TopControlBarContent(QWidget *parent = nullptr)
        : QWidget(parent),
        left_group(new QWidget(this)),
        center_group(new QWidget(this)),
        right_group(new QWidget(this)),
        left_layout(new QHBoxLayout(this->left_group)),
        center_layout(new QHBoxLayout(this->center_group)),
        right_layout(new QHBoxLayout(this->right_group))
    {
        this->left_layout->setContentsMargins(0, 0, 0, 0);
        this->left_layout->setSpacing(10);
        this->center_layout->setContentsMargins(0, 0, 0, 0);
        this->center_layout->setSpacing(10);

        this->right_layout->setContentsMargins(0, 0, 0, 0);
        this->right_layout->setSpacing(10);

        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(Sizes::TopControlBarHeight);
    }

    QHBoxLayout *leftLayout() const
    {
        return this->left_layout;
    }

    QHBoxLayout *centerLayout() const
    {
        return this->center_layout;
    }

    QHBoxLayout *rightLayout() const
    {
        return this->right_layout;
    }

    QSize sizeHint() const override
    {
        const int width = this->left_group->sizeHint().width()
                          + this->center_group->sizeHint().width()
                          + this->right_group->sizeHint().width()
                          + 64;
        return QSize(width, Sizes::TopControlBarHeight);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        placeGroups();
    }

    bool event(QEvent *event) override
    {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::LayoutRequest ||
            event->type() == QEvent::LayoutDirectionChange ||
            event->type() == QEvent::LanguageChange)
            placeGroups();
        return handled;
    }

private:
    void placeGroups()
    {
        constexpr int side_margin = 10;
        constexpr int group_spacing = 14;

        const QSize left_size = this->left_group->sizeHint();
        const QSize center_size = this->center_group->sizeHint();
        const QSize right_size = this->right_group->sizeHint();
        const int left_y = qMax(0, (height() - left_size.height()) / 2);
        const int center_y = qMax(0, (height() - center_size.height()) / 2);
        const int right_y = qMax(0, (height() - right_size.height()) / 2);

        // The three groups are manually positioned rather than using a Qt
        // layout. Mirror their *physical* anchors when the application enters
        // Arabic RTL mode, while retaining the same controls and state.
        const bool right_to_left = this->layoutDirection() == Qt::RightToLeft;
        const int left_x = right_to_left
            ? width() - side_margin - left_size.width() : side_margin;
        const int right_x = right_to_left
            ? side_margin : width() - side_margin - right_size.width();
        const int centered_x = (width() - center_size.width()) / 2;
        const int minimum_center_x = right_to_left
            ? right_x + right_size.width() + group_spacing
            : left_x + left_size.width() + group_spacing;
        const int maximum_center_x = right_to_left
            ? left_x - group_spacing - center_size.width()
            : right_x - group_spacing - center_size.width();

        int center_x = centered_x;
        if (minimum_center_x <= maximum_center_x)
            center_x = qBound(minimum_center_x, centered_x, maximum_center_x);
        else
            center_x = minimum_center_x;

        this->left_group->setGeometry(left_x, left_y, left_size.width(), left_size.height());
        this->center_group->setGeometry(center_x, center_y, center_size.width(), center_size.height());
        this->right_group->setGeometry(right_x, right_y, right_size.width(), right_size.height());
    }

    QWidget *left_group = nullptr;
    QWidget *center_group = nullptr;
    QWidget *right_group = nullptr;
    QHBoxLayout *left_layout = nullptr;
    QHBoxLayout *center_layout = nullptr;
    QHBoxLayout *right_layout = nullptr;
};

