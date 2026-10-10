// The Noise submenu.
#include "effects/Effect.h"

#include <array>
#include <cmath>

namespace tessel {

void registerNoise(EffectRegistry& registry)
{
    // ------------------------------------------------------------ add noise
    fx::add(registry, "add_noise", "Add Noise", "Noise",
            {
                ParamSpec::intParam("intensity", "Intensity", 0, 100, 64),
                ParamSpec::intParam("saturation", "Colour saturation", 0, 400, 100, "%"),
                ParamSpec::intParam("coverage", "Coverage", 0, 100, 100, "%"),
                ParamSpec::intParam("seed", "Seed", 0, 999, 1),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const float intensity = values.intValue("intensity", 64) / 100.0f;
                const float saturation = values.intValue("saturation", 100) / 100.0f;
                const float coverage = values.intValue("coverage", 100) / 100.0f;
                const unsigned seed = unsigned(values.intValue("seed", 1)) * 2246822519u + 3u;
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    if (fx::hashFloat(unsigned(x), unsigned(y), seed + 99u) > coverage)
                        return c;
                    const float mono = (fx::hashFloat(unsigned(x), unsigned(y), seed) - 0.5f)
                        * 2.0f * intensity;
                    const float dr = (fx::hashFloat(unsigned(x), unsigned(y), seed + 11u) - 0.5f)
                        * 2.0f * intensity * saturation;
                    const float dg = (fx::hashFloat(unsigned(x), unsigned(y), seed + 22u) - 0.5f)
                        * 2.0f * intensity * saturation;
                    const float db = (fx::hashFloat(unsigned(x), unsigned(y), seed + 33u) - 0.5f)
                        * 2.0f * intensity * saturation;
                    return Rgba(toByte(c.r / 255.0f + mono + dr),
                                toByte(c.g / 255.0f + mono + dg),
                                toByte(c.b / 255.0f + mono + db), c.a);
                });
            });

    // ------------------------------------------------------------ median
    fx::add(registry, "median", "Median", "Noise",
            {
                ParamSpec::intParam("radius", "Radius", 1, 20, 3, " px"),
                ParamSpec::intParam("percentile", "Percentile", 1, 100, 50),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int radius = std::clamp(values.intValue("radius", 3), 1, 20);
                const double percentile = std::clamp(values.intValue("percentile", 50), 1, 100)
                    / 100.0;
                const Image& src = *context.source;
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    std::array<int, 256> histR{}, histG{}, histB{};
                    int total = 0;
                    for (int dy = -radius; dy <= radius; ++dy) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            if (dx * dx + dy * dy > radius * radius)
                                continue;
                            const Rgba s = src.atClamped(x + dx, y + dy);
                            ++histR[s.r];
                            ++histG[s.g];
                            ++histB[s.b];
                            ++total;
                        }
                    }
                    if (total == 0)
                        return c;
                    const int target = std::max(1, int(std::lround(total * percentile)));
                    auto pick = [&](const std::array<int, 256>& hist) {
                        int running = 0;
                        for (int i = 0; i < 256; ++i) {
                            running += hist[static_cast<size_t>(i)];
                            if (running >= target)
                                return static_cast<uint8_t>(i);
                        }
                        return static_cast<uint8_t>(255);
                    };
                    return Rgba(pick(histR), pick(histG), pick(histB), c.a);
                });
            });

    // ------------------------------------------------------------ reduce noise
    fx::add(registry, "reduce_noise", "Reduce Noise", "Noise",
            {
                ParamSpec::intParam("radius", "Radius", 1, 10, 3, " px"),
                ParamSpec::doubleParam("strength", "Strength", 0.0, 1.0, 0.4, 0.02, 2),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int radius = std::clamp(values.intValue("radius", 3), 1, 10);
                const double strength = std::clamp(values.doubleValue("strength", 0.4),
                                                   0.0, 1.0);
                const Image& src = *context.source;
                // Variance guided smoothing: flat areas are averaged hard,
                // detailed areas are left mostly alone.
                return fx::mapPixels(context, [&](Rgba c, int x, int y) {
                    double sum[3] = {0, 0, 0};
                    double sumSq = 0;
                    int count = 0;
                    for (int dy = -radius; dy <= radius; ++dy) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const Rgba s = src.atClamped(x + dx, y + dy);
                            sum[0] += s.r;
                            sum[1] += s.g;
                            sum[2] += s.b;
                            const double v = ops::luminance8(s);
                            sumSq += v * v;
                            ++count;
                        }
                    }
                    const double mean = (sum[0] + sum[1] + sum[2]) / (3.0 * count);
                    const double variance = sumSq / count - mean * mean;
                    const double noise = 64.0 + 512.0 * (1.0 - strength);
                    const double mix = strength * noise / (noise + std::max(0.0, variance));
                    return Rgba(toByte(float((c.r * (1.0 - mix) + sum[0] / count * mix) / 255.0)),
                                toByte(float((c.g * (1.0 - mix) + sum[1] / count * mix) / 255.0)),
                                toByte(float((c.b * (1.0 - mix) + sum[2] / count * mix) / 255.0)),
                                c.a);
                });
            });
}

} // namespace tessel
