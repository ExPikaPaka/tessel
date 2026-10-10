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
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>

#include "core/ImageOps.h"
#include "effects/EffectPlugin.h"
#include "effects/EffectPluginHost.h"
#include "core/Io.h"
#include "core/Palette.h"
#include "tools/Tool.h"
#include "ui/CanvasView.h"
#include "ui/ColorPanel.h"
#include "ui/Dialogs.h"
#include "ui/Docks.h"
#include "ui/CurveWidget.h"
#include "ui/EffectDialog.h"
#include "ui/Icons.h"
#include "ui/KeyLayoutFilter.h"
#include "ui/MainWindow.h"
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
                {ParamSpec::intParam(QStringLiteral("amount"), QStringLiteral("Amount"),
                                     0, 255, 10)},
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
        QPushButton* load = panel.findChild<QPushButton*>(QStringLiteral("paletteLoad"));
        QPushButton* save = panel.findChild<QPushButton*>(QStringLiteral("paletteSave"));
        QPushButton* reset = panel.findChild<QPushButton*>(QStringLiteral("paletteReset"));
        QPushButton* pick = panel.findChild<QPushButton*>(QStringLiteral("screenPick"));
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
