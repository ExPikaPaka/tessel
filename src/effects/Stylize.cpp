// The Stylize and Artistic submenus.
#include "effects/Effect.h"

#include <array>
#include <cmath>

namespace tessel {

namespace {

// Applies a 3x3 kernel to the luminance-weighted colour channels.
template <class F>
Image convolve3x3(const EffectContext& context, const std::array<float, 9>& kernel,
                  F&& finish)
{
    const Image& src = *context.source;
    Image out = src;
    const Rect area = context.region.intersected(src.rect());
    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* dst = out.row(y);
            for (int x = area.x; x < area.right(); ++x) {
                float acc[3] = {0, 0, 0};
                for (int ky = -1; ky <= 1; ++ky) {
                    for (int kx = -1; kx <= 1; ++kx) {
                        const float weight = kernel[static_cast<size_t>((ky + 1) * 3 + kx + 1)];
                        if (weight == 0.0f)
                            continue;
                        const Rgba c = src.atClamped(x + kx, y + ky);
                        acc[0] += c.r * weight;
                        acc[1] += c.g * weight;
                        acc[2] += c.b * weight;
                    }
                }
                dst[x] = finish(acc, src.row(y)[x]);
            }
        }
    });
    return out;
}

float sobelMagnitude(const Image& src, int x, int y, float& gx, float& gy)
{
    static const int kx[9] = {-1, 0, 1, -2, 0, 2, -1, 0, 1};
    static const int ky[9] = {-1, -2, -1, 0, 0, 0, 1, 2, 1};
    gx = gy = 0.0f;
    int index = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx, ++index) {
            const float v = ops::luminance(src.atClamped(x + dx, y + dy));
            gx += v * kx[index];
            gy += v * ky[index];
        }
    }
    return std::hypot(gx, gy);
}

} // namespace

