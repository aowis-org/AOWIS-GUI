#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QWidget>
#include <functional>
#include <utility>

// Run a presentation update on construction and on every Qt language change,
// including while the owning widget is on an inactive stacked page.
class LiveUiText final : public QObject
{
public:
    static void bind(QWidget *owner, std::function<void()> refresh)
    {
        new LiveUiText(owner, std::move(refresh));
    }

    static QString translate(const char *source)
    {
        return QCoreApplication::translate("LiveUiText", source);
    }

    template<typename Widget>
    static Widget *caption(const char *source, QWidget *parent)
    {
        Widget *widget = new Widget(parent);
        bind(widget, [widget, source] { widget->setText(translate(source)); });
        return widget;
    }

private:
    LiveUiText(QWidget *owner, std::function<void()> refresh)
        : QObject(owner), refresh_text(std::move(refresh))
    {
        owner->installEventFilter(this);
        this->refresh_text();
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::LanguageChange)
            this->refresh_text();
        return false;
    }

    std::function<void()> refresh_text;
};
