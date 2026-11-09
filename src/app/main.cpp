#include <QApplication>
#include <QIcon>
#if defined(AOWIS_GUI_LOCALIZATION_ACCEPTANCE)
#include <QTemporaryDir>
#include <memory>
#endif
#include "app/main_window.h"
#include "config/gui_configuration.h"

#include "config/gui_language.h"

#if defined(AOWIS_GUI_LOCALIZATION_ACCEPTANCE)
int runEntityInspectorLanguageAcceptance(QApplication &app);
#endif

int main(int argc, char *argv[])
{
#if defined(AOWIS_GUI_LOCALIZATION_ACCEPTANCE)
    // Isolate configuration *before* constructing QApplication, so a headless
    // acceptance run cannot read or overwrite the developer's preferences.
    bool language_acceptance = false;
    std::unique_ptr<QTemporaryDir> language_test_settings;
    for (int index = 1; index < argc; ++index)
        if (QString::fromLocal8Bit(argv[index]) == QStringLiteral("--test-live-entity-inspector-language"))
            language_acceptance = true;
    if (language_acceptance)
    {
        language_test_settings = std::make_unique<QTemporaryDir>();
        if (!language_test_settings->isValid())
            return 2;
        qputenv("XDG_DATA_HOME", language_test_settings->path().toUtf8());
        qputenv("XDG_CONFIG_HOME", language_test_settings->path().toUtf8());
        qputenv("HOME", language_test_settings->path().toUtf8());
        qputenv("APPDATA", language_test_settings->path().toUtf8());
    }
#endif
    QApplication app(argc, argv);

#if defined(AOWIS_GUI_LOCALIZATION_ACCEPTANCE)
    // Exercise the real inspector without constructing MainWindow, a bgfx device,
    // a database client, or any network services. Only enabled in test builds.
    if (language_acceptance)
        return runEntityInspectorLanguageAcceptance(app);
#endif

#ifndef __EMSCRIPTEN__
    // Materialize the native configuration file immediately on application startup.
    (void)guiConfiguration();
#endif

    GuiLanguage::instance().restore();
    app.setWindowIcon(QIcon(":/icon/aowis_light_128.png"));
    
    MainWindow w;
    w.show();
    
    return app.exec();
}
