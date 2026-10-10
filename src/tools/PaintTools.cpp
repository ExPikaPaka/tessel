// Pencil, paintbrush, eraser, paint bucket, gradient, clone stamp and recolor.
#include "tools/Tool.h"

#include <cmath>
#include <vector>

#include <QComboBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QPointer>
#include <QSettings>

#include "core/ImageOps.h"
#include "core/Parallel.h"
#include "core/Strings.h"

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

// Paint.NET's Overwrite: the stroke colour and its coverage become the pixel
// instead of being composited over it, so a soft brush can fade whatever is
// under it out to that colour rather than only adding to it.
void colorReplace(Image& dst, const Mask& mask, Rgba color, const Rect& region)
{
    const Rect area = region.intersected(dst.rect()).intersected(mask.rect());
    if (area.isEmpty())
        return;
    const float colorAlpha = color.a * (1.0f / 255.0f);
    for (int y = area.y; y < area.bottom(); ++y) {
        Rgba* line = dst.row(y);
        const float* m = mask.row(y);
        for (int x = area.x; x < area.right(); ++x) {
            if (m[x] <= 0.0f)
                continue;
            line[x] = Rgba(color.r, color.g, color.b, toByte(m[x] * colorAlpha));
        }
    }
}

// Antialiasing used to be a yes or no switch. These are the steps that
// replaced it, from no ramp beyond the unavoidable one to the widest ramp
// worth having.
QStringList antialiasLevelNames()
{
    return {
        QCoreApplication::translate("Tools", "None"),
        QCoreApplication::translate("Tools", "Low"),
        QCoreApplication::translate("Tools", "Normal"),
        QCoreApplication::translate("Tools", "High"),
        QCoreApplication::translate("Tools", "Maximum"),
    };
}

// The Blending box of the stroke tools: the layer blend modes, and one entry
// past them for Overwrite, which is not a blend at all.
QStringList strokeBlendNames()
{
    QStringList out = blendModeLabels();
    out.append(QCoreApplication::translate("Tools", "Overwrite"));
    return out;
}

// Tablet pressure maps straight onto the setting it drives, with a floor so
// the lightest touch the pen reports still leaves a mark rather than nothing.
double pressureScale(double pressure)
{
    return 0.08 + 0.92 * std::clamp(pressure, 0.0, 1.0);
}

// Weighted average of the last few pointer positions, the newest weighing the
// most so the line follows the hand instead of trailing behind it. A window of
// one point is the raw position, which is how stabilisation zero stays exactly
// what the stroke engine used to be handed.
class PointSmoother {
public:
    void setWindow(int window) { window_ = std::max(1, window); }

    void reset(QPointF at)
    {
        points_.clear();
        points_.push_back(at);
    }

    QPointF push(QPointF at)
    {
        points_.push_back(at);
        while (int(points_.size()) > window_)
            points_.erase(points_.begin());
        if (window_ <= 1 || points_.size() < 2)
            return at;
        double x = 0.0, y = 0.0, total = 0.0;
        for (size_t i = 0; i < points_.size(); ++i) {
            const double weight = double(i + 1);
            x += points_[i].x() * weight;
            y += points_[i].y() * weight;
            total += weight;
        }
        return QPointF(x / total, y / total);
    }

private:
    std::vector<QPointF> points_;
    int window_ = 1;
};

// Bilinear reading of a mask, zero outside it.
float sampleMask(const Mask& mask, double x, double y)
{
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = float(x - x0), fy = float(y - y0);
    const float top =
        mask.at(x0, y0) + (mask.at(x0 + 1, y0) - mask.at(x0, y0)) * fx;
    const float bottom =
        mask.at(x0, y0 + 1) + (mask.at(x0 + 1, y0 + 1) - mask.at(x0, y0 + 1)) * fx;
    return top + (bottom - top) * fy;
}

