// Line / curve, the shape family, and the text tool. All three stay editable
// after the initial drag and are only written into the layer on commit.
#include "tools/Tool.h"

#include <cmath>

#include <QFont>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPolygonF>

#include "core/ImageOps.h"

namespace tessel {

namespace {

// Paints through a QPainter into a full canvas RGBA image.
template <class F>
Image renderToImage(int width, int height, bool antialias, F&& paint)
{
    QImage canvas(std::max(1, width), std::max(1, height),
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, antialias);
        painter.setRenderHint(QPainter::TextAntialiasing, antialias);
        paint(painter);
    }
    return Image::fromQImage(canvas.convertToFormat(QImage::Format_RGBA8888));
}

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

enum FillStyle { OutlineOnly, FillOnly, OutlineAndFill };

// ---------------------------------------------------------------- base

// Shared lifecycle: snapshot the layer, re-render a preview whenever the shape
// changes, and push one history entry when the shape is committed.
class InteractiveShapeTool : public Tool {
public:
    void deactivated(ToolContext& context) override { commit(context); }

    void commit(ToolContext& context) override
    {
        if (!editing_ || !layer_)
            return;
        editing_ = false;
        dragging_ = false;
        renderPreview(context);
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
        clearState(context);
    }

    void cancel(ToolContext& context) override
    {
        if (!editing_ || !layer_)
            return;
        editing_ = false;
        dragging_ = false;
        layer_->image = backup_;
        context.doc->invalidateLayer(layer_);
        context.repaintAll();
        clearState(context);
    }

    bool isBusy() const override { return editing_; }

    bool keyPress(ToolContext& context, QKeyEvent* event) override
    {
        if (!editing_)
            return false;
        if (event->key() == Qt::Key_Escape) {
            cancel(context);
            return true;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            commit(context);
            return true;
        }
        return false;
    }

protected:
    virtual void drawShape(QPainter& painter, const ToolContext& context) const = 0;
    virtual Rect shapeBounds() const = 0;

    bool beginEdit(ToolContext& context)
    {
        if (!context.valid())
            return false;
        Layer* layer = context.doc->activeLayer();
        if (!layer)
            return false;
        if (editing_ && layer_ == layer)
            return true;
        if (editing_)
            commit(context);
        layer_ = layer;
        layerUid_ = layer->uid();
        backup_ = layer->image;
        dirty_ = Rect();
        editing_ = true;
        return true;
    }

    void renderPreview(ToolContext& context)
    {
        if (!layer_)
            return;
        const Rect area = shapeBounds().intersected(layer_->image.rect());
        const Rect restore = dirty_.united(area).intersected(layer_->image.rect());
        layer_->image.copyRectFrom(backup_, restore);
        if (!area.isEmpty()) {
            Image painted = renderToImage(layer_->image.width(), layer_->image.height(),
                                          antialias_,
                                          [&](QPainter& painter) {
                                              drawShape(painter, context);
                                          });
            Mask mask(layer_->image.width(), layer_->image.height(), 1.0f);
            clipToSelection(mask, *context.doc, area);
            ops::imageOver(layer_->image, painted, &mask, blend_, area, float(opacity_));
            dirty_ = dirty_.united(area);
        }
        context.doc->invalidateLayer(layer_);
        context.repaint(restore);
    }

    void buildShapeOptions(ToolOptionsBar& bar, bool withFillStyle)
    {
        if (withFillStyle) {
            bar.addCombo(QStringLiteral("fillstyle"), QStringLiteral("Style"),
                         {QStringLiteral("Outline"), QStringLiteral("Filled"),
                          QStringLiteral("Outline and fill")},
                         int(fillStyle_));
        }
        bar.addSpin(QStringLiteral("width"), QStringLiteral("Width"), 1, 500,
                    int(std::lround(penWidth_)), QStringLiteral(" px"));
        bar.addPercent(QStringLiteral("opacity"), QStringLiteral("Opacity"),
                       int(std::lround(opacity_ * 100.0)));
        bar.addCheck(QStringLiteral("antialias"), QStringLiteral("Antialiasing"),
                     antialias_);
        bar.addCombo(QStringLiteral("blend"), QStringLiteral("Blending"),
                     blendModeNames(), int(blend_));
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("fillstyle"))
            fillStyle_ = static_cast<FillStyle>(std::clamp(value.toInt(), 0, 2));
        else if (key == QLatin1String("width"))
            penWidth_ = std::max(1, value.toInt());
        else if (key == QLatin1String("opacity"))
            opacity_ = std::clamp(value.toInt(), 1, 100) / 100.0;
        else if (key == QLatin1String("antialias"))
            antialias_ = value.toBool();
        else if (key == QLatin1String("blend"))
            blend_ = static_cast<BlendMode>(std::clamp(value.toInt(), 0,
                                                      int(BlendMode::Count) - 1));
        pendingRefresh_ = true;
    }

