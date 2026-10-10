// The Color and Object submenus: palette reduction, and effects that work from
// the shape of whatever is drawn on the layer.
#include "effects/Effect.h"

#include <algorithm>
#include <cmath>

namespace tessel {

namespace {

struct Bucket {
    std::vector<Rgba> colors;
    int widestChannel = 0;
    int spread = 0;

    void measure()
    {
        int lo[3] = {255, 255, 255};
        int hi[3] = {0, 0, 0};
        for (const Rgba& c : colors) {
            const int channels[3] = {c.r, c.g, c.b};
            for (int i = 0; i < 3; ++i) {
                lo[i] = std::min(lo[i], channels[i]);
                hi[i] = std::max(hi[i], channels[i]);
            }
        }
        spread = 0;
        for (int i = 0; i < 3; ++i) {
            if (hi[i] - lo[i] > spread) {
                spread = hi[i] - lo[i];
                widestChannel = i;
            }
        }
    }

    Rgba average() const
    {
        if (colors.empty())
            return Rgba(0, 0, 0, 255);
        long sum[3] = {0, 0, 0};
        for (const Rgba& c : colors) {
            sum[0] += c.r;
            sum[1] += c.g;
            sum[2] += c.b;
        }
        const long n = static_cast<long>(colors.size());
        return Rgba(uint8_t(sum[0] / n), uint8_t(sum[1] / n), uint8_t(sum[2] / n), 255);
    }
};

// Median cut: split the colour cloud along its widest axis until there are as
// many boxes as wanted palette entries, then take the average of each box.
std::vector<Rgba> medianCutPalette(const Image& image, const Rect& region, int wanted)
{
    Bucket initial;
    initial.colors.reserve(4096);
    // Sampling keeps the cost flat on large images without changing the result
    // noticeably.
    const int step = std::max(1, int(std::sqrt(double(region.w) * region.h / 20000.0)));
    for (int y = region.y; y < region.bottom(); y += step) {
        const Rgba* line = image.row(y);
        for (int x = region.x; x < region.right(); x += step) {
            if (line[x].a > 8)
                initial.colors.push_back(line[x]);
        }
    }
    if (initial.colors.empty())
        return {Rgba(0, 0, 0, 255)};
    initial.measure();

    std::vector<Bucket> buckets{std::move(initial)};
    while (int(buckets.size()) < wanted) {
        auto widest = std::max_element(buckets.begin(), buckets.end(),
                                       [](const Bucket& a, const Bucket& b) {
                                           return a.spread < b.spread;
                                       });
        if (widest == buckets.end() || widest->spread == 0 || widest->colors.size() < 2)
            break;
        const int channel = widest->widestChannel;
        std::sort(widest->colors.begin(), widest->colors.end(),
                  [channel](const Rgba& a, const Rgba& b) {
                      const int lhs[3] = {a.r, a.g, a.b};
                      const int rhs[3] = {b.r, b.g, b.b};
                      return lhs[channel] < rhs[channel];
                  });
        const size_t half = widest->colors.size() / 2;
        Bucket upper;
        upper.colors.assign(widest->colors.begin() + long(half), widest->colors.end());
        widest->colors.erase(widest->colors.begin() + long(half), widest->colors.end());
        widest->measure();
        upper.measure();
        buckets.push_back(std::move(upper));
    }

    std::vector<Rgba> palette;
    palette.reserve(buckets.size());
    for (const Bucket& bucket : buckets)
        palette.push_back(bucket.average());
    return palette;
}

int nearestPaletteEntry(const std::vector<Rgba>& palette, int r, int g, int b)
{
    int best = 0;
    long bestDistance = -1;
    for (size_t i = 0; i < palette.size(); ++i) {
        const long dr = r - palette[i].r;
        const long dg = g - palette[i].g;
        const long db = b - palette[i].b;
        const long distance = dr * dr + dg * dg + db * db;
        if (bestDistance < 0 || distance < bestDistance) {
            bestDistance = distance;
            best = int(i);
        }
    }
    return best;
}

// Alpha coverage of the layer, grown by `radius` pixels. Used by the object
// effects to find the silhouette of whatever is drawn.
Mask dilatedAlpha(const Image& image, const Rect& region, int radius)
{
    Mask mask(image.width(), image.height());
    for (int y = region.y; y < region.bottom(); ++y) {
        const Rgba* line = image.row(y);
        float* out = mask.row(y);
        for (int x = region.x; x < region.right(); ++x)
            out[x] = line[x].a * (1.0f / 255.0f);
    }
    if (radius <= 0)
        return mask;
    // Circular dilation done as a separable maximum, which is close enough and
    // stays linear in the radius.
    Mask horizontal = mask;
    for (int y = region.y; y < region.bottom(); ++y) {
        const float* src = mask.row(y);
        float* dst = horizontal.row(y);
        for (int x = region.x; x < region.right(); ++x) {
            float value = 0.0f;
            const int lo = std::max(region.x, x - radius);
            const int hi = std::min(region.right() - 1, x + radius);
            for (int k = lo; k <= hi; ++k)
                value = std::max(value, src[k]);
            dst[x] = value;
        }
    }
    Mask out = horizontal;
    for (int x = region.x; x < region.right(); ++x) {
        for (int y = region.y; y < region.bottom(); ++y) {
            float value = 0.0f;
            const int lo = std::max(region.y, y - radius);
            const int hi = std::min(region.bottom() - 1, y + radius);
            for (int k = lo; k <= hi; ++k)
                value = std::max(value, horizontal.row(k)[x]);
            out.row(y)[x] = value;
        }
    }
    return out;
}

} // namespace

void registerColorEffects(EffectRegistry& registry)
{
    // ------------------------------------------------------------ quantize
    fx::add(registry, "quantize", "Quantize", "Color",
            {
                ParamSpec::intParam("colors", "Palette size", 2, 256, 16),
                ParamSpec::intParam("dither", "Dithering", 0, 8, 4),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int wanted = std::clamp(values.intValue("colors", 16), 2, 256);
                const float dither = std::clamp(values.intValue("dither", 4), 0, 8) / 8.0f;
                const Image& src = *context.source;
                const Rect area = context.region.intersected(src.rect());
                Image out = src;
                if (area.isEmpty())
                    return out;
                const std::vector<Rgba> palette = medianCutPalette(src, area, wanted);

                // Floyd and Steinberg error diffusion, which needs the rows in
                // order and so is left serial.
                std::vector<float> errorCurrent(size_t(area.w + 2) * 3, 0.0f);
                std::vector<float> errorNext(size_t(area.w + 2) * 3, 0.0f);
                for (int y = area.y; y < area.bottom(); ++y) {
                    const Rgba* line = src.row(y);
                    Rgba* dst = out.row(y);
                    std::fill(errorNext.begin(), errorNext.end(), 0.0f);
                    for (int x = area.x; x < area.right(); ++x) {
                        const size_t slot = size_t(x - area.x + 1) * 3;
                        const int r = std::clamp(int(std::lround(line[x].r + errorCurrent[slot] * dither)), 0, 255);
                        const int g = std::clamp(int(std::lround(line[x].g + errorCurrent[slot + 1] * dither)), 0, 255);
                        const int b = std::clamp(int(std::lround(line[x].b + errorCurrent[slot + 2] * dither)), 0, 255);
                        const Rgba chosen = palette[size_t(nearestPaletteEntry(palette, r, g, b))];
                        dst[x] = Rgba(chosen.r, chosen.g, chosen.b, line[x].a);
                        if (dither <= 0.0f)
                            continue;
                        const float errors[3] = {float(r - chosen.r), float(g - chosen.g),
                                                 float(b - chosen.b)};
                        for (int c = 0; c < 3; ++c) {
                            errorCurrent[slot + 3 + size_t(c)] += errors[c] * (7.0f / 16.0f);
                            errorNext[slot - 3 + size_t(c)] += errors[c] * (3.0f / 16.0f);
                            errorNext[slot + size_t(c)] += errors[c] * (5.0f / 16.0f);
                            errorNext[slot + 3 + size_t(c)] += errors[c] * (1.0f / 16.0f);
                        }
                    }
                    errorCurrent.swap(errorNext);
                }
                return out;
            });

    // ------------------------------------------------------------ palette map
    fx::add(registry, "duotone", "Duotone", "Color",
            {
                ParamSpec::colorParam("shadow", "Shadow colour", Rgba(20, 24, 70, 255)),
                ParamSpec::colorParam("highlight", "Highlight colour", Rgba(255, 220, 150, 255)),
                ParamSpec::intParam("mix", "Strength", 0, 100, 100, "%"),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const Rgba shadow = values.colorValue("shadow", Rgba(20, 24, 70, 255));
                const Rgba highlight = values.colorValue("highlight",
                                                         Rgba(255, 220, 150, 255));
                const float mix = std::clamp(values.intValue("mix", 100), 0, 100) / 100.0f;
                return fx::mapPixels(context, [&](Rgba c, int, int) {
                    const float t = ops::luminance(c);
                    const float r = (shadow.r + (highlight.r - shadow.r) * t) / 255.0f;
                    const float g = (shadow.g + (highlight.g - shadow.g) * t) / 255.0f;
                    const float b = (shadow.b + (highlight.b - shadow.b) * t) / 255.0f;
                    return Rgba(toByte(c.r / 255.0f + (r - c.r / 255.0f) * mix),
                                toByte(c.g / 255.0f + (g - c.g / 255.0f) * mix),
                                toByte(c.b / 255.0f + (b - c.b / 255.0f) * mix), c.a);
                });
            });
}

void registerObjectEffects(EffectRegistry& registry)
{
    // ------------------------------------------------------------ drop shadow
    fx::add(registry, "drop_shadow", "Drop Shadow", "Object",
            {
                ParamSpec::intParam("offset_x", "Offset X", -500, 500, 8, " px"),
                ParamSpec::intParam("offset_y", "Offset Y", -500, 500, 8, " px"),
                ParamSpec::doubleParam("blur", "Blur radius", 0.0, 200.0, 6.0, 0.5, 1, " px"),
                ParamSpec::colorParam("color", "Shadow colour", Rgba(0, 0, 0, 180)),
                ParamSpec::boolParam("keep_object", "Keep the object", true),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int dx = values.intValue("offset_x", 8);
                const int dy = values.intValue("offset_y", 8);
                const double blur = values.doubleValue("blur", 6.0);
                const Rgba color = values.colorValue("color", Rgba(0, 0, 0, 180));
                const bool keepObject = values.boolValue("keep_object", true);
                const Image& src = *context.source;

                // Paint the silhouette in the shadow colour, offset it, blur it,
                // then put the original back on top.
                Image shadow(src.width(), src.height());
                for (int y = 0; y < src.height(); ++y) {
                    const int sy = y - dy;
                    if (sy < 0 || sy >= src.height())
                        continue;
                    const Rgba* line = src.row(sy);
                    Rgba* dst = shadow.row(y);
                    for (int x = 0; x < src.width(); ++x) {
                        const int sx = x - dx;
                        if (sx < 0 || sx >= src.width())
                            continue;
                        const float coverage = line[sx].a * (1.0f / 255.0f);
                        dst[x] = Rgba(color.r, color.g, color.b,
                                      toByte(coverage * (color.a / 255.0f)));
                    }
                }
                if (blur > 0.0)
                    shadow = ops::gaussianBlur(shadow, blur);
                Image out = shadow;
                if (keepObject)
                    ops::imageOver(out, src, nullptr, BlendMode::Normal, out.rect());
                return fx::replaceRegion(context, out);
            });

    // ------------------------------------------------------------ outline object
    fx::add(registry, "outline_object", "Outline Object", "Object",
            {
                ParamSpec::intParam("thickness", "Thickness", 1, 100, 4, " px"),
                ParamSpec::colorParam("color", "Outline colour", Rgba(255, 255, 255, 255)),
                ParamSpec::doubleParam("softness", "Softness", 0.0, 20.0, 0.0, 0.5, 1, " px"),
                ParamSpec::choiceParam("position", "Position",
                                       {"Outside", "Inside"}, 0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int thickness = std::max(1, values.intValue("thickness", 4));
                const Rgba color = values.colorValue("color", Rgba(255, 255, 255, 255));
                const double softness = values.doubleValue("softness", 0.0);
                const bool inside = values.choiceValue("position", 0) == 1;
                const Image& src = *context.source;
                const Rect area = context.region.intersected(src.rect());

                Mask grown = dilatedAlpha(src, src.rect(), inside ? 0 : thickness);
                Image outline(src.width(), src.height());
                for (int y = area.y; y < area.bottom(); ++y) {
                    const Rgba* line = src.row(y);
                    const float* edge = grown.row(y);
                    Rgba* dst = outline.row(y);
                    for (int x = area.x; x < area.right(); ++x) {
                        const float own = line[x].a * (1.0f / 255.0f);
                        // Outside: the grown area minus the object itself.
                        // Inside: the object minus an eroded copy, approximated
                        // by fading the interior out.
                        const float band = inside ? std::max(0.0f, own) : std::max(0.0f, edge[x] - own);
                        if (band <= 0.0f)
                            continue;
                        dst[x] = Rgba(color.r, color.g, color.b,
                                      toByte(band * (color.a / 255.0f)));
                    }
                }
                if (softness > 0.0)
                    outline = ops::gaussianBlur(outline, softness);
                Image out = src;
                if (inside) {
                    // Draw the band on top so it hugs the inner edge.
                    ops::imageOver(out, outline, nullptr, BlendMode::Normal, area);
                } else {
                    Image combined = outline;
                    ops::imageOver(combined, src, nullptr, BlendMode::Normal, area);
                    out = fx::replaceRegion(context, combined);
                }
                return out;
            });

    // ------------------------------------------------------------ glow object
    fx::add(registry, "object_glow", "Outer Glow", "Object",
            {
                ParamSpec::doubleParam("radius", "Radius", 1.0, 200.0, 12.0, 0.5, 1, " px"),
                ParamSpec::intParam("intensity", "Intensity", 1, 400, 150, "%"),
                ParamSpec::colorParam("color", "Glow colour", Rgba(255, 220, 120, 255)),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double radius = values.doubleValue("radius", 12.0);
                const float intensity = std::max(1, values.intValue("intensity", 150))
                    / 100.0f;
                const Rgba color = values.colorValue("color", Rgba(255, 220, 120, 255));
                const Image& src = *context.source;

                Image halo(src.width(), src.height());
                for (int y = 0; y < src.height(); ++y) {
                    const Rgba* line = src.row(y);
                    Rgba* dst = halo.row(y);
                    for (int x = 0; x < src.width(); ++x) {
                        dst[x] = Rgba(color.r, color.g, color.b,
                                      toByte(line[x].a * (1.0f / 255.0f)
                                             * (color.a / 255.0f)));
                    }
                }
                halo = ops::gaussianBlur(halo, radius);
                for (size_t i = 0; i < halo.pixelCount(); ++i)
                    halo.data()[i].a = toByte(halo.data()[i].a * (1.0f / 255.0f) * intensity);
                Image out = halo;
                ops::imageOver(out, src, nullptr, BlendMode::Normal, out.rect());
                return fx::replaceRegion(context, out);
            });
}

} // namespace tessel
