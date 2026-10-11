// Headless interface tests: the real widgets, tools and dialogs are driven
// through their public API with the offscreen Qt platform plugin.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QPointingDevice>
#include <QSettings>
#include <QTabletEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QShortcut>
#include <QSettings>
#include <QSpinBox>
#include <QMenuBar>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTranslator>
#include <QTreeWidget>
#include <QTest>
#include <QXmlStreamReader>
#include <QTextBrowser>
#include <QToolButton>
#include <QThread>
#include <QTimer>

#include "core/ImageOps.h"
#include "core/Strings.h"
#include "effects/EffectPlugin.h"
#include "effects/EffectPluginHost.h"
#include "core/Io.h"
#include "core/Palette.h"
#include "core/Recovery.h"
#include "tools/Tool.h"
#include "ui/CanvasView.h"
#include "ui/ColorPanel.h"
#include "ui/Dialogs.h"
#include "ui/Docks.h"
#include "ui/Panels.h"
#include "ui/CurveWidget.h"
#include "ui/EffectDialog.h"
#include "ui/Icons.h"
#include "ui/KeyLayoutFilter.h"
#include "ui/MainWindow.h"
#include "ui/RecoveryDialog.h"
#include "ui/Theme.h"

using namespace tessel;

namespace {

int failures = 0;
int checks = 0;
const char* currentTest = "";

void check(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL %s:%d  %s\n", currentTest, line, expression);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

std::vector<std::pair<const char*, std::function<void()>>> tests;
void addTest(const char* name, std::function<void()> fn)
{
    tests.emplace_back(name, std::move(fn));
}

bool nearly(double a, double b, double tolerance = 1e-6)
{
    return std::abs(a - b) <= tolerance;
}

// Stands in for a loaded plugin. The window cannot tell the difference: it
// reads the registry and the load report, not the libraries.
class GuiFakePlugin : public EffectPlugin {
public:
    int interfaceVersion() const override { return kEffectPluginVersion; }
    QString name() const override { return QStringLiteral("Gui Fake Plugin"); }
    QString version() const override { return QStringLiteral("1.0"); }

    void registerEffects(EffectRegistry& registry) override
    {
        fx::add(registry, "gui_fake_effect", "Gui Fake Effect", "Gui Plugin",
                {ParamSpec::intParam(QStringLiteral("amount"), "Amount", 0, 255, 10)},
                [](const EffectContext& context, const ParamValues& values) {
                    const int amount = values.intValue(QStringLiteral("amount"), 10);
                    return fx::mapPixels(context, [amount](Rgba c, int, int) {
                        return Rgba(uint8_t(amount), c.g, c.b, c.a);
                    });
                });
    }
};

// About the Plugins is modal, so its text has to be collected from a timer
// inside its own nested event loop, the same way the selection dialogs are.
void collectPluginsDialog(const std::shared_ptr<QString>& into)
{
    auto* timer = new QTimer(qApp);
    auto attempts = std::make_shared<int>(0);
    timer->setInterval(5);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, into, attempts] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QDialog*>(widget);
            if (!dialog || !dialog->isVisible()
                || dialog->objectName() != QStringLiteral("pluginsDialog"))
                continue;
            if (auto* text = dialog->findChild<QTextBrowser*>(
                    QStringLiteral("pluginsText")))
                *into = text->toPlainText();
            dialog->accept();
            timer->stop();
            timer->deleteLater();
            return;
        }
        if (++*attempts > 200) {
            timer->stop();
            timer->deleteLater();
            QApplication::exit(1);
        }
    });
    timer->start();
}

ToolEvent makeEvent(QPointF pos, Qt::MouseButton button = Qt::LeftButton,
                    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    ToolEvent event;
    event.pos = pos;
    event.pixel = QPoint(int(pos.x()), int(pos.y()));
    event.viewPos = pos;
    event.button = button;
    event.buttons = button;
    event.modifiers = modifiers;
    return event;
}

// Runs a press / move / release gesture through a tool.
void drag(Tool* tool, ToolContext& context, QPointF from, QPointF to,
          Qt::MouseButton button = Qt::LeftButton,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    tool->press(context, makeEvent(from, button, modifiers));
    const int steps = 6;
    for (int i = 1; i <= steps; ++i) {
        const double t = double(i) / steps;
        ToolEvent move = makeEvent(QPointF(from.x() + (to.x() - from.x()) * t,
                                           from.y() + (to.y() - from.y()) * t),
                                   button, modifiers);
        move.button = Qt::NoButton;
        tool->move(context, move);
    }
    tool->release(context, makeEvent(to, button, modifiers));
}

// Types a word into a tool one key press at a time.
void typeInto(Tool* tool, ToolContext& context, const QString& words)
{
    for (const QChar letter : words) {
        QKeyEvent key(QEvent::KeyPress, letter.unicode(), Qt::NoModifier,
                      QString(letter));
        tool->keyPress(context, &key);
    }
}

void pressKey(Tool* tool, ToolContext& context, int key,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    tool->keyPress(context, &event);
}

// The same gesture from a pen held at one steady pressure. A mouse reports a
// full press, which is what makeEvent leaves in place.
ToolEvent makePenEvent(QPointF pos, double pressure,
                       Qt::MouseButton button = Qt::LeftButton)
{
    ToolEvent event = makeEvent(pos, button);
    event.fromTablet = true;
    event.pressure = pressure;
    return event;
}

void penDrag(Tool* tool, ToolContext& context, QPointF from, QPointF to,
             double pressure)
{
    tool->press(context, makePenEvent(from, pressure));
    const int steps = 6;
    for (int i = 1; i <= steps; ++i) {
        const double t = double(i) / steps;
        ToolEvent move = makePenEvent(QPointF(from.x() + (to.x() - from.x()) * t,
                                              from.y() + (to.y() - from.y()) * t),
                                      pressure);
        move.button = Qt::NoButton;
        tool->move(context, move);
    }
    tool->release(context, makePenEvent(to, pressure));
}

// The selection dialogs are modal, so the answer has to be typed in from a
// timer that fires inside their own nested event loop.
void answerSelectionDialog(int amount)
{
    auto* timer = new QTimer(qApp);
    auto attempts = std::make_shared<int>(0);
    timer->setInterval(5);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, amount, attempts] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<SelectionOpDialog*>(widget);
            if (!dialog || !dialog->isVisible())
                continue;
            if (QSpinBox* spin = dialog->findChild<QSpinBox*>())
                spin->setValue(amount);
            dialog->accept();
            timer->stop();
            timer->deleteLater();
            return;
        }
        // Never leave a nested event loop spinning if the dialog never shows.
        if (++*attempts > 200) {
            timer->stop();
            timer->deleteLater();
            QApplication::exit(1);
        }
    });
    timer->start();
}

bool sameImage(const Image& a, const Image& b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return false;
    for (size_t i = 0; i < a.pixelCount(); ++i) {
        if (a.data()[i] != b.data()[i])
            return false;
    }
    return true;
}

size_t nonTransparentPixels(const Image& image)
{
    size_t count = 0;
    for (size_t i = 0; i < image.pixelCount(); ++i) {
        if (image.data()[i].a != 0)
            ++count;
    }
    return count;
}

bool samePixels(const Image& a, const Image& b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return false;
    return std::memcmp(a.data(), b.data(), a.byteCount()) == 0;
}

void paintGradient(Image& image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setAt(x, y, Rgba(uint8_t((x * 7) & 0xff), uint8_t((y * 11) & 0xff),
                                   uint8_t(((x ^ y) * 5) & 0xff), 255));
        }
    }
}

const Rgba kPrimary(200, 40, 40, 255);
const Rgba kSecondary(255, 255, 255, 255);

// Exactly what the old single threaded commit produced: the effect's own
// output mixed into the source through the clip mask.
Image directApply(Effect* effect, const Image& source, const ParamValues& values,
                  const Mask* clip, const Rect& region)
{
    EffectContext context;
    context.source = &source;
    context.clip = clip;
    context.region = region;
    context.primary = kPrimary;
    context.secondary = kSecondary;
    const Image result = effect->apply(context, values);
    Image out = source;
    ops::mixThroughMask(out, result, clip, region);
    return out;
}

std::unique_ptr<Document> gradientDocument(int width, int height)
{
    auto doc = std::make_unique<Document>(width, height);
    paintGradient(doc->activeLayer()->image);
    doc->invalidate();
    return doc;
}

