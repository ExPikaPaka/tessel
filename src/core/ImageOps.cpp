#include "core/ImageOps.h"

#include <array>
#include <cmath>
#include <numeric>

#include <QPainter>
#include <QTransform>

#include "core/Parallel.h"

namespace tessel::ops {

double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// --------------------------------------------------------------- float buffers

FloatImage premultiply(const Image& src)
{
    FloatImage out(src.width(), src.height());
    if (src.isNull())
        return out;
    parallelFor(0, src.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const Rgba* in = src.row(y);
            float* dst = out.row(y);
            for (int x = 0; x < src.width(); ++x) {
                const float a = in[x].a * (1.0f / 255.0f);
                dst[x * 4 + 0] = in[x].r * (1.0f / 255.0f) * a;
                dst[x * 4 + 1] = in[x].g * (1.0f / 255.0f) * a;
                dst[x * 4 + 2] = in[x].b * (1.0f / 255.0f) * a;
                dst[x * 4 + 3] = a;
            }
        }
    });
    return out;
}

Image unpremultiply(const FloatImage& src)
{
    Image out(src.width(), src.height());
    if (src.isNull())
        return out;
    parallelFor(0, src.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* in = src.row(y);
            Rgba* dst = out.row(y);
            for (int x = 0; x < src.width(); ++x) {
                const float a = in[x * 4 + 3];
                if (a <= 1.0e-6f) {
                    dst[x] = Rgba(0, 0, 0, 0);
                    continue;
                }
                const float inv = 1.0f / a;
                dst[x] = Rgba(toByte(in[x * 4 + 0] * inv), toByte(in[x * 4 + 1] * inv),
                              toByte(in[x * 4 + 2] * inv), toByte(a));
            }
        }
    });
    return out;
}

// --------------------------------------------------------------- blur

namespace {

void boxBlurHorizontal(const FloatImage& src, FloatImage& dst, int radius)
{
    const int w = src.width();
    const int h = src.height();
    const double norm = 1.0 / (2.0 * radius + 1.0);
    parallelFor(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* in = src.row(y);
            float* out = dst.row(y);
            double acc[4] = {0, 0, 0, 0};
            for (int i = -radius; i <= radius; ++i) {
                const int xi = std::clamp(i, 0, w - 1);
                for (int c = 0; c < 4; ++c)
                    acc[c] += in[xi * 4 + c];
            }
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 4; ++c)
                    out[x * 4 + c] = static_cast<float>(acc[c] * norm);
                const int addX = std::clamp(x + radius + 1, 0, w - 1);
                const int subX = std::clamp(x - radius, 0, w - 1);
                for (int c = 0; c < 4; ++c)
                    acc[c] += in[addX * 4 + c] - in[subX * 4 + c];
            }
        }
    });
}

void boxBlurVertical(const FloatImage& src, FloatImage& dst, int radius)
{
    const int w = src.width();
    const int h = src.height();
    const double norm = 1.0 / (2.0 * radius + 1.0);
    parallelFor(0, w, [&](int x0, int x1) {
        std::vector<double> acc(static_cast<size_t>(x1 - x0) * 4, 0.0);
        for (int i = -radius; i <= radius; ++i) {
            const float* in = src.row(std::clamp(i, 0, h - 1));
            for (int x = x0; x < x1; ++x) {
                for (int c = 0; c < 4; ++c)
                    acc[static_cast<size_t>(x - x0) * 4 + c] += in[x * 4 + c];
            }
        }
        for (int y = 0; y < h; ++y) {
            float* out = dst.row(y);
            const float* add = src.row(std::clamp(y + radius + 1, 0, h - 1));
            const float* sub = src.row(std::clamp(y - radius, 0, h - 1));
            for (int x = x0; x < x1; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const size_t k = static_cast<size_t>(x - x0) * 4 + c;
                    out[x * 4 + c] = static_cast<float>(acc[k] * norm);
                    acc[k] += add[x * 4 + c] - sub[x * 4 + c];
                }
            }
        }
    });
}

