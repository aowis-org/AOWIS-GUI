#include "app/top_control_bar_content.h"
#include "config/gui_language.h"
#include "config/live_ui_text.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QWidget>

// The test deliberately uses the *real* TopControlBarContent widget from the
// production toolbar, not a second implementation of its positioning logic.
static bool require(bool condition, const QString &description)
{
    if (!condition)
        qCritical() << description;
    return condition;
}

int main(int argc, char **argv)
{
    QTemporaryDir settings_directory;
    if (!settings_directory.isValid())
        return 1;
    qputenv("XDG_DATA_HOME", settings_directory.path().toUtf8());
    qputenv("HOME", settings_directory.path().toUtf8());
    qputenv("APPDATA", settings_directory.path().toUtf8());
    QApplication app(argc, argv);
    GuiLanguage &language = GuiLanguage::instance();

    TopControlBarContent toolbar;
    toolbar.setFixedSize(1400, Sizes::TopControlBarHeight + 25);
    QLabel left_caption(QStringLiteral("Project"));
    QLineEdit active_draft;
    active_draft.setText(QStringLiteral("Unsaved network name"));
    QComboBox right_choice;
    right_choice.addItem(QStringLiteral("3D view"), 7);
    right_choice.addItem(QStringLiteral("2D view"), 8);
    right_choice.setCurrentIndex(1);
    QLabel translated_language;
    LiveUiText::bind(&translated_language, [&translated_language] {
        translated_language.setText(QCoreApplication::translate("TopControlBar", "Language"));
    });
    toolbar.leftLayout()->addWidget(&left_caption);
    toolbar.centerLayout()->addWidget(&active_draft);
    toolbar.rightLayout()->addWidget(&right_choice);
    toolbar.rightLayout()->addWidget(&translated_language);

    toolbar.show();
    app.processEvents();
    active_draft.setFocus();
    active_draft.setSelection(3, 5);
    int edits = 0;
    int selections = 0;
    QObject::connect(&active_draft, &QLineEdit::textEdited,
        [&edits](const QString &) { ++edits; });
    QObject::connect(&right_choice, &QComboBox::currentIndexChanged,
        [&selections](int) { ++selections; });

    const QString codes[] = {QStringLiteral("en"), QStringLiteral("fr"),
        QStringLiteral("ar"), QStringLiteral("de"), QStringLiteral("es"),
        QStringLiteral("en"), QStringLiteral("ar"), QStringLiteral("en")};
    const QString captions[] = {QStringLiteral("Language"), QStringLiteral("Langue"),
        QString::fromUtf8("اللغة"), QStringLiteral("Sprache"), QStringLiteral("Idioma"),
        QStringLiteral("Language"), QString::fromUtf8("اللغة"), QStringLiteral("Language")};
    for (int step = 0; step < 8; ++step)
    {
        const QString &code = codes[step];
        if (!require(language.select(code), code + QStringLiteral(": failed to select")))
            return 2;
        app.processEvents();
        const bool rtl = code == QStringLiteral("ar");
        const QWidget *left_group = left_caption.parentWidget();
        const QWidget *center_group = active_draft.parentWidget();
        const QWidget *right_group = right_choice.parentWidget();
        const QRect left = left_group->geometry();
        const QRect center = center_group->geometry();
        const QRect right = right_group->geometry();
        if (!require(toolbar.layoutDirection() == (rtl ? Qt::RightToLeft : Qt::LeftToRight),
                     code + QStringLiteral(": toolbar direction"))
            || !require(rtl ? left.right() == toolbar.width() - 11 : left.left() == 10,
                        code + QStringLiteral(": left group anchor"))
            || !require(rtl ? right.left() == 10 : right.right() == toolbar.width() - 11,
                        code + QStringLiteral(": right group anchor"))
            || !require(rtl ? right.right() + 14 < center.left() : left.right() + 14 < center.left(),
                        code + QStringLiteral(": left/center overlap"))
            || !require(rtl ? center.right() + 14 < left.left() : center.right() + 14 < right.left(),
                        code + QStringLiteral(": center/right overlap"))
            || !require(translated_language.text() == captions[step],
                        code + QStringLiteral(": stale or missing toolbar translation"))
            || !require(active_draft.text() == QStringLiteral("Unsaved network name"),
                        code + QStringLiteral(": draft replaced"))
            || !require(active_draft.selectionStart() == 3
                            && active_draft.selectedText() == QStringLiteral("aved "),
                        code + QStringLiteral(": cursor selection lost"))
            || !require(right_choice.currentIndex() == 1 && right_choice.currentData().toInt() == 8,
                        code + QStringLiteral(": selection identity changed"))
            || !require(edits == 0 && selections == 0,
                        code + QStringLiteral(": presentation update emitted edit signal")))
            return 3;
    }
    return 0;
}
