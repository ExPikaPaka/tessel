// Pencil, paintbrush, eraser, paint bucket, gradient, clone stamp and recolor.
#include "tools/Tool.h"

#include <cmath>

#include <QKeyEvent>

#include "core/ImageOps.h"
#include "core/Parallel.h"

namespace tessel {

namespace {

// Multiplies a stroke mask by the active selection so paint never leaks out.
void clipToSelection(Mask& mask, const Document& doc, const Rect& area)
{
    const Mask* clip = doc.selection().clipMask();
    if (!clip)
        return;
    const Rect box = area.intersected(mask.rect()).intersected(clip->rect());
    for (int y = box.y; y < box.bottom(); ++y) {
        float* line = mask.row(y);
        const float* sel = clip->row(y);
        for (int x = box.x; x < box.right(); ++x)
            line[x] *= sel[x];
    }
}

// ---------------------------------------------------------------- stroke base

// Shared machinery for every tool that drags a brush across the canvas: the
// layer is snapshotted on press, a float coverage mask accumulates the stroke
// with a max operation so overlapping stamps do not darken, and only the
// touched rectangle is recomposited and recorded in the history.
class StrokeTool : public Tool {
public:
    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!context.valid())
            return;
        Layer* layer = context.doc->activeLayer();
        if (!layer)
            return;
        if (busy_)
            cancel(context);
        layer_ = layer;
        layerUid_ = layer->uid();
        backup_ = layer->image;
        coverage_ = Mask(layer->width(), layer->height());
        touch_.assign(size_t(layer->width()) * layer->height(), -1.0e9f);
        travelled_ = 0.0;
        dirty_ = Rect();
        secondary_ = event.isSecondary();
        busy_ = true;
        last_ = event.pos;
        onStrokeStart(context, event);
        addStroke(event.pos, event.pos);
        flush(context, stampBounds(event.pos, event.pos));
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!busy_ || !layer_)
            return;
        const QPointF from = last_;
        addStroke(from, event.pos);
        last_ = event.pos;
        flush(context, stampBounds(from, event.pos));
    }

    void release(ToolContext& context, const ToolEvent& event) override
    {
        if (!busy_ || !layer_)
            return;
        busy_ = false;
        const Rect area = dirty_.intersected(layer_->image.rect());
        if (!area.isEmpty()) {
            Image before = backup_.cropped(area);
            Image after = layer_->image.cropped(area);
            context.history->push(std::make_unique<PixelEdit>(
                name(), iconName(), layerUid_, area, std::move(before),
                std::move(after)));
            context.doc->invalidateLayer(layer_);
            if (context.documentChanged)
                context.documentChanged();
        }
        layer_ = nullptr;
        backup_ = Image();
        coverage_ = Mask();
    }

    void cancel(ToolContext& context) override
    {
        if (!busy_ || !layer_)
            return;
        busy_ = false;
        layer_->image = backup_;
        context.doc->invalidateLayer(layer_);
        context.repaintAll();
        layer_ = nullptr;
        backup_ = Image();
        coverage_ = Mask();
    }

    bool isBusy() const override { return busy_; }