    QPen shapePen(const ToolContext& context) const
    {
        QPen pen((secondary_ ? context.secondary : context.primary).toQColor());
        pen.setWidthF(penWidth_);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        return pen;
    }

    QBrush shapeBrush(const ToolContext& context) const
    {
        return QBrush((secondary_ ? context.primary : context.secondary).toQColor());
    }

    Rect boundsOf(const QRectF& box) const
    {
        const double pad = penWidth_ + 2.0;
        return Rect(int(std::floor(box.left() - pad)), int(std::floor(box.top() - pad)),
                    int(std::ceil(box.width() + 2 * pad)) + 1,
                    int(std::ceil(box.height() + 2 * pad)) + 1);
    }

    void clearState(ToolContext&)
    {
        layer_ = nullptr;
        backup_ = Image();
        dirty_ = Rect();
    }

    FillStyle fillStyle_ = OutlineOnly;
    double penWidth_ = 2.0;
    double opacity_ = 1.0;
    bool antialias_ = true;
    BlendMode blend_ = BlendMode::Normal;
    bool secondary_ = false;
    bool editing_ = false;
    bool dragging_ = false;
    bool pendingRefresh_ = false;
    Layer* layer_ = nullptr;
    int layerUid_ = 0;
    Image backup_;
    Rect dirty_;
};

// ---------------------------------------------------------------- line / curve

class LineCurveTool : public InteractiveShapeTool {
public:
    QString id() const override { return QStringLiteral("line"); }
    QString name() const override { return QStringLiteral("Line / Curve"); }
    QString iconName() const override { return QStringLiteral("line"); }
    QString shortcut() const override { return QStringLiteral("O"); }
    QString hint() const override
    {
        return QStringLiteral("Drag for a line, then drag the middle handles to bend "
                              "it. Enter finishes, Escape cancels.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        buildShapeOptions(bar, false);
        bar.addSeparator();
        bar.addCombo(QStringLiteral("startcap"), QStringLiteral("Start"),
                     {QStringLiteral("Flat"), QStringLiteral("Arrow"),
                      QStringLiteral("Round")},
                     startCap_);
        bar.addCombo(QStringLiteral("endcap"), QStringLiteral("End"),
                     {QStringLiteral("Flat"), QStringLiteral("Arrow"),
                      QStringLiteral("Round")},
                     endCap_);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("startcap"))
            startCap_ = std::clamp(value.toInt(), 0, 2);
        else if (key == QLatin1String("endcap"))
            endCap_ = std::clamp(value.toInt(), 0, 2);
        else
            InteractiveShapeTool::optionChanged(key, value);
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!beginEdit(context))
            return;
        secondary_ = event.isSecondary();
        grabbed_ = nearestHandle(event.pos, context.zoom);
        if (grabbed_ < 0) {
            // Start a fresh line, committing whatever was being edited.
            if (hasShape_) {
                commit(context);
                if (!beginEdit(context))
                    return;
            }
            points_[0] = points_[1] = points_[2] = points_[3] = event.pos;
            hasShape_ = true;
            grabbed_ = 3;
        }
        dragging_ = true;
        renderPreview(context);
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_ || grabbed_ < 0)
            return;
        QPointF target = event.pos;
        if (event.modifiers.testFlag(Qt::ShiftModifier) && grabbed_ == 3)
            target = constrainAngle(points_[0], target);
        points_[grabbed_] = target;
        if (grabbed_ == 0 || grabbed_ == 3) {
            // While the ends move, keep the control points proportional so the
            // line stays straight until deliberately bent.
            if (!bent_) {
                points_[1] = points_[0] + (points_[3] - points_[0]) / 3.0;
                points_[2] = points_[0] + (points_[3] - points_[0]) * 2.0 / 3.0;
            }
        } else {
            bent_ = true;
        }
        renderPreview(context);
    }

    void release(ToolContext&, const ToolEvent&) override
    {
        dragging_ = false;
        grabbed_ = -1;
    }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!editing_ || !hasShape_)
            return;
        const double size = 3.5 / std::max(0.05, context.zoom);
        painter.setPen(overlayPen(context.zoom, QColor(60, 160, 255, 200), 1.0,
                                  Qt::DashLine));
        painter.drawLine(points_[0], points_[1]);
        painter.drawLine(points_[2], points_[3]);
        painter.setPen(overlayPen(context.zoom, QColor(20, 20, 20, 220), 1.0));
        for (int i = 0; i < 4; ++i) {
            painter.setBrush(i == 0 || i == 3 ? QColor(255, 255, 255, 235)
                                              : QColor(60, 160, 255, 235));
            painter.drawEllipse(points_[i], size, size);
        }
    }

