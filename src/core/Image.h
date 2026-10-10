#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <QImage>

namespace tessel {

// Straight (non premultiplied) 8 bit RGBA, laid out to match
// QImage::Format_RGBA8888 so a QImage can wrap the buffer with no copy.
struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 0;

    constexpr Rgba() = default;
    constexpr Rgba(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
        : r(red), g(green), b(blue), a(alpha) {}

    static Rgba fromQColor(const QColor& c)
    {
        return Rgba(static_cast<uint8_t>(c.red()), static_cast<uint8_t>(c.green()),
                    static_cast<uint8_t>(c.blue()), static_cast<uint8_t>(c.alpha()));
    }
    QColor toQColor() const { return QColor(r, g, b, a); }
    uint32_t key() const
    {
        return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | a;
    }
    bool operator==(const Rgba& o) const { return key() == o.key(); }
    bool operator!=(const Rgba& o) const { return key() != o.key(); }
};

static_assert(sizeof(Rgba) == 4, "Rgba must be tightly packed");

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    constexpr Rect() = default;
    constexpr Rect(int px, int py, int pw, int ph) : x(px), y(py), w(pw), h(ph) {}

    bool isEmpty() const { return w <= 0 || h <= 0; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool contains(int px, int py) const
    {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    Rect intersected(const Rect& o) const
    {
        const int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const int x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
    }
    Rect united(const Rect& o) const
    {
        if (isEmpty())
            return o;
        if (o.isEmpty())
            return *this;
        const int x0 = std::min(x, o.x), y0 = std::min(y, o.y);
        const int x1 = std::max(right(), o.right()), y1 = std::max(bottom(), o.bottom());
        return Rect(x0, y0, x1 - x0, y1 - y0);
    }
    Rect expanded(int by) const { return Rect(x - by, y - by, w + 2 * by, h + 2 * by); }
    QRect toQRect() const { return QRect(x, y, w, h); }
    static Rect fromQRect(const QRect& r)
    {
        return Rect(r.x(), r.y(), r.width(), r.height());
    }
};

// A heap allocated RGBA raster.
class Image {
public:
    Image() = default;
    Image(int width, int height) : w_(std::max(0, width)), h_(std::max(0, height))
    {
        px_.assign(static_cast<size_t>(w_) * h_, Rgba(0, 0, 0, 0));
    }
    Image(int width, int height, Rgba fill) : w_(std::max(0, width)), h_(std::max(0, height))
    {
        px_.assign(static_cast<size_t>(w_) * h_, fill);
    }

    int width() const { return w_; }
    int height() const { return h_; }
    bool isNull() const { return w_ <= 0 || h_ <= 0; }
    size_t pixelCount() const { return px_.size(); }
    size_t byteCount() const { return px_.size() * sizeof(Rgba); }
    Rect rect() const { return Rect(0, 0, w_, h_); }
    QSize size() const { return QSize(w_, h_); }

    Rgba* data() { return px_.data(); }
    const Rgba* data() const { return px_.data(); }
    Rgba* row(int y) { return px_.data() + static_cast<size_t>(y) * w_; }
    const Rgba* row(int y) const { return px_.data() + static_cast<size_t>(y) * w_; }

    Rgba at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w_ || y >= h_)
            return Rgba(0, 0, 0, 0);
        return px_[static_cast<size_t>(y) * w_ + x];
    }
    Rgba atClamped(int x, int y) const
    {
        x = std::clamp(x, 0, w_ - 1);
        y = std::clamp(y, 0, h_ - 1);
        return px_[static_cast<size_t>(y) * w_ + x];
    }
    void setAt(int x, int y, Rgba c)
    {
        if (x >= 0 && y >= 0 && x < w_ && y < h_)
            px_[static_cast<size_t>(y) * w_ + x] = c;
    }

    void fill(Rgba c) { std::fill(px_.begin(), px_.end(), c); }
    void clear() { fill(Rgba(0, 0, 0, 0)); }

    Image cropped(const Rect& r) const;
    void blit(const Image& src, int dx, int dy);          // straight copy
    void copyRectFrom(const Image& src, const Rect& r);   // same coordinates
    bool isFullyOpaque() const;
    Rect opaqueBounds() const;

    // Wraps the pixel buffer without copying. The Image must outlive the view.
    QImage view() const
    {
        if (isNull())
            return QImage();
        return QImage(reinterpret_cast<const uchar*>(px_.data()), w_, h_,
                      static_cast<qsizetype>(w_) * 4, QImage::Format_RGBA8888);
    }
    QImage toQImage() const { return view().copy(); }
    static Image fromQImage(const QImage& img);

private:
    int w_ = 0;
    int h_ = 0;
    std::vector<Rgba> px_;
};

// Single channel float mask, used for brush strokes, selections and effects.
class Mask {
public:
    Mask() = default;
    Mask(int width, int height, float fill = 0.0f)
        : w_(std::max(0, width)), h_(std::max(0, height))
    {
        v_.assign(static_cast<size_t>(w_) * h_, fill);
    }

    int width() const { return w_; }
    int height() const { return h_; }
    bool isNull() const { return w_ <= 0 || h_ <= 0; }
    Rect rect() const { return Rect(0, 0, w_, h_); }
    size_t byteCount() const { return v_.size() * sizeof(float); }

    float* data() { return v_.data(); }
    const float* data() const { return v_.data(); }
    float* row(int y) { return v_.data() + static_cast<size_t>(y) * w_; }
    const float* row(int y) const { return v_.data() + static_cast<size_t>(y) * w_; }
    float at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w_ || y >= h_)
            return 0.0f;
        return v_[static_cast<size_t>(y) * w_ + x];
    }
    void setAt(int x, int y, float value)
    {
        if (x >= 0 && y >= 0 && x < w_ && y < h_)
            v_[static_cast<size_t>(y) * w_ + x] = value;
    }
    void fill(float value) { std::fill(v_.begin(), v_.end(), value); }
    void invert();
    float maxValue() const;
    bool anyAbove(float threshold) const;
    Rect boundsAbove(float threshold) const;
    size_t countAbove(float threshold) const;

private:
    int w_ = 0;
    int h_ = 0;
    std::vector<float> v_;
};

} // namespace tessel