protected:
    virtual double diameter() const { return width_; }
    virtual double hardness() const { return antialias_ ? 0.72 : 1.0; }
    virtual float strength() const { return 1.0f; }
    virtual bool wantsSinglePixel() const { return false; }
    virtual void onStrokeStart(ToolContext&, const ToolEvent&) {}
    // Paints `coverage_` into the layer for the given rectangle, starting from
    // the untouched backup so repeated passes stay idempotent.
    virtual void applyStroke(ToolContext& context, const Rect& area) = 0;

    Rgba strokeColor(const ToolContext& context) const
    {
        return secondary_ ? context.secondary : context.primary;
    }

    void addStroke(QPointF from, QPointF to)
    {
        if (wantsSinglePixel()) {
            if (from == to) {
                coverage_.setAt(int(std::floor(from.x())), int(std::floor(from.y())),
                                1.0f);
            } else {
                drawPixelLine(from, to);
            }
            return;
        }
        accumulateSegment(from, to);
    }

    // Coverage of the brush swept along one segment, measured as the true
    // distance from each pixel to that segment.
    //
    // This used to stamp a dab every few pixels and take the maximum. The
    // maximum of two offset cones dips between their centres, so a soft brush
    // laid down a visible string of beads. A swept distance field has no such
    // ripple.
    //
    // How a segment joins what is already there depends on how far back along
    // the stroke that was. Consecutive segments overlap heavily around their
    // shared joint, so they are combined with a maximum: taking the union there
    // would pile coverage up at every joint. But where a stroke loops back over
    // ground it covered much earlier, a maximum leaves a crease along the line
    // where the two passes are equidistant, which on a soft brush shows up as a
    // bright streak through the crossing. Those revisits are combined as a
    // union instead, so a crossing grows smoothly denser the way a second pass
    // of paint would, with no seam.
    void accumulateSegment(QPointF from, QPointF to)
    {
        const double radius = std::max(0.5, diameter() / 2.0);
        const double hard = std::clamp(hardness(), 0.0, 0.999);
        const double inner = radius * hard;
        // Half a pixel of ramp even at maximum hardness, so edges stay
        // antialiased instead of turning into stair steps.
        const double falloff = std::max(radius - inner, 0.5);
        const float amount = strength();
        // Shape of the falloff across that band. A straight ramp, or a
        // smoothstep, flattens off near the middle and leaves a solid core, so
        // even the softest setting still painted a hard centre with a thin
        // fuzzy rim. Raising the ramp to a power bends it the other way: the
        // coverage drops away immediately from the centre line and trails off.
        const double shape = 1.0 + 2.0 * (1.0 - hard) * (1.0 - hard);
        const bool straight = shape <= 1.02;
        // How much pigment one pass lays down. A hard brush covers completely,
        // but a very soft one is a haze that has to be built up, which is what
        // a real soft brush does and what makes the lowest settings usable.
        // It also takes the sting out of crossings: with no saturated core, the
        // kink where two passes of the stroke are equidistant shows up across
        // the whole falloff, and scaling the whole thing down scales that down
        // with it. Anything at or above roughly a third hardness still covers
        // fully, so the default brush is unchanged.
        const double density = 0.15 + 0.85 * std::min(1.0, hard / 0.35);

        const Rect box = stampBounds(from, to).intersected(coverage_.rect());
        const double abx = to.x() - from.x();
        const double aby = to.y() - from.y();
        const double lengthSq = abx * abx + aby * aby;
        const double length = std::sqrt(lengthSq);
        const double arcStart = travelled_;
        travelled_ += length;
        if (box.isEmpty())
            return;

        // Anything laid down within a brush width of here counts as the same
        // pass rather than a revisit.
        const float window = float(std::max(2.0 * radius, 4.0));
        const int width = coverage_.width();

        parallelFor(box.y, box.bottom(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* line = coverage_.row(y);
                float* touch = touch_.data() + size_t(y) * width;
                for (int x = box.x; x < box.right(); ++x) {
                    const double px = x + 0.5 - from.x();
                    const double py = y + 0.5 - from.y();
                    double along = lengthSq > 1.0e-12
                        ? (px * abx + py * aby) / lengthSq
                        : 0.0;
                    along = std::clamp(along, 0.0, 1.0);
                    const double dx = px - abx * along;
                    const double dy = py - aby * along;
                    const double distance = std::sqrt(dx * dx + dy * dy);
                    double coverage = (radius - distance) / falloff;
                    if (coverage <= 0.0)
                        continue;
                    coverage = std::min(1.0, coverage);
                    if (!straight)
                        coverage = std::pow(coverage, shape);
                    const float value = float(coverage * density) * amount;
                    const float here = float(arcStart + along * length);
                    const float previous = line[x];
                    if (previous <= 0.0f) {
                        line[x] = value;
                    } else if (std::abs(here - touch[x]) <= window) {
                        if (value <= previous)
                            continue;
                        line[x] = value;
                    } else {
                        line[x] = previous + value * (1.0f - previous);
                    }
                    touch[x] = here;
                }
            }
        }, 32);
    }

    // Bresenham, so the pencil stays exactly one pixel wide and aliased.
    void drawPixelLine(QPointF from, QPointF to)
    {
        int x0 = int(std::floor(from.x())), y0 = int(std::floor(from.y()));
        const int x1 = int(std::floor(to.x())), y1 = int(std::floor(to.y()));
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        while (true) {
            coverage_.setAt(x0, y0, 1.0f);
            if (x0 == x1 && y0 == y1)
                break;
            const int doubled = 2 * error;
            if (doubled >= dy) {
                error += dy;
                x0 += sx;
            }
            if (doubled <= dx) {
                error += dx;
                y0 += sy;
            }
        }
    }

    Rect stampBounds(QPointF a, QPointF b) const
    {
        const double pad = wantsSinglePixel() ? 2.0 : diameter() / 2.0 + 2.0;
        const int x0 = int(std::floor(std::min(a.x(), b.x()) - pad));
        const int y0 = int(std::floor(std::min(a.y(), b.y()) - pad));
        const int x1 = int(std::ceil(std::max(a.x(), b.x()) + pad));
        const int y1 = int(std::ceil(std::max(a.y(), b.y()) + pad));
        return Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }

    void flush(ToolContext& context, const Rect& area)
    {
        const Rect box = area.intersected(layer_->image.rect());
        if (box.isEmpty())
            return;
        layer_->image.copyRectFrom(backup_, box);
        applyStroke(context, box);
        dirty_ = dirty_.united(box);
        context.doc->invalidateLayer(layer_);
        context.repaint(box);
    }

    void buildCommonOptions(ToolOptionsBar& bar, bool withBlendMode = true)
    {
        bar.addSpin(QStringLiteral("width"), QStringLiteral("Width"), 1, 2000,
                    int(std::lround(width_)), QStringLiteral(" px"));
        bar.addPercent(QStringLiteral("opacity"), QStringLiteral("Opacity"),
                       int(std::lround(opacity_ * 100.0)));
        bar.addCheck(QStringLiteral("antialias"), QStringLiteral("Antialiasing"),
                     antialias_);
        if (withBlendMode) {
            bar.addCombo(QStringLiteral("blend"), QStringLiteral("Blending"),
                         blendModeNames(), int(blend_));
        }
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("width"))
            width_ = std::max(1, value.toInt());
        else if (key == QLatin1String("opacity"))
            opacity_ = std::clamp(value.toInt(), 1, 100) / 100.0;
        else if (key == QLatin1String("antialias"))
            antialias_ = value.toBool();
        else if (key == QLatin1String("blend"))
            blend_ = static_cast<BlendMode>(std::clamp(value.toInt(), 0,
                                                      int(BlendMode::Count) - 1));
    }

    Mask& coverage() { return coverage_; }
    const Image& backup() const { return backup_; }
    Layer* layer() const { return layer_; }

    double width_ = 12.0;
    double opacity_ = 1.0;
    bool antialias_ = true;
    BlendMode blend_ = BlendMode::Normal;
    bool secondary_ = false;

