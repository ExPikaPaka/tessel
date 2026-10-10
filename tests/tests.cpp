// Headless tests for the Tessel core: pixel maths, documents, history and file
// formats. Run with ctest or by invoking the binary directly.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTransform>

#include "core/Blend.h"
#include "core/Parallel.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/Image.h"
#include "core/ImageOps.h"
#include "core/Io.h"
#include "core/Palette.h"
#include "core/Recovery.h"
#include "core/Selection.h"
#include "core/Units.h"
#include "core/Zip.h"
#include "effects/Effect.h"
#include "effects/EffectPlugin.h"
#include "effects/EffectPluginHost.h"

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

// Byte for byte comparison, which is what the effect tests need.
bool samePixels(const Image& a, const Image& b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return false;
    return std::memcmp(a.data(), b.data(), a.byteCount()) == 0;
}

// A deterministic non flat image, so an effect that does nothing is visible.
Image gradient(int w, int h)
{
    Image image(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            image.setAt(x, y, Rgba(uint8_t((x * 7) & 0xff), uint8_t((y * 11) & 0xff),
                                   uint8_t(((x ^ y) * 5) & 0xff), 255));
        }
    }
    return image;
}

// How often the counting effect below actually ran, which is how the caching
// tests tell a recompute from a cache hit.
int countingRuns = 0;

// A stored effect with a parameter, registered on demand. The registry ignores
// a second registration of the same id, so calling this from several tests is
// safe.
void registerCountingEffect()
{
    fx::add(EffectRegistry::instance(), "test_counting", "Counting", "Testing",
            {ParamSpec::intParam(QStringLiteral("shift"), "Shift", 0, 200, 10)},
            [](const EffectContext& context, const ParamValues& values) {
                ++countingRuns;
                const int shift = values.intValue(QStringLiteral("shift"), 10);
                return fx::mapPixels(context, [shift](Rgba c, int, int) {
                    return Rgba(uint8_t(std::min(255, c.r + shift)), c.g, c.b, c.a);
                });
            });
}

// A plugin that never left the process, which is how the loader's version gate
// and its reporting are tested without building a library for every case.
class FakePlugin : public EffectPlugin {
public:
    FakePlugin(int interfaceVersion, QString effectId, QString category)
        : version_(interfaceVersion), id_(std::move(effectId)),
          category_(std::move(category))
    {
    }

    int interfaceVersion() const override { return version_; }
    QString name() const override { return QStringLiteral("Fake Plugin"); }
    QString version() const override { return QStringLiteral("2.3"); }
    QString author() const override { return QStringLiteral("Nobody"); }

    void registerEffects(EffectRegistry& registry) override
    {
        fx::add(registry, id_.toLatin1().constData(), "Fake Effect",
                category_.toLatin1().constData(),
                {ParamSpec::intParam(QStringLiteral("amount"), "Amount", 0, 10, 3)},
                [](const EffectContext& context, const ParamValues& values) {
                    const int amount = values.intValue(QStringLiteral("amount"), 3);
                    return fx::mapPixels(context, [amount](Rgba c, int, int) {
                        return Rgba(uint8_t(amount), c.g, c.b, c.a);
                    });
                });
    }

private:
    int version_;
    QString id_;
    QString category_;
};

// What a library is called on this platform, so the loader sees a file it is
// meant to try to open.
const char* libraryName()
{
#ifdef Q_OS_WIN
    return "broken.dll";
#elif defined(Q_OS_MACOS)
    return "broken.dylib";
#else
    return "broken.so";
#endif
}

