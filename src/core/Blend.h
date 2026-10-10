#pragma once

#include <QString>
#include <QStringList>
#include <vector>

#include "core/Image.h"

namespace tessel {

enum class BlendMode {
    Normal = 0,
    Multiply,
    Additive,
    ColorBurn,
    ColorDodge,
    Reflect,
    Glow,
    Overlay,
    Difference,
    Negation,
    Lighten,
    Darken,
    Screen,
    Xor,
    Count
};

const QStringList& blendModeNames();
QString blendModeName(BlendMode mode);
BlendMode blendModeFromName(const QString& name, BlendMode fallback = BlendMode::Normal);

// Blends one channel pair, both in 0..1. cb is the backdrop, cs the source.
inline float blendChannel(BlendMode mode, float cb, float cs)
{
    switch (mode) {
    case BlendMode::Normal:
        return cs;
    case BlendMode::Multiply:
        return cb * cs;
    case BlendMode::Additive:
        return std::min(1.0f, cb + cs);
    case BlendMode::ColorBurn:
        return cs <= 0.0f ? 0.0f : std::clamp(1.0f - (1.0f - cb) / cs, 0.0f, 1.0f);
    case BlendMode::ColorDodge:
        return cs >= 1.0f ? 1.0f : std::clamp(cb / (1.0f - cs), 0.0f, 1.0f);
    case BlendMode::Reflect:
        return cs >= 1.0f ? 1.0f : std::clamp(cb * cb / (1.0f - cs), 0.0f, 1.0f);
    case BlendMode::Glow:
        return cb >= 1.0f ? 1.0f : std::clamp(cs * cs / (1.0f - cb), 0.0f, 1.0f);
    case BlendMode::Overlay:
        return cb < 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    case BlendMode::Difference:
        return std::abs(cb - cs);
    case BlendMode::Negation:
        return 1.0f - std::abs(1.0f - cb - cs);
    case BlendMode::Lighten:
        return std::max(cb, cs);
    case BlendMode::Darken:
        return std::min(cb, cs);
    case BlendMode::Screen:
        return 1.0f - (1.0f - cb) * (1.0f - cs);
    case BlendMode::Xor: {
        const int a = static_cast<int>(cb * 255.0f + 0.5f);
        const int b = static_cast<int>(cs * 255.0f + 0.5f);
        return static_cast<float>(a ^ b) * (1.0f / 255.0f);
    }
    default:
        return cs;
    }
}

inline uint8_t toByte(float v)
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Composites src over dst with straight alpha, honouring a blend mode.
inline Rgba blendOver(Rgba dst, Rgba src, BlendMode mode, float alphaScale = 1.0f)
{
    const float sa = (src.a * (1.0f / 255.0f)) * alphaScale;
    if (sa <= 0.0f)
        return dst;
    const float da = dst.a * (1.0f / 255.0f);
    if (da <= 0.0f && mode == BlendMode::Normal) {
        return Rgba(src.r, src.g, src.b, toByte(sa));
    }
    const float sr = src.r * (1.0f / 255.0f);
    const float sg = src.g * (1.0f / 255.0f);
    const float sb = src.b * (1.0f / 255.0f);
    const float dr = dst.r * (1.0f / 255.0f);
    const float dg = dst.g * (1.0f / 255.0f);
    const float db = dst.b * (1.0f / 255.0f);

    float br = sr, bg = sg, bb = sb;
    if (mode != BlendMode::Normal) {
        br = blendChannel(mode, dr, sr);
        bg = blendChannel(mode, dg, sg);
        bb = blendChannel(mode, db, sb);
    }
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0f)
        return Rgba(0, 0, 0, 0);
    const float w1 = sa * (1.0f - da);
    const float w2 = sa * da;
    const float w3 = (1.0f - sa) * da;
    const float inv = 1.0f / oa;
    return Rgba(toByte((w1 * sr + w2 * br + w3 * dr) * inv),
                toByte((w1 * sg + w2 * bg + w3 * dg) * inv),
                toByte((w1 * sb + w2 * bb + w3 * db) * inv), toByte(oa));
}

// One entry of a flattening job. `image` must stay alive for the call.
struct CompositeLayer {
    const Image* image = nullptr;
    uint8_t opacity = 255;
    BlendMode mode = BlendMode::Normal;
    bool visible = true;
};

Image compositeLayers(const std::vector<CompositeLayer>& layers, int width, int height);
void compositeRegion(const std::vector<CompositeLayer>& layers, Image& out, const Rect& region);

} // namespace tessel
