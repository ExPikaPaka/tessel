// Places an imported or pasted picture on the canvas.
//
// The picture is held whole while it is being positioned, even the parts that
// currently hang off the edge, so it can be dragged back into view. Only what
// overlaps the canvas is written when the placement is committed.
#include "tools/Tool.h"

#include <cmath>

#include <QCoreApplication>

#include <QKeyEvent>

#include "core/ImageOps.h"

namespace tessel {

namespace {

class PlaceTool : public Tool {
public:
    QString id() const override { return QStringLiteral("place"); }
    QString name() const override
    {
        return QCoreApplication::translate("Tools", "Place Image");
    }
    QString iconName() const override { return QStringLiteral("paste"); }
    QString hint() const override
    {
        return QCoreApplication::translate(
            "Tools", "Drag to position the picture, a corner to scale it. "
                     "Enter places it, Escape cancels. Anything outside the "
                     "canvas is trimmed off.");
    }
    QCursor cursor() const override { return QCursor(Qt::SizeAllCursor); }

    void begin(ToolContext& context, Image picture, const QString& layerName,
               bool onNewLayer)
    {
        if (!context.valid() || picture.isNull())
            return;
        if (active_)
            commit(context);

        before_ = context.doc->snapshot();
        Layer* target = nullptr;
        if (onNewLayer) {
            target = context.doc->addLayer(std::make_unique<Layer>(
                Image(context.doc->width(), context.doc->height()),
                context.doc->uniqueLayerName(layerName)));
        } else {
            target = context.doc->activeLayer();
        }
        if (!target)
            return;

        layerUid_ = target->uid();
        base_ = target->image;
        picture_ = std::move(picture);
        scale_ = 1.0;
        // Start centred, which puts an oversized picture symmetrically over the
        // canvas with equal amounts hanging off each side.
        offset_ = QPointF((context.doc->width() - picture_.width()) / 2.0,
                          (context.doc->height() - picture_.height()) / 2.0);
        active_ = true;
        render(context);
        context.say(hint());
    }

    bool isBusy() const override { return active_; }

    void deactivated(ToolContext& context) override { commit(context); }

    void commit(ToolContext& context) override
    {
        if (!active_)
            return;
        active_ = false;
        Layer* layer = context.doc->layerByUid(layerUid_);
        clearPreview(context);
        if (layer) {
            layer->image = compose();
            DocumentState after = context.doc->snapshot();
            context.history->push(std::make_unique<DocumentEdit>(
                QCoreApplication::translate("Tools", "Place Image"),
                QStringLiteral("paste"),
                std::move(before_), std::move(after)));
            context.doc->invalidateLayer(layer);
        }
        release();
        context.repaintAll();
        if (context.documentChanged)
            context.documentChanged();
    }

    void cancel(ToolContext& context) override
    {
        if (!active_)
            return;
        active_ = false;
        clearPreview(context);
        context.doc->restore(before_);
        release();
        context.repaintAll();
        if (context.documentChanged)
            context.documentChanged();
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!active_)
            return;
        grabbed_ = handleAt(event.pos, context.zoom);
        dragStart_ = event.pos;
        offsetAtGrab_ = offset_;
        scaleAtGrab_ = scale_;
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!active_ || !event.buttons.testFlag(Qt::LeftButton))
            return;
        const QPointF delta = event.pos - dragStart_;
        if (grabbed_ < 0) {
            offset_ = offsetAtGrab_ + delta;
        } else {
            // Scale about the corner opposite the one being dragged, so that
            // corner stays put.
            const QRectF start = placedRect(offsetAtGrab_, scaleAtGrab_);
            const QPointF anchor = cornerOf(start, (grabbed_ + 2) % 4);
            const QPointF moved = cornerOf(start, grabbed_) + delta;
            const double width = std::abs(moved.x() - anchor.x());
            const double height = std::abs(moved.y() - anchor.y());
            const double byWidth = width / std::max(1.0, double(picture_.width()));
            const double byHeight = height / std::max(1.0, double(picture_.height()));
            // Uniform by default; Shift lets it stretch to the dragged corner.
            scale_ = std::clamp(event.modifiers.testFlag(Qt::ShiftModifier)
                                    ? std::max(byWidth, byHeight)
                                    : std::min(byWidth, byHeight),
                                0.01, 32.0);
            const QRectF grown = placedRect(QPointF(), scale_);
            offset_ = QPointF(
                grabbed_ == 1 || grabbed_ == 2 ? anchor.x() : anchor.x() - grown.width(),
                grabbed_ == 2 || grabbed_ == 3 ? anchor.y() : anchor.y() - grown.height());
        }
        render(context);
    }

    void release(ToolContext&, const ToolEvent&) override { grabbed_ = -1; }

    void doubleClick(ToolContext& context, const ToolEvent&) override { commit(context); }