private:
    Layer* layer_ = nullptr;
    int layerUid_ = 0;
    Image backup_;
    Mask coverage_;
    std::vector<float> touch_;   // arc length at which each pixel was covered
    double travelled_ = 0.0;
    QPointF last_;
    Rect dirty_;
    bool busy_ = false;
};

// ---------------------------------------------------------------- paintbrush

class PaintbrushTool : public StrokeTool {
public:
    QString id() const override { return QStringLiteral("brush"); }
    QString name() const override { return QStringLiteral("Paintbrush"); }
    QString shortcut() const override { return QStringLiteral("B"); }
    QString hint() const override
    {
        return QStringLiteral("Drag to paint. Right drag paints with the secondary colour.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        buildCommonOptions(bar);
        bar.addSeparator();
        bar.addPercent(QStringLiteral("hardness"), QStringLiteral("Hardness"),
                       int(std::lround(hardness_ * 100.0)));
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("hardness"))
            hardness_ = std::clamp(value.toInt(), 1, 100) / 100.0;
        else
            StrokeTool::optionChanged(key, value);
    }

protected:
    double hardness() const override
    {
        return antialias_ ? std::clamp(hardness_, 0.02, 0.995) : 1.0;
    }

    void applyStroke(ToolContext& context, const Rect& area) override
    {
        Mask mask = coverage();
        clipToSelection(mask, *context.doc, area);
        Rgba color = strokeColor(context);
        color.a = toByte(color.a * (1.0f / 255.0f) * float(opacity_));
        ops::colorOver(layer()->image, mask, color, blend_, area);
    }

private:
    double hardness_ = 0.75;
};