// Fits a loaded tip to the brush width and bakes the hardness into it, so the
// stroke itself only has to stamp the result. QImage does the scaling because
// it averages when it shrinks, and a tip file is usually far larger than the
// brush: sampling such a tip directly came out speckled.
Mask buildTipStamp(const Mask& source, double diameter, double hardness)
{
    if (source.isNull())
        return Mask();
    const double hard = std::clamp(hardness, 0.0, 0.999);
    const double side = std::max(1.0, diameter);
    const double scale = side / std::max(source.width(), source.height());
    const int tipW = std::max(1, int(std::lround(source.width() * scale)));
    const int tipH = std::max(1, int(std::lround(source.height() * scale)));

    // Carried through a premultiplied image with the coverage in every
    // channel, which every QImage scaling path handles the same way.
    QImage carrier(source.width(), source.height(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        const float* in = source.row(y);
        QRgb* out = reinterpret_cast<QRgb*>(carrier.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const int v = int(std::clamp(in[x], 0.0f, 1.0f) * 255.0f + 0.5f);
            out[x] = qRgba(v, v, v, v);
        }
    }
    const QImage scaled = carrier.scaled(tipW, tipH, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);

    // The stamp is square and a little larger than the brush, which leaves the
    // blurred edge of a soft tip somewhere to fade out.
    const int size = int(std::ceil(side)) + 4;
    Mask stamp(size, size);
    const int offsetX = (size - scaled.width()) / 2;
    const int offsetY = (size - scaled.height()) / 2;
    for (int y = 0; y < scaled.height(); ++y) {
        const QRgb* in = reinterpret_cast<const QRgb*>(scaled.constScanLine(y));
        float* out = stamp.row(y + offsetY);
        for (int x = 0; x < scaled.width(); ++x)
            out[x + offsetX] = qAlpha(in[x]) * (1.0f / 255.0f);
    }

    // Hardness fades the edge across the same band the round brush fades
    // across, and the same density rule applies, so a soft custom tip is a
    // haze that builds up between strokes rather than one flat coat.
    const double blur = (1.0 - hard) * side * 0.5;
    if (blur > 0.25)
        stamp = ops::blurMask(stamp, blur);
    const double density = 0.10 + 0.90 * std::min(1.0, hard / 0.35);
    if (density < 1.0) {
        for (int y = 0; y < stamp.height(); ++y) {
            float* line = stamp.row(y);
            for (int x = 0; x < stamp.width(); ++x)
                line[x] *= float(density);
        }
    }
    return stamp;
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
        dirty_ = Rect();
        secondary_ = event.isSecondary();
        busy_ = true;
        last_ = event.pos;
        pressure_ = std::clamp(event.pressure, 0.0, 1.0);
        smoother_.reset(event.pos);
        carry_ = 0.0;
        prepareTip();
        onStrokeStart(context, event);
        addStroke(event.pos, event.pos, pressure_, pressure_);
        flush(context, stampBounds(event.pos, event.pos));
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!busy_ || !layer_)
            return;
        const QPointF from = last_;
        const QPointF to = smoother_.push(event.pos);
        const double pressure = std::clamp(event.pressure, 0.0, 1.0);
        addStroke(from, to, pressure_, pressure);
        last_ = to;
        pressure_ = pressure;
        flush(context, stampBounds(from, to));
    }

    void release(ToolContext& context, const ToolEvent& event) override
    {
        if (!busy_ || !layer_)
            return;
        // Stabilisation keeps the line a little behind the pointer, so the
        // last stretch runs to the raw release position: the stroke has to end
        // where the pointer came up, not where the average had got to.
        if (event.pos != last_) {
            // A pen reports no pressure at all as it leaves the tablet, which
            // would taper that last stretch away to nothing.
            const double pressure = event.pressure > 0.0
                ? std::clamp(event.pressure, 0.0, 1.0)
                : pressure_;
            const QPointF from = last_;
            addStroke(from, event.pos, pressure_, pressure);
            last_ = event.pos;
            flush(context, stampBounds(from, event.pos));
        }
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
    // Antialiasing level. Zero is what the old check box did when it was
    // unchecked: the hardness slider is ignored and the edge is as crisp as
    // the field allows. The steps above it say how narrow a ramp the edge may
    // come down to, which is the only part of a hard brush where antialiasing
    // shows at all. Level two is what the checked box used to do, so the
    // default brush is unchanged.
    static constexpr int kAntialiasLevels = 4;
    static constexpr int kDefaultAntialiasLevel = 2;
    // How many positions the strongest stabilisation averages over.
    static constexpr int kMaxSmoothing = 16;

    virtual double diameter() const { return width_; }
    virtual double hardness() const { return antialiased() ? 0.72 : 1.0; }
    virtual float strength() const { return 1.0f; }
    virtual bool wantsSinglePixel() const { return false; }
    // The brush tip this tool paints with. Null is the built in round tip,
    // which keeps the swept distance field to itself.
    virtual const Mask* tipSource() const { return nullptr; }
    virtual void onStrokeStart(ToolContext&, const ToolEvent&) {}
    // Paints `coverage_` into the layer for the given rectangle, starting from
    // the untouched backup so repeated passes stay idempotent.
    virtual void applyStroke(ToolContext& context, const Rect& area) = 0;

    bool antialiased() const { return antialiasLevel_ > 0; }

    double edgeRamp() const
    {
        static constexpr double ramps[kAntialiasLevels + 1] = {0.5, 0.35, 0.5,
                                                               0.7, 1.0};
        return ramps[std::clamp(antialiasLevel_, 0, kAntialiasLevels)];
    }

    Rgba strokeColor(const ToolContext& context) const
    {
        return secondary_ ? context.secondary : context.primary;
    }

    // Lays the stroke colour into the layer the way the Blending box asks.
    void paintColor(Mask& mask, Rgba color, const Rect& area)
    {
        if (overwrite_)
            colorReplace(layer_->image, mask, color, area);
        else
            ops::colorOver(layer_->image, mask, color, blend_, area);
    }

    // Both pressure mappings are off unless the user asks for them, and a
    // mouse reports a full press, so a mouse stroke is unaffected either way.
    double widthScale(double pressure) const
    {
        return pressureWidth_ ? pressureScale(pressure) : 1.0;
    }

    float amountFor(double pressure) const
    {
        return pressureOpacity_ ? float(strength() * pressureScale(pressure))
                                : strength();
    }

    void addStroke(QPointF from, QPointF to, double pressureFrom, double pressureTo)
    {
        if (wantsSinglePixel()) {
            if (from == to) {
                maxAt(int(std::floor(from.x())), int(std::floor(from.y())),
                      amountFor(pressureTo));
            } else {
                drawPixelLine(from, to, amountFor(pressureFrom),
                              amountFor(pressureTo));
            }
            return;
        }
        if (!tipStamp_.isNull()) {
            stampTipAlong(from, to, pressureFrom, pressureTo);
            return;
        }
        accumulateSegment(from, to, pressureFrom, pressureTo);
    }

    // Coverage of the brush swept along one segment, measured as the true
    // distance from each pixel to that segment.
    //
    // This used to stamp a dab every few pixels and take the maximum. The
    // maximum of two offset cones dips between their centres, so a soft brush
    // laid down a visible string of beads. A swept distance field has no such
    // ripple.
    //
    // One stroke lays down one coat of paint. Everywhere the stroke covers,
    // including the places it passes over twice, the coverage is the most the
    // brush ever offered there, and the whole coat is composited onto the
    // layer once. Anything else shows: adding the second visit to the first
    // darkens a band whose edge falls wherever the rule changed its mind, and
    // that edge is the crescent that used to appear along fast strokes and
    // through crossings. Paint builds up between strokes instead, which is
    // where compositing already does it, and what Paint.NET does too.
    void accumulateSegment(QPointF from, QPointF to, double pressureFrom,
                           double pressureTo)
    {
        const double full = std::max(0.5, diameter() / 2.0);
        // Pressure narrows the brush along the segment, so the radius and the
        // pigment are read off the distance travelled rather than fixed for
        // the whole run. With both switches off the two ends agree and the
        // arithmetic is the same as it ever was.
        const double radiusFrom = std::max(0.5, full * widthScale(pressureFrom));
        const double radiusTo = std::max(0.5, full * widthScale(pressureTo));
        const double hard = std::clamp(hardness(), 0.0, 0.999);
        const float amountFrom = amountFor(pressureFrom);
        const float amountTo = amountFor(pressureTo);
        const bool varying = radiusFrom != radiusTo || amountFrom != amountTo;
        // Half a pixel of ramp even at maximum hardness, so edges stay
        // antialiased instead of turning into stair steps. The antialiasing
        // level widens that floor.
        const double ramp = edgeRamp();
        const double fallFrom = std::max(radiusFrom - radiusFrom * hard, ramp);
        // Shape of the falloff across that band. A straight ramp meets the
        // centre line at an angle and the rim at an angle, so a soft brush
        // came out as a flat grey strip with a spine down the middle rather
        // than as a blur; raising the ramp to a power only moved the angle.
        // A bell is flat at both ends: coverage leaves the centre smoothly,
        // spreads the fade across the whole radius, and arrives at the rim
        // with nothing to see. That is what makes a soft brush look blurred.
        // At high hardness the band is a fraction of a pixel wide, so the
        // curve across it makes no difference and the edge stays crisp.
        constexpr double kBell = 4.5;
        const double pedestal = std::exp(-kBell);
        const double bellScale = 1.0 / (1.0 - pedestal);
        // How much pigment one stroke lays down. A hard brush covers
        // completely, but the softest one is a haze that has to be built up
        // over several strokes, which is what a real soft brush does and what
        // makes the lowest settings worth having. Anything at or above roughly
        // a third hardness still covers fully in one go, so the default brush
        // is unchanged.
        const double density = 0.10 + 0.90 * std::min(1.0, hard / 0.35);

        const Rect box = stampBounds(from, to).intersected(coverage_.rect());
        const double abx = to.x() - from.x();
        const double aby = to.y() - from.y();
        const double lengthSq = abx * abx + aby * aby;
        if (box.isEmpty())
            return;

        parallelFor(box.y, box.bottom(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* line = coverage_.row(y);
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
                    double radius = radiusFrom;
                    double falloff = fallFrom;
                    float amount = amountFrom;
                    if (varying) {
                        radius = radiusFrom + (radiusTo - radiusFrom) * along;
                        falloff = std::max(radius - radius * hard, ramp);
                        amount = amountFrom + float(along) * (amountTo - amountFrom);
                    }
                    double across = (radius - distance) / falloff;
                    if (across <= 0.0)
                        continue;
                    across = std::min(1.0, across);
                    const double rest = 1.0 - across;
                    const double coverage =
                        (std::exp(-kBell * rest * rest) - pedestal) * bellScale;
                    const float value = float(coverage * density) * amount;
                    if (value > line[x])
                        line[x] = value;
                }
            }
        }, 32);
    }

    // A custom tip is stamped along the segment with a gap small enough that
    // neighbouring stamps overlap well inside their soft edges, which is what
    // keeps the stroke from coming out as a string of beads. The distance left
    // over carries into the next segment, so the rhythm does not restart every
    // time the pointer reports a new position.
    void stampTipAlong(QPointF from, QPointF to, double pressureFrom,
                       double pressureTo)
    {
        // A twelfth of the width the stamps are actually being laid down at,
        // which is a gap pressure cannot open up by thinning them.
        const double step = std::max(0.5, diameter() * 0.08
            * std::min(widthScale(pressureFrom), widthScale(pressureTo)));
        const double abx = to.x() - from.x();
        const double aby = to.y() - from.y();
        const double length = std::hypot(abx, aby);
        if (length < 1.0e-9) {
            stampTip(from, pressureTo);
            carry_ = step;
            return;
        }
        double at = carry_;
        while (at <= length) {
            const double t = at / length;
            stampTip(QPointF(from.x() + abx * t, from.y() + aby * t),
                     pressureFrom + (pressureTo - pressureFrom) * t);
            at += step;
        }
        carry_ = at - length;
    }

    // One stamp of the prepared tip, taken into the coverage with a maximum so
    // one stroke still lays down one coat however much the stamps overlap. The
    // tip is prepared once at the full brush width and pressure shrinks it
    // here, because a smooth stamp loses nothing worth seeing when it is read
    // at a smaller size, while preparing one per stamp would mean a blur per
    // stamp.
    void stampTip(QPointF at, double pressure)
    {
        const double scale = widthScale(pressure);
        if (scale <= 0.0)
            return;
        const double side = tipStamp_.width() * scale;
        const float amount = amountFor(pressure);
        const double left = at.x() - side / 2.0;
        const double top = at.y() - side / 2.0;
        const int span = int(std::ceil(side)) + 2;
        const Rect box = Rect(int(std::floor(left)), int(std::floor(top)), span, span)
                             .intersected(coverage_.rect());
        if (box.isEmpty())
            return;
        for (int y = box.y; y < box.bottom(); ++y) {
            float* line = coverage_.row(y);
            const double v = (y + 0.5 - top) / scale - 0.5;
            for (int x = box.x; x < box.right(); ++x) {
                const double u = (x + 0.5 - left) / scale - 0.5;
                const float value = sampleMask(tipStamp_, u, v) * amount;
                if (value > line[x])
                    line[x] = value;
            }
        }
    }

    // Bresenham, so the pencil stays exactly one pixel wide and aliased.
    void drawPixelLine(QPointF from, QPointF to, float amountFrom, float amountTo)
    {
        int x0 = int(std::floor(from.x())), y0 = int(std::floor(from.y()));
        const int x1 = int(std::floor(to.x())), y1 = int(std::floor(to.y()));
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        const int span = std::max(dx, -dy);
        int error = dx + dy;
        int step = 0;
        while (true) {
            const float t = span > 0 ? float(step) / float(span) : 1.0f;
            maxAt(x0, y0, amountFrom + (amountTo - amountFrom) * t);
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
            ++step;
        }
    }

    Rect stampBounds(QPointF a, QPointF b) const
    {
        // A custom tip's stamp is a shade wider than the brush so its soft
        // edge has room, and sampling it reaches half a pixel wider again.
        const double pad = wantsSinglePixel()
            ? 2.0
            : diameter() / 2.0 + (tipStamp_.isNull() ? 2.0 : 4.0);
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
        bar.addSpin(QStringLiteral("width"),
                    QCoreApplication::translate("Tools", "Width"), 1, 2000,
                    int(std::lround(width_)), QStringLiteral(" px"));
        bar.addPercent(QStringLiteral("opacity"),
                       QCoreApplication::translate("Tools", "Opacity"),
                       int(std::lround(opacity_ * 100.0)));
        bar.addCombo(QStringLiteral("antialias"),
                     QCoreApplication::translate("Tools", "Antialiasing"),
                     antialiasLevelNames(), antialiasLevel_);
        if (withBlendMode) {
            bar.addCombo(QStringLiteral("blend"),
                         QCoreApplication::translate("Tools", "Blending"),
                         strokeBlendNames(),
                         overwrite_ ? int(blendModeNames().size()) : int(blend_));
        }
    }

    // The two pressure switches, both off by default so a mouse behaves as it
    // always has.
    void buildPressureOptions(ToolOptionsBar& bar, bool withWidth = true)
    {
        bar.addSeparator();
        bar.addLabel(QCoreApplication::translate("Tools", "Pressure"));
        if (withWidth) {
            bar.addTextToggle(QStringLiteral("pressureWidth"),
                              QCoreApplication::translate("Tools", "Width"),
                              QCoreApplication::translate(
                                  "Tools", "Tablet pressure sets how wide the "
                                           "brush is"),
                              pressureWidth_);
        }
        bar.addTextToggle(QStringLiteral("pressureOpacity"),
                          QCoreApplication::translate("Tools", "Opacity"),
                          QCoreApplication::translate(
                              "Tools", "Tablet pressure sets how opaque the "
                                       "stroke is"),
                          pressureOpacity_);
    }

    void buildStabiliseOption(ToolOptionsBar& bar)
    {
        bar.addPercent(QStringLiteral("stabilise"),
                       QCoreApplication::translate("Tools", "Stabilisation"),
                       stabilisation_, 0, 100);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("width"))
            width_ = std::max(1, value.toInt());
        else if (key == QLatin1String("opacity"))
            opacity_ = std::clamp(value.toInt(), 1, 100) / 100.0;
        else if (key == QLatin1String("antialias")) {
            // This was a check box, so a plain yes or no still means off or
            // the level the box used to stand for.
            antialiasLevel_ = value.typeId() == QMetaType::Bool
                ? (value.toBool() ? kDefaultAntialiasLevel : 0)
                : std::clamp(value.toInt(), 0, kAntialiasLevels);
        } else if (key == QLatin1String("blend")) {
            // One entry past the blend modes is Overwrite, which is not a
            // blend mode: it replaces what is under the brush.
            const int modes = int(blendModeNames().size());
            const int index = std::clamp(value.toInt(), 0, modes);
            overwrite_ = index == modes;
            if (!overwrite_)
                blend_ = static_cast<BlendMode>(index);
        } else if (key == QLatin1String("pressureWidth"))
            pressureWidth_ = value.toBool();
        else if (key == QLatin1String("pressureOpacity"))
            pressureOpacity_ = value.toBool();
        else if (key == QLatin1String("stabilise"))
            setStabilisation(value.toInt());
    }

    void setStabilisation(int amount)
    {
        stabilisation_ = std::clamp(amount, 0, 100);
        smoother_.setWindow(
            1 + int(std::lround(stabilisation_ / 100.0 * (kMaxSmoothing - 1))));
    }

    void prepareTip()
    {
        const Mask* source = tipSource();
        tipStamp_ = source && !source->isNull()
            ? buildTipStamp(*source, diameter(), hardness())
            : Mask();
    }

    Mask& coverage() { return coverage_; }
    const Image& backup() const { return backup_; }
    Layer* layer() const { return layer_; }

    double width_ = 12.0;
    double opacity_ = 1.0;
    int antialiasLevel_ = kDefaultAntialiasLevel;
    BlendMode blend_ = BlendMode::Normal;
    bool overwrite_ = false;
    bool secondary_ = false;
    bool pressureWidth_ = false;
    bool pressureOpacity_ = false;
    int stabilisation_ = 0;

