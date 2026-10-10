// The Adjustments menu: tone and colour corrections. Most of them are lookup
// table based, which makes them essentially free even on large images.
#include "effects/Effect.h"

#include <cmath>

#include <QPolygonF>

namespace tessel {

namespace {

ops::Lut lutFromVariant(const QVariant& value)
{
    if (!value.isValid())
        return ops::identityLut();
    const QPolygonF polygon = value.value<QPolygonF>();
    if (polygon.size() < 2)
        return ops::identityLut();
    std::vector<QPointF> points(polygon.begin(), polygon.end());
    return ops::lutFromControlPoints(points);
}

ops::Lut combineLuts(const ops::Lut& first, const ops::Lut& second)
{
    ops::Lut out{};
    for (int i = 0; i < 256; ++i)
        out[static_cast<size_t>(i)] = second[first[static_cast<size_t>(i)]];
    return out;
}

ops::Lut levelsLut(double inBlack, double inWhite, double gamma, double outBlack,
                   double outWhite)
{
    ops::Lut lut{};
    const double span = std::max(1.0, inWhite - inBlack);
    const double invGamma = 1.0 / std::clamp(gamma, 0.01, 10.0);
    for (int i = 0; i < 256; ++i) {
        double v = (i - inBlack) / span;
        v = std::clamp(v, 0.0, 1.0);
        v = std::pow(v, invGamma);
        v = outBlack + v * (outWhite - outBlack);
        lut[static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp(v, 0.0, 255.0) + 0.5);
    }
    return lut;
}

} // namespace

void registerAdjustments(EffectRegistry& registry)
{
    // ------------------------------------------------------------ auto level
    fx::add(registry, "auto_level", "Auto Level", "Adjustments", {},
            [](const EffectContext& context, const ParamValues&) {
                const ops::Histogram histogram =
                    ops::computeHistogram(*context.source, context.clip, context.region);
                auto stretch = [&](const std::array<uint32_t, 256>& channel) {
                    const int low = histogram.percentile(channel, 0.005);
                    const int high = histogram.percentile(channel, 0.995);
                    return levelsLut(low, std::max(low + 1, high), 1.0, 0.0, 255.0);
                };
                const ops::Lut red = stretch(histogram.red);
                const ops::Lut green = stretch(histogram.green);
                const ops::Lut blue = stretch(histogram.blue);
                Image out = *context.source;
                ops::applyLutRgb(out, red, green, blue, context.region);
                return out;
            },
            "Ctrl+Shift+L");

    // ------------------------------------------------------------ black and white
    fx::add(registry, "black_and_white", "Black and White", "Adjustments", {},
            [](const EffectContext& context, const ParamValues&) {
                return fx::mapPixels(context, [](Rgba c, int, int) {
                    const uint8_t v = ops::luminance8(c);
                    return Rgba(v, v, v, c.a);
                });
            },
            "Ctrl+Shift+G");

    // ------------------------------------------------------------ brightness / contrast
    fx::add(registry, "brightness_contrast", "Brightness / Contrast", "Adjustments",
            {
                ParamSpec::intParam("brightness", "Brightness", -100, 100, 0),
                ParamSpec::intParam("contrast", "Contrast", -100, 100, 0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double brightness = values.intValue("brightness") * 2.55;
                const int contrast = values.intValue("contrast");
                // A squared factor gives the slider a usable feel at both ends.
                const double factor = contrast >= 0
                    ? std::pow((100.0 + contrast) / 100.0, 2.0)
                    : (100.0 + contrast) / 100.0;
                return fx::mapLut(context, [&](ops::Lut& r, ops::Lut& g, ops::Lut& b) {
                    for (int i = 0; i < 256; ++i) {
                        double v = (i - 127.5) * factor + 127.5 + brightness;
                        const uint8_t value =
                            static_cast<uint8_t>(std::clamp(v, 0.0, 255.0) + 0.5);
                        r[static_cast<size_t>(i)] = value;
                        g[static_cast<size_t>(i)] = value;
                        b[static_cast<size_t>(i)] = value;
                    }
                });
            },
            "Ctrl+Shift+T");

    // ------------------------------------------------------------ curves
    {
        QPolygonF identity;
        identity << QPointF(0, 0) << QPointF(255, 255);
        std::vector<ParamSpec> specs;
        ParamSpec channel = ParamSpec::choiceParam(
            "channel", "Transfer map", {"Luminosity", "Red", "Green", "Blue"}, 0);
        specs.push_back(channel);
        for (const char* id : {"curve_rgb", "curve_r", "curve_g", "curve_b"}) {
            ParamSpec spec;
            spec.type = ParamType::Curve;
            spec.id = QString::fromLatin1(id);
            spec.label = QStringLiteral("Curve");
            specs.push_back(spec);
        }
        fx::add(registry, "curves", "Curves", "Adjustments", specs,
                [](const EffectContext& context, const ParamValues& values) {
                    const ops::Lut master = lutFromVariant(values.raw("curve_rgb"));
                    ops::Lut red = combineLuts(master, lutFromVariant(values.raw("curve_r")));
                    ops::Lut green = combineLuts(master, lutFromVariant(values.raw("curve_g")));
                    ops::Lut blue = combineLuts(master, lutFromVariant(values.raw("curve_b")));
                    Image out = *context.source;
                    ops::applyLutRgb(out, red, green, blue, context.region);
                    return out;
                },
                "Ctrl+Shift+M");
    }

    // ------------------------------------------------------------ hue / saturation
    fx::add(registry, "hue_saturation", "Hue / Saturation", "Adjustments",
            {
                ParamSpec::intParam("hue", "Hue", -180, 180, 0, " deg"),
                ParamSpec::intParam("saturation", "Saturation", 0, 200, 100, "%"),
                ParamSpec::intParam("lightness", "Lightness", -100, 100, 0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const float hueShift = static_cast<float>(values.intValue("hue"));
                const float saturation = values.intValue("saturation", 100) / 100.0f;
                const int lightness = values.intValue("lightness");
                const float lightScale = lightness >= 0
                    ? 1.0f
                    : 1.0f + lightness / 100.0f;
                const float lightAdd = lightness > 0 ? lightness / 100.0f : 0.0f;
                return fx::mapPixels(context, [&](Rgba c, int, int) {
                    float h = 0, s = 0, v = 0;
                    ops::rgbToHsv(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, h, s, v);
                    h += hueShift;
                    s = std::clamp(s * saturation, 0.0f, 1.0f);
                    v = std::clamp(v * lightScale + lightAdd * (1.0f - v), 0.0f, 1.0f);
                    float r = 0, g = 0, b = 0;
                    ops::hsvToRgb(h, s, v, r, g, b);
                    return Rgba(toByte(r), toByte(g), toByte(b), c.a);
                });
            },
            "Ctrl+Shift+U");

    // ------------------------------------------------------------ invert
    fx::add(registry, "invert_colors", "Invert Colors", "Adjustments", {},
            [](const EffectContext& context, const ParamValues&) {
                return fx::mapLut(context, [](ops::Lut& r, ops::Lut& g, ops::Lut& b) {
                    for (int i = 0; i < 256; ++i) {
                        const uint8_t value = static_cast<uint8_t>(255 - i);
                        r[static_cast<size_t>(i)] = value;
                        g[static_cast<size_t>(i)] = value;
                        b[static_cast<size_t>(i)] = value;
                    }
                });
            },
            "Ctrl+Shift+I");

    // ------------------------------------------------------------ invert alpha
    fx::add(registry, "invert_alpha", "Invert Alpha", "Adjustments", {},
            [](const EffectContext& context, const ParamValues&) {
                return fx::mapPixels(context, [](Rgba c, int, int) {
                    // Opaque becomes clear and clear becomes opaque; the colour
                    // of a fully transparent pixel is kept so it can come back.
                    return Rgba(c.r, c.g, c.b, static_cast<uint8_t>(255 - c.a));
                });
            },
            "Ctrl+Alt+I");

    // ------------------------------------------------------------ levels
    fx::add(registry, "levels", "Levels", "Adjustments",
            {
                ParamSpec::intParam("in_black", "Input black", 0, 254, 0),
                ParamSpec::intParam("in_white", "Input white", 1, 255, 255),
                ParamSpec::doubleParam("gamma", "Gamma", 0.1, 10.0, 1.0, 0.01, 2),
                ParamSpec::intParam("out_black", "Output black", 0, 255, 0),
                ParamSpec::intParam("out_white", "Output white", 0, 255, 255),
                ParamSpec::boolParam("red", "Red channel", true),
                ParamSpec::boolParam("green", "Green channel", true),
                ParamSpec::boolParam("blue", "Blue channel", true),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const ops::Lut active = levelsLut(
                    values.intValue("in_black", 0),
                    std::max(values.intValue("in_black", 0) + 1, values.intValue("in_white", 255)),
                    values.doubleValue("gamma", 1.0), values.intValue("out_black", 0),
                    values.intValue("out_white", 255));
                const ops::Lut identity = ops::identityLut();
                Image out = *context.source;
                ops::applyLutRgb(out, values.boolValue("red", true) ? active : identity,
                                 values.boolValue("green", true) ? active : identity,
                                 values.boolValue("blue", true) ? active : identity,
                                 context.region);
                return out;
            },
            "Ctrl+L");

    // ------------------------------------------------------------ posterize
    fx::add(registry, "posterize", "Posterize", "Adjustments",
            {
                ParamSpec::intParam("levels", "Levels", 2, 64, 16),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int steps = std::max(2, values.intValue("levels", 16));
                return fx::mapLut(context, [&](ops::Lut& r, ops::Lut& g, ops::Lut& b) {
                    for (int i = 0; i < 256; ++i) {
                        const int band = (i * steps) / 256;
                        const uint8_t value = static_cast<uint8_t>(
                            std::lround(band * 255.0 / (steps - 1)));
                        r[static_cast<size_t>(i)] = value;
                        g[static_cast<size_t>(i)] = value;
                        b[static_cast<size_t>(i)] = value;
                    }
                });
            },
            "Ctrl+Shift+P");

    // ------------------------------------------------------------ sepia
    fx::add(registry, "sepia", "Sepia", "Adjustments", {},
            [](const EffectContext& context, const ParamValues&) {
                return fx::mapPixels(context, [](Rgba c, int, int) {
                    const float v = ops::luminance(c);
                    return Rgba(toByte(std::min(1.0f, v * 1.07f)),
                                toByte(v * 0.88f), toByte(v * 0.68f), c.a);
                });
            },
            "Ctrl+Shift+E");

    // ------------------------------------------------------------ colour balance
    fx::add(registry, "color_balance", "Color Balance", "Adjustments",
            {
                ParamSpec::intParam("red", "Red", -100, 100, 0),
                ParamSpec::intParam("green", "Green", -100, 100, 0),
                ParamSpec::intParam("blue", "Blue", -100, 100, 0),
                ParamSpec::choiceParam("tones", "Affect", {"Shadows", "Midtones", "Highlights", "All tones"}, 3),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double red = values.intValue("red") / 100.0;
                const double green = values.intValue("green") / 100.0;
                const double blue = values.intValue("blue") / 100.0;
                const int tones = values.choiceValue("tones", 3);
                auto weight = [tones](float v) -> float {
                    switch (tones) {
                    case 0: return std::clamp(1.0f - v * 1.6f, 0.0f, 1.0f);
                    case 1: return 1.0f - std::abs(v - 0.5f) * 2.0f;
                    case 2: return std::clamp((v - 0.375f) * 1.6f, 0.0f, 1.0f);
                    default: return 1.0f;
                    }
                };
                return fx::mapPixels(context, [&](Rgba c, int, int) {
                    const float v = ops::luminance(c);
                    const float w = weight(v) * 64.0f;
                    return Rgba(toByte((c.r + red * w) / 255.0f),
                                toByte((c.g + green * w) / 255.0f),
                                toByte((c.b + blue * w) / 255.0f), c.a);
                });
            });

    // ------------------------------------------------------------ temperature
    fx::add(registry, "temperature", "Color Temperature", "Adjustments",
            {
                ParamSpec::intParam("warmth", "Warmth", -100, 100, 0),
                ParamSpec::intParam("tint", "Tint", -100, 100, 0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double warmth = values.intValue("warmth") / 100.0;
                const double tint = values.intValue("tint") / 100.0;
                return fx::mapLut(context, [&](ops::Lut& r, ops::Lut& g, ops::Lut& b) {
                    for (int i = 0; i < 256; ++i) {
                        const double v = i / 255.0;
                        const double shade = 4.0 * v * (1.0 - v) + 0.25;
                        r[static_cast<size_t>(i)] = toByte(float(v + warmth * 0.18 * shade));
                        g[static_cast<size_t>(i)] = toByte(float(v + tint * 0.12 * shade));
                        b[static_cast<size_t>(i)] = toByte(float(v - warmth * 0.18 * shade));
                    }
                });
            });

    // ------------------------------------------------------------ threshold
    fx::add(registry, "threshold", "Threshold", "Adjustments",
            {
                ParamSpec::intParam("level", "Level", 0, 255, 128),
                ParamSpec::boolParam("keep_color", "Keep colour", false),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int level = values.intValue("level", 128);
                const bool keepColor = values.boolValue("keep_color");
                return fx::mapPixels(context, [&](Rgba c, int, int) {
                    const bool on = ops::luminance8(c) >= level;
                    if (keepColor)
                        return on ? c : Rgba(0, 0, 0, c.a);
                    const uint8_t v = on ? 255 : 0;
                    return Rgba(v, v, v, c.a);
                });
            });
}

} // namespace tessel