// ---------------------------------------------------------------- pencil

class PencilTool : public StrokeTool {
public:
    PencilTool() { width_ = 1.0; }

    QString id() const override { return QStringLiteral("pencil"); }
    QString name() const override { return QStringLiteral("Pencil"); }
    QString shortcut() const override { return QStringLiteral("P"); }
    QString hint() const override
    {
        return QStringLiteral("Draws single hard pixels, with no antialiasing.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addCombo(QStringLiteral("blend"), QStringLiteral("Blending"),
                     blendModeNames(), int(blend_));
    }

protected:
    bool wantsSinglePixel() const override { return true; }

    void applyStroke(ToolContext& context, const Rect& area) override
    {
        Mask mask = coverage();
        clipToSelection(mask, *context.doc, area);
        ops::colorOver(layer()->image, mask, strokeColor(context), blend_, area);
    }
};

// ---------------------------------------------------------------- eraser

class EraserTool : public StrokeTool {
public:
    EraserTool() { width_ = 24.0; }

    QString id() const override { return QStringLiteral("eraser"); }
    QString name() const override { return QStringLiteral("Eraser"); }
    QString iconName() const override { return QStringLiteral("eraser"); }
    QString shortcut() const override { return QStringLiteral("E"); }
    QString hint() const override
    {
        return QStringLiteral("Erases to transparency. Right drag erases softly.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        buildCommonOptions(bar, false);
        bar.addSeparator();
        bar.addPercent(QStringLiteral("hardness"), QStringLiteral("Hardness"),
                       int(std::lround(hardness_ * 100.0)));
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("hardness"))
            hardness_ = std::clamp(value.toInt(), 1, 100) / 100.0;
        else
            StrokeTool::optionChanged(key, value);
    }

protected:
    double hardness() const override
    {
        return antialias_ ? std::clamp(hardness_, 0.02, 0.995) : 1.0;
    }

    void applyStroke(ToolContext& context, const Rect& area) override
    {
        Mask mask = coverage();
        clipToSelection(mask, *context.doc, area);
        // The right button erases at reduced strength, which is handy for
        // feathering an edge.
        const float strength = float(opacity_) * (secondary_ ? 0.35f : 1.0f);
        ops::eraseMask(layer()->image, mask, strength, area);
    }

private:
    double hardness_ = 0.85;
};

// ---------------------------------------------------------------- clone stamp

class CloneStampTool : public StrokeTool {
public:
    CloneStampTool() { width_ = 40.0; }

    QString id() const override { return QStringLiteral("clone"); }
    QString name() const override { return QStringLiteral("Clone Stamp"); }
    QString shortcut() const override { return QStringLiteral("L"); }
    QString hint() const override
    {
        return QStringLiteral("Ctrl+click sets the source, then drag to paint copies of it.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        buildCommonOptions(bar, false);
        bar.addSeparator();
        bar.addCheck(QStringLiteral("aligned"), QStringLiteral("Aligned"), aligned_);
        bar.addCheck(QStringLiteral("merged"), QStringLiteral("Sample merged"), merged_);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("aligned"))
            aligned_ = value.toBool();
        else if (key == QLatin1String("merged"))
            merged_ = value.toBool();
        else
            StrokeTool::optionChanged(key, value);
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (event.modifiers.testFlag(Qt::ControlModifier)) {
            origin_ = event.pos;
            hasOrigin_ = true;
            context.say(QStringLiteral("Clone source set to %1, %2")
                            .arg(event.pixel.x())
                            .arg(event.pixel.y()));
            return;
        }
        if (!hasOrigin_) {
            context.say(QStringLiteral("Ctrl+click first to pick the clone source."));
            return;
        }
        StrokeTool::press(context, event);
    }

protected:
    void onStrokeStart(ToolContext& context, const ToolEvent& event) override
    {
        if (!aligned_ || !hasOffset_) {
            offset_ = origin_ - event.pos;
            hasOffset_ = true;
        }
        source_ = merged_ ? context.doc->composite() : backup();
    }

