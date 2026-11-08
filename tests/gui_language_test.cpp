#include "config/gui_configuration.h"
#include "config/gui_language.h"
#include "tabs/projects_inspector_panels.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QLabel>
#include <QLocale>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

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

template<typename T>
static bool matches(const QString &language, const char *field, const T &actual, const T &expected)
{
    if (actual == expected)
        return true;
    qCritical() << language << field << "expected" << expected << "actual" << actual;
    return false;
}

int main(int argc, char **argv)
{
    QTemporaryDir directory;
    if (!directory.isValid())
    {
        qCritical() << "Temporary settings directory:" << directory.errorString();
        return 1;
    }
    qputenv("XDG_DATA_HOME", directory.path().toUtf8());
    qputenv("HOME", directory.path().toUtf8());
    qputenv("APPDATA", directory.path().toUtf8());
    QApplication app(argc, argv);
    GuiLanguage &language = GuiLanguage::instance();
    QWidget window;
    QVBoxLayout layout(&window);
    LanguageLabel label;
    ProjectInspectorOverview overview;
    layout.addWidget(&label);
    layout.addWidget(&overview);
    overview.name->setText(QStringLiteral("West supply"));
    overview.description->setPlainText(QStringLiteral("Draft description"));
    QFormLayout *form = static_cast<QFormLayout *>(overview.fields->layout());
    const QLabel *description_label = static_cast<QLabel *>(form->labelForField(overview.description));
    const char *const descriptions[] = {"Description", "Description", "الوصف", "Beschreibung", "Descripción"};
    const char *const imports[] = {"Import network…", "Importer un réseau…", "استيراد شبكة…",
                                   "Netz importieren…", "Importar red…"};
    RevisionNetworkOverview network;
    layout.addWidget(&network);
    const QString title = QStringLiteral("Main %2");
    QTabWidget tabs;
    RevisionInspectorOverview details;
    RevisionInspectorBranch branch_page(details.name);
    tabs.addTab(&branch_page, QStringLiteral("branch"));
    tabs.addTab(&details, QStringLiteral("details"));
    tabs.setCurrentWidget(&details);
    layout.addWidget(&tabs);
    window.show();
    overview.name->setFocus();
    app.processEvents();
    // Finish queued focus handling from showing the window before seeding the selection.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    overview.name->setSelection(5, 6);
    const QString draft = QStringLiteral("Check north pump\nDraft changes");
    details.commit_message->setPlainText(draft);
    QTextCursor cursor = details.commit_message->textCursor();
    cursor.setPosition(6);
    cursor.setPosition(11, QTextCursor::KeepAnchor);
    details.commit_message->setTextCursor(cursor);
    branch_page.name->setText(QStringLiteral("main / north"));
    const char *const commit_labels[] = {"Commit message", "Message de commit", "رسالة تسجيل المراجعة",
                                        "Commit-Nachricht", "Mensaje de commit"};
    QFormLayout *details_form = static_cast<QFormLayout *>(details.fields->layout());
    const QLabel *commit_label = static_cast<QLabel *>(details_form->labelForField(details.commit_message));
    int draft_changes = 0;
    QObject::connect(details.commit_message, &QPlainTextEdit::textChanged, [&draft_changes] { ++draft_changes; });
    const QLocale numeric_locale;
    const char *const names[] = {"Language", "Langue", "اللغة", "Sprache", "Idioma"};
    const char *const cancel[] = {"Cancel", "Annuler", "إلغاء", "Abbrechen", "Cancelar"};
    for (std::size_t step = 1; step <= GuiLanguages.size(); ++step)
    {
        const std::size_t index = step % GuiLanguages.size();
        const QString code = QString::fromLatin1(GuiLanguages[index].code);
        if (!matches(code, "select language", language.select(code), true))
            return 2;
        app.processEvents();
        if (!matches(code, "toolbar label", label.text(), QString::fromUtf8(names[index]))
            || !matches(code, "HUD cancel", QCoreApplication::translate("MapBgfxHud", "Cancel"), QString::fromUtf8(cancel[index]))
            || !matches(code, "active language", language.code(), code)
            || !matches(code, "numeric locale preserved", numeric_locale == QLocale(), true)
            || !matches(code, "layout direction", QApplication::layoutDirection(), QLocale(code).textDirection()))
            return 3;
        if (!matches(code, "description label", description_label->text(), QString::fromUtf8(descriptions[index]))
            || !matches(code, "import action", overview.import_network->text(), QString::fromUtf8(imports[index]))
            || !matches(code, "project name selection", overview.name->selectedText(), QStringLiteral("supply"))
            || !matches(code, "project description", overview.description->toPlainText(), QStringLiteral("Draft description")))
            return 7;
        const QTextCursor current_cursor = details.commit_message->textCursor();
        if (!matches(code, "commit label", commit_label->text(), QString::fromUtf8(commit_labels[index]))
            || !matches(code, "commit draft", details.commit_message->toPlainText(), draft)
            || !matches(code, "draft change signals", draft_changes, 0)
            || !matches(code, "draft selection anchor", current_cursor.anchor(), 6)
            || !matches(code, "draft cursor", current_cursor.position(), 11)
            || !matches(code, "details tab selected", tabs.currentWidget() == &details, true)
            || !matches(code, "branch name", branch_page.name->text(), QStringLiteral("main / north")))
            return 8;
        if (step == 1)
        {
            if (!matches(code, "loading summary", network.network_status->text(), QStringLiteral("Vérification du réseau…")))
                return 9;
            QTimer::singleShot(0, &network, [&network, title] {
                network.showCounts(QJsonObject{{QStringLiteral("pipes"), 3}}, 3);
                network.setStatus({"Network: %1", {title}},
                                  {"\nSaved private working copy · generation %1", {QStringLiteral("42")}});
            });
            app.processEvents();
        }
        const QString summary = QCoreApplication::translate("ProjectsWidget", "Network: %1").arg(title)
            + QCoreApplication::translate("ProjectsWidget", "\nSaved private working copy · generation %1")
                .arg(QStringLiteral("42"));
        const QString total = QCoreApplication::translate("ProjectsWidget", "Total hydraulic entities: %1")
            .arg(numeric_locale.toString(3));
        if (!matches(code, "saved network summary", network.network_status->text(), summary)
            || !matches(code, "pipe count", network.counts.at(1)->text(), numeric_locale.toString(3))
            || !matches(code, "network total", network.other_counts->text(), total))
            return 9;
        QSettings persisted(guiConfigurationFilePath(), QSettings::IniFormat);
        if (!matches(code, "persisted language", persisted.value(QStringLiteral("interface/language")).toString(), code))
            return 4;
    }
    QSettings settings(guiConfigurationFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("interface/language"), QStringLiteral("de"));
    settings.sync();
    language.restore();
    if (!matches(QStringLiteral("restore"), "stored language", language.code(), QStringLiteral("de"))
        || !matches(QStringLiteral("xx"), "accepted unknown language", language.select(QStringLiteral("xx")), false))
        return 5;
    settings.setValue(QStringLiteral("interface/language"), QStringLiteral("invalid"));
    settings.sync();
    language.restore();
    return matches(QStringLiteral("restore"), "invalid settings fallback", language.code(), QStringLiteral("en")) ? 0 : 6;
}
