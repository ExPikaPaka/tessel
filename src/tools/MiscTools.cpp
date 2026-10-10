// Colour picker, zoom and pan.
#include "tools/Tool.h"

#include <cmath>

#include "core/ImageOps.h"

namespace tessel {

namespace {

class ColorPickerTool : public Tool {
public:
    QString id() const override { return QStringLiteral("picker"); }
    QString name() const override { return QStringLiteral("Color Picker"); }
    QString iconName() const override { return QStringLiteral("picker"); }
    QString shortcut() const override { return QStringLiteral("K"); }
    QString hint() const override
    {
        return QStringLiteral("Click to pick the primary colour, right click for the "
                              "secondary colour.");
    }
    QCursor cursor() const override { return QCursor(Qt::CrossCursor); }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addCombo(QStringLiteral("sampling"), QStringLiteral("Sampling"),
                     {QStringLiteral("Layer"), QStringLiteral("Image")},
                     fromImage_ ? 1 : 0);
        bar.addCombo(QStringLiteral("radius"), QStringLiteral("Sample size"),
                     {QStringLiteral("1 x 1"), QStringLiteral("3 x 3"),
                      QStringLiteral("5 x 5"), QStringLiteral("9 x 9")},
                     radiusIndex_);
        bar.addCombo(QStringLiteral("after"), QStringLiteral("After click"),
                     {QStringLiteral("Do not switch"),
                      QStringLiteral("Switch to previous tool"),
                      QStringLiteral("Switch to Pencil"),
                      QStringLiteral("Switch to Paintbrush")},
                     afterIndex_);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("sampling"))
            fromImage_ = value.toInt() == 1;
        else if (key == QLatin1String("radius"))
            radiusIndex_ = std::clamp(value.toInt(), 0, 3);
        else if (key == QLatin1String("after"))
            afterIndex_ = std::clamp(value.toInt(), 0, 3);
    }

    void press(ToolContext& context, const ToolEvent& event) override { sample(context, event); }
    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (event.buttons != Qt::NoButton)
            sample(context, event);
    }

    void release(ToolContext& context, const ToolEvent&) override
    {
        if (!context.requestTool)
            return;
        switch (afterIndex_) {
        case 1: context.requestTool(QString()); break;            // previous tool
        case 2: context.requestTool(QStringLiteral("pencil")); break;
        case 3: context.requestTool(QStringLiteral("brush")); break;
        default: break;
        }
    }

private:
    void sample(ToolContext& context, const ToolEvent& event)
    {
        if (!context.valid())
            return;
        const Layer* layer = context.doc->activeLayer();
        if (!layer)
            return;
        const Image& source = fromImage_ ? context.doc->composite() : layer->image;
        if (!source.rect().contains(event.pixel.x(), event.pixel.y()))
            return;
        static const int radii[4] = {0, 1, 2, 4};
        const int radius = radii[radiusIndex_];
        double acc[4] = {0, 0, 0, 0};
        int count = 0;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int x = event.pixel.x() + dx;
                const int y = event.pixel.y() + dy;
                if (!source.rect().contains(x, y))
                    continue;
                const Rgba c = source.row(y)[x];
                const double alpha = c.a * (1.0 / 255.0);
                acc[0] += c.r * alpha;
                acc[1] += c.g * alpha;
                acc[2] += c.b * alpha;
                acc[3] += alpha;
                ++count;
            }
        }
        if (count == 0)
            return;
        Rgba picked(0, 0, 0, 0);
        if (acc[3] > 1.0e-9) {
            picked = Rgba(static_cast<uint8_t>(std::lround(acc[0] / acc[3])),
                          static_cast<uint8_t>(std::lround(acc[1] / acc[3])),
                          static_cast<uint8_t>(std::lround(acc[2] / acc[3])),
                          toByte(float(acc[3] / count)));
        }
        if (context.pickedColor)
            context.pickedColor(picked, !event.isSecondary());
        context.say(QStringLiteral("R %1  G %2  B %3  A %4")
                        .arg(picked.r).arg(picked.g).arg(picked.b).arg(picked.a));
    }

    bool fromImage_ = false;
    int radiusIndex_ = 0;
    int afterIndex_ = 0;
};

class ZoomTool : public Tool {
public:
    QString id() const override { return QStringLiteral("zoom"); }
    QString name() const override { return QStringLiteral("Zoom"); }
    QString iconName() const override { return QStringLiteral("zoom"); }
    QString shortcut() const override { return QStringLiteral("Z"); }
    QString hint() const override
    {
        return QStringLiteral("Click to zoom in, right click to zoom out, or drag a "
                              "rectangle to zoom into it.");
    }
    QCursor cursor() const override { return QCursor(Qt::CrossCursor); }

    void press(ToolContext&, const ToolEvent& event) override
    {
        start_ = event.pos;
        current_ = event.pos;
        dragging_ = true;
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        current_ = event.pos;
        if (std::hypot(current_.x() - start_.x(), current_.y() - start_.y()) > 4.0)
            marquee_ = true;
        context.repaintAll();
    }

    void release(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        dragging_ = false;
        if (marquee_ && context.zoomTo) {
            marquee_ = false;
            const QRectF box = QRectF(start_, event.pos).normalized();
            if (box.width() > 2.0 && box.height() > 2.0) {
                // Zoom so the dragged rectangle fills the view.
                const double targetZoom = std::clamp(
                    context.zoom * std::min(1000.0 / box.width(), 700.0 / box.height()),
                    0.01, 32.0);
                context.zoomTo(targetZoom, box.center());
                context.repaintAll();
                return;
            }
        }
        marquee_ = false;
        if (context.zoomTo) {
            const double step = event.isSecondary() ? 1.0 / 1.5 : 1.5;
            context.zoomTo(std::clamp(context.zoom * step, 0.01, 32.0), event.pos);
        }
        context.repaintAll();
    }

    void cancel(ToolContext& context) override
    {
        dragging_ = false;
        marquee_ = false;
        context.repaintAll();
    }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!dragging_ || !marquee_)
            return;
        painter.setBrush(QColor(80, 150, 255, 40));
        painter.setPen(overlayPen(context.zoom, QColor(255, 255, 255, 230), 1.0,
                                  Qt::DashLine));
        painter.drawRect(QRectF(start_, current_).normalized());
    }

private:
    QPointF start_;
    QPointF current_;
    bool dragging_ = false;
    bool marquee_ = false;
};

class PanTool : public Tool {
public:
    QString id() const override { return QStringLiteral("pan"); }
    QString name() const override { return QStringLiteral("Pan"); }
    QString iconName() const override { return QStringLiteral("pan"); }
    QString shortcut() const override { return QStringLiteral("H"); }
    QString hint() const override { return QStringLiteral("Drag to scroll the canvas."); }
    QCursor cursor() const override { return QCursor(Qt::OpenHandCursor); }

    void press(ToolContext&, const ToolEvent& event) override
    {
        last_ = event.viewPos;
        dragging_ = true;
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_ || !context.scrollBy)
            return;
        context.scrollBy(last_ - event.viewPos);
        last_ = event.viewPos;
    }

    void release(ToolContext&, const ToolEvent&) override { dragging_ = false; }
    void cancel(ToolContext&) override { dragging_ = false; }

private:
    QPointF last_;
    bool dragging_ = false;
};

} // namespace

void registerMiscTools(ToolRegistry& registry)
{
    registry.add(std::make_unique<ColorPickerTool>());
    registry.add(std::make_unique<ZoomTool>());
    registry.add(std::make_unique<PanTool>());
}

} // namespace tessel
