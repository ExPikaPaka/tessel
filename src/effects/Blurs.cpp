// The Blurs submenu.
#include "effects/Effect.h"

#include <cmath>

namespace tessel {

namespace {

// Averages the premultiplied colour of a list of sample positions.
struct Accumulator {
    double r = 0, g = 0, b = 0, a = 0, weight = 0;

    void add(Rgba c, double w = 1.0)
    {
        const double alpha = c.a * (1.0 / 255.0) * w;
        r += c.r * (1.0 / 255.0) * alpha;
        g += c.g * (1.0 / 255.0) * alpha;
        b += c.b * (1.0 / 255.0) * alpha;
        a += alpha;
        weight += w;
    }

    Rgba result() const
    {
        if (a <= 1.0e-9 || weight <= 0.0)
            return Rgba(0, 0, 0, 0);
        const double inv = 1.0 / a;
        return Rgba(toByte(float(r * inv)), toByte(float(g * inv)),
                    toByte(float(b * inv)), toByte(float(a / weight)));
    }
};

// Disc (out of focus) blur. Row prefix sums make the cost per pixel linear in
// the radius rather than quadratic.
Image discBlur(const Image& src, int radius, const Rect& region)
{
    if (radius < 1)
        return src;
    const int w = src.width(), h = src.height();
    // Prefix sums of premultiplied channels, one extra column for the offset.
    std::vector<double> sums(static_cast<size_t>(w + 1) * h * 4, 0.0);
    parallelFor(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const Rgba* line = src.row(y);
            double* out = sums.data() + static_cast<size_t>(y) * (w + 1) * 4;
            for (int c = 0; c < 4; ++c)
                out[c] = 0.0;
            for (int x = 0; x < w; ++x) {
                const double alpha = line[x].a * (1.0 / 255.0);
                const double next[4] = {line[x].r * (1.0 / 255.0) * alpha,
                                        line[x].g * (1.0 / 255.0) * alpha,
                                        line[x].b * (1.0 / 255.0) * alpha, alpha};
                for (int c = 0; c < 4; ++c)
                    out[(x + 1) * 4 + c] = out[x * 4 + c] + next[c];
            }
        }
    });

    // Half width of the disc for every row offset.
    std::vector<int> spans(static_cast<size_t>(2 * radius + 1));
    for (int dy = -radius; dy <= radius; ++dy) {
        const double inside = double(radius) * radius - double(dy) * dy;
        spans[static_cast<size_t>(dy + radius)] =
            inside <= 0.0 ? 0 : static_cast<int>(std::floor(std::sqrt(inside)));
    }

    Image out = src;
    const Rect area = region.intersected(src.rect());
    parallelFor(area.y, area.bottom(), [&](int yStart, int yEnd) {
        for (int y = yStart; y < yEnd; ++y) {
            Rgba* dst = out.row(y);
            for (int x = area.x; x < area.right(); ++x) {
                double acc[4] = {0, 0, 0, 0};
                double count = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    const int sy = std::clamp(y + dy, 0, h - 1);
                    const int span = spans[static_cast<size_t>(dy + radius)];
                    const int x0 = std::clamp(x - span, 0, w - 1);
                    const int x1 = std::clamp(x + span, 0, w - 1);
                    const double* row = sums.data() + static_cast<size_t>(sy) * (w + 1) * 4;
                    for (int c = 0; c < 4; ++c)
                        acc[c] += row[(x1 + 1) * 4 + c] - row[x0 * 4 + c];
                    count += x1 - x0 + 1;
                }
                if (count <= 0 || acc[3] <= 1.0e-9) {
                    dst[x] = Rgba(0, 0, 0, 0);
                    continue;
                }
                const double inv = 1.0 / acc[3];
                dst[x] = Rgba(toByte(float(acc[0] * inv)), toByte(float(acc[1] * inv)),
                              toByte(float(acc[2] * inv)), toByte(float(acc[3] / count)));
            }
        }
    });
    return out;
}

} // namespace