void registerStylize(EffectRegistry& registry)
{
    // ------------------------------------------------------------ edge detect
    fx::add(registry, "edge_detect", QT_TRANSLATE_NOOP("Effects", "Edge Detect"),
            "Stylize",
            {
                ParamSpec::angleParam("angle", QT_TRANSLATE_NOOP("Effects", "Angle"),
                                      45.0),
                ParamSpec::doubleParam("strength",
                                       QT_TRANSLATE_NOOP("Effects", "Strength"),
                                       0.1, 8.0, 1.5, 0.1, 1),
                ParamSpec::boolParam("colored",
                                     QT_TRANSLATE_NOOP("Effects", "Keep colour"),
                                     false),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double radians = values.doubleValue("angle", 45.0) * M_PI / 180.0;
                const float strength = float(values.doubleValue("strength", 1.5));
                const bool colored = values.boolValue("colored");
                const float dirX = float(std::cos(radians));
                const float dirY = float(-std::sin(radians));
                const Image& src = *context.source;
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    float gx = 0, gy = 0;
                    sobelMagnitude(src, x, y, gx, gy);
                    // Project the gradient onto the chosen direction.
                    const float directional = std::abs(gx * dirX + gy * dirY) * strength;
                    const uint8_t v = toByte(directional);
                    if (!colored)
                        return Rgba(v, v, v, c.a);
                    const float scale = std::clamp(directional, 0.0f, 1.0f);
                    return Rgba(toByte(c.r / 255.0f * scale), toByte(c.g / 255.0f * scale),
                                toByte(c.b / 255.0f * scale), c.a);
                });
            });

    // ------------------------------------------------------------ emboss
    fx::add(registry, "emboss", QT_TRANSLATE_NOOP("Effects", "Emboss"), "Stylize",
            {
                ParamSpec::angleParam("angle", QT_TRANSLATE_NOOP("Effects", "Angle"),
                                      0.0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double radians = values.doubleValue("angle", 0.0) * M_PI / 180.0;
                const float dx = float(std::cos(radians));
                const float dy = float(-std::sin(radians));
                std::array<float, 9> kernel{};
                int index = 0;
                for (int ky = -1; ky <= 1; ++ky) {
                    for (int kx = -1; kx <= 1; ++kx, ++index) {
                        kernel[static_cast<size_t>(index)] =
                            -(kx * dx + ky * dy) * (kx == 0 && ky == 0 ? 0.0f : 1.0f);
                    }
                }
                return convolve3x3(context, kernel, [](const float acc[3], Rgba c) {
                    const float grey = (acc[0] * 0.299f + acc[1] * 0.587f + acc[2] * 0.114f)
                            / 255.0f
                        + 0.5f;
                    const uint8_t v = toByte(grey);
                    return Rgba(v, v, v, c.a);
                });
            });

    // ------------------------------------------------------------ relief
    fx::add(registry, "relief", QT_TRANSLATE_NOOP("Effects", "Relief"), "Stylize",
            {
                ParamSpec::angleParam("angle", QT_TRANSLATE_NOOP("Effects", "Angle"),
                                      45.0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double radians = values.doubleValue("angle", 45.0) * M_PI / 180.0;
                const float dx = float(std::cos(radians));
                const float dy = float(-std::sin(radians));
                std::array<float, 9> kernel{};
                int index = 0;
                for (int ky = -1; ky <= 1; ++ky) {
                    for (int kx = -1; kx <= 1; ++kx, ++index) {
                        float weight = -(kx * dx + ky * dy);
                        if (kx == 0 && ky == 0)
                            weight = 1.0f;
                        kernel[static_cast<size_t>(index)] = weight;
                    }
                }
                return convolve3x3(context, kernel, [](const float acc[3], Rgba c) {
                    return Rgba(toByte(acc[0] / 255.0f), toByte(acc[1] / 255.0f),
                                toByte(acc[2] / 255.0f), c.a);
                });
            });

    // ------------------------------------------------------------ outline
    fx::add(registry, "outline", QT_TRANSLATE_NOOP("Effects", "Outline"), "Stylize",
            {
                ParamSpec::intParam("thickness",
                                    QT_TRANSLATE_NOOP("Effects", "Thickness"),
                                    1, 50, 3, " px"),
                ParamSpec::intParam("intensity",
                                    QT_TRANSLATE_NOOP("Effects", "Intensity"),
                                    0, 100, 50),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int thickness = std::max(1, values.intValue("thickness", 3));
                const float intensity = values.intValue("intensity", 50) / 50.0f;
                const Image& src = *context.source;
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    // Morphological gradient: brightest minus darkest nearby.
                    float low = 1.0f, high = 0.0f;
                    for (int dy = -thickness; dy <= thickness; ++dy) {
                        for (int dx = -thickness; dx <= thickness; ++dx) {
                            if (dx * dx + dy * dy > thickness * thickness)
                                continue;
                            const float v = ops::luminance(src.atClamped(x + dx, y + dy));
                            low = std::min(low, v);
                            high = std::max(high, v);
                        }
                    }
                    const uint8_t v = toByte(1.0f - (high - low) * intensity);
                    return Rgba(v, v, v, c.a);
                });
            });

    // ------------------------------------------------------------ oil painting
    fx::add(registry, "oil_painting", QT_TRANSLATE_NOOP("Effects", "Oil Painting"),
            "Artistic",
            {
                ParamSpec::intParam("brush", QT_TRANSLATE_NOOP("Effects", "Brush size"),
                                    1, 8, 3, " px"),
                ParamSpec::intParam("coarseness",
                                    QT_TRANSLATE_NOOP("Effects", "Coarseness"),
                                    2, 255, 50),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int radius = std::clamp(values.intValue("brush", 3), 1, 8);
                const int bins = std::clamp(values.intValue("coarseness", 50), 2, 255);
                const Image& src = *context.source;
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    // Classic intensity histogram painting: take the average
                    // colour of the most common brightness bucket nearby.
                    std::array<int, 256> count{};
                    std::array<int, 256> sumR{}, sumG{}, sumB{};
                    for (int dy = -radius; dy <= radius; ++dy) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const Rgba s = src.atClamped(x + dx, y + dy);
                            const int bin = int(ops::luminance(s) * (bins - 1) + 0.5f);
                            const size_t slot = static_cast<size_t>(std::clamp(bin, 0, 255));
                            ++count[slot];
                            sumR[slot] += s.r;
                            sumG[slot] += s.g;
                            sumB[slot] += s.b;
                        }
                    }
                    size_t best = 0;
                    for (size_t i = 1; i < 256; ++i) {
                        if (count[i] > count[best])
                            best = i;
                    }
                    if (count[best] == 0)
                        return c;
                    const int n = count[best];
                    return Rgba(static_cast<uint8_t>(sumR[best] / n),
                                static_cast<uint8_t>(sumG[best] / n),
                                static_cast<uint8_t>(sumB[best] / n), c.a);
                });
            });

    // ------------------------------------------------------------ pencil sketch
    fx::add(registry, "pencil_sketch", QT_TRANSLATE_NOOP("Effects", "Pencil Sketch"),
            "Artistic",
            {
                ParamSpec::intParam("tip",
                                    QT_TRANSLATE_NOOP("Effects", "Pencil tip size"),
                                    1, 20, 2, " px"),
                ParamSpec::intParam("range",
                                    QT_TRANSLATE_NOOP("Effects", "Colour range"),
                                    -20, 20, 0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int tip = std::max(1, values.intValue("tip", 2));
                const double range = values.intValue("range", 0) / 20.0;
                const Image& src = *context.source;
                // Colour dodge of the grey image by its blurred negative, the
                // standard way of turning a photo into pencil shading.
                Image grey = src;
                for (int y = 0; y < grey.height(); ++y) {
                    Rgba* line = grey.row(y);
                    for (int x = 0; x < grey.width(); ++x) {
                        const uint8_t v = static_cast<uint8_t>(255 - ops::luminance8(line[x]));
                        line[x] = Rgba(v, v, v, line[x].a);
                    }
                }
                const Image blurred = ops::gaussianBlur(grey, tip * 2.0);
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    const float base = ops::luminance(c);
                    const float invBlur = 1.0f - ops::luminance(blurred.row(y)[x]);
                    float dodged = invBlur >= 0.999f ? 1.0f
                                                     : std::min(1.0f, base / (1.0f - invBlur));
                    dodged = std::clamp(float(dodged + range * 0.3), 0.0f, 1.0f);
                    const uint8_t v = toByte(dodged);
                    return Rgba(v, v, v, c.a);
                });
            });

    // ------------------------------------------------------------ ink sketch
    fx::add(registry, "ink_sketch", QT_TRANSLATE_NOOP("Effects", "Ink Sketch"),
            "Artistic",
            {
                ParamSpec::intParam("ink", QT_TRANSLATE_NOOP("Effects", "Ink outline"),
                                    0, 99, 50),
                ParamSpec::intParam("coloring",
                                    QT_TRANSLATE_NOOP("Effects", "Colouring"),
                                    0, 100, 50),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const float ink = values.intValue("ink", 50) / 50.0f;
                const float coloring = values.intValue("coloring", 50) / 100.0f;
                const Image& src = *context.source;
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    float gx = 0, gy = 0;
                    const float edge = sobelMagnitude(src, x, y, gx, gy) * ink;
                    const float darkness = std::clamp(1.0f - edge, 0.0f, 1.0f);
                    // Posterised colour under the ink lines.
                    const float levels = 5.0f;
                    auto quantise = [&](uint8_t v) {
                        const float f = std::round(v / 255.0f * levels) / levels;
                        return f * coloring + (v / 255.0f) * (1.0f - coloring);
                    };
                    return Rgba(toByte(quantise(c.r) * darkness),
                                toByte(quantise(c.g) * darkness),
                                toByte(quantise(c.b) * darkness), c.a);
                });
            });

    // ------------------------------------------------------------ mosaic tiles
    fx::add(registry, "mosaic", QT_TRANSLATE_NOOP("Effects", "Mosaic Tiles"),
            "Artistic",
            {
                ParamSpec::intParam("tile", QT_TRANSLATE_NOOP("Effects", "Tile size"),
                                    2, 100, 10, " px"),
                ParamSpec::intParam("gap", QT_TRANSLATE_NOOP("Effects", "Grout width"),
                                    0, 20, 1, " px"),
                ParamSpec::colorParam("grout",
                                      QT_TRANSLATE_NOOP("Effects", "Grout colour"),
                                      Rgba(24, 24, 28, 255)),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int tile = std::max(2, values.intValue("tile", 10));
                const int gap = std::clamp(values.intValue("gap", 1), 0, tile / 2);
                const Rgba grout = values.colorValue("grout", Rgba(24, 24, 28, 255));
                const Image& src = *context.source;
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    const int lx = x % tile, ly = y % tile;
                    if (lx < gap || ly < gap)
                        return Rgba(grout.r, grout.g, grout.b, c.a);
                    const int cx = (x / tile) * tile + tile / 2;
                    const int cy = (y / tile) * tile + tile / 2;
                    const Rgba centre = src.atClamped(cx, cy);
                    // A soft highlight across each tile reads as glazed stone.
                    const float shade = 0.85f + 0.3f * (1.0f - float(lx + ly) / (2.0f * tile));
                    return Rgba(toByte(centre.r / 255.0f * shade),
                                toByte(centre.g / 255.0f * shade),
                                toByte(centre.b / 255.0f * shade), centre.a);
                });
            });
}

} // namespace tessel