// Three box passes approximate a gaussian closely; these are the classic
// integer box widths for a given sigma.
std::array<int, 3> boxesForGauss(double sigma)
{
    std::array<int, 3> radii = {0, 0, 0};
    if (sigma <= 0.0)
        return radii;
    const int passes = 3;
    const double ideal = std::sqrt(12.0 * sigma * sigma / passes + 1.0);
    int wl = static_cast<int>(std::floor(ideal));
    if (wl % 2 == 0)
        --wl;
    wl = std::max(1, wl);
    const int wu = wl + 2;
    const double idealM = (12.0 * sigma * sigma - passes * double(wl) * wl
                           - 4.0 * passes * wl - 3.0 * passes)
        / (-4.0 * wl - 4.0);
    const int m = static_cast<int>(std::lround(idealM));
    for (int i = 0; i < passes; ++i) {
        const int size = i < m ? wl : wu;
        radii[static_cast<size_t>(i)] = std::max(0, (size - 1) / 2);
    }
    return radii;
}

} // namespace

void boxBlurPlanes(FloatImage& buffer, int radius)
{
    if (radius < 1 || buffer.isNull())
        return;
    FloatImage tmp(buffer.width(), buffer.height());
    boxBlurHorizontal(buffer, tmp, radius);
    boxBlurVertical(tmp, buffer, radius);
}

FloatImage gaussianBlurPlanes(const FloatImage& src, double sigma)
{
    FloatImage out = src;
    if (sigma <= 0.05 || src.isNull())
        return out;
    for (int radius : boxesForGauss(sigma)) {
        if (radius >= 1)
            boxBlurPlanes(out, radius);
    }
    return out;
}

Image gaussianBlur(const Image& src, double radius)
{
    if (radius <= 0.0 || src.isNull())
        return src;
    const double sigma = radius * 0.5 + 0.25;
    return unpremultiply(gaussianBlurPlanes(premultiply(src), sigma));
}

Image boxBlur(const Image& src, int radius)
{
    if (radius < 1 || src.isNull())
        return src;
    FloatImage buffer = premultiply(src);
    boxBlurPlanes(buffer, radius);
    return unpremultiply(buffer);
}

Mask blurMask(const Mask& src, double radius)
{
    if (radius <= 0.0 || src.isNull())
        return src;
    FloatImage buffer(src.width(), src.height());
    for (int y = 0; y < src.height(); ++y) {
        const float* in = src.row(y);
        float* out = buffer.row(y);
        for (int x = 0; x < src.width(); ++x) {
            out[x * 4 + 0] = in[x];
            out[x * 4 + 3] = in[x];
        }
    }
    FloatImage blurred = gaussianBlurPlanes(buffer, radius * 0.6);
    Mask out(src.width(), src.height());
    for (int y = 0; y < src.height(); ++y) {
        const float* in = blurred.row(y);
        float* dst = out.row(y);
        for (int x = 0; x < src.width(); ++x)
            dst[x] = std::clamp(in[x * 4 + 0], 0.0f, 1.0f);
    }
    return out;
}

// --------------------------------------------------------------- sampling

namespace {

inline int wrapCoord(int v, int size, EdgeMode edge, bool& outside)
{
    switch (edge) {
    case EdgeMode::Wrap: {
        int m = v % size;
        return m < 0 ? m + size : m;
    }
    case EdgeMode::Reflect: {
        const int period = 2 * size;
        int m = v % period;
        if (m < 0)
            m += period;
        return m >= size ? period - 1 - m : m;
    }
    case EdgeMode::Transparent:
        if (v < 0 || v >= size) {
            outside = true;
            return std::clamp(v, 0, size - 1);
        }
        return v;
    case EdgeMode::Clamp:
    default:
        return std::clamp(v, 0, size - 1);
    }
}

} // namespace