    void applyStroke(ToolContext& context, const Rect& area) override
    {
        Mask mask = coverage();
        clipToSelection(mask, *context.doc, area);
        const int dx = int(std::lround(offset_.x()));
        const int dy = int(std::lround(offset_.y()));
        Image shifted(layer()->image.width(), layer()->image.height());
        // Only the rectangle being painted needs to be lifted from the source.
        for (int y = area.y; y < area.bottom(); ++y) {
            Rgba* dst = shifted.row(y);
            for (int x = area.x; x < area.right(); ++x)
                dst[x] = source_.at(x + dx, y + dy);
        }
        ops::imageOver(layer()->image, shifted, &mask, BlendMode::Normal, area,
                       float(opacity_));
    }

private:
    QPointF origin_;
    QPointF offset_;
    Image source_;
    bool hasOrigin_ = false;
    bool hasOffset_ = false;
    bool aligned_ = true;
    bool merged_ = false;
};

// ---------------------------------------------------------------- recolor

class RecolorTool : public StrokeTool {
public:
    RecolorTool() { width_ = 30.0; }

    QString id() const override { return QStringLiteral("recolor"); }
    QString name() const override { return QStringLiteral("Recolor"); }
    QString shortcut() const override { return QStringLiteral("R"); }
    QString hint() const override
    {
        return QStringLiteral("Paints the primary colour only over pixels that match "
                              "the secondary colour.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        buildCommonOptions(bar, false);
        bar.addSeparator();
        bar.addPercent(QStringLiteral("tolerance"), QStringLiteral("Tolerance"),
                       int(std::lround(tolerance_ * 100.0)), 0, 100);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("tolerance"))
            tolerance_ = std::clamp(value.toInt(), 0, 100) / 100.0;
        else
            StrokeTool::optionChanged(key, value);
    }

protected:
    void applyStroke(ToolContext& context, const Rect& area) override
    {
        Mask mask = coverage();
        clipToSelection(mask, *context.doc, area);
        // Keep the brush coverage only where the old colour matches.
        const Rgba target = secondary_ ? context.primary : context.secondary;
        const Rgba paint = secondary_ ? context.secondary : context.primary;
        const int limit = int(std::lround(tolerance_ * 255.0));
        for (int y = area.y; y < area.bottom(); ++y) {
            float* line = mask.row(y);
            const Rgba* src = backup().row(y);
            for (int x = area.x; x < area.right(); ++x) {
                if (line[x] <= 0.0f)
                    continue;
                int diff = std::abs(int(src[x].r) - int(target.r));
                diff = std::max(diff, std::abs(int(src[x].g) - int(target.g)));
                diff = std::max(diff, std::abs(int(src[x].b) - int(target.b)));
                if (diff > limit)
                    line[x] = 0.0f;
            }
        }
        Rgba color = paint;
        color.a = toByte(color.a * (1.0f / 255.0f) * float(opacity_));
        ops::colorOver(layer()->image, mask, color, BlendMode::Normal, area);
    }

private:
    double tolerance_ = 0.2;
};

// ---------------------------------------------------------------- paint bucket

