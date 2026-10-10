// Rotate / Zoom: an on canvas gizmo for turning, scaling, panning and tiling
// the active layer, with the result previewed live until it is committed.
#include "tools/Tool.h"

#include <cmath>

#include <QKeyEvent>
#include <QPolygonF>

#include "core/ImageOps.h"

namespace tessel {

namespace {

// Which part of the gizmo the pointer grabbed.
enum class Grip { None, Body, Corner, Rotate, Pivot };

class TransformTool : public Tool {
public:
    QString id() const override { return QStringLiteral("transform"); }
    QString name() const override { return QStringLiteral("Rotate / Zoom"); }
    QString iconName() const override { return QStringLiteral("rotate-cw"); }
    QString hint() const override
    {
        return QStringLiteral("Drag inside to move, a corner to scale, the top handle "
                              "to rotate. Enter applies, Escape cancels.");
    }
    QCursor cursor() const override { return QCursor(Qt::OpenHandCursor); }

    // ------------------------------------------------------------ lifecycle

    void activated(ToolContext& context) override
    {
        if (!context.valid())
            return;
        Layer* layer = context.doc->activeLayer();
        if (!layer)
            return;
        layerUid_ = layer->uid();
        source_ = layer->image;
        before_ = context.doc->snapshot();
        reset();
        armed_ = true;
        render(context);
        context.say(hint());
    }

    void deactivated(ToolContext& context) override { commit(context); }

    void commit(ToolContext& context) override
    {
        if (!armed_)
            return;
        armed_ = false;
        clearPreview(context);
        Layer* layer = context.doc->layerByUid(layerUid_);
        if (layer && !isIdentity()) {
            layer->image = renderImage();
            DocumentState after = context.doc->snapshot();
            context.history->push(std::make_unique<DocumentEdit>(
                name(), iconName(), std::move(before_), std::move(after)));
            context.doc->invalidateLayer(layer);
            if (context.documentChanged)
                context.documentChanged();
        }
        source_ = Image();
        context.repaintAll();
    }

    void cancel(ToolContext& context) override
    {
        if (!armed_)
            return;
        armed_ = false;
        clearPreview(context);
        if (Layer* layer = context.doc->layerByUid(layerUid_))
            layer->image = source_;
        context.doc->invalidate();
        source_ = Image();
        context.repaintAll();
        if (context.documentChanged)
            context.documentChanged();
    }

    bool isBusy() const override { return armed_ && !isIdentity(); }

