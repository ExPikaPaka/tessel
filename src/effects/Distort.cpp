// The Distort submenu: geometric warps and cell based distortions.
#include "effects/Effect.h"

#include <cmath>

namespace tessel {

namespace {

ops::EdgeMode edgeFromChoice(int choice)
{
    switch (choice) {
    case 1: return ops::EdgeMode::Wrap;
    case 2: return ops::EdgeMode::Reflect;
    case 3: return ops::EdgeMode::Transparent;
    default: return ops::EdgeMode::Clamp;
    }
}

ParamSpec edgeParam()
{
    return ParamSpec::choiceParam("edge", "Edge behaviour",
                                  {"Clamp", "Wrap", "Reflect", "Transparent"}, 0);
}

// Shared warp driver: `fn` maps a destination pixel to a source position.
template <class F>
Image warp(const EffectContext& context, ops::EdgeMode edge, F&& fn)
{
    const Image& src = *context.source;
    Image out = src;
    const Rect area = context.region.intersected(src.rect());
    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* dst = out.row(y);
            for (int x = area.x; x < area.right(); ++x) {
                float sx = float(x), sy = float(y);
                fn(x, y, sx, sy);
                dst[x] = ops::sampleBilinear(src, sx, sy, edge);
            }
        }
    });
    return out;
}

} // namespace

