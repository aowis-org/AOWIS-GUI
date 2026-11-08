#include "config/gui_configuration.h"
#include "config/gui_language.h"
#include "tabs/projects_inspector_panels.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QLocale>
#include <QSettings>
#include <QTemporaryDir>

class LanguageLabel final : public QLabel
{
protected:
    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            setText(QCoreApplication::translate("TopControlBar", "Language"));
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    qputenv("XDG_DATA_HOME", directory.path().toUtf8());
    qputenv("HOME", directory.path().toUtf8());
    qputenv("APPDATA", directory.path().toUtf8());
    QApplication app(argc, argv);
    GuiLanguage &language = GuiLanguage::instance();
    LanguageLabel label;
    label.show();
    ProjectInspectorOverview overview;
    overview.show();
    overview.name->setText(QStringLiteral("West supply"));
    overview.name->setSelection(5, 6);
    overview.description->setPlainText(QStringLiteral("Draft description"));
    QFormLayout *form = static_cast<QFormLayout *>(overview.fields->layout());
    const QLabel *description_label = static_cast<QLabel *>(form->labelForField(overview.description));
    const char *const descriptions[] = {"Description", "Description", "الوصف", "Beschreibung", "Descripción"};
    const char *const imports[] = {"Import network…", "Importer un réseau…", "استيراد شبكة…",
                                   "Netz importieren…", "Importar red…"};
    const QLocale numeric_locale;
    const char *const names[] = {"Language", "Langue", "اللغة", "Sprache", "Idioma"};
    const char *const cancel[] = {"Cancel", "Annuler", "إلغاء", "Abbrechen", "Cancelar"};
    for (std::size_t step = 1; step <= GuiLanguages.size(); ++step)
    {
        const std::size_t index = step % GuiLanguages.size();
        const QString code = QString::fromLatin1(GuiLanguages[index].code);
        if (!language.select(code))
            return 2;
        app.processEvents();
        if (label.text() != QString::fromUtf8(names[index])
            || QCoreApplication::translate("MapBgfxHud", "Cancel") != QString::fromUtf8(cancel[index])
            || language.code() != code || numeric_locale != QLocale()
            || QApplication::layoutDirection() != QLocale(code).textDirection())
            return 3;
        if (description_label->text() != QString::fromUtf8(descriptions[index])
            || overview.import_network->text() != QString::fromUtf8(imports[index])
            || overview.name->selectedText() != QStringLiteral("supply")
            || overview.description->toPlainText() != QStringLiteral("Draft description"))
            return 7;
        QSettings persisted(guiConfigurationFilePath(), QSettings::IniFormat);
        if (persisted.value(QStringLiteral("interface/language")).toString() != code)
            return 4;
    }
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("interface/language"), QStringLiteral("de"));
    settings.sync();
    language.restore();
    if (language.code() != QStringLiteral("de") || language.select(QStringLiteral("xx")))
        return 5;
    settings.setValue(QStringLiteral("interface/language"), QStringLiteral("invalid"));
    settings.sync();
    language.restore();
    return language.code() == QStringLiteral("en") ? 0 : 6;
}