Rgba sampleNearest(const Image& src, int x, int y, EdgeMode edge)
{
    bool outside = false;
    const int sx = wrapCoord(x, src.width(), edge, outside);
    const int sy = wrapCoord(y, src.height(), edge, outside);
    if (outside)
        return Rgba(0, 0, 0, 0);
    return src.row(sy)[sx];
}

Rgba sampleBilinear(const Image& src, float x, float y, EdgeMode edge)
{
    if (src.isNull())
        return Rgba(0, 0, 0, 0);
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0;
    const float fy = y - y0;

    bool outside = false;
    const int xa = wrapCoord(x0, src.width(), edge, outside);
    const int xb = wrapCoord(x0 + 1, src.width(), edge, outside);
    const int ya = wrapCoord(y0, src.height(), edge, outside);
    const int yb = wrapCoord(y0 + 1, src.height(), edge, outside);
    if (edge == EdgeMode::Transparent
        && (x < -1.0f || y < -1.0f || x > src.width() || y > src.height()))
        return Rgba(0, 0, 0, 0);

    const Rgba p00 = src.row(ya)[xa];
    const Rgba p10 = src.row(ya)[xb];
    const Rgba p01 = src.row(yb)[xa];
    const Rgba p11 = src.row(yb)[xb];

    // Interpolate in premultiplied space so transparent pixels do not bleed.
    float acc[4] = {0, 0, 0, 0};
    const Rgba corners[4] = {p00, p10, p01, p11};
    const float weights[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    for (int i = 0; i < 4; ++i) {
        const float a = corners[i].a * (1.0f / 255.0f);
        const float wgt = weights[i];
        acc[0] += corners[i].r * (1.0f / 255.0f) * a * wgt;
        acc[1] += corners[i].g * (1.0f / 255.0f) * a * wgt;
        acc[2] += corners[i].b * (1.0f / 255.0f) * a * wgt;
        acc[3] += a * wgt;
    }
    if (acc[3] <= 1.0e-6f)
        return Rgba(0, 0, 0, 0);
    const float inv = 1.0f / acc[3];
    return Rgba(toByte(acc[0] * inv), toByte(acc[1] * inv), toByte(acc[2] * inv),
                toByte(acc[3]));
}

Image remap(const Image& src, int outWidth, int outHeight,
            const std::function<void(int, int, float&, float&)>& fn, EdgeMode edge)
{
    Image out(outWidth, outHeight);
    parallelFor(0, outHeight, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* dst = out.row(y);
            for (int x = 0; x < outWidth; ++x) {
                float sx = static_cast<float>(x);
                float sy = static_cast<float>(y);
                fn(x, y, sx, sy);
                dst[x] = sampleBilinear(src, sx, sy, edge);
            }
        }
    });
    return out;
}

// --------------------------------------------------------------- geometry

Image resize(const Image& src, int width, int height, bool smooth)
{
    width = std::max(1, width);
    height = std::max(1, height);
    if (src.width() == width && src.height() == height)
        return src;
    if (src.isNull())
        return Image(width, height);
    const QImage premul = src.view().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage scaled = premul.scaled(width, height, Qt::IgnoreAspectRatio,
                                        smooth ? Qt::SmoothTransformation
                                               : Qt::FastTransformation);
    return Image::fromQImage(scaled.convertToFormat(QImage::Format_RGBA8888));
}

Image rotate90(const Image& src, int turns)
{
    turns = ((turns % 4) + 4) % 4;
    if (turns == 0)
        return src;
    const int w = src.width(), h = src.height();
    Image out = (turns % 2) ? Image(h, w) : Image(w, h);
    for (int y = 0; y < h; ++y) {
        const Rgba* in = src.row(y);
        for (int x = 0; x < w; ++x) {
            int nx = 0, ny = 0;
            switch (turns) {
            case 1: nx = h - 1 - y; ny = x; break;
            case 2: nx = w - 1 - x; ny = h - 1 - y; break;
            default: nx = y; ny = w - 1 - x; break;
            }
            out.row(ny)[nx] = in[x];
        }
    }
    return out;
}

