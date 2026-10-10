// Headless interface tests: the real widgets, tools and dialogs are driven
// through their public API with the offscreen Qt platform plugin.
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>

#include "core/ImageOps.h"
#include "core/Io.h"
#include "tools/Tool.h"
#include "ui/CanvasView.h"
#include "ui/ColorPanel.h"
#include "ui/Docks.h"
#include "ui/CurveWidget.h"
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