private:
    void maxAt(int x, int y, float value)
    {
        if (value > coverage_.at(x, y))
            coverage_.setAt(x, y, value);
    }

    Layer* layer_ = nullptr;
    int layerUid_ = 0;
    Image backup_;
    Mask coverage_;
    Mask tipStamp_;
    PointSmoother smoother_;
    QPointF last_;
    Rect dirty_;
    double pressure_ = 1.0;
    double carry_ = 0.0;
    bool busy_ = false;
};

// ---------------------------------------------------------------- paintbrush

class PaintbrushTool : public StrokeTool {
public:
    QString id() const override { return QStringLiteral("brush"); }
    QString name() const override
    {
        return QCoreApplication::translate("Tools", "Paintbrush");
    }
    QString shortcut() const override { return QStringLiteral("B"); }
    QString hint() const override
    {
        return QCoreApplication::translate(
            "Tools", "Drag to paint. Right drag paints with the secondary colour.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        syncTip();
        buildCommonOptions(bar);
        bar.addSeparator();
        bar.addPercent(QStringLiteral("hardness"),
                       QCoreApplication::translate("Tools", "Hardness"),
                       int(std::lround(hardness_ * 100.0)));
        buildStabiliseOption(bar);
        bar.addSeparator();
        tipCombo_ = bar.addCombo(QStringLiteral("tip"),
                                 QCoreApplication::translate("Tools", "Tip"),
                                 tipItems(), tipIndex());
        buildPressureOptions(bar);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("hardness"))
            hardness_ = std::clamp(value.toInt(), 1, 100) / 100.0;
        else if (key == QLatin1String("tip"))
            chooseTip(value.toInt());
        else if (key == QLatin1String("tipFile"))
            // A path rather than a position in the box, which is how a tip can
            // be set without the box being there to pick it from.
            loadTip(value.toString());
        else
            StrokeTool::optionChanged(key, value);
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        syncTip();
        StrokeTool::press(context, event);
    }

protected:
    double hardness() const override
    {
        return antialiased() ? std::clamp(hardness_, 0.02, 0.995) : 1.0;
    }