// The effects that carry the cooperative progress channel.
const char* const kProgressEffects[] = {"surface_blur", "median",   "oil_painting",
                                        "reduce_noise", "crystalize", "julia",
                                        "mandelbrot"};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ------------------------------------------------------------ blending
    addTest("a parallel pass visits every row, batch after batch", [] {
        // The pool keeps its workers between calls, so one batch has to be
        // completely over before the next is set up. A worker still inside the
        // previous batch would take an index from the new one while holding a
        // pointer to the old batch's lambda, which by then is a destroyed
        // stack object: a crash, and a row of the new batch left undone.
        // This does not reproduce that race on demand, it was found by running
        // the whole interface suite in a loop; what it does is hold the pool
        // to the invariant that broke, over thousands of back to back passes
        // cut as finely as the pool allows.
        bool everyRowOnce = true;
        bool rangesDisjoint = true;
        for (int round = 0; round < 3000 && everyRowOnce; ++round) {
            const int rows = 1 + (round % 97);
            std::vector<int> seen(size_t(rows), 0);
            parallelFor(0, rows, [&](int lo, int hi) {
                if (lo < 0 || hi > rows || lo >= hi)
                    rangesDisjoint = false;
                for (int y = lo; y < hi; ++y)
                    ++seen[size_t(y)];
                // A little work in each chunk, so the workers are really in
                // the pass when the caller comes out of it.
                volatile double sink = 0.0;
                for (int i = 0; i < 400; ++i)
                    sink += double(i) * 0.5;
                (void)sink;
            }, 1);
            for (int y = 0; y < rows; ++y) {
                if (seen[size_t(y)] != 1)
                    everyRowOnce = false;
            }
        }
        CHECK(everyRowOnce);
        CHECK(rangesDisjoint);
    });

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

    addTest("selection feather", [] {
        Selection sel(64, 64);
        Mask rect(64, 64);
        for (int y = 20; y < 44; ++y)
            for (int x = 20; x < 44; ++x)
                rect.setAt(x, y, 1.0f);
        sel.setMask(rect);
        CHECK(sel.clipMask()->at(19, 32) == 0.0f); // a hard edge to begin with

        sel.feather(4.0);
        const Mask* mask = sel.clipMask();
        CHECK(mask != nullptr);
        // The core stays solid and the edge becomes a ramp that straddles it.
        CHECK(mask->at(32, 32) == 1.0f);
        CHECK(mask->at(20, 32) > 0.0f && mask->at(20, 32) < 1.0f);
        CHECK(mask->at(19, 32) > 0.0f && mask->at(19, 32) < 0.5f);
        CHECK(mask->at(32, 20) > 0.0f && mask->at(32, 20) < 1.0f);
        // The ramp is as wide as the radius and no wider.
        CHECK(mask->at(10, 32) == 0.0f);
        CHECK(mask->at(32, 53) == 0.0f);
        // Feathering moves weight across the edge without moving the edge.
        CHECK_NEAR(double(sel.pixelCount()), 24.0 * 24.0, 24.0 * 4.0);

        // Nothing selected means everything, which has no edge to soften.
        Selection empty(16, 16);
        empty.feather(3.0);
        CHECK(!empty.isActive());
    });

    addTest("selection grow and shrink", [] {
        Selection sel(80, 80);
        Mask rect(80, 80);
        for (int y = 30; y < 50; ++y)
            for (int x = 30; x < 50; ++x)
                rect.setAt(x, y, 1.0f);
        sel.setMask(rect);
        const size_t area = sel.pixelCount();
        CHECK(area == 400);

        sel.grow(5);
        CHECK(sel.pixelCount() > area);
        Rect bounds = sel.bounds();
        CHECK(bounds.x == 25 && bounds.y == 25);
        CHECK(bounds.w == 30 && bounds.h == 30);
        // Dilation stays round, so the corners are cut rather than square.
        CHECK(sel.pixelCount() < 900);
        CHECK(!sel.contains(26, 26));
        CHECK(sel.contains(26, 40));

        // Eroding by the same amount comes back to roughly where it started.
        sel.grow(-5);
        bounds = sel.bounds();
        CHECK(std::abs(bounds.x - 30) <= 1 && std::abs(bounds.y - 30) <= 1);
        CHECK(std::abs(bounds.w - 20) <= 2 && std::abs(bounds.h - 20) <= 2);
        CHECK_NEAR(double(sel.pixelCount()), double(area), 40.0);

        // Shrinking on its own takes area away.
        sel.setMask(rect);
        sel.grow(-4);
        CHECK(sel.pixelCount() < area);
        bounds = sel.bounds();
        CHECK(bounds.x == 34 && bounds.w == 12);

        // Eroding everything away leaves nothing selected.
        sel.setMask(rect);
        sel.grow(-40);
        CHECK(!sel.isActive());
    });

    addTest("selection border", [] {
        Selection sel(60, 60);
        Mask rect(60, 60);
        for (int y = 10; y < 40; ++y)
            for (int x = 10; x < 40; ++x)
                rect.setAt(x, y, 1.0f);
        sel.setMask(rect);
        const Rect before = sel.bounds();
        const size_t area = sel.pixelCount();

        sel.border(3);
        const Rect after = sel.bounds();
        // The band lies inside the old edge, so the bounds do not move.
        CHECK(after.x == before.x && after.y == before.y);
        CHECK(after.w == before.w && after.h == before.h);
        // It is a ring: the rim is in and the middle is not.
        CHECK(sel.contains(10, 25) && sel.contains(39, 25));
        CHECK(sel.contains(25, 10) && sel.contains(25, 39));
        CHECK(sel.contains(12, 25) && !sel.contains(13, 25));
        CHECK(!sel.contains(25, 25));
        CHECK(sel.pixelCount() == area - 24 * 24);
    });

    addTest("selection follows a transform", [] {
        Selection sel(41, 41);
        Mask rect(41, 41);
        for (int y = 10; y < 20; ++y)
            for (int x = 4; x < 8; ++x)
                rect.setAt(x, y, 1.0f);
        sel.setMask(rect);

        sel.transformBy(QTransform::fromTranslate(3, 0));
        Rect bounds = sel.bounds();
        CHECK(bounds.x == 7 && bounds.y == 10);
        CHECK(bounds.w == 4 && bounds.h == 10);
        CHECK(sel.pixelCount() == 40);

        // A quarter turn about the canvas centre swaps the extents.
        QTransform turn;
        turn.translate(20.5, 20.5);
        turn.rotate(90.0);
        turn.translate(-20.5, -20.5);
        sel.transformBy(turn);
        bounds = sel.bounds();
        CHECK(bounds.w == 10 && bounds.h == 4);
        CHECK_NEAR(double(sel.pixelCount()), 40.0, 6.0);
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

    addTest("layer masks", [] {
        Document doc(8, 8, Rgba(0, 0, 255, 255));
        Layer* top = doc.addEmptyLayer();
        top->image.fill(Rgba(255, 0, 0, 255));
        doc.invalidate();
        CHECK(doc.composite().at(0, 0) == Rgba(255, 0, 0, 255));

        Layer* mask = doc.addMask(top);
        CHECK(mask != nullptr);
        CHECK(top->hasMask());
        CHECK(top->maskEnabled());
        CHECK(mask->uid() != top->uid());
        // A fresh mask is white, so the layer shows as before.
        doc.invalidate();
        CHECK(doc.composite().at(0, 0) == Rgba(255, 0, 0, 255));

        // Black hides, white reveals.
        ops::fillRegion(mask->image, Rect(0, 0, 4, 8), Rgba(0, 0, 0, 255));
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(0, 0, 255, 255));
        CHECK(doc.composite().at(5, 1) == Rgba(255, 0, 0, 255));
        // A grey mask value lets part of the layer through.
        ops::fillRegion(mask->image, Rect(0, 0, 2, 8), Rgba(128, 128, 128, 255));
        doc.invalidate();
        CHECK_NEAR(doc.composite().at(0, 0).r, 128, 3);

        // Toggling the mask off ignores it entirely.
        top->setMaskEnabled(false);
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(255, 0, 0, 255));
        top->setMaskEnabled(true);
        doc.invalidate();

        // Inverting swaps what is hidden and what is revealed.
        ops::fillRegion(mask->image, Rect(0, 0, 4, 8), Rgba(0, 0, 0, 255));
        top->invertMask();
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(255, 0, 0, 255));
        CHECK(doc.composite().at(5, 1) == Rgba(0, 0, 255, 255));

        // Painting lands in the mask while the mask is the selected target.
        CHECK(doc.activeLayer() == top);
        top->setMaskSelected(true);
        CHECK(doc.activeLayer() == mask);
        CHECK(doc.activeNode() == top);
        CHECK(doc.layerByUid(mask->uid()) == mask);

        // Applying the mask bakes it into the pixels and drops it.
        CHECK(doc.applyMask(top));
        CHECK(!top->hasMask());
        CHECK(top->image.at(1, 1).a == 255);
        CHECK(top->image.at(5, 1).a == 0);
        CHECK(doc.activeLayer() == top);

        // And a mask can simply be deleted, which reveals the layer again.
        doc.addMask(top, false);
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(0, 0, 255, 255));
        doc.removeMask(top);
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(255, 0, 0, 255));
    });

    addTest("layer groups", [] {
        Document doc(8, 8, Rgba(0, 0, 255, 255));
        Layer* group = doc.addGroup(QStringLiteral("Set"));
        CHECK(group->isGroup());
        CHECK(doc.layerCount() == 2);
        CHECK(doc.activeNode() == group);
        // A group is not a paint target, so there is nothing to paint into.
        CHECK(doc.activeLayer() == group);
        CHECK(group->width() == 0);

        // A layer added while a group is active lands inside it.
        Layer* inside = doc.addEmptyLayer();
        CHECK(inside->parent() == group);
        CHECK(group->childCount() == 1);
        inside->image.fill(Rgba(255, 0, 0, 255));
        doc.invalidate();
        CHECK(doc.composite().at(0, 0) == Rgba(255, 0, 0, 255));

        // The flat index runs bottom to top with children ahead of the group.
        CHECK(doc.layerAt(0)->name == QStringLiteral("Background"));
        CHECK(doc.layerAt(1) == inside);
        CHECK(doc.layerAt(2) == group);
        CHECK(doc.indexOfUid(group->uid()) == 2);

        // The group's own opacity, visibility and mode apply to the children.
        group->opacity = 128;
        doc.invalidate();
        CHECK_NEAR(doc.composite().at(0, 0).r, 127, 2);
        CHECK_NEAR(doc.composite().at(0, 0).b, 127, 2);
        group->opacity = 255;
        group->visible = false;
        doc.invalidate();
        CHECK(doc.composite().at(0, 0) == Rgba(0, 0, 255, 255));
        group->visible = true;
        group->mode = BlendMode::Multiply;
        doc.invalidate();
        CHECK(doc.composite().at(0, 0) == Rgba(0, 0, 0, 255));
        group->mode = BlendMode::Normal;
        doc.invalidate();

        // A group can carry a mask of its own.
        Layer* groupMask = doc.addMask(group);
        ops::fillRegion(groupMask->image, Rect(0, 0, 4, 8), Rgba(0, 0, 0, 255));
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(0, 0, 255, 255));
        CHECK(doc.composite().at(5, 1) == Rgba(255, 0, 0, 255));
        doc.removeMask(group);
        doc.invalidate();

        // Dragging a layer out of a group and back in again.
        CHECK(doc.reparentLayer(inside, doc.rootGroup(), 0));
        CHECK(inside->parent() == doc.rootGroup());
        CHECK(group->childCount() == 0);
        CHECK(doc.layerAt(0) == inside);
        CHECK(doc.reparentLayer(inside, group, 0));
        CHECK(inside->parent() == group);
        // Nothing can be dragged into itself, or into a plain layer.
        CHECK(!doc.reparentLayer(group, group, 0));
        CHECK(!doc.canReparent(group, inside));

        // Duplicating a group copies its contents with fresh identities.
        Layer* copy = doc.duplicateLayer(doc.indexOfUid(group->uid()));
        CHECK(copy != nullptr && copy->isGroup());
        CHECK(copy->childCount() == 1);
        CHECK(copy->childAt(0)->uid() != inside->uid());
        CHECK(doc.layerByUid(copy->childAt(0)->uid()) == copy->childAt(0));
        CHECK(copy->name != group->name);
        // Deleting a group takes its children with it.
        CHECK(doc.removeLayer(doc.indexOfUid(copy->uid())));
        CHECK(doc.indexOfUid(copy->uid()) < 0);
        CHECK(doc.layerCount() == 3);

        // Move up and down step over a group rather than into it.
        const int backgroundUid = doc.layerAt(0)->uid();
        doc.moveLayer(0, 1);
        CHECK(doc.indexOfUid(backgroundUid) == 2);
        CHECK(doc.layerByUid(backgroundUid)->parent() == doc.rootGroup());
        doc.moveLayer(2, 1);
        CHECK(doc.indexOfUid(backgroundUid) == 0);

        // Merging a group down flattens it into the layer below.
        CHECK(doc.mergeLayerDown(doc.indexOfUid(group->uid())));
        CHECK(doc.layerCount() == 1);
        CHECK(!doc.layerAt(0)->isGroup());
        CHECK(doc.layerAt(0)->image.at(0, 0) == Rgba(255, 0, 0, 255));
    });

    addTest("canvas operations follow masks and groups", [] {
        Document doc(10, 20, Rgba(0, 0, 255, 255));
        Layer* group = doc.addGroup(QStringLiteral("Set"));
        Layer* inside = doc.addChild(
            group, std::make_unique<Layer>(10, 20, QStringLiteral("Inside"),
                                           Rgba(255, 0, 0, 255)));
        Layer* mask = doc.addMask(inside);
        ops::fillRegion(mask->image, Rect(0, 0, 10, 10), Rgba(0, 0, 0, 255));
        doc.invalidate();
        CHECK(doc.composite().at(5, 5) == Rgba(0, 0, 255, 255));

        doc.rotate90(1);
        CHECK(doc.width() == 20 && doc.height() == 10);
        CHECK(inside->maskImage()->width() == 20);
        CHECK(doc.composite().width() == 20);
        doc.resizeCanvas(40, 30, Anchor::TopLeft, false);
        CHECK(inside->maskImage()->width() == 40);
        // Fresh canvas reveals, so the mask never hides what was not there.
        CHECK(inside->maskImage()->at(39, 29) == Rgba(255, 255, 255, 255));
        doc.cropTo(Rect(0, 0, 8, 8));
        CHECK(inside->maskImage()->width() == 8);
        doc.flip(true);
        CHECK(inside->maskImage()->width() == 8);
        CHECK(doc.composite().width() == 8);
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

    addTest("masks and groups survive undo", [] {
        Document doc(8, 8, Rgba(0, 0, 255, 255));
        History history(&doc);
        recordDocumentEdit(doc, history, QStringLiteral("Add Group"),
                           QStringLiteral("layer"), [&] {
                               Layer* group = doc.addGroup(QStringLiteral("Set"));
                               doc.addChild(group, std::make_unique<Layer>(
                                                       8, 8, QStringLiteral("Inside"),
                                                       Rgba(255, 0, 0, 255)));
                           });
        CHECK(doc.layerCount() == 3);
        const int groupUid = doc.layerAt(2)->uid();
        CHECK(doc.composite().at(1, 1) == Rgba(255, 0, 0, 255));

        recordDocumentEdit(doc, history, QStringLiteral("Add Mask"),
                           QStringLiteral("layer"), [&] {
                               Layer* inside = doc.layerAt(1);
                               Layer* mask = doc.addMask(inside);
                               ops::fillRegion(mask->image, Rect(0, 0, 4, 8),
                                               Rgba(0, 0, 0, 255));
                               inside->setMaskSelected(true);
                           });
        CHECK(doc.composite().at(1, 1) == Rgba(0, 0, 255, 255));

        CHECK(history.undo());
        CHECK(!doc.layerAt(1)->hasMask());
        CHECK(doc.composite().at(1, 1) == Rgba(255, 0, 0, 255));
        CHECK(history.undo());
        CHECK(doc.layerCount() == 1);

        CHECK(history.redo());
        CHECK(doc.layerCount() == 3);
        CHECK(doc.layerAt(2)->isGroup());
        CHECK(doc.layerAt(2)->uid() == groupUid);
        CHECK(doc.layerAt(1)->parent() == doc.layerAt(2));
        CHECK(doc.layerAt(1)->name == QStringLiteral("Inside"));
        CHECK(history.redo());
        CHECK(doc.layerAt(1)->hasMask());
        CHECK(doc.layerAt(1)->maskSelected());
        CHECK(doc.activeLayer() == doc.layerAt(1)->maskLayer());
        CHECK(doc.layerAt(1)->maskImage()->at(1, 1).r == 0);
        CHECK(doc.composite().at(1, 1) == Rgba(0, 0, 255, 255));

        // A mask is an ordinary raster, so painting in it records pixel undo.
        Layer* mask = doc.activeLayer();
        {
            EditScope scope(doc, history, mask, QStringLiteral("Paintbrush"),
                            QStringLiteral("brush"));
            ops::fillRegion(mask->image, Rect(0, 0, 4, 8), Rgba(255, 255, 255, 255));
        }
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(255, 0, 0, 255));
        CHECK(history.undo());
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(0, 0, 255, 255));
    });

    addTest("masks and groups round trip files", [] {
        QTemporaryDir dir;
        Document doc(16, 12, Rgba(0, 0, 255, 255));
        Layer* group = doc.addGroup(QStringLiteral("Set"));
        group->opacity = 200;
        group->mode = BlendMode::Multiply;
        Layer* inside = doc.addChild(
            group, std::make_unique<Layer>(16, 12, QStringLiteral("Inside"),
                                           Rgba(255, 0, 0, 255)));
        Layer* mask = doc.addMask(inside);
        ops::fillRegion(mask->image, Rect(0, 0, 8, 12), Rgba(0, 0, 0, 255));
        inside->setMaskSelected(true);
        // Set after the child, which opens the group it lands in.
        group->expanded = false;
        doc.invalidate();
        const Image expected = doc.composite();
        QString error;

        const QString path = dir.filePath(QStringLiteral("tree.tessel"));
        CHECK(io::saveDocument(doc, path, &error));
        auto loaded = io::openDocument(path, &error);
        CHECK(loaded != nullptr);
        if (loaded) {
            CHECK(loaded->layerCount() == 3);
            const Layer* loadedGroup = loaded->layerAt(2);
            CHECK(loadedGroup->isGroup());
            CHECK(loadedGroup->name == QStringLiteral("Set"));
            CHECK(loadedGroup->opacity == 200);
            CHECK(loadedGroup->mode == BlendMode::Multiply);
            CHECK(!loadedGroup->expanded);
            CHECK(loadedGroup->childCount() == 1);
            const Layer* loadedInside = loaded->layerAt(1);
            CHECK(loadedInside->parent() == loadedGroup);
            CHECK(loadedInside->hasMask());
            CHECK(loadedInside->maskSelected());
            CHECK(loadedInside->maskImage()->at(1, 1).r == 0);
            CHECK(loadedInside->maskImage()->at(12, 1).r == 255);
            CHECK(samePixels(loaded->composite(), expected));
        }

        // OpenRaster keeps a group as a nested stack, and bakes the mask into
        // the pixels it writes because the format has no mask of its own.
        const QString oraPath = dir.filePath(QStringLiteral("tree.ora"));
        CHECK(io::saveDocument(doc, oraPath, &error));
        auto ora = io::openDocument(oraPath, &error);
        CHECK(ora != nullptr);
        if (ora) {
            CHECK(ora->layerCount() == 3);
            CHECK(ora->layerAt(2)->isGroup());
            CHECK(ora->layerAt(2)->name == QStringLiteral("Set"));
            CHECK(ora->layerAt(2)->opacity == 200);
            CHECK(ora->layerAt(2)->mode == BlendMode::Multiply);
            CHECK(ora->layerAt(1)->parent() == ora->layerAt(2));
            CHECK(!ora->layerAt(1)->hasMask());
            CHECK(samePixels(ora->composite(), expected));
        }
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

    // ------------------------------------------------------------ units
    addTest("unit conversions round trip", [] {
        for (double dpi : {72.0, 96.0, 150.0, 300.0, 600.0}) {
            for (int pixels : {1, 37, 800, 1234, 6000}) {
                const double inches = units::fromPixels(pixels, Unit::Inches, dpi);
                const double centimetres =
                    units::fromPixels(pixels, Unit::Centimetres, dpi);
                CHECK_NEAR(centimetres, inches * units::kCentimetresPerInch, 1e-9);
                CHECK_NEAR(units::toPixels(inches, Unit::Inches, dpi), pixels, 1e-9);
                CHECK_NEAR(units::toPixels(centimetres, Unit::Centimetres, dpi),
                           pixels, 1e-9);
                // pixels -> inches -> centimetres -> pixels
                const double backAgain = units::toPixels(
                    units::fromPixels(units::toPixels(inches, Unit::Inches, dpi),
                                      Unit::Centimetres, dpi),
                    Unit::Centimetres, dpi);
                CHECK_NEAR(backAgain, pixels, 1e-9);
            }
            // One inch is exactly the resolution in pixels.
            CHECK_NEAR(units::fromPixels(dpi, Unit::Inches, dpi), 1.0, 1e-12);
            CHECK_NEAR(units::resolutionFromDotsPerMetre(units::dotsPerMetre(dpi)),
                       dpi, 1e-9);
        }
        // Percent measures against the reference pixel count it is handed.
        CHECK_NEAR(units::fromPixels(400, Unit::Percent, 96.0, 800), 50.0, 1e-9);
        CHECK_NEAR(units::toPixels(50.0, Unit::Percent, 96.0, 800), 400.0, 1e-9);
        // Pixels pass straight through whatever the resolution.
        CHECK_NEAR(units::toPixels(42.0, Unit::Pixels, 300.0), 42.0, 1e-12);
        CHECK_NEAR(units::fromPixels(42.0, Unit::Pixels, 300.0), 42.0, 1e-12);
        // Nonsense resolutions fall back to the default rather than producing
        // infinite print sizes.
        CHECK_NEAR(units::sanitize(300.0), 300.0, 1e-12);
        CHECK_NEAR(units::sanitize(0.0), units::kDefaultResolution, 1e-12);
        CHECK_NEAR(units::sanitize(-5.0), units::kDefaultResolution, 1e-12);
        CHECK_NEAR(units::sanitize(1.0e9), units::kDefaultResolution, 1e-12);
        CHECK_NEAR(units::sanitize(std::nan("")), units::kDefaultResolution, 1e-12);
        // The settings keys survive a round trip of their own.
        for (Unit unit : {Unit::Pixels, Unit::Inches, Unit::Centimetres, Unit::Percent})
            CHECK(units::unitFromId(units::unitId(unit)) == unit);
        CHECK(units::unitFromId(QStringLiteral("centimeters")) == Unit::Centimetres);
        CHECK(units::unitFromId(QStringLiteral("nonsense")) == Unit::Pixels);
        CHECK(units::measurableNames().size() == 3);
        // Readouts drop the trailing zeros a fixed precision would leave.
        CHECK(units::describeSize(2550, 3300, 300.0, Unit::Inches)
              == QStringLiteral("8.5 x 11 in"));
        CHECK(units::describeSize(2550, 3300, 300.0, Unit::Centimetres)
              == QStringLiteral("21.59 x 27.94 cm"));
        CHECK(units::describeSize(96, 48, 96.0, Unit::Pixels)
              == QStringLiteral("96 x 48 px"));
        // Percent cannot label a size, so it borrows pixels.
        CHECK(units::describeSize(96, 48, 96.0, Unit::Percent)
              == QStringLiteral("96 x 48 px"));
        CHECK(units::describePrintSize(2550, 3300, 300.0, Unit::Pixels)
              == QStringLiteral("8.5 x 11 in at 300 ppi"));
    });

    addTest("ruler tick spacing", [] {
        // Pixels climb the 1 / 5 / 10 ladder until the labels fit.
        CHECK_NEAR(units::rulerTicks(Unit::Pixels, 96.0, 1.0).major, 100.0, 1e-9);
        CHECK_NEAR(units::rulerTicks(Unit::Pixels, 96.0, 8.0).major, 10.0, 1e-9);
        CHECK_NEAR(units::rulerTicks(Unit::Pixels, 96.0, 0.1).major, 1000.0, 1e-9);
        // Inches step in inches, not in round pixel counts.
        const units::RulerTicks inch = units::rulerTicks(Unit::Inches, 96.0, 1.0);
        CHECK_NEAR(inch.major, 96.0, 1e-9);
        CHECK(inch.minorPerMajor == 4);
        // Zoomed in, a quarter inch is wide enough to label.
        CHECK_NEAR(units::rulerTicks(Unit::Inches, 96.0, 4.0).major, 24.0, 1e-9);
        // At 300 dpi an inch is 300 pixels, so half an inch already labels
        // comfortably even at half zoom.
        CHECK_NEAR(units::rulerTicks(Unit::Inches, 300.0, 0.5).major, 150.0, 1e-9);
        CHECK_NEAR(units::rulerTicks(Unit::Inches, 300.0, 0.1).major, 600.0, 1e-9);
        // Centimetres step in whole or half centimetres.
        const units::RulerTicks cm = units::rulerTicks(Unit::Centimetres, 96.0, 1.0);
        CHECK_NEAR(cm.major, units::toPixels(2.0, Unit::Centimetres, 96.0), 1e-9);
        CHECK_NEAR(units::rulerTicks(Unit::Centimetres, 96.0, 4.0).major,
                   units::toPixels(0.5, Unit::Centimetres, 96.0), 1e-9);
        // A degenerate zoom must not hang or divide by zero.
        CHECK(units::rulerTicks(Unit::Inches, 96.0, 0.0).major > 0.0);
        CHECK(units::rulerTicks(Unit::Pixels, 96.0, -1.0).major > 0.0);
        // Labels read in the chosen unit.
        CHECK(units::rulerLabel(300.0, Unit::Pixels, 96.0) == QStringLiteral("300"));
        CHECK(units::rulerLabel(96.0, Unit::Inches, 96.0) == QStringLiteral("1"));
        CHECK(units::rulerLabel(48.0, Unit::Inches, 96.0) == QStringLiteral("0.5"));
        CHECK(units::rulerLabel(units::toPixels(2.0, Unit::Centimetres, 96.0),
                                Unit::Centimetres, 96.0) == QStringLiteral("2"));
    });

    addTest("resolution survives document operations", [] {
        Document doc(40, 30);
        CHECK_NEAR(doc.resolution(), units::kDefaultResolution, 1e-12);
        doc.setResolution(240.0);
        CHECK_NEAR(doc.resolution(), 240.0, 1e-12);
        const DocumentState saved = doc.snapshot();
        CHECK_NEAR(saved.resolution, 240.0, 1e-12);

        doc.resizeCanvas(80, 60, Anchor::Center, true);
        CHECK_NEAR(doc.resolution(), 240.0, 1e-12);
        doc.cropTo(Rect(5, 5, 20, 20));
        CHECK_NEAR(doc.resolution(), 240.0, 1e-12);
        doc.rotate90(1);
        CHECK_NEAR(doc.resolution(), 240.0, 1e-12);
        doc.flip(true);
        CHECK_NEAR(doc.resolution(), 240.0, 1e-12);

        // Snapshot and restore stay in step, so a resolution change undoes.
        doc.setResolution(72.0);
        CHECK_NEAR(doc.snapshot().resolution, 72.0, 1e-12);
        doc.restore(saved);
        CHECK_NEAR(doc.resolution(), 240.0, 1e-12);
        CHECK(doc.width() == 40 && doc.height() == 30);
        // A state carrying no resolution restores the default.
        DocumentState bare = doc.snapshot();
        bare.resolution = 0.0;
        doc.restore(bare);
        CHECK_NEAR(doc.resolution(), units::kDefaultResolution, 1e-12);
        // Out of range values never reach the document.
        doc.setResolution(-10.0);
        CHECK_NEAR(doc.resolution(), units::kDefaultResolution, 1e-12);
    });

    addTest("resolution survives file round trips", [] {
        QTemporaryDir dir;
        Document doc(32, 24);
        doc.setResolution(300.0);
        ops::fillRegion(doc.layerAt(0)->image, Rect(0, 0, 32, 24),
                        Rgba(40, 50, 60, 255));
        doc.invalidate();

        // The layered containers store it exactly; PNG and JPEG store it as a
        // whole number of dots per metre, so allow the rounding.
        for (const char* extension : {"tessel", "ora", "png", "jpg", "tiff"}) {
            const QString path = dir.filePath(QStringLiteral("res.%1").arg(extension));
            if (!io::canWrite(path))
                continue;
            QString error;
            CHECK(io::saveDocument(doc, path, &error, 95));
            auto loaded = io::openDocument(path, &error);
            CHECK(loaded != nullptr);
            if (!loaded)
                continue;
            CHECK_NEAR(loaded->resolution(), 300.0, 0.01);
        }

        // A .tessel manifest written before resolutions existed must open at
        // the default rather than at zero.
        {
            const QString source = dir.filePath(QStringLiteral("res.tessel"));
            const QString legacy = dir.filePath(QStringLiteral("legacy.tessel"));
            ZipReader reader(source);
            CHECK(reader.isValid());
            ZipWriter writer(legacy);
            CHECK(writer.isOpen());
            for (const QString& name : reader.names()) {
                QByteArray data = reader.read(name);
                if (name == QStringLiteral("document.json")) {
                    QJsonObject root = QJsonDocument::fromJson(data).object();
                    root.remove(QStringLiteral("resolution"));
                    data = QJsonDocument(root).toJson();
                }
                writer.add(name, data, name != QStringLiteral("mimetype"));
            }
            CHECK(writer.close());
            QString error;
            auto loaded = io::openDocument(legacy, &error);
            CHECK(loaded != nullptr);
            if (loaded)
                CHECK_NEAR(loaded->resolution(), units::kDefaultResolution, 1e-9);
        }

        // The same for an OpenRaster image whose image element has no xres.
        {
            const QString source = dir.filePath(QStringLiteral("res.ora"));
            const QString legacy = dir.filePath(QStringLiteral("legacy.ora"));
            ZipReader reader(source);
            CHECK(reader.isValid());
            CHECK(reader.read(QStringLiteral("stack.xml"))
                      .contains(QByteArray("xres=\"300\"")));
            ZipWriter writer(legacy);
            CHECK(writer.isOpen());
            for (const QString& name : reader.names()) {
                QByteArray data = reader.read(name);
                if (name == QStringLiteral("stack.xml")) {
                    data.replace(QByteArray(" xres=\"300\""), QByteArray());
                    data.replace(QByteArray(" yres=\"300\""), QByteArray());
                }
                writer.add(name, data, name != QStringLiteral("mimetype"));
            }
            CHECK(writer.close());
            QString error;
            auto loaded = io::openDocument(legacy, &error);
            CHECK(loaded != nullptr);
            if (loaded)
                CHECK_NEAR(loaded->resolution(), units::kDefaultResolution, 1e-9);
        }
    });

    // ------------------------------------------------------------ text items
    // Stands in for the text tool, which lives in the interface library: lay
    // "glyphs" into a layer and keep what was under them, the way a committed
    // piece of text does.
    auto layTextItem = [](Document& doc, const Rect& box) {
        Layer* layer = doc.activeLayer();
        TextItem item;
        item.layerUid = layer->uid();
        item.lines = QStringList{QStringLiteral("Hello"), QStringLiteral("world")};
        item.x = box.x;
        item.y = box.y;
        item.family = QStringLiteral("Sans Serif");
        item.pointSize = 18.0;
        item.italic = true;
        item.align = 2;
        item.color = Rgba(10, 20, 30, 255);
        item.opacity = 0.75;
        item.antialias = false;
        item.mode = BlendMode::Multiply;
        item.box = box;
        item.backdrop = layer->image.cropped(box);
        for (int x = box.x + 1; x < box.right() - 1; ++x)
            layer->image.setAt(x, box.y + 2, Rgba(10, 20, 30, 255));
        item.fingerprint = textFingerprint(layer->image, box);
        doc.invalidate();
        return item;
    };

    addTest("text items go stale when their pixels change", [&layTextItem] {
        Document doc(60, 40);
        const Rect box(5, 5, 20, 10);
        const TextItem item = layTextItem(doc, box);
        CHECK(item.fingerprint != 0);
        CHECK(textItemLive(doc, item));

        // An edit outside the box leaves the item alone.
        const Rgba keep = doc.layerAt(0)->image.at(40, 30);
        doc.layerAt(0)->image.setAt(40, 30, Rgba(1, 2, 3, 255));
        CHECK(textItemLive(doc, item));
        doc.layerAt(0)->image.setAt(40, 30, keep);

        // One pixel inside it is enough to drop the item, and putting that
        // pixel back (an undo) makes the item live again.
        const Rgba under = doc.layerAt(0)->image.at(box.x + 3, box.y + 7);
        doc.layerAt(0)->image.setAt(box.x + 3, box.y + 7, Rgba(9, 9, 9, 255));
        CHECK(!textItemLive(doc, item));
        doc.layerAt(0)->image.setAt(box.x + 3, box.y + 7, under);
        CHECK(textItemLive(doc, item));

        // A box that hangs off the layer has no fingerprint at all.
        CHECK(textFingerprint(doc.layerAt(0)->image, Rect(55, 35, 20, 20)) == 0);
        CHECK(textFingerprint(doc.layerAt(0)->image, Rect()) == 0);
        TextItem off = item;
        off.box = Rect(55, 35, 20, 20);
        CHECK(!textItemLive(doc, off));

        // An item naming a layer that is gone is stale, whatever the pixels.
        TextItem orphan = item;
        orphan.layerUid = item.layerUid + 1000;
        CHECK(!textItemLive(doc, orphan));
    });

    addTest("text items survive the native round trip", [&layTextItem] {
        QTemporaryDir dir;
        Document doc(60, 40);
        doc.addEmptyLayer();
        doc.setActiveIndex(1);
        ops::fillRegion(doc.activeLayer()->image, Rect(0, 0, 60, 40),
                        Rgba(200, 200, 200, 255));
        const Rect box(5, 5, 20, 10);
        const TextItem item = layTextItem(doc, box);
        doc.textItems().push_back(item);
        const Image pixels = doc.activeLayer()->image;

        const QString path = dir.filePath(QStringLiteral("text.tessel"));
        QString error;
        CHECK(io::saveDocument(doc, path, &error));
        auto loaded = io::openDocument(path, &error);
        CHECK(loaded != nullptr);
        if (!loaded)
            return;
        // The pixels in the layer are exactly what they were, so a reader that
        // knows nothing about text items sees the same picture.
        CHECK(samePixels(loaded->layerAt(1)->image, pixels));
        CHECK(loaded->textItems().size() == 1);
        if (loaded->textItems().empty())
            return;
        const TextItem& back = loaded->textItems().front();
        CHECK(back.layerUid == loaded->layerAt(1)->uid());
        CHECK(back.lines == item.lines);
        CHECK_NEAR(back.x, item.x, 1e-9);
        CHECK_NEAR(back.y, item.y, 1e-9);
        CHECK(back.family == item.family);
        CHECK_NEAR(back.pointSize, item.pointSize, 1e-9);
        CHECK(back.bold == item.bold && back.italic == item.italic);
        CHECK(back.underline == item.underline);
        CHECK(back.align == item.align);
        CHECK(back.color == item.color);
        CHECK_NEAR(back.opacity, item.opacity, 1e-6);
        CHECK(back.antialias == item.antialias);
        CHECK(back.mode == item.mode);
        CHECK(back.box.x == box.x && back.box.y == box.y);
        CHECK(back.box.w == box.w && back.box.h == box.h);
        CHECK(back.fingerprint == item.fingerprint);
        CHECK(samePixels(back.backdrop, item.backdrop));
        // Which is the point of all of it: the text is still editable.
        CHECK(textItemLive(*loaded, back));

        // The items ride beside their own layer in the manifest.
        ZipReader zip(path);
        CHECK(zip.isValid());
        const QJsonObject root =
            QJsonDocument::fromJson(zip.read(QStringLiteral("document.json"))).object();
        const QJsonArray layers = root.value(QStringLiteral("layers")).toArray();
        CHECK(layers.size() == 2);
        CHECK(!layers.at(0).toObject().contains(QStringLiteral("textItems")));
        CHECK(layers.at(1).toObject().value(QStringLiteral("textItems")).toArray().size()
              == 1);
    });

    addTest("a file carries groups, masks and editable text together", [&layTextItem] {
        // The tree, the masks and the text items all live in document.json and
        // were written by different hands, so the one case none of their own
        // tests covers is all three in the same file.
        QTemporaryDir dir;
        Document doc(60, 40);
        Layer* group = doc.addGroup(QStringLiteral("Folder"));
        CHECK(group != nullptr);
        Layer* inside = doc.addChild(group, std::make_unique<Layer>(
            Image(60, 40), QStringLiteral("Inside")));
        CHECK(inside != nullptr);
        ops::fillRegion(inside->image, Rect(0, 0, 60, 40), Rgba(200, 200, 200, 255));
        Image mask(60, 40);
        ops::fillRegion(mask, Rect(0, 0, 60, 40), Rgba(255, 255, 255, 255));
        ops::fillRegion(mask, Rect(0, 0, 30, 40), Rgba(0, 0, 0, 255));
        CHECK(doc.setMask(inside, mask) != nullptr);
        doc.setActiveIndex(doc.indexOfUid(inside->uid()));
        const TextItem item = layTextItem(doc, Rect(5, 5, 20, 10));
        doc.textItems().push_back(item);
        const Image pixels = inside->image;

        const QString path = dir.filePath(QStringLiteral("all.tessel"));
        QString error;
        CHECK(io::saveDocument(doc, path, &error));
        auto loaded = io::openDocument(path, &error);
        CHECK(loaded != nullptr);
        if (!loaded)
            return;
        const Layer* backGroup = nullptr;
        for (int i = 0; i < loaded->rootGroup()->childCount(); ++i) {
            if (loaded->rootGroup()->childAt(i)->isGroup())
                backGroup = loaded->rootGroup()->childAt(i);
        }
        CHECK(backGroup != nullptr && backGroup->isGroup());
        if (!backGroup || !backGroup->isGroup())
            return;
        const Layer* backInside = backGroup->childAt(0);
        CHECK(backInside != nullptr);
        if (!backInside)
            return;
        CHECK(backInside->name == QLatin1String("Inside"));
        CHECK(backInside->hasMask());
        CHECK(samePixels(backInside->image, pixels));
        // And the text that was typed on the layer inside the group came back
        // attached to that same layer, not to the one at the top.
        CHECK(loaded->textItems().size() == 1);
        if (loaded->textItems().empty())
            return;
        CHECK(loaded->textItems().front().layerUid == backInside->uid());
        CHECK(loaded->textItems().front().lines == item.lines);
    });

    addTest("a manifest with no text items still opens", [&layTextItem] {
        QTemporaryDir dir;
        Document doc(60, 40);
        const Rect box(5, 5, 20, 10);
        doc.textItems().push_back(layTextItem(doc, box));
        const QString source = dir.filePath(QStringLiteral("text.tessel"));
        QString error;
        CHECK(io::saveDocument(doc, source, &error));

        // An older file simply has no key, and the picture must be unaffected.
        const QString legacy = dir.filePath(QStringLiteral("legacy.tessel"));
        ZipReader reader(source);
        CHECK(reader.isValid());
        ZipWriter writer(legacy);
        CHECK(writer.isOpen());
        for (const QString& name : reader.names()) {
            QByteArray data = reader.read(name);
            if (name == QStringLiteral("document.json")) {
                QJsonObject root = QJsonDocument::fromJson(data).object();
                QJsonArray layers = root.value(QStringLiteral("layers")).toArray();
                QJsonObject entry = layers.at(0).toObject();
                entry.remove(QStringLiteral("textItems"));
                layers.replace(0, entry);
                root[QStringLiteral("layers")] = layers;
                data = QJsonDocument(root).toJson();
            }
            writer.add(name, data, name != QStringLiteral("mimetype"));
        }
        CHECK(writer.close());
        auto loaded = io::openDocument(legacy, &error);
        CHECK(loaded != nullptr);
        if (!loaded)
            return;
        CHECK(loaded->textItems().empty());
        CHECK(samePixels(loaded->layerAt(0)->image, doc.layerAt(0)->image));
    });

    addTest("stale text items are not saved", [&layTextItem] {
        QTemporaryDir dir;
        Document doc(60, 40);
        const Rect box(5, 5, 20, 10);
        doc.textItems().push_back(layTextItem(doc, box));
        // Paint over the text: it is plain pixels now and must not come back
        // as something editable after a round trip.
        doc.layerAt(0)->image.setAt(box.x + 2, box.y + 2, Rgba(7, 7, 7, 255));
        const QString path = dir.filePath(QStringLiteral("stale.tessel"));
        QString error;
        CHECK(io::saveDocument(doc, path, &error));
        auto loaded = io::openDocument(path, &error);
        CHECK(loaded != nullptr);
        if (loaded)
            CHECK(loaded->textItems().empty());
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

    addTest("the progress channel does not change the pixels", [] {
        registerBuiltinEffects();
        const Image source = gradient(48, 40);
        for (const char* id : kProgressEffects) {
            Effect* effect = EffectRegistry::instance().byId(QString::fromLatin1(id));
            check(effect != nullptr, id, __LINE__);
            if (!effect)
                continue;
            const ParamValues values = ParamValues::defaults(effect->params());
            EffectContext plain;
            plain.source = &source;
            plain.region = source.rect();
            const Image expected = effect->apply(plain, values);

            EffectProgress progress;
            EffectContext watched = plain;
            watched.progress = &progress;
            const Image actual = effect->apply(watched, values);
            check(samePixels(expected, actual), id, __LINE__);
            // Every row was reported, so the bar reaches the end.
            check(progress.fraction() > 0.99, id, __LINE__);
            check(!progress.isCancelled(), id, __LINE__);
        }
    });

    addTest("a cancelled effect gives up without writing", [] {
        registerBuiltinEffects();
        const Image source = gradient(96, 96);
        for (const char* id : kProgressEffects) {
            Effect* effect = EffectRegistry::instance().byId(QString::fromLatin1(id));
            if (!effect)
                continue;
            const ParamValues values = ParamValues::defaults(effect->params());
            EffectProgress progress;
            progress.cancel();
            EffectContext context;
            context.source = &source;
            context.region = source.rect();
            context.progress = &progress;
            const Image out = effect->apply(context, values);
            // Not one row was touched, so the output is still the copy of the
            // source that the effect started from.
            check(samePixels(out, source), id, __LINE__);

            // Sanity check that the same effect does change the image when it
            // is left alone, otherwise the comparison above proves nothing.
            EffectContext plain;
            plain.source = &source;
            plain.region = source.rect();
            check(!samePixels(effect->apply(plain, values), source), id, __LINE__);
        }
    });

    addTest("effect parameter defaults", [] {
        std::vector<ParamSpec> specs = {
            ParamSpec::intParam(QStringLiteral("i"), "I", 0, 10, 7),
            ParamSpec::boolParam(QStringLiteral("b"), "B", true),
            ParamSpec::doubleParam(QStringLiteral("d"), "D", 0, 1, 0.25),
            ParamSpec::colorParam(QStringLiteral("c"), "C", Rgba(1, 2, 3, 255)),
            ParamSpec::offsetParam(QStringLiteral("o"), "O", 0.5, -0.5),
            ParamSpec::choiceParam(QStringLiteral("ch"), "Ch",
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

    addTest("parameter values round trip through json", [] {
        QPolygonF curve;
        curve << QPointF(0, 0) << QPointF(96, 140) << QPointF(255, 255);
        ParamValues values;
        values.set(QStringLiteral("count"), 7);
        values.set(QStringLiteral("amount"), 0.125);
        values.set(QStringLiteral("on"), true);
        values.set(QStringLiteral("tint"), Rgba(10, 20, 30, 40).toQColor());
        values.set(QStringLiteral("centre"), QPointF(-0.25, 0.5));
        values.set(QStringLiteral("curve"), QVariant::fromValue(curve));
        const ParamValues back = ParamValues::fromJson(values.toJson());
        CHECK(back == values);
        CHECK(back.hash() == values.hash());
        CHECK(back.intValue(QStringLiteral("count")) == 7);
        CHECK(back.colorValue(QStringLiteral("tint")) == Rgba(10, 20, 30, 40));
        CHECK(back.pointValue(QStringLiteral("centre")) == QPointF(-0.25, 0.5));
        CHECK(back.raw(QStringLiteral("curve")).value<QPolygonF>() == curve);
        // A different value has to be a different digest, or a cached effect
        // result would be handed back for controls that have moved.
        ParamValues moved = values;
        moved.set(QStringLiteral("count"), 8);
        CHECK(moved.hash() != values.hash());
    });

    // ------------------------------------------- adjustment layers and stacks
    addTest("an adjustment layer applies to what is below it", [] {
        registerBuiltinEffects();
        Document doc(8, 8, Rgba(10, 20, 30, 255));
        Layer* adjustment = doc.addAdjustmentLayer(QStringLiteral("invert_colors"),
                                                   ParamValues(),
                                                   QStringLiteral("Invert"));
        CHECK(adjustment != nullptr);
        if (!adjustment)
            return;
        CHECK(adjustment->isAdjustment());
        CHECK(doc.layerCount() == 2);
        CHECK(doc.composite().at(1, 1) == Rgba(245, 235, 225, 255));
        // It takes part in the tree like any other layer.
        adjustment->visible = false;
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(10, 20, 30, 255));
        adjustment->visible = true;
        adjustment->opacity = 128;
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(128, 128, 128, 255));
        adjustment->opacity = 255;
        Layer* mask = doc.addMask(adjustment);
        ops::fillRegion(mask->image, Rect(0, 0, 4, 8), Rgba(0, 0, 0, 255));
        doc.invalidate();
        CHECK(doc.composite().at(1, 1) == Rgba(10, 20, 30, 255));
        CHECK(doc.composite().at(5, 1) == Rgba(245, 235, 225, 255));
        // And it has nothing of its own to draw.
        CHECK(doc.renderLayer(*adjustment).at(1, 1).a == 0);
    });

    addTest("an adjustment layer stops at its own group", [] {
        registerBuiltinEffects();
        Document doc(8, 8, Rgba(10, 20, 30, 255));
        Layer* group = doc.addGroup(QStringLiteral("Set"));
        Layer* inside = doc.addChild(
            group, std::make_unique<Layer>(8, 8, QStringLiteral("Inside")));
        ops::fillRegion(inside->image, Rect(0, 0, 4, 8), Rgba(100, 100, 100, 255));
        doc.setActiveIndex(doc.indexOfUid(inside->uid()));
        Layer* adjustment = doc.addAdjustmentLayer(QStringLiteral("invert_colors"),
                                                   ParamValues());
        CHECK(adjustment != nullptr && adjustment->parent() == group);
        doc.invalidate();
        // The group's own pixels are inverted; the background below the group
        // is not, because the adjustment never leaves the group.
        CHECK(doc.composite().at(1, 1) == Rgba(155, 155, 155, 255));
        CHECK(doc.composite().at(6, 1) == Rgba(10, 20, 30, 255));
    });

    addTest("a layer effect stack leaves the stored pixels alone", [] {
        registerBuiltinEffects();
        Document doc(8, 8, Rgba(10, 20, 30, 255));
        Layer* layer = doc.layerAt(0);
        ParamValues darker;
        darker.set(QStringLiteral("brightness"), -4);
        darker.set(QStringLiteral("contrast"), 0);
        doc.setLayerEffects(layer, {{QStringLiteral("invert_colors"), ParamValues(), true},
                                    {QStringLiteral("brightness_contrast"), darker, true}});
        // Invert first, then darken: 10 becomes 245 and then 235.
        CHECK(doc.composite().at(1, 1) == Rgba(235, 225, 215, 255));
        CHECK(layer->image.at(1, 1) == Rgba(10, 20, 30, 255));

        // The order of the entries is the order they run in.
        std::swap(layer->effects()[0], layer->effects()[1]);
        doc.effectsChanged(layer);
        CHECK(doc.composite().at(1, 1) == Rgba(255, 245, 235, 255));

        // A disabled entry is skipped without being forgotten.
        layer->effects()[0].enabled = false;
        doc.effectsChanged(layer);
        CHECK(doc.composite().at(1, 1) == Rgba(245, 235, 225, 255));
        CHECK(layer->effects().size() == 2);
        layer->effects()[0].enabled = true;
        doc.effectsChanged(layer);

        // Flattening writes what the stack produced and empties it.
        const Image expected = doc.composite();
        CHECK(doc.flattenLayerEffects(layer));
        CHECK(layer->effects().empty());
        CHECK(layer->image.at(1, 1) == Rgba(255, 245, 235, 255));
        CHECK(samePixels(doc.composite(), expected));
        CHECK(!doc.flattenLayerEffects(layer));
    });

    addTest("a stack and an adjustment layer are only recomputed when needed", [] {
        registerCountingEffect();
        Document doc(64, 64, Rgba(10, 20, 30, 255));
        Layer* layer = doc.layerAt(0);
        Layer* above = doc.addEmptyLayer();
        std::vector<LayerEffect> stack;
        for (int shift : {5, 10, 20}) {
            ParamValues values;
            values.set(QStringLiteral("shift"), shift);
            stack.push_back({QStringLiteral("test_counting"), values, true});
        }
        doc.setLayerEffects(layer, stack);

        countingRuns = 0;
        doc.composite();
        CHECK(countingRuns == 3);
        // A second composite is the cached one.
        doc.composite();
        CHECK(countingRuns == 3);
        // Pixels on another layer have nothing to do with this stack.
        doc.invalidateLayer(above);
        doc.composite();
        CHECK(countingRuns == 3);
        // Editing the top entry reruns that entry alone.
        layer->effects()[2].values.set(QStringLiteral("shift"), 21);
        doc.effectsChanged(layer);
        doc.composite();
        CHECK(countingRuns == 4);
        // Editing the bottom one reruns everything above it as well.
        layer->effects()[0].values.set(QStringLiteral("shift"), 6);
        doc.effectsChanged(layer);
        doc.composite();
        CHECK(countingRuns == 7);
        // So does painting on the layer the stack sits on.
        doc.invalidateLayer(layer);
        doc.composite();
        CHECK(countingRuns == 10);

        // An adjustment layer caches the same way, keyed on the backdrop it
        // was computed from rather than on any one layer's pixels.
        ParamValues values;
        values.set(QStringLiteral("shift"), 30);
        Layer* adjustment = doc.addAdjustmentLayer(QStringLiteral("test_counting"),
                                                   values);
        countingRuns = 0;
        doc.composite();
        CHECK(countingRuns == 4); // three for the stack, one for the adjustment
        doc.composite();
        CHECK(countingRuns == 4);
        // Its own opacity is applied after the effect, so moving it reuses the
        // result.
        adjustment->opacity = 200;
        doc.effectsChanged(adjustment);
        doc.composite();
        CHECK(countingRuns == 4);
        // Pixels below it are what it works on, so those do rerun it.
        doc.invalidateLayer(above);
        doc.composite();
        CHECK(countingRuns == 5);
    });

    addTest("stacks and adjustment layers survive undo", [] {
        registerBuiltinEffects();
        Document doc(8, 8, Rgba(10, 20, 30, 255));
        History history(&doc);
        Layer* layer = doc.layerAt(0);
        recordDocumentEdit(doc, history, QStringLiteral("Add Effect"),
                           QStringLiteral("effect"), [&] {
                               doc.setLayerEffects(
                                   layer, {{QStringLiteral("invert_colors"),
                                            ParamValues(), true}});
                           });
        CHECK(doc.composite().at(1, 1) == Rgba(245, 235, 225, 255));
        recordDocumentEdit(doc, history, QStringLiteral("New Adjustment Layer"),
                           QStringLiteral("adjust"), [&] {
                               doc.addAdjustmentLayer(
                                   QStringLiteral("invert_colors"), ParamValues(),
                                   QStringLiteral("Invert"));
                           });
        CHECK(doc.layerCount() == 2);
        CHECK(doc.composite().at(1, 1) == Rgba(10, 20, 30, 255));

        CHECK(history.undo());
        CHECK(doc.layerCount() == 1);
        CHECK(doc.layerAt(0)->effects().size() == 1);
        CHECK(doc.composite().at(1, 1) == Rgba(245, 235, 225, 255));
        CHECK(history.undo());
        CHECK(doc.layerAt(0)->effects().empty());
        CHECK(doc.composite().at(1, 1) == Rgba(10, 20, 30, 255));

        CHECK(history.redo());
        CHECK(doc.layerAt(0)->effects().size() == 1);
        CHECK(doc.layerAt(0)->effects().front().effectId
              == QStringLiteral("invert_colors"));
        CHECK(history.redo());
        CHECK(doc.layerCount() == 2);
        CHECK(doc.layerAt(1)->isAdjustment());
        CHECK(doc.layerAt(1)->name == QStringLiteral("Invert"));
        CHECK(doc.composite().at(1, 1) == Rgba(10, 20, 30, 255));
    });

    addTest("stacks and adjustment layers round trip files", [] {
        registerBuiltinEffects();
        QTemporaryDir dir;
        Document doc(16, 12, Rgba(10, 20, 30, 255));
        Layer* bottom = doc.layerAt(0);
        ParamValues brighter;
        brighter.set(QStringLiteral("brightness"), 20);
        brighter.set(QStringLiteral("contrast"), 0);
        doc.setLayerEffects(bottom,
                            {{QStringLiteral("invert_colors"), ParamValues(), true},
                             {QStringLiteral("brightness_contrast"), brighter, false}});
        Layer* adjustment = doc.addAdjustmentLayer(QStringLiteral("brightness_contrast"),
                                                   brighter, QStringLiteral("Brighter"));
        adjustment->opacity = 200;
        adjustment->mode = BlendMode::Multiply;
        Layer* mask = doc.addMask(adjustment);
        ops::fillRegion(mask->image, Rect(0, 0, 8, 12), Rgba(0, 0, 0, 255));
        doc.invalidate();
        const Image expected = doc.composite();
        QString error;

        const QString path = dir.filePath(QStringLiteral("fx.tessel"));
        CHECK(io::saveDocument(doc, path, &error));
        auto loaded = io::openDocument(path, &error);
        CHECK(loaded != nullptr);
        if (loaded) {
            CHECK(loaded->layerCount() == 2);
            const Layer* loadedBottom = loaded->layerAt(0);
            CHECK(loadedBottom->effects().size() == 2);
            CHECK(loadedBottom->effects()[0].effectId
                  == QStringLiteral("invert_colors"));
            CHECK(loadedBottom->effects()[0].enabled);
            CHECK(loadedBottom->effects()[1].effectId
                  == QStringLiteral("brightness_contrast"));
            CHECK(!loadedBottom->effects()[1].enabled);
            CHECK(loadedBottom->effects()[1].values.intValue(
                      QStringLiteral("brightness")) == 20);
            // The pixels are stored untouched, the stack is what changes them.
            CHECK(loadedBottom->image.at(1, 1) == Rgba(10, 20, 30, 255));
            const Layer* loadedAdjustment = loaded->layerAt(1);
            CHECK(loadedAdjustment->isAdjustment());
            CHECK(loadedAdjustment->name == QStringLiteral("Brighter"));
            CHECK(loadedAdjustment->opacity == 200);
            CHECK(loadedAdjustment->mode == BlendMode::Multiply);
            CHECK(loadedAdjustment->hasMask());
            CHECK(loadedAdjustment->effects().size() == 1);
            CHECK(samePixels(loaded->composite(), expected));
        }

        // OpenRaster and the flat formats have neither, so both are baked into
        // the pixels that are written.
        const QString oraPath = dir.filePath(QStringLiteral("fx.ora"));
        CHECK(io::saveDocument(doc, oraPath, &error));
        auto ora = io::openDocument(oraPath, &error);
        CHECK(ora != nullptr);
        if (ora) {
            for (int i = 0; i < ora->layerCount(); ++i) {
                CHECK(!ora->layerAt(i)->isAdjustment());
                CHECK(ora->layerAt(i)->effects().empty());
            }
            CHECK(samePixels(ora->composite(), expected));
        }
        const QString pngPath = dir.filePath(QStringLiteral("fx.png"));
        CHECK(io::saveDocument(doc, pngPath, &error));
        auto png = io::openDocument(pngPath, &error);
        CHECK(png != nullptr);
        if (png)
            CHECK(samePixels(png->composite(), expected));
    });

    addTest("a manifest written before effects existed still opens", [] {
        registerBuiltinEffects();
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("old.tessel"));
        {
            Document doc(8, 8, Rgba(10, 20, 30, 255));
            QString error;
            CHECK(io::saveDocument(doc, path, &error));
        }
        // Strip every key this change added, which is what a file written by
        // the previous release looks like.
        ZipReader reader(path);
        CHECK(reader.isValid());
        QJsonObject root =
            QJsonDocument::fromJson(reader.read(QStringLiteral("document.json"))).object();
        QJsonArray layers = root.value(QStringLiteral("layers")).toArray();
        QJsonObject first = layers.at(0).toObject();
        CHECK(!first.contains(QStringLiteral("effects")));
        CHECK(!first.contains(QStringLiteral("adjustment")));
        QString error;
        auto loaded = io::openDocument(path, &error);
        CHECK(loaded != nullptr);
        if (loaded) {
            CHECK(loaded->layerCount() == 1);
            CHECK(loaded->layerAt(0)->effects().empty());
            CHECK(!loaded->layerAt(0)->isAdjustment());
            CHECK(loaded->composite().at(1, 1) == Rgba(10, 20, 30, 255));
        }
    });

    addTest("a stack of three effects on a large document", [] {
        registerBuiltinEffects();
        Document doc(2000, 1500, Rgba(10, 20, 30, 255));
        doc.layerAt(0)->image = gradient(2000, 1500);
        Layer* layer = doc.layerAt(0);
        ParamValues blur;
        blur.set(QStringLiteral("radius"), 4.0);
        ParamValues bright;
        bright.set(QStringLiteral("brightness"), 15);
        bright.set(QStringLiteral("contrast"), 10);
        ParamValues hue;
        hue.set(QStringLiteral("hue"), 20);
        hue.set(QStringLiteral("saturation"), 120);
        hue.set(QStringLiteral("lightness"), 0);
        doc.setLayerEffects(layer,
                            {{QStringLiteral("gaussian_blur"), blur, true},
                             {QStringLiteral("brightness_contrast"), bright, true},
                             {QStringLiteral("hue_saturation"), hue, true}});
        doc.invalidate();
        QElapsedTimer clock;
        clock.start();
        doc.composite();
        const qint64 cold = clock.elapsed();
        clock.restart();
        doc.invalidate();
        doc.composite();
        const qint64 again = clock.elapsed();
        // Dragging a slider on the last entry: the two below it are answered
        // from the cache.
        clock.restart();
        layer->effects()[2].values.set(QStringLiteral("hue"), 21);
        doc.effectsChanged(layer);
        doc.composite();
        const qint64 lastEntry = clock.elapsed();
        clock.restart();
        layer->effects()[0].values.set(QStringLiteral("radius"), 4.5);
        doc.effectsChanged(layer);
        doc.composite();
        const qint64 firstEntry = clock.elapsed();
        // An adjustment layer over the same document: dragging its slider
        // reruns its own effect over the backdrop, and nothing else.
        ParamValues shift;
        shift.set(QStringLiteral("brightness"), 10);
        shift.set(QStringLiteral("contrast"), 0);
        Layer* adjustment =
            doc.addAdjustmentLayer(QStringLiteral("brightness_contrast"), shift);
        doc.composite();
        clock.restart();
        adjustment->effects().front().values.set(QStringLiteral("brightness"), 11);
        doc.effectsChanged(adjustment);
        doc.composite();
        const qint64 adjusted = clock.elapsed();
        std::printf("    2000x1500, three effects: %lld ms cold, %lld ms rebuilt, "
                    "%lld ms for the last entry, %lld ms for the first, "
                    "%lld ms for an adjustment layer\n",
                    static_cast<long long>(cold), static_cast<long long>(again),
                    static_cast<long long>(lastEntry),
                    static_cast<long long>(firstEntry),
                    static_cast<long long>(adjusted));
        // Rerunning one cheap entry has to cost a fraction of the whole stack,
        // or no slider could be dragged.
        CHECK(lastEntry < cold);
        CHECK(lastEntry <= firstEntry);
        CHECK(adjusted < cold);
    });

    // ------------------------------------------------------------ plugins
    addTest("plugin directories", [] {
        const QStringList dirs = plugins::directories();
        CHECK(!dirs.isEmpty());
        for (const QString& dir : dirs)
            check(dir.endsWith(QStringLiteral("/plugins")), "ends with /plugins", __LINE__);
        // One of them always sits beside the executable, so a build tree can be
        // run from without installing anything.
        CHECK(dirs.contains(QCoreApplication::applicationDirPath()
                            + QStringLiteral("/plugins")));
        // The same directory is never scanned twice.
        CHECK(QSet<QString>(dirs.begin(), dirs.end()).size() == dirs.size());
    });

    addTest("the plugin version gate", [] {
        EffectRegistry registry;
        plugins::LoadReport report;

        // Built against a version this host does not host: refused, named, and
        // nothing of its own reaches the registry.
        FakePlugin stale(kEffectPluginVersion + 1, QStringLiteral("stale_fake"),
                         QStringLiteral("Examples"));
        CHECK(!plugins::acceptPlugin(&stale, QStringLiteral("/tmp/stale.so"), registry,
                                     report));
        CHECK(report.loaded.empty());
        CHECK(report.failed.size() == 1);
        CHECK(report.failed[0].reason.contains(
            QString::number(kEffectPluginVersion + 1)));
        CHECK(report.failed[0].reason.contains(QString::number(kEffectPluginVersion)));
        CHECK(registry.all().empty());

        // A matching one registers, and the report says what it brought.
        FakePlugin good(kEffectPluginVersion, QStringLiteral("good_fake"),
                        QStringLiteral("Examples"));
        CHECK(plugins::acceptPlugin(&good, QStringLiteral("/tmp/good.so"), registry,
                                    report));
        CHECK(report.loaded.size() == 1);
        CHECK(report.loaded[0].name == QStringLiteral("Fake Plugin"));
        CHECK(report.loaded[0].version == QStringLiteral("2.3"));
        CHECK(report.loaded[0].author == QStringLiteral("Nobody"));
        CHECK(report.loaded[0].effects == QStringList{QStringLiteral("Fake Effect")});
        CHECK(registry.byId(QStringLiteral("good_fake")) != nullptr);
        CHECK(registry.categories() == QStringList{QStringLiteral("Examples")});

        // A null plugin is a failure, not a crash.
        CHECK(!plugins::acceptPlugin(nullptr, QStringLiteral("/tmp/null.so"), registry,
                                     report));
        CHECK(report.failed.size() == 2);
    });

    addTest("loading a directory of broken plugins", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        EffectRegistry registry;
        plugins::LoadReport report;

        // Nothing there at all, and a directory that does not exist: both are
        // silent, because that is the normal case.
        plugins::loadDirectory(dir.path(), registry, report);
        plugins::loadDirectory(dir.filePath(QStringLiteral("absent")), registry, report);
        CHECK(report.loaded.empty());
        CHECK(report.failed.empty());

        // Anything that is not a library is somebody's notes, not a plugin.
        QFile notes(dir.filePath(QStringLiteral("README.txt")));
        CHECK(notes.open(QIODevice::WriteOnly));
        notes.write("not a plugin\n");
        notes.close();
        plugins::loadDirectory(dir.path(), registry, report);
        CHECK(report.failed.empty());

        // A library that is not one is reported with the loader's own reason.
        QFile broken(dir.filePath(QString::fromLatin1(libraryName())));
        CHECK(broken.open(QIODevice::WriteOnly));
        broken.write("this is not an object file\n");
        broken.close();
        plugins::loadDirectory(dir.path(), registry, report);
        CHECK(report.loaded.empty());
        CHECK(report.failed.size() == 1);
        CHECK(!report.failed[0].reason.isEmpty());
        CHECK(report.failed[0].path.endsWith(QString::fromLatin1(libraryName())));
    });

    addTest("the plugin report reads back", [] {
        plugins::LoadReport empty;
        // A machine with no plugins has nothing to say in the status bar.
        CHECK(empty.summary().isEmpty());
        CHECK(empty.html().contains(QStringLiteral("No plugins were found")));

        plugins::LoadReport report;
        report.directories = QStringList{QStringLiteral("/home/someone/plugins")};
        report.loaded.push_back({QStringLiteral("/home/someone/plugins/a.so"),
                                 QStringLiteral("Scanlines & Co"), QStringLiteral("1.0"),
                                 QStringLiteral("Nobody"),
                                 QStringList{QStringLiteral("Scanlines")}});
        // The count goes through a plural message, so the English source text
        // keeps its "(s)" until a translation replaces it.
        CHECK(report.summary() == QStringLiteral("1 effect plugin(s) loaded."));

        report.failed.push_back({QStringLiteral("/home/someone/plugins/b.so"),
                                 QStringLiteral("cannot load")});
        // A failure takes over the line, because it is the part worth reading.
        CHECK(report.summary().contains(QStringLiteral("failed to load")));
        CHECK(report.summary().contains(QStringLiteral("About the")));

        const QString html = report.html();
        CHECK(html.contains(QStringLiteral("/home/someone/plugins")));
        CHECK(html.contains(QStringLiteral("Scanlines &amp; Co"))); // escaped
        CHECK(html.contains(QStringLiteral("Nobody")));
        CHECK(html.contains(QStringLiteral("b.so")));
        CHECK(html.contains(QStringLiteral("cannot load")));
        CHECK(html.contains(QString::number(kEffectPluginVersion)));
    });

#ifdef TESSEL_EXAMPLE_PLUGIN_FILE
    addTest("the example plugin loads from its library", [] {
        EffectRegistry registry;
        plugins::LoadReport report;
        plugins::loadFile(QString::fromLatin1(TESSEL_EXAMPLE_PLUGIN_FILE), registry,
                          report);
        CHECK(report.failed.empty());
        CHECK(report.loaded.size() == 1);
        CHECK(report.loaded[0].name == QStringLiteral("Scanlines Example"));
        CHECK(report.loaded[0].effects == QStringList{QStringLiteral("Scanlines")});

        Effect* effect = registry.byId(QStringLiteral("example_scanlines"));
        CHECK(effect != nullptr);
        if (!effect)
            return;
        CHECK(effect->category() == QStringLiteral("Examples"));
        CHECK(effect->params().size() == 3);
        CHECK(effect->hasDialog());

        // The effect itself runs like any other, so the generated dialog and
        // the live preview have everything they need from it.
        const Image source = solid(16, 16, Rgba(200, 200, 200, 255));
        EffectContext context;
        context.source = &source;
        context.region = source.rect();
        ParamValues values = ParamValues::defaults(effect->params());
        CHECK(values.intValue(QStringLiteral("spacing")) == 4);
        const Image out = effect->apply(context, values);
        CHECK(out.width() == 16 && out.height() == 16);
        CHECK(out.at(0, 0).r < 200); // every fourth row is tinted
        CHECK(out.at(0, 1) == source.at(0, 1));
        CHECK(out.at(0, 4).r < 200);
        // Strength at zero is a no op, which is where the preview starts from
        // when the slider is dragged down.
        values.set(QStringLiteral("strength"), 0.0);
        CHECK(samePixels(effect->apply(context, values), source));
    });
#endif

    // ------------------------------------------------------------ palettes
    addTest("palette gpl round trip", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        QVector<Rgba> colors;
        for (int i = 0; i < 24; ++i) {
            colors.append(Rgba(uint8_t(i * 7), uint8_t(255 - i * 3), uint8_t(i * 11),
                               255));
        }
        Palette palette(colors, QStringLiteral("Round trip"));
        palette.setColumns(8);

        const QString path = dir.filePath(QStringLiteral("round.gpl"));
        QString error = QStringLiteral("unset");
        CHECK(palette.save(path, &error));
        CHECK(error.isEmpty());
        const Palette back = Palette::load(path, &error);
        CHECK(error.isEmpty());
        CHECK(back.size() == palette.size());
        CHECK(back.colors() == colors);
        CHECK(back.name() == QStringLiteral("Round trip"));
        CHECK(back.columns() == 8);

        // Written by hand the way GIMP writes it, tabs and all.
        const Palette parsed = Palette::parseGpl(QStringLiteral(
            "GIMP Palette\nName: Hand made\nColumns: 4\n#\n"
            "  0   0   0\tBlack\n255 255 255\tWhite\n 18  52  86\tSome blue\n"));
        CHECK(parsed.size() == 3);
        CHECK(parsed.name() == QStringLiteral("Hand made"));
        CHECK(parsed.columns() == 4);
        CHECK(parsed.colors()[0] == Rgba(0, 0, 0, 255));
        CHECK(parsed.colors()[1] == Rgba(255, 255, 255, 255));
        CHECK(parsed.colors()[2] == Rgba(18, 52, 86, 255));

        // The format carries no alpha, so translucency is dropped on the way
        // out rather than corrupting the file.
        const QString alphaPath = dir.filePath(QStringLiteral("alpha.gpl"));
        CHECK(Palette(QVector<Rgba>{Rgba(10, 20, 30, 64)}).save(alphaPath, &error));
        const Palette alphaBack = Palette::load(alphaPath, &error);
        CHECK(alphaBack.size() == 1);
        CHECK(alphaBack.colors()[0] == Rgba(10, 20, 30, 255));

        // A GIMP palette saved under another extension is still recognised.
        const QString oddPath = dir.filePath(QStringLiteral("odd.txt"));
        QFile odd(oddPath);
        CHECK(odd.open(QIODevice::WriteOnly));
        odd.write(palette.toGpl().toUtf8());
        odd.close();
        CHECK(Palette::load(oddPath, &error).colors() == colors);
    });

    addTest("palette hex round trip", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QVector<Rgba> colors{Rgba(255, 0, 0, 255), Rgba(0, 255, 0, 128),
                                   Rgba(1, 2, 3, 0)};
        Palette palette(colors, QStringLiteral("Hexes"));
        const QString text = palette.toHex();
        CHECK(text.contains(QStringLiteral("#FF0000\n")));
        CHECK(text.contains(QStringLiteral("#00FF0080\n")));
        CHECK(text.contains(QStringLiteral("#01020300\n")));

        const Palette back = Palette::parseHex(text);
        CHECK(back.colors() == colors);
        CHECK(back.name() == QStringLiteral("Hexes"));

        const QString path = dir.filePath(QStringLiteral("list.txt"));
        QString error = QStringLiteral("unset");
        CHECK(palette.save(path, &error));
        CHECK(error.isEmpty());
        const Palette fromFile = Palette::load(path, &error);
        CHECK(error.isEmpty());
        CHECK(fromFile.colors() == colors);

        // Blank lines and comments are skipped, the hash is optional, and a
        // comment is told from a colour by what follows the hash.
        const Palette mixed = Palette::parseHex(QStringLiteral(
            "# a comment\n\n   \n#112233\n\t#445566  \nffaabb\n"
            "# Name: Mixed\n#not a colour\n"));
        CHECK(mixed.size() == 3);
        CHECK(mixed.name() == QStringLiteral("Mixed"));
        CHECK(mixed.colors()[0] == Rgba(0x11, 0x22, 0x33, 255));
        CHECK(mixed.colors()[1] == Rgba(0x44, 0x55, 0x66, 255));
        CHECK(mixed.colors()[2] == Rgba(0xff, 0xaa, 0xbb, 255));

        // Case does not matter and CRLF files read the same.
        const Palette crlf = Palette::parseHex(QStringLiteral("#AbCdEf\r\n#123456\r\n"));
        CHECK(crlf.size() == 2);
        CHECK(crlf.colors()[0] == Rgba(0xab, 0xcd, 0xef, 255));

        Rgba single;
        CHECK(Palette::parseHexColor(QStringLiteral("  #0a0B0c  "), single));
        CHECK(single == Rgba(10, 11, 12, 255));
        CHECK(Palette::parseHexColor(QStringLiteral("0a0b0c40"), single));
        CHECK(single == Rgba(10, 11, 12, 0x40));
        CHECK(Palette::toHexString(Rgba(10, 11, 12, 255)) == QStringLiteral("#0A0B0C"));
        CHECK(Palette::toHexString(Rgba(10, 11, 12, 1)) == QStringLiteral("#0A0B0C01"));
    });

    addTest("palette parsing survives malformed input", [] {
        // Nothing here may crash, hang or report colours that are not there.
        const char* junk[] = {
            "",
            "\n\n\n\r\r\n",
            "GIMP Palette",
            "GIMP Palette\nName:\nColumns: nonsense\nColumns: -9000\n",
            "#", "###", "# comment only\n", "Name: only\n",
            "0 0\n", "oops 1 2 3\n", "1 2\t\n",
            "99999999999999999999 0 0\n",
            "\x01\x02\x03 binary \xff\xfe",
            "#12345\n#1234567\n#123456789\n#GGHHII\n#\n##\n",
            "0,0,0\n255,255,255\n",
        };
        for (const char* text : junk) {
            const QString input = QString::fromUtf8(text);
            CHECK(Palette::parseGpl(input).isEmpty());
            CHECK(Palette::parseHex(input).isEmpty());
        }

        // A bad line costs only itself.
        const Palette partial = Palette::parseGpl(QStringLiteral(
            "GIMP Palette\nnot a colour at all\n255 255 255 White\n"
            "4294967296 0 0\n0 0 0\n"));
        CHECK(partial.size() == 2);
        CHECK(partial.colors()[0] == Rgba(255, 255, 255, 255));
        CHECK(partial.colors()[1] == Rgba(0, 0, 0, 255));

        // Out of range channels are clamped, since the entry is clearly meant
        // to be a colour.
        const Palette clamped = Palette::parseGpl(QStringLiteral("300 -10 20 Bad\n"));
        CHECK(clamped.size() == 1);
        CHECK(clamped.colors()[0] == Rgba(255, 0, 20, 255));

        // Embedded nul bytes and a very long line are just skipped.
        QString nulled = QStringLiteral("#112233\n");
        nulled.append(QChar(QChar::Null));
        nulled.append(QStringLiteral("0 0 0\n"));
        CHECK(Palette::parseHex(nulled).size() == 1);
        CHECK(Palette::parseHex(QString(200000, QLatin1Char('x'))).isEmpty());

        // Files that cannot be read, or hold nothing useful, report an error
        // instead of an empty palette with no explanation.
        QTemporaryDir dir;
        CHECK(dir.isValid());
        QString error;
        CHECK(Palette::load(dir.filePath(QStringLiteral("absent.gpl")), &error).isEmpty());
        CHECK(!error.isEmpty());
        error.clear();
        CHECK(Palette::load(dir.path(), &error).isEmpty());      // a directory
        CHECK(!error.isEmpty());
        error.clear();
        const QString emptyPath = dir.filePath(QStringLiteral("empty.gpl"));
        QFile empty(emptyPath);
        CHECK(empty.open(QIODevice::WriteOnly));
        empty.close();
        CHECK(Palette::load(emptyPath, &error).isEmpty());
        CHECK(!error.isEmpty());
        error.clear();
        const QString binaryPath = dir.filePath(QStringLiteral("binary.txt"));
        QFile binary(binaryPath);
        CHECK(binary.open(QIODevice::WriteOnly));
        QByteArray noise;
        for (int i = 0; i < 4096; ++i)
            noise.append(char(0x80 + (i % 0x7f)));   // not valid UTF-8, no digits
        binary.write(noise);
        binary.close();
        CHECK(Palette::load(binaryPath, &error).isEmpty());
        CHECK(!error.isEmpty());

        // Saving where nothing can be written fails cleanly.
        error.clear();
        CHECK(!Palette(QVector<Rgba>{Rgba(1, 2, 3, 255)})
                   .save(dir.filePath(QStringLiteral("no/such/dir/p.gpl")), &error));
        CHECK(!error.isEmpty());
    });

    addTest("palettes with thousands of entries", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        QVector<Rgba> colors;
        for (int i = 0; i < 5000; ++i) {
            colors.append(Rgba(uint8_t(i & 0xff), uint8_t((i >> 3) & 0xff),
                               uint8_t((i >> 6) & 0xff), 255));
        }
        const Palette palette(colors, QStringLiteral("Big"));
        QString error = QStringLiteral("unset");

        const QString gplPath = dir.filePath(QStringLiteral("big.gpl"));
        CHECK(palette.save(gplPath, &error));
        const Palette fromGpl = Palette::load(gplPath, &error);
        CHECK(error.isEmpty());
        CHECK(fromGpl.size() == 5000);
        CHECK(fromGpl.colors() == colors);

        const QString hexPath = dir.filePath(QStringLiteral("big.txt"));
        CHECK(palette.save(hexPath, &error));
        const Palette fromHex = Palette::load(hexPath, &error);
        CHECK(error.isEmpty());
        CHECK(fromHex.size() == 5000);
        CHECK(fromHex.colors() == colors);

        // Beyond the cap the parser stops instead of growing without bound.
        QString overflow;
        for (int i = 0; i < Palette::kMaxColors + 32; ++i)
            overflow += QStringLiteral("#010203\n");
        CHECK(Palette::parseHex(overflow).size() == Palette::kMaxColors);
        Palette capped;
        for (int i = 0; i < Palette::kMaxColors + 32; ++i)
            capped.append(Rgba(1, 2, 3, 255));
        CHECK(capped.size() == Palette::kMaxColors);
    });

    // ------------------------------------------------------------ recovery
    addTest("recovery options round trip", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        {
            QSettings settings(file, QSettings::IniFormat);
            const recovery::Options defaults = recovery::loadOptions(settings);
            CHECK(defaults.autosave);
            CHECK(defaults.restoreSession);
            CHECK(defaults.autosaveMinutes == 2);
            CHECK(defaults.keepDays == 14);

            recovery::Options options;
            options.autosave = false;
            options.autosaveMinutes = 9;
            options.restoreSession = false;
            options.keepDays = 3;
            recovery::storeOptions(settings, options);
        }
        QSettings settings(file, QSettings::IniFormat);
        const recovery::Options options = recovery::loadOptions(settings);
        CHECK(!options.autosave);
        CHECK(options.autosaveMinutes == 9);
        CHECK(!options.restoreSession);
        CHECK(options.keepDays == 3);

        // Nonsense in the file is clamped rather than taken literally, because
        // a zero interval would mean a timer that never fires.
        settings.setValue(QStringLiteral("recovery/autosaveMinutes"), 100000);
        settings.setValue(QStringLiteral("recovery/keepDays"), 0);
        const recovery::Options fixed = recovery::loadOptions(settings);
        CHECK(fixed.autosaveMinutes == recovery::kMaxAutosaveMinutes);
        CHECK(fixed.keepDays == recovery::kMinKeepDays);
    });

    addTest("the session list drops files that went away", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString present = dir.filePath(QStringLiteral("a.tessel"));
        Document doc(4, 4, Rgba(1, 2, 3, 255));
        QString error;
        CHECK(io::saveDocument(doc, present, &error));

        recovery::OpenDocuments open;
        open.paths = QStringList{dir.filePath(QStringLiteral("gone.tessel")), present};
        open.current = 1;
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        {
            QSettings settings(file, QSettings::IniFormat);
            recovery::storeOpenDocuments(settings, open);
        }
        QSettings settings(file, QSettings::IniFormat);
        const recovery::OpenDocuments loaded = recovery::loadOpenDocuments(settings);
        CHECK(loaded.paths.size() == 2);
        CHECK(loaded.current == 1);

        const recovery::OpenDocuments kept = recovery::existingOnly(loaded);
        CHECK(kept.paths.size() == 1);
        CHECK(kept.paths.first() == present);
        CHECK(kept.current == 0);

        // An empty list stays empty, with nothing in front.
        CHECK(recovery::existingOnly(recovery::OpenDocuments()).paths.isEmpty());
        CHECK(recovery::existingOnly(recovery::OpenDocuments()).current == -1);
    });

    addTest("a recovery copy outlives its session", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));

        Document doc(20, 12, Rgba(255, 255, 255, 255));
        doc.addEmptyLayer();
        doc.layerAt(1)->image.setAt(3, 4, Rgba(10, 20, 30, 255));
        doc.layerAt(1)->name = QStringLiteral("Sketch");

        QString written;
        {
            recovery::Session session;
            CHECK(session.isOpen());
            CHECK(session.write(7, doc.snapshot(),
                                QStringLiteral("/pictures/sketch.png"),
                                QStringLiteral("sketch.png"), nullptr));
            written = session.pathFor(7);
            CHECK(QFileInfo::exists(written));
            // A session that is still going is never offered for recovery.
            CHECK(recovery::findAbandoned().empty());
        }

        std::vector<recovery::Abandoned> abandoned = recovery::findAbandoned();
        CHECK(abandoned.size() == 1);
        const std::vector<recovery::Entry> entries = recovery::allEntries(abandoned);
        CHECK(entries.size() == 1);
        CHECK(entries.front().path == written);
        CHECK(entries.front().originalPath == QStringLiteral("/pictures/sketch.png"));
        CHECK(entries.front().title == QStringLiteral("sketch.png"));
        CHECK(entries.front().displayName() == QStringLiteral("sketch.png"));
        CHECK(entries.front().size > 0);
        CHECK(entries.front().savedAt.isValid());

        QString error;
        std::unique_ptr<Document> read = io::openDocument(entries.front().path, &error);
        CHECK(read != nullptr);
        if (read) {
            CHECK(read->width() == 20 && read->height() == 12);
            CHECK(read->layerCount() == 2);
            CHECK(read->layerAt(1)->name == QStringLiteral("Sketch"));
            CHECK(read->layerAt(1)->image.at(3, 4) == Rgba(10, 20, 30, 255));
        }

        recovery::discard(abandoned);
        CHECK(abandoned.empty());
        CHECK(!QFileInfo::exists(written));
        CHECK(recovery::findAbandoned().empty());
        recovery::setRoot(QString());
    });

    addTest("a dropped recovery copy takes its session with it", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));
        Document doc(8, 8, Rgba(0, 0, 0, 255));
        QString written;
        QString directory;
        {
            recovery::Session session;
            CHECK(session.write(1, doc.snapshot(), QString(),
                                QStringLiteral("Untitled 1"), nullptr));
            written = session.pathFor(1);
            directory = session.directory();
            CHECK(QFileInfo::exists(written));
            session.drop(1);
            CHECK(!QFileInfo::exists(written));
        }
        // Nothing was left to recover, so the directory went as well and the
        // next startup has nothing to offer.
        CHECK(!QFileInfo::exists(directory));
        CHECK(recovery::findAbandoned().empty());

        // An empty document is refused rather than written as a broken file.
        recovery::Session session;
        DocumentState empty;
        QString error;
        CHECK(!session.write(2, empty, QString(), QStringLiteral("Broken"), &error));
        CHECK(!error.isEmpty());
        recovery::setRoot(QString());
    });

    addTest("the autosaver writes only what changed", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));

        Document doc(64, 48, Rgba(255, 255, 255, 255));
        int snapshots = 0;
        qint64 revision = 1;
        int written = 0;
        QString failure;
        recovery::Autosaver saver;
        QObject::connect(&saver, &recovery::Autosaver::wrote,
                         [&written](int count) { written += count; });
        QObject::connect(&saver, &recovery::Autosaver::failed,
                         [&failure](const QString& message) { failure = message; });
        saver.setSource([&] {
            recovery::Autosaver::Request request;
            request.key = 3;
            request.title = QStringLiteral("Untitled 1");
            request.revision = revision;
            request.snapshot = [&] {
                ++snapshots;
                return doc.snapshot();
            };
            return std::vector<recovery::Autosaver::Request>{request};
        });

        // The way the timer does it: start the write and let the event loop
        // deliver the end of it.
        saver.saveNow();
        QElapsedTimer clock;
        clock.start();
        while (saver.isBusy() && clock.elapsed() < 10000)
            QCoreApplication::processEvents();
        CHECK(!saver.isBusy());
        CHECK(failure.isEmpty());
        CHECK(written == 1);
        CHECK(snapshots == 1);
        CHECK(saver.session() != nullptr);
        const QString path = saver.session() ? saver.session()->pathFor(3) : QString();
        CHECK(QFileInfo::exists(path));

        // Unchanged, so nothing is written and not even a snapshot is taken.
        saver.saveNow();
        saver.waitForIdle();
        CHECK(written == 1);
        CHECK(snapshots == 1);

        ++revision;
        saver.saveNow();
        saver.waitForIdle();
        CHECK(written == 2);
        CHECK(snapshots == 2);

        saver.forget(3);
        CHECK(!QFileInfo::exists(path));
        // Forgetting is not final: the document is still open and changed, so
        // the next pass writes it again.
        saver.saveNow();
        saver.waitForIdle();
        CHECK(QFileInfo::exists(path));
        CHECK(written == 3);

        saver.discardAll();
        CHECK(!QFileInfo::exists(path));
        CHECK(recovery::findAbandoned().empty());
        recovery::setRoot(QString());
    });

    addTest("the autosave timer follows the interval setting", [] {
        recovery::Autosaver saver;
        // On by default, because losing work is the expensive mistake here.
        CHECK(saver.isEnabled());
        CHECK(saver.intervalMinutes() == 2);
        // A coarse timer may round the interval up a little, so the bound is
        // the interval with some room above it.
        CHECK(saver.millisecondsToNextWrite() > 60 * 1000);
        CHECK(saver.millisecondsToNextWrite() <= 3 * 60 * 1000);
        saver.setIntervalMinutes(5);
        CHECK(saver.intervalMinutes() == 5);
        CHECK(saver.millisecondsToNextWrite() > 4 * 60 * 1000);
        CHECK(saver.millisecondsToNextWrite() <= 6 * 60 * 1000);
        // Out of range values are clamped rather than turning the timer off.
        saver.setIntervalMinutes(0);
        CHECK(saver.intervalMinutes() == recovery::kMinAutosaveMinutes);
        CHECK(saver.millisecondsToNextWrite() > 0);
        saver.setEnabled(false);
        CHECK(!saver.isEnabled());
        CHECK(saver.millisecondsToNextWrite() == -1);
        saver.setEnabled(true);
        CHECK(saver.millisecondsToNextWrite() > 0);
    });

    addTest("autosaving a large image hardly stops the caller", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));
        Document doc(2000, 1500, Rgba(10, 20, 30, 255));
        doc.layerAt(0)->image = gradient(2000, 1500);
        doc.invalidate();

        recovery::Autosaver saver;
        saver.setSource([&doc] {
            recovery::Autosaver::Request request;
            request.key = 1;
            request.title = QStringLiteral("Big");
            request.revision = 1;
            request.snapshot = [&doc] { return doc.snapshot(); };
            return std::vector<recovery::Autosaver::Request>{request};
        });
        QElapsedTimer clock;
        clock.start();
        saver.saveNow();
        const qint64 blocked = clock.elapsed();
        saver.waitForIdle();
        const qint64 total = clock.elapsed();
        std::printf("    2000x1500 autosave: %lld ms on the caller, %lld ms in all\n",
                    static_cast<long long>(blocked), static_cast<long long>(total));
        // The caller only copies the pixels; encoding and writing them is the
        // worker's business.
        CHECK(blocked <= total);
        CHECK(blocked < 500);
        CHECK(saver.session() != nullptr);
        CHECK(QFileInfo::exists(saver.session()->pathFor(1)));
        saver.discardAll();
        recovery::setRoot(QString());
    });

    addTest("stale recovery copies are swept up", [] {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        recovery::setRoot(dir.filePath(QStringLiteral("recovery")));
        Document doc(6, 6, Rgba(0, 0, 0, 255));
        {
            recovery::Session session;
            CHECK(session.write(1, doc.snapshot(), QString(),
                                QStringLiteral("Untitled 1"), nullptr));
        }
        std::vector<recovery::Abandoned> found = recovery::findAbandoned();
        CHECK(found.size() == 1);
        const QString directory = found.empty() ? QString() : found.front().directory;
        found.clear(); // releases the claim, so the sweep can take it over

        // Backdate what the sidecar says, which is both what the dialog shows
        // and what the sweep goes by.
        QFile meta(QDir(directory).filePath(QStringLiteral("doc-1.json")));
        CHECK(meta.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QJsonObject object;
        object[QStringLiteral("originalPath")] = QString();
        object[QStringLiteral("title")] = QStringLiteral("Untitled 1");
        object[QStringLiteral("savedAt")] =
            QDateTime::currentDateTime().addDays(-30).toString(Qt::ISODate);
        meta.write(QJsonDocument(object).toJson());
        meta.close();
        CHECK(recovery::findAbandoned().size() == 1);
        CHECK(recovery::pruneOlderThan(14) == 1);
        CHECK(recovery::findAbandoned().empty());
        CHECK(!QFileInfo::exists(directory));

        // Something written just now is left alone.
        {
            recovery::Session session;
            CHECK(session.write(2, doc.snapshot(), QString(),
                                QStringLiteral("Untitled 2"), nullptr));
        }
        CHECK(recovery::pruneOlderThan(14) == 0);
        std::vector<recovery::Abandoned> left = recovery::findAbandoned();
        CHECK(left.size() == 1);
        recovery::discard(left);
        recovery::setRoot(QString());
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