protected:
    void drawShape(QPainter& painter, const ToolContext& context) const override
    {
        if (!hasShape_)
            return;
        QPainterPath path(points_[0]);
        path.cubicTo(points_[1], points_[2], points_[3]);
        QPen pen = shapePen(context);
        pen.setCapStyle(endCap_ == 2 ? Qt::RoundCap : Qt::FlatCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        if (startCap_ == 1)
            drawArrow(painter, context, points_[0], points_[1]);
        if (endCap_ == 1)
            drawArrow(painter, context, points_[3], points_[2]);
    }

    Rect shapeBounds() const override
    {
        if (!hasShape_)
            return Rect();
        QPainterPath path(points_[0]);
        path.cubicTo(points_[1], points_[2], points_[3]);
        return boundsOf(path.boundingRect().adjusted(-penWidth_ * 3, -penWidth_ * 3,
                                                     penWidth_ * 3, penWidth_ * 3));
    }

private:
    void drawArrow(QPainter& painter, const ToolContext& context, QPointF tip,
                   QPointF from) const
    {
        const QPointF direction = tip - from;
        const double length = std::hypot(direction.x(), direction.y());
        if (length < 1.0e-6)
            return;
        const QPointF unit = direction / length;
        const QPointF normal(-unit.y(), unit.x());
        const double size = std::max(6.0, penWidth_ * 4.0);
        QPolygonF head;
        head << tip << tip - unit * size + normal * size * 0.45
             << tip - unit * size - normal * size * 0.45;
        painter.setPen(Qt::NoPen);
        painter.setBrush(shapePen(context).color());
        painter.drawPolygon(head);
    }

    static QPointF constrainAngle(QPointF from, QPointF to)
    {
        const double dx = to.x() - from.x(), dy = to.y() - from.y();
        const double length = std::hypot(dx, dy);
        if (length < 1.0e-6)
            return to;
        const double angle = std::round(std::atan2(dy, dx) / (M_PI / 12.0)) * (M_PI / 12.0);
        return QPointF(from.x() + std::cos(angle) * length,
                       from.y() + std::sin(angle) * length);
    }

    int nearestHandle(QPointF point, double zoom) const
    {
        if (!hasShape_)
            return -1;
        const double reach = 7.0 / std::max(0.05, zoom);
        int best = -1;
        double bestDistance = reach;
        for (int i = 0; i < 4; ++i) {
            const double distance = std::hypot(point.x() - points_[i].x(),
                                               point.y() - points_[i].y());
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        return best;
    }

    QPointF points_[4];
    bool hasShape_ = false;
    bool bent_ = false;
    int grabbed_ = -1;
    int startCap_ = 0;
    int endCap_ = 0;
};

// ---------------------------------------------------------------- shapes

class ShapesTool : public InteractiveShapeTool {
public:
    enum Kind {
        Rectangle, RoundedRectangle, Ellipse, Triangle, RightTriangle, Trapezoid,
        Parallelogram, Diamond, Pentagon, Hexagon, Heptagon, Octagon,
        Star3, Star4, Star5, Star6, Star8,
        Arrow, DoubleArrow, Chevron, Cross, Plus,
        Heart, Crescent, Cloud, SpeechBubble, Ring, PieWedge, Lightning, Teardrop,
        KindCount
    };

    static QStringList shapeNames()
    {
        return {QStringLiteral("Rectangle"), QStringLiteral("Rounded rectangle"),
                QStringLiteral("Ellipse"), QStringLiteral("Triangle"),
                QStringLiteral("Right triangle"), QStringLiteral("Trapezoid"),
                QStringLiteral("Parallelogram"), QStringLiteral("Diamond"),
                QStringLiteral("Pentagon"), QStringLiteral("Hexagon"),
                QStringLiteral("Heptagon"), QStringLiteral("Octagon"),
                QStringLiteral("Star (3 points)"), QStringLiteral("Star (4 points)"),
                QStringLiteral("Star (5 points)"), QStringLiteral("Star (6 points)"),
                QStringLiteral("Star (8 points)"), QStringLiteral("Arrow"),
                QStringLiteral("Double arrow"), QStringLiteral("Chevron"),
                QStringLiteral("Cross"), QStringLiteral("Plus"),
                QStringLiteral("Heart"), QStringLiteral("Crescent"),
                QStringLiteral("Cloud"), QStringLiteral("Speech bubble"),
                QStringLiteral("Ring"), QStringLiteral("Pie wedge"),
                QStringLiteral("Lightning"), QStringLiteral("Teardrop")};
    }

    QString id() const override { return QStringLiteral("shapes"); }
    QString name() const override { return QStringLiteral("Shapes"); }
    QString iconName() const override { return QStringLiteral("shapes"); }
    QString shortcut() const override { return QStringLiteral("O"); }
    QString hint() const override
    {
        return QStringLiteral("Drag to draw. Shift keeps the proportions, Enter "
                              "finishes, Escape cancels.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addCombo(QStringLiteral("kind"), QStringLiteral("Shape"), shapeNames(),
                     int(kind_));
        buildShapeOptions(bar, true);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("kind"))
            kind_ = static_cast<Kind>(std::clamp(value.toInt(), 0, int(KindCount) - 1));
        else
            InteractiveShapeTool::optionChanged(key, value);
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!beginEdit(context))
            return;
        if (hasShape_) {
            const int handle = handleAt(event.pos, context.zoom);
            if (handle >= 0) {
                grabbed_ = handle;
                dragging_ = true;
                startRect_ = box_;
                dragStart_ = event.pos;
                return;
            }
            commit(context);
            if (!beginEdit(context))
                return;
        }
        secondary_ = event.isSecondary();
        anchor_ = event.pos;
        box_ = QRectF(event.pos, event.pos);
        hasShape_ = true;
        grabbed_ = -1;
        dragging_ = true;
        renderPreview(context);
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        if (grabbed_ >= 0) {
            const QPointF delta = event.pos - dragStart_;
            QRectF rect = startRect_;
            switch (grabbed_) {
            case 0: rect.setTopLeft(startRect_.topLeft() + delta); break;
            case 1: rect.setTopRight(startRect_.topRight() + delta); break;
            case 2: rect.setBottomRight(startRect_.bottomRight() + delta); break;
            default: rect.setBottomLeft(startRect_.bottomLeft() + delta); break;
            }
            box_ = rect.normalized();
        } else {
            double w = event.pos.x() - anchor_.x();
            double h = event.pos.y() - anchor_.y();
            if (event.modifiers.testFlag(Qt::ShiftModifier)) {
                const double side = std::max(std::abs(w), std::abs(h));
                w = w < 0 ? -side : side;
                h = h < 0 ? -side : side;
            }
            box_ = QRectF(anchor_, QPointF(anchor_.x() + w, anchor_.y() + h)).normalized();
        }
        renderPreview(context);
        context.say(QStringLiteral("%1 x %2")
                        .arg(int(std::lround(box_.width())))
                        .arg(int(std::lround(box_.height()))));
    }

    void release(ToolContext&, const ToolEvent&) override
    {
        dragging_ = false;
        grabbed_ = -1;
    }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!editing_ || !hasShape_)
            return;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(overlayPen(context.zoom, QColor(60, 160, 255, 180), 1.0,
                                  Qt::DashLine));
        painter.drawRect(box_);
        const double size = 3.5 / std::max(0.05, context.zoom);
        painter.setPen(overlayPen(context.zoom, QColor(20, 20, 20, 220), 1.0));
        painter.setBrush(QColor(255, 255, 255, 235));
        for (const QPointF& corner : {box_.topLeft(), box_.topRight(),
                                      box_.bottomRight(), box_.bottomLeft()}) {
            painter.drawRect(QRectF(corner.x() - size, corner.y() - size, size * 2,
                                    size * 2));
        }
    }