Image flip(const Image& src, bool horizontal)
{
    Image out(src.width(), src.height());
    for (int y = 0; y < src.height(); ++y) {
        const Rgba* in = src.row(horizontal ? y : src.height() - 1 - y);
        Rgba* dst = out.row(y);
        if (horizontal) {
            for (int x = 0; x < src.width(); ++x)
                dst[x] = in[src.width() - 1 - x];
        } else {
            std::memcpy(dst, in, static_cast<size_t>(src.width()) * sizeof(Rgba));
        }
    }
    return out;
}

Image rotateFree(const Image& src, double degrees, bool expand)
{
    if (src.isNull())
        return src;
    const QImage premul = src.view().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QTransform transform;
    transform.rotate(degrees);
    QRect bounds = transform.mapRect(QRect(0, 0, src.width(), src.height()));
    const int outW = expand ? bounds.width() : src.width();
    const int outH = expand ? bounds.height() : src.height();
    QImage out(outW, outH, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(outW / 2.0, outH / 2.0);
    painter.rotate(degrees);
    painter.translate(-src.width() / 2.0, -src.height() / 2.0);
    painter.drawImage(0, 0, premul);
    painter.end();
    return Image::fromQImage(out.convertToFormat(QImage::Format_RGBA8888));
}

// --------------------------------------------------------------- colour

void rgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
    const float mx = std::max(r, std::max(g, b));
    const float mn = std::min(r, std::min(g, b));
    const float d = mx - mn;
    v = mx;
    s = mx <= 1.0e-6f ? 0.0f : d / mx;
    if (d <= 1.0e-6f) {
        h = 0.0f;
        return;
    }
    if (mx == r)
        h = std::fmod(60.0f * ((g - b) / d) + 360.0f, 360.0f);
    else if (mx == g)
        h = 60.0f * ((b - r) / d) + 120.0f;
    else
        h = 60.0f * ((r - g) / d) + 240.0f;
}

void hsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const int i = static_cast<int>(std::floor(h)) % 6;
    const float f = h - std::floor(h);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

Lut identityLut()
{
    Lut lut{};
    for (int i = 0; i < 256; ++i)
        lut[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    return lut;
}

void applyLut(Image& image, const Lut& lut, const Rect& region)
{
    applyLutRgb(image, lut, lut, lut, region);
}

void applyLutRgb(Image& image, const Lut& r, const Lut& g, const Lut& b,
                 const Rect& region)
{
    const Rect area = region.intersected(image.rect());
    if (area.isEmpty())
        return;
    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* line = image.row(y);
            for (int x = area.x; x < area.right(); ++x) {
                line[x].r = r[line[x].r];
                line[x].g = g[line[x].g];
                line[x].b = b[line[x].b];
            }
        }
    });
}

// --------------------------------------------------------------- painting

void colorOver(Image& dst, const Mask& mask, Rgba color, BlendMode mode,
               const Rect& region)
{
    const Rect area = region.intersected(dst.rect()).intersected(mask.rect());
    if (area.isEmpty())
        return;
    const float colorAlpha = color.a * (1.0f / 255.0f);
    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* line = dst.row(y);
            const float* m = mask.row(y);
            for (int x = area.x; x < area.right(); ++x) {
                const float a = m[x] * colorAlpha;
                if (a <= 0.0f)
                    continue;
                line[x] = blendOver(line[x], Rgba(color.r, color.g, color.b, 255),
                                    mode, a);
            }
        }
    });
}