    bool keyPress(ToolContext& context, QKeyEvent* event) override
    {
        if (!active_)
            return false;
        const double step = event->modifiers().testFlag(Qt::ControlModifier) ? 10.0 : 1.0;
        switch (event->key()) {
        case Qt::Key_Left: offset_.rx() -= step; break;
        case Qt::Key_Right: offset_.rx() += step; break;
        case Qt::Key_Up: offset_.ry() -= step; break;
        case Qt::Key_Down: offset_.ry() += step; break;
        case Qt::Key_Return:
        case Qt::Key_Enter: commit(context); return true;
        case Qt::Key_Escape: cancel(context); return true;
        default: return false;
        }
        render(context);
        return true;
    }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!active_)
            return;
        const QRectF box = placedRect(offset_, scale_);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(overlayPen(context.zoom, QColor(20, 20, 20, 190), 1.6));
        painter.drawRect(box);
        painter.setPen(overlayPen(context.zoom, QColor(80, 170, 255, 230), 1.0,
                                  Qt::DashLine));
        painter.drawRect(box);

        const double size = 4.5 / std::max(0.05, context.zoom);
        painter.setPen(overlayPen(context.zoom, QColor(20, 20, 20, 220), 1.2));
        painter.setBrush(QColor(255, 255, 255, 240));
        for (int i = 0; i < 4; ++i) {
            const QPointF corner = cornerOf(box, i);
            painter.drawRect(QRectF(corner.x() - size, corner.y() - size, size * 2,
                                    size * 2));
        }
    }

private:
    QRectF placedRect(QPointF at, double scale) const
    {
        return QRectF(at.x(), at.y(), picture_.width() * scale,
                      picture_.height() * scale);
    }

    static QPointF cornerOf(const QRectF& box, int index)
    {
        switch (index) {
        case 0: return box.topLeft();
        case 1: return box.topRight();
        case 2: return box.bottomRight();
        default: return box.bottomLeft();
        }
    }

    int handleAt(QPointF at, double viewZoom) const
    {
        const double reach = 9.0 / std::max(0.05, viewZoom);
        const QRectF box = placedRect(offset_, scale_);
        for (int i = 0; i < 4; ++i) {
            const QPointF corner = cornerOf(box, i);
            if (std::hypot(at.x() - corner.x(), at.y() - corner.y()) <= reach)
                return i;
        }
        return -1;
    }

    // The layer as it will look with the picture laid on top, clipped to the
    // canvas. What hangs off the edge simply never lands.
    Image compose() const
    {
        const int width = std::max(1, int(std::lround(picture_.width() * scale_)));
        const int height = std::max(1, int(std::lround(picture_.height() * scale_)));
        const Image scaled = (width == picture_.width() && height == picture_.height())
            ? picture_
            : ops::resize(picture_, width, height, true);
        const int x = int(std::lround(offset_.x()));
        const int y = int(std::lround(offset_.y()));

        Image staged(base_.width(), base_.height());
        staged.blit(scaled, x, y);
        Image out = base_;
        ops::imageOver(out, staged, nullptr, BlendMode::Normal,
                       Rect(x, y, scaled.width(), scaled.height()));
        return out;
    }

    void render(ToolContext& context)
    {
        preview_ = compose();
        context.doc->setPreview(layerUid_, &preview_);
        previewing_ = true;
        context.repaintAll();
        const QRectF box = placedRect(offset_, scale_);
        context.say(
                    QCoreApplication::translate(
                        "Tools", "Placed at %1, %2   %3 x %4   %5%")
                        .arg(int(std::lround(box.x())))
                        .arg(int(std::lround(box.y())))
                        .arg(int(std::lround(box.width())))
                        .arg(int(std::lround(box.height())))
                        .arg(int(std::lround(scale_ * 100.0))));
    }

    void clearPreview(ToolContext& context)
    {
        if (!previewing_)
            return;
        previewing_ = false;
        context.doc->clearPreview();
    }

    void release()
    {
        picture_ = Image();
        base_ = Image();
        preview_ = Image();
    }

    Image picture_;   // the whole picture, including anything off canvas
    Image base_;      // the target layer before the picture was laid on it
    Image preview_;
    DocumentState before_;
    QPointF offset_;
    QPointF dragStart_;
    QPointF offsetAtGrab_;
    double scale_ = 1.0;
    double scaleAtGrab_ = 1.0;
    int layerUid_ = 0;
    int grabbed_ = -1;
    bool active_ = false;
    bool previewing_ = false;
};

} // namespace

void registerPlaceTool(ToolRegistry& registry)
{
    registry.add(std::make_unique<PlaceTool>());
}

bool startPlacement(ToolContext& context, Image picture, const QString& layerName,
                    bool onNewLayer)
{
    auto* tool = dynamic_cast<PlaceTool*>(
        ToolRegistry::instance().byId(QStringLiteral("place")));
    if (!tool || picture.isNull())
        return false;
    tool->begin(context, std::move(picture), layerName, onNewLayer);
    return tool->isBusy();
}

} // namespace tessel