    const Mask* tipSource() const override
    {
        return tip_.isNull() ? nullptr : &tip_;
    }

    void applyStroke(ToolContext& context, const Rect& area) override
    {
        Mask mask = coverage();
        clipToSelection(mask, *context.doc, area);
        Rgba color = strokeColor(context);
        color.a = toByte(color.a * (1.0f / 255.0f) * float(opacity_));
        paintColor(mask, color, area);
    }

private:
    // Round first, then the remembered tips, then the way to add one.
    QStringList tipItems() const
    {
        QStringList items{QCoreApplication::translate("Tools", "Round")};
        for (const QString& path : brushtips::remembered())
            items.append(QFileInfo(path).completeBaseName());
        items.append(QCoreApplication::translate("Tools", "Load from file..."));
        return items;
    }

    int tipIndex() const
    {
        const int found = int(brushtips::remembered().indexOf(tipPath_));
        return found < 0 ? 0 : found + 1;
    }

    void chooseTip(int index)
    {
        const QStringList tips = brushtips::remembered();
        if (index <= 0)
            brushtips::choose(QString());
        else if (index <= tips.size())
            brushtips::remember(tips.at(index - 1));
        else
            browseForTip();
        syncTip();
    }

    void loadTip(const QString& path)
    {
        if (!path.isEmpty() && brushtips::remember(path))
            syncTip();
    }