void imageOver(Image& dst, const Image& src, const Mask* mask, BlendMode mode,
               const Rect& region, float opacity)
{
    Rect area = region.intersected(dst.rect()).intersected(src.rect());
    if (mask)
        area = area.intersected(mask->rect());
    if (area.isEmpty())
        return;
    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* line = dst.row(y);
            const Rgba* in = src.row(y);
            const float* m = mask ? mask->row(y) : nullptr;
            for (int x = area.x; x < area.right(); ++x) {
                const float a = (m ? m[x] : 1.0f) * opacity;
                if (a <= 0.0f)
                    continue;
                line[x] = blendOver(line[x], in[x], mode, a);
            }
        }
    });
}

void eraseMask(Image& dst, const Mask& mask, float strength, const Rect& region)
{
    const Rect area = region.intersected(dst.rect()).intersected(mask.rect());
    if (area.isEmpty())
        return;
    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* line = dst.row(y);
            const float* m = mask.row(y);
            for (int x = area.x; x < area.right(); ++x) {
                const float cut = std::clamp(m[x] * strength, 0.0f, 1.0f);
                if (cut <= 0.0f)
                    continue;
                line[x].a = toByte(line[x].a * (1.0f / 255.0f) * (1.0f - cut));
                if (line[x].a == 0)
                    line[x] = Rgba(line[x].r, line[x].g, line[x].b, 0);
            }
        }
    });
}

void mixThroughMask(Image& dst, const Image& src, const Mask* clip,
                    const Rect& region)
{
    Rect area = region.intersected(dst.rect()).intersected(src.rect());
    if (clip)
        area = area.intersected(clip->rect());
    if (area.isEmpty())
        return;
    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* line = dst.row(y);
            const Rgba* in = src.row(y);
            const float* m = clip ? clip->row(y) : nullptr;
            for (int x = area.x; x < area.right(); ++x) {
                const float t = m ? m[x] : 1.0f;
                if (t <= 0.0f)
                    continue;
                if (t >= 1.0f) {
                    line[x] = in[x];
                    continue;
                }
                // Mix in premultiplied space to keep edges clean.
                const float da = line[x].a * (1.0f / 255.0f);
                const float sa = in[x].a * (1.0f / 255.0f);
                const float oa = da + (sa - da) * t;
                if (oa <= 1.0e-6f) {
                    line[x] = Rgba(0, 0, 0, 0);
                    continue;
                }
                const float inv = 1.0f / oa;
                const float pr = (line[x].r * da * (1 - t) + in[x].r * sa * t) * (1.0f / 255.0f);
                const float pg = (line[x].g * da * (1 - t) + in[x].g * sa * t) * (1.0f / 255.0f);
                const float pb = (line[x].b * da * (1 - t) + in[x].b * sa * t) * (1.0f / 255.0f);
                line[x] = Rgba(toByte(pr * inv), toByte(pg * inv), toByte(pb * inv),
                               toByte(oa));
            }
        }
    });
}

void fillRegion(Image& dst, const Rect& region, Rgba color)
{
    const Rect area = region.intersected(dst.rect());
    for (int y = area.y; y < area.bottom(); ++y)
        std::fill(dst.row(y) + area.x, dst.row(y) + area.right(), color);
}

// --------------------------------------------------------------- brush

Mask brushKernel(double diameter, double hardness)
{
    diameter = std::max(1.0, diameter);
    int size = static_cast<int>(std::ceil(diameter)) + 2;
    if (size % 2 == 0)
        ++size;
    Mask kernel(size, size);
    const double c = (size - 1) / 2.0;
    const double radius = diameter / 2.0;
    hardness = clampd(hardness, 0.0, 0.999);
    const double inner = radius * hardness;
    const double falloff = std::max(radius - inner, 0.6);
    const bool crisp = hardness >= 0.98;
    for (int y = 0; y < size; ++y) {
        float* line = kernel.row(y);
        for (int x = 0; x < size; ++x) {
            const double dist = std::hypot(x - c, y - c);
            const double value = crisp ? (radius + 0.5 - dist)
                                       : ((radius - dist) / falloff);
            line[x] = static_cast<float>(clampd(value, 0.0, 1.0));
        }
    }
    return kernel;
}