void registerDistortions(EffectRegistry& registry)
{
    // ------------------------------------------------------------ bulge
    fx::add(registry, "bulge", "Bulge", "Distort",
            {
                ParamSpec::intParam("amount", "Amount", -200, 100, 45, "%"),
                ParamSpec::offsetParam("center", "Offset", 0.0, 0.0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double amount = values.intValue("amount", 45) / 100.0;
                const QPointF offset = values.pointValue("center");
                const Image& src = *context.source;
                const double cx = src.width() * 0.5 * (1.0 + offset.x());
                const double cy = src.height() * 0.5 * (1.0 + offset.y());
                const double maxRadius = std::min(src.width(), src.height()) * 0.5;
                return warp(context, ops::EdgeMode::Clamp,
                            [&](int x, int y, float& sx, float& sy) {
                                const double dx = x - cx, dy = y - cy;
                                const double r = std::hypot(dx, dy);
                                if (r >= maxRadius || r < 1.0e-6)
                                    return;
                                const double t = r / maxRadius;
                                const double factor = 1.0 - amount * std::pow(1.0 - t, 2.0);
                                sx = float(cx + dx * factor);
                                sy = float(cy + dy * factor);
                            });
            });

    // ------------------------------------------------------------ twist
    fx::add(registry, "twist", "Twist", "Distort",
            {
                ParamSpec::doubleParam("amount", "Amount", -200.0, 200.0, 30.0, 1.0, 1),
                ParamSpec::intParam("size", "Size", 1, 200, 100, "%"),
                ParamSpec::offsetParam("center", "Offset", 0.0, 0.0),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double amount = values.doubleValue("amount", 30.0) * M_PI / 180.0;
                const double size = std::max(1, values.intValue("size", 100)) / 100.0;
                const QPointF offset = values.pointValue("center");
                const Image& src = *context.source;
                const double cx = src.width() * 0.5 * (1.0 + offset.x());
                const double cy = src.height() * 0.5 * (1.0 + offset.y());
                const double maxRadius = std::min(src.width(), src.height()) * 0.5 * size;
                return warp(context, ops::EdgeMode::Clamp,
                            [&](int x, int y, float& sx, float& sy) {
                                const double dx = x - cx, dy = y - cy;
                                const double r = std::hypot(dx, dy);
                                if (r >= maxRadius || r < 1.0e-6)
                                    return;
                                const double t = 1.0 - r / maxRadius;
                                const double angle = amount * t * t;
                                const double ca = std::cos(angle), sa = std::sin(angle);
                                sx = float(cx + dx * ca - dy * sa);
                                sy = float(cy + dx * sa + dy * ca);
                            });
            });

    // ------------------------------------------------------------ polar inversion
    fx::add(registry, "polar_inversion", "Polar Inversion", "Distort",
            {
                ParamSpec::doubleParam("amount", "Amount", -4.0, 4.0, 0.0, 0.05, 2),
                ParamSpec::offsetParam("center", "Offset", 0.0, 0.0),
                edgeParam(),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double amount = values.doubleValue("amount", 0.0);
                const QPointF offset = values.pointValue("center");
                const Image& src = *context.source;
                const double half = std::min(src.width(), src.height()) * 0.5;
                const double cx = src.width() * 0.5 * (1.0 + offset.x());
                const double cy = src.height() * 0.5 * (1.0 + offset.y());
                const ops::EdgeMode edge = edgeFromChoice(values.choiceValue("edge", 0));
                return warp(context, edge, [&](int x, int y, float& sx, float& sy) {
                    const double u = (x - cx) / half;
                    const double v = (y - cy) / half;
                    const double r2 = u * u + v * v;
                    if (r2 < 1.0e-9)
                        return;
                    const double scale = std::pow(r2, -amount * 0.5);
                    sx = float(cx + u * scale * half);
                    sy = float(cy + v * scale * half);
                });
            });

    // ------------------------------------------------------------ tile reflection
    fx::add(registry, "tile_reflection", "Tile Reflection", "Distort",
            {
                ParamSpec::angleParam("rotation", "Rotation", 30.0),
                ParamSpec::intParam("size", "Tile size", 2, 400, 48, " px"),
                ParamSpec::doubleParam("curvature", "Curvature", -100.0, 100.0, 8.0, 0.5, 1),
                edgeParam(),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double rotation = values.doubleValue("rotation", 30.0) * M_PI / 180.0;
                const double tile = std::max(2, values.intValue("size", 48));
                const double curvature = values.doubleValue("curvature", 8.0);
                const ops::EdgeMode edge = edgeFromChoice(values.choiceValue("edge", 0));
                const Image& src = *context.source;
                const double cx = src.width() * 0.5, cy = src.height() * 0.5;
                const double ca = std::cos(rotation), sa = std::sin(rotation);
                const double scale = M_PI * 2.0 / tile;
                return warp(context, edge, [&](int x, int y, float& sx, float& sy) {
                    const double dx = x - cx, dy = y - cy;
                    // Work in the rotated frame, bend, then rotate back.
                    const double u = dx * ca + dy * sa;
                    const double v = -dx * sa + dy * ca;
                    const double bu = u + curvature * std::sin(v * scale);
                    const double bv = v + curvature * std::sin(u * scale);
                    sx = float(cx + bu * ca - bv * sa);
                    sy = float(cy + bu * sa + bv * ca);
                });
            });

    // ------------------------------------------------------------ dents
    fx::add(registry, "dents", "Dents", "Distort",
            {
                ParamSpec::doubleParam("scale", "Scale", 1.0, 200.0, 25.0, 1.0, 1),
                ParamSpec::doubleParam("refraction", "Refraction", 0.0, 100.0, 12.0, 0.5, 1),
                ParamSpec::doubleParam("roughness", "Roughness", 0.05, 0.95, 0.5, 0.05, 2),
                ParamSpec::intParam("detail", "Detail", 1, 6, 3),
                ParamSpec::intParam("seed", "Seed", 0, 999, 17),
                edgeParam(),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double scale = std::max(1.0, values.doubleValue("scale", 25.0));
                const double refraction = values.doubleValue("refraction", 12.0);
                const float roughness = float(values.doubleValue("roughness", 0.5));
                const int detail = values.intValue("detail", 3);
                const unsigned seed = unsigned(values.intValue("seed", 17)) * 7919u + 13u;
                const ops::EdgeMode edge = edgeFromChoice(values.choiceValue("edge", 0));
                return warp(context, edge, [&](int x, int y, float& sx, float& sy) {
                    const float fx = float(x / scale), fy = float(y / scale);
                    const float nx = fx::fractalNoise(fx, fy, seed, detail, roughness) - 0.5f;
                    const float ny = fx::fractalNoise(fx + 31.7f, fy - 17.3f, seed + 977u,
                                                      detail, roughness) - 0.5f;
                    sx = float(x + nx * 2.0 * refraction);
                    sy = float(y + ny * 2.0 * refraction);
                });
            });

    // ------------------------------------------------------------ frosted glass
    fx::add(registry, "frosted_glass", "Frosted Glass", "Distort",
            {
                ParamSpec::intParam("scatter", "Maximum scatter", 1, 100, 4, " px"),
                ParamSpec::intParam("seed", "Seed", 0, 999, 42),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const double scatter = std::max(1, values.intValue("scatter", 4));
                const unsigned seed = unsigned(values.intValue("seed", 42)) * 2654435761u + 1u;
                return warp(context, ops::EdgeMode::Reflect,
                            [&](int x, int y, float& sx, float& sy) {
                                const float a = fx::hashFloat(unsigned(x), unsigned(y), seed)
                                    * 2.0f * float(M_PI);
                                const float r = fx::hashFloat(unsigned(x), unsigned(y),
                                                              seed + 7777u)
                                    * float(scatter);
                                sx = float(x + std::cos(a) * r);
                                sy = float(y + std::sin(a) * r);
                            });
            });

    // ------------------------------------------------------------ pixelate
    fx::add(registry, "pixelate", "Pixelate", "Distort",
            {
                ParamSpec::intParam("cell", "Cell size", 2, 200, 6, " px"),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int cell = std::max(2, values.intValue("cell", 6));
                const Image& src = *context.source;
                Image out = src;
                const Rect area = context.region.intersected(src.rect());
                // Snap cells to the canvas grid so the result does not shift
                // when a selection is used.
                const int firstCell = area.y / cell;
                const int lastCell = (area.bottom() - 1) / cell;
                parallelFor(firstCell, lastCell + 1, [&](int cy0, int cy1) {
                    for (int cy = cy0; cy < cy1; ++cy) {
                        const int y0 = std::max(area.y, cy * cell);
                        const int y1 = std::min(area.bottom(), (cy + 1) * cell);
                        for (int cx = area.x / cell; cx <= (area.right() - 1) / cell; ++cx) {
                            const int x0 = std::max(area.x, cx * cell);
                            const int x1 = std::min(area.right(), (cx + 1) * cell);
                            if (x1 <= x0 || y1 <= y0)
                                continue;
                            double acc[4] = {0, 0, 0, 0};
                            int count = 0;
                            for (int y = cy * cell; y < (cy + 1) * cell; ++y) {
                                const int sy = std::clamp(y, 0, src.height() - 1);
                                const Rgba* line = src.row(sy);
                                for (int x = cx * cell; x < (cx + 1) * cell; ++x) {
                                    const Rgba c = line[std::clamp(x, 0, src.width() - 1)];
                                    const double alpha = c.a * (1.0 / 255.0);
                                    acc[0] += c.r * alpha;
                                    acc[1] += c.g * alpha;
                                    acc[2] += c.b * alpha;
                                    acc[3] += alpha;
                                    ++count;
                                }
                            }
                            Rgba value(0, 0, 0, 0);
                            if (acc[3] > 1.0e-9) {
                                value = Rgba(static_cast<uint8_t>(std::lround(acc[0] / acc[3])),
                                             static_cast<uint8_t>(std::lround(acc[1] / acc[3])),
                                             static_cast<uint8_t>(std::lround(acc[2] / acc[3])),
                                             toByte(float(acc[3] / count)));
                            }
                            for (int y = y0; y < y1; ++y)
                                std::fill(out.row(y) + x0, out.row(y) + x1, value);
                        }
                    }
                });
                return out;
            });

    // ------------------------------------------------------------ crystalize
    fx::add(registry, "crystalize", "Crystalize", "Distort",
            {
                ParamSpec::intParam("cell", "Cell size", 2, 200, 8, " px"),
                ParamSpec::intParam("seed", "Seed", 0, 999, 5),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int cell = std::max(2, values.intValue("cell", 8));
                const unsigned seed = unsigned(values.intValue("seed", 5)) * 40503u + 7u;
                const Image& src = *context.source;
                auto cellCenter = [&](int gx, int gy, double& px, double& py) {
                    const float jx = fx::hashFloat(unsigned(gx + 4096), unsigned(gy + 4096), seed);
                    const float jy = fx::hashFloat(unsigned(gx + 4096), unsigned(gy + 4096),
                                                   seed + 1013u);
                    px = (gx + jx) * cell;
                    py = (gy + jy) * cell;
                };
                Image out = src;
                const Rect area = context.region.intersected(src.rect());
                fx::forEachRow(context, area.y, area.bottom(), [&](int y) {
                    Rgba* dst = out.row(y);
                    for (int x = area.x; x < area.right(); ++x) {
                        const int gx = x / cell, gy = y / cell;
                        double best = 1.0e30;
                        int bestX = x, bestY = y;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                double px = 0, py = 0;
                                cellCenter(gx + dx, gy + dy, px, py);
                                const double d = (px - x) * (px - x) + (py - y) * (py - y);
                                if (d < best) {
                                    best = d;
                                    bestX = int(std::lround(px));
                                    bestY = int(std::lround(py));
                                }
                            }
                        }
                        dst[x] = src.atClamped(bestX, bestY);
                    }
                });
                return out;
            });
}

} // namespace tessel
