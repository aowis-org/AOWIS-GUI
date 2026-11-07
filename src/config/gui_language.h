#pragma once

#include <QString>
#include <QTranslator>

#include <array>
#include <memory>

struct GuiLanguageChoice
{
    const char *code;
    const char *name;
};

inline constexpr std::array<GuiLanguageChoice, 5> GuiLanguages{{
    {"en", "English"}, {"fr", "Français"}, {"ar", "العربية"},
    {"de", "Deutsch"}, {"es", "Español"}
}};

class GuiLanguage
{
public:
    static GuiLanguage &instance();
    QString code() const;
    void restore();
    bool select(const QString &code);

private:
    GuiLanguage() = default;
    bool activate(const QString &code);
    QString active_code = QStringLiteral("en");
    std::array<std::unique_ptr<QTranslator>, 2> translators;
};
