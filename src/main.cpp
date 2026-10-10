#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <QStringList>

#include "tessel_version.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/WheelFilter.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral(TESSEL_APP_NAME));
    QApplication::setApplicationVersion(QStringLiteral(TESSEL_VERSION_STRING));
    QApplication::setOrganizationName(QStringLiteral("Tessel"));
    QApplication::setDesktopFileName(QStringLiteral("org.tessel.Tessel"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Tessel - a layered raster image editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QStringLiteral("Images to open."),
                                 QStringLiteral("[files...]"));
    QCommandLineOption lightOption(QStringList{QStringLiteral("light")},
                                   QStringLiteral("Start with the light theme."));
    parser.addOption(lightOption);
    // Development aid: render the window to a PNG and exit, which makes it
    // possible to check the interface without a display.
    QCommandLineOption shotOption(QStringList{QStringLiteral("screenshot")},
                                  QStringLiteral("Save a window screenshot and exit."),
                                  QStringLiteral("file"));
    parser.addOption(shotOption);
    QCommandLineOption shotDelayOption(QStringList{QStringLiteral("screenshot-delay")},
                                       QStringLiteral("Milliseconds to wait first."),
                                       QStringLiteral("ms"), QStringLiteral("700"));
    parser.addOption(shotDelayOption);
    parser.process(app);

    QSettings settings;
    const bool dark = !parser.isSet(lightOption)
        && settings.value(QStringLiteral("theme"), QStringLiteral("dark")).toString()
            != QStringLiteral("light");
    tessel::theme::apply(app, dark ? tessel::theme::Mode::Dark : tessel::theme::Mode::Light);
    QApplication::setWindowIcon(tessel::icons::appIcon());

    // Wheel over a value field lowers it when pushed forward.
    app.installEventFilter(new tessel::WheelFilter(&app));

    tessel::MainWindow window;
    window.show();

    QStringList paths;
    for (const QString& argument : parser.positionalArguments()) {
        const QFileInfo info(argument);
        if (info.exists())
            paths.append(info.absoluteFilePath());
    }
    if (paths.isEmpty())
        window.newDocument(800, 600, tessel::Rgba(255, 255, 255, 255));
    else
        window.openPaths(paths);

    if (parser.isSet(shotOption)) {
        const QString target = parser.value(shotOption);
        const int delay = parser.value(shotDelayOption).toInt();
        QTimer::singleShot(delay, &app, [&window, target] {
            window.grab().save(target);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
