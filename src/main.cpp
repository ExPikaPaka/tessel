#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTimer>
#include <QStringList>
#include <QTranslator>

#include "tessel_version.h"
#include "ui/Icons.h"
#include "ui/KeyLayoutFilter.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/WheelFilter.h"

namespace {

// The language the interface starts in. No setting, or "system", means follow
// the system locale; otherwise the setting names the locale, as in "en" or
// "ru". It is read once here: changing it takes a restart, which is what the
// Settings dialog says.
QLocale startupLocale(const QSettings& settings)
{
    const QString name =
        settings.value(QStringLiteral("language"), QStringLiteral("system")).toString();
    if (name.isEmpty() || name == QLatin1String("system"))
        return QLocale::system();
    return QLocale(name);
}

// Qt's own strings go in first (the buttons of the standard dialogs, the file
// chooser, the colour dialog), then ours: the translator installed last is
// asked first, so ours wins wherever both carry the same text.
//
// Either load may fail, which is not an error: it only means there is no
// translation for that language and the English source text is used.
void installTranslators(QApplication& app, const QLocale& locale)
{
    auto* qt = new QTranslator(&app);
    if (qt->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                 QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(qt);
    }
    auto* ours = new QTranslator(&app);
    // The .qm files are built into the executable as a resource, so one binary
    // carries every language it was built with.
    if (ours->load(locale, QStringLiteral("tessel"), QStringLiteral("_"),
                   QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(ours);
    }
}

} // namespace

int main(int argc, char** argv)
{
    // Answered before anything else exists. Qt's own version option puts the
    // text in a message box on a Windows GUI application, which waits for a
    // click that a script will never give it.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("%s %s\n", TESSEL_APP_NAME, TESSEL_VERSION_STRING);
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral(TESSEL_APP_NAME));
    QApplication::setApplicationVersion(QStringLiteral(TESSEL_VERSION_STRING));
    QApplication::setOrganizationName(QStringLiteral("Tessel"));
    QApplication::setDesktopFileName(QStringLiteral("org.tessel.Tessel"));

    // Before the parser, so that --help is answered in the chosen language.
    QSettings settings;
    installTranslators(app, startupLocale(settings));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("Main", "Tessel - a layered raster image editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QCoreApplication::translate("Main", "Images to open."),
                                 QStringLiteral("[files...]"));
    QCommandLineOption lightOption(
        QStringList{QStringLiteral("light")},
        QCoreApplication::translate("Main", "Start with the light theme."));
    parser.addOption(lightOption);
    // Development aid: render the window to a PNG and exit, which makes it
    // possible to check the interface without a display.
    QCommandLineOption shotOption(
        QStringList{QStringLiteral("screenshot")},
        QCoreApplication::translate("Main", "Save a window screenshot and exit."),
        QStringLiteral("file"));
    parser.addOption(shotOption);
    QCommandLineOption shotDelayOption(
        QStringList{QStringLiteral("screenshot-delay")},
        QCoreApplication::translate("Main", "Milliseconds to wait first."),
        QStringLiteral("ms"), QStringLiteral("700"));
    parser.addOption(shotDelayOption);
    parser.process(app);

    const bool dark = !parser.isSet(lightOption)
        && settings.value(QStringLiteral("theme"), QStringLiteral("dark")).toString()
            != QStringLiteral("light");
    tessel::theme::apply(app, dark ? tessel::theme::Mode::Dark : tessel::theme::Mode::Light);
    QApplication::setWindowIcon(tessel::icons::appIcon());

    // Wheel over a value field lowers it when pushed forward.
    app.installEventFilter(new tessel::WheelFilter(&app));
    app.installEventFilter(new tessel::KeyLayoutFilter(&app));

    tessel::MainWindow window;
    window.show();

    QStringList paths;
    for (const QString& argument : parser.positionalArguments()) {
        const QFileInfo info(argument);
        if (info.exists())
            paths.append(info.absoluteFilePath());
    }
    // A screenshot run answers no questions: a modal recovery or reopen offer
    // would hold the grab up for ever.
    const bool screenshot = parser.isSet(shotOption);
    if (screenshot) {
        if (paths.isEmpty())
            window.newDocument(800, 600, tessel::Rgba(255, 255, 255, 255));
        else
            window.openPaths(paths);
    } else {
        // Recovery files from a run that did not finish, then the reopen
        // offer, then whatever was named on the command line.
        window.startSession(paths);
    }

    if (screenshot) {
        const QString target = parser.value(shotOption);
        const int delay = parser.value(shotDelayOption).toInt();
        QTimer::singleShot(delay, &app, [&window, target] {
            window.grab().save(target);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
