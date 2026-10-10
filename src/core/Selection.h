#pragma once

#include <vector>

#include <QPainterPath>
#include <QTransform>

#include "core/Image.h"

namespace tessel {

enum class CombineMode { Replace, Union, Subtract, Intersect, Xor };

// Coalesces a mask into as few rectangles as possible, and the outline path
// that traces their union.
std::vector<Rect> maskToRects(const Mask& mask, float threshold = 0.5f);
QPainterPath pathFromMask(const Mask& mask, float threshold = 0.5f);

// An antialiased selection. An inactive selection means "everything", which is
// what tools and effects fall back to.
class Selection {
public:
    Selection() = default;
    Selection(int width, int height) : w_(width), h_(height) {}

    int width() const { return w_; }
    int height() const { return h_; }
    bool isActive() const { return active_; }

    void clear();
    void selectAll();
    void setMask(Mask mask);
    void combine(const Mask& mask, CombineMode mode);
    void invert();
    void resizeCanvas(int width, int height, bool scaleContent);
    void transform(bool flipH, bool flipV, int turns);
    void translate(int dx, int dy);

    // Edge operations. All of them need something selected to work on, so they
    // do nothing while the selection is inactive.
    void feather(double radius);
    void grow(int pixels);   // negative values erode
    void border(int pixels);
    // Resamples the mask through a transform in canvas coordinates, used when
    // floated pixels are scaled or rotated and the outline has to follow.
    void transformBy(const QTransform& transform);

    // Null when nothing is selected, meaning "no clipping".
    const Mask* clipMask() const { return active_ ? &mask_ : nullptr; }
    Mask maskOrFull() const;
    Rect bounds() const;
    bool contains(int x, int y) const;
    size_t pixelCount() const;
    const QPainterPath& outline() const;

private:
    void markDirty();

    int w_ = 0;
    int h_ = 0;
    bool active_ = false;
    Mask mask_;
    mutable QPainterPath outline_;
    mutable bool outlineValid_ = false;
};

} // namespace tessel
