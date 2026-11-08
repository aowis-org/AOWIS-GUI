#include "config/gui_language.h"
#include "config/gui_configuration.h"

#include <algorithm>
#include <QApplication>
#include <QDebug>
#include <QLocale>
#include <QSettings>

#ifdef __EMSCRIPTEN__
#include <cstdlib>
#include <emscripten.h>

EM_JS(char *, loadGuiLanguage, (), {
    try { return stringToNewUTF8(localStorage.getItem("aowis.gui.language") || "en"); }
    catch (error) { return stringToNewUTF8("en"); }
});

EM_JS(int, saveGuiLanguage, (const char *code), {
    try { localStorage.setItem("aowis.gui.language", UTF8ToString(code)); return 1; }
    catch (error) { return 0; }
});
#endif

GuiLanguage &GuiLanguage::instance()
{
    static GuiLanguage manager;
    return manager;
}

QString GuiLanguage::code() const
{
    return this->active_code;
}

void GuiLanguage::restore()
{
#ifdef __EMSCRIPTEN__
    char *stored = loadGuiLanguage();
    const QString code = QString::fromUtf8(stored);
    std::free(stored);
#else
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    const QString code = settings.value(QStringLiteral("interface/language"), QStringLiteral("en")).toString();
#endif
    if (!activate(code))
        activate(QStringLiteral("en"));
}

bool GuiLanguage::select(const QString &code)
{
    if (!activate(code))
        return false;
#ifdef __EMSCRIPTEN__
    if (!saveGuiLanguage(code.toUtf8().constData()))
        qWarning() << "Could not save interface language";
#else
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("interface/language"), code);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        qWarning() << "Could not save interface language";
#endif
    return true;
}

bool GuiLanguage::activate(const QString &code)
{
    if (std::none_of(GuiLanguages.begin(), GuiLanguages.end(), [&code](const GuiLanguageChoice &choice) {
            return code == QLatin1String(choice.code);
        }))
        return false;

    if (code != this->active_code)
    {
        std::array<std::unique_ptr<QTranslator>, 2> next;
        if (code != QStringLiteral("en"))
        {
            const std::array<QString, 2> domains{QStringLiteral("gui"), QStringLiteral("render")};
            for (std::size_t index = 0; index < domains.size(); ++index)
            {
                next[index] = std::make_unique<QTranslator>();
                if (!next[index]->load(QStringLiteral(":/i18n/aowis_%1_%2.qm").arg(domains[index], code)))
                    return false;
            }
            std::size_t installed = 0;
            for (const std::unique_ptr<QTranslator> &catalogue : next)
            {
                if (!QCoreApplication::installTranslator(catalogue.get()))
                {
                    for (std::size_t index = 0; index < installed; ++index)
                        QCoreApplication::removeTranslator(next[index].get());
                    return false;
                }
                ++installed;
            }
        }
        for (const std::unique_ptr<QTranslator> &catalogue : this->translators)
            if (catalogue)
                QCoreApplication::removeTranslator(catalogue.get());
        this->translators = std::move(next);
        this->active_code = code;
    }
    QApplication::setLayoutDirection(QLocale(code).textDirection());
    return true;
}