    // The last entry of the box opens a file chooser, and a tip picked there
    // joins the list so it can be chosen again without the chooser.
    void browseForTip()
    {
        const QString path = QFileDialog::getOpenFileName(
            tipCombo_ ? tipCombo_->window() : nullptr,
            QCoreApplication::translate("Tools", "Choose a brush tip"), QString(),
            QCoreApplication::translate("Tools", "Brush tips (*.png *.PNG)"));
        const int before = int(brushtips::remembered().size());
        if (!path.isEmpty())
            loadTip(path);
        if (!tipCombo_)
            return;
        if (int(brushtips::remembered().size()) > before)
            tipCombo_->insertItem(before + 1, QFileInfo(tipPath_).completeBaseName());
        // Whether or not a tip was picked, the box has to end up showing the
        // tip actually in use rather than the chooser entry.
        tipCombo_->setCurrentIndex(tipIndex());
    }

    // The settings hold which tip is in use, so this is also how a fresh run
    // starts out with the tip the last one ended with. A file that has gone
    // missing leaves the path in place and the stamp empty, which is the round
    // tip, rather than being read again on every stroke.
    void syncTip()
    {
        const QString path = brushtips::chosen();
        if (path == tipPath_)
            return;
        tipPath_ = path;
        tip_ = path.isEmpty() ? Mask() : brushtips::load(path);
    }

