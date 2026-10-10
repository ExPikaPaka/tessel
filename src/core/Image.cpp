#include "core/Image.h"

#include "core/Parallel.h"

namespace tessel {

Image Image::cropped(const Rect& r) const
{
    Image out(std::max(1, r.w), std::max(1, r.h));
    const Rect src = r.intersected(rect());
    if (src.isEmpty())
        return out;
    for (int y = 0; y < src.h; ++y) {
        std::memcpy(out.row(src.y - r.y + y) + (src.x - r.x), row(src.y + y) + src.x,
                    static_cast<size_t>(src.w) * sizeof(Rgba));
    }
    return out;
}

void Image::blit(const Image& src, int dx, int dy)
{
    const Rect target = Rect(dx, dy, src.width(), src.height()).intersected(rect());
    if (target.isEmpty())
        return;
    for (int y = 0; y < target.h; ++y) {
        std::memcpy(row(target.y + y) + target.x,
                    src.row(target.y - dy + y) + (target.x - dx),
                    static_cast<size_t>(target.w) * sizeof(Rgba));
    }
}

void Image::copyRectFrom(const Image& src, const Rect& r)
{
    const Rect area = r.intersected(rect()).intersected(src.rect());
    if (area.isEmpty())
        return;
    for (int y = 0; y < area.h; ++y) {
        std::memcpy(row(area.y + y) + area.x, src.row(area.y + y) + area.x,
                    static_cast<size_t>(area.w) * sizeof(Rgba));
    }
}

bool Image::isFullyOpaque() const
{
    for (const Rgba& p : px_) {
        if (p.a != 255)
            return false;
    }
    return true;
}

Rect Image::opaqueBounds() const
{
    int minX = w_, minY = h_, maxX = -1, maxY = -1;
    for (int y = 0; y < h_; ++y) {
        const Rgba* line = row(y);
        int first = -1, last = -1;
        for (int x = 0; x < w_; ++x) {
            if (line[x].a != 0) {
                if (first < 0)
                    first = x;
                last = x;
            }
        }
        if (first >= 0) {
            minX = std::min(minX, first);
            maxX = std::max(maxX, last);
            minY = std::min(minY, y);
            maxY = y;
        }
    }
    if (maxX < 0)
        return Rect();
    return Rect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

Image Image::fromQImage(const QImage& src)
{
    if (src.isNull())
        return Image();
    const QImage conv = src.format() == QImage::Format_RGBA8888
        ? src
        : src.convertToFormat(QImage::Format_RGBA8888);
    Image out(conv.width(), conv.height());
    for (int y = 0; y < conv.height(); ++y) {
        std::memcpy(out.row(y), conv.constScanLine(y),
                    static_cast<size_t>(conv.width()) * sizeof(Rgba));
    }
    return out;
}

void Mask::invert()
{
    for (float& v : v_)
        v = 1.0f - v;
}

float Mask::maxValue() const
{
    float m = 0.0f;
    for (float v : v_)
        m = std::max(m, v);
    return m;
}

bool Mask::anyAbove(float threshold) const
{
    for (float v : v_) {
        if (v > threshold)
            return true;
    }
    return false;
}

Rect Mask::boundsAbove(float threshold) const
{
    int minX = w_, minY = h_, maxX = -1, maxY = -1;
    for (int y = 0; y < h_; ++y) {
        const float* line = row(y);
        int first = -1, last = -1;
        for (int x = 0; x < w_; ++x) {
            if (line[x] > threshold) {
                if (first < 0)
                    first = x;
                last = x;
            }
        }
        if (first >= 0) {
            minX = std::min(minX, first);
            maxX = std::max(maxX, last);
            minY = std::min(minY, y);
            maxY = y;
        }
    }
    if (maxX < 0)
        return Rect();
    return Rect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

size_t Mask::countAbove(float threshold) const
{
    size_t n = 0;
    for (float v : v_) {
        if (v > threshold)
            ++n;
    }
    return n;
}

} // namespace tessel
