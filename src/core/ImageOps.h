#pragma once

#include <array>
#include <functional>
#include <vector>

#include <QPointF>
#include <QTransform>

#include "core/Blend.h"
#include "core/Image.h"

namespace tessel::ops {

// --------------------------------------------------------------- float buffers

// Interleaved RGBA float buffer with premultiplied alpha, which is the correct
// space for blurring and resampling.
class FloatImage {
public:
    FloatImage() = default;
    FloatImage(int width, int height)
        : w_(std::max(0, width)), h_(std::max(0, height))
    {
        v_.assign(static_cast<size_t>(w_) * h_ * 4, 0.0f);
    }

    int width() const { return w_; }
    int height() const { return h_; }
    bool isNull() const { return w_ <= 0 || h_ <= 0; }
    float* row(int y) { return v_.data() + static_cast<size_t>(y) * w_ * 4; }
    const float* row(int y) const { return v_.data() + static_cast<size_t>(y) * w_ * 4; }
    float* data() { return v_.data(); }
    const float* data() const { return v_.data(); }

private:
    int w_ = 0;
    int h_ = 0;
    std::vector<float> v_;
};

FloatImage premultiply(const Image& src);
Image unpremultiply(const FloatImage& src);

// --------------------------------------------------------------- blur

void boxBlurPlanes(FloatImage& buffer, int radius);
FloatImage gaussianBlurPlanes(const FloatImage& src, double sigma);
Image gaussianBlur(const Image& src, double radius);
Image boxBlur(const Image& src, int radius);
Mask blurMask(const Mask& src, double radius);

// --------------------------------------------------------------- sampling

enum class EdgeMode { Clamp, Wrap, Reflect, Transparent };

Rgba sampleBilinear(const Image& src, float x, float y, EdgeMode edge);
Rgba sampleNearest(const Image& src, int x, int y, EdgeMode edge);

// Builds a new image by asking `fn(x, y, &sx, &sy)` where to read from.
Image remap(const Image& src, int outWidth, int outHeight,
            const std::function<void(int, int, float&, float&)>& fn,
            EdgeMode edge);

// --------------------------------------------------------------- geometry

Image resize(const Image& src, int width, int height, bool smooth = true);
Image rotate90(const Image& src, int turns);
Image flip(const Image& src, bool horizontal);
Image rotateFree(const Image& src, double degrees, bool expand);

// --------------------------------------------------------------- colour

void rgbToHsv(float r, float g, float b, float& h, float& s, float& v);
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b);

inline float luminance(Rgba c)
{
    return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) * (1.0f / 255.0f);
}
inline uint8_t luminance8(Rgba c)
{
    return static_cast<uint8_t>((c.r * 77 + c.g * 151 + c.b * 28) >> 8);
}

using Lut = std::array<uint8_t, 256>;
Lut identityLut();
void applyLut(Image& image, const Lut& lut, const Rect& region);
void applyLutRgb(Image& image, const Lut& r, const Lut& g, const Lut& b,
                 const Rect& region);

// --------------------------------------------------------------- painting

void colorOver(Image& dst, const Mask& mask, Rgba color, BlendMode mode,
               const Rect& region);
void imageOver(Image& dst, const Image& src, const Mask* mask, BlendMode mode,
               const Rect& region, float opacity = 1.0f);
void eraseMask(Image& dst, const Mask& mask, float strength, const Rect& region);
// Mixes `src` into `dst` through a clip mask; used to confine effects to a
// selection without touching anything outside it.
void mixThroughMask(Image& dst, const Image& src, const Mask* clip,
                    const Rect& region);
void fillRegion(Image& dst, const Rect& region, Rgba color);

// --------------------------------------------------------------- brush

Mask brushKernel(double diameter, double hardness);
void stampMax(Mask& target, const Mask& kernel, double cx, double cy,
              float strength = 1.0f);
Mask strokeMask(int width, int height, const std::vector<QPointF>& points,
                double diameter, double hardness, double spacing = 0.2,
                float strength = 1.0f);
Rect strokeBounds(const std::vector<QPointF>& points, double diameter);

// --------------------------------------------------------------- fills

std::vector<uint8_t> matchMask(const Image& src, Rgba seed, double tolerance,
                               bool compareAlpha);
Mask floodFill(const Image& src, int seedX, int seedY, double tolerance,
               bool contiguous, bool antialias);
Mask maskFromBooleans(const std::vector<uint8_t>& flags, int width, int height);
Mask featherMask(const Mask& src, double radius);
// Positive pixels dilate, negative pixels erode. Measured with a Euclidean
// distance field so the result stays round instead of growing into a diamond.
Mask growMask(const Mask& src, int pixels);
// The band of `pixels` width lying just inside the current edge, so the result
// keeps the same bounds as the input.
Mask borderMask(const Mask& src, int pixels);
// Signed distance in pixels from each cell to the mask edge, negative inside.
std::vector<float> signedDistanceField(const Mask& src, float threshold = 0.5f);
// Resamples a mask through an arbitrary transform; used when floated pixels are
// scaled or rotated and the selection has to follow them.
Mask transformMask(const Mask& src, const QTransform& transform, int outWidth,
                   int outHeight);

// --------------------------------------------------------------- misc

Image colorizeChecker(int width, int height, int cell, Rgba a, Rgba b);
double clampd(double v, double lo, double hi);

} // namespace tessel::ops

namespace tessel::ops {

// Per channel histograms, used by Levels and Auto Level.
struct Histogram {
    std::array<uint32_t, 256> red{};
    std::array<uint32_t, 256> green{};
    std::array<uint32_t, 256> blue{};
    std::array<uint32_t, 256> luma{};
    uint32_t samples = 0;

    int percentile(const std::array<uint32_t, 256>& channel, double fraction) const;
};

Histogram computeHistogram(const Image& image, const Mask* clip, const Rect& region);

// Monotone cubic interpolation through control points given in 0..255 space.
Lut lutFromControlPoints(const std::vector<QPointF>& points);

} // namespace tessel::ops