protected:
    void drawShape(QPainter& painter, const ToolContext& context) const override
    {
        if (!hasShape_ || box_.width() < 0.5 || box_.height() < 0.5)
            return;
        const QPainterPath path = buildPath();
        painter.setPen(fillStyle_ == FillOnly ? QPen(Qt::NoPen) : shapePen(context));
        painter.setBrush(fillStyle_ == OutlineOnly ? QBrush(Qt::NoBrush)
                                                   : shapeBrush(context));
        painter.drawPath(path);
    }

    Rect shapeBounds() const override
    {
        if (!hasShape_)
            return Rect();
        return boundsOf(box_);
    }

private:
    int handleAt(QPointF point, double zoom) const
    {
        const double reach = 7.0 / std::max(0.05, zoom);
        const QPointF list[4] = {box_.topLeft(), box_.topRight(), box_.bottomRight(),
                                 box_.bottomLeft()};
        for (int i = 0; i < 4; ++i) {
            if (std::hypot(point.x() - list[i].x(), point.y() - list[i].y()) <= reach)
                return i;
        }
        return -1;
    }

    static QPolygonF regularPolygon(const QRectF& box, int sides, double rotation)
    {
        QPolygonF polygon;
        const QPointF centre = box.center();
        const double rx = box.width() / 2.0, ry = box.height() / 2.0;
        for (int i = 0; i < sides; ++i) {
            const double angle = rotation + 2.0 * M_PI * i / sides;
            polygon << QPointF(centre.x() + std::cos(angle) * rx,
                               centre.y() + std::sin(angle) * ry);
        }
        return polygon;
    }

    static QPolygonF starPolygon(const QRectF& box, int points, double innerRatio)
    {
        QPolygonF polygon;
        const QPointF centre = box.center();
        const double rx = box.width() / 2.0, ry = box.height() / 2.0;
        for (int i = 0; i < points * 2; ++i) {
            const double angle = -M_PI / 2.0 + M_PI * i / points;
            const double scale = (i % 2 == 0) ? 1.0 : innerRatio;
            polygon << QPointF(centre.x() + std::cos(angle) * rx * scale,
                               centre.y() + std::sin(angle) * ry * scale);
        }
        return polygon;
    }

    QPainterPath buildPath() const
    {
        QPainterPath path;
        const QRectF box = box_;
        const double w = box.width(), h = box.height();
        const QPointF centre = box.center();
        switch (kind_) {
        case Rectangle:
            path.addRect(box);
            break;
        case RoundedRectangle: {
            const double radius = std::min(w, h) * 0.18;
            path.addRoundedRect(box, radius, radius);
            break;
        }
        case Ellipse:
            path.addEllipse(box);
            break;
        case Triangle:
            path.addPolygon(QPolygonF({QPointF(centre.x(), box.top()), box.bottomRight(),
                                       box.bottomLeft()}));
            path.closeSubpath();
            break;
        case RightTriangle:
            path.addPolygon(QPolygonF({box.topLeft(), box.bottomRight(),
                                       box.bottomLeft()}));
            path.closeSubpath();
            break;
        case Trapezoid:
            path.addPolygon(QPolygonF({QPointF(box.left() + w * 0.25, box.top()),
                                       QPointF(box.right() - w * 0.25, box.top()),
                                       box.bottomRight(), box.bottomLeft()}));
            path.closeSubpath();
            break;
        case Parallelogram:
            path.addPolygon(QPolygonF({QPointF(box.left() + w * 0.25, box.top()),
                                       box.topRight(),
                                       QPointF(box.right() - w * 0.25, box.bottom()),
                                       box.bottomLeft()}));
            path.closeSubpath();
            break;
        case Diamond:
            path.addPolygon(QPolygonF({QPointF(centre.x(), box.top()),
                                       QPointF(box.right(), centre.y()),
                                       QPointF(centre.x(), box.bottom()),
                                       QPointF(box.left(), centre.y())}));
            path.closeSubpath();
            break;
        case Pentagon:
            path.addPolygon(regularPolygon(box, 5, -M_PI / 2.0));
            path.closeSubpath();
            break;
        case Hexagon:
            path.addPolygon(regularPolygon(box, 6, 0.0));
            path.closeSubpath();
            break;
        case Heptagon:
            path.addPolygon(regularPolygon(box, 7, -M_PI / 2.0));
            path.closeSubpath();
            break;
        case Octagon:
            path.addPolygon(regularPolygon(box, 8, M_PI / 8.0));
            path.closeSubpath();
            break;
        case Star3:
            path.addPolygon(starPolygon(box, 3, 0.38));
            path.closeSubpath();
            break;
        case Star4:
            path.addPolygon(starPolygon(box, 4, 0.38));
            path.closeSubpath();
            break;
        case Star5:
            path.addPolygon(starPolygon(box, 5, 0.42));
            path.closeSubpath();
            break;
        case Star6:
            path.addPolygon(starPolygon(box, 6, 0.55));
            path.closeSubpath();
            break;
        case Star8:
            path.addPolygon(starPolygon(box, 8, 0.6));
            path.closeSubpath();
            break;
        case Arrow: {
            const double headX = box.right() - w * 0.38;
            const double thickness = h * 0.26;
            path.addPolygon(QPolygonF({QPointF(box.left(), centre.y() - thickness),
                                       QPointF(headX, centre.y() - thickness),
                                       QPointF(headX, box.top()),
                                       QPointF(box.right(), centre.y()),
                                       QPointF(headX, box.bottom()),
                                       QPointF(headX, centre.y() + thickness),
                                       QPointF(box.left(), centre.y() + thickness)}));
            path.closeSubpath();
            break;
        }
        case DoubleArrow: {
            const double headLeft = box.left() + w * 0.3;
            const double headRight = box.right() - w * 0.3;
            const double thickness = h * 0.22;
            path.addPolygon(QPolygonF({QPointF(box.left(), centre.y()),
                                       QPointF(headLeft, box.top()),
                                       QPointF(headLeft, centre.y() - thickness),
                                       QPointF(headRight, centre.y() - thickness),
                                       QPointF(headRight, box.top()),
                                       QPointF(box.right(), centre.y()),
                                       QPointF(headRight, box.bottom()),
                                       QPointF(headRight, centre.y() + thickness),
                                       QPointF(headLeft, centre.y() + thickness),
                                       QPointF(headLeft, box.bottom())}));
            path.closeSubpath();
            break;
        }
        case Chevron: {
            const double notch = w * 0.3;
            path.addPolygon(QPolygonF({box.topLeft(),
                                       QPointF(box.right() - notch, box.top()),
                                       QPointF(box.right(), centre.y()),
                                       QPointF(box.right() - notch, box.bottom()),
                                       box.bottomLeft(),
                                       QPointF(box.left() + notch, centre.y())}));
            path.closeSubpath();
            break;
        }
        case Cross:
        case Plus: {
            const double tw = w * (kind_ == Plus ? 0.34 : 0.3);
            const double th = h * (kind_ == Plus ? 0.34 : 0.3);
            path.addPolygon(QPolygonF({QPointF(centre.x() - tw / 2, box.top()),
                                       QPointF(centre.x() + tw / 2, box.top()),
                                       QPointF(centre.x() + tw / 2, centre.y() - th / 2),
                                       QPointF(box.right(), centre.y() - th / 2),
                                       QPointF(box.right(), centre.y() + th / 2),
                                       QPointF(centre.x() + tw / 2, centre.y() + th / 2),
                                       QPointF(centre.x() + tw / 2, box.bottom()),
                                       QPointF(centre.x() - tw / 2, box.bottom()),
                                       QPointF(centre.x() - tw / 2, centre.y() + th / 2),
                                       QPointF(box.left(), centre.y() + th / 2),
                                       QPointF(box.left(), centre.y() - th / 2),
                                       QPointF(centre.x() - tw / 2, centre.y() - th / 2)}));
            path.closeSubpath();
            break;
        }
        case Heart:
            path.moveTo(centre.x(), box.bottom());
            path.cubicTo(box.left() - w * 0.1, box.top() + h * 0.55,
                         box.left() + w * 0.18, box.top() - h * 0.1,
                         centre.x(), box.top() + h * 0.28);
            path.cubicTo(box.right() - w * 0.18, box.top() - h * 0.1,
                         box.right() + w * 0.1, box.top() + h * 0.55,
                         centre.x(), box.bottom());
            path.closeSubpath();
            break;
        case Crescent: {
            // A disc with a second, offset disc subtracted from it.
            QPainterPath outer;
            outer.addEllipse(box);
            QPainterPath inner;
            inner.addEllipse(QRectF(box.left() + w * 0.28, box.top() + h * 0.06,
                                    w * 0.86, h * 0.88));
            path = outer.subtracted(inner);
            break;
        }
        case Cloud: {
            // Four overlapping discs joined into one outline.
            QPainterPath blob;
            blob.addEllipse(QRectF(box.left(), box.top() + h * 0.42, w * 0.46, h * 0.58));
            blob.addEllipse(QRectF(box.left() + w * 0.18, box.top(), w * 0.44, h * 0.72));
            blob.addEllipse(QRectF(box.left() + w * 0.44, box.top() + h * 0.14,
                                   w * 0.4, h * 0.6));
            blob.addEllipse(QRectF(box.left() + w * 0.54, box.top() + h * 0.42,
                                   w * 0.46, h * 0.58));
            blob.addRect(QRectF(box.left() + w * 0.2, box.top() + h * 0.6, w * 0.6,
                                h * 0.4));
            path = blob.simplified();
            break;
        }
        case SpeechBubble: {
            const double bodyHeight = h * 0.76;
            QPainterPath body;
            body.addRoundedRect(QRectF(box.left(), box.top(), w, bodyHeight),
                                std::min(w, bodyHeight) * 0.2,
                                std::min(w, bodyHeight) * 0.2);
            QPainterPath tail;
            tail.addPolygon(QPolygonF({QPointF(box.left() + w * 0.22, box.top() + bodyHeight - 1),
                                       QPointF(box.left() + w * 0.2, box.bottom()),
                                       QPointF(box.left() + w * 0.46, box.top() + bodyHeight - 1)}));
            path = body.united(tail).simplified();
            break;
        }
        case Ring: {
            QPainterPath outer;
            outer.addEllipse(box);
            QPainterPath inner;
            inner.addEllipse(QRectF(box.left() + w * 0.25, box.top() + h * 0.25,
                                    w * 0.5, h * 0.5));
            path = outer.subtracted(inner);
            break;
        }
        case PieWedge:
            path.moveTo(centre);
            path.arcTo(box, 45.0, 270.0);
            path.closeSubpath();
            break;
        case Lightning:
            path.addPolygon(QPolygonF({QPointF(box.left() + w * 0.56, box.top()),
                                       QPointF(box.left() + w * 0.12, centre.y() + h * 0.06),
                                       QPointF(box.left() + w * 0.44, centre.y() + h * 0.06),
                                       QPointF(box.left() + w * 0.3, box.bottom()),
                                       QPointF(box.left() + w * 0.88, centre.y() - h * 0.1),
                                       QPointF(box.left() + w * 0.52, centre.y() - h * 0.1)}));
            path.closeSubpath();
            break;
        case Teardrop:
            path.moveTo(centre.x(), box.top());
            path.cubicTo(box.right(), box.top() + h * 0.42, box.right(),
                         box.bottom() - h * 0.1, centre.x(), box.bottom());
            path.cubicTo(box.left(), box.bottom() - h * 0.1, box.left(),
                         box.top() + h * 0.42, centre.x(), box.top());
            path.closeSubpath();
            break;
        default:
            path.addRect(box);
            break;
        }
        return path;
    }

    Kind kind_ = Rectangle;
    QRectF box_;
    QRectF startRect_;
    QPointF anchor_;
    QPointF dragStart_;
    bool hasShape_ = false;
    int grabbed_ = -1;
};