void registerBlurs(EffectRegistry& registry)
{
    // ------------------------------------------------------------ gaussian
    fx::add(registry, "gaussian_blur", "Gaussian Blur", "Blurs",
            {
                ParamSpec::doubleParam("radius", "Radius", 0.0, 200.0, 4.0, 0.5, 1, " px"),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double radius = values.doubleValue("radius", 4.0);
                if (radius <= 0.0)
                    return *context.source;
                return fx::replaceRegion(context, ops::gaussianBlur(*context.source, radius));
            });

    // ------------------------------------------------------------ unfocus
    fx::add(registry, "unfocus", "Unfocus", "Blurs",
            {
                ParamSpec::intParam("radius", "Radius", 1, 200, 6, " px"),
            },
            [](const EffectContext& context, const ParamValues& values) {
                return discBlur(*context.source, values.intValue("radius", 6),
                                context.region);
            });

    // ------------------------------------------------------------ motion blur
    fx::add(registry, "motion_blur", "Motion Blur", "Blurs",
            {
                ParamSpec::angleParam("angle", "Angle", 25.0),
                ParamSpec::intParam("distance", "Distance", 1, 200, 25, " px"),
                ParamSpec::boolParam("centered", "Centered", true),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double radians = values.doubleValue("angle", 25.0) * M_PI / 180.0;
                const int distance = std::max(1, values.intValue("distance", 25));
                const bool centered = values.boolValue("centered", true);
                const double dx = std::cos(radians);
                const double dy = -std::sin(radians);
                const int steps = std::min(256, distance);
                const Image& src = *context.source;
                Image out = src;
                const Rect area = context.region.intersected(src.rect());
                parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y) {
                        Rgba* dst = out.row(y);
                        for (int x = area.x; x < area.right(); ++x) {
                            Accumulator acc;
                            for (int i = 0; i <= steps; ++i) {
                                double t = double(i) / steps;
                                if (centered)
                                    t -= 0.5;
                                const double offset = t * distance;
                                acc.add(ops::sampleBilinear(
                                    src, float(x + dx * offset), float(y + dy * offset),
                                    ops::EdgeMode::Clamp));
                            }
                            dst[x] = acc.result();
                        }
                    }
                });
                return out;
            });

    // ------------------------------------------------------------ radial blur
    fx::add(registry, "radial_blur", "Radial Blur", "Blurs",
            {
                ParamSpec::doubleParam("amount", "Angle", 0.0, 180.0, 10.0, 0.5, 1, " deg"),
                ParamSpec::offsetParam("center", "Center", 0.0, 0.0),
                ParamSpec::intParam("quality", "Quality", 1, 8, 3),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double amount = values.doubleValue("amount", 10.0) * M_PI / 180.0;
                const QPointF center = values.pointValue("center");
                const int quality = std::clamp(values.intValue("quality", 3), 1, 8);
                const Image& src = *context.source;
                const double cx = src.width() * 0.5 * (1.0 + center.x());
                const double cy = src.height() * 0.5 * (1.0 + center.y());
                const int steps = 4 * quality;
                Image out = src;
                const Rect area = context.region.intersected(src.rect());
                parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y) {
                        Rgba* dst = out.row(y);
                        for (int x = area.x; x < area.right(); ++x) {
                            const double rx = x - cx;
                            const double ry = y - cy;
                            Accumulator acc;
                            for (int i = 0; i <= steps; ++i) {
                                const double t = double(i) / steps - 0.5;
                                const double angle = t * amount;
                                const double sa = std::sin(angle), ca = std::cos(angle);
                                acc.add(ops::sampleBilinear(
                                    src, float(cx + rx * ca - ry * sa),
                                    float(cy + rx * sa + ry * ca), ops::EdgeMode::Clamp));
                            }
                            dst[x] = acc.result();
                        }
                    }
                });
                return out;
            });

    // ------------------------------------------------------------ zoom blur
    fx::add(registry, "zoom_blur", "Zoom Blur", "Blurs",
            {
                ParamSpec::intParam("amount", "Amount", 0, 100, 10),
                ParamSpec::offsetParam("center", "Center", 0.0, 0.0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double amount = values.intValue("amount", 10) / 100.0;
                const QPointF center = values.pointValue("center");
                const Image& src = *context.source;
                const double cx = src.width() * 0.5 * (1.0 + center.x());
                const double cy = src.height() * 0.5 * (1.0 + center.y());
                const int steps = 24;
                Image out = src;
                const Rect area = context.region.intersected(src.rect());
                parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y) {
                        Rgba* dst = out.row(y);
                        for (int x = area.x; x < area.right(); ++x) {
                            Accumulator acc;
                            for (int i = 0; i <= steps; ++i) {
                                const double scale = 1.0 - amount * double(i) / steps * 0.5;
                                acc.add(ops::sampleBilinear(
                                    src, float(cx + (x - cx) * scale),
                                    float(cy + (y - cy) * scale), ops::EdgeMode::Clamp));
                            }
                            dst[x] = acc.result();
                        }
                    }
                });
                return out;
            });

    // ------------------------------------------------------------ fragment
    fx::add(registry, "fragment", "Fragment", "Blurs",
            {
                ParamSpec::intParam("fragments", "Fragments", 2, 50, 4),
                ParamSpec::intParam("distance", "Distance", 0, 200, 8, " px"),
                ParamSpec::angleParam("rotation", "Rotation", 0.0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int count = std::max(2, values.intValue("fragments", 4));
                const double distance = values.intValue("distance", 8);
                const double rotation = values.doubleValue("rotation", 0.0) * M_PI / 180.0;
                const Image& src = *context.source;
                std::vector<QPointF> offsets;
                offsets.reserve(static_cast<size_t>(count));
                for (int i = 0; i < count; ++i) {
                    const double angle = rotation + 2.0 * M_PI * i / count;
                    offsets.emplace_back(std::cos(angle) * distance,
                                         -std::sin(angle) * distance);
                }
                Image out = src;
                const Rect area = context.region.intersected(src.rect());
                parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y) {
                        Rgba* dst = out.row(y);
                        for (int x = area.x; x < area.right(); ++x) {
                            Accumulator acc;
                            for (const QPointF& offset : offsets) {
                                acc.add(ops::sampleBilinear(src, float(x + offset.x()),
                                                            float(y + offset.y()),
                                                            ops::EdgeMode::Clamp));
                            }
                            dst[x] = acc.result();
                        }
                    }
                });
                return out;
            });

    // ------------------------------------------------------------ surface blur
    fx::add(registry, "surface_blur", "Surface Blur", "Blurs",
            {
                ParamSpec::intParam("radius", "Radius", 1, 12, 6, " px"),
                ParamSpec::intParam("threshold", "Threshold", 1, 100, 25),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int radius = std::clamp(values.intValue("radius", 6), 1, 12);
                const double threshold = std::max(1, values.intValue("threshold", 25)) * 2.55;
                const double sigmaRange = 2.0 * threshold * threshold;
                const Image& src = *context.source;
                Image out = src;
                const Rect area = context.region.intersected(src.rect());
                // The widest kernel here is 25x25 per pixel, so this is one of
                // the effects that has to stay interruptible.
                fx::forEachRow(context, area.y, area.bottom(), [&](int y) {
                    Rgba* dst = out.row(y);
                    for (int x = area.x; x < area.right(); ++x) {
                        const Rgba centre = src.row(y)[x];
                        double sum[3] = {0, 0, 0};
                        double weight = 0;
                        for (int dy = -radius; dy <= radius; ++dy) {
                            const int sy = std::clamp(y + dy, 0, src.height() - 1);
                            const Rgba* line = src.row(sy);
                            for (int dx = -radius; dx <= radius; ++dx) {
                                const int sx = std::clamp(x + dx, 0, src.width() - 1);
                                const Rgba c = line[sx];
                                const double diff = std::abs(int(c.r) - int(centre.r))
                                    + std::abs(int(c.g) - int(centre.g))
                                    + std::abs(int(c.b) - int(centre.b));
                                const double w = std::exp(-(diff * diff) / sigmaRange);
                                sum[0] += c.r * w;
                                sum[1] += c.g * w;
                                sum[2] += c.b * w;
                                weight += w;
                            }
                        }
                        if (weight <= 0.0) {
                            dst[x] = centre;
                            continue;
                        }
                        dst[x] = Rgba(static_cast<uint8_t>(std::lround(sum[0] / weight)),
                                      static_cast<uint8_t>(std::lround(sum[1] / weight)),
                                      static_cast<uint8_t>(std::lround(sum[2] / weight)),
                                      centre.a);
                    }
                });
                return out;
            });
}

} // namespace tessel