void stampMax(Mask& target, const Mask& kernel, double cx, double cy, float strength)
{
    const int kw = kernel.width(), kh = kernel.height();
    const int x0 = static_cast<int>(std::lround(cx - (kw - 1) / 2.0));
    const int y0 = static_cast<int>(std::lround(cy - (kh - 1) / 2.0));
    const Rect area = Rect(x0, y0, kw, kh).intersected(target.rect());
    if (area.isEmpty())
        return;
    for (int y = area.y; y < area.bottom(); ++y) {
        float* dst = target.row(y);
        const float* src = kernel.row(y - y0);
        for (int x = area.x; x < area.right(); ++x) {
            const float v = src[x - x0] * strength;
            if (v > dst[x])
                dst[x] = v;
        }
    }
}

Mask strokeMask(int width, int height, const std::vector<QPointF>& points,
                double diameter, double hardness, double spacing, float strength)
{
    Mask mask(width, height);
    if (points.empty())
        return mask;
    const Mask kernel = brushKernel(diameter, hardness);
    const double step = std::max(0.65, diameter * spacing);
    stampMax(mask, kernel, points.front().x(), points.front().y(), strength);
    double carry = 0.0;
    for (size_t i = 1; i < points.size(); ++i) {
        const QPointF a = points[i - 1];
        const QPointF b = points[i];
        const double dx = b.x() - a.x();
        const double dy = b.y() - a.y();
        const double length = std::hypot(dx, dy);
        if (length < 1.0e-6)
            continue;
        double pos = step - carry;
        while (pos <= length) {
            const double t = pos / length;
            stampMax(mask, kernel, a.x() + dx * t, a.y() + dy * t, strength);
            pos += step;
        }
        carry = std::fmod(length - (pos - step), step);
    }
    stampMax(mask, kernel, points.back().x(), points.back().y(), strength);
    return mask;
}

Rect strokeBounds(const std::vector<QPointF>& points, double diameter)
{
    if (points.empty())
        return Rect();
    double minX = points[0].x(), maxX = minX, minY = points[0].y(), maxY = minY;
    for (const QPointF& p : points) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }
    const int pad = static_cast<int>(std::ceil(diameter / 2.0)) + 2;
    const int x0 = static_cast<int>(std::floor(minX)) - pad;
    const int y0 = static_cast<int>(std::floor(minY)) - pad;
    const int x1 = static_cast<int>(std::ceil(maxX)) + pad;
    const int y1 = static_cast<int>(std::ceil(maxY)) + pad;
    return Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

// --------------------------------------------------------------- fills

std::vector<uint8_t> matchMask(const Image& src, Rgba seed, double tolerance,
                               bool compareAlpha)
{
    std::vector<uint8_t> flags(src.pixelCount(), 0);
    const int limit = static_cast<int>(std::lround(clampd(tolerance, 0.0, 1.0) * 255.0));
    parallelFor(0, src.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const Rgba* line = src.row(y);
            uint8_t* out = flags.data() + static_cast<size_t>(y) * src.width();
            for (int x = 0; x < src.width(); ++x) {
                int diff = std::abs(int(line[x].r) - int(seed.r));
                diff = std::max(diff, std::abs(int(line[x].g) - int(seed.g)));
                diff = std::max(diff, std::abs(int(line[x].b) - int(seed.b)));
                if (compareAlpha)
                    diff = std::max(diff, std::abs(int(line[x].a) - int(seed.a)));
                out[x] = diff <= limit ? 1 : 0;
            }
        }
    });
    return flags;
}

Mask maskFromBooleans(const std::vector<uint8_t>& flags, int width, int height)
{
    Mask mask(width, height);
    for (int y = 0; y < height; ++y) {
        float* dst = mask.row(y);
        const uint8_t* src = flags.data() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x)
            dst[x] = src[x] ? 1.0f : 0.0f;
    }
    return mask;
}

