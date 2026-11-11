#include "config/gui_language.h"
#include "config/gui_preferences.h"

#include <algorithm>
#include <QApplication>
#include <QDebug>
#include <QLocale>
#include <QJsonObject>

namespace
{
const QString preference_group = QStringLiteral("interface");
const QString preference_key = QStringLiteral("language");
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// The former localStorage item: deleted, not migrated.
EM_JS(void, removeLegacyGuiLanguage, (), {
    try { localStorage.removeItem("aowis.gui.language"); }
    catch (error) {}
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
    removeLegacyGuiLanguage();
#endif
    const QString code = GuiPreferences::value(preference_group, preference_key).toString(QStringLiteral("en"));
    if (!activate(code))
        activate(QStringLiteral("en"));
}

bool GuiLanguage::select(const QString &code)
{
    if (!activate(code))
        return false;
    if (!GuiPreferences::update(preference_group, QJsonObject{{preference_key, code}}))
        qWarning() << "Could not save interface language";
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
