// The Render submenu: effects that generate pixels rather than transform them.
#include "effects/Effect.h"

#include <cmath>

#include "core/Strings.h"

namespace tessel {

void registerRenderers(EffectRegistry& registry)
{
    // ------------------------------------------------------------ clouds
    fx::add(registry, "clouds", QT_TRANSLATE_NOOP("Effects", "Clouds"), "Render",
            {
                ParamSpec::doubleParam("scale", QT_TRANSLATE_NOOP("Effects", "Scale"),
                                       2.0, 1000.0, 180.0, 1.0, 1),
                ParamSpec::doubleParam("roughness",
                                       QT_TRANSLATE_NOOP("Effects", "Roughness"),
                                       0.05, 0.95, 0.5, 0.05, 2),
                ParamSpec::intParam("detail", QT_TRANSLATE_NOOP("Effects", "Detail"),
                                    1, 8, 6),
                ParamSpec::intParam("seed", QT_TRANSLATE_NOOP("Effects", "Seed"),
                                    0, 999, 0),
                ParamSpec::choiceParam("blend",
                                       QT_TRANSLATE_NOOP("Effects", "Blend mode"),
                                       blendModeLabels(), 0),
                ParamSpec::boolParam("use_palette",
                                     QT_TRANSLATE_NOOP("Effects", "Use palette colours"),
                                     true),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double scale = std::max(2.0, values.doubleValue("scale", 180.0));
                const float roughness = float(values.doubleValue("roughness", 0.5));
                const int detail = values.intValue("detail", 6);
                const unsigned seed = unsigned(values.intValue("seed", 0)) * 6367u + 101u;
                const BlendMode mode =
                    static_cast<BlendMode>(std::clamp(values.choiceValue("blend", 0), 0,
                                                      int(BlendMode::Count) - 1));
                const bool palette = values.boolValue("use_palette", true);
                const Rgba from = palette ? context.primary : Rgba(0, 0, 0, 255);
                const Rgba to = palette ? context.secondary : Rgba(255, 255, 255, 255);

                const Image& src = *context.source;
                Image clouds(src.width(), src.height());
                const Rect area = context.region.intersected(src.rect());
                parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y) {
                        Rgba* line = clouds.row(y);
                        for (int x = area.x; x < area.right(); ++x) {
                            const float n = fx::fractalNoise(float(x / scale), float(y / scale),
                                                             seed, detail, roughness);
                            const float t = std::clamp(n, 0.0f, 1.0f);
                            line[x] = Rgba(
                                toByte((from.r + (to.r - from.r) * t) / 255.0f),
                                toByte((from.g + (to.g - from.g) * t) / 255.0f),
                                toByte((from.b + (to.b - from.b) * t) / 255.0f),
                                toByte((from.a + (to.a - from.a) * t) / 255.0f));
                        }
                    }
                });
                Image out = src;
                ops::imageOver(out, clouds, nullptr, mode, area);
                return out;
            });

    // ------------------------------------------------------------ julia
    fx::add(registry, "julia", QT_TRANSLATE_NOOP("Effects", "Julia Fractal"), "Render",
            {
                ParamSpec::doubleParam("factor", QT_TRANSLATE_NOOP("Effects", "Factor"),
                                       1.0, 10.0, 4.0, 0.1, 2),
                ParamSpec::doubleParam("zoom", QT_TRANSLATE_NOOP("Effects", "Zoom"),
                                       0.1, 50.0, 1.0, 0.1, 2),
                ParamSpec::angleParam("angle", QT_TRANSLATE_NOOP("Effects", "Angle"),
                                      0.0),
                ParamSpec::doubleParam("cr", QT_TRANSLATE_NOOP("Effects", "Seed real"),
                                       -2.0, 2.0, -0.74, 0.01, 3),
                ParamSpec::doubleParam("ci",
                                       QT_TRANSLATE_NOOP("Effects", "Seed imaginary"),
                                       -2.0, 2.0, 0.19, 0.01, 3),
                ParamSpec::intParam("quality", QT_TRANSLATE_NOOP("Effects", "Quality"),
                                    1, 5, 2),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double factor = values.doubleValue("factor", 4.0);
                const double zoom = std::max(0.1, values.doubleValue("zoom", 1.0));
                const double angle = values.doubleValue("angle", 0.0) * M_PI / 180.0;
                const double cr = values.doubleValue("cr", -0.74);
                const double ci = values.doubleValue("ci", 0.19);
                const int iterations = 64 * std::clamp(values.intValue("quality", 2), 1, 5);
                const Image& src = *context.source;
                const double half = std::min(src.width(), src.height()) * 0.5;
                const double ca = std::cos(angle), sa = std::sin(angle);
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    double u = (x - src.width() * 0.5) / (half * zoom);
                    double v = (y - src.height() * 0.5) / (half * zoom);
                    const double zr0 = u * ca - v * sa;
                    const double zi0 = u * sa + v * ca;
                    double zr = zr0, zi = zi0;
                    int i = 0;
                    for (; i < iterations; ++i) {
                        const double zr2 = zr * zr - zi * zi + cr;
                        zi = 2.0 * zr * zi + ci;
                        zr = zr2;
                        if (zr * zr + zi * zi > 16.0)
                            break;
                    }
                    if (i >= iterations)
                        return Rgba(0, 0, 0, 255);
                    const double smooth = (i + 1.0 - std::log2(std::max(1.0, std::log2(
                                               std::sqrt(zr * zr + zi * zi)))))
                        / iterations;
                    float r = 0, g = 0, b = 0;
                    ops::hsvToRgb(float(360.0 * std::fmod(smooth * factor, 1.0)), 0.85f,
                                  std::min(1.0f, float(smooth * 6.0)), r, g, b);
                    return Rgba(toByte(r), toByte(g), toByte(b), 255);
                });
            });

    // ------------------------------------------------------------ mandelbrot
    fx::add(registry, "mandelbrot", QT_TRANSLATE_NOOP("Effects", "Mandelbrot Fractal"),
            "Render",
            {
                ParamSpec::doubleParam("factor", QT_TRANSLATE_NOOP("Effects", "Factor"),
                                       1.0, 10.0, 3.0, 0.1, 2),
                ParamSpec::doubleParam("zoom", QT_TRANSLATE_NOOP("Effects", "Zoom"),
                                       0.1, 500.0, 1.0, 0.1, 2),
                ParamSpec::doubleParam("cx", QT_TRANSLATE_NOOP("Effects", "Centre x"),
                                       -2.5, 1.5, -0.6, 0.001, 4),
                ParamSpec::doubleParam("cy", QT_TRANSLATE_NOOP("Effects", "Centre y"),
                                       -2.0, 2.0, 0.0, 0.001, 4),
                ParamSpec::intParam("quality", QT_TRANSLATE_NOOP("Effects", "Quality"),
                                    1, 5, 2),
                ParamSpec::boolParam("invert",
                                     QT_TRANSLATE_NOOP("Effects", "Invert colours"),
                                     false),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double factor = values.doubleValue("factor", 3.0);
                const double zoom = std::max(0.1, values.doubleValue("zoom", 1.0));
                const double centreX = values.doubleValue("cx", -0.6);
                const double centreY = values.doubleValue("cy", 0.0);
                const int iterations = 96 * std::clamp(values.intValue("quality", 2), 1, 5);
                const bool invert = values.boolValue("invert");
                const Image& src = *context.source;
                const double half = std::min(src.width(), src.height()) * 0.5;
                return fx::mapPixels(context, [&](Rgba, int x, int y) {
                    const double cr = centreX + (x - src.width() * 0.5) / (half * zoom) * 1.5;
                    const double ci = centreY + (y - src.height() * 0.5) / (half * zoom) * 1.5;
                    double zr = 0, zi = 0;
                    int i = 0;
                    for (; i < iterations; ++i) {
                        const double zr2 = zr * zr - zi * zi + cr;
                        zi = 2.0 * zr * zi + ci;
                        zr = zr2;
                        if (zr * zr + zi * zi > 16.0)
                            break;
                    }
                    if (i >= iterations)
                        return invert ? Rgba(255, 255, 255, 255) : Rgba(0, 0, 0, 255);
                    const double t = double(i) / iterations;
                    float r = 0, g = 0, b = 0;
                    ops::hsvToRgb(float(360.0 * std::fmod(t * factor, 1.0)), 0.8f,
                                  std::min(1.0f, float(std::sqrt(t) * 2.0)), r, g, b);
                    if (invert) {
                        r = 1.0f - r;
                        g = 1.0f - g;
                        b = 1.0f - b;
                    }
                    return Rgba(toByte(r), toByte(g), toByte(b), 255);
                });
            });

    // ------------------------------------------------------------ checker
    fx::add(registry, "checkerboard", QT_TRANSLATE_NOOP("Effects", "Checkerboard"),
            "Render",
            {
                ParamSpec::intParam("cell", QT_TRANSLATE_NOOP("Effects", "Cell size"),
                                    1, 512, 16, " px"),
                ParamSpec::choiceParam("blend",
                                       QT_TRANSLATE_NOOP("Effects", "Blend mode"),
                                       blendModeLabels(), 0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int cell = std::max(1, values.intValue("cell", 16));
                const BlendMode mode =
                    static_cast<BlendMode>(std::clamp(values.choiceValue("blend", 0), 0,
                                                      int(BlendMode::Count) - 1));
                const Image& src = *context.source;
                Image pattern = ops::colorizeChecker(src.width(), src.height(), cell,
                                                     context.primary, context.secondary);
                Image out = src;
                ops::imageOver(out, pattern, nullptr, mode,
                               context.region.intersected(src.rect()));
                return out;
            });
}

} // namespace tessel
