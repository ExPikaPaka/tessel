// Headless interface tests: the real widgets, tools and dialogs are driven
// through their public API with the offscreen Qt platform plugin.
#include <cstdio>
#include <functional>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QTemporaryDir>

#include "core/ImageOps.h"
#include "core/Io.h"
#include "tools/Tool.h"
#include "ui/CanvasView.h"
#include "ui/ColorPanel.h"
#include "ui/Docks.h"
#include "ui/EffectDialog.h"
#include "ui/Icons.h"
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

size_t nonTransparentPixels(const Image& image)
{
    size_t count = 0;
    for (size_t i = 0; i < image.pixelCount(); ++i) {
        if (image.data()[i].a != 0)
            ++count;
    }
    return count;
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
        const QStringList names = {
            QStringLiteral("pencil"), QStringLiteral("brush"), QStringLiteral("eraser"),
            QStringLiteral("bucket"), QStringLiteral("gradient"), QStringLiteral("line"),
            QStringLiteral("shapes"), QStringLiteral("text"), QStringLiteral("clone"),
            QStringLiteral("recolor"), QStringLiteral("picker"), QStringLiteral("zoom"),
            QStringLiteral("pan"), QStringLiteral("select-rect"), QStringLiteral("lasso"),
            QStringLiteral("wand"), QStringLiteral("move-selected"),
            QStringLiteral("move-selection"), QStringLiteral("select-ellipse"),
            QStringLiteral("new"), QStringLiteral("open"), QStringLiteral("save"),
            QStringLiteral("undo"), QStringLiteral("redo"), QStringLiteral("layer-add"),
            QStringLiteral("visible"), QStringLiteral("hidden"), QStringLiteral("effect"),
        };
        for (const QString& name : names) {
            const QPixmap pixmap = icons::pixmap(name, 24, Qt::white);
            check(!pixmap.isNull() && pixmap.width() == 24,
                  name.toLocal8Bit().constData(), __LINE__);
            // A glyph must actually draw something.
            const QImage image = pixmap.toImage();
            bool anyInk = false;
            for (int y = 0; y < image.height() && !anyInk; ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (qAlpha(image.pixel(x, y)) > 0) {
                        anyInk = true;
                        break;
                    }
                }
            }
            check(anyInk, name.toLocal8Bit().constData(), __LINE__);
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
        // Every registered tool appears exactly once in the palette.
        for (Tool* tool : ToolRegistry::instance().all())
            check(order.count(tool->id()) == 1, tool->id().toLocal8Bit().constData(),
                  __LINE__);
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
        // A wide, very soft brush, which is the case that beaded worst.
        brush->optionChanged(QStringLiteral("width"), 40);
        brush->optionChanged(QStringLiteral("hardness"), 5);

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