    double hardness_ = 0.75;
    Mask tip_;
    QString tipPath_;
    QPointer<QComboBox> tipCombo_;
};

// ---------------------------------------------------------------- pencil

class PencilTool : public StrokeTool {
public:
    PencilTool() { width_ = 1.0; }

    QString id() const override { return QStringLiteral("pencil"); }
    QString name() const override
    {
        return QCoreApplication::translate("Tools", "Pencil");
    }
    QString shortcut() const override { return QStringLiteral("P"); }
    QString hint() const override
    {
        return QCoreApplication::translate(
            "Tools", "Draws single hard pixels, with no antialiasing.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addCombo(QStringLiteral("blend"),
                     QCoreApplication::translate("Tools", "Blending"),
                     strokeBlendNames(),
                     overwrite_ ? int(blendModeNames().size()) : int(blend_));
        buildStabiliseOption(bar);
        // The nib is a single pixel, so there is no width for pressure to
        // scale; it only has the pigment to work on.
        buildPressureOptions(bar, false);
    }

protected:
    bool wantsSinglePixel() const override { return true; }

    void applyStroke(ToolContext& context, const Rect& area) override
    {
        Mask mask = coverage();
        clipToSelection(mask, *context.doc, area);
        paintColor(mask, strokeColor(context), area);
    }
};