Mask floodFill(const Image& src, int seedX, int seedY, double tolerance,
               bool contiguous, bool antialias)
{
    Mask mask(src.width(), src.height());
    if (!src.rect().contains(seedX, seedY))
        return mask;
    const Rgba seed = src.at(seedX, seedY);
    std::vector<uint8_t> match = matchMask(src, seed, tolerance, true);
    if (!contiguous) {
        mask = maskFromBooleans(match, src.width(), src.height());
        return antialias ? featherMask(mask, 1.0) : mask;
    }

    // Span based scanline fill: each popped seed fills one horizontal run and
    // pushes the runs above and below it.
    const int w = src.width(), h = src.height();
    std::vector<uint8_t> filled(match.size(), 0);
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(seedX, seedY);
    while (!stack.empty()) {
        const auto [sx, sy] = stack.back();
        stack.pop_back();
        const size_t base = static_cast<size_t>(sy) * w;
        if (!match[base + sx] || filled[base + sx])
            continue;
        int left = sx;
        while (left - 1 >= 0 && match[base + left - 1] && !filled[base + left - 1])
            --left;
        int right = sx;
        while (right + 1 < w && match[base + right + 1] && !filled[base + right + 1])
            ++right;
        for (int x = left; x <= right; ++x)
            filled[base + x] = 1;
        for (int ny : {sy - 1, sy + 1}) {
            if (ny < 0 || ny >= h)
                continue;
            const size_t nbase = static_cast<size_t>(ny) * w;
            int x = left;
            while (x <= right) {
                while (x <= right && (!match[nbase + x] || filled[nbase + x]))
                    ++x;
                if (x > right)
                    break;
                const int runStart = x;
                while (x <= right && match[nbase + x] && !filled[nbase + x])
                    ++x;
                stack.emplace_back(runStart, ny);
            }
        }
    }
    mask = maskFromBooleans(filled, w, h);
    return antialias ? featherMask(mask, 1.0) : mask;
}

Mask featherMask(const Mask& src, double radius)
{
    if (radius <= 0.0)
        return src;
    Mask blurred = blurMask(src, radius);
    // Keep the solid core solid; only the rim should soften.
    for (int y = 0; y < blurred.height(); ++y) {
        float* line = blurred.row(y);
        const float* orig = src.row(y);
        for (int x = 0; x < blurred.width(); ++x)
            line[x] = std::clamp(std::max(line[x] * 1.15f, orig[x] * 0.85f), 0.0f, 1.0f);
    }
    return blurred;
}

Mask growMask(const Mask& src, int pixels)
{
    if (pixels == 0)
        return src;
    Mask out = src;
    const bool grow = pixels > 0;
    const int steps = std::abs(pixels);
    for (int s = 0; s < steps; ++s) {
        Mask next = out;
        for (int y = 0; y < out.height(); ++y) {
            float* dst = next.row(y);
            for (int x = 0; x < out.width(); ++x) {
                float v = out.at(x, y);
                const float neighbours[4] = {out.at(x - 1, y), out.at(x + 1, y),
                                             out.at(x, y - 1), out.at(x, y + 1)};
                for (float n : neighbours)
                    v = grow ? std::max(v, n) : std::min(v, n);
                dst[x] = v;
            }
        }
        out = next;
    }
    return out;
}

Image colorizeChecker(int width, int height, int cell, Rgba a, Rgba b)
{
    Image out(width, height);
    for (int y = 0; y < height; ++y) {
        Rgba* line = out.row(y);
        for (int x = 0; x < width; ++x)
            line[x] = ((x / cell + y / cell) & 1) ? b : a;
    }
    return out;
}

} // namespace tessel::ops