// ---------------------------------------------------------------- text

class TextTool : public InteractiveShapeTool {
public:
    QString id() const override { return QStringLiteral("text"); }
    QString name() const override { return QStringLiteral("Text"); }
    QString iconName() const override { return QStringLiteral("text"); }
    QString shortcut() const override { return QStringLiteral("T"); }
    QString hint() const override
    {
        return QStringLiteral("Click and type. Enter adds a line, Ctrl+Enter or a tool "
                              "change finishes, Escape cancels.");
    }
    QCursor cursor() const override { return QCursor(Qt::IBeamCursor); }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addFontCombo(QStringLiteral("family"), family_);
        bar.addSpin(QStringLiteral("size"), QStringLiteral("Size"), 4, 1000, pointSize_,
                    QStringLiteral(" pt"));
        bar.addToggle(QStringLiteral("bold"), QStringLiteral("text"),
                      QStringLiteral("Bold"), bold_);
        bar.addToggle(QStringLiteral("italic"), QStringLiteral("line"),
                      QStringLiteral("Italic"), italic_);
        bar.addToggle(QStringLiteral("underline"), QStringLiteral("adjust"),
                      QStringLiteral("Underline"), underline_);
        bar.addCombo(QStringLiteral("align"), QStringLiteral("Align"),
                     {QStringLiteral("Left"), QStringLiteral("Centre"),
                      QStringLiteral("Right")},
                     align_);
        bar.addSeparator();
        bar.addPercent(QStringLiteral("opacity"), QStringLiteral("Opacity"),
                       int(std::lround(opacity_ * 100.0)));
        bar.addCheck(QStringLiteral("antialias"), QStringLiteral("Antialiasing"),
                     antialias_);
        bar.addCombo(QStringLiteral("blend"), QStringLiteral("Blending"),
                     blendModeNames(), int(blend_));
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("family"))
            family_ = value.toString();
        else if (key == QLatin1String("size"))
            pointSize_ = std::max(4, value.toInt());
        else if (key == QLatin1String("bold"))
            bold_ = value.toBool();
        else if (key == QLatin1String("italic"))
            italic_ = value.toBool();
        else if (key == QLatin1String("underline"))
            underline_ = value.toBool();
        else if (key == QLatin1String("align"))
            align_ = std::clamp(value.toInt(), 0, 2);
        else
            InteractiveShapeTool::optionChanged(key, value);
        if (editing_ && refresh_)
            refresh_();
    }

    void activated(ToolContext& context) override
    {
        refresh_ = [this, &context] { renderPreview(context); };
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (editing_ && !lines_.isEmpty() && !(lines_.size() == 1 && lines_[0].isEmpty())) {
            commit(context);
        }
        if (!beginEdit(context))
            return;
        secondary_ = event.isSecondary();
        anchor_ = event.pos;
        lines_ = {QString()};
        caretLine_ = 0;
        caretColumn_ = 0;
        renderPreview(context);
        context.say(hint());
    }

    bool keyPress(ToolContext& context, QKeyEvent* event) override
    {
        if (!editing_)
            return false;
        if (event->key() == Qt::Key_Escape) {
            cancel(context);
            return true;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && event->modifiers().testFlag(Qt::ControlModifier)) {
            commit(context);
            return true;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            const QString tail = lines_[caretLine_].mid(caretColumn_);
            lines_[caretLine_].truncate(caretColumn_);
            lines_.insert(caretLine_ + 1, tail);
            ++caretLine_;
            caretColumn_ = 0;
        } else if (event->key() == Qt::Key_Backspace) {
            if (caretColumn_ > 0) {
                lines_[caretLine_].remove(--caretColumn_, 1);
            } else if (caretLine_ > 0) {
                caretColumn_ = lines_[caretLine_ - 1].size();
                lines_[caretLine_ - 1] += lines_[caretLine_];
                lines_.removeAt(caretLine_);
                --caretLine_;
            }
        } else if (event->key() == Qt::Key_Delete) {
            if (caretColumn_ < lines_[caretLine_].size()) {
                lines_[caretLine_].remove(caretColumn_, 1);
            } else if (caretLine_ + 1 < lines_.size()) {
                lines_[caretLine_] += lines_[caretLine_ + 1];
                lines_.removeAt(caretLine_ + 1);
            }
        } else if (event->key() == Qt::Key_Left) {
            if (caretColumn_ > 0)
                --caretColumn_;
            else if (caretLine_ > 0)
                caretColumn_ = lines_[--caretLine_].size();
        } else if (event->key() == Qt::Key_Right) {
            if (caretColumn_ < lines_[caretLine_].size())
                ++caretColumn_;
            else if (caretLine_ + 1 < lines_.size()) {
                ++caretLine_;
                caretColumn_ = 0;
            }
        } else if (event->key() == Qt::Key_Up) {
            if (caretLine_ > 0)
                caretColumn_ = std::min(caretColumn_, int(lines_[--caretLine_].size()));
        } else if (event->key() == Qt::Key_Down) {
            if (caretLine_ + 1 < lines_.size())
                caretColumn_ = std::min(caretColumn_, int(lines_[++caretLine_].size()));
        } else if (event->key() == Qt::Key_Home) {
            caretColumn_ = 0;
        } else if (event->key() == Qt::Key_End) {
            caretColumn_ = lines_[caretLine_].size();
        } else if (!event->text().isEmpty() && event->text().at(0).isPrint()) {
            lines_[caretLine_].insert(caretColumn_, event->text());
            caretColumn_ += event->text().size();
        } else {
            return false;
        }
        renderPreview(context);
        return true;
    }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!editing_)
            return;
        const QFont font = buildFont();
        const QFontMetricsF metrics(font);
        const double lineHeight = metrics.height();
        const double caretY = anchor_.y() + caretLine_ * lineHeight;
        const QString before = lines_[caretLine_].left(caretColumn_);
        const double offset = alignOffset(metrics, lines_[caretLine_]);
        const double caretX = anchor_.x() + offset + metrics.horizontalAdvance(before);
        painter.setPen(overlayPen(context.zoom, QColor(60, 160, 255, 230), 1.4));
        painter.drawLine(QPointF(caretX, caretY), QPointF(caretX, caretY + lineHeight));
        // A faint box shows where the text block sits.
        painter.setPen(overlayPen(context.zoom, QColor(60, 160, 255, 110), 1.0,
                                  Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        const QRectF box = textBounds(metrics);
        if (box.isValid())
            painter.drawRect(box);
    }

protected:
    void drawShape(QPainter& painter, const ToolContext& context) const override
    {
        if (lines_.isEmpty())
            return;
        const QFont font = buildFont();
        const QFontMetricsF metrics(font);
        painter.setFont(font);
        painter.setPen((secondary_ ? context.secondary : context.primary).toQColor());
        const double lineHeight = metrics.height();
        for (int i = 0; i < lines_.size(); ++i) {
            const double offset = alignOffset(metrics, lines_[i]);
            painter.drawText(QPointF(anchor_.x() + offset,
                                     anchor_.y() + i * lineHeight + metrics.ascent()),
                             lines_[i]);
        }
    }

    Rect shapeBounds() const override
    {
        if (lines_.isEmpty())
            return Rect();
        const QFontMetricsF metrics(buildFont());
        const QRectF box = textBounds(metrics);
        if (!box.isValid())
            return Rect();
        return Rect(int(std::floor(box.left())) - 4, int(std::floor(box.top())) - 4,
                    int(std::ceil(box.width())) + 9, int(std::ceil(box.height())) + 9);
    }

private:
    QFont buildFont() const
    {
        QFont font(family_);
        font.setPointSizeF(pointSize_);
        font.setBold(bold_);
        font.setItalic(italic_);
        font.setUnderline(underline_);
        font.setHintingPreference(QFont::PreferNoHinting);
        return font;
    }

    double alignOffset(const QFontMetricsF& metrics, const QString& line) const
    {
        if (align_ == 0)
            return 0.0;
        double widest = 0.0;
        for (const QString& other : lines_)
            widest = std::max(widest, metrics.horizontalAdvance(other));
        const double width = metrics.horizontalAdvance(line);
        return align_ == 1 ? (widest - width) / 2.0 : widest - width;
    }

    QRectF textBounds(const QFontMetricsF& metrics) const
    {
        double widest = 0.0;
        for (const QString& line : lines_)
            widest = std::max(widest, metrics.horizontalAdvance(line));
        const double height = metrics.height() * std::max(1, int(lines_.size()));
        return QRectF(anchor_.x(), anchor_.y(), std::max(widest, 2.0), height);
    }

    QString family_ = QStringLiteral("Sans Serif");
    int pointSize_ = 24;
    bool bold_ = false;
    bool italic_ = false;
    bool underline_ = false;
    int align_ = 0;
    QPointF anchor_;
    QStringList lines_;
    int caretLine_ = 0;
    int caretColumn_ = 0;
    std::function<void()> refresh_;
};

} // namespace

void registerShapeTools(ToolRegistry& registry)
{
    registry.add(std::make_unique<LineCurveTool>());
    registry.add(std::make_unique<ShapesTool>());
    registry.add(std::make_unique<TextTool>());
}

} // namespace tessel
