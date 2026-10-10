#include "core/Selection.h"

#include "core/ImageOps.h"

namespace tessel {

std::vector<Rect> maskToRects(const Mask& mask, float threshold)
{
    std::vector<Rect> out;
    if (mask.isNull())
        return out;
    // Horizontal runs are merged downwards while they keep the same extent, so
    // a rectangular selection comes out as a single rectangle.
    struct Open {
        int x0, x1, y0, height;
    };
    std::vector<Open> open;
    std::vector<Open> next;
    std::vector<std::pair<int, int>> runs;
    for (int y = 0; y < mask.height(); ++y) {
        runs.clear();
        const float* line = mask.row(y);
        int x = 0;
        while (x < mask.width()) {
            while (x < mask.width() && line[x] <= threshold)
                ++x;
            if (x >= mask.width())
                break;
            const int start = x;
            while (x < mask.width() && line[x] > threshold)
                ++x;
            runs.emplace_back(start, x - 1);
        }
        next.clear();
        for (const auto& run : runs) {
            bool extended = false;
            for (size_t i = 0; i < open.size(); ++i) {
                if (open[i].x0 == run.first && open[i].x1 == run.second
                    && open[i].y0 + open[i].height == y) {
                    Open grown = open[i];
                    grown.height += 1;
                    next.push_back(grown);
                    open.erase(open.begin() + static_cast<long>(i));
                    extended = true;
                    break;
                }
            }
            if (!extended)
                next.push_back({run.first, run.second, y, 1});
        }
        for (const Open& stale : open)
            out.push_back(Rect(stale.x0, stale.y0, stale.x1 - stale.x0 + 1, stale.height));
        open = next;
    }
    for (const Open& stale : open)
        out.push_back(Rect(stale.x0, stale.y0, stale.x1 - stale.x0 + 1, stale.height));
    return out;
}

QPainterPath pathFromMask(const Mask& mask, float threshold)
{
    QPainterPath path;
    for (const Rect& rect : maskToRects(mask, threshold))
        path.addRect(rect.x, rect.y, rect.w, rect.h);
    return path.simplified();
}

void Selection::markDirty()
{
    outlineValid_ = false;
    outline_ = QPainterPath();
}

void Selection::clear()
{
    active_ = false;
    mask_ = Mask();
    markDirty();
}

void Selection::selectAll()
{
    mask_ = Mask(w_, h_, 1.0f);
    active_ = true;
    markDirty();
}

void Selection::setMask(Mask mask)
{
    if (mask.isNull() || !mask.anyAbove(0.004f)) {
        clear();
        return;
    }
    mask_ = std::move(mask);
    w_ = mask_.width();
    h_ = mask_.height();
    active_ = true;
    markDirty();
}

void Selection::combine(const Mask& mask, CombineMode mode)
{
    if (mode == CombineMode::Replace) {
        Mask copy = mask;
        setMask(std::move(copy));
        return;
    }
    if (!active_) {
        // With nothing selected the canvas counts as fully selected for the
        // modes that take something away, and as empty for the ones that add.
        if (mode == CombineMode::Union) {
            Mask copy = mask;
            setMask(std::move(copy));
            return;
        }
        mask_ = Mask(w_, h_, 1.0f);
        active_ = true;
    }
    Mask out(w_, h_);
    for (int y = 0; y < h_; ++y) {
        const float* a = mask_.row(y);
        const float* b = mask.row(y);
        float* dst = out.row(y);
        for (int x = 0; x < w_; ++x) {
            switch (mode) {
            case CombineMode::Union:
                dst[x] = std::max(a[x], b[x]);
                break;
            case CombineMode::Subtract:
                dst[x] = std::clamp(a[x] - b[x], 0.0f, 1.0f);
                break;
            case CombineMode::Intersect:
                dst[x] = a[x] * b[x];
                break;
            case CombineMode::Xor:
                dst[x] = std::abs(a[x] - b[x]);
                break;
            default:
                dst[x] = b[x];
                break;
            }
        }
    }
    setMask(std::move(out));
}

void Selection::invert()
{
    if (!active_) {
        // Everything was selected, so the inverse is nothing.
        clear();
        return;
    }
    mask_.invert();
    if (!mask_.anyAbove(0.004f)) {
        clear();
        return;
    }
    markDirty();
}

void Selection::resizeCanvas(int width, int height, bool scaleContent)
{
    if (!active_) {
        w_ = width;
        h_ = height;
        return;
    }
    Mask out(width, height);
    if (scaleContent) {
        for (int y = 0; y < height; ++y) {
            const float sy = h_ > 1 ? (y + 0.5f) * h_ / height - 0.5f : 0.0f;
            float* dst = out.row(y);
            for (int x = 0; x < width; ++x) {
                const float sx = w_ > 1 ? (x + 0.5f) * w_ / width - 0.5f : 0.0f;
                const int x0 = std::clamp(int(std::floor(sx)), 0, w_ - 1);
                const int y0 = std::clamp(int(std::floor(sy)), 0, h_ - 1);
                const int x1 = std::clamp(x0 + 1, 0, w_ - 1);
                const int y1 = std::clamp(y0 + 1, 0, h_ - 1);
                const float fx = std::clamp(sx - x0, 0.0f, 1.0f);
                const float fy = std::clamp(sy - y0, 0.0f, 1.0f);
                const float top = mask_.at(x0, y0) + (mask_.at(x1, y0) - mask_.at(x0, y0)) * fx;
                const float bottom = mask_.at(x0, y1) + (mask_.at(x1, y1) - mask_.at(x0, y1)) * fx;
                dst[x] = top + (bottom - top) * fy;
            }
        }
    } else {
        const int cw = std::min(width, w_);
        const int ch = std::min(height, h_);
        for (int y = 0; y < ch; ++y)
            std::memcpy(out.row(y), mask_.row(y), static_cast<size_t>(cw) * sizeof(float));
    }
    w_ = width;
    h_ = height;
    setMask(std::move(out));
}

void Selection::transform(bool flipH, bool flipV, int turns)
{
    if (!active_) {
        if (turns % 2)
            std::swap(w_, h_);
        return;
    }
    Mask work = mask_;
    if (flipH || flipV) {
        Mask out(w_, h_);
        for (int y = 0; y < h_; ++y) {
            float* dst = out.row(y);
            const float* src = work.row(flipV ? h_ - 1 - y : y);
            for (int x = 0; x < w_; ++x)
                dst[x] = src[flipH ? w_ - 1 - x : x];
        }
        work = std::move(out);
    }
    turns = ((turns % 4) + 4) % 4;
    for (int t = 0; t < turns; ++t) {
        Mask out(work.height(), work.width());
        for (int y = 0; y < work.height(); ++y) {
            const float* src = work.row(y);
            for (int x = 0; x < work.width(); ++x)
                out.row(x)[work.height() - 1 - y] = src[x];
        }
        work = std::move(out);
    }
    w_ = work.width();
    h_ = work.height();
    setMask(std::move(work));
}

void Selection::translate(int dx, int dy)
{
    if (!active_ || (dx == 0 && dy == 0))
        return;
    Mask out(w_, h_);
    for (int y = 0; y < h_; ++y) {
        const int sy = y - dy;
        if (sy < 0 || sy >= h_)
            continue;
        const float* src = mask_.row(sy);
        float* dst = out.row(y);
        for (int x = 0; x < w_; ++x) {
            const int sx = x - dx;
            if (sx >= 0 && sx < w_)
                dst[x] = src[sx];
        }
    }
    setMask(std::move(out));
}

void Selection::feather(double radius)
{
    if (!active_ || radius <= 0.0)
        return;
    setMask(ops::featherMask(mask_, radius));
}

void Selection::grow(int pixels)
{
    if (!active_ || pixels == 0)
        return;
    setMask(ops::growMask(mask_, pixels));
}

void Selection::border(int pixels)
{
    if (!active_ || pixels <= 0)
        return;
    setMask(ops::borderMask(mask_, pixels));
}

void Selection::transformBy(const QTransform& transform)
{
    if (!active_ || transform.isIdentity() || !transform.isInvertible())
        return;
    setMask(ops::transformMask(mask_, transform, w_, h_));
}

Mask Selection::maskOrFull() const
{
    if (active_)
        return mask_;
    return Mask(w_, h_, 1.0f);
}

Rect Selection::bounds() const
{
    if (!active_)
        return Rect(0, 0, w_, h_);
    return mask_.boundsAbove(0.004f);
}

bool Selection::contains(int x, int y) const
{
    if (x < 0 || y < 0 || x >= w_ || y >= h_)
        return false;
    if (!active_)
        return true;
    return mask_.at(x, y) > 0.5f;
}

size_t Selection::pixelCount() const
{
    if (!active_)
        return static_cast<size_t>(w_) * h_;
    return mask_.countAbove(0.5f);
}

const QPainterPath& Selection::outline() const
{
    if (!outlineValid_) {
        outline_ = active_ ? pathFromMask(mask_) : QPainterPath();
        outlineValid_ = true;
    }
    return outline_;
}

} // namespace tessel