class PaintBucketTool : public Tool {
public:
    QString id() const override { return QStringLiteral("bucket"); }
    QString name() const override { return QStringLiteral("Paint Bucket"); }
    QString shortcut() const override { return QStringLiteral("F"); }
    QString hint() const override
    {
        return QStringLiteral("Fills an area of similar colour. Right click fills with "
                              "the secondary colour.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addCombo(QStringLiteral("mode"), QStringLiteral("Fill"),
                     {QStringLiteral("Contiguous"), QStringLiteral("Global")},
                     contiguous_ ? 0 : 1);
        bar.addPercent(QStringLiteral("tolerance"), QStringLiteral("Tolerance"),
                       int(std::lround(tolerance_ * 100.0)), 0, 100);
        bar.addPercent(QStringLiteral("opacity"), QStringLiteral("Opacity"),
                       int(std::lround(opacity_ * 100.0)));
        bar.addCheck(QStringLiteral("antialias"), QStringLiteral("Antialiasing"),
                     antialias_);
        bar.addCheck(QStringLiteral("merged"), QStringLiteral("Sample merged"), merged_);
        bar.addCombo(QStringLiteral("blend"), QStringLiteral("Blending"),
                     blendModeNames(), int(blend_));
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("mode"))
            contiguous_ = value.toInt() == 0;
        else if (key == QLatin1String("tolerance"))
            tolerance_ = std::clamp(value.toInt(), 0, 100) / 100.0;
        else if (key == QLatin1String("opacity"))
            opacity_ = std::clamp(value.toInt(), 1, 100) / 100.0;
        else if (key == QLatin1String("antialias"))
            antialias_ = value.toBool();
        else if (key == QLatin1String("merged"))
            merged_ = value.toBool();
        else if (key == QLatin1String("blend"))
            blend_ = static_cast<BlendMode>(std::clamp(value.toInt(), 0,
                                                      int(BlendMode::Count) - 1));
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!context.valid())
            return;
        Layer* layer = context.doc->activeLayer();
        if (!layer || !layer->image.rect().contains(event.pixel.x(), event.pixel.y()))
            return;
        const Image& sample = merged_ ? context.doc->composite() : layer->image;
        Mask mask = ops::floodFill(sample, event.pixel.x(), event.pixel.y(), tolerance_,
                                   contiguous_, antialias_);
        clipToSelection(mask, *context.doc, mask.rect());
        const Rect area = mask.boundsAbove(0.004f);
        if (area.isEmpty()) {
            context.say(QStringLiteral("Nothing to fill here."));
            return;
        }
        Rgba color = event.isSecondary() ? context.secondary : context.primary;
        color.a = toByte(color.a * (1.0f / 255.0f) * float(opacity_));
        EditScope scope(*context.doc, *context.history, layer, name(), iconName(), area);
        ops::colorOver(layer->image, mask, color, blend_, area);
        scope.commit();
        context.doc->invalidateLayer(layer);
        context.repaint(area);
        if (context.documentChanged)
            context.documentChanged();
        context.say(QStringLiteral("Filled %1 pixels").arg(mask.countAbove(0.5f)));
    }

private:
    bool contiguous_ = true;
    bool antialias_ = true;
    bool merged_ = false;
    double tolerance_ = 0.25;
    double opacity_ = 1.0;
    BlendMode blend_ = BlendMode::Normal;
};

// ---------------------------------------------------------------- gradient

class GradientTool : public Tool {
public:
    enum Shape { Linear, LinearReflected, Radial, Conical, Diamond };