    // ------------------------------------------------------------ input

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!armed_) {
            activated(context);
            if (!armed_)
                return;
        }
        grip_ = gripAt(event.pos, context.zoom);
        dragStart_ = event.pos;
        angleAtGrab_ = angle_;
        zoomAtGrab_ = zoom_;
        offsetAtGrab_ = offset_;
        pivotAtGrab_ = pivot_;
        grabAngle_ = std::atan2(event.pos.y() - pivotPoint().y(),
                                event.pos.x() - pivotPoint().x());
        grabRadius_ = std::hypot(event.pos.x() - pivotPoint().x(),
                                 event.pos.y() - pivotPoint().y());
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!armed_ || grip_ == Grip::None)
            return;
        const QPointF centre = pivotPoint();
        switch (grip_) {
        case Grip::Body:
            offset_ = offsetAtGrab_ + (event.pos - dragStart_);
            break;
        case Grip::Pivot:
            // Moving the pivot must not move the image, so the offset absorbs
            // the change.
            pivot_ = pivotAtGrab_ + (event.pos - dragStart_);
            break;
        case Grip::Rotate: {
            const double now = std::atan2(event.pos.y() - centre.y(),
                                          event.pos.x() - centre.x());
            double next = angleAtGrab_ + (now - grabAngle_) * 180.0 / M_PI;
            if (event.modifiers.testFlag(Qt::ShiftModifier))
                next = std::round(next / 15.0) * 15.0;
            angle_ = std::fmod(next + 360.0, 360.0);
            break;
        }
        case Grip::Corner: {
            const double now = std::hypot(event.pos.x() - centre.x(),
                                          event.pos.y() - centre.y());
            if (grabRadius_ > 1.0e-6)
                zoom_ = std::clamp(zoomAtGrab_ * now / grabRadius_, 0.02, 32.0);
            if (event.modifiers.testFlag(Qt::ShiftModifier))
                zoom_ = std::round(zoom_ * 10.0) / 10.0;
            break;
        }
        default:
            return;
        }
        render(context);
    }

    void release(ToolContext&, const ToolEvent&) override { grip_ = Grip::None; }

    void doubleClick(ToolContext& context, const ToolEvent&) override
    {
        commit(context);
    }

    bool keyPress(ToolContext& context, QKeyEvent* event) override
    {
        if (!armed_)
            return false;
        const double step = event->modifiers().testFlag(Qt::ControlModifier) ? 10.0 : 1.0;
        switch (event->key()) {
        case Qt::Key_Left: offset_.rx() -= step; break;
        case Qt::Key_Right: offset_.rx() += step; break;
        case Qt::Key_Up: offset_.ry() -= step; break;
        case Qt::Key_Down: offset_.ry() += step; break;
        case Qt::Key_BracketLeft: angle_ = std::fmod(angle_ - step + 360.0, 360.0); break;
        case Qt::Key_BracketRight: angle_ = std::fmod(angle_ + step + 360.0, 360.0); break;
        default:
            return false;
        }
        render(context);
        return true;
    }

    // ------------------------------------------------------------ options

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addDouble(QStringLiteral("angle"), QStringLiteral("Angle"), -360.0, 360.0,
                      angle_, 1.0, 2, QStringLiteral(" deg"));
        bar.addDouble(QStringLiteral("zoom"), QStringLiteral("Zoom"), 0.02, 32.0, zoom_,
                      0.05, 3);
        bar.addCheck(QStringLiteral("tile"), QStringLiteral("Tile"), tile_);
        bar.addCombo(QStringLiteral("sampling"), QStringLiteral("Sampling"),
                     {QStringLiteral("Smooth"), QStringLiteral("Nearest")},
                     smooth_ ? 0 : 1);
        bar.addToggle(QStringLiteral("reset"), QStringLiteral("undo"),
                      QStringLiteral("Reset the transform"), false);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("angle"))
            angle_ = value.toDouble();
        else if (key == QLatin1String("zoom"))
            zoom_ = std::clamp(value.toDouble(), 0.02, 32.0);
        else if (key == QLatin1String("tile"))
            tile_ = value.toBool();
        else if (key == QLatin1String("sampling"))
            smooth_ = value.toInt() == 0;
        else if (key == QLatin1String("reset") && value.toBool())
            reset();
        else
            return;
        if (refresh_)
            refresh_();
    }

    // ------------------------------------------------------------ overlay

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!armed_ || source_.isNull())
            return;
        const QPolygonF quad = outlineQuad();
        const double scale = 1.0 / std::max(0.05, context.zoom);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(overlayPen(context.zoom, QColor(20, 20, 20, 190), 1.6));
        painter.drawPolygon(quad);
        painter.setPen(overlayPen(context.zoom, QColor(80, 170, 255, 230), 1.0,
                                  Qt::DashLine));
        painter.drawPolygon(quad);

        // Rotation arm, rising from the midpoint of the top edge.
        const QPointF top = (quad[0] + quad[1]) / 2.0;
        const QPointF handle = rotateHandle();
        painter.setPen(overlayPen(context.zoom, QColor(80, 170, 255, 230), 1.2));
        painter.drawLine(top, handle);

        const double size = 4.5 * scale;
        painter.setPen(overlayPen(context.zoom, QColor(20, 20, 20, 220), 1.2));
        painter.setBrush(QColor(255, 255, 255, 240));
        for (const QPointF& corner : quad)
            painter.drawRect(QRectF(corner.x() - size, corner.y() - size, size * 2,
                                    size * 2));
        painter.setBrush(QColor(80, 170, 255, 240));
        painter.drawEllipse(handle, size * 1.1, size * 1.1);

        // Pivot: a ring with a cross through it.
        const QPointF centre = pivotPoint();
        painter.setBrush(Qt::NoBrush);
        painter.setPen(overlayPen(context.zoom, QColor(255, 220, 90, 240), 1.6));
        painter.drawEllipse(centre, size * 1.3, size * 1.3);
        painter.drawLine(QPointF(centre.x() - size * 2, centre.y()),
                         QPointF(centre.x() + size * 2, centre.y()));
        painter.drawLine(QPointF(centre.x(), centre.y() - size * 2),
                         QPointF(centre.x(), centre.y() + size * 2));
    }

    void setRefresh(std::function<void()> fn) { refresh_ = std::move(fn); }