// ---------------------------------------------------------------- eraser

class EraserTool : public StrokeTool {
public:
    EraserTool() { width_ = 24.0; }

    QString id() const override { return QStringLiteral("eraser"); }
    QString name() const override
    {
        return QCoreApplication::translate("Tools", "Eraser");
    }
    QString iconName() const override { return QStringLiteral("eraser"); }
    QString shortcut() const override { return QStringLiteral("E"); }
    QString hint() const override
    {
        return QCoreApplication::translate(
            "Tools", "Erases to transparency. Right drag erases softly.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        buildCommonOptions(bar, false);
        bar.addSeparator();
        bar.addPercent(QStringLiteral("hardness"),
                       QCoreApplication::translate("Tools", "Hardness"),
                       int(std::lround(hardness_ * 100.0)));
        buildStabiliseOption(bar);
        buildPressureOptions(bar);
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
        return antialiased() ? std::clamp(hardness_, 0.02, 0.995) : 1.0;
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
    QString name() const override
    {
        return QCoreApplication::translate("Tools", "Clone Stamp");
    }
    QString shortcut() const override { return QStringLiteral("L"); }
    QString hint() const override
    {
        return QCoreApplication::translate(
            "Tools", "Ctrl+click sets the source, then drag to paint copies of it.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        buildCommonOptions(bar, false);
        bar.addSeparator();
        bar.addCheck(QStringLiteral("aligned"),
                     QCoreApplication::translate("Tools", "Aligned"), aligned_);
        bar.addCheck(QStringLiteral("merged"),
                     QCoreApplication::translate("Tools", "Sample merged"), merged_);
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
            context.say(
                        QCoreApplication::translate("Tools",
                                                    "Clone source set to %1, %2")
                            .arg(event.pixel.x())
                            .arg(event.pixel.y()));
            return;
        }
        if (!hasOrigin_) {
            context.say(
                        QCoreApplication::translate(
                            "Tools",
                            "Ctrl+click first to pick the clone source."));
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
    QString name() const override
    {
        return QCoreApplication::translate("Tools", "Recolor");
    }
    QString shortcut() const override { return QStringLiteral("R"); }
    QString hint() const override
    {
        return QCoreApplication::translate(
            "Tools", "Paints the primary colour only over pixels that match "
                     "the secondary colour.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        buildCommonOptions(bar, false);
        bar.addSeparator();
        bar.addPercent(QStringLiteral("tolerance"),
                       QCoreApplication::translate("Tools", "Tolerance"),
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
    QString name() const override
    {
        return QCoreApplication::translate("Tools", "Paint Bucket");
    }
    QString shortcut() const override { return QStringLiteral("F"); }
    QString hint() const override
    {
        return QCoreApplication::translate(
            "Tools", "Fills an area of similar colour. Right click fills with "
                     "the secondary colour.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addCombo(QStringLiteral("mode"),
                     QCoreApplication::translate("Tools", "Fill"),
                     {QCoreApplication::translate("Tools", "Contiguous"),
                      QCoreApplication::translate("Tools", "Global")},
                     contiguous_ ? 0 : 1);
        bar.addPercent(QStringLiteral("tolerance"),
                       QCoreApplication::translate("Tools", "Tolerance"),
                       int(std::lround(tolerance_ * 100.0)), 0, 100);
        bar.addPercent(QStringLiteral("opacity"),
                       QCoreApplication::translate("Tools", "Opacity"),
                       int(std::lround(opacity_ * 100.0)));
        bar.addCheck(QStringLiteral("antialias"),
                     QCoreApplication::translate("Tools", "Antialiasing"),
                     antialias_);
        bar.addCheck(QStringLiteral("merged"),
                     QCoreApplication::translate("Tools", "Sample merged"), merged_);
        bar.addCombo(QStringLiteral("blend"),
                     QCoreApplication::translate("Tools", "Blending"),
                     blendModeLabels(), int(blend_));
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
            context.say(QCoreApplication::translate("Tools", "Nothing to fill here."));
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
        context.say(
                    QCoreApplication::translate("Tools", "Filled %1 pixels")
                        .arg(mask.countAbove(0.5f)));
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
    QString name() const override
    {
        return QCoreApplication::translate("Tools", "Gradient");
    }
    QString shortcut() const override { return QStringLiteral("G"); }
    QString hint() const override
    {
        return QCoreApplication::translate(
            "Tools", "Drag to lay down a gradient. Hold Shift to constrain "
                     "the angle, right drag to swap the colours.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addCombo(QStringLiteral("shape"),
                     QCoreApplication::translate("Tools", "Shape"),
                     {QCoreApplication::translate("Tools", "Linear"),
                      QCoreApplication::translate("Tools", "Linear reflected"),
                      QCoreApplication::translate("Tools", "Radial"),
                      QCoreApplication::translate("Tools", "Conical"),
                      QCoreApplication::translate("Tools", "Diamond")},
                     int(shape_));
        bar.addCombo(QStringLiteral("channel"),
                     QCoreApplication::translate("Tools", "Mode"),
                     {QCoreApplication::translate("Tools", "Colour"),
                      QCoreApplication::translate("Tools", "Transparency")},
                     transparencyMode_ ? 1 : 0);
        bar.addPercent(QStringLiteral("opacity"),
                       QCoreApplication::translate("Tools", "Opacity"),
                       int(std::lround(opacity_ * 100.0)));
        bar.addCheck(QStringLiteral("dither"),
                     QCoreApplication::translate("Tools", "Dither"), dither_);
        bar.addCombo(QStringLiteral("blend"),
                     QCoreApplication::translate("Tools", "Blending"),
                     blendModeLabels(), int(blend_));
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

// ---------------------------------------------------------------- brush tips

namespace brushtips {

namespace {

constexpr char kListKey[] = "tools/brushTips";
constexpr char kChosenKey[] = "tools/brushTip";

struct Library {
    QStringList tips;
    QString chosen;
    bool loaded = false;
};

Library& library()
{
    static Library one;
    if (!one.loaded) {
        one.loaded = true;
        QSettings settings;
        one.tips = settings.value(QLatin1String(kListKey)).toStringList();
        one.chosen = settings.value(QLatin1String(kChosenKey)).toString();
    }
    return one;
}

void save()
{
    QSettings settings;
    settings.setValue(QLatin1String(kListKey), library().tips);
    settings.setValue(QLatin1String(kChosenKey), library().chosen);
}

} // namespace

QStringList remembered() { return library().tips; }

QString chosen() { return library().chosen; }

bool remember(const QString& path)
{
    if (load(path).isNull())
        return false;
    Library& lib = library();
    if (!lib.tips.contains(path))
        lib.tips.append(path);
    lib.chosen = path;
    save();
    return true;
}

void choose(const QString& path)
{
    library().chosen = path;
    save();
}

void forgetAll()
{
    Library& lib = library();
    lib.tips.clear();
    lib.chosen.clear();
    save();
}

void reload()
{
    library().loaded = false;
    library();
}

// A tip is the alpha channel where the file has one that varies, and the
// luminance otherwise, so a cut out shape and a plain grey stamp both work.
// What comes out is trimmed to what it actually marks: a tip drawn in the
// middle of a large empty canvas would otherwise scale down to a dot.
Mask load(const QString& path)
{
    QImage image(path);
    if (image.isNull())
        return Mask();
    const bool hadAlpha = image.hasAlphaChannel();
    image = image.convertToFormat(QImage::Format_ARGB32);
    bool varies = false;
    for (int y = 0; hadAlpha && !varies && y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) != 255) {
                varies = true;
                break;
            }
        }
    }

    Mask mask(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        float* out = mask.row(y);
        for (int x = 0; x < image.width(); ++x)
            out[x] = (varies ? qAlpha(line[x]) : qGray(line[x])) * (1.0f / 255.0f);
    }

    const Rect bounds = mask.boundsAbove(0.004f);
    if (bounds.isEmpty())
        return Mask();
    Mask trimmed(bounds.w, bounds.h);
    for (int y = 0; y < bounds.h; ++y) {
        const float* in = mask.row(bounds.y + y) + bounds.x;
        std::copy(in, in + bounds.w, trimmed.row(y));
    }
    return trimmed;
}

} // namespace brushtips

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