    QString id() const override { return QStringLiteral("gradient"); }
    QString name() const override { return QStringLiteral("Gradient"); }
    QString shortcut() const override { return QStringLiteral("G"); }
    QString hint() const override
    {
        return QStringLiteral("Drag to lay down a gradient. Hold Shift to constrain "
                              "the angle, right drag to swap the colours.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addCombo(QStringLiteral("shape"), QStringLiteral("Shape"),
                     {QStringLiteral("Linear"), QStringLiteral("Linear reflected"),
                      QStringLiteral("Radial"), QStringLiteral("Conical"),
                      QStringLiteral("Diamond")},
                     int(shape_));
        bar.addCombo(QStringLiteral("channel"), QStringLiteral("Mode"),
                     {QStringLiteral("Colour"), QStringLiteral("Transparency")},
                     transparencyMode_ ? 1 : 0);
        bar.addPercent(QStringLiteral("opacity"), QStringLiteral("Opacity"),
                       int(std::lround(opacity_ * 100.0)));
        bar.addCheck(QStringLiteral("dither"), QStringLiteral("Dither"), dither_);
        bar.addCombo(QStringLiteral("blend"), QStringLiteral("Blending"),
                     blendModeNames(), int(blend_));
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("shape"))
            shape_ = static_cast<Shape>(std::clamp(value.toInt(), 0, 4));
        else if (key == QLatin1String("channel"))
            transparencyMode_ = value.toInt() == 1;
        else if (key == QLatin1String("opacity"))
            opacity_ = std::clamp(value.toInt(), 1, 100) / 100.0;
        else if (key == QLatin1String("dither"))
            dither_ = value.toBool();
        else if (key == QLatin1String("blend"))
            blend_ = static_cast<BlendMode>(std::clamp(value.toInt(), 0,
                                                      int(BlendMode::Count) - 1));
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!context.valid())
            return;
        Layer* layer = context.doc->activeLayer();
        if (!layer)
            return;
        layer_ = layer;
        layerUid_ = layer->uid();
        backup_ = layer->image;
        start_ = event.pos;
        end_ = event.pos;
        reversed_ = event.isSecondary();
        busy_ = true;
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!busy_)
            return;
        end_ = event.modifiers.testFlag(Qt::ShiftModifier) ? constrain(start_, event.pos)
                                                           : event.pos;
        render(context);
    }

    void release(ToolContext& context, const ToolEvent& event) override
    {
        if (!busy_)
            return;
        busy_ = false;
        render(context);
        const Rect area = affectedArea(context);
        Image before = backup_.cropped(area);
        Image after = layer_->image.cropped(area);
        context.history->push(std::make_unique<PixelEdit>(name(), iconName(), layerUid_,
                                                          area, std::move(before),
                                                          std::move(after)));
        context.doc->invalidateLayer(layer_);
        if (context.documentChanged)
            context.documentChanged();
        layer_ = nullptr;
        backup_ = Image();
    }

    void cancel(ToolContext& context) override
    {
        if (!busy_ || !layer_)
            return;
        busy_ = false;
        layer_->image = backup_;
        context.doc->invalidateLayer(layer_);
        context.repaintAll();
        layer_ = nullptr;
        backup_ = Image();
    }

    bool isBusy() const override { return busy_; }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!busy_)
            return;
        painter.setPen(overlayPen(context.zoom, QColor(255, 255, 255, 220), 1.2));
        painter.drawLine(start_, end_);
        painter.setPen(overlayPen(context.zoom, QColor(0, 0, 0, 160), 1.2,
                                  Qt::DashLine));
        painter.drawLine(start_, end_);
        const double handle = 3.0 / std::max(0.05, context.zoom);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(overlayPen(context.zoom, QColor(255, 255, 255, 220), 1.2));
        painter.drawEllipse(start_, handle, handle);
        painter.drawEllipse(end_, handle, handle);
    }