private:
    // ------------------------------------------------------------ geometry

    QPointF pivotPoint() const { return pivot_ + offset_; }

    QPointF mapPoint(QPointF source) const
    {
        const double radians = angle_ * M_PI / 180.0;
        const double cosA = std::cos(radians), sinA = std::sin(radians);
        const double dx = (source.x() - pivot_.x()) * zoom_;
        const double dy = (source.y() - pivot_.y()) * zoom_;
        return QPointF(pivot_.x() + offset_.x() + dx * cosA - dy * sinA,
                       pivot_.y() + offset_.y() + dx * sinA + dy * cosA);
    }

    QPolygonF outlineQuad() const
    {
        const double w = source_.width(), h = source_.height();
        QPolygonF quad;
        quad << mapPoint(QPointF(0, 0)) << mapPoint(QPointF(w, 0))
             << mapPoint(QPointF(w, h)) << mapPoint(QPointF(0, h));
        return quad;
    }

    QPointF rotateHandle() const
    {
        const QPolygonF quad = outlineQuad();
        const QPointF top = (quad[0] + quad[1]) / 2.0;
        const QPointF centre = pivotPoint();
        QPointF direction = top - centre;
        const double length = std::hypot(direction.x(), direction.y());
        if (length < 1.0e-6)
            return top - QPointF(0, 40);
        direction /= length;
        return top + direction * 34.0;
    }

    Grip gripAt(QPointF at, double viewZoom) const
    {
        const double reach = 9.0 / std::max(0.05, viewZoom);
        auto near = [&](QPointF point) {
            return std::hypot(at.x() - point.x(), at.y() - point.y()) <= reach;
        };
        if (near(rotateHandle()))
            return Grip::Rotate;
        if (near(pivotPoint()))
            return Grip::Pivot;
        for (const QPointF& corner : outlineQuad()) {
            if (near(corner))
                return Grip::Corner;
        }
        return outlineQuad().containsPoint(at, Qt::OddEvenFill) ? Grip::Body : Grip::Body;
    }

    bool isIdentity() const
    {
        return std::abs(angle_) < 1.0e-6 && std::abs(zoom_ - 1.0) < 1.0e-6
            && offset_.manhattanLength() < 1.0e-6 && !tile_;
    }

    void reset()
    {
        angle_ = 0.0;
        zoom_ = 1.0;
        offset_ = QPointF();
        pivot_ = QPointF(source_.width() / 2.0, source_.height() / 2.0);
    }

    // ------------------------------------------------------------ rendering

    Image renderImage() const
    {
        const double radians = -angle_ * M_PI / 180.0;
        const double cosA = std::cos(radians), sinA = std::sin(radians);
        const double inv = 1.0 / std::max(0.02, zoom_);
        const QPointF centre = pivot_;
        const QPointF shift = offset_;
        const ops::EdgeMode edge = tile_ ? ops::EdgeMode::Wrap
                                         : ops::EdgeMode::Transparent;
        // Destination to source, since the resampler pulls rather than pushes.
        return ops::remap(source_, source_.width(), source_.height(),
                          [&](int x, int y, float& sx, float& sy) {
                              const double dx = x - centre.x() - shift.x();
                              const double dy = y - centre.y() - shift.y();
                              const double rx = (dx * cosA - dy * sinA) * inv;
                              const double ry = (dx * sinA + dy * cosA) * inv;
                              sx = float(centre.x() + rx);
                              sy = float(centre.y() + ry);
                          },
                          edge);
    }

    void render(ToolContext& context)
    {
        if (source_.isNull())
            return;
        preview_ = renderImage();
        context.doc->setPreview(layerUid_, &preview_);
        previewing_ = true;
        context.repaintAll();
        context.say(QStringLiteral("Angle %1 deg   Zoom %2   Offset %3, %4")
                        .arg(angle_, 0, 'f', 1)
                        .arg(zoom_, 0, 'f', 3)
                        .arg(int(std::lround(offset_.x())))
                        .arg(int(std::lround(offset_.y()))));
    }

    void clearPreview(ToolContext& context)
    {
        if (!previewing_)
            return;
        previewing_ = false;
        context.doc->clearPreview();
    }

    double angle_ = 0.0;
    double zoom_ = 1.0;
    QPointF offset_;
    QPointF pivot_;
    bool tile_ = false;
    bool smooth_ = true;

    Image source_;
    Image preview_;
    DocumentState before_;
    int layerUid_ = 0;
    bool armed_ = false;
    bool previewing_ = false;

    Grip grip_ = Grip::None;
    QPointF dragStart_;
    QPointF offsetAtGrab_;
    QPointF pivotAtGrab_;
    double angleAtGrab_ = 0.0;
    double zoomAtGrab_ = 1.0;
    double grabAngle_ = 0.0;
    double grabRadius_ = 0.0;
    std::function<void()> refresh_;
};

} // namespace

void registerTransformTools(ToolRegistry& registry)
{
    registry.add(std::make_unique<TransformTool>());
}

} // namespace tessel
