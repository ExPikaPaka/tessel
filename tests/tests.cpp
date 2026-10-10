// Headless tests for the Tessel core: pixel maths, documents, history and file
// formats. Run with ctest or by invoking the binary directly.
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include "core/Blend.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/Image.h"
#include "core/ImageOps.h"
#include "core/Io.h"
#include "core/Selection.h"
#include "core/Zip.h"
#include "effects/Effect.h"

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
#define CHECK_NEAR(a, b, tol) check(std::abs(double(a) - double(b)) <= (tol), \
                                    #a " ~= " #b, __LINE__)

std::vector<std::pair<const char*, std::function<void()>>> tests;
void addTest(const char* name, std::function<void()> fn)
{
    tests.emplace_back(name, std::move(fn));
}

Image solid(int w, int h, Rgba color) { return Image(w, h, color); }

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ------------------------------------------------------------ blending
    addTest("blend modes", [] {
        const Rgba base(200, 100, 50, 255);
        const Rgba top(50, 50, 50, 255);
        CHECK(blendOver(base, top, BlendMode::Normal) == top);
        CHECK(blendOver(base, top, BlendMode::Multiply)
              == Rgba(toByte(200 / 255.0f * 50 / 255.0f), toByte(100 / 255.0f * 50 / 255.0f),
                      toByte(50 / 255.0f * 50 / 255.0f), 255));
        CHECK(blendOver(base, top, BlendMode::Lighten) == Rgba(200, 100, 50, 255));
        CHECK(blendOver(base, top, BlendMode::Darken) == Rgba(50, 50, 50, 255));
        CHECK(blendOver(base, top, BlendMode::Screen).r > base.r);
        CHECK(blendOver(base, top, BlendMode::Xor).r == (200 ^ 50));
        // A fully transparent source must not change the backdrop.
        CHECK(blendOver(base, Rgba(0, 255, 0, 0), BlendMode::Normal) == base);
        // Half alpha normal blend sits in the middle.
        const Rgba half = blendOver(Rgba(0, 0, 0, 255), Rgba(255, 255, 255, 128),
                                    BlendMode::Normal);
        CHECK_NEAR(half.r, 128, 2);
        CHECK(half.a == 255);
        CHECK(blendModeFromName(QStringLiteral("Overlay")) == BlendMode::Overlay);
        CHECK(blendModeName(BlendMode::ColorDodge) == QStringLiteral("Color Dodge"));
    });

    addTest("compositing stack", [] {
        Image bottom = solid(4, 4, Rgba(255, 0, 0, 255));
        Image top = solid(4, 4, Rgba(0, 0, 255, 128));
        std::vector<CompositeLayer> layers = {
            {&bottom, 255, BlendMode::Normal, true},
            {&top, 255, BlendMode::Normal, true},
        };
        Image out = compositeLayers(layers, 4, 4);
        CHECK(out.at(0, 0).a == 255);
        CHECK_NEAR(out.at(0, 0).r, 127, 2);
        CHECK_NEAR(out.at(0, 0).b, 128, 2);
        // Hidden and zero opacity layers contribute nothing.
        layers[1].visible = false;
        CHECK(compositeLayers(layers, 4, 4).at(0, 0) == Rgba(255, 0, 0, 255));
        layers[1].visible = true;
        layers[1].opacity = 0;
        CHECK(compositeLayers(layers, 4, 4).at(0, 0) == Rgba(255, 0, 0, 255));
        // Empty stack stays transparent.
        CHECK(compositeLayers({}, 4, 4).at(0, 0).a == 0);
    });

    // ------------------------------------------------------------ image ops
    addTest("blur preserves flat colour", [] {
        Image flat = solid(64, 48, Rgba(80, 120, 200, 255));
        Image blurred = ops::gaussianBlur(flat, 8.0);
        CHECK(blurred.at(0, 0) == Rgba(80, 120, 200, 255));
        CHECK(blurred.at(32, 24) == Rgba(80, 120, 200, 255));
        CHECK(blurred.at(63, 47) == Rgba(80, 120, 200, 255));
    });

    addTest("blur spreads energy", [] {
        Image dot(33, 33, Rgba(0, 0, 0, 0));
        dot.setAt(16, 16, Rgba(255, 255, 255, 255));
        Image blurred = ops::gaussianBlur(dot, 4.0);
        CHECK(blurred.at(16, 16).a > 0);
        CHECK(blurred.at(16, 16).a < 255);
        CHECK(blurred.at(20, 16).a > 0);
        CHECK(blurred.at(16, 16).r > 200); // unpremultiply keeps the hue
        double sum = 0;
        for (size_t i = 0; i < blurred.pixelCount(); ++i)
            sum += blurred.data()[i].a;
        CHECK_NEAR(sum, 255.0, 40.0); // alpha mass is roughly conserved
    });

    addTest("disc and box blur", [] {
        Image flat = solid(40, 40, Rgba(10, 20, 30, 255));
        CHECK(ops::boxBlur(flat, 5).at(20, 20) == Rgba(10, 20, 30, 255));
    });

    addTest("bilinear sampling", [] {
        Image two(2, 1);
        two.setAt(0, 0, Rgba(0, 0, 0, 255));
        two.setAt(1, 0, Rgba(255, 255, 255, 255));
        CHECK(ops::sampleBilinear(two, 0.0f, 0.0f, ops::EdgeMode::Clamp).r == 0);
        CHECK(ops::sampleBilinear(two, 1.0f, 0.0f, ops::EdgeMode::Clamp).r == 255);
        CHECK_NEAR(ops::sampleBilinear(two, 0.5f, 0.0f, ops::EdgeMode::Clamp).r, 128, 2);
        // Wrapping reads from the other side.
        CHECK(ops::sampleBilinear(two, -1.0f, 0.0f, ops::EdgeMode::Wrap).r == 255);
        // Transparent edges stay transparent.
        CHECK(ops::sampleBilinear(two, -5.0f, 0.0f, ops::EdgeMode::Transparent).a == 0);
    });

    addTest("geometry", [] {
        Image img(4, 2, Rgba(0, 0, 0, 255));
        img.setAt(0, 0, Rgba(255, 0, 0, 255));
        Image rotated = ops::rotate90(img, 1);
        CHECK(rotated.width() == 2 && rotated.height() == 4);
        CHECK(rotated.at(1, 0) == Rgba(255, 0, 0, 255));
        CHECK(ops::rotate90(ops::rotate90(img, 2), 2).at(0, 0) == Rgba(255, 0, 0, 255));
        CHECK(ops::flip(img, true).at(3, 0) == Rgba(255, 0, 0, 255));
        CHECK(ops::flip(img, false).at(0, 1) == Rgba(255, 0, 0, 255));
        Image scaled = ops::resize(img, 8, 4, false);
        CHECK(scaled.width() == 8 && scaled.height() == 4);
        Image cropped = img.cropped(Rect(1, 0, 2, 2));
        CHECK(cropped.width() == 2 && cropped.at(0, 0) == Rgba(0, 0, 0, 255));
        // Cropping outside the image yields transparent padding.
        CHECK(img.cropped(Rect(-2, -2, 2, 2)).at(0, 0).a == 0);
    });

    addTest("colour conversions", [] {
        float h = 0, s = 0, v = 0, r = 0, g = 0, b = 0;
        ops::rgbToHsv(1.0f, 0.0f, 0.0f, h, s, v);
        CHECK_NEAR(h, 0.0, 0.01);
        CHECK_NEAR(s, 1.0, 0.01);
        ops::rgbToHsv(0.2f, 0.4f, 0.6f, h, s, v);
        ops::hsvToRgb(h, s, v, r, g, b);
        CHECK_NEAR(r, 0.2, 0.01);
        CHECK_NEAR(g, 0.4, 0.01);
        CHECK_NEAR(b, 0.6, 0.01);
        CHECK(ops::luminance8(Rgba(255, 255, 255, 255)) >= 254);
        CHECK(ops::luminance8(Rgba(0, 0, 0, 255)) == 0);
    });

    addTest("curve lookup tables", [] {
        std::vector<QPointF> identity = {QPointF(0, 0), QPointF(255, 255)};
        ops::Lut lut = ops::lutFromControlPoints(identity);
        for (int i = 0; i < 256; ++i)
            CHECK(lut[size_t(i)] == i);
        std::vector<QPointF> lift = {QPointF(0, 0), QPointF(128, 192), QPointF(255, 255)};
        ops::Lut lifted = ops::lutFromControlPoints(lift);
        CHECK(lifted[128] > 180);
        CHECK(lifted[0] == 0);
        CHECK(lifted[255] == 255);
        // Monotone interpolation must never overshoot.
        for (int i = 1; i < 256; ++i)
            CHECK(lifted[size_t(i)] >= lifted[size_t(i - 1)]);
    });

    addTest("histogram", [] {
        Image img = solid(10, 10, Rgba(100, 100, 100, 255));
        ops::Histogram hist = ops::computeHistogram(img, nullptr, img.rect());
        CHECK(hist.samples == 100);
        CHECK(hist.red[100] == 100);
        CHECK(hist.percentile(hist.red, 0.5) == 100);
    });

    // ------------------------------------------------------------ painting
    addTest("brush stroke", [] {
        Mask kernel = ops::brushKernel(10.0, 0.75);
        CHECK(kernel.width() == kernel.height());
        CHECK(kernel.at(kernel.width() / 2, kernel.height() / 2) > 0.99f);
        CHECK(kernel.at(0, 0) < 0.01f);
        std::vector<QPointF> points = {QPointF(5, 5), QPointF(50, 30)};
        Mask stroke = ops::strokeMask(64, 64, points, 8.0, 0.8, 0.2, 1.0f);
        CHECK(stroke.at(5, 5) > 0.5f);
        CHECK(stroke.at(50, 30) > 0.5f);
        CHECK(stroke.at(27, 17) > 0.5f); // middle of the line is covered
        CHECK(stroke.at(60, 60) < 0.01f);
        Rect bounds = ops::strokeBounds(points, 8.0);
        CHECK(bounds.contains(5, 5) && bounds.contains(50, 30));
    });

    addTest("colour over and erase", [] {
        Image target = solid(8, 8, Rgba(0, 0, 0, 255));
        Mask mask(8, 8, 1.0f);
        ops::colorOver(target, mask, Rgba(255, 0, 0, 255), BlendMode::Normal, target.rect());
        CHECK(target.at(4, 4) == Rgba(255, 0, 0, 255));
        ops::eraseMask(target, mask, 1.0f, target.rect());
        CHECK(target.at(4, 4).a == 0);
        // Half strength erase halves alpha.
        Image half = solid(4, 4, Rgba(9, 9, 9, 200));
        Mask full(4, 4, 1.0f);
        ops::eraseMask(half, full, 0.5f, half.rect());
        CHECK_NEAR(half.at(0, 0).a, 100, 2);
    });

    addTest("flood fill", [] {
        Image img = solid(32, 32, Rgba(255, 255, 255, 255));
        for (int y = 0; y < 32; ++y)
            img.setAt(16, y, Rgba(0, 0, 0, 255)); // a wall down the middle
        Mask left = ops::floodFill(img, 4, 4, 0.1, true, false);
        CHECK(left.at(4, 4) > 0.5f);
        CHECK(left.at(15, 20) > 0.5f);
        CHECK(left.at(20, 4) < 0.5f);   // the wall stops the fill
        CHECK(left.at(16, 4) < 0.5f);
        Mask global = ops::floodFill(img, 4, 4, 0.1, false, false);
        CHECK(global.at(20, 4) > 0.5f); // global mode crosses the wall
    });

    // ------------------------------------------------------------ selection
    addTest("selection masks", [] {
        Selection sel(20, 10);
        CHECK(!sel.isActive());
        CHECK(sel.clipMask() == nullptr);
        CHECK(sel.bounds().w == 20);
        CHECK(sel.contains(5, 5));

        Mask rect(20, 10);
        for (int y = 2; y < 6; ++y)
            for (int x = 4; x < 12; ++x)
                rect.setAt(x, y, 1.0f);
        sel.setMask(rect);
        CHECK(sel.isActive());
        CHECK(sel.pixelCount() == 32);
        const Rect bounds = sel.bounds();
        CHECK(bounds.x == 4 && bounds.y == 2 && bounds.w == 8 && bounds.h == 4);
        CHECK(sel.contains(5, 3));
        CHECK(!sel.contains(0, 0));

        // The outline is a single rectangle for a rectangular selection.
        std::vector<Rect> rects = maskToRects(rect);
        CHECK(rects.size() == 1);
        CHECK(rects[0].x == 4 && rects[0].w == 8 && rects[0].h == 4);
        CHECK(!sel.outline().isEmpty());

        sel.invert();
        CHECK(sel.isActive());
        CHECK(sel.pixelCount() == 200 - 32);
        CHECK(!sel.contains(5, 3));

        Mask other(20, 10);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 20; ++x)
                other.setAt(x, y, 1.0f);
        sel.combine(other, CombineMode::Union);
        CHECK(sel.pixelCount() == 200);
        sel.combine(rect, CombineMode::Intersect);
        CHECK(sel.pixelCount() == 32);
        sel.combine(rect, CombineMode::Subtract);
        CHECK(!sel.isActive()); // nothing left means no selection
    });

    addTest("selection transforms", [] {
        Selection sel(10, 6);
        Mask mask(10, 6);
        mask.setAt(0, 0, 1.0f);
        sel.setMask(mask);
        sel.transform(true, false, 0);
        CHECK(sel.contains(9, 0));
        sel.transform(false, false, 1);
        CHECK(sel.width() == 6 && sel.height() == 10);
        CHECK(sel.contains(5, 9)); // a quarter turn moves the corner pixel
        sel.translate(-1, -1);
        CHECK(sel.pixelCount() == 1);
        CHECK(sel.contains(4, 8));
        // Translating the last pixel off the canvas clears the selection.
        sel.translate(-100, 0);
        CHECK(!sel.isActive());
    });

    // ------------------------------------------------------------ document
    addTest("document layers", [] {
        Document doc(16, 12);
        CHECK(doc.layerCount() == 1);
        CHECK(doc.composite().at(0, 0) == Rgba(255, 255, 255, 255));
        Layer* top = doc.addEmptyLayer();
        CHECK(doc.layerCount() == 2);
        CHECK(doc.activeIndex() == 1);
        CHECK(doc.activeLayer() == top);
        CHECK(top->isEmpty());
        ops::fillRegion(top->image, Rect(0, 0, 16, 12), Rgba(0, 0, 255, 255));
        doc.invalidate();
        CHECK(doc.composite().at(0, 0) == Rgba(0, 0, 255, 255));
        top->opacity = 128;
        doc.invalidate();
        CHECK_NEAR(doc.composite().at(0, 0).r, 127, 2);

        // Names stay unique.
        CHECK(doc.uniqueLayerName(QStringLiteral("Background"))
              != QStringLiteral("Background"));

        // Merging down bakes opacity into the lower layer.
        CHECK(doc.mergeLayerDown(1));
        CHECK(doc.layerCount() == 1);
        CHECK_NEAR(doc.layerAt(0)->image.at(0, 0).r, 127, 2);

        // The last layer cannot be removed.
        CHECK(!doc.removeLayer(0));

        doc.addEmptyLayer();
        doc.addEmptyLayer();
        CHECK(doc.layerCount() == 3);
        doc.moveLayer(2, 0);
        CHECK(doc.activeIndex() == 0);
        doc.flatten();
        CHECK(doc.layerCount() == 1);
    });

    addTest("document canvas operations", [] {
        Document doc(10, 20);
        doc.rotate90(1);
        CHECK(doc.width() == 20 && doc.height() == 10);
        doc.resizeCanvas(40, 30, Anchor::TopLeft, false);
        CHECK(doc.width() == 40 && doc.height() == 30);
        CHECK(doc.layerAt(0)->image.at(39, 29).a == 0); // new area is transparent
        CHECK(doc.layerAt(0)->image.at(0, 0).a == 255);
        doc.resizeCanvas(20, 15, Anchor::Center, true);
        CHECK(doc.width() == 20 && doc.composite().width() == 20);
        doc.cropTo(Rect(2, 2, 8, 8));
        CHECK(doc.width() == 8 && doc.height() == 8);
        CHECK(doc.composite().width() == 8);
        doc.flip(true);
        CHECK(doc.width() == 8);
    });

    addTest("anchor offsets", [] {
        CHECK(anchorOffset(Anchor::TopLeft, 10, 10, 20, 20) == QPoint(0, 0));
        CHECK(anchorOffset(Anchor::Center, 10, 10, 20, 20) == QPoint(5, 5));
        CHECK(anchorOffset(Anchor::BottomRight, 10, 10, 20, 20) == QPoint(10, 10));
    });

    // ------------------------------------------------------------ history
    addTest("pixel history", [] {
        Document doc(20, 20);
        History history(&doc);
        Layer* layer = doc.activeLayer();
        const Rgba before = layer->image.at(5, 5);
        {
            EditScope scope(doc, history, layer, QStringLiteral("Paintbrush"),
                            QStringLiteral("brush"));
            ops::fillRegion(layer->image, Rect(4, 4, 4, 4), Rgba(1, 2, 3, 255));
        }
        CHECK(history.count() == 1);
        CHECK(history.canUndo());
        CHECK(history.undoName() == QStringLiteral("Paintbrush"));
        CHECK(layer->image.at(5, 5) == Rgba(1, 2, 3, 255));
        CHECK(history.undo());
        CHECK(doc.activeLayer()->image.at(5, 5) == before);
        CHECK(history.canRedo());
        CHECK(history.redo());
        CHECK(doc.activeLayer()->image.at(5, 5) == Rgba(1, 2, 3, 255));

        // Only the changed rectangle is stored, so the patch stays small.
        CHECK(history.byteCount() < 4 * 4 * 4 * 2 + 64);

        // An edit that changes nothing is not recorded.
        {
            EditScope scope(doc, history, layer, QStringLiteral("Nothing"),
                            QStringLiteral("edit"));
        }
        CHECK(history.count() == 1);
    });

    addTest("structural history and jumping", [] {
        Document doc(8, 8);
        History history(&doc);
        recordDocumentEdit(doc, history, QStringLiteral("Add Layer"),
                           QStringLiteral("layer"), [&] { doc.addEmptyLayer(); });
        recordDocumentEdit(doc, history, QStringLiteral("Flatten"),
                           QStringLiteral("flatten"), [&] { doc.flatten(); });
        CHECK(doc.layerCount() == 1);
        history.goTo(0);
        CHECK(doc.layerCount() == 2);
        history.goTo(-1);
        CHECK(doc.layerCount() == 1);
        history.goTo(1);
        CHECK(doc.layerCount() == 1);
        CHECK(history.currentIndex() == 1);

        // New work after an undo drops the redo tail.
        history.goTo(0);
        recordDocumentEdit(doc, history, QStringLiteral("Add Layer"),
                           QStringLiteral("layer"), [&] { doc.addEmptyLayer(); });
        CHECK(history.count() == 2);
        CHECK(!history.canRedo());

        history.markSaved();
        CHECK(!doc.isModified());
        history.undo();
        CHECK(doc.isModified());
    });

    addTest("selection history", [] {
        Document doc(8, 8);
        History history(&doc);
        recordSelectionEdit(doc, history, QStringLiteral("Select All"),
                            [&] { doc.selection().selectAll(); });
        CHECK(doc.selection().isActive());
        history.undo();
        CHECK(!doc.selection().isActive());
        history.redo();
        CHECK(doc.selection().isActive());
    });

    addTest("history limits", [] {
        Document doc(8, 8);
        History history(&doc);
        history.setLimits(3, 1024ull * 1024 * 1024);
        for (int i = 0; i < 6; ++i) {
            recordDocumentEdit(doc, history, QStringLiteral("Step"),
                               QStringLiteral("edit"), [&] { doc.addEmptyLayer(); });
        }
        CHECK(history.count() == 3);
        CHECK(history.currentIndex() == 2);
    });

    // ------------------------------------------------------------ zip and io
    addTest("zip round trip", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("test.zip"));
        QByteArray payload;
        for (int i = 0; i < 5000; ++i)
            payload.append(char('a' + (i % 26)));
        {
            ZipWriter writer(path);
            CHECK(writer.isOpen());
            CHECK(writer.add(QStringLiteral("stored.txt"), QByteArray("hello"), false));
            CHECK(writer.add(QStringLiteral("deflated.txt"), payload, true));
            CHECK(writer.close());
        }
        ZipReader reader(path);
        CHECK(reader.isValid());
        CHECK(reader.names().size() == 2);
        CHECK(reader.contains(QStringLiteral("stored.txt")));
        CHECK(reader.read(QStringLiteral("stored.txt")) == QByteArray("hello"));
        CHECK(reader.read(QStringLiteral("deflated.txt")) == payload);
        CHECK(reader.read(QStringLiteral("missing.txt")).isEmpty());
    });

    addTest("layered file round trip", [] {
        QTemporaryDir dir;
        Document doc(24, 16);
        Layer* top = doc.addEmptyLayer();
        top->name = QStringLiteral("Paint");
        top->opacity = 180;
        top->mode = BlendMode::Multiply;
        ops::fillRegion(top->image, Rect(2, 2, 10, 8), Rgba(10, 200, 30, 255));
        Mask mask(24, 16);
        for (int y = 1; y < 5; ++y)
            for (int x = 1; x < 5; ++x)
                mask.setAt(x, y, 1.0f);
        doc.selection().setMask(mask);

        for (const char* extension : {"tessel", "ora"}) {
            const QString path = dir.filePath(QStringLiteral("doc.%1").arg(extension));
            QString error;
            CHECK(io::saveDocument(doc, path, &error));
            CHECK(error.isEmpty());
            auto loaded = io::openDocument(path, &error);
            CHECK(loaded != nullptr);
            if (!loaded)
                continue;
            CHECK(loaded->width() == 24 && loaded->height() == 16);
            CHECK(loaded->layerCount() == 2);
            CHECK(loaded->layerAt(1)->name == QStringLiteral("Paint"));
            CHECK(loaded->layerAt(1)->opacity == 180);
            CHECK(loaded->layerAt(1)->mode == BlendMode::Multiply);
            CHECK(loaded->layerAt(1)->image.at(3, 3) == Rgba(10, 200, 30, 255));
            CHECK(!loaded->isModified());
        }
        // The selection survives in the native format only.
        QString error;
        auto native = io::openDocument(dir.filePath(QStringLiteral("doc.tessel")), &error);
        CHECK(native && native->selection().isActive());
    });

    addTest("flat file round trip", [] {
        QTemporaryDir dir;
        Document doc(20, 10);
        ops::fillRegion(doc.layerAt(0)->image, Rect(0, 0, 20, 10), Rgba(12, 34, 56, 255));
        for (const char* extension : {"png", "bmp", "jpg", "webp", "tiff"}) {
            const QString path = dir.filePath(QStringLiteral("flat.%1").arg(extension));
            QString error;
            if (!io::canWrite(path))
                continue;
            CHECK(io::saveDocument(doc, path, &error, 95));
            auto loaded = io::openDocument(path, &error);
            CHECK(loaded != nullptr);
            if (!loaded)
                continue;
            CHECK(loaded->width() == 20 && loaded->height() == 10);
            CHECK(loaded->layerCount() == 1);
            const Rgba pixel = loaded->composite().at(10, 5);
            const int tolerance = io::isLossyFormat(path) ? 12 : 0;
            CHECK(std::abs(int(pixel.r) - 12) <= tolerance);
            CHECK(std::abs(int(pixel.b) - 56) <= tolerance);
        }
        // Alpha survives a PNG round trip.
        Document alpha(4, 4);
        alpha.layerAt(0)->image.fill(Rgba(255, 0, 0, 64));
        alpha.invalidate();
        const QString path = dir.filePath(QStringLiteral("alpha.png"));
        QString error;
        CHECK(io::saveDocument(alpha, path, &error));
        auto loaded = io::openDocument(path, &error);
        CHECK(loaded && loaded->composite().at(0, 0).a == 64);
    });

    addTest("io helpers", [] {
        CHECK(io::isLayeredFormat(QStringLiteral("/tmp/a.tessel")));
        CHECK(io::isLayeredFormat(QStringLiteral("/tmp/a.ORA")));
        CHECK(!io::isLayeredFormat(QStringLiteral("/tmp/a.png")));
        CHECK(io::flattensLayers(QStringLiteral("a.png")));
        CHECK(io::isLossyFormat(QStringLiteral("a.jpg")));
        CHECK(!io::isLossyFormat(QStringLiteral("a.png")));
        CHECK(io::readableExtensions().contains(QStringLiteral("png")));
        CHECK(io::openFilter().contains(QStringLiteral("tessel")));
        CHECK(io::saveFilter().contains(QStringLiteral("PNG")));
        QString error;
        CHECK(io::openDocument(QStringLiteral("/nonexistent/file.png"), &error) == nullptr);
        CHECK(!error.isEmpty());
        // .pdn gives a helpful message rather than a crash.
        error.clear();
        CHECK(io::openDocument(QStringLiteral("/tmp/does-not-exist.pdn"), &error) == nullptr);
        CHECK(!error.isEmpty());
    });

    // ------------------------------------------------------------ effects
    addTest("effect registry", [] {
        registerBuiltinEffects();
        EffectRegistry& registry = EffectRegistry::instance();
        CHECK(registry.all().size() >= 30);
        CHECK(registry.byId(QStringLiteral("gaussian_blur")) != nullptr);
        CHECK(registry.byId(QStringLiteral("nope")) == nullptr);
        CHECK(registry.categories().contains(QStringLiteral("Adjustments")));
        CHECK(registry.categories().contains(QStringLiteral("Blurs")));
        CHECK(!registry.inCategory(QStringLiteral("Distort")).empty());
        CHECK(registry.byId(QStringLiteral("invert_colors"))->isAdjustment());
        CHECK(!registry.byId(QStringLiteral("gaussian_blur"))->isAdjustment());
    });

    addTest("every effect runs", [] {
        registerBuiltinEffects();
        Image source(48, 32);
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 48; ++x) {
                source.setAt(x, y, Rgba(uint8_t(x * 5), uint8_t(y * 7),
                                        uint8_t((x + y) * 3), 255));
            }
        }
        EffectContext context;
        context.source = &source;
        context.region = source.rect();
        context.primary = Rgba(220, 30, 40, 255);
        context.secondary = Rgba(255, 255, 255, 255);
        for (Effect* effect : EffectRegistry::instance().all()) {
            const ParamValues values = ParamValues::defaults(effect->params());
            const Image out = effect->apply(context, values);
            check(out.width() == source.width() && out.height() == source.height(),
                  effect->id().toLocal8Bit().constData(), __LINE__);
            // Results must stay inside the canvas and keep valid pixels.
            check(!out.isNull(), effect->id().toLocal8Bit().constData(), __LINE__);
        }
    });

    addTest("effects honour the region", [] {
        registerBuiltinEffects();
        Image source(32, 32, Rgba(200, 200, 200, 255));
        EffectContext context;
        context.source = &source;
        context.region = Rect(8, 8, 8, 8);
        Effect* invert = EffectRegistry::instance().byId(QStringLiteral("invert_colors"));
        CHECK(invert != nullptr);
        const Image out = invert->apply(context, ParamValues::defaults(invert->params()));
        CHECK(out.at(10, 10).r == 55);          // inside the region
        CHECK(out.at(0, 0).r == 200);           // untouched outside
        CHECK(out.at(31, 31).r == 200);
    });

    addTest("effect parameter defaults", [] {
        std::vector<ParamSpec> specs = {
            ParamSpec::intParam(QStringLiteral("i"), QStringLiteral("I"), 0, 10, 7),
            ParamSpec::boolParam(QStringLiteral("b"), QStringLiteral("B"), true),
            ParamSpec::doubleParam(QStringLiteral("d"), QStringLiteral("D"), 0, 1, 0.25),
            ParamSpec::colorParam(QStringLiteral("c"), QStringLiteral("C"),
                                  Rgba(1, 2, 3, 255)),
            ParamSpec::offsetParam(QStringLiteral("o"), QStringLiteral("O"), 0.5, -0.5),
            ParamSpec::choiceParam(QStringLiteral("ch"), QStringLiteral("Ch"),
                                   {QStringLiteral("a"), QStringLiteral("b")}, 1),
        };
        const ParamValues values = ParamValues::defaults(specs);
        CHECK(values.intValue(QStringLiteral("i")) == 7);
        CHECK(values.boolValue(QStringLiteral("b")));
        CHECK_NEAR(values.doubleValue(QStringLiteral("d")), 0.25, 1e-9);
        CHECK(values.colorValue(QStringLiteral("c")) == Rgba(1, 2, 3, 255));
        CHECK(values.pointValue(QStringLiteral("o")) == QPointF(0.5, -0.5));
        CHECK(values.choiceValue(QStringLiteral("ch")) == 1);
        CHECK(values.intValue(QStringLiteral("absent"), 42) == 42);
    });

    // ------------------------------------------------------------ run
    std::printf("Tessel core tests\n");
    for (auto& [name, fn] : tests) {
        currentTest = name;
        const int before = failures;
        fn();
        std::printf("  %-36s %s\n", name, failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
