// The Photo submenu.
#include "effects/Effect.h"

#include <cmath>

namespace tessel {

void registerPhoto(EffectRegistry& registry)
{
    // ------------------------------------------------------------ sharpen
    fx::add(registry, "sharpen", "Sharpen", "Photo",
            {
                ParamSpec::doubleParam("amount", "Amount", 0.0, 20.0, 2.0, 0.1, 1),
                ParamSpec::doubleParam("radius", "Radius", 0.5, 20.0, 1.5, 0.1, 1, " px"),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const float amount = float(values.doubleValue("amount", 2.0));
                const double radius = values.doubleValue("radius", 1.5);
                const Image& src = *context.source;
                const Image blurred = ops::gaussianBlur(src, radius);
                // Unsharp mask: add back the difference from a blurred copy.
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    const Rgba b = blurred.row(y)[x];
                    return Rgba(toByte((c.r + (c.r - b.r) * amount) / 255.0f),
                                toByte((c.g + (c.g - b.g) * amount) / 255.0f),
                                toByte((c.b + (c.b - b.b) * amount) / 255.0f), c.a);
                });
            });

    // ------------------------------------------------------------ glow
    fx::add(registry, "glow", "Glow", "Photo",
            {
                ParamSpec::intParam("radius", "Radius", 1, 200, 6, " px"),
                ParamSpec::intParam("brightness", "Brightness", -100, 100, 10),
                ParamSpec::intParam("contrast", "Contrast", -100, 100, 10),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double radius = std::max(1, values.intValue("radius", 6));
                const double brightness = values.intValue("brightness", 10) / 100.0;
                const double contrast = values.intValue("contrast", 10) / 100.0;
                const Image& src = *context.source;
                const Image blurred = ops::gaussianBlur(src, radius);
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    const Rgba b = blurred.row(y)[x];
                    auto screen = [&](uint8_t base, uint8_t light) {
                        float lb = light / 255.0f;
                        lb = std::clamp(float((lb - 0.5) * (1.0 + contrast) + 0.5 + brightness),
                                        0.0f, 1.0f);
                        const float cb = base / 255.0f;
                        return toByte(1.0f - (1.0f - cb) * (1.0f - lb));
                    };
                    return Rgba(screen(c.r, b.r), screen(c.g, b.g), screen(c.b, b.b), c.a);
                });
            });

    // ------------------------------------------------------------ vignette
    fx::add(registry, "vignette", "Vignette", "Photo",
            {
                ParamSpec::doubleParam("radius", "Radius", 0.1, 2.0, 0.6, 0.02, 2),
                ParamSpec::doubleParam("amount", "Amount", 0.0, 2.0, 1.0, 0.05, 2),
                ParamSpec::doubleParam("softness", "Softness", 0.05, 2.0, 0.7, 0.05, 2),
                ParamSpec::offsetParam("center", "Centre", 0.0, 0.0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double radius = values.doubleValue("radius", 0.6);
                const double amount = values.doubleValue("amount", 1.0);
                const double softness = std::max(0.05, values.doubleValue("softness", 0.7));
                const QPointF offset = values.pointValue("center");
                const Image& src = *context.source;
                const double cx = src.width() * 0.5 * (1.0 + offset.x());
                const double cy = src.height() * 0.5 * (1.0 + offset.y());
                const double half = std::hypot(src.width(), src.height()) * 0.5;
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    const double r = std::hypot(x - cx, y - cy) / half;
                    const double t = std::clamp((r - radius) / softness, 0.0, 1.0);
                    const float shade = float(std::clamp(1.0 - t * t * amount, 0.0, 1.0));
                    return Rgba(toByte(c.r / 255.0f * shade), toByte(c.g / 255.0f * shade),
                                toByte(c.b / 255.0f * shade), c.a);
                });
            });

    // ------------------------------------------------------------ soft portrait
    fx::add(registry, "soft_portrait", "Soft Portrait", "Photo",
            {
                ParamSpec::intParam("softness", "Softness", 0, 10, 5),
                ParamSpec::intParam("lighting", "Lighting", -20, 20, 0),
                ParamSpec::intParam("warmth", "Warmth", 0, 20, 10),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int softness = std::clamp(values.intValue("softness", 5), 0, 10);
                const double lighting = values.intValue("lighting", 0) / 20.0;
                const double warmth = values.intValue("warmth", 10) / 20.0;
                const Image& src = *context.source;
                const Image blurred = ops::gaussianBlur(src, 2.0 + softness * 2.0);
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    const Rgba b = blurred.row(y)[x];
                    // Screen the blurred copy back in, then warm the result.
                    auto mix = [&](uint8_t base, uint8_t soft, double tint) {
                        const float cb = base / 255.0f;
                        const float sb = soft / 255.0f;
                        float v = 1.0f - (1.0f - cb) * (1.0f - sb * 0.55f);
                        v = float(v * (1.0 + lighting * 0.5) + tint * 0.08);
                        return toByte(v);
                    };
                    return Rgba(mix(c.r, b.r, warmth), mix(c.g, b.g, warmth * 0.4),
                                mix(c.b, b.b, -warmth * 0.5), c.a);
                });
            });

    // ------------------------------------------------------------ red eye
    fx::add(registry, "red_eye", "Red Eye Removal", "Photo",
            {
                ParamSpec::intParam("tolerance", "Tolerance", 0, 100, 70),
                ParamSpec::intParam("saturation", "Saturation", 0, 100, 90),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const float tolerance = values.intValue("tolerance", 70) / 100.0f;
                const float saturation = values.intValue("saturation", 90) / 100.0f;
                return fx::mapPixels(context, [&](Rgba c, int, int) {
                    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
                    const float other = std::max(g, b);
                    if (r <= other || (r - other) < (1.0f - tolerance) * 0.5f)
                        return c;
                    // Pull the red channel down towards the other two.
                    const float target = (g + b) * 0.5f;
                    const float newR = r + (target - r) * saturation;
                    return Rgba(toByte(newR), c.g, c.b, c.a);
                });
            });
}

} // namespace tessel