// Spins the event loop until `ready` holds, so the tests never sleep blindly.
bool spinUntil(const std::function<bool()>& ready, int timeoutMs = 30000)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (ready())
            return true;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("TesselTests"));
    QApplication::setApplicationName(QStringLiteral("TesselTests"));
    theme::apply(app, theme::Mode::Dark);
    registerBuiltinTools();
    registerBuiltinEffects();

    // ------------------------------------------------------------ translations
    addTest("the Russian translation reaches the interface", [] {
#ifdef TESSEL_HAS_TRANSLATIONS
        // The .qm travels inside the executable, exactly as it does in the
        // real program, so this is the same lookup src/main.cpp performs.
        auto* translator = new QTranslator(qApp);
        CHECK(translator->load(QStringLiteral(":/i18n/tessel_ru.qm")));
        const QString englishTool =
            ToolRegistry::instance().byId(QStringLiteral("brush"))->name();
        const QString englishBlend = blendModeLabel(BlendMode::Normal);
        CHECK(QApplication::installTranslator(translator));

        // One string per translation context that has no QObject of its own.
        CHECK(ToolRegistry::instance().byId(QStringLiteral("brush"))->name()
              == QString::fromUtf8("Кисть"));
        CHECK(blendModeLabel(BlendMode::Normal) == QString::fromUtf8("Обычный"));
        CHECK(QCoreApplication::translate("Effects", "Gaussian Blur")
              == QString::fromUtf8("Размытие по Гауссу"));
        CHECK(QCoreApplication::translate("Core", "Layer %1").arg(2)
              == QString::fromUtf8("Слой 2"));
        CHECK(effectCategoryLabel(QStringLiteral("Blurs"))
              == QString::fromUtf8("Размытие"));
        // A category a plugin invented is shown as the plugin named it.
        CHECK(effectCategoryLabel(QStringLiteral("Gui Plugin"))
              == QStringLiteral("Gui Plugin"));

        // Russian needs three plural forms, and the count has to survive.
        CHECK(QCoreApplication::translate("tessel::MainWindow", "Saved %n image(s)",
                                          nullptr, 1)
              == QString::fromUtf8("Сохранено 1 изображение"));
        CHECK(QCoreApplication::translate("tessel::MainWindow", "Saved %n image(s)",
                                          nullptr, 3)
              == QString::fromUtf8("Сохранено 3 изображения"));
        CHECK(QCoreApplication::translate("tessel::MainWindow", "Saved %n image(s)",
                                          nullptr, 7)
              == QString::fromUtf8("Сохранено 7 изображений"));

        {
            // A window built with the translator in place: the menus, the
            // panels and the status bar all come back in Russian.
            MainWindow window;
            window.newDocument(32, 24, Rgba(255, 255, 255, 255));
            QStringList menus;
            for (QAction* action : window.menuBar()->actions())
                menus.append(action->text());
            CHECK(menus.contains(QString::fromUtf8("&Файл")));
            CHECK(menus.contains(QString::fromUtf8("&Слои")));
            // Every menu keeps a mnemonic, which is what makes Alt usable.
            for (const QString& title : menus)
                check(title.contains(QLatin1Char('&')), "menu mnemonic", __LINE__);
            if (QAction* undo = window.findChild<QAction*>(QStringLiteral("undo")))
                CHECK(undo->text() == QString::fromUtf8("&Отменить"));
            if (QLabel* size = window.findChild<QLabel*>(QStringLiteral("imagesize")))
                CHECK(size->text() == QStringLiteral("32 x 24 px"));
            const QList<CanvasView*> views = window.findChildren<CanvasView*>();
            CHECK(!views.isEmpty());
            if (!views.isEmpty()) {
                CHECK(views.last()->document()->activeLayer()->name
                      == QString::fromUtf8("Фон"));
            }
        }

        QApplication::removeTranslator(translator);
        // Taking the translator away puts the English source text back, which
        // is what a build with no translation at all shows.
        CHECK(ToolRegistry::instance().byId(QStringLiteral("brush"))->name()
              == englishTool);
        CHECK(ToolRegistry::instance().byId(QStringLiteral("brush"))->name()
              == QStringLiteral("Paintbrush"));
        CHECK(blendModeLabel(BlendMode::Normal) == englishBlend);
        CHECK(QCoreApplication::translate("Effects", "Gaussian Blur")
              == QStringLiteral("Gaussian Blur"));
        delete translator;
#endif
    });

    addTest("every translated string keeps its placeholders", [] {
        // Read from the .ts rather than the .qm: this is the file a
        // translator edits, and a dropped %1 in it is the mistake that turns
        // a message into nonsense at runtime.
        QFile file(QStringLiteral(TESSEL_TRANSLATIONS_DIR "/tessel_ru.ts"));
        CHECK(file.open(QIODevice::ReadOnly));
        const auto markers = [](const QString& text) {
            QStringList found;
            for (int i = 0; i + 1 < text.size(); ++i) {
                if (text.at(i) != QLatin1Char('%'))
                    continue;
                const QChar next = text.at(i + 1);
                if (next.isDigit() || next == QLatin1Char('n'))
                    found.append(text.mid(i, 2));
            }
            found.sort();
            return found;
        };
        QXmlStreamReader xml(&file);
        QString source;
        int messages = 0;
        int translations = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            if (xml.name() == QLatin1String("source")) {
                source = xml.readElementText();
                ++messages;
                continue;
            }
            if (xml.name() != QLatin1String("translation"))
                continue;
            // A plural message holds one numerusform per Russian form, an
            // ordinary one holds its text directly.
            const auto compare = [&](const QString& text) {
                if (text.trimmed().isEmpty())
                    return;
                ++translations;
                check(markers(text) == markers(source),
                      source.toLocal8Bit().constData(), __LINE__);
            };
            xml.readNext();
            while (!(xml.isEndElement() && xml.name() == QLatin1String("translation"))
                   && !xml.atEnd()) {
                if (xml.isCharacters())
                    compare(xml.text().toString());
                else if (xml.isStartElement()
                         && xml.name() == QLatin1String("numerusform"))
                    compare(xml.readElementText());
                xml.readNext();
            }
        }
        CHECK(!xml.hasError());
        // A translation for every string, so nothing is left half done.
        CHECK(messages > 800);
        CHECK(translations >= messages);
    });

    addTest("the settings dialog chooses the language", [] {
        QSettings settings;
        const QVariant before = settings.value(QStringLiteral("language"));
        settings.setValue(QStringLiteral("language"), QStringLiteral("system"));
        MainWindow window;
        // The dialog is modal, so it is answered from inside the loop it runs.
        QTimer::singleShot(0, &window, [] {
            QDialog* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto* combo = dialog->findChild<QComboBox*>(QStringLiteral("language"));
            check(combo != nullptr, "language box", __LINE__);
            if (combo) {
                check(combo->count() == 3, "three languages", __LINE__);
                check(combo->itemData(0).toString() == QLatin1String("system"),
                      "system first", __LINE__);
                check(combo->itemData(2).toString() == QLatin1String("ru"),
                      "russian last", __LINE__);
                check(combo->currentIndex() == 0, "reads the setting", __LINE__);
                combo->setCurrentIndex(2);
            }
            dialog->accept();
            // Accepting it pops the box that says the language waits for a
            // restart, which is answered the same way.
            QTimer::singleShot(0, qApp, [] {
                if (QWidget* modal = QApplication::activeModalWidget())
                    modal->close();
            });
        });
        QTimer::singleShot(3000, &window, [] {
            if (QWidget* modal = QApplication::activeModalWidget())
                modal->close();
        });
        if (auto* button = window.findChild<QToolButton*>(
                QStringLiteral("settingsButton"))) {
            button->click();
        }
        // The locale name is what is stored, not the label, so the setting
        // does not change with the language it is written in.
        CHECK(settings.value(QStringLiteral("language")).toString()
              == QStringLiteral("ru"));
        settings.setValue(QStringLiteral("language"), before);
    });

    // ------------------------------------------------------------ icons
    addTest("icons render", [] {
        const QStringList names = icons::names();
        CHECK(names.size() >= 60);
        // Every glyph has to draw something at every size the interface asks
        // for, and has to stay inside the 2 unit margin at the sizes where a
        // stray pixel would be visible.
        for (const QString& name : names) {
            for (int size : {16, 18, 20, 22, 24}) {
                const QPixmap pixmap = icons::pixmap(name, size, Qt::white);
                check(!pixmap.isNull() && pixmap.width() == size,
                      name.toLocal8Bit().constData(), __LINE__);
                const QImage image = pixmap.toImage();
                int ink = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        if (qAlpha(image.pixel(x, y)) > 24) {
                            ++ink;
                            const bool inside = x > 0 && y > 0
                                                && x < image.width() - 1
                                                && y < image.height() - 1;
                            check(inside, name.toLocal8Bit().constData(), __LINE__);
                        }
                    }
                }
                // A glyph that covers almost nothing is unreadable, and one
                // that covers almost everything is a solid block.
                const int cells = image.width() * image.height();
                check(ink > cells / 20 && ink < cells * 9 / 10,
                      name.toLocal8Bit().constData(), __LINE__);
            }
        }
        CHECK(!icons::appIcon().isNull());
        CHECK(!icons::checkerPixmap(8).isNull());
    });

    // ------------------------------------------------------------ tool palette
    addTest("tool palette covers every tool", [] {
        const QStringList order = ToolsPalette::toolOrder();
        CHECK(order.size() == 19);
        for (const QString& id : order)
            check(ToolRegistry::instance().byId(id) != nullptr,
                  id.toLocal8Bit().constData(), __LINE__);
        // Every registered tool appears exactly once in the palette, apart from
        // the ones reached from a menu instead. Rotate / Zoom is one of those:
        // it is a layer command on Ctrl+Shift+Z, not a palette entry.
        const QStringList menuOnly = {QStringLiteral("transform"),
                                      QStringLiteral("place")};
        for (Tool* tool : ToolRegistry::instance().all()) {
            const int expected = menuOnly.contains(tool->id()) ? 0 : 1;
            check(order.count(tool->id()) == expected,
                  tool->id().toLocal8Bit().constData(), __LINE__);
        }
    });

    // ------------------------------------------------------------ canvas view
    addTest("canvas view zoom", [] {
        auto doc = std::make_unique<Document>(400, 300);
        CanvasView view(std::move(doc));
        view.resize(640, 480);
        view.setZoom(1.0);
        CHECK(std::abs(view.zoom() - 1.0) < 1e-9);
        view.zoomIn();
        CHECK(view.zoom() > 1.0);
        view.zoomOut();
        CHECK(std::abs(view.zoom() - 1.0) < 1e-6);
        view.zoomToFit();
        CHECK(view.isFitMode());
        view.zoomToActualSize();
        CHECK(!view.isFitMode());
        // Zoom is clamped at both ends.
        view.setZoom(1000.0);
        CHECK(view.zoom() <= 32.0);
        view.setZoom(0.0001);
        CHECK(view.zoom() >= 0.01);
    });

    // ------------------------------------------------------------ painting
    addTest("paint tools mark the layer", [] {
        struct Case {
            const char* id;
            bool expectsPixels;
        };
        const Case cases[] = {
            {"brush", true}, {"pencil", true}, {"gradient", true},
            {"shapes", true}, {"line", true},
        };
        for (const Case& item : cases) {
            auto doc = std::make_unique<Document>(80, 60, Rgba(0, 0, 0, 0));
            CanvasView view(std::move(doc));
            Tool* tool = ToolRegistry::instance().byId(QString::fromLatin1(item.id));
            check(tool != nullptr, item.id, __LINE__);
            if (!tool)
                continue;
            view.setTool(tool);
            view.setPrimaryColor(Rgba(255, 0, 0, 255));
            ToolContext context = view.makeContext();
            drag(tool, context, QPointF(10, 10), QPointF(60, 45));
            tool->commit(context);
            const Image& result = view.document()->activeLayer()->image;
            check(nonTransparentPixels(result) > 0, item.id, __LINE__);
            check(view.history()->count() >= 1, item.id, __LINE__);
            // Undo must put the layer back exactly as it was.
            view.history()->goTo(-1);
            check(nonTransparentPixels(view.document()->activeLayer()->image) == 0,
                  item.id, __LINE__);
        }
    });

    addTest("a stroke keeps the composite whole", [] {
        // The composite is kept up to date one rectangle at a time while a
        // tool paints, so what the canvas draws has to be what a rebuild of
        // the whole document would give, after every single dab.
        auto doc = std::make_unique<Document>(300, 200, Rgba(30, 60, 90, 255));
        Layer* group =
            doc->addChild(doc->rootGroup(), Layer::makeGroup(QStringLiteral("Set")));
        group->opacity = 180;
        group->mode = BlendMode::Multiply;
        Layer* inside = doc->addChild(
            group, std::make_unique<Layer>(300, 200, QStringLiteral("Inside"),
                                           Rgba(200, 40, 40, 160)));
        paintGradient(doc->addMask(inside)->image);
        Layer* target = doc->addChild(
            doc->rootGroup(),
            std::make_unique<Layer>(300, 200, QStringLiteral("Paint")));
        target->opacity = 200;
        doc->setActiveIndex(doc->indexOfUid(target->uid()));

        CanvasView view(std::move(doc));
        Document* document = view.document();
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        CHECK(brush != nullptr);
        if (!brush)
            return;
        view.setTool(brush);
        view.setPrimaryColor(Rgba(255, 220, 0, 255));
        ToolContext context = view.makeContext();

        // Every rebuilt composite is compared against a rebuild of all of it.
        auto matchesRebuild = [document] {
            const Image incremental = document->composite();
            document->invalidate();
            return samePixels(incremental, document->composite());
        };

        brush->press(context, makeEvent(QPointF(20, 20)));
        CHECK(matchesRebuild());
        for (int i = 1; i <= 20; ++i) {
            ToolEvent move = makeEvent(QPointF(20 + i * 13, 20 + i * 8));
            move.button = Qt::NoButton;
            brush->move(context, move);
            CHECK(matchesRebuild());
        }
        brush->release(context, makeEvent(QPointF(280, 180)));
        CHECK(matchesRebuild());
        CHECK(nonTransparentPixels(target->image) > 0);

        // Undo writes its rectangle back, which has to reach the tiles too.
        CHECK(view.history()->undo());
        CHECK(matchesRebuild());
        CHECK(nonTransparentPixels(target->image) == 0);
        CHECK(view.history()->redo());
        CHECK(matchesRebuild());
    });

    addTest("eraser removes pixels", [] {
        auto doc = std::make_unique<Document>(60, 60, Rgba(0, 0, 255, 255));
        CanvasView view(std::move(doc));
        Tool* eraser = ToolRegistry::instance().byId(QStringLiteral("eraser"));
        view.setTool(eraser);
        const size_t before = nonTransparentPixels(view.document()->activeLayer()->image);
        ToolContext context = view.makeContext();
        drag(eraser, context, QPointF(10, 30), QPointF(50, 30));
        const size_t after = nonTransparentPixels(view.document()->activeLayer()->image);
        CHECK(after < before);
        CHECK(view.history()->count() == 1);
        view.history()->undo();
        CHECK(nonTransparentPixels(view.document()->activeLayer()->image) == before);
    });

    addTest("paint bucket fills a region", [] {
        auto doc = std::make_unique<Document>(50, 50, Rgba(255, 255, 255, 255));
        CanvasView view(std::move(doc));
        Tool* bucket = ToolRegistry::instance().byId(QStringLiteral("bucket"));
        view.setTool(bucket);
        view.setPrimaryColor(Rgba(0, 200, 0, 255));
        ToolContext context = view.makeContext();
        bucket->press(context, makeEvent(QPointF(25, 25)));
        const Image& image = view.document()->activeLayer()->image;
        CHECK(image.at(25, 25) == Rgba(0, 200, 0, 255));
        CHECK(image.at(0, 0) == Rgba(0, 200, 0, 255));
        CHECK(view.history()->count() == 1);
    });

    addTest("right button paints with the secondary colour", [] {
        auto doc = std::make_unique<Document>(40, 40, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        view.setTool(brush);
        view.setPrimaryColor(Rgba(255, 0, 0, 255));
        view.setSecondaryColor(Rgba(0, 0, 255, 255));
        ToolContext context = view.makeContext();
        drag(brush, context, QPointF(20, 20), QPointF(22, 22), Qt::RightButton);
        const Rgba painted = view.document()->activeLayer()->image.at(21, 21);
        CHECK(painted.b > painted.r);
    });

    addTest("painting is clipped to the selection", [] {
        auto doc = std::make_unique<Document>(60, 60, Rgba(0, 0, 0, 0));
        Mask mask(60, 60);
        for (int y = 20; y < 40; ++y)
            for (int x = 20; x < 40; ++x)
                mask.setAt(x, y, 1.0f);
        doc->selection().setMask(mask);
        CanvasView view(std::move(doc));
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        view.setTool(brush);
        view.setPrimaryColor(Rgba(255, 0, 0, 255));
        ToolContext context = view.makeContext();
        drag(brush, context, QPointF(5, 30), QPointF(55, 30));
        const Image& image = view.document()->activeLayer()->image;
        CHECK(image.at(30, 30).a > 0);  // inside the selection
        CHECK(image.at(5, 30).a == 0);  // outside it
        CHECK(image.at(55, 30).a == 0);
    });

    // ------------------------------------------------------------ selection
    addTest("selection tools build selections", [] {
        for (const char* id : {"select-rect", "select-ellipse", "lasso"}) {
            auto doc = std::make_unique<Document>(100, 100);
            CanvasView view(std::move(doc));
            Tool* tool = ToolRegistry::instance().byId(QString::fromLatin1(id));
            view.setTool(tool);
            ToolContext context = view.makeContext();
            drag(tool, context, QPointF(20, 20), QPointF(70, 60));
            check(view.document()->selection().isActive(), id, __LINE__);
            const Rect bounds = view.document()->selection().bounds();
            check(bounds.w > 10 && bounds.h > 10, id, __LINE__);
            check(view.history()->count() == 1, id, __LINE__);
            view.history()->undo();
            check(!view.document()->selection().isActive(), id, __LINE__);
        }
    });

    addTest("magic wand selects by colour", [] {
        auto doc = std::make_unique<Document>(60, 60, Rgba(255, 255, 255, 255));
        ops::fillRegion(doc->activeLayer()->image, Rect(10, 10, 20, 20),
                        Rgba(255, 0, 0, 255));
        doc->invalidate();
        CanvasView view(std::move(doc));
        Tool* wand = ToolRegistry::instance().byId(QStringLiteral("wand"));
        view.setTool(wand);
        ToolContext context = view.makeContext();
        wand->press(context, makeEvent(QPointF(15, 15)));
        wand->release(context, makeEvent(QPointF(15, 15)));
        CHECK(view.document()->selection().isActive());
        const Rect bounds = view.document()->selection().bounds();
        CHECK(bounds.x >= 9 && bounds.y >= 9);
        CHECK(bounds.w <= 22 && bounds.h <= 22);
    });

    addTest("move selected pixels", [] {
        auto doc = std::make_unique<Document>(60, 60, Rgba(0, 0, 0, 0));
        ops::fillRegion(doc->activeLayer()->image, Rect(5, 5, 10, 10),
                        Rgba(0, 255, 0, 255));
        Mask mask(60, 60);
        for (int y = 5; y < 15; ++y)
            for (int x = 5; x < 15; ++x)
                mask.setAt(x, y, 1.0f);
        doc->selection().setMask(mask);
        doc->invalidate();
        CanvasView view(std::move(doc));
        Tool* move = ToolRegistry::instance().byId(QStringLiteral("move-selected"));
        view.setTool(move);
        ToolContext context = view.makeContext();
        drag(move, context, QPointF(10, 10), QPointF(40, 40));
        move->commit(context);
        const Image& image = view.document()->activeLayer()->image;
        CHECK(image.at(10, 10).a == 0);   // the original spot is empty
        CHECK(image.at(40, 40).a > 0);    // the pixels landed here
        CHECK(view.history()->count() == 1);
        view.history()->undo();
        CHECK(view.document()->activeLayer()->image.at(10, 10).a > 0);
    });

    addTest("colour picker reports a colour", [] {
        auto doc = std::make_unique<Document>(20, 20, Rgba(12, 34, 56, 255));
        CanvasView view(std::move(doc));
        Tool* picker = ToolRegistry::instance().byId(QStringLiteral("picker"));
        view.setTool(picker);
        Rgba picked(0, 0, 0, 0);
        bool wasPrimary = false;
        QObject::connect(&view, &CanvasView::colorPicked,
                         [&](Rgba color, bool primary) {
                             picked = color;
                             wasPrimary = primary;
                         });
        ToolContext context = view.makeContext();
        picker->press(context, makeEvent(QPointF(10, 10)));
        CHECK(picked == Rgba(12, 34, 56, 255));
        CHECK(wasPrimary);
    });

    addTest("text tool writes glyphs", [] {
        auto doc = std::make_unique<Document>(220, 90, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        for (const QChar letter : QStringLiteral("Tessel")) {
            QKeyEvent key(QEvent::KeyPress, letter.unicode(), Qt::NoModifier,
                          QString(letter));
            text->keyPress(context, &key);
        }
        text->commit(context);
        CHECK(nonTransparentPixels(view.document()->activeLayer()->image) > 20);
        CHECK(view.history()->count() == 1);
    });

    addTest("committed text is kept as an editable item", [] {
        auto doc = std::make_unique<Document>(300, 120, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        typeInto(text, context, QStringLiteral("Tes"));
        pressKey(text, context, Qt::Key_Return);
        typeInto(text, context, QStringLiteral("sel"));
        text->commit(context);
        Document* document = view.document();
        CHECK(document->textItems().size() == 1);
        if (document->textItems().empty())
            return;
        const TextItem& item = document->textItems().front();
        CHECK(item.lines == (QStringList{QStringLiteral("Tes"), QStringLiteral("sel")}));
        CHECK(item.layerUid == document->activeLayer()->uid());
        CHECK(item.color == Rgba(255, 255, 255, 255));
        CHECK(nearly(item.x, 10.0) && nearly(item.y, 20.0));
        CHECK(item.box.contains(10, 20));
        CHECK(item.backdrop.width() == item.box.w);
        CHECK(textItemLive(*document, item));
    });

    addTest("clicking text with the text tool edits it again", [] {
        auto doc = std::make_unique<Document>(300, 120, Rgba(30, 60, 90, 255));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        typeInto(text, context, QStringLiteral("Tessel"));
        text->commit(context);
        const Image committed = view.document()->activeLayer()->image;
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem first = view.document()->textItems().front();

        // A click inside the dashed box picks the text up for editing, which
        // takes it out of the document until it is committed again.
        text->press(context, makeEvent(QPointF(first.x + 4, first.y + 6)));
        CHECK(view.document()->textItems().empty());
        CHECK(text->isBusy());
        // Fix a typo and put the letter back: the words, and with them every
        // pixel, have to end up where they started.
        pressKey(text, context, Qt::Key_Backspace);
        CHECK(!samePixels(view.document()->activeLayer()->image, committed));
        typeInto(text, context, QStringLiteral("l"));
        text->commit(context);
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem& again = view.document()->textItems().front();
        CHECK(again.lines == QStringList{QStringLiteral("Tessel")});
        CHECK(samePixels(view.document()->activeLayer()->image, committed));
        CHECK(textItemLive(*view.document(), again));
        // Two edits, two undo steps, and the first one still undoes cleanly.
        CHECK(view.history()->count() == 2);
        view.history()->goTo(-1);
        CHECK(view.document()->activeLayer()->image.at(12, 30)
              == Rgba(30, 60, 90, 255));
    });

    addTest("dragging inside the box moves live text", [] {
        // Wide enough, and moved far enough, that the text cannot land on top
        // of where it started: how wide a word comes out depends on the fonts
        // the machine has, and on Windows the same word is half again as wide.
        auto doc = std::make_unique<Document>(700, 120, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        typeInto(text, context, QStringLiteral("Tessel"));
        text->commit(context);
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem first = view.document()->textItems().front();
        CHECK(!first.box.isEmpty());
        CHECK(first.box.right() <= 300);

        const double shift = first.box.w + 8.0;
        text->press(context, makeEvent(QPointF(first.x + 4, first.y + 6)));
        drag(text, context, QPointF(first.x + 4, first.y + 6),
             QPointF(first.x + 4 + shift, first.y + 6));
        text->commit(context);
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem moved = view.document()->textItems().front();
        CHECK(nearly(moved.x, first.x + shift, 0.001));
        CHECK(nearly(moved.y, first.y, 0.001));
        CHECK(moved.lines == QStringList{QStringLiteral("Tessel")});
        CHECK(textItemLive(*view.document(), moved));
        // Nothing is left behind where the text used to be.
        const Image& pixels = view.document()->activeLayer()->image;
        size_t strays = 0;
        for (int y = first.box.y; y < first.box.bottom(); ++y)
            for (int x = first.box.x; x < first.box.right(); ++x)
                if (pixels.at(x, y).a != 0)
                    ++strays;
        CHECK(strays == 0);
        CHECK(nonTransparentPixels(pixels) > 20);
    });

    addTest("painting over text drops it back to pixels", [] {
        auto doc = std::make_unique<Document>(300, 120, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        QString note;
        QObject::connect(&view, &CanvasView::statusMessage, &view,
                         [&note](const QString& message) { note = message; });
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        typeInto(text, context, QStringLiteral("Tessel"));
        text->commit(context);
        CHECK(view.document()->textItems().size() == 1);

        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        view.setTool(brush);
        ToolContext brushContext = view.makeContext();
        drag(brush, brushContext, QPointF(5, 28), QPointF(150, 28));
        brush->commit(brushContext);

        // The item is only noticed on the way back to the text tool, and the
        // note has to survive the hint the tool change puts up.
        view.setTool(text);
        QImage surface(300, 120, QImage::Format_ARGB32);
        surface.fill(Qt::black);
        QPainter painter(&surface);
        ToolContext overlay = view.makeContext();
        text->paintOverlay(painter, overlay);
        painter.end();
        CHECK(view.document()->textItems().empty());
        CHECK(note.contains(QStringLiteral("no longer editable")));
        // The letters themselves stay: nothing of the user's work is lost.
        CHECK(nonTransparentPixels(view.document()->activeLayer()->image) > 20);
    });

    addTest("live text is outlined while the text tool is active", [] {
        auto doc = std::make_unique<Document>(300, 120, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(20, 30)));
        typeInto(text, context, QStringLiteral("Tessel"));
        text->commit(context);
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const Rect box = view.document()->textItems().front().box;

        // The overlay painter maps document to view, which is one to one here.
        auto litAlongTopEdge = [&view, text, box] {
            QImage surface(300, 120, QImage::Format_ARGB32);
            surface.fill(Qt::black);
            QPainter painter(&surface);
            ToolContext overlay = view.makeContext();
            text->paintOverlay(painter, overlay);
            painter.end();
            int lit = 0;
            for (int x = box.x; x < box.right(); ++x) {
                if (surface.pixelColor(x, box.y) != QColor(Qt::black))
                    ++lit;
            }
            return lit;
        };
        CHECK(litAlongTopEdge() > 4);

        // Once the pixels change the box has to go, because it is the only
        // thing telling the user the text is still live.
        Image& pixels = view.document()->activeLayer()->image;
        pixels.setAt(box.x + 2, box.y + 2, Rgba(1, 2, 3, 255));
        CHECK(litAlongTopEdge() == 0);
        CHECK(view.document()->textItems().empty());
    });

    addTest("escape leaves earlier text as it was", [] {
        auto doc = std::make_unique<Document>(300, 120, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        typeInto(text, context, QStringLiteral("Tessel"));
        text->commit(context);
        const Image committed = view.document()->activeLayer()->image;
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem first = view.document()->textItems().front();

        text->press(context, makeEvent(QPointF(first.x + 4, first.y + 6)));
        typeInto(text, context, QStringLiteral("XYZ"));
        pressKey(text, context, Qt::Key_Escape);
        CHECK(!text->isBusy());
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem& back = view.document()->textItems().front();
        CHECK(back.lines == QStringList{QStringLiteral("Tessel")});
        CHECK(samePixels(view.document()->activeLayer()->image, committed));
        CHECK(textItemLive(*view.document(), back));
        CHECK(view.history()->count() == 1);
    });

    addTest("re-edited text keeps its colour until another is chosen", [] {
        auto doc = std::make_unique<Document>(300, 120, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        typeInto(text, context, QStringLiteral("Tessel"));
        text->commit(context);
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem first = view.document()->textItems().front();
        CHECK(first.color == Rgba(255, 255, 255, 255));

        // Picking the text up while another colour sits in the palette must
        // not recolour it behind the user's back.
        view.setPrimaryColor(Rgba(255, 0, 0, 255));
        ToolContext red = view.makeContext();
        text->press(red, makeEvent(QPointF(first.x + 4, first.y + 6)));
        typeInto(text, red, QStringLiteral("!"));
        text->commit(red);
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem second = view.document()->textItems().front();
        CHECK(second.lines == QStringList{QStringLiteral("Tessel!")});
        CHECK(second.color == Rgba(255, 255, 255, 255));

        // Choosing a colour during the edit does take over.
        text->press(red, makeEvent(QPointF(second.x + 4, second.y + 6)));
        view.setPrimaryColor(Rgba(0, 0, 255, 255));
        ToolContext blue = view.makeContext();
        typeInto(text, blue, QStringLiteral("?"));
        text->commit(blue);
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        CHECK(view.document()->textItems().front().color == Rgba(0, 0, 255, 255));
    });

    addTest("the options bar re-renders the text being edited", [] {
        auto doc = std::make_unique<Document>(300, 160, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        typeInto(text, context, QStringLiteral("Tessel"));
        const size_t atTwentyFour =
            nonTransparentPixels(view.document()->activeLayer()->image);
        // The bar hands values over one at a time, with no context of its own.
        text->optionChanged(QStringLiteral("size"), 48);
        const size_t atFortyEight =
            nonTransparentPixels(view.document()->activeLayer()->image);
        CHECK(atFortyEight > atTwentyFour);
        text->optionChanged(QStringLiteral("align"), 2);
        text->commit(context);
        CHECK(view.document()->textItems().size() == 1);
        if (view.document()->textItems().empty())
            return;
        const TextItem& item = view.document()->textItems().front();
        CHECK(nearly(item.pointSize, 48.0));
        CHECK(item.align == 2);
        CHECK(textItemLive(*view.document(), item));
        // Put the bar back where the other tests expect it.
        text->optionChanged(QStringLiteral("size"), 24);
        text->optionChanged(QStringLiteral("align"), 0);
    });

    addTest("text items follow the document through save and load", [] {
        QTemporaryDir dir;
        auto doc = std::make_unique<Document>(300, 120, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* text = ToolRegistry::instance().byId(QStringLiteral("text"));
        view.setTool(text);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        text->press(context, makeEvent(QPointF(10, 20)));
        typeInto(text, context, QStringLiteral("Tessel"));
        text->commit(context);
        CHECK(view.document()->textItems().size() == 1);

        const QString path = dir.filePath(QStringLiteral("typed.tessel"));
        QString error;
        CHECK(io::saveDocument(*view.document(), path, &error));
        auto loaded = io::openDocument(path, &error);
        CHECK(loaded != nullptr);
        if (!loaded || loaded->textItems().empty())
            return;
        CHECK(loaded->textItems().size() == 1);
        CHECK(textItemLive(*loaded, loaded->textItems().front()));

        // A click on the reopened text still picks it up for editing.
        const TextItem opened = loaded->textItems().front();
        const Image before = loaded->activeLayer()->image;
        CanvasView reopened(std::move(loaded));
        reopened.setTool(text);
        reopened.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext again = reopened.makeContext();
        text->press(again, makeEvent(QPointF(opened.x + 4, opened.y + 6)));
        CHECK(text->isBusy());
        CHECK(reopened.document()->textItems().empty());
        text->commit(again);
        CHECK(reopened.document()->textItems().size() == 1);
        CHECK(samePixels(reopened.document()->activeLayer()->image, before));
    });

    // ------------------------------------------------------------ effects
    addTest("every effect applies through the dialog path", [] {
        for (Effect* effect : EffectRegistry::instance().all()) {
            auto doc = std::make_unique<Document>(40, 32);
            Image& image = doc->activeLayer()->image;
            for (int y = 0; y < 32; ++y) {
                for (int x = 0; x < 40; ++x) {
                    image.setAt(x, y, Rgba(uint8_t(x * 6), uint8_t(y * 8),
                                           uint8_t((x + y) * 3), 255));
                }
            }
            doc->invalidate();
            History history(doc.get());
            const ParamValues values = ParamValues::defaults(effect->params());
            EffectDialog::applyDirect(effect, doc.get(), &history, values,
                                      Rgba(200, 40, 40, 255), Rgba(255, 255, 255, 255));
            check(doc->activeLayer()->image.width() == 40,
                  effect->id().toLocal8Bit().constData(), __LINE__);
            // Any recorded step must undo cleanly.
            if (history.count() > 0) {
                history.undo();
                check(doc->activeLayer()->image.at(5, 5)
                          == Rgba(30, 40, 30, 255),
                      effect->id().toLocal8Bit().constData(), __LINE__);
            }
        }
    });

    addTest("threaded effect results match the direct call", [] {
        // The slow effects, which are the ones that now carry a progress
        // channel, plus two that ignore it and must still work.
        const char* ids[] = {"surface_blur", "median",     "oil_painting",
                             "crystalize",   "reduce_noise", "mandelbrot",
                             "gaussian_blur", "emboss"};
        for (const char* id : ids) {
            Effect* effect = EffectRegistry::instance().byId(QString::fromLatin1(id));
            check(effect != nullptr, id, __LINE__);
            if (!effect)
                continue;
            const ParamValues values = ParamValues::defaults(effect->params());

            // The commit path, which is what the parameterless adjustments
            // and "repeat last effect" go through.
            auto doc = gradientDocument(72, 56);
            const Image expected = directApply(effect, doc->activeLayer()->image, values,
                                               nullptr, doc->rect());
            History history(doc.get());
            EffectDialog::applyDirect(effect, doc.get(), &history, values, kPrimary,
                                      kSecondary);
            check(samePixels(doc->activeLayer()->image, expected), id, __LINE__);
            check(history.count() == 1, id, __LINE__);

            // The preview path, which settles at full resolution and is then
            // reused by the commit.
            auto other = gradientDocument(72, 56);
            History otherHistory(other.get());
            EffectSession session(effect, other.get(), &otherHistory, kPrimary,
                                  kSecondary);
            session.requestPreview(values);
            check(session.waitUntilIdle(), id, __LINE__);
            check(samePixels(session.previewImage(), expected), id, __LINE__);
            check(session.commit(values), id, __LINE__);
            check(samePixels(other->activeLayer()->image, expected), id, __LINE__);
            check(otherHistory.count() == 1, id, __LINE__);
        }
    });

    addTest("threaded effects honour the selection mask", [] {
        auto doc = gradientDocument(64, 64);
        Mask mask(64, 64);
        for (int y = 12; y < 40; ++y)
            for (int x = 8; x < 50; ++x)
                mask.setAt(x, y, y < 20 ? 0.5f : 1.0f);
        doc->selection().setMask(mask);
        History history(doc.get());
        Effect* effect = EffectRegistry::instance().byId(QStringLiteral("median"));
        const ParamValues values = ParamValues::defaults(effect->params());
        const Image expected = directApply(effect, doc->activeLayer()->image, values,
                                           doc->selection().clipMask(),
                                           doc->selection().bounds());
        EffectSession session(effect, doc.get(), &history, kPrimary, kSecondary);
        session.requestPreview(values);
        CHECK(session.waitUntilIdle());
        CHECK(samePixels(session.previewImage(), expected));
        CHECK(session.commit(values));
        CHECK(samePixels(doc->activeLayer()->image, expected));
    });

    addTest("a cheap effect stops waiting a quarter second to sharpen", [] {
        // The full resolution pass used to be held back by a fixed 250 ms
        // whatever it cost, which is what still felt slow once the work itself
        // had been made fast. The wait now follows the measured cost.
        auto doc = gradientDocument(256, 192);
        History history(doc.get());
        Effect* effect = EffectRegistry::instance().byId(QStringLiteral("brightness_contrast"));
        CHECK(effect != nullptr);
        ParamValues values = ParamValues::defaults(effect->params());
        EffectSession session(effect, doc.get(), &history, kPrimary, kSecondary);
        const int before = session.refineDelayMs();
        CHECK(before == 250);       // the cautious starting point

        session.requestPreview(values);
        CHECK(session.waitUntilIdle());
        const int after = session.refineDelayMs();
        CHECK(after < before);      // a cheap pass earns a short wait
        CHECK(after >= 60);         // but never a wait of nothing at all
    });

    addTest("large previews draft first and refine to full resolution", [] {
        auto doc = gradientDocument(192, 144);
        History history(doc.get());
        Effect* effect = EffectRegistry::instance().byId(QStringLiteral("crystalize"));
        const ParamValues values = ParamValues::defaults(effect->params());
        EffectSession session(effect, doc.get(), &history, kPrimary, kSecondary);
        // Far below the real budget, so this small test image takes the same
        // route a 20 megapixel one does.
        session.setDraftPixelBudget(2000);
        int drafts = 0;
        int fulls = 0;
        QObject::connect(&session, &EffectSession::previewUpdated, &session,
                         [&](bool full) { full ? ++fulls : ++drafts; });
        session.requestPreview(values);
        CHECK(session.waitUntilIdle());
        CHECK(drafts >= 1);
        CHECK(fulls >= 1);
        // Whatever the drafts looked like, the settled preview and the commit
        // are full resolution.
        const Image expected = directApply(effect, session.source(), values, nullptr,
                                           doc->rect());
        CHECK(samePixels(session.previewImage(), expected));
        CHECK(session.commit(values));
        CHECK(samePixels(doc->activeLayer()->image, expected));
    });

    addTest("cancelling an effect leaves the image and history alone", [] {
        auto doc = gradientDocument(320, 256);
        const Image before = doc->activeLayer()->image;
        History history(doc.get());
        Effect* effect = EffectRegistry::instance().byId(QStringLiteral("surface_blur"));
        ParamValues values = ParamValues::defaults(effect->params());
        values.set(QStringLiteral("radius"), 12);

        EffectSession session(effect, doc.get(), &history, kPrimary, kSecondary);
        session.requestPreview(values);
        // Wait for the worker to pick the job up, then stop it.
        CHECK(spinUntil([&] { return session.isBusy(); }));
        session.cancel();
        CHECK(!session.isBusy());
        session.clearPreview();
        session.restoreSource();
        CHECK(samePixels(doc->activeLayer()->image, before));
        CHECK(!doc->hasPreview());
        CHECK(history.count() == 0);
        CHECK(!history.canUndo());
    });

    addTest("rejecting the dialog records nothing", [] {
        auto doc = gradientDocument(256, 192);
        const Image before = doc->activeLayer()->image;
        History history(doc.get());
        Effect* effect = EffectRegistry::instance().byId(QStringLiteral("median"));
        auto* dialog = new EffectDialog(effect, doc.get(), &history, kPrimary, kSecondary);
        dialog->session().setDraftPixelBudget(4000);
        CHECK(spinUntil([&] { return dialog->session().isBusy(); }));
        dialog->reject();
        CHECK(samePixels(doc->activeLayer()->image, before));
        CHECK(!doc->hasPreview());
        CHECK(history.count() == 0);
        delete dialog;
        CHECK(samePixels(doc->activeLayer()->image, before));
    });

    addTest("rapid parameter changes settle on the final value", [] {
        auto doc = gradientDocument(120, 96);
        History history(doc.get());
        Effect* effect = EffectRegistry::instance().byId(QStringLiteral("median"));
        EffectDialog dialog(effect, doc.get(), &history, kPrimary, kSecondary);
        dialog.session().setDraftPixelBudget(3000);
        ParamValues values = ParamValues::defaults(effect->params());
        for (int step = 0; step < 60; ++step) {
            values.set(QStringLiteral("radius"), (step % 7) + 1);
            values.set(QStringLiteral("percentile"), 20 + (step % 5) * 15);
            dialog.setValues(values);
            // Barely any pause: this is the stampede the old debounce queued.
            QApplication::processEvents(QEventLoop::AllEvents, 1);
        }
        CHECK(dialog.session().waitUntilIdle());
        const Image expected = directApply(effect, dialog.session().source(),
                                           dialog.values(), nullptr, doc->rect());
        CHECK(samePixels(dialog.session().previewImage(), expected));
        dialog.accept();
        CHECK(samePixels(doc->activeLayer()->image, expected));
        CHECK(history.count() == 1);
        history.undo();
        CHECK(nonTransparentPixels(doc->activeLayer()->image) > 0);
    });

    addTest("the curves and levels dialogs share the threaded preview", [] {
        Effect* levels = EffectRegistry::instance().byId(QStringLiteral("levels"));
        Effect* curves = EffectRegistry::instance().byId(QStringLiteral("curves"));
        CHECK(levels != nullptr);
        CHECK(curves != nullptr);

        auto doc = gradientDocument(128, 96);
        const Image before = doc->activeLayer()->image;
        History history(doc.get());
        {
            LevelsDialog dialog(levels, doc.get(), &history, kPrimary, kSecondary);
            // The defaults are the identity, so the input black point is
            // moved to get something worth committing.
            const QList<QSpinBox*> spins = dialog.findChildren<QSpinBox*>();
            CHECK(spins.size() >= 4);
            if (!spins.isEmpty())
                spins.first()->setValue(40);
            const Image expected = directApply(levels, before, dialog.values(), nullptr,
                                               doc->rect());
            CHECK(dialog.session().waitUntilIdle());
            CHECK(samePixels(dialog.session().previewImage(), expected));
            CHECK(doc->hasPreview());
            dialog.accept();
            CHECK(samePixels(doc->activeLayer()->image, expected));
            CHECK(history.count() == 1);
        }
        history.undo();

        auto second = gradientDocument(128, 96);
        History curveHistory(second.get());
        {
            CurvesDialog dialog(curves, second.get(), &curveHistory, kPrimary, kSecondary);
            CHECK(dialog.session().waitUntilIdle());
            CHECK(second->hasPreview());
            dialog.reject();
            CHECK(samePixels(second->activeLayer()->image, before));
            CHECK(!second->hasPreview());
            CHECK(curveHistory.count() == 0);
        }
    });

    addTest("effects respect the selection", [] {
        auto doc = std::make_unique<Document>(40, 40, Rgba(200, 200, 200, 255));
        Mask mask(40, 40);
        for (int y = 10; y < 20; ++y)
            for (int x = 10; x < 20; ++x)
                mask.setAt(x, y, 1.0f);
        doc->selection().setMask(mask);
        History history(doc.get());
        Effect* invert = EffectRegistry::instance().byId(QStringLiteral("invert_colors"));
        EffectDialog::applyDirect(invert, doc.get(), &history, ParamValues(),
                                  Rgba(0, 0, 0, 255), Rgba(255, 255, 255, 255));
        const Image& image = doc->activeLayer()->image;
        CHECK(image.at(15, 15).r == 55);
        CHECK(image.at(2, 2).r == 200);
        CHECK(image.at(39, 39).r == 200);
    });

    // -------------------------------------- adjustment layers and effect stacks
    addTest("the new adjustment layer submenu adds a layer that acts on the rest", [] {
        MainWindow window;
        window.newDocument(40, 30, Rgba(10, 20, 30, 255));
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Document* doc = view->document();

        QMenu* menu = window.findChild<QMenu*>(QStringLiteral("newadjustmentmenu"));
        CHECK(menu != nullptr);
        if (!menu)
            return;
        QAction* invert = nullptr;
        for (QAction* candidate : menu->actions()) {
            if (candidate->text() == QStringLiteral("Invert Colors"))
                invert = candidate;
        }
        // The submenu also carries the effects, grouped by category.
        bool hasBlurs = false;
        for (QAction* candidate : menu->actions()) {
            if (candidate->menu() && candidate->menu()->title() == QStringLiteral("Blurs"))
                hasBlurs = true;
        }
        CHECK(invert != nullptr);
        CHECK(hasBlurs);
        if (!invert)
            return;

        invert->trigger();
        CHECK(doc->layerCount() == 2);
        const Layer* adjustment = doc->layerAt(1);
        CHECK(adjustment->isAdjustment());
        CHECK(adjustment->name == QStringLiteral("Invert Colors"));
        CHECK(doc->composite().at(2, 2) == Rgba(245, 235, 225, 255));
        // The pixels below are untouched: the layer is the only change.
        CHECK(doc->layerAt(0)->image.at(2, 2) == Rgba(10, 20, 30, 255));
        // And the Layers panel shows it as a row of its own.
        LayerTree* tree = window.findChild<LayerTree*>();
        CHECK(tree != nullptr && tree->topLevelItemCount() == 2);

        QAction* undo = window.findChild<QAction*>(QStringLiteral("undo"));
        CHECK(undo != nullptr);
        if (undo) {
            undo->trigger();
            CHECK(doc->layerCount() == 1);
            CHECK(doc->composite().at(2, 2) == Rgba(10, 20, 30, 255));
        }
    });

    addTest("double clicking an adjustment layer asks for its parameters", [] {
        // An ordinary row opens the layer properties; an adjustment layer is
        // its parameters, so that is what a double click asks for instead.
        // The tree's signal is emitted directly because the offscreen platform
        // plugin does not turn a synthesised double click into one.
        MainWindow window;
        window.resize(1200, 800);
        window.newDocument(40, 30, Rgba(10, 20, 30, 255));
        window.show();
        QApplication::processEvents();
        Document* doc = window.findChildren<CanvasView*>().last()->document();
        doc->addAdjustmentLayer(QStringLiteral("invert_colors"), ParamValues(),
                                QStringLiteral("Invert"));
        LayersPanel* panel = window.findChild<LayersPanel*>();
        LayerTree* tree = window.findChild<LayerTree*>();
        CHECK(panel != nullptr && tree != nullptr);
        if (!panel || !tree)
            return;
        panel->refresh();
        QApplication::processEvents();
        CHECK(tree->topLevelItemCount() == 2);
        if (tree->topLevelItemCount() != 2)
            return;

        int adjustmentAsked = 0;
        int propertiesAsked = 0;
        QObject::connect(panel, &LayersPanel::editAdjustmentRequested, panel,
                         [&adjustmentAsked] { ++adjustmentAsked; });
        QObject::connect(panel, &LayersPanel::propertiesRequested, panel,
                         [&propertiesAsked] { ++propertiesAsked; });
        // The adjustment layer is on top, which the panel draws first.
        emit tree->itemDoubleClicked(tree->topLevelItem(0), 0);
        CHECK(adjustmentAsked == 1);
        CHECK(propertiesAsked == 0);
        emit tree->itemDoubleClicked(tree->topLevelItem(1), 0);
        CHECK(adjustmentAsked == 1);
        CHECK(propertiesAsked == 1);
    });

    addTest("an adjustment layer's parameters can be edited again", [] {
        auto doc = std::make_unique<Document>(40, 30, Rgba(100, 100, 100, 255));
        History history(doc.get());
        Effect* effect = EffectRegistry::instance().byId(
            QStringLiteral("brightness_contrast"));
        CHECK(effect != nullptr);
        if (!effect)
            return;
        Layer* layer = doc->addAdjustmentLayer(effect->id(),
                                               ParamValues::defaults(effect->params()),
                                               effect->name());
        CHECK(layer != nullptr && layer->isAdjustment());
        CHECK(doc->composite().at(2, 2) == Rgba(100, 100, 100, 255));

        auto* dialog = new StoredEffectDialog(effect, doc.get(), &history, layer, 0);
        // The parameters preview straight through the composite, so moving a
        // control is all it takes.
        QSpinBox* brightness = dialog->findChildren<QSpinBox*>().value(0);
        CHECK(brightness != nullptr);
        if (brightness) {
            brightness->setValue(20);
            CHECK(doc->composite().at(2, 2).r == 151);
            CHECK(layer->effects().front().values.intValue(
                      QStringLiteral("brightness")) == 20);
        }
        dialog->accept();
        CHECK(history.canUndo());
        CHECK(history.undoName() == QStringLiteral("Brightness / Contrast"));
        CHECK(history.undo());
        CHECK(layer->effects().front().values.intValue(QStringLiteral("brightness"))
              == 0);
        CHECK(doc->composite().at(2, 2) == Rgba(100, 100, 100, 255));
        CHECK(history.redo());
        CHECK(doc->composite().at(2, 2).r == 151);
        delete dialog;

        // Cancel puts the values back instead.
        auto* second = new StoredEffectDialog(effect, doc.get(), &history, layer, 0);
        QSpinBox* again = second->findChildren<QSpinBox*>().value(0);
        if (again)
            again->setValue(60);
        second->reject();
        CHECK(layer->effects().front().values.intValue(QStringLiteral("brightness"))
              == 20);
        CHECK(doc->composite().at(2, 2).r == 151);
        delete second;
    });

    addTest("painting with an adjustment layer active changes nothing", [] {
        // An adjustment layer owns no pixels, so the painting tools have
        // nothing to write into and must come away empty rather than reaching
        // past the end of an image that is not there.
        auto doc = std::make_unique<Document>(60, 40, Rgba(10, 20, 30, 255));
        doc->addAdjustmentLayer(QStringLiteral("invert_colors"), ParamValues(),
                                QStringLiteral("Invert"));
        CanvasView view(std::move(doc));
        Document* document = view.document();
        const Image expected = document->composite();
        for (const char* id : {"brush", "pencil", "eraser", "bucket", "gradient"}) {
            Tool* tool = ToolRegistry::instance().byId(QString::fromLatin1(id));
            if (!tool)
                continue;
            view.setTool(tool);
            view.setPrimaryColor(Rgba(255, 0, 0, 255));
            ToolContext context = view.makeContext();
            drag(tool, context, QPointF(5, 5), QPointF(50, 35));
            tool->commit(context);
            document->invalidate();
            check(samePixels(document->composite(), expected), id, __LINE__);
        }
    });

    addTest("the layer properties dialog edits the effect stack", [] {
        auto doc = std::make_unique<Document>(40, 30, Rgba(10, 20, 30, 255));
        History history(doc.get());
        Layer* layer = doc->activeNode();
        auto* dialog = new LayerPropertiesDialog(doc.get(), &history);

        QToolButton* add = dialog->findChild<QToolButton*>(QStringLiteral("addEffect"));
        QListWidget* list = dialog->findChild<QListWidget*>();
        CHECK(add != nullptr);
        CHECK(list != nullptr);
        if (!add || !list) {
            delete dialog;
            return;
        }
        QAction* invert = nullptr;
        for (QAction* candidate : add->menu()->actions()) {
            if (candidate->text() == QStringLiteral("Invert Colors"))
                invert = candidate;
        }
        CHECK(invert != nullptr);
        if (!invert) {
            delete dialog;
            return;
        }
        invert->trigger();
        CHECK(layer->effects().size() == 1);
        CHECK(list->count() == 1);
        CHECK(list->item(0)->text() == QStringLiteral("Invert Colors"));
        // The composite shows the effect while the stored pixels do not.
        CHECK(doc->composite().at(2, 2) == Rgba(245, 235, 225, 255));
        CHECK(layer->image.at(2, 2) == Rgba(10, 20, 30, 255));

        // Unchecking a row disables that entry without forgetting it.
        list->item(0)->setCheckState(Qt::Unchecked);
        CHECK(layer->effects().size() == 1);
        CHECK(!layer->effects().front().enabled);
        CHECK(doc->composite().at(2, 2) == Rgba(10, 20, 30, 255));
        list->item(0)->setCheckState(Qt::Checked);
        CHECK(doc->composite().at(2, 2) == Rgba(245, 235, 225, 255));

        // Flattening writes the result into the pixels and empties the list.
        QToolButton* flatten =
            dialog->findChild<QToolButton*>(QStringLiteral("flattenEffects"));
        CHECK(flatten != nullptr);
        if (flatten) {
            flatten->click();
            CHECK(layer->effects().empty());
            CHECK(list->count() == 0);
            CHECK(layer->image.at(2, 2) == Rgba(245, 235, 225, 255));
            CHECK(doc->composite().at(2, 2) == Rgba(245, 235, 225, 255));
        }
        // Each list action is a step of its own, so the flatten and the entry
        // can both be taken back.
        // The undo of a flatten restores the document, so the layer is looked
        // up again rather than held on to.
        CHECK(history.undo());
        CHECK(doc->layerAt(0)->image.at(2, 2) == Rgba(10, 20, 30, 255));
        CHECK(doc->layerAt(0)->effects().size() == 1);
        CHECK(doc->composite().at(2, 2) == Rgba(245, 235, 225, 255));
        delete dialog;
    });

    addTest("the properties of an adjustment layer show what it holds", [] {
        auto doc = std::make_unique<Document>(40, 30, Rgba(10, 20, 30, 255));
        History history(doc.get());
        doc->addAdjustmentLayer(QStringLiteral("brightness_contrast"), ParamValues(),
                                QStringLiteral("Brighter"));
        auto* dialog = new LayerPropertiesDialog(doc.get(), &history);
        QListWidget* list = dialog->findChild<QListWidget*>();
        CHECK(list != nullptr);
        if (list) {
            CHECK(list->count() == 1);
            CHECK(list->item(0)->text() == QStringLiteral("Brightness / Contrast"));
        }
        // Its one entry cannot be added to, reordered or removed, so those
        // buttons are not there at all.
        CHECK(dialog->findChild<QToolButton*>(QStringLiteral("addEffect")) == nullptr);
        CHECK(dialog->findChild<QToolButton*>(QStringLiteral("removeEffect")) == nullptr);
        QToolButton* edit = dialog->findChild<QToolButton*>(QStringLiteral("editEffect"));
        CHECK(edit != nullptr && edit->isEnabled());
        delete dialog;
    });

    addTest("a stack entry keeps its own parameters", [] {
        auto doc = std::make_unique<Document>(40, 30, Rgba(100, 100, 100, 255));
        History history(doc.get());
        Effect* effect = EffectRegistry::instance().byId(
            QStringLiteral("brightness_contrast"));
        Layer* layer = doc->activeNode();
        ParamValues first = ParamValues::defaults(effect->params());
        first.set(QStringLiteral("brightness"), 10);
        doc->setLayerEffects(layer, {{QStringLiteral("invert_colors"), ParamValues(), true},
                                     {effect->id(), first, true}});
        const int before = doc->composite().at(2, 2).r;

        // The dialog is opened on the second entry, so only that one moves.
        auto* dialog = new StoredEffectDialog(effect, doc.get(), &history, layer, 1);
        QSpinBox* brightness = dialog->findChildren<QSpinBox*>().value(0);
        CHECK(brightness != nullptr);
        if (brightness) {
            CHECK(brightness->value() == 10);
            brightness->setValue(30);
        }
        dialog->accept();
        CHECK(layer->effects()[0].effectId == QStringLiteral("invert_colors"));
        CHECK(layer->effects()[1].values.intValue(QStringLiteral("brightness")) == 30);
        CHECK(doc->composite().at(2, 2).r > before);
        delete dialog;
    });

    // ------------------------------------------------------------ regressions
    addTest("colour wheel picks a visible colour from black", [] {
        // The disc is drawn at full brightness, so a pick has to return what is
        // shown even when the current colour is black, which is the default
        // primary. Otherwise the wheel looks dead until a colour is chosen
        // elsewhere first.
        ColorPanel panel;
        panel.resize(240, 520);
        panel.show();
        QApplication::processEvents();
        CHECK(panel.primary() == Rgba(0, 0, 0, 255));

        ColorWheel* wheel = panel.findChild<ColorWheel*>();
        CHECK(wheel != nullptr);
        if (!wheel)
            return;
        int primarySignals = 0;
        QObject::connect(&panel, &ColorPanel::primaryChanged,
                         [&](Rgba) { ++primarySignals; });

        const QPointF at(wheel->width() * 0.72, wheel->height() * 0.34);
        QMouseEvent press(QEvent::MouseButtonPress, at, wheel->mapToGlobal(at),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(wheel, &press);
        QApplication::processEvents();

        CHECK(primarySignals > 0);
        CHECK(!(panel.primary() == Rgba(0, 0, 0, 255)));
        CHECK(ops::luminance(panel.primary()) > 0.1f);
        // The secondary path, which always worked, must keep working.
        panel.setSecondary(Rgba(0, 0, 0, 255));
        CHECK(panel.secondary() == Rgba(0, 0, 0, 255));
    });

    addTest("panning works when the image fits the viewport", [] {
        auto doc = std::make_unique<Document>(100, 80);
        CanvasView view(std::move(doc));
        view.resize(600, 420);
        view.show();
        QApplication::processEvents();
        view.setZoom(1.0);

        auto probe = [&view](QPointF at) {
            QMouseEvent move(QEvent::MouseMove, at, view.viewport()->mapToGlobal(at),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &move);
            return view.lastCursorPixel();
        };

        const QPointF spot(300, 210);
        const QPoint before = probe(spot);

        // Middle button drag, which used to do nothing at all while the whole
        // image was visible because the scroll bars had no range.
        QMouseEvent press(QEvent::MouseButtonPress, spot,
                          view.viewport()->mapToGlobal(spot), Qt::MiddleButton,
                          Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &press);
        const QPointF moved = spot + QPointF(40, 20);
        QMouseEvent drag(QEvent::MouseMove, moved, view.viewport()->mapToGlobal(moved),
                         Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &drag);
        QMouseEvent release(QEvent::MouseButtonRelease, moved,
                            view.viewport()->mapToGlobal(moved), Qt::MiddleButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &release);
        QApplication::processEvents();

        const QPoint after = probe(spot);
        CHECK(after != before);
        // Dragging right by 40 at 100% moves the image under the cursor by 40.
        CHECK(std::abs((before.x() - after.x()) - 40) <= 2);
        CHECK(std::abs((before.y() - after.y()) - 20) <= 2);
    });

    addTest("zoom keeps the anchor under the cursor when the image fits", [] {
        auto doc = std::make_unique<Document>(120, 90);
        CanvasView view(std::move(doc));
        view.resize(640, 480);
        view.show();
        QApplication::processEvents();
        view.setZoom(1.0);

        auto probe = [&view](QPointF at) {
            QMouseEvent move(QEvent::MouseMove, at, view.viewport()->mapToGlobal(at),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &move);
            return view.lastCursorPixel();
        };

        const QPointF spot(360, 200);
        const QPoint before = probe(spot);
        view.setZoom(4.0, QPointF(before.x(), before.y()));
        QApplication::processEvents();
        const QPoint after = probe(spot);
        // The pixel under the cursor should barely move, rather than the image
        // simply staying centred as it did before.
        CHECK(std::abs(after.x() - before.x()) <= 2);
        CHECK(std::abs(after.y() - before.y()) <= 2);
    });

    addTest("panning still works when the image is larger than the viewport", [] {
        auto doc = std::make_unique<Document>(2000, 1500);
        CanvasView view(std::move(doc));
        view.resize(500, 400);
        view.show();
        QApplication::processEvents();
        view.setZoom(1.0);

        auto probe = [&view](QPointF at) {
            QMouseEvent move(QEvent::MouseMove, at, view.viewport()->mapToGlobal(at),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &move);
            return view.lastCursorPixel();
        };

        const QPointF spot(250, 200);
        const QPoint before = probe(spot);
        QMouseEvent press(QEvent::MouseButtonPress, spot,
                          view.viewport()->mapToGlobal(spot), Qt::MiddleButton,
                          Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &press);
        const QPointF moved = spot + QPointF(-30, -25);
        QMouseEvent drag(QEvent::MouseMove, moved, view.viewport()->mapToGlobal(moved),
                         Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &drag);
        QApplication::processEvents();
        const QPoint after = probe(spot);
        CHECK(std::abs((before.x() - after.x()) + 30) <= 2);
        CHECK(std::abs((before.y() - after.y()) + 25) <= 2);
    });

    addTest("soft brush lays down an even stroke", [] {
        // Dabs spaced along the path and combined with a maximum used to leave
        // a visible ripple on the centre line of a soft brush. The coverage
        // along the middle of a straight stroke must now be flat.
        auto doc = std::make_unique<Document>(220, 100, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        CHECK(brush != nullptr);
        view.setTool(brush);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        // A wide brush at middling hardness: soft enough that beading showed,
        // hard enough that one pass still covers completely.
        brush->optionChanged(QStringLiteral("width"), 40);
        brush->optionChanged(QStringLiteral("hardness"), 50);

        ToolContext context = view.makeContext();
        drag(brush, context, QPointF(20, 50), QPointF(200, 50));

        const Image& result = view.document()->activeLayer()->image;
        int lowest = 255, highest = 0;
        for (int x = 40; x <= 180; ++x) {
            const int alpha = result.at(x, 50).a;
            lowest = std::min(lowest, alpha);
            highest = std::max(highest, alpha);
        }
        CHECK(lowest >= 250);          // the core of the stroke is solid
        CHECK(highest - lowest <= 2);  // and has no ripple along its length
        // The edge must still be soft: alpha falls away from the centre line.
        CHECK(result.at(110, 50).a > result.at(110, 62).a);
        CHECK(result.at(110, 62).a > result.at(110, 69).a);
        // Put the options back so later tests see the defaults.
        brush->optionChanged(QStringLiteral("width"), 12);
        brush->optionChanged(QStringLiteral("hardness"), 75);
    });

    addTest("one stroke lays down one coat, however it crosses itself", [] {
        // Where a stroke passes back over ground it has already covered, the
        // paint must come out the same as on a straight run. Adding the second
        // visit to the first darkened a patch whose edge showed as a crescent.
        auto doc = std::make_unique<Document>(200, 200, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        view.setTool(brush);
        view.setPrimaryColor(Rgba(0, 0, 0, 255));
        brush->optionChanged(QStringLiteral("width"), 40);
        brush->optionChanged(QStringLiteral("hardness"), 20);

        ToolContext context = view.makeContext();
        const QList<QPointF> path = {QPointF(40, 100), QPointF(160, 100),
                                     QPointF(100, 40), QPointF(100, 160)};
        brush->press(context, makeEvent(path.first()));
        for (int i = 1; i < path.size(); ++i) {
            ToolEvent step = makeEvent(path[i]);
            step.button = Qt::NoButton;
            brush->move(context, step);
        }
        brush->release(context, makeEvent(path.last()));
        brush->commit(context);

        const Image& image = view.document()->activeLayer()->image;
        const int plain = image.at(60, 100).a;
        CHECK(plain > 100);                                // the stroke is there
        CHECK(std::abs(image.at(100, 100).a - plain) <= 1); // crossing matches it
        // No step anywhere across the crossing either.
        int previous = image.at(100, 85).a;
        for (int y = 86; y <= 115; ++y) {
            const int alpha = image.at(100, y).a;
            check(std::abs(alpha - previous) <= 3, "smooth through the crossing",
                  __LINE__);
            previous = alpha;
        }
        // A second stroke over the same ground does build up, though.
        drag(brush, context, QPointF(40, 100), QPointF(160, 100));
        CHECK(view.document()->activeLayer()->image.at(60, 100).a > plain + 10);

        brush->optionChanged(QStringLiteral("width"), 12);
        brush->optionChanged(QStringLiteral("hardness"), 75);
    });

    addTest("a fast soft stroke has no scalloped edge", [] {
        // The pointer only reports a position every so often, so a quick drag
        // arrives as a handful of long segments. Coverage along the edge of
        // the stroke has to come out the same whatever that spacing is, or the
        // stroke grows a row of crescents down each side.
        auto doc = std::make_unique<Document>(400, 300, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        view.setTool(brush);
        view.setPrimaryColor(Rgba(0, 0, 0, 255));
        brush->optionChanged(QStringLiteral("width"), 80);
        brush->optionChanged(QStringLiteral("hardness"), 15);

        ToolContext context = view.makeContext();
        drag(brush, context, QPointF(40, 150), QPointF(360, 150));

        const Image& result = view.document()->activeLayer()->image;
        // A line through the soft part of the stroke, well inside its length.
        for (int offset : {20, 28, 34}) {
            int lowest = 255, highest = 0;
            for (int x = 100; x <= 300; ++x) {
                const int alpha = result.at(x, 150 + offset).a;
                lowest = std::min(lowest, alpha);
                highest = std::max(highest, alpha);
            }
            check(highest - lowest <= 2, "even edge along a fast stroke", __LINE__);
        }
        brush->optionChanged(QStringLiteral("width"), 12);
        brush->optionChanged(QStringLiteral("hardness"), 75);
    });

    addTest("the softest brush is a faint haze that builds up", [] {
        // At the lowest hardness one pass should barely mark the canvas, and
        // going over the same ground again should deepen it.
        auto doc = std::make_unique<Document>(200, 100, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        view.setTool(brush);
        view.setPrimaryColor(Rgba(0, 0, 0, 255));
        brush->optionChanged(QStringLiteral("width"), 40);
        brush->optionChanged(QStringLiteral("hardness"), 1);

        ToolContext context = view.makeContext();
        drag(brush, context, QPointF(20, 50), QPointF(180, 50));
        const Image& haze = view.document()->activeLayer()->image;
        const int once = haze.at(100, 50).a;
        CHECK(once > 10);   // it does mark the canvas
        CHECK(once < 90);   // but only faintly

        // And it has to look blurred rather than like a flat grey strip: the
        // coverage leaves the centre line almost level, fades the whole way
        // out to the rim, and never steps back up on the way.
        int previous = once;
        for (int y = 51; y <= 70; ++y) {
            const int alpha = haze.at(100, y).a;
            check(alpha <= previous, "the soft brush fades outwards", __LINE__);
            previous = alpha;
        }
        CHECK(once - haze.at(100, 52).a <= 4);   // level across the middle
        CHECK(haze.at(100, 66).a >= 1);          // still something near the rim
        CHECK(haze.at(100, 66).a <= once / 4);   // but only a trace of it

        drag(brush, context, QPointF(20, 50), QPointF(180, 50));
        const int twice = view.document()->activeLayer()->image.at(100, 50).a;
        CHECK(twice > once + 10);  // a second pass deepens it

        // A hard brush still covers completely in one pass.
        brush->optionChanged(QStringLiteral("hardness"), 75);
        auto solidDoc = std::make_unique<Document>(200, 100, Rgba(0, 0, 0, 0));
        CanvasView solidView(std::move(solidDoc));
        solidView.setTool(brush);
        solidView.setPrimaryColor(Rgba(0, 0, 0, 255));
        ToolContext solidContext = solidView.makeContext();
        drag(brush, solidContext, QPointF(20, 50), QPointF(180, 50));
        CHECK(solidView.document()->activeLayer()->image.at(100, 50).a == 255);
        brush->optionChanged(QStringLiteral("width"), 12);
    });

    addTest("a tablet press carries its pressure to the tool", [] {
        // Tablet events arrive addressed to the viewport, and one that nobody
        // accepts comes back as a synthesised mouse event, so this checks both
        // that the tool is reached and that it is reached only once.
        class Probe : public Tool {
        public:
            QString id() const override { return QStringLiteral("probe"); }
            QString name() const override { return QStringLiteral("Probe"); }
            void press(ToolContext&, const ToolEvent& event) override
            {
                seen.push_back(event);
            }
            void move(ToolContext&, const ToolEvent& event) override
            {
                seen.push_back(event);
            }
            void release(ToolContext&, const ToolEvent& event) override
            {
                seen.push_back(event);
            }
            void doubleClick(ToolContext&, const ToolEvent&) override { ++doubles; }
            std::vector<ToolEvent> seen;
            int doubles = 0;
        };

        Probe probe;
        auto doc = std::make_unique<Document>(120, 120, Rgba(255, 255, 255, 255));
        CanvasView view(std::move(doc));
        view.setTool(&probe);

        const QPointingDevice* pen = QPointingDevice::primaryPointingDevice();
        const auto send = [&](QEvent::Type type, QPointF at, double pressure) {
            QTabletEvent event(type, pen, at, at, pressure, 12.0f, -5.0f, 0.0f, 0.0,
                               0.0f, Qt::NoModifier, Qt::LeftButton,
                               type == QEvent::TabletRelease ? Qt::NoButton
                                                             : Qt::LeftButton);
            QApplication::sendEvent(view.viewport(), &event);
        };
        send(QEvent::TabletPress, QPointF(20, 20), 0.42);
        send(QEvent::TabletMove, QPointF(30, 24), 0.8);
        send(QEvent::TabletRelease, QPointF(40, 28), 0.0);
        QApplication::processEvents();

        CHECK(probe.seen.size() == 3);
        if (probe.seen.size() == 3) {
            CHECK(probe.seen[0].fromTablet);
            CHECK(nearly(probe.seen[0].pressure, 0.42, 1e-3));
            CHECK(nearly(probe.seen[1].pressure, 0.8, 1e-3));
            CHECK(nearly(probe.seen[1].tiltX, 12.0, 1e-3));
            CHECK(nearly(probe.seen[1].tiltY, -5.0, 1e-3));
        }

        // A mouse has no sensor, and says a full press rather than nothing.
        QMouseEvent mouse(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &mouse);
        CHECK(probe.seen.size() == 4);
        if (probe.seen.size() == 4) {
            CHECK(!probe.seen.back().fromTablet);
            CHECK(nearly(probe.seen.back().pressure, 1.0));
        }

        // Accepting the tablet events costs the double click Qt used to make
        // out of them, so a double tap has to be recognised on the way in.
        CHECK(probe.doubles == 0);
        send(QEvent::TabletPress, QPointF(50, 50), 0.5);
        send(QEvent::TabletRelease, QPointF(50, 50), 0.0);
        send(QEvent::TabletPress, QPointF(50, 50), 0.5);
        CHECK(probe.doubles == 1);
        view.setTool(nullptr);
    });

    addTest("the painting tools open with the new options switched off", [] {
        // Everything added for a tablet starts out doing nothing, so a mouse
        // paints what it has always painted until one of them is turned on.
        for (const char* id : {"brush", "pencil", "eraser"}) {
            Tool* tool = ToolRegistry::instance().byId(QString::fromLatin1(id));
            check(tool != nullptr, id, __LINE__);
            ToolOptionsBar bar;
            bar.startTool(tool);
            check(!bar.value(QStringLiteral("pressureOpacity")).toBool(), id,
                  __LINE__);
            check(bar.value(QStringLiteral("stabilise")).toInt() == 0, id, __LINE__);
        }

        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        ToolOptionsBar brushBar;
        brushBar.startTool(brush);
        CHECK(!brushBar.value(QStringLiteral("pressureWidth")).toBool());
        CHECK(brushBar.value(QStringLiteral("tip")).toInt() == 0);   // Round
        // The level the check box used to stand for when it was checked, and
        // Normal rather than Overwrite.
        CHECK(brushBar.value(QStringLiteral("antialias")).toInt() == 2);
        CHECK(brushBar.value(QStringLiteral("blend")).toInt() == 0);

        // The pencil nib is one pixel, so it is not offered a width switch at
        // all rather than one that does nothing.
        Tool* pencil = ToolRegistry::instance().byId(QStringLiteral("pencil"));
        ToolOptionsBar pencilBar;
        pencilBar.startTool(pencil);
        CHECK(!pencilBar.value(QStringLiteral("pressureWidth")).isValid());
    });

    addTest("pressure drives the brush width and the opacity when asked", [] {
        // Both switches are off to start with, so a light touch has to paint
        // exactly what a mouse paints until one of them is turned on.
        auto doc = std::make_unique<Document>(240, 220, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        view.setTool(brush);
        view.setPrimaryColor(Rgba(0, 0, 0, 255));
        brush->optionChanged(QStringLiteral("width"), 40);
        brush->optionChanged(QStringLiteral("hardness"), 95);
        ToolContext context = view.makeContext();

        const Image& image = view.document()->activeLayer()->image;
        const auto spread = [&image](int x, int y) {
            int count = 0;
            for (int dy = -40; dy <= 40; ++dy) {
                if (image.at(x, y + dy).a > 128)
                    ++count;
            }
            return count;
        };

        penDrag(brush, context, QPointF(20, 40), QPointF(220, 40), 0.3);
        const int ignored = spread(120, 40);
        CHECK(image.at(120, 40).a == 255);
        CHECK(ignored >= 38 && ignored <= 42);

        brush->optionChanged(QStringLiteral("pressureWidth"), true);
        penDrag(brush, context, QPointF(20, 110), QPointF(220, 110), 0.3);
        const int narrow = spread(120, 110);
        CHECK(image.at(120, 110).a == 255);   // a light touch still marks
        CHECK(narrow >= 8 && narrow < ignored / 2);
        // Full pressure is the width that was asked for.
        penDrag(brush, context, QPointF(20, 180), QPointF(220, 180), 1.0);
        CHECK(spread(120, 180) == ignored);
        brush->optionChanged(QStringLiteral("pressureWidth"), false);

        // Opacity is the other mapping: the width stays put and the pigment
        // thins out instead.
        auto thinDoc = std::make_unique<Document>(240, 100, Rgba(0, 0, 0, 0));
        CanvasView thinView(std::move(thinDoc));
        thinView.setTool(brush);
        thinView.setPrimaryColor(Rgba(0, 0, 0, 255));
        ToolContext thinContext = thinView.makeContext();
        brush->optionChanged(QStringLiteral("pressureOpacity"), true);
        penDrag(brush, thinContext, QPointF(20, 50), QPointF(220, 50), 0.3);
        const Image& thin = thinView.document()->activeLayer()->image;
        CHECK(thin.at(120, 50).a > 60 && thin.at(120, 50).a < 130);
        int faint = 0;
        for (int dy = -40; dy <= 40; ++dy) {
            if (thin.at(120, 50 + dy).a > 40)
                ++faint;
        }
        CHECK(faint >= 38 && faint <= 42);   // as wide as ever
        brush->optionChanged(QStringLiteral("pressureOpacity"), false);

        // The pencil has the one switch, since a single pixel nib has no
        // width to scale.
        Tool* pencil = ToolRegistry::instance().byId(QStringLiteral("pencil"));
        thinView.setTool(pencil);
        pencil->optionChanged(QStringLiteral("pressureOpacity"), true);
        ToolContext pencilContext = thinView.makeContext();
        penDrag(pencil, pencilContext, QPointF(20, 90), QPointF(220, 90), 0.5);
        CHECK(thin.at(120, 90).a > 100 && thin.at(120, 90).a < 170);
        pencil->optionChanged(QStringLiteral("pressureOpacity"), false);

        // And so does the eraser, which takes less away at a light touch.
        Tool* eraser = ToolRegistry::instance().byId(QStringLiteral("eraser"));
        auto fullDoc = std::make_unique<Document>(240, 120, Rgba(0, 0, 0, 255));
        CanvasView fullView(std::move(fullDoc));
        fullView.setTool(eraser);
        ToolContext eraseContext = fullView.makeContext();
        eraser->optionChanged(QStringLiteral("width"), 30);
        eraser->optionChanged(QStringLiteral("pressureOpacity"), true);
        penDrag(eraser, eraseContext, QPointF(20, 40), QPointF(220, 40), 0.3);
        penDrag(eraser, eraseContext, QPointF(20, 90), QPointF(220, 90), 1.0);
        const Image& erased = fullView.document()->activeLayer()->image;
        CHECK(erased.at(120, 40).a > 100 && erased.at(120, 40).a < 200);
        CHECK(erased.at(120, 90).a == 0);
        eraser->optionChanged(QStringLiteral("pressureOpacity"), false);
        eraser->optionChanged(QStringLiteral("width"), 24);

        brush->optionChanged(QStringLiteral("width"), 12);
        brush->optionChanged(QStringLiteral("hardness"), 75);
    });

    addTest("stabilisation steadies a shaky line and still ends where it ended", [] {
        // The same wobbling drag twice: untouched it follows every twitch, and
        // turned up it comes out as a line, but either way the last stretch
        // has to reach the point where the pointer came up.
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        brush->optionChanged(QStringLiteral("width"), 6);
        brush->optionChanged(QStringLiteral("hardness"), 100);

        const auto shake = [brush](CanvasView& view) {
            ToolContext context = view.makeContext();
            brush->press(context, makeEvent(QPointF(20, 60)));
            for (int i = 1; i <= 16; ++i) {
                const double x = 20.0 + i * 10.0;
                const double y = 60.0 + ((i % 2) ? 10.0 : -10.0);
                ToolEvent step = makeEvent(QPointF(x, y));
                step.button = Qt::NoButton;
                brush->move(context, step);
            }
            brush->release(context, makeEvent(QPointF(180, 60)));
        };
        // How far from the middle line the paint reaches, over the stretch
        // where the wobble is in full swing.
        const auto wander = [](const Image& image) {
            int worst = 0;
            for (int x = 60; x <= 150; ++x) {
                for (int dy = 1; dy <= 25; ++dy) {
                    if (image.at(x, 60 + dy).a > 128 || image.at(x, 60 - dy).a > 128)
                        worst = std::max(worst, dy);
                }
            }
            return worst;
        };

        auto rawDoc = std::make_unique<Document>(220, 140, Rgba(0, 0, 0, 0));
        CanvasView rawView(std::move(rawDoc));
        rawView.setTool(brush);
        rawView.setPrimaryColor(Rgba(0, 0, 0, 255));
        shake(rawView);
        const int loose = wander(rawView.document()->activeLayer()->image);
        CHECK(loose >= 10);   // the wobble is all there

        auto steadyDoc = std::make_unique<Document>(220, 140, Rgba(0, 0, 0, 0));
        CanvasView steadyView(std::move(steadyDoc));
        steadyView.setTool(brush);
        steadyView.setPrimaryColor(Rgba(0, 0, 0, 255));
        brush->optionChanged(QStringLiteral("stabilise"), 100);
        shake(steadyView);
        const Image& steady = steadyView.document()->activeLayer()->image;
        CHECK(wander(steady) <= 4);
        CHECK(steady.at(100, 60).a == 255);   // and it is still a line
        // The release point is painted whatever the smoothing was holding back.
        CHECK(steady.at(178, 60).a > 128);

        brush->optionChanged(QStringLiteral("stabilise"), 0);
        brush->optionChanged(QStringLiteral("width"), 12);
        brush->optionChanged(QStringLiteral("hardness"), 75);
    });

    addTest("a custom brush tip is stamped along the stroke", [] {
        // A tip that is nothing like a disc, so the stroke it leaves cannot be
        // mistaken for the round brush's swept field: a bar four times wider
        // than it is tall.
        QTemporaryDir dir;
        CHECK(dir.isValid());
        QImage bar(64, 64, QImage::Format_ARGB32);
        bar.fill(Qt::transparent);
        for (int y = 22; y < 42; ++y) {
            for (int x = 0; x < 64; ++x)
                bar.setPixelColor(x, y, QColor(255, 255, 255, 255));
        }
        const QString path = dir.filePath(QStringLiteral("bar.png"));
        CHECK(bar.save(path, "PNG"));

        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        brush->optionChanged(QStringLiteral("width"), 40);
        brush->optionChanged(QStringLiteral("hardness"), 100);
        brush->optionChanged(QStringLiteral("tipFile"), path);

        auto doc = std::make_unique<Document>(220, 140, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        view.setTool(brush);
        view.setPrimaryColor(Rgba(0, 0, 0, 255));
        ToolContext context = view.makeContext();
        drag(brush, context, QPointF(40, 70), QPointF(180, 70));

        const Image& image = view.document()->activeLayer()->image;
        CHECK(image.at(110, 70).a == 255);
        CHECK(image.at(110, 74).a == 255);
        // The bar is thirteen pixels tall once it is fitted to a forty pixel
        // brush, where the round tip would have painted all the way out.
        CHECK(image.at(110, 85).a == 0);
        CHECK(image.at(110, 55).a == 0);

        // No beads: a soft tip has to come out as even along the stroke as the
        // round brush does.
        auto softDoc = std::make_unique<Document>(220, 140, Rgba(0, 0, 0, 0));
        CanvasView softView(std::move(softDoc));
        softView.setTool(brush);
        softView.setPrimaryColor(Rgba(0, 0, 0, 255));
        ToolContext softContext = softView.makeContext();
        brush->optionChanged(QStringLiteral("hardness"), 50);
        drag(brush, softContext, QPointF(40, 70), QPointF(180, 70));
        const Image& soft = softView.document()->activeLayer()->image;
        for (int row : {70, 74, 78}) {
            int lowest = 255, highest = 0;
            for (int x = 70; x <= 150; ++x) {
                lowest = std::min(lowest, int(soft.at(x, row).a));
                highest = std::max(highest, int(soft.at(x, row).a));
            }
            check(highest - lowest <= 3, "even along a stamped stroke", __LINE__);
            check(highest > 20, "and there is paint to measure", __LINE__);
        }
        // Hardness thins the pigment the same way it does for the round brush.
        CHECK(soft.at(110, 70).a < 255);

        // Opacity and the blending mode go through the same path as ever.
        auto overDoc = std::make_unique<Document>(220, 140, Rgba(128, 128, 128, 255));
        CanvasView overView(std::move(overDoc));
        overView.setTool(brush);
        overView.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext overContext = overView.makeContext();
        brush->optionChanged(QStringLiteral("hardness"), 100);
        brush->optionChanged(QStringLiteral("opacity"), 50);
        drag(brush, overContext, QPointF(40, 40), QPointF(180, 40));
        const Image& over = overView.document()->activeLayer()->image;
        CHECK(over.at(110, 40).r > 180 && over.at(110, 40).r < 215);
        brush->optionChanged(QStringLiteral("opacity"), 100);
        // Multiply by white leaves a mid grey exactly where it was.
        brush->optionChanged(QStringLiteral("blend"),
                             int(blendModeNames().indexOf(QStringLiteral("Multiply"))));
        drag(brush, overContext, QPointF(40, 100), QPointF(180, 100));
        CHECK(over.at(110, 100).r == 128);

        brush->optionChanged(QStringLiteral("blend"), 0);
        brush->optionChanged(QStringLiteral("tip"), 0);
        brush->optionChanged(QStringLiteral("width"), 12);
        brush->optionChanged(QStringLiteral("hardness"), 75);
        brushtips::forgetAll();
    });

    addTest("a brush tip without alpha comes from its luminance", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        // No alpha channel at all, so the bright part is the stamp.
        QImage plain(40, 40, QImage::Format_RGB32);
        plain.fill(Qt::black);
        for (int y = 10; y < 30; ++y) {
            for (int x = 12; x < 28; ++x)
                plain.setPixelColor(x, y, QColor(255, 255, 255));
        }
        const QString path = dir.filePath(QStringLiteral("plain.png"));
        CHECK(plain.save(path, "PNG"));

        const Mask tip = brushtips::load(path);
        // Trimmed to what it marks, which is the bright block and nothing else.
        CHECK(tip.width() == 16 && tip.height() == 20);
        CHECK(tip.at(8, 10) > 0.99f);
        CHECK(tip.maxValue() > 0.99f);

        // An unreadable file is not remembered, and the list is left alone.
        const QStringList before = brushtips::remembered();
        CHECK(!brushtips::remember(dir.filePath(QStringLiteral("nothing.png"))));
        CHECK(brushtips::remembered() == before);
    });

    addTest("a custom brush tip is remembered between runs", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        QImage disc(48, 48, QImage::Format_ARGB32);
        disc.fill(Qt::transparent);
        for (int y = 0; y < 48; ++y) {
            for (int x = 0; x < 48; ++x) {
                if (std::hypot(x - 23.5, y - 23.5) <= 20.0)
                    disc.setPixelColor(x, y, QColor(0, 0, 0, 255));
            }
        }
        const QString path = dir.filePath(QStringLiteral("disc.png"));
        CHECK(disc.save(path, "PNG"));

        brushtips::forgetAll();
        CHECK(brushtips::remembered().isEmpty());
        CHECK(brushtips::chosen().isEmpty());
        CHECK(brushtips::remember(path));
        CHECK(brushtips::chosen() == path);

        // It went through the settings, which is what makes the next run find
        // it again.
        CHECK(QSettings().value(QStringLiteral("tools/brushTips"))
                  .toStringList()
                  .contains(path));
        brushtips::reload();
        CHECK(brushtips::remembered() == QStringList{path});
        CHECK(brushtips::chosen() == path);

        // The options bar offers Round, the remembered tip and the chooser,
        // and opens on the tip that is in use.
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        ToolOptionsBar bar;
        bar.startTool(brush);
        CHECK(bar.value(QStringLiteral("tip")).toInt() == 1);
        bool sawRound = false;
        bool sawTip = false;
        for (QComboBox* combo : bar.findChildren<QComboBox*>()) {
            sawRound = sawRound || combo->findText(QStringLiteral("Round")) >= 0;
            sawTip = sawTip || combo->findText(QStringLiteral("disc")) >= 0;
        }
        CHECK(sawRound);
        CHECK(sawTip);

        brush->optionChanged(QStringLiteral("tip"), 0);
        CHECK(brushtips::chosen().isEmpty());
        brushtips::forgetAll();
    });

    addTest("Overwrite blending replaces what is under the brush", [] {
        // Painting over an opaque field with a soft brush: composited it stays
        // opaque, overwritten it takes the coverage of the brush with it, so
        // the field can be faded out to the stroke colour.
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        brush->optionChanged(QStringLiteral("width"), 40);
        brush->optionChanged(QStringLiteral("hardness"), 20);

        ToolOptionsBar bar;
        bar.startTool(brush);
        bool sawOverwrite = false;
        for (QComboBox* combo : bar.findChildren<QComboBox*>())
            sawOverwrite = sawOverwrite || combo->findText(QStringLiteral("Overwrite")) >= 0;
        CHECK(sawOverwrite);

        auto normalDoc = std::make_unique<Document>(220, 120, Rgba(255, 0, 0, 255));
        CanvasView normalView(std::move(normalDoc));
        normalView.setTool(brush);
        normalView.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext normalContext = normalView.makeContext();
        drag(brush, normalContext, QPointF(40, 60), QPointF(180, 60));
        const Rgba composited = normalView.document()->activeLayer()->image.at(110, 60);
        CHECK(composited.a == 255);     // nothing was taken away
        CHECK(composited.g < 255);      // and the red is still showing through

        auto doc = std::make_unique<Document>(220, 120, Rgba(255, 0, 0, 255));
        CanvasView view(std::move(doc));
        view.setTool(brush);
        view.setPrimaryColor(Rgba(255, 255, 255, 255));
        ToolContext context = view.makeContext();
        brush->optionChanged(QStringLiteral("blend"), int(blendModeNames().size()));
        drag(brush, context, QPointF(40, 60), QPointF(180, 60));

        const Image& image = view.document()->activeLayer()->image;
        const Rgba middle = image.at(110, 60);
        CHECK(middle.r == 255 && middle.g == 255 && middle.b == 255);
        CHECK(middle.a > 100 && middle.a < 200);   // the coverage, not opacity
        CHECK(image.at(110, 78).a < 40);           // faded right out at the rim
        CHECK(image.at(110, 100) == Rgba(255, 0, 0, 255));   // and untouched outside

        brush->optionChanged(QStringLiteral("blend"), 0);
        brush->optionChanged(QStringLiteral("width"), 12);
        brush->optionChanged(QStringLiteral("hardness"), 75);
    });

    addTest("the antialiasing level sets how wide the edge ramp is", [] {
        // Level zero is what the old unchecked box did: the hardness slider is
        // ignored and the brush covers completely. The levels above it widen
        // the narrowest ramp an edge is allowed to come down to, and the wider
        // that ramp the more of its coverage the rim gives up, which is the
        // one number that counts the whole edge at once. The stroke runs on a
        // slant so its edge meets the pixel grid at every phase; an edge laid
        // straight along a row only ever lands one way and cannot tell a third
        // of a pixel from half of one.
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        brush->optionChanged(QStringLiteral("width"), 40);
        brush->optionChanged(QStringLiteral("hardness"), 100);

        const auto inkAt = [brush](const QVariant& level) {
            auto doc = std::make_unique<Document>(240, 240, Rgba(0, 0, 0, 0));
            CanvasView view(std::move(doc));
            view.setTool(brush);
            view.setPrimaryColor(Rgba(0, 0, 0, 255));
            ToolContext context = view.makeContext();
            brush->optionChanged(QStringLiteral("antialias"), level);
            drag(brush, context, QPointF(30, 30), QPointF(210, 150));
            const Image& image = view.document()->activeLayer()->image;
            long long total = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x)
                    total += image.at(x, y).a;
            }
            return total;
        };

        long long previous = inkAt(1);
        for (int level = 2; level <= 4; ++level) {
            const long long ink = inkAt(level);
            check(ink < previous, "a higher level gives up more of the rim",
                  __LINE__);
            previous = ink;
        }
        // What the check box used to mean is still exactly what it means.
        CHECK(inkAt(2) == inkAt(QVariant(true)));
        CHECK(inkAt(0) == inkAt(QVariant(false)));

        // And the widest ramp is a ramp where the default is still solid.
        const auto edgeAlpha = [brush](const QVariant& level) {
            auto doc = std::make_unique<Document>(240, 120, Rgba(0, 0, 0, 0));
            CanvasView view(std::move(doc));
            view.setTool(brush);
            view.setPrimaryColor(Rgba(0, 0, 0, 255));
            ToolContext context = view.makeContext();
            brush->optionChanged(QStringLiteral("antialias"), level);
            drag(brush, context, QPointF(30, 60), QPointF(210, 60));
            return int(view.document()->activeLayer()->image.at(120, 79).a);
        };
        CHECK(edgeAlpha(2) == 255);
        CHECK(edgeAlpha(4) > 16 && edgeAlpha(4) < 160);

        // At level zero the hardness slider has nothing to say, so even the
        // softest brush covers in one pass.
        brush->optionChanged(QStringLiteral("hardness"), 1);
        brush->optionChanged(QStringLiteral("antialias"), 0);
        auto hardDoc = std::make_unique<Document>(240, 120, Rgba(0, 0, 0, 0));
        CanvasView hardView(std::move(hardDoc));
        hardView.setTool(brush);
        hardView.setPrimaryColor(Rgba(0, 0, 0, 255));
        ToolContext hardContext = hardView.makeContext();
        drag(brush, hardContext, QPointF(30, 60), QPointF(210, 60));
        CHECK(hardView.document()->activeLayer()->image.at(120, 60).a == 255);

        brush->optionChanged(QStringLiteral("antialias"), 2);
        brush->optionChanged(QStringLiteral("width"), 12);
        brush->optionChanged(QStringLiteral("hardness"), 75);
    });

    addTest("a crowded column scrolls the Colors panel instead of flattening it", [] {
        // The wheel, the value and alpha bars, the recent row and the palette
        // do not all fit beside History and Layers in a short window. The
        // layout used to squeeze the rows of swatches to nothing, which is
        // what the panel loses first and misses most.
        MainWindow window;
        window.newDocument(400, 300, Rgba(255, 255, 255, 255));
        window.resize(1400, 760);
        window.show();
        QApplication::processEvents();
        // The panels remember where they were left, and the settings carry
        // over between runs, so this starts from the arrangement that ships.
        window.resetPanelLayout();
        QApplication::processEvents();

        auto* colors = window.findChild<ColorPanel*>();
        CHECK(colors != nullptr);
        auto* scroll = window.findChild<QScrollArea*>();
        CHECK(scroll != nullptr);
        if (!colors || !scroll)
            return;
        // Whatever the dock can spare, the panel keeps its own height.
        CHECK(colors->height() >= colors->minimumSizeHint().height());
        CHECK(colors->minimumSizeHint().height() > 300);
        // Which means the swatches are drawn at a size worth clicking.
        for (PaletteGrid* grid : window.findChildren<PaletteGrid*>()) {
            check(grid->height() >= 14, "a row of swatches is not flattened",
                  __LINE__);
        }
        // And when that does not fit, the dock scrolls to it.
        if (scroll->viewport()->height() < colors->height())
            CHECK(scroll->verticalScrollBar()->isVisible());
    });

    addTest("the tools palette is no taller than its buttons", [] {
        // A panel used to own a column of the window whatever it had in it,
        // which left the palette stretched down the whole side with most of
        // it empty.
        MainWindow window;
        window.newDocument(400, 300, Rgba(255, 255, 255, 255));
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
        // The panels remember where they were left, and the settings carry
        // over between runs, so this starts from the arrangement that ships.
        window.resetPanelLayout();
        QApplication::processEvents();
        auto* tools = window.findChild<ToolsPalette*>();
        auto* panel = window.findChild<Panel*>(QStringLiteral("toolsPanel"));
        CHECK(tools != nullptr && panel != nullptr);
        if (!tools || !panel)
            return;
        CHECK(tools->height() <= tools->maximumHeight());
        CHECK(tools->height() < 500);          // not the height of the window
        CHECK(tools->height() > 250);          // but enough for ten rows
        CHECK(panel->height() <= tools->maximumHeight() + 60);
        CHECK(panel->height() < 500);
        // The title bar is ours, so its button is an ordinary tool button the
        // theme lights up, at a size worth aiming at, and it does its job.
        auto* bar = panel->findChild<QWidget*>(QStringLiteral("panelTitleBar"));
        CHECK(bar != nullptr);
        if (!bar)
            return;
        auto* close = bar->findChild<QToolButton*>(QStringLiteral("panelCloseButton"));
        auto* title = bar->findChild<QLabel*>(QStringLiteral("panelTitle"));
        CHECK(close != nullptr && title != nullptr);
        if (!close || !title)
            return;
        check(close->width() >= 22 && close->height() >= 22,
              "a panel title button is big enough to hit", __LINE__);
        check(!close->icon().isNull(), "a panel title button has an icon", __LINE__);
        CHECK(!title->text().isEmpty());
        // The title itself takes no mouse events, so the whole bar drags.
        CHECK(title->testAttribute(Qt::WA_TransparentForMouseEvents));
        close->click();
        QApplication::processEvents();
        CHECK(!panel->isVisible());
        panel->show();
    });

    addTest("the panels sit in the corners and the canvas runs under them", [] {
        // Qt's docking gives a panel a strip down one side of the window and
        // that strip the full height, whatever is in the panel. These are in
        // the four corners instead, over a canvas that runs the whole width.
        MainWindow window;
        window.newDocument(400, 300, Rgba(255, 255, 255, 255));
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
        // The panels remember where they were left, and the settings carry
        // over between runs, so this starts from the arrangement that ships.
        window.resetPanelLayout();
        QApplication::processEvents();

        auto* view = window.findChild<CanvasView*>();
        auto* panel = window.findChild<Panel*>(QStringLiteral("toolsPanel"));
        CHECK(view != nullptr && panel != nullptr);
        if (!view || !panel)
            return;
        QWidget* canvas = view->parentWidget(); // the stack of open images
        CHECK(canvas != nullptr);
        if (!canvas)
            return;
        CHECK(canvas->x() == 0);
        CHECK(canvas->x() + canvas->width() == window.width());

        // Each one is in its own corner of that band, near the edges.
        const QRect area = canvas->geometry();
        const std::pair<const char*, QPoint> corners[] = {
            {"toolsPanel", QPoint(0, 0)},
            {"historyPanel", QPoint(1, 0)},
            {"colorsPanel", QPoint(0, 1)},
            {"layersPanel", QPoint(1, 1)}};
        for (const auto& [name, corner] : corners) {
            auto* one = window.findChild<Panel*>(QString::fromLatin1(name));
            if (!one) {
                check(false, "the panel is there", __LINE__);
                continue;
            }
            const QRect box = one->geometry();
            check(box.width() < area.width() / 2,
                  "a panel is not half the window wide", __LINE__);
            check(corner.x() == 0 ? box.left() < area.left() + 24
                                  : box.right() > area.right() - 24,
                  "the panel is against the side it belongs to", __LINE__);
            check(corner.y() == 0 ? box.top() < area.top() + 24
                                  : box.bottom() > area.bottom() - 24,
                  "the panel is against the end it belongs to", __LINE__);
        }

        // The panel stands over the canvas, and is the one that answers the
        // pointer, while next to it the same band is canvas.
        CHECK(panel->geometry().intersects(canvas->geometry()));
        QWidget* hit = window.childAt(panel->geometry().center());
        CHECK(hit != nullptr && (hit == panel || panel->isAncestorOf(hit)));
        // Below where it ends, with the corner below it out of the way, the
        // same column of the window is canvas.
        if (auto* below = window.findChild<Panel*>(QStringLiteral("colorsPanel")))
            below->hide();
        QApplication::processEvents();
        const QPoint under(panel->geometry().center().x(),
                           panel->geometry().bottom() + 40);
        hit = window.childAt(under);
        CHECK(hit != nullptr && (hit == view || view->isAncestorOf(hit)));

        // And a panel paints itself. The image runs under the panels now, so
        // any part of one that left its background to the window behind would
        // show the image through, with the text on it unreadable. The panel
        // is drawn over a colour nothing in the theme uses, with the window
        // behind left out of the drawing: whatever is left of that colour is
        // what the image would be seen through.
        if (QWidget* inside = panel->content())
            inside->setFixedSize(24, 24); // leave most of the frame bare
        QApplication::processEvents();
        QPixmap sheet(panel->size());
        sheet.fill(QColor(255, 0, 255));
        panel->render(&sheet, QPoint(), QRegion(), QWidget::DrawChildren);
        const QImage drawn = sheet.toImage();
        const QPoint probes[] = {QPoint(panel->width() - 2, panel->height() - 2),
                                 QPoint(panel->width() / 2, panel->height() / 2),
                                 QPoint(1, panel->height() - 2)};
        for (const QPoint& probe : probes) {
            check(drawn.pixelColor(probe) != QColor(255, 0, 255),
                  "the panel paints every part of itself", __LINE__);
        }
    });

    addTest("a panel dragged by its title lands in the corner it was let go in", [] {
        MainWindow window;
        window.newDocument(400, 300, Rgba(255, 255, 255, 255));
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
        // The panels remember where they were left, and the settings carry
        // over between runs, so this starts from the arrangement that ships.
        window.resetPanelLayout();
        QApplication::processEvents();

        auto* panel = window.findChild<Panel*>(QStringLiteral("toolsPanel"));
        CHECK(panel != nullptr);
        if (!panel)
            return;
        auto* bar = panel->findChild<QWidget*>(QStringLiteral("panelTitleBar"));
        CHECK(bar != nullptr);
        if (!bar)
            return;
        CHECK(panel->corner() == Corner::TopLeft);

        // Taken by the title bar, carried to the far corner, let go of.
        const QPoint grab(bar->width() / 2, bar->height() / 2);
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, grab);
        const QPoint far(window.width() - 60, window.height() - 120);
        QTest::mouseMove(bar, bar->mapFrom(&window, far));
        QApplication::processEvents();
        // The targets are shown while it is in the air.
        auto* hint = window.findChild<PanelDropHint*>();
        CHECK(hint != nullptr && hint->isVisible());
        if (hint)
            CHECK(hint->active().has_value() && *hint->active() == Corner::BottomRight);
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier,
                            bar->mapFrom(&window, far));
        QApplication::processEvents();

        CHECK(panel->corner() == Corner::BottomRight);
        CHECK(hint == nullptr || !hint->isVisible());
        const QRect area = window.findChild<CanvasView*>()->parentWidget()->geometry();
        CHECK(panel->geometry().right() > area.right() - 24);
        // Right into the corner, with what was already there moved up.
        CHECK(panel->geometry().bottom() > area.bottom() - 24);
        // And the one that was already there made room for it rather than
        // being covered by it.
        auto* layers = window.findChild<Panel*>(QStringLiteral("layersPanel"));
        CHECK(layers != nullptr);
        if (layers)
            CHECK(!layers->geometry().intersects(panel->geometry()));
    });

    addTest("a panel let go of away from the corners floats where it was put", [] {
        MainWindow window;
        window.newDocument(400, 300, Rgba(255, 255, 255, 255));
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
        window.resetPanelLayout();
        QApplication::processEvents();

        auto* panel = window.findChild<Panel*>(QStringLiteral("toolsPanel"));
        CHECK(panel != nullptr);
        if (!panel)
            return;
        auto* bar = panel->findChild<QWidget*>(QStringLiteral("panelTitleBar"));
        CHECK(bar != nullptr && !panel->isFloating());
        if (!bar)
            return;

        const QPoint middle(window.width() / 2, window.height() / 2);
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier,
                          QPoint(bar->width() / 2, bar->height() / 2));
        QTest::mouseMove(bar, bar->mapFrom(&window, middle));
        QApplication::processEvents();
        // Nothing is picked out, because the middle is not a corner.
        auto* hint = window.findChild<PanelDropHint*>();
        CHECK(hint != nullptr && hint->isVisible());
        if (hint)
            CHECK(!hint->active().has_value());
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier,
                            bar->mapFrom(&window, middle));
        QApplication::processEvents();

        CHECK(panel->isFloating());
        // Where it was let go of, near enough: the pointer held the title bar
        // rather than the top left of the panel.
        CHECK(std::abs(panel->geometry().center().x() - middle.x()) < 200);
        CHECK(panel->y() < middle.y() && panel->geometry().bottom() > middle.y() - 40);
        // And it stays in the same part of the window when the window changes
        // size, rather than being left off the edge of it.
        const QRect area = window.findChild<CanvasView*>()->parentWidget()->geometry();
        const double across = double(panel->x() - area.left()) / area.width();
        window.resize(1000, 700);
        QApplication::processEvents();
        const QRect smaller = window.findChild<CanvasView*>()->parentWidget()->geometry();
        CHECK(smaller.contains(panel->geometry()));
        CHECK(std::abs(double(panel->x() - smaller.left()) / smaller.width() - across)
              < 0.08);

        // The pin in the title bar says the same thing without the dragging:
        // it puts a floating panel back into the corner it stands nearest,
        // and lets a pinned one go.
        auto* pin = bar->findChild<QToolButton*>(QStringLiteral("panelPinButton"));
        CHECK(pin != nullptr);
        if (!pin)
            return;
        pin->click();
        QApplication::processEvents();
        CHECK(!panel->isFloating());
        CHECK(panel->geometry().left() < smaller.left() + 24
              || panel->geometry().right() > smaller.right() - 24);
        pin->click();
        QApplication::processEvents();
        CHECK(panel->isFloating());
    });

    addTest("panels down one side of the window never stand on each other", [] {
        // A list that grows used to push its panel through the one above it:
        // each corner was given the whole height to share out.
        // A short window, so the panels on one side have to share what there
        // is between them rather than each taking what it would like.
        MainWindow window;
        window.newDocument(400, 300, Rgba(255, 255, 255, 255));
        window.resize(1100, 600);
        window.show();
        QApplication::processEvents();
        window.resetPanelLayout();
        QApplication::processEvents();

        auto* layers = window.findChild<LayersPanel*>();
        CHECK(layers != nullptr);
        if (!layers)
            return;
        for (int i = 0; i < 12; ++i)
            QMetaObject::invokeMethod(layers, "addLayerRequested");
        QApplication::processEvents();
        QApplication::processEvents();

        QList<Panel*> shown;
        for (Panel* panel : window.findChildren<Panel*>()) {
            if (panel->isVisible())
                shown.append(panel);
        }
        CHECK(shown.size() == 4);
        for (int i = 0; i < shown.size(); ++i) {
            for (int j = i + 1; j < shown.size(); ++j) {
                check(!shown[i]->geometry().intersects(shown[j]->geometry()),
                      "two panels do not overlap", __LINE__);
            }
        }
        // And the list is what gave way: it scrolls rather than growing past
        // the panel above it.
        auto* tree = window.findChild<LayerTree*>();
        CHECK(tree != nullptr);
        if (tree)
            CHECK(tree->verticalScrollBar()->maximum() > 0);
    });

    addTest("a panel is no taller than what it holds", [] {
        MainWindow window;
        window.newDocument(400, 300, Rgba(255, 255, 255, 255));
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
        // The panels remember where they were left, and the settings carry
        // over between runs, so this starts from the arrangement that ships.
        window.resetPanelLayout();
        QApplication::processEvents();

        for (const char* name : {"toolsPanel", "layersPanel", "historyPanel"}) {
            auto* panel = window.findChild<Panel*>(QString::fromLatin1(name));
            if (!panel || !panel->content()) {
                check(false, "the panel is there to measure", __LINE__);
                continue;
            }
            check(panel->height() <= panel->wantedSize().height() + 8,
                  "the panel stops at the end of its contents", __LINE__);
            check(panel->height() < window.height() - 200,
                  "the panel is not the height of the window", __LINE__);
        }

        // And it follows what it holds: a longer list is a taller panel.
        auto* layers = window.findChild<LayersPanel*>();
        auto* panel = window.findChild<Panel*>(QStringLiteral("layersPanel"));
        CHECK(layers != nullptr && panel != nullptr);
        if (!layers || !panel)
            return;
        const int before = panel->height();
        for (int i = 0; i < 5; ++i)
            QMetaObject::invokeMethod(layers, "addLayerRequested");
        QApplication::processEvents();
        CHECK(panel->height() > before);
        CHECK(panel->height() <= panel->wantedSize().height() + 8);
    });

    addTest("a grab left behind does not deafen the panel buttons", [] {
        // Wayland refuses the mouse grabs Qt asks for, and a drag that ends
        // without Qt hearing of it leaves the grab in place: every click then
        // goes to whatever grabbed instead of to the button under the
        // pointer, and the title bar buttons look dead until something else
        // clears it.
        MainWindow window;
        window.newDocument(400, 300, Rgba(255, 255, 255, 255));
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
        // The panels remember where they were left, and the settings carry
        // over between runs, so this starts from the arrangement that ships.
        window.resetPanelLayout();
        QApplication::processEvents();

        auto* panel = window.findChild<Panel*>(QStringLiteral("toolsPanel"));
        CHECK(panel != nullptr);
        if (!panel)
            return;
        panel->grabMouse();
        CHECK(QWidget::mouseGrabber() == panel);
        const QPointF at(4, 4);
        QMouseEvent move(QEvent::MouseMove, at, at, panel->mapToGlobal(at),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(panel, &move);
        CHECK(QWidget::mouseGrabber() == nullptr);
        if (QWidget::mouseGrabber())
            QWidget::mouseGrabber()->releaseMouse();
    });

    addTest("no two commands claim the same key", [] {
        // Qt refuses to choose between two actions bound to one sequence, so a
        // clash does not pick a winner: it makes the key do nothing at all,
        // silently, and only the menu entries keep working. Ctrl+Shift+G was
        // bound to both Add Group and the Black and White adjustment.
        MainWindow window;
        window.newDocument(64, 48, Rgba(255, 255, 255, 255));
        QMap<QString, QStringList> owners;
        for (QAction* action : window.findChildren<QAction*>()) {
            for (const QKeySequence& key : action->shortcuts()) {
                if (key.isEmpty())
                    continue;
                owners[key.toString()].append(
                    action->objectName().isEmpty() ? action->text()
                                                   : action->objectName());
            }
        }
        for (QShortcut* shortcut : window.findChildren<QShortcut*>()) {
            const QKeySequence key = shortcut->key();
            if (!key.isEmpty())
                owners[key.toString()].append(QStringLiteral("shortcut"));
        }
        QStringList clashes;
        for (auto it = owners.cbegin(); it != owners.cend(); ++it) {
            if (it.value().size() > 1)
                clashes.append(it.key() + QStringLiteral(": ") + it.value().join(QStringLiteral(", ")));
        }
        if (!clashes.isEmpty())
            std::printf("    clashes: %s\n", clashes.join(QStringLiteral(" | ")).toUtf8().constData());
        CHECK(clashes.isEmpty());
    });

    addTest("the rotate zoom shortcut reaches the transform tool", [] {
        // Menu shortcuts only fire for the active window, and only through the
        // shortcut map, so this one has to be pressed the way a person presses
        // it rather than by triggering the action.
        MainWindow window;
        window.newDocument(160, 120, Rgba(255, 255, 255, 255));
        window.show();
        window.activateWindow();
        QApplication::processEvents();
        CanvasView* view = window.findChildren<CanvasView*>().last();
        CHECK(view->tool() != nullptr);
        CHECK(view->tool()->id() != QLatin1String("transform"));
        QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::processEvents();
        std::printf("    tool after Ctrl+Shift+Z: %s\n",
                    view->tool()->id().toUtf8().constData());
        CHECK(view->tool()->id() == QLatin1String("transform"));
        // Activating it has to be visible, or it looks like nothing happened:
        // the gizmo must be on the canvas and the options bar must name it.
        QApplication::processEvents();
        const QImage shot = view->viewport()->grab().toImage();
        int painted = 0;
        for (int y = 0; y < shot.height(); ++y)
            for (int x = 0; x < shot.width(); ++x)
                if (qGray(shot.pixel(x, y)) < 200)
                    ++painted;
        std::printf("    gizmo pixels: %d of %d\n", painted,
                    shot.width() * shot.height());
        CHECK(painted > 100);
        const QList<QLabel*> labels = window.findChildren<QLabel*>();
        bool named = false;
        for (QLabel* label : labels)
            if (label->text() == QLatin1String("Rotate / Zoom"))
                named = true;
        CHECK(named);
    });

    addTest("shortcuts still work with a Cyrillic keyboard layout", [] {
        // With the Russian layout the Z key reports Ya, which no shortcut is
        // bound to, so Ctrl+Shift+Z did nothing at all. The filter translates
        // the letter back to the key it is printed on.
        CHECK(KeyLayoutFilter::latinKeyFor(0x042F) == Qt::Key_Z);
        CHECK(KeyLayoutFilter::latinKeyFor(0x044F) == Qt::Key_Z);
        CHECK(KeyLayoutFilter::latinKeyFor(Qt::Key_Z) == 0);
        auto* filter = new KeyLayoutFilter(qApp);
        qApp->installEventFilter(filter);
        MainWindow window;
        window.newDocument(160, 120, Rgba(255, 255, 255, 255));
        window.show();
        window.activateWindow();
        QApplication::processEvents();
        CanvasView* view = window.findChildren<CanvasView*>().last();
        CHECK(view->tool()->id() != QLatin1String("transform"));
        // Sent rather than typed: QTest cannot carry a key that has no ASCII
        // equivalent, and the filter sees the event either way, since it is
        // installed on the application.
        QKeyEvent typed(QEvent::KeyPress, 0x042F,
                        Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("Я"));
        QApplication::sendEvent(&window, &typed);
        QApplication::processEvents();
        CHECK(view->tool()->id() == QLatin1String("transform"));
        qApp->removeEventFilter(filter);
        delete filter;
    });

    addTest("escape leaves moved pixels where they were dragged", [] {
        // Escape used to hand the pixels back to where they started, which
        // threw away a move that looked finished. It now lets go of them and
        // drops the outline as well, so one press finishes the job.
        auto doc = std::make_unique<Document>(200, 200, Rgba(255, 255, 255, 255));
        ops::fillRegion(doc->activeLayer()->image, Rect(40, 40, 60, 60),
                        Rgba(255, 0, 0, 255));
        Mask mask(200, 200);
        for (int y = 40; y < 100; ++y)
            for (int x = 40; x < 100; ++x)
                mask.setAt(x, y, 1.0f);
        doc->selection().setMask(mask);
        doc->invalidate();
        CanvasView view(std::move(doc));
        view.resize(400, 400);
        view.show();
        view.setZoom(1.0);
        QApplication::processEvents();
        Tool* move = ToolRegistry::instance().byId(QStringLiteral("move-selected"));
        CHECK(move != nullptr);
        view.setTool(move);
        ToolContext context = view.makeContext();
        drag(move, context, QPointF(70, 70), QPointF(120, 120));

        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&view, &escape);
        const Image& after = view.document()->activeLayer()->image;
        CHECK(after.at(120, 120).r == 255);   // the pixels stayed where they went
        CHECK(after.at(120, 120).g == 0);
        CHECK(after.at(70, 70).a == 0);       // and left a hole behind them
        CHECK(!move->isBusy());               // the tool has let go
        CHECK(!view.document()->selection().isActive());  // and so has the outline

        // One undo puts the pixels and the outline back together.
        CHECK(view.history()->canUndo());
        view.history()->undo();
        view.refresh();
        CHECK(view.document()->activeLayer()->image.at(70, 70).g == 0);
        CHECK(view.document()->selection().isActive());
    });

    addTest("escape clears the selection", [] {
        auto doc = std::make_unique<Document>(120, 90);
        CanvasView view(std::move(doc));
        view.resize(400, 300);
        view.show();
        QApplication::processEvents();
        view.document()->selection().selectAll();
        CHECK(view.document()->selection().isActive());

        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&view, &escape);
        CHECK(!view.document()->selection().isActive());
        // And it is undoable, rather than quietly throwing the selection away.
        CHECK(view.history()->canUndo());
        view.history()->undo();
        CHECK(view.document()->selection().isActive());
    });

    addTest("escape still cancels work in progress first", [] {
        auto doc = std::make_unique<Document>(120, 90, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        view.resize(400, 300);
        view.show();
        QApplication::processEvents();
        view.document()->selection().selectAll();

        Tool* shapes = ToolRegistry::instance().byId(QStringLiteral("shapes"));
        view.setTool(shapes);
        ToolContext context = view.makeContext();
        shapes->press(context, makeEvent(QPointF(20, 20)));
        ToolEvent move = makeEvent(QPointF(80, 70));
        move.button = Qt::NoButton;
        shapes->move(context, move);
        CHECK(shapes->isBusy());

        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&view, &escape);
        // The shape is abandoned, but the selection survives this first press.
        CHECK(!shapes->isBusy());
        CHECK(view.document()->selection().isActive());
        QApplication::sendEvent(&view, &escape);
        CHECK(!view.document()->selection().isActive());
    });

    addTest("rotate and zoom gizmo transforms the layer", [] {
        auto doc = std::make_unique<Document>(120, 90, Rgba(0, 0, 0, 0));
        // A mark off centre, so a rotation is detectable.
        ops::fillRegion(doc->activeLayer()->image, Rect(10, 10, 20, 15),
                        Rgba(255, 0, 0, 255));
        doc->invalidate();
        CanvasView view(std::move(doc));
        Tool* transform = ToolRegistry::instance().byId(QStringLiteral("transform"));
        CHECK(transform != nullptr);
        if (!transform)
            return;
        const Image before = view.document()->activeLayer()->image;

        view.setTool(transform);               // activating arms the gizmo
        transform->optionChanged(QStringLiteral("angle"), 90.0);
        ToolContext context = view.makeContext();
        transform->press(context, makeEvent(QPointF(60, 45)));
        ToolEvent move = makeEvent(QPointF(60, 45));
        move.button = Qt::NoButton;
        transform->move(context, move);
        transform->commit(context);

        const Image& after = view.document()->activeLayer()->image;
        CHECK(nonTransparentPixels(after) > 0);
        // A quarter turn about the centre moves the mark to a different corner.
        CHECK(after.at(20, 17).a != before.at(20, 17).a);
        CHECK(view.history()->count() == 1);
        view.history()->undo();
        CHECK(view.document()->activeLayer()->image.at(20, 17).a == before.at(20, 17).a);
    });

    addTest("rotate and zoom gizmo can be abandoned", [] {
        auto doc = std::make_unique<Document>(80, 60, Rgba(0, 0, 0, 0));
        ops::fillRegion(doc->activeLayer()->image, Rect(5, 5, 10, 10),
                        Rgba(0, 255, 0, 255));
        doc->invalidate();
        CanvasView view(std::move(doc));
        Tool* transform = ToolRegistry::instance().byId(QStringLiteral("transform"));
        const Image before = view.document()->activeLayer()->image;
        view.setTool(transform);
        transform->optionChanged(QStringLiteral("angle"), 33.0);
        ToolContext context = view.makeContext();
        transform->cancel(context);
        CHECK(view.document()->activeLayer()->image.at(8, 8) == before.at(8, 8));
        CHECK(view.history()->count() == 0);
    });

    addTest("placing an oversized picture keeps what hangs off the canvas", [] {
        // Importing used to trim the picture to the canvas the moment it was
        // loaded, so dragging it afterwards only moved the surviving crop and
        // the rest could never be brought back into view.
        auto doc = std::make_unique<Document>(100, 80, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* place = ToolRegistry::instance().byId(QStringLiteral("place"));
        CHECK(place != nullptr);
        if (!place)
            return;
        view.setTool(place);

        // Twice the canvas, with a mark only in its bottom right quarter.
        Image picture(200, 160, Rgba(0, 0, 0, 0));
        ops::fillRegion(picture, Rect(150, 120, 40, 30), Rgba(255, 0, 0, 255));

        ToolContext context = view.makeContext();
        CHECK(startPlacement(context, picture, QStringLiteral("test"), true));
        CHECK(place->isBusy());
        CHECK(view.document()->layerCount() == 2);

        // Centred, that mark sits past the bottom right corner, so nothing of
        // it shows yet.
        CHECK(view.document()->composite().at(95, 75).a == 0);

        // Drag the picture up and left to bring the mark into view.
        ToolEvent down = makeEvent(QPointF(50, 40));
        place->press(context, down);
        ToolEvent drag = makeEvent(QPointF(-10, -10));
        drag.button = Qt::NoButton;
        drag.buttons = Qt::LeftButton;
        place->move(context, drag);
        place->release(context, makeEvent(QPointF(-10, -10)));
        place->commit(context);

        // The part that was off canvas has landed, which is the whole point.
        const Image& result = view.document()->activeLayer()->image;
        CHECK(result.at(50, 40) == Rgba(255, 0, 0, 255));
        // And nothing spilled outside the canvas.
        CHECK(result.width() == 100 && result.height() == 80);
        CHECK(view.history()->count() == 1);
        view.history()->undo();
        CHECK(view.document()->layerCount() == 1);
    });

    addTest("placement can be abandoned", [] {
        auto doc = std::make_unique<Document>(60, 60, Rgba(0, 0, 0, 0));
        CanvasView view(std::move(doc));
        Tool* place = ToolRegistry::instance().byId(QStringLiteral("place"));
        view.setTool(place);
        Image picture(40, 40, Rgba(0, 255, 0, 255));
        ToolContext context = view.makeContext();
        CHECK(startPlacement(context, picture, QStringLiteral("test"), true));
        place->cancel(context);
        CHECK(view.document()->layerCount() == 1);
        CHECK(nonTransparentPixels(view.document()->activeLayer()->image) == 0);
        CHECK(view.history()->count() == 0);
    });

    // ------------------------------------------------------------ main window
    addTest("main window starts and holds a document", [] {
        MainWindow window;
        window.newDocument(320, 240, Rgba(255, 255, 255, 255));
        const QList<CanvasView*> views = window.findChildren<CanvasView*>();
        CHECK(!views.isEmpty());
        CanvasView* view = views.last();
        CHECK(view->document()->width() == 320);
        CHECK(view->document()->height() == 240);
        CHECK(window.windowTitle().contains(QStringLiteral("Tessel")));
        // The four panels exist.
        CHECK(window.findChild<ToolsPalette*>() != nullptr);
        CHECK(window.findChild<ColorPanel*>() != nullptr);
        CHECK(window.findChild<LayersPanel*>() != nullptr);
        CHECK(window.findChild<HistoryPanel*>() != nullptr);
        CHECK(window.findChild<ImageStrip*>() != nullptr);
        // And so do the menu actions.
        for (const char* name : {"new", "open", "save", "undo", "redo", "cut", "copy",
                                 "paste", "selectall", "deselect", "crop", "resize",
                                 "canvassize", "flatten", "addlayer", "layerprops"}) {
            check(window.findChild<QAction*>(QString::fromLatin1(name)) != nullptr, name,
                  __LINE__);
        }
    });

    addTest("window commands edit the document", [] {
        MainWindow window;
        window.newDocument(100, 80, Rgba(255, 255, 255, 255));
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Document* doc = view->document();
        auto trigger = [&window](const char* name) {
            QAction* action = window.findChild<QAction*>(QString::fromLatin1(name));
            if (action && action->isEnabled())
                action->trigger();
            return action != nullptr;
        };

        CHECK(trigger("selectall"));
        CHECK(doc->selection().isActive());
        CHECK(trigger("addlayer"));
        CHECK(doc->layerCount() == 2);
        CHECK(trigger("duplicatelayer"));
        CHECK(doc->layerCount() == 3);
        CHECK(trigger("mergedown"));
        CHECK(doc->layerCount() == 2);
        CHECK(trigger("fill"));
        CHECK(trigger("flatten"));
        CHECK(doc->layerCount() == 1);
        CHECK(trigger("rotatecw"));
        CHECK(doc->width() == 80 && doc->height() == 100);
        CHECK(trigger("fliph"));
        CHECK(trigger("undo"));
        CHECK(trigger("undo"));
        CHECK(doc->width() == 100 && doc->height() == 80);
        CHECK(trigger("redo"));
        CHECK(doc->width() == 80);
        CHECK(trigger("deselect"));
        CHECK(!doc->selection().isActive());
    });

    addTest("the layers panel shows groups as a tree", [] {
        MainWindow window;
        window.resize(1200, 800);
        window.newDocument(60, 40, Rgba(255, 255, 255, 255));
        window.show();
        QApplication::processEvents();
        Document* doc = window.findChildren<CanvasView*>().last()->document();
        LayerTree* tree = window.findChild<LayerTree*>();
        QAction* addGroup = window.findChild<QAction*>(QStringLiteral("addgroup"));
        CHECK(tree != nullptr);
        CHECK(addGroup != nullptr);
        if (!tree || !addGroup)
            return;

        addGroup->trigger();
        CHECK(doc->layerCount() == 2);
        Layer* group = doc->activeNode();
        CHECK(group != nullptr && group->isGroup());
        CHECK(tree->topLevelItemCount() == 2);

        // A layer added while the group is active lands inside it, and the
        // panel draws it as a child row rather than beside the group.
        window.findChild<QAction*>(QStringLiteral("addlayer"))->trigger();
        CHECK(doc->activeNode()->parent() == group);
        CHECK(tree->topLevelItemCount() == 2);
        QTreeWidgetItem* groupItem = tree->topLevelItem(0);
        CHECK(groupItem != nullptr);
        if (!groupItem)
            return;
        CHECK(groupItem->text(0) == group->name);
        CHECK(groupItem->childCount() == 1);

        // Collapsing is remembered on the group itself.
        groupItem->setExpanded(false);
        CHECK(!group->expanded);
        groupItem->setExpanded(true);
        CHECK(group->expanded);

        // Both steps undo, and the panel follows the document back.
        QAction* undo = window.findChild<QAction*>(QStringLiteral("undo"));
        undo->trigger();
        undo->trigger();
        CHECK(doc->layerCount() == 1);
        CHECK(tree->topLevelItemCount() == 1);
    });

    addTest("the layers panel picks the paint target", [] {
        MainWindow window;
        window.resize(1200, 800);
        window.newDocument(40, 30, Rgba(255, 255, 255, 255));
        window.show();
        QApplication::processEvents();
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Document* doc = view->document();
        LayerTree* tree = window.findChild<LayerTree*>();
        QAction* addMask = window.findChild<QAction*>(QStringLiteral("addmask"));
        CHECK(tree != nullptr);
        CHECK(addMask != nullptr);
        if (!tree || !addMask)
            return;

        addMask->trigger();
        Layer* layer = doc->activeNode();
        CHECK(layer != nullptr && layer->hasMask());
        if (!layer || !layer->hasMask())
            return;
        // A fresh mask becomes the paint target straight away.
        CHECK(layer->maskSelected());
        CHECK(doc->activeLayer() == layer->maskLayer());
        QApplication::processEvents();

        // The panel shows both thumbnails, and clicking one picks the target.
        QTreeWidgetItem* item = tree->topLevelItem(0);
        CHECK(item != nullptr);
        if (!item)
            return;
        CHECK(!item->data(0, LayerRowDelegate::MaskThumbnail).value<QImage>().isNull());
        const QRect row = tree->visualItemRect(item);
        CHECK(!row.isEmpty());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          LayerRowDelegate::thumbnailRect(row).center());
        CHECK(!layer->maskSelected());
        CHECK(doc->activeLayer() == layer);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          LayerRowDelegate::maskRect(row).center());
        CHECK(layer->maskSelected());

        // So a brush stroke now lands in the mask and leaves the pixels alone.
        const Image before = layer->image;
        Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"));
        CHECK(brush != nullptr);
        if (!brush)
            return;
        view->setTool(brush);
        ToolContext context = view->makeContext();
        brush->press(context, makeEvent(QPointF(20, 15)));
        brush->release(context, makeEvent(QPointF(20, 15)));
        CHECK(samePixels(layer->image, before));
        CHECK(layer->maskImage()->at(20, 15) != Rgba(255, 255, 255, 255));
        CHECK(view->history()->count() == 2);

        // The stroke in the mask undoes on its own, without the layer pixels
        // taking part.
        view->history()->undo();
        Layer* again = doc->activeNode();
        CHECK(again->maskImage()->at(20, 15) == Rgba(255, 255, 255, 255));
        CHECK(samePixels(again->image, before));

        // Applying the mask bakes it and leaves the layer as the only target.
        QAction* apply = window.findChild<QAction*>(QStringLiteral("applymask"));
        CHECK(apply != nullptr && apply->isEnabled());
        apply->trigger();
        CHECK(!doc->activeNode()->hasMask());
        CHECK(doc->activeLayer() == doc->activeNode());
    });

    addTest("dropping a layer on a group puts it inside", [] {
        MainWindow window;
        window.resize(1200, 800);
        window.newDocument(32, 24, Rgba(255, 255, 255, 255));
        window.show();
        QApplication::processEvents();
        Document* doc = window.findChildren<CanvasView*>().last()->document();
        LayerTree* tree = window.findChild<LayerTree*>();
        CHECK(tree != nullptr);
        if (!tree)
            return;
        window.findChild<QAction*>(QStringLiteral("addgroup"))->trigger();
        const int groupUid = doc->activeNode()->uid();
        CHECK(doc->activeNode()->isGroup());
        CHECK(tree->topLevelItemCount() == 2);

        QTreeWidgetItem* groupItem = tree->topLevelItem(0);
        QTreeWidgetItem* backgroundItem = tree->topLevelItem(1);
        CHECK(groupItem != nullptr && backgroundItem != nullptr);
        if (!groupItem || !backgroundItem)
            return;
        const int backgroundUid =
            backgroundItem->data(0, LayerRowDelegate::LayerUid).toInt();

        // Dragging itself is Qt's business, so the drop is handed over the way
        // the tree hands it over.
        tree->layerDropped(backgroundItem, groupItem, LayerTree::Drop::Into);
        QApplication::processEvents();
        CHECK(doc->layerCount() == 2);
        CHECK(doc->layerByUid(backgroundUid) != nullptr);
        CHECK(doc->layerByUid(backgroundUid)->parent() == doc->layerByUid(groupUid));
        CHECK(tree->topLevelItemCount() == 1);

        // The move is a structural edit like any other, so it undoes.
        window.findChild<QAction*>(QStringLiteral("undo"))->trigger();
        CHECK(doc->layerByUid(backgroundUid)->parent() == doc->rootGroup());
        window.findChild<QAction*>(QStringLiteral("redo"))->trigger();
        CHECK(doc->layerByUid(backgroundUid)->parent() == doc->layerByUid(groupUid));

        // And it can be dragged back out, above the group's own row.
        QTreeWidgetItem* groupRow = tree->topLevelItem(0);
        CHECK(groupRow != nullptr && groupRow->childCount() == 1);
        if (!groupRow || groupRow->childCount() != 1)
            return;
        tree->layerDropped(groupRow->child(0), groupRow, LayerTree::Drop::Above);
        QApplication::processEvents();
        CHECK(doc->layerByUid(backgroundUid)->parent() == doc->rootGroup());
        CHECK(doc->layerCount() == 2);
        CHECK(tree->topLevelItemCount() == 2);
    });

    addTest("clipboard round trip", [] {
        MainWindow window;
        window.newDocument(60, 60, Rgba(10, 120, 240, 255));
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Document* doc = view->document();
        Mask mask(60, 60);
        for (int y = 5; y < 25; ++y)
            for (int x = 5; x < 25; ++x)
                mask.setAt(x, y, 1.0f);
        doc->selection().setMask(mask);

        window.findChild<QAction*>(QStringLiteral("copy"))->trigger();
        const QImage clip = QGuiApplication::clipboard()->image();
        CHECK(!clip.isNull());
        CHECK(clip.width() == 20 && clip.height() == 20);

        QAction* pasteLayer = window.findChild<QAction*>(QStringLiteral("pastelayer"));
        pasteLayer->setEnabled(true);
        pasteLayer->trigger();
        CHECK(doc->layerCount() == 2);
        CHECK(doc->activeLayer()->image.at(10, 10).a > 0);
    });

    addTest("save and open through the window", [] {
        QTemporaryDir dir;
        MainWindow window;
        window.newDocument(48, 36, Rgba(20, 90, 140, 255));
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Document* doc = view->document();
        doc->addEmptyLayer();
        ops::fillRegion(doc->activeLayer()->image, Rect(4, 4, 12, 12),
                        Rgba(250, 250, 10, 255));
        doc->invalidate();

        const QString path = dir.filePath(QStringLiteral("window.tessel"));
        QString error;
        CHECK(io::saveDocument(*doc, path, &error));
        view->history()->markSaved();
        CHECK(!doc->isModified());

        CHECK(window.openPath(path));
        CanvasView* opened = window.findChildren<CanvasView*>().last();
        CHECK(opened->document()->layerCount() == 2);
        CHECK(opened->document()->width() == 48);
        CHECK(opened->document()->layerAt(1)->image.at(8, 8) == Rgba(250, 250, 10, 255));
        CHECK(!opened->document()->isModified());
    });

    addTest("selection submenu edits the selection", [] {
        MainWindow window;
        window.newDocument(80, 60, Rgba(255, 255, 255, 255));
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Document* doc = view->document();
        History* history = view->history();

        for (const char* name : {"feather", "grow", "shrink", "border",
                                 "freetransform"}) {
            check(window.findChild<QAction*>(QString::fromLatin1(name)) != nullptr, name,
                  __LINE__);
        }
        auto action = [&window](const char* name) {
            return window.findChild<QAction*>(QString::fromLatin1(name));
        };
        // They all live together in one submenu under Edit.
        QMenu* submenu = nullptr;
        for (QMenu* candidate : window.findChildren<QMenu*>()) {
            if (candidate->title() == QStringLiteral("Se&lection"))
                submenu = candidate;
        }
        CHECK(submenu != nullptr);
        CHECK(submenu && submenu->actions().contains(action("feather")));
        CHECK(submenu && submenu->actions().contains(action("freetransform")));
        // The edge operations need something to work on.
        CHECK(!action("grow")->isEnabled());
        action("selectall")->trigger();
        CHECK(action("grow")->isEnabled());
        CHECK(action("feather")->isEnabled());
        CHECK(action("border")->isEnabled());

        // Work on a rectangle clear of the canvas edges.
        Mask rect(80, 60);
        for (int y = 15; y < 45; ++y)
            for (int x = 20; x < 60; ++x)
                rect.setAt(x, y, 1.0f);
        doc->selection().setMask(rect);
        const size_t area = doc->selection().pixelCount();
        const int steps = history->count();

        answerSelectionDialog(6);
        action("grow")->trigger();
        CHECK(doc->selection().pixelCount() > area);
        CHECK(history->count() == steps + 1);
        CHECK(history->undoName() == QStringLiteral("Grow Selection"));
        history->undo();
        CHECK(doc->selection().pixelCount() == area);

        answerSelectionDialog(6);
        action("shrink")->trigger();
        CHECK(doc->selection().pixelCount() < area);
        history->undo();
        CHECK(doc->selection().pixelCount() == area);

        answerSelectionDialog(4);
        action("border")->trigger();
        CHECK(doc->selection().pixelCount() < area);
        CHECK(doc->selection().bounds().w == 40);
        CHECK(!doc->selection().contains(40, 30)); // hollow in the middle
        CHECK(doc->selection().contains(20, 30));  // but not along the rim
        history->undo();
        CHECK(doc->selection().contains(40, 30));

        answerSelectionDialog(5);
        action("feather")->trigger();
        const Mask* mask = doc->selection().clipMask();
        CHECK(mask != nullptr);
        CHECK(mask->at(40, 30) == 1.0f);
        CHECK(mask->at(20, 30) > 0.0f && mask->at(20, 30) < 1.0f);
        CHECK(mask->at(18, 30) > 0.0f && mask->at(18, 30) < 0.5f);
        history->undo();
        CHECK(doc->selection().clipMask()->at(20, 30) == 1.0f);
        // Each operation is a single step, pushed over the one just undone.
        CHECK(history->count() == steps + 1);
        CHECK(history->canRedo());
        CHECK(history->redoName() == QStringLiteral("Feather Selection"));

        // Free Transform just picks up the tool that floats the pixels.
        action("freetransform")->trigger();
        CHECK(view->tool() != nullptr);
        CHECK(view->tool()->id() == QStringLiteral("move-selected"));
    });

    addTest("free transform rotates the floated pixels", [] {
        auto doc = std::make_unique<Document>(80, 80, Rgba(0, 0, 0, 0));
        // A tall bar, so a quarter turn is obvious in the bounds.
        ops::fillRegion(doc->activeLayer()->image, Rect(30, 20, 8, 40),
                        Rgba(255, 40, 40, 255));
        Mask mask(80, 80);
        for (int y = 20; y < 60; ++y)
            for (int x = 30; x < 38; ++x)
                mask.setAt(x, y, 1.0f);
        doc->selection().setMask(mask);
        doc->invalidate();
        CanvasView view(std::move(doc));
        const Image original = view.document()->activeLayer()->image;

        Tool* move = ToolRegistry::instance().byId(QStringLiteral("move-selected"));
        view.setTool(move);
        ToolContext context = view.makeContext();
        // Grabbing outside the box rotates about its centre, and Shift snaps the
        // angle, so this drag is exactly a quarter turn.
        drag(move, context, QPointF(70, 40), QPointF(34, 76), Qt::LeftButton,
             Qt::ShiftModifier);
        move->commit(context);

        const Image& image = view.document()->activeLayer()->image;
        const Rect bounds = image.opaqueBounds();
        CHECK(bounds.w > bounds.h);
        CHECK(bounds.w >= 36 && bounds.h <= 12);
        CHECK(image.at(34, 40).a > 0);   // still centred where it was
        CHECK(image.at(34, 22).a == 0);  // the top of the old bar is bare
        CHECK(image.at(34, 58).a == 0);
        // The outline travels with the pixels.
        const Rect selected = view.document()->selection().bounds();
        CHECK(selected.w > selected.h);
        CHECK(view.history()->count() == 1);

        view.history()->undo();
        CHECK(sameImage(view.document()->activeLayer()->image, original));
        CHECK(view.document()->selection().bounds().h > 30);
    });

    addTest("colour panel drives the views", [] {
        MainWindow window;
        window.newDocument(32, 32, Rgba(255, 255, 255, 255));
        ColorPanel* colors = window.findChild<ColorPanel*>();
        CanvasView* view = window.findChildren<CanvasView*>().last();
        colors->setPrimary(Rgba(9, 8, 7, 255));
        CHECK(view->primaryColor() == Rgba(9, 8, 7, 255));
        colors->setSecondary(Rgba(1, 2, 3, 255));
        CHECK(view->secondaryColor() == Rgba(1, 2, 3, 255));
        colors->swapColors();
        CHECK(view->primaryColor() == Rgba(1, 2, 3, 255));
        CHECK(view->secondaryColor() == Rgba(9, 8, 7, 255));
        colors->resetColors();
        CHECK(view->primaryColor() == Rgba(0, 0, 0, 255));
        CHECK(view->secondaryColor() == Rgba(255, 255, 255, 255));
    });

    addTest("multiple images and closing", [] {
        MainWindow window;
        window.newDocument(40, 40, Rgba(255, 255, 255, 255));
        window.newDocument(60, 60, Rgba(255, 255, 255, 255));
        window.newDocument(80, 80, Rgba(255, 255, 255, 255));
        CHECK(window.findChildren<CanvasView*>().size() >= 3);
        QAction* next = window.findChild<QAction*>(QStringLiteral("nextimage"));
        CHECK(next != nullptr && next->isEnabled());
        next->trigger();
        // Closing an unmodified image needs no prompt.
        QAction* close = window.findChild<QAction*>(QStringLiteral("close"));
        close->trigger();
        CHECK(window.findChildren<CanvasView*>().size() >= 2);
    });

    // ------------------------------------------------------------ palettes
    addTest("recent colours follow the chosen colours", [] {
        ColorPanel panel;
        panel.clearRecentColors();
        CHECK(panel.recentColors().isEmpty());

        panel.setPrimary(Rgba(10, 20, 30, 255));
        CHECK(panel.recentColors().size() == 1);
        CHECK(panel.recentColors().front() == Rgba(10, 20, 30, 255));

        // The secondary colour counts as used too, and the newest comes first.
        panel.setSecondary(Rgba(40, 50, 60, 255));
        CHECK(panel.recentColors().size() == 2);
        CHECK(panel.recentColors().front() == Rgba(40, 50, 60, 255));
        CHECK(panel.recentColors().back() == Rgba(10, 20, 30, 255));

        // Using a colour again moves it to the front instead of repeating it.
        panel.setPrimary(Rgba(10, 20, 30, 255));
        CHECK(panel.recentColors().size() == 2);
        CHECK(panel.recentColors().front() == Rgba(10, 20, 30, 255));

        // Alpha is part of the identity, so this is a different entry.
        panel.setPrimary(Rgba(10, 20, 30, 128));
        CHECK(panel.recentColors().size() == 3);

        // The row is a widget of its own, next to the palette grid.
        CHECK(panel.findChildren<PaletteGrid*>().size() == 2);
    });

    addTest("recent colours cap and stay unique", [] {
        ColorPanel panel;
        panel.clearRecentColors();
        const int limit = ColorPanel::kRecentLimit;
        CHECK(limit >= 16);
        for (int i = 0; i < limit * 2; ++i)
            panel.setPrimary(Rgba(uint8_t(i), 7, 9, 255));
        CHECK(panel.recentColors().size() == limit);
        CHECK(panel.recentColors().front() == Rgba(uint8_t(limit * 2 - 1), 7, 9, 255));
        CHECK(panel.recentColors().back() == Rgba(uint8_t(limit), 7, 9, 255));

        // Repeating one colour many times never lengthens the list.
        for (int i = 0; i < 50; ++i)
            panel.setPrimary(Rgba(3, 3, 3, 255));
        CHECK(panel.recentColors().size() == limit);
        const QVector<Rgba> recent = panel.recentColors();
        for (int i = 0; i < recent.size(); ++i) {
            for (int j = i + 1; j < recent.size(); ++j)
                CHECK(recent[i] != recent[j]);
        }

        // Swapping and resetting go through the same route.
        panel.clearRecentColors();
        panel.setPrimary(Rgba(1, 1, 1, 255));
        panel.setSecondary(Rgba(2, 2, 2, 255));
        panel.swapColors();
        CHECK(panel.recentColors().size() == 2);
        CHECK(panel.recentColors().front() == panel.primary());
        panel.resetColors();
        CHECK(panel.recentColors().front() == Rgba(0, 0, 0, 255));
        CHECK(panel.recentColors().contains(Rgba(255, 255, 255, 255)));
    });

    addTest("recent colours cover the picker tool", [] {
        MainWindow window;
        window.newDocument(16, 16, Rgba(7, 77, 177, 255));
        ColorPanel* colors = window.findChild<ColorPanel*>();
        CHECK(colors != nullptr);
        if (!colors)
            return;
        colors->clearRecentColors();
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Tool* picker = ToolRegistry::instance().byId(QStringLiteral("picker"));
        CHECK(picker != nullptr);
        view->setTool(picker);
        ToolContext context = view->makeContext();
        picker->press(context, makeEvent(QPointF(8, 8)));
        CHECK(colors->recentColors().front() == Rgba(7, 77, 177, 255));
    });

    addTest("palette buttons load, save and reset", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        ColorPanel panel;
        auto* load = panel.findChild<QAbstractButton*>(QStringLiteral("paletteLoad"));
        auto* save = panel.findChild<QAbstractButton*>(QStringLiteral("paletteSave"));
        auto* reset = panel.findChild<QAbstractButton*>(QStringLiteral("paletteReset"));
        auto* pick = panel.findChild<QAbstractButton*>(QStringLiteral("screenPick"));
        CHECK(load != nullptr);
        CHECK(save != nullptr);
        CHECK(reset != nullptr);
        CHECK(pick != nullptr);
        if (!load || !save || !reset)
            return;
        CHECK(load->isEnabled());
        CHECK(save->isEnabled());
        CHECK(reset->isEnabled());
        CHECK(panel.paletteColors() == PaletteGrid::defaultPalette());

        // Saving writes what the grid holds, in either format.
        const QString gplPath = dir.filePath(QStringLiteral("panel.gpl"));
        QString error = QStringLiteral("unset");
        CHECK(panel.savePaletteFile(gplPath, &error));
        CHECK(error.isEmpty());
        CHECK(Palette::load(gplPath, &error).colors() == PaletteGrid::defaultPalette());

        // Loading replaces the grid.
        const QString threePath = dir.filePath(QStringLiteral("three.txt"));
        QFile three(threePath);
        CHECK(three.open(QIODevice::WriteOnly));
        three.write("# three colours\n#FF0000\n#00FF00\n#0000FF\n");
        three.close();
        CHECK(panel.loadPaletteFile(threePath, &error));
        CHECK(error.isEmpty());
        CHECK(panel.paletteColors().size() == 3);
        CHECK(panel.paletteColors()[0] == Rgba(255, 0, 0, 255));
        CHECK(panel.paletteColors()[2] == Rgba(0, 0, 255, 255));

        // Saving again writes the loaded palette, not the built in one.
        const QString again = dir.filePath(QStringLiteral("again.gpl"));
        CHECK(panel.savePaletteFile(again, &error));
        CHECK(Palette::load(again, &error).size() == 3);

        // A palette entry still sets the colour it was clicked for.
        PaletteGrid* grid = panel.findChildren<PaletteGrid*>().last();
        CHECK(grid != nullptr);
        emit grid->colorChosen(Rgba(255, 0, 0, 255), true);
        CHECK(panel.primary() == Rgba(255, 0, 0, 255));

        // A file with nothing usable in it is refused and the grid is kept.
        const QString junkPath = dir.filePath(QStringLiteral("junk.gpl"));
        QFile junk(junkPath);
        CHECK(junk.open(QIODevice::WriteOnly));
        junk.write("not a palette\nnor this line\n");
        junk.close();
        error.clear();
        CHECK(!panel.loadPaletteFile(junkPath, &error));
        CHECK(!error.isEmpty());
        CHECK(panel.paletteColors().size() == 3);

        // The reset button restores the built in palette.
        reset->click();
        CHECK(panel.paletteColors() == PaletteGrid::defaultPalette());
    });

    addTest("screen picking degrades gracefully", [] {
        // The offscreen platform hands back no screen content, which is the
        // same situation as a Wayland compositor refusing a capture.
        CHECK(!ColorPanel::isUsableGrab(QImage()));
        QImage black(40, 30, QImage::Format_RGB32);
        black.fill(Qt::black);
        CHECK(!ColorPanel::isUsableGrab(black));
        QImage content = black;
        content.setPixelColor(21, 11, QColor(3, 4, 5));
        CHECK(ColorPanel::isUsableGrab(content));

        ColorPanel panel;
        panel.clearRecentColors();
        const Rgba before = panel.primary();
        QString error = QStringLiteral("unset");
        if (panel.beginScreenPick(&error)) {
            // A platform that does allow the grab: nothing is picked until the
            // user clicks, and the overlay goes away again.
            CHECK(error.isEmpty());
            CHECK(panel.screenPickActive());
            panel.cancelScreenPick();
            CHECK(!panel.screenPickActive());
        } else {
            // The failure is reported rather than silently picking black.
            CHECK(!error.isEmpty());
            CHECK(!panel.screenPickActive());
            CHECK(panel.primary() == before);
            CHECK(panel.recentColors().isEmpty());
        }
        QApplication::processEvents();
    });

    addTest("view menu units drive the status bar", [] {
        MainWindow window;
        window.newDocument(192, 96, Rgba(255, 255, 255, 255));
        CanvasView* view = window.findChildren<CanvasView*>().last();
        view->document()->setResolution(96.0);

        QAction* pixels = window.findChild<QAction*>(QStringLiteral("unit-pixels"));
        QAction* inches = window.findChild<QAction*>(QStringLiteral("unit-inches"));
        QAction* centimetres =
            window.findChild<QAction*>(QStringLiteral("unit-centimetres"));
        CHECK(pixels != nullptr);
        CHECK(inches != nullptr);
        CHECK(centimetres != nullptr);
        CHECK(pixels->isCheckable() && inches->isCheckable());
        QLabel* size = window.findChild<QLabel*>(QStringLiteral("imagesize"));
        CHECK(size != nullptr);
        if (!pixels || !inches || !centimetres || !size)
            return;

        // Start from centimetres so every later switch is a real change,
        // whatever unit the stored settings left behind.
        centimetres->trigger();
        const QString inCentimetres = size->text();
        CHECK(inCentimetres == QStringLiteral("5.08 x 2.54 cm"));
        CHECK(view->rulerUnit() == Unit::Centimetres);

        inches->trigger();
        const QString inInches = size->text();
        CHECK(inInches != inCentimetres);
        CHECK(inInches == QStringLiteral("2 x 1 in"));
        CHECK(view->rulerUnit() == Unit::Inches);
        CHECK(inches->isChecked() && !centimetres->isChecked());

        pixels->trigger();
        CHECK(size->text() == QStringLiteral("192 x 96 px"));
        CHECK(size->text() != inInches);
        CHECK(view->rulerUnit() == Unit::Pixels);

        // A denser document prints smaller, and the status bar says so.
        inches->trigger();
        view->document()->setResolution(192.0);
        view->notifyDocumentChanged();
        CHECK(size->text() == QStringLiteral("1 x 0.5 in"));

        // Views opened later start in the chosen unit.
        window.newDocument(100, 100, Rgba(255, 255, 255, 255));
        CHECK(window.findChildren<CanvasView*>().last()->rulerUnit() == Unit::Inches);
    });

    addTest("resize dialog converts between units", [] {
        // 600 x 300 pixels at 300 ppi is a 2 x 1 inch print.
        ResizeDialog dialog(QStringLiteral("Resize Image"), 600, 300, 300.0,
                            Unit::Pixels, false);
        CHECK(dialog.unit() == Unit::Pixels);
        CHECK(dialog.newWidth() == 600 && dialog.newHeight() == 300);
        CHECK(nearly(dialog.displayedWidth(), 600.0));
        CHECK(dialog.findChild<QComboBox*>(QStringLiteral("units")) != nullptr);
        CHECK(dialog.findChild<QDoubleSpinBox*>(QStringLiteral("resolution")) != nullptr);

        // Switching units only relabels the same image.
        dialog.setUnit(Unit::Inches);
        CHECK(dialog.newWidth() == 600 && dialog.newHeight() == 300);
        CHECK(nearly(dialog.displayedWidth(), 2.0, 1e-3));
        CHECK(nearly(dialog.displayedHeight(), 1.0, 1e-3));
        CHECK(dialog.printSizeText().contains(QStringLiteral("2 x 1 in at 300 ppi")));

        dialog.setUnit(Unit::Centimetres);
        CHECK(dialog.newWidth() == 600 && dialog.newHeight() == 300);
        CHECK(nearly(dialog.displayedWidth(), 5.08, 0.01));
        CHECK(nearly(dialog.displayedHeight(), 2.54, 0.01));

        dialog.setUnit(Unit::Percent);
        CHECK(dialog.newWidth() == 600 && dialog.newHeight() == 300);
        CHECK(nearly(dialog.displayedWidth(), 100.0, 0.01));

        dialog.setUnit(Unit::Pixels);
        CHECK(dialog.newWidth() == 600 && dialog.newHeight() == 300);
        CHECK(nearly(dialog.displayedWidth(), 600.0));
        CHECK(nearly(dialog.newResolution(), 300.0));

        // Only typing a new size changes the pixels, and the aspect ratio is
        // kept by default.
        dialog.setUnit(Unit::Inches);
        dialog.setDisplayedWidth(4.0);
        CHECK(dialog.newWidth() == 1200);
        CHECK(dialog.newHeight() == 600);
        CHECK(dialog.printSizeText().contains(QStringLiteral("4 x 2 in at 300 ppi")));

        // Percent measures against the size the dialog opened with.
        dialog.setUnit(Unit::Percent);
        CHECK(nearly(dialog.displayedWidth(), 200.0, 0.01));
        dialog.setDisplayedWidth(50.0);
        CHECK(dialog.newWidth() == 300 && dialog.newHeight() == 150);
    });

    addTest("resize dialog resample switch", [] {
        // Resampling off: a new resolution changes the print size only.
        ResizeDialog plain(QStringLiteral("Resize Image"), 600, 300, 300.0,
                           Unit::Inches, false);
        CHECK(plain.resamples());
        plain.setResample(false);
        CHECK(!plain.resamples());
        CHECK(plain.newWidth() == 600 && plain.newHeight() == 300);
        plain.setResolution(150.0);
        CHECK(plain.newWidth() == 600 && plain.newHeight() == 300);
        CHECK(nearly(plain.newResolution(), 150.0));
        CHECK(nearly(plain.displayedWidth(), 4.0, 1e-3));
        CHECK(plain.printSizeText().contains(QStringLiteral("4 x 2 in at 150 ppi")));
        // Asking for a printed size instead moves the resolution.
        plain.setDisplayedWidth(2.0);
        CHECK(plain.newWidth() == 600 && plain.newHeight() == 300);
        CHECK(nearly(plain.newResolution(), 300.0, 1e-3));

        // Resampling on: the same change keeps the print size and resamples.
        ResizeDialog resampled(QStringLiteral("Resize Image"), 600, 300, 300.0,
                               Unit::Inches, false);
        resampled.setResolution(150.0);
        CHECK(resampled.newWidth() == 300 && resampled.newHeight() == 150);
        CHECK(nearly(resampled.newResolution(), 150.0));
        CHECK(nearly(resampled.displayedWidth(), 2.0, 1e-3));

        // Canvas Size offers the units but no resolution: padding a canvas
        // cannot change how densely it prints.
        ResizeDialog canvas(QStringLiteral("Canvas Size"), 600, 300, 300.0,
                            Unit::Inches, true);
        CHECK(canvas.findChild<QComboBox*>(QStringLiteral("units")) != nullptr);
        CHECK(canvas.findChild<QDoubleSpinBox*>(QStringLiteral("resolution")) == nullptr);
        CHECK(canvas.resamples());
        canvas.setUnit(Unit::Centimetres);
        CHECK(canvas.newWidth() == 600 && canvas.newHeight() == 300);
        canvas.setDisplayedWidth(10.16); // four inches
        CHECK(canvas.newWidth() == 1200 && canvas.newHeight() == 600);
        CHECK(nearly(canvas.newResolution(), 300.0));
    });

    // ------------------------------------------------------------ plugins
    addTest("plugin effects reach the Effects menu", [] {
        plugins::LoadReport& report = plugins::report();
        const plugins::LoadReport saved = report;

        // One plugin that registered an effect, and one file that did not
        // load, which together are everything the window has to report.
        GuiFakePlugin plugin;
        CHECK(plugins::acceptPlugin(&plugin, QStringLiteral("/plugins/gui_fake.so"),
                                    EffectRegistry::instance(), report));
        report.directories.append(QStringLiteral("/plugins"));
        report.failed.push_back({QStringLiteral("/plugins/wrong.so"),
                                 QStringLiteral("built for another version")});

        MainWindow window;
        window.newDocument(32, 24, Rgba(255, 255, 255, 255));

        // The declared category becomes a submenu of its own, after the built
        // in ones, holding the effect with the ellipsis that says it has a
        // generated dialog.
        QMenu* submenu = nullptr;
        for (QMenu* candidate : window.findChildren<QMenu*>()) {
            if (candidate->title() == QStringLiteral("Gui Plugin"))
                submenu = candidate;
        }
        CHECK(submenu != nullptr);
        QAction* effectAction = nullptr;
        if (submenu) {
            for (QAction* candidate : submenu->actions()) {
                if (candidate->text() == QStringLiteral("Gui Fake Effect..."))
                    effectAction = candidate;
            }
        }
        CHECK(effectAction != nullptr);

        // It applies like any other effect, through the same dialog path.
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Effect* effect = EffectRegistry::instance().byId(
            QStringLiteral("gui_fake_effect"));
        CHECK(effect != nullptr);
        if (effect) {
            EffectDialog::applyDirect(effect, view->document(), view->history(),
                                      ParamValues::defaults(effect->params()),
                                      Rgba(0, 0, 0, 255), Rgba(255, 255, 255, 255));
            CHECK(view->document()->activeLayer()->image.at(4, 4).r == 10);
        }

#ifdef TESSEL_EXAMPLE_PLUGIN_FILE
        // The example plugin was built, so the window loaded it for real from
        // the plugins directory beside this binary before it made its menus.
        bool exampleLoaded = false;
        for (const plugins::PluginInfo& info : report.loaded) {
            if (info.name == QStringLiteral("Scanlines Example"))
                exampleLoaded = true;
        }
        CHECK(exampleLoaded);
        CHECK(EffectRegistry::instance().byId(QStringLiteral("example_scanlines"))
              != nullptr);
        bool exampleSubmenu = false;
        for (QMenu* candidate : window.findChildren<QMenu*>()) {
            if (candidate->title() == QStringLiteral("Examples"))
                exampleSubmenu = true;
        }
        CHECK(exampleSubmenu);
#endif

        // A failure is visible without opening anything: the status bar says so
        // once the event loop turns.
        QCoreApplication::processEvents();
        CHECK(window.statusBar()->currentMessage().contains(
            QStringLiteral("failed to load")));

        // And it is still readable afterwards, in the dialog that also says
        // where plugins go.
        QAction* about = window.findChild<QAction*>(QStringLiteral("aboutplugins"));
        CHECK(about != nullptr);
        auto text = std::make_shared<QString>();
        collectPluginsDialog(text);
        if (about)
            about->trigger();
        CHECK(text->contains(QStringLiteral("Gui Fake Plugin")));
        CHECK(text->contains(QStringLiteral("Gui Fake Effect")));
        CHECK(text->contains(QStringLiteral("wrong.so")));
        CHECK(text->contains(QStringLiteral("built for another version")));
        CHECK(text->contains(QStringLiteral("/plugins")));

        // The report is process wide, so leave it as it was found.
        report = saved;
    });

    // ------------------------------------------------------------ recovery
    addTest("autosave writes a recovery copy and a save drops it", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));
        {
            QSettings settings;
            settings.setValue(QStringLiteral("recovery/autosave"), true);
            settings.setValue(QStringLiteral("recovery/autosaveMinutes"), 2);
            settings.setValue(QStringLiteral("recovery/restoreSession"), false);
        }
        auto copies = [&dir] {
            QStringList found;
            QDirIterator walk(dir.filePath(QStringLiteral("recovery")),
                              QStringList{QStringLiteral("*.tessel")},
                              QDir::Files, QDirIterator::Subdirectories);
            while (walk.hasNext())
                found.append(walk.next());
            found.sort();
            return found;
        };

        MainWindow window;
        window.newDocument(40, 30, Rgba(255, 255, 255, 255));
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Document* doc = view->document();
        // An unchanged document has nothing worth writing.
        window.autosaveNow();
        CHECK(copies().isEmpty());

        doc->activeLayer()->image.setAt(5, 6, Rgba(9, 8, 7, 255));
        doc->invalidate();
        doc->setModified(true);
        window.autosaveNow();
        CHECK(copies().size() == 1);
        QString error;
        std::unique_ptr<Document> copy = io::openDocument(copies().first(), &error);
        CHECK(copy != nullptr);
        if (copy) {
            CHECK(copy->width() == 40 && copy->height() == 30);
            CHECK(copy->activeLayer()->image.at(5, 6) == Rgba(9, 8, 7, 255));
        }

        // Saving the document properly leaves nothing to recover.
        const QString target = dir.filePath(QStringLiteral("saved.tessel"));
        doc->setFilePath(target);
        QAction* save = nullptr;
        for (QAction* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("&Save"))
                save = action;
        }
        CHECK(save != nullptr);
        if (save)
            save->trigger();
        CHECK(QFileInfo::exists(target));
        CHECK(!doc->isModified());
        CHECK(copies().isEmpty());
        recovery::setRoot(QString());
    });

    addTest("a recovered document opens modified and unnamed", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));
        const QString original = dir.filePath(QStringLiteral("sketch.tessel"));

        Document source(24, 18, Rgba(1, 2, 3, 255));
        source.activeLayer()->image.setAt(2, 3, Rgba(40, 50, 60, 255));
        {
            recovery::Session session;
            CHECK(session.isOpen());
            CHECK(session.write(4, source.snapshot(), original,
                                QStringLiteral("sketch.tessel"), nullptr));
        }
        std::vector<recovery::Abandoned> abandoned = recovery::findAbandoned();
        CHECK(abandoned.size() == 1);
        const std::vector<recovery::Entry> entries = recovery::allEntries(abandoned);
        CHECK(entries.size() == 1);

        MainWindow window;
        CHECK(window.openRecovered(entries.front()));
        CanvasView* view = window.findChildren<CanvasView*>().last();
        Document* doc = view->document();
        // Modified, so closing it asks; pointing back at the file it came from,
        // never at the recovery copy.
        CHECK(doc->isModified());
        CHECK(doc->filePath() == original);
        CHECK(doc->title().startsWith(QStringLiteral("Recovered")));
        CHECK(doc->activeLayer()->image.at(2, 3) == Rgba(40, 50, 60, 255));

        recovery::discard(abandoned);
        recovery::setRoot(QString());
    });

    addTest("the recovery dialog lists what was found", [] {
        std::vector<recovery::Entry> entries;
        recovery::Entry saved;
        saved.path = QStringLiteral("/recovery/doc-1.tessel");
        saved.originalPath = QStringLiteral("/pictures/beach.png");
        saved.title = QStringLiteral("beach.png");
        saved.size = 2048;
        saved.savedAt = QDateTime::currentDateTime().addSecs(-600);
        entries.push_back(saved);
        recovery::Entry unnamed;
        unnamed.path = QStringLiteral("/recovery/doc-2.tessel");
        unnamed.title = QStringLiteral("Untitled 3");
        unnamed.size = 100000;
        unnamed.savedAt = QDateTime::currentDateTime().addDays(-1);
        entries.push_back(unnamed);

        RecoveryDialog dialog(entries);
        auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("recoveryList"));
        CHECK(list != nullptr);
        if (list) {
            CHECK(list->topLevelItemCount() == 2);
            CHECK(list->topLevelItem(0)->text(0) == QStringLiteral("beach.png"));
            CHECK(!list->topLevelItem(0)->text(1).isEmpty());
            CHECK(list->topLevelItem(0)->text(2) == QStringLiteral("10 minute(s) ago"));
            CHECK(list->topLevelItem(0)->toolTip(0).endsWith(QStringLiteral("beach.png")));
            CHECK(list->topLevelItem(1)->text(0) == QStringLiteral("Untitled 3"));
            CHECK(list->topLevelItem(1)->toolTip(0).contains(QStringLiteral("Never saved")));
        }
        CHECK(RecoveryDialog::describe(saved).contains(QStringLiteral("beach.png")));
        CHECK(RecoveryDialog::describe(saved).contains(QStringLiteral("10 minute(s) ago")));

        // Nothing is chosen until a button is pressed, and only recovering and
        // discarding accept.
        CHECK(dialog.choice() == RecoveryDialog::Choice::Later);
        dialog.chooseRecover();
        CHECK(dialog.choice() == RecoveryDialog::Choice::Recover);
        CHECK(dialog.result() == QDialog::Accepted);
        RecoveryDialog discarding(entries);
        discarding.chooseDiscard();
        CHECK(discarding.choice() == RecoveryDialog::Choice::Discard);
        CHECK(discarding.result() == QDialog::Accepted);
        RecoveryDialog later(entries);
        later.chooseLater();
        CHECK(later.choice() == RecoveryDialog::Choice::Later);
        CHECK(later.result() == QDialog::Rejected);
    });

    addTest("a clean close remembers the open images", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));
        const QString first = dir.filePath(QStringLiteral("one.tessel"));
        const QString second = dir.filePath(QStringLiteral("two.tessel"));
        QString error;
        Document a(10, 10, Rgba(255, 0, 0, 255));
        Document b(12, 8, Rgba(0, 255, 0, 255));
        CHECK(io::saveDocument(a, first, &error));
        CHECK(io::saveDocument(b, second, &error));
        {
            QSettings settings;
            settings.setValue(QStringLiteral("recovery/restoreSession"), true);
            settings.remove(QStringLiteral("session/openDocuments"));
        }

        {
            MainWindow window;
            CHECK(window.openPath(first));
            CHECK(window.openPath(second));
            // Nothing is modified, so the close needs no prompt.
            window.close();
        }
        QSettings settings;
        const recovery::OpenDocuments stored = recovery::loadOpenDocuments(settings);
        CHECK(stored.paths.size() == 2);
        if (stored.paths.size() == 2) {
            CHECK(stored.paths.at(0) == first);
            CHECK(stored.paths.at(1) == second);
        }
        CHECK(stored.current == 1);
        // Closing cleanly also means there is nothing to recover.
        CHECK(recovery::findAbandoned().empty());
        settings.remove(QStringLiteral("session/openDocuments"));
        settings.remove(QStringLiteral("session/current"));
        recovery::setRoot(QString());
    });

    addTest("the startup offer recovers what a crash left behind", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));
        {
            QSettings settings;
            settings.setValue(QStringLiteral("recovery/restoreSession"), false);
        }
        Document source(16, 12, Rgba(7, 7, 7, 255));
        {
            recovery::Session session;
            CHECK(session.isOpen());
            CHECK(session.write(1, source.snapshot(), QString(),
                                QStringLiteral("Untitled 1"), nullptr));
        }

        MainWindow window;
        // The offer is modal, so it is answered from inside the nested event
        // loop it runs, the way a person would answer it.
        QTimer::singleShot(0, &window, [&window] {
            for (RecoveryDialog* dialog : window.findChildren<RecoveryDialog*>())
                dialog->chooseRecover();
        });
        // A backstop, so a dialog that was never found cannot wedge the suite.
        QTimer::singleShot(3000, &window, [] {
            if (QWidget* modal = QApplication::activeModalWidget())
                modal->close();
        });
        window.startSession(QStringList());

        const QList<CanvasView*> views = window.findChildren<CanvasView*>();
        CHECK(views.size() == 1);
        if (views.size() == 1) {
            Document* doc = views.last()->document();
            CHECK(doc->isModified());
            CHECK(doc->filePath().isEmpty()); // it had no file of its own
            CHECK(doc->width() == 16 && doc->height() == 12);
            CHECK(doc->activeLayer()->image.at(1, 1) == Rgba(7, 7, 7, 255));
        }
        // Recovering the files consumes them, so the next start is quiet.
        CHECK(recovery::findAbandoned().empty());
        recovery::setRoot(QString());
    });

    addTest("the startup sequence always leaves an image open", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));
        {
            QSettings settings;
            settings.setValue(QStringLiteral("recovery/restoreSession"), false);
        }
        // Nothing to recover and no reopen offer, so this asks nothing and
        // still ends with a document to draw on.
        MainWindow window;
        window.startSession(QStringList());
        CHECK(window.findChildren<CanvasView*>().size() == 1);
        recovery::setRoot(QString());
    });

    // ------------------------------------------------------------ run
    std::printf("Tessel interface tests\n");
    for (auto& [name, fn] : tests) {
        currentTest = name;
        const int before = failures;
        fn();
        std::printf("  %-44s %s\n", name, failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