private:
    static QPointF constrain(QPointF from, QPointF to)
    {
        const double dx = to.x() - from.x();
        const double dy = to.y() - from.y();
        const double length = std::hypot(dx, dy);
        if (length < 1.0e-6)
            return to;
        const double angle = std::round(std::atan2(dy, dx) / (M_PI / 8.0)) * (M_PI / 8.0);
        return QPointF(from.x() + std::cos(angle) * length,
                       from.y() + std::sin(angle) * length);
    }

    Rect affectedArea(const ToolContext& context) const
    {
        const Rect bounds = context.doc->selection().isActive()
            ? context.doc->selection().bounds()
            : layer_->image.rect();
        return bounds.intersected(layer_->image.rect());
    }

    double parameterAt(double x, double y) const
    {
        const double dx = end_.x() - start_.x();
        const double dy = end_.y() - start_.y();
        const double length2 = dx * dx + dy * dy;
        const double px = x - start_.x();
        const double py = y - start_.y();
        switch (shape_) {
        case Radial: {
            const double length = std::sqrt(length2);
            if (length < 1.0e-6)
                return 1.0;
            return std::hypot(px, py) / length;
        }
        case Conical: {
            const double base = std::atan2(dy, dx);
            double angle = std::atan2(py, px) - base;
            while (angle < -M_PI)
                angle += 2.0 * M_PI;
            while (angle > M_PI)
                angle -= 2.0 * M_PI;
            return std::abs(angle) / M_PI;
        }
        case Diamond: {
            const double length = std::sqrt(length2);
            if (length < 1.0e-6)
                return 1.0;
            const double ux = dx / length, uy = dy / length;
            const double along = px * ux + py * uy;
            const double across = -px * uy + py * ux;
            return (std::abs(along) + std::abs(across)) / length;
        }
        case LinearReflected: {
            if (length2 < 1.0e-9)
                return 1.0;
            return std::abs((px * dx + py * dy) / length2);
        }
        case Linear:
        default:
            if (length2 < 1.0e-9)
                return 1.0;
            return (px * dx + py * dy) / length2;
        }
    }

    void render(ToolContext& context)
    {
        if (!layer_)
            return;
        const Rect area = affectedArea(context);
        if (area.isEmpty())
            return;
        layer_->image.copyRectFrom(backup_, area);

        Rgba from = reversed_ ? context.secondary : context.primary;
        Rgba to = reversed_ ? context.primary : context.secondary;
        const Mask* clip = context.doc->selection().clipMask();

        if (transparencyMode_) {
            // Fade the existing pixels out instead of painting a colour.
            for (int y = area.y; y < area.bottom(); ++y) {
                Rgba* line = layer_->image.row(y);
                const float* sel = clip ? clip->row(y) : nullptr;
                for (int x = area.x; x < area.right(); ++x) {
                    double t = std::clamp(parameterAt(x + 0.5, y + 0.5), 0.0, 1.0);
                    if (dither_)
                        t = dithered(t, x, y);
                    const float keep = float(1.0 - t * opacity_);
                    const float selWeight = sel ? sel[x] : 1.0f;
                    const float factor = 1.0f - (1.0f - keep) * selWeight;
                    line[x].a = toByte(line[x].a * (1.0f / 255.0f) * factor);
                }
            }
        } else {
            Image gradient(layer_->image.width(), layer_->image.height());
            for (int y = area.y; y < area.bottom(); ++y) {
                Rgba* line = gradient.row(y);
                for (int x = area.x; x < area.right(); ++x) {
                    double t = std::clamp(parameterAt(x + 0.5, y + 0.5), 0.0, 1.0);
                    if (dither_)
                        t = dithered(t, x, y);
                    line[x] = Rgba(toByte(float(from.r + (to.r - from.r) * t) / 255.0f),
                                   toByte(float(from.g + (to.g - from.g) * t) / 255.0f),
                                   toByte(float(from.b + (to.b - from.b) * t) / 255.0f),
                                   toByte(float(from.a + (to.a - from.a) * t) / 255.0f));
                }
            }
            Mask mask(layer_->image.width(), layer_->image.height(), 1.0f);
            clipToSelection(mask, *context.doc, area);
            ops::imageOver(layer_->image, gradient, &mask, blend_, area,
                           float(opacity_));
        }
        context.doc->invalidateLayer(layer_);
        context.repaint(area);
    }

    static double dithered(double t, int x, int y)
    {
        // A small ordered pattern breaks up banding in long gradients.
        static const int pattern[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6},
                                          {3, 11, 1, 9}, {15, 7, 13, 5}};
        const double offset = (pattern[y & 3][x & 3] / 16.0 - 0.5) / 255.0;
        return std::clamp(t + offset, 0.0, 1.0);
    }

    Shape shape_ = Linear;
    bool transparencyMode_ = false;
    bool dither_ = true;
    double opacity_ = 1.0;
    BlendMode blend_ = BlendMode::Normal;
    QPointF start_;
    QPointF end_;
    Image backup_;
    Layer* layer_ = nullptr;
    int layerUid_ = 0;
    bool busy_ = false;
    bool reversed_ = false;
};

} // namespace

void registerPaintTools(ToolRegistry& registry)
{
    registry.add(std::make_unique<PaintbrushTool>());
    registry.add(std::make_unique<PencilTool>());
    registry.add(std::make_unique<EraserTool>());
    registry.add(std::make_unique<PaintBucketTool>());
    registry.add(std::make_unique<GradientTool>());
    registry.add(std::make_unique<CloneStampTool>());
    registry.add(std::make_unique<RecolorTool>());
}

} // namespace tessel