namespace tessel::ops {

int Histogram::percentile(const std::array<uint32_t, 256>& channel,
                          double fraction) const
{
    if (samples == 0)
        return fraction < 0.5 ? 0 : 255;
    const uint32_t target = static_cast<uint32_t>(
        std::clamp(fraction, 0.0, 1.0) * samples);
    uint32_t running = 0;
    for (int i = 0; i < 256; ++i) {
        running += channel[static_cast<size_t>(i)];
        if (running >= target)
            return i;
    }
    return 255;
}

Histogram computeHistogram(const Image& image, const Mask* clip, const Rect& region)
{
    Histogram out;
    Rect area = region.intersected(image.rect());
    if (clip)
        area = area.intersected(clip->rect());
    if (area.isEmpty())
        return out;
    for (int y = area.y; y < area.bottom(); ++y) {
        const Rgba* line = image.row(y);
        const float* mask = clip ? clip->row(y) : nullptr;
        for (int x = area.x; x < area.right(); ++x) {
            if (mask && mask[x] <= 0.5f)
                continue;
            if (line[x].a == 0)
                continue;
            ++out.red[line[x].r];
            ++out.green[line[x].g];
            ++out.blue[line[x].b];
            ++out.luma[luminance8(line[x])];
            ++out.samples;
        }
    }
    return out;
}

Lut lutFromControlPoints(const std::vector<QPointF>& input)
{
    Lut lut = identityLut();
    if (input.size() < 2)
        return lut;
    std::vector<QPointF> points = input;
    std::sort(points.begin(), points.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    // Drop duplicate x values, which would make the slopes blow up.
    points.erase(std::unique(points.begin(), points.end(),
                             [](const QPointF& a, const QPointF& b) {
                                 return std::abs(a.x() - b.x()) < 0.5;
                             }),
                 points.end());
    const size_t n = points.size();
    if (n < 2)
        return lut;

    // Fritsch and Carlson monotone cubic: keeps the curve from overshooting.
    std::vector<double> slope(n - 1), tangent(n, 0.0);
    for (size_t i = 0; i + 1 < n; ++i) {
        const double dx = points[i + 1].x() - points[i].x();
        slope[i] = dx != 0.0 ? (points[i + 1].y() - points[i].y()) / dx : 0.0;
    }
    tangent[0] = slope[0];
    tangent[n - 1] = slope[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) {
        if (slope[i - 1] * slope[i] <= 0.0) {
            tangent[i] = 0.0;
        } else {
            const double dx0 = points[i].x() - points[i - 1].x();
            const double dx1 = points[i + 1].x() - points[i].x();
            const double common = dx0 + dx1;
            tangent[i] = 3.0 * common
                / ((common + dx1) / slope[i - 1] + (common + dx0) / slope[i]);
        }
    }

    size_t segment = 0;
    for (int v = 0; v < 256; ++v) {
        const double x = v;
        if (x <= points.front().x()) {
            lut[static_cast<size_t>(v)] = static_cast<uint8_t>(
                std::clamp(points.front().y(), 0.0, 255.0) + 0.5);
            continue;
        }
        if (x >= points.back().x()) {
            lut[static_cast<size_t>(v)] = static_cast<uint8_t>(
                std::clamp(points.back().y(), 0.0, 255.0) + 0.5);
            continue;
        }
        while (segment + 2 < n && x > points[segment + 1].x())
            ++segment;
        const double x0 = points[segment].x(), x1 = points[segment + 1].x();
        const double y0 = points[segment].y(), y1 = points[segment + 1].y();
        const double h = x1 - x0;
        const double t = (x - x0) / h;
        const double t2 = t * t, t3 = t2 * t;
        const double h00 = 2 * t3 - 3 * t2 + 1;
        const double h10 = t3 - 2 * t2 + t;
        const double h01 = -2 * t3 + 3 * t2;
        const double h11 = t3 - t2;
        const double y = h00 * y0 + h10 * h * tangent[segment] + h01 * y1
            + h11 * h * tangent[segment + 1];
        lut[static_cast<size_t>(v)] = static_cast<uint8_t>(std::clamp(y, 0.0, 255.0) + 0.5);
    }
    return lut;
}

} // namespace tessel::ops
