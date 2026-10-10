// Selection tools: rectangle, ellipse, lasso, magic wand, and the two move
// tools that act on the selected pixels or on the selection outline itself.
#include "tools/Tool.h"

#include <cmath>

#include <QKeyEvent>

#include "core/ImageOps.h"

namespace tessel {

namespace {

QList<QPair<QString, QString>> combineModeIcons()
{
    return {{QStringLiteral("select-rect"), QStringLiteral("Replace (default)")},
            {QStringLiteral("select-all"), QStringLiteral("Union: add to the selection (Ctrl)")},
            {QStringLiteral("deselect"), QStringLiteral("Subtract from the selection (Alt)")},
            {QStringLiteral("crop"), QStringLiteral("Intersect with the selection")},
            {QStringLiteral("invert"), QStringLiteral("Exclusive or")}};
}

CombineMode effectiveMode(CombineMode base, Qt::KeyboardModifiers modifiers)
{
    // Ctrl adds and Alt subtracts while dragging, as in most editors.
    if (modifiers.testFlag(Qt::ControlModifier) && modifiers.testFlag(Qt::AltModifier))
        return CombineMode::Intersect;
    if (modifiers.testFlag(Qt::ControlModifier))
        return CombineMode::Union;
    if (modifiers.testFlag(Qt::AltModifier))
        return CombineMode::Subtract;
    return base;
}

QRectF normalized(QPointF a, QPointF b, Qt::KeyboardModifiers modifiers)
{
    double x0 = std::min(a.x(), b.x());
    double y0 = std::min(a.y(), b.y());
    double w = std::abs(b.x() - a.x());
    double h = std::abs(b.y() - a.y());
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        const double side = std::max(w, h);
        if (b.x() < a.x())
            x0 = a.x() - side;
        if (b.y() < a.y())
            y0 = a.y() - side;
        w = h = side;
    }
    return QRectF(x0, y0, w, h);
}

// ---------------------------------------------------------------- base

class SelectionToolBase : public Tool {
public:
    QCursor cursor() const override { return QCursor(Qt::CrossCursor); }

    void buildOptions(ToolOptionsBar& bar) override
    {
        bar.addToggleGroup(QStringLiteral("mode"), QStringLiteral("Mode"),
                           combineModeIcons(), int(mode_));
        bar.addSeparator();
        bar.addCheck(QStringLiteral("antialias"), QStringLiteral("Antialiasing"),
                     antialias_);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("mode"))
            mode_ = static_cast<CombineMode>(std::clamp(value.toInt(), 0, 4));
        else if (key == QLatin1String("antialias"))
            antialias_ = value.toBool();
    }

protected:
    void applySelection(ToolContext& context, const Mask& mask, CombineMode mode)
    {
        recordSelectionEdit(*context.doc, *context.history, name(), [&] {
            context.doc->selection().combine(mask, mode);
        });
        context.repaintAll();
        if (context.documentChanged)
            context.documentChanged();
        const Selection& selection = context.doc->selection();
        if (!selection.isActive()) {
            context.say(QStringLiteral("Selection cleared"));
        } else {
            const Rect bounds = selection.bounds();
            context.say(QStringLiteral("Selection %1 x %2 at %3, %4")
                            .arg(bounds.w).arg(bounds.h).arg(bounds.x).arg(bounds.y));
        }
    }

    CombineMode mode_ = CombineMode::Replace;
    bool antialias_ = true;
};

// ---------------------------------------------------------------- rect / ellipse

class RectangleSelectTool : public SelectionToolBase {
public:
    explicit RectangleSelectTool(bool ellipse = false) : ellipse_(ellipse) {}

    QString id() const override
    {
        return ellipse_ ? QStringLiteral("select-ellipse") : QStringLiteral("select-rect");
    }
    QString name() const override
    {
        return ellipse_ ? QStringLiteral("Ellipse Select")
                        : QStringLiteral("Rectangle Select");
    }
    QString shortcut() const override { return QStringLiteral("S"); }
    QString hint() const override
    {
        return QStringLiteral("Drag to select. Shift keeps it square, Ctrl adds, Alt "
                              "subtracts.");
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!context.valid())
            return;
        start_ = event.pos;
        current_ = event.pos;
        dragging_ = true;
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        current_ = event.pos;
        modifiers_ = event.modifiers;
        context.repaintAll();
    }

    void release(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        dragging_ = false;
        const QRectF box = normalized(start_, event.pos, event.modifiers);
        if (box.width() < 0.5 || box.height() < 0.5) {
            // A click with no drag clears the selection, like clicking away.
            recordSelectionEdit(*context.doc, *context.history,
                                QStringLiteral("Deselect"),
                                [&] { context.doc->selection().clear(); });
            context.repaintAll();
            if (context.documentChanged)
                context.documentChanged();
            return;
        }
        QPainterPath path;
        if (ellipse_)
            path.addEllipse(box);
        else
            path.addRect(box);
        Mask mask = maskFromPath(path, context.doc->width(), context.doc->height(),
                                 antialias_);
        applySelection(context, mask, effectiveMode(mode_, event.modifiers));
    }

    void cancel(ToolContext& context) override
    {
        if (dragging_) {
            dragging_ = false;
            context.repaintAll();
        }
    }

    bool isBusy() const override { return dragging_; }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!dragging_)
            return;
        const QRectF box = normalized(start_, current_, modifiers_);
        painter.setBrush(QColor(80, 150, 255, 40));
        painter.setPen(overlayPen(context.zoom, QColor(255, 255, 255, 230), 1.0));
        if (ellipse_)
            painter.drawEllipse(box);
        else
            painter.drawRect(box);
    }

private:
    bool ellipse_;
    bool dragging_ = false;
    QPointF start_;
    QPointF current_;
    Qt::KeyboardModifiers modifiers_ = Qt::NoModifier;
};

// ---------------------------------------------------------------- lasso

class LassoSelectTool : public SelectionToolBase {
public:
    QString id() const override { return QStringLiteral("lasso"); }
    QString name() const override { return QStringLiteral("Lasso Select"); }
    QString iconName() const override { return QStringLiteral("lasso"); }
    QString shortcut() const override { return QStringLiteral("S"); }
    QString hint() const override
    {
        return QStringLiteral("Draw a freehand outline; it closes itself when you let go.");
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!context.valid())
            return;
        points_.clear();
        points_.append(event.pos);
        dragging_ = true;
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        // Skip points that are too close together to keep the path light.
        if (points_.isEmpty()
            || std::hypot(event.pos.x() - points_.last().x(),
                          event.pos.y() - points_.last().y()) > 0.75) {
            points_.append(event.pos);
            context.repaintAll();
        }
    }

    void release(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        dragging_ = false;
        if (points_.size() < 3) {
            points_.clear();
            context.repaintAll();
            return;
        }
        QPainterPath path;
        path.addPolygon(QPolygonF(points_));
        path.closeSubpath();
        Mask mask = maskFromPath(path, context.doc->width(), context.doc->height(),
                                 antialias_);
        points_.clear();
        applySelection(context, mask, effectiveMode(mode_, event.modifiers));
    }

    void cancel(ToolContext& context) override
    {
        if (dragging_ || !points_.isEmpty()) {
            dragging_ = false;
            points_.clear();
            context.repaintAll();
        }
    }

    bool isBusy() const override { return dragging_; }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (points_.size() < 2)
            return;
        QPainterPath path;
        path.addPolygon(QPolygonF(points_));
        painter.setBrush(QColor(80, 150, 255, 40));
        painter.setPen(overlayPen(context.zoom, QColor(255, 255, 255, 230), 1.0));
        painter.drawPath(path);
    }

private:
    QList<QPointF> points_;
    bool dragging_ = false;
};

// ---------------------------------------------------------------- magic wand

class MagicWandTool : public SelectionToolBase {
public:
    QString id() const override { return QStringLiteral("wand"); }
    QString name() const override { return QStringLiteral("Magic Wand"); }
    QString iconName() const override { return QStringLiteral("wand"); }
    QString shortcut() const override { return QStringLiteral("S"); }
    QString hint() const override
    {
        return QStringLiteral("Click to select a region of similar colour. Drag up or "
                              "down to adjust the tolerance.");
    }

    void buildOptions(ToolOptionsBar& bar) override
    {
        SelectionToolBase::buildOptions(bar);
        bar.addSeparator();
        bar.addSpin(QStringLiteral("tolerance"), QStringLiteral("Tolerance"), 0, 100,
                    int(std::lround(tolerance_ * 100.0)), QStringLiteral("%"));
        bar.addCombo(QStringLiteral("fill"), QStringLiteral("Flood mode"),
                     {QStringLiteral("Contiguous"), QStringLiteral("Global")},
                     contiguous_ ? 0 : 1);
        bar.addCheck(QStringLiteral("merged"), QStringLiteral("Sample merged"), merged_);
    }

    void optionChanged(const QString& key, const QVariant& value) override
    {
        if (key == QLatin1String("tolerance"))
            tolerance_ = std::clamp(value.toInt(), 0, 100) / 100.0;
        else if (key == QLatin1String("fill"))
            contiguous_ = value.toInt() == 0;
        else if (key == QLatin1String("merged"))
            merged_ = value.toBool();
        else
            SelectionToolBase::optionChanged(key, value);
    }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!context.valid())
            return;
        seed_ = event.pixel;
        dragStart_ = event.viewPos;
        baseTolerance_ = tolerance_;
        pendingMode_ = effectiveMode(mode_, event.modifiers);
        dragging_ = true;
        grow(context, tolerance_);
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        // Vertical drag nudges the tolerance for quick trial and error.
        const double delta = (event.viewPos.y() - dragStart_.y()) / 200.0;
        grow(context, std::clamp(baseTolerance_ + delta, 0.0, 1.0));
    }

    void release(ToolContext& context, const ToolEvent&) override
    {
        if (!dragging_)
            return;
        dragging_ = false;
        if (!pending_.isNull())
            applySelection(context, pending_, pendingMode_);
        pending_ = Mask();
    }

    void cancel(ToolContext& context) override
    {
        dragging_ = false;
        pending_ = Mask();
        context.repaintAll();
    }

    bool isBusy() const override { return dragging_; }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (preview_.isEmpty())
            return;
        painter.setBrush(QColor(80, 150, 255, 55));
        painter.setPen(overlayPen(context.zoom, QColor(255, 255, 255, 200), 1.0));
        painter.drawPath(preview_);
    }

private:
    void grow(ToolContext& context, double tolerance)
    {
        Layer* layer = context.doc->activeLayer();
        if (!layer)
            return;
        const Image& sample = merged_ ? context.doc->composite() : layer->image;
        pending_ = ops::floodFill(sample, seed_.x(), seed_.y(), tolerance, contiguous_,
                                  antialias_);
        preview_ = pathFromMask(pending_);
        context.say(QStringLiteral("Tolerance %1%  -  %2 pixels")
                        .arg(int(std::lround(tolerance * 100.0)))
                        .arg(pending_.countAbove(0.5f)));
        context.repaintAll();
    }

    QPoint seed_;
    QPointF dragStart_;
    double tolerance_ = 0.3;
    double baseTolerance_ = 0.3;
    bool contiguous_ = true;
    bool merged_ = false;
    bool dragging_ = false;
    Mask pending_;
    QPainterPath preview_;
    CombineMode pendingMode_ = CombineMode::Replace;
};

// ---------------------------------------------------------------- move pixels

// Lifts the selected pixels onto a floating buffer that can be dragged and
// scaled, then stamps them back down when the move is finished.
class MoveSelectedTool : public Tool {
public:
    QString id() const override { return QStringLiteral("move-selected"); }
    QString name() const override { return QStringLiteral("Move Selected Pixels"); }
    QString iconName() const override { return QStringLiteral("move-selected"); }
    QString shortcut() const override { return QStringLiteral("M"); }
    QString hint() const override
    {
        return QStringLiteral("Drag the selected pixels. Drag a corner handle to "
                              "scale, press Enter to finish or Escape to cancel.");
    }
    QCursor cursor() const override { return QCursor(Qt::SizeAllCursor); }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!context.valid())
            return;
        if (!floating_ && !lift(context))
            return;
        grabbedHandle_ = handleAt(event.pos, context.zoom);
        dragStart_ = event.pos;
        startRect_ = target_;
        dragging_ = true;
        constrain_ = event.modifiers.testFlag(Qt::ShiftModifier);
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_ || !floating_)
            return;
        constrain_ = event.modifiers.testFlag(Qt::ShiftModifier);
        const QPointF delta = event.pos - dragStart_;
        if (grabbedHandle_ < 0) {
            target_ = startRect_.translated(delta);
        } else {
            target_ = resizedRect(startRect_, grabbedHandle_, delta, constrain_);
        }
        render(context);
    }

    void release(ToolContext&, const ToolEvent&) override { dragging_ = false; }

    bool keyPress(ToolContext& context, QKeyEvent* event) override
    {
        if (!floating_)
            return false;
        const int step = event->modifiers().testFlag(Qt::ControlModifier) ? 10 : 1;
        switch (event->key()) {
        case Qt::Key_Left: target_.translate(-step, 0); break;
        case Qt::Key_Right: target_.translate(step, 0); break;
        case Qt::Key_Up: target_.translate(0, -step); break;
        case Qt::Key_Down: target_.translate(0, step); break;
        case Qt::Key_Return:
        case Qt::Key_Enter: commit(context); return true;
        case Qt::Key_Escape: cancel(context); return true;
        default: return false;
        }
        render(context);
        return true;
    }

    void commit(ToolContext& context) override
    {
        if (!floating_)
            return;
        floating_ = false;
        render(context);
        // Move the selection outline along with the pixels.
        const QPointF shift = target_.topLeft() - origin_.topLeft();
        Selection& selection = context.doc->selection();
        if (selection.isActive()) {
            selection.translate(int(std::lround(shift.x())), int(std::lround(shift.y())));
        }
        DocumentState after = context.doc->snapshot();
        context.history->push(std::make_unique<DocumentEdit>(
            name(), iconName(), std::move(before_), std::move(after)));
        context.doc->invalidate();
        context.repaintAll();
        if (context.documentChanged)
            context.documentChanged();
        lifted_ = Image();
    }

    void cancel(ToolContext& context) override
    {
        if (!floating_)
            return;
        floating_ = false;
        dragging_ = false;
        context.doc->restore(before_);
        context.repaintAll();
        if (context.documentChanged)
            context.documentChanged();
        lifted_ = Image();
    }

    bool isBusy() const override { return floating_; }

    void paintOverlay(QPainter& painter, const ToolContext& context) const override
    {
        if (!floating_)
            return;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(overlayPen(context.zoom, QColor(255, 255, 255, 220), 1.0,
                                  Qt::DashLine));
        painter.drawRect(target_);
        const double size = 4.0 / std::max(0.05, context.zoom);
        painter.setBrush(QColor(255, 255, 255, 230));
        painter.setPen(overlayPen(context.zoom, QColor(30, 30, 30, 220), 1.0));
        for (const QPointF& corner : corners(target_))
            painter.drawRect(QRectF(corner.x() - size, corner.y() - size, size * 2, size * 2));
    }

private:
    static QList<QPointF> corners(const QRectF& rect)
    {
        return {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
    }

    int handleAt(QPointF point, double zoom) const
    {
        const double reach = 6.0 / std::max(0.05, zoom);
        const QList<QPointF> list = corners(target_);
        for (int i = 0; i < list.size(); ++i) {
            if (std::hypot(point.x() - list[i].x(), point.y() - list[i].y()) <= reach)
                return i;
        }
        return -1;
    }

    static QRectF resizedRect(const QRectF& start, int handle, QPointF delta,
                              bool keepAspect)
    {
        QRectF rect = start;
        switch (handle) {
        case 0: rect.setTopLeft(start.topLeft() + delta); break;
        case 1: rect.setTopRight(start.topRight() + delta); break;
        case 2: rect.setBottomRight(start.bottomRight() + delta); break;
        default: rect.setBottomLeft(start.bottomLeft() + delta); break;
        }
        if (keepAspect && start.width() > 0.5 && start.height() > 0.5) {
            const double ratio = start.width() / start.height();
            const double width = std::max(1.0, std::abs(rect.width()));
            const double height = std::max(1.0, width / ratio);
            QPointF anchor;
            switch (handle) {
            case 0: anchor = start.bottomRight(); break;
            case 1: anchor = start.bottomLeft(); break;
            case 2: anchor = start.topLeft(); break;
            default: anchor = start.topRight(); break;
            }
            const double signX = (handle == 0 || handle == 3) ? -1.0 : 1.0;
            const double signY = (handle == 0 || handle == 1) ? -1.0 : 1.0;
            rect = QRectF(anchor, QPointF(anchor.x() + signX * width,
                                          anchor.y() + signY * height)).normalized();
        }
        if (std::abs(rect.width()) < 1.0)
            rect.setWidth(rect.width() < 0 ? -1.0 : 1.0);
        if (std::abs(rect.height()) < 1.0)
            rect.setHeight(rect.height() < 0 ? -1.0 : 1.0);
        return rect;
    }

    bool lift(ToolContext& context)
    {
        Layer* layer = context.doc->activeLayer();
        if (!layer)
            return false;
        Selection& selection = context.doc->selection();
        const Rect bounds = selection.isActive() ? selection.bounds() : layer->image.rect();
        if (bounds.isEmpty())
            return false;

        before_ = context.doc->snapshot();
        layerUid_ = layer->uid();
        origin_ = QRectF(bounds.x, bounds.y, bounds.w, bounds.h);
        target_ = origin_;

        // Copy the selected pixels out, applying the selection as alpha, then
        // clear them from the layer.
        lifted_ = layer->image.cropped(bounds);
        if (const Mask* mask = selection.clipMask()) {
            for (int y = 0; y < bounds.h; ++y) {
                Rgba* line = lifted_.row(y);
                const float* sel = mask->row(bounds.y + y);
                for (int x = 0; x < bounds.w; ++x)
                    line[x].a = toByte(line[x].a * (1.0f / 255.0f) * sel[bounds.x + x]);
            }
            Mask cut = *mask;
            ops::eraseMask(layer->image, cut, 1.0f, bounds);
        } else {
            ops::fillRegion(layer->image, bounds, Rgba(0, 0, 0, 0));
        }
        cleared_ = layer->image;
        floating_ = true;
        context.doc->invalidateLayer(layer);
        render(context);
        return true;
    }

    void render(ToolContext& context)
    {
        Layer* layer = context.doc->layerByUid(layerUid_);
        if (!layer)
            return;
        layer->image = cleared_;
        const QRectF box = target_.normalized();
        const int width = std::max(1, int(std::lround(box.width())));
        const int height = std::max(1, int(std::lround(box.height())));
        Image placed = (width == lifted_.width() && height == lifted_.height())
            ? lifted_
            : ops::resize(lifted_, width, height, true);
        const int x = int(std::lround(box.x()));
        const int y = int(std::lround(box.y()));
        Image staged(layer->image.width(), layer->image.height());
        staged.blit(placed, x, y);
        ops::imageOver(layer->image, staged, nullptr, BlendMode::Normal,
                       Rect(x, y, placed.width(), placed.height()));
        context.doc->invalidateLayer(layer);
        context.repaintAll();
        context.say(QStringLiteral("Moved to %1, %2  -  %3 x %4")
                        .arg(x).arg(y).arg(width).arg(height));
    }

    DocumentState before_;
    Image lifted_;
    Image cleared_;
    QRectF origin_;
    QRectF target_;
    QRectF startRect_;
    QPointF dragStart_;
    int layerUid_ = 0;
    int grabbedHandle_ = -1;
    bool floating_ = false;
    bool dragging_ = false;
    bool constrain_ = false;
};

// ---------------------------------------------------------------- move outline

class MoveSelectionTool : public Tool {
public:
    QString id() const override { return QStringLiteral("move-selection"); }
    QString name() const override { return QStringLiteral("Move Selection"); }
    QString iconName() const override { return QStringLiteral("move-selection"); }
    QString shortcut() const override { return QStringLiteral("M"); }
    QString hint() const override
    {
        return QStringLiteral("Drags the selection outline without moving any pixels.");
    }
    QCursor cursor() const override { return QCursor(Qt::SizeAllCursor); }

    void press(ToolContext& context, const ToolEvent& event) override
    {
        if (!context.valid() || !context.doc->selection().isActive())
            return;
        before_ = *context.doc->selection().clipMask();
        start_ = event.pixel;
        offset_ = QPoint();
        dragging_ = true;
    }

    void move(ToolContext& context, const ToolEvent& event) override
    {
        if (!dragging_)
            return;
        const QPoint next = event.pixel - start_;
        if (next == offset_)
            return;
        Mask shifted = before_;
        Selection& selection = context.doc->selection();
        selection.setMask(before_);
        selection.translate(next.x(), next.y());
        offset_ = next;
        context.repaintAll();
        const Rect bounds = selection.bounds();
        context.say(QStringLiteral("Selection at %1, %2").arg(bounds.x).arg(bounds.y));
    }

    void release(ToolContext& context, const ToolEvent&) override
    {
        if (!dragging_)
            return;
        dragging_ = false;
        if (offset_.isNull())
            return;
        const Mask after = context.doc->selection().isActive()
            ? *context.doc->selection().clipMask()
            : Mask();
        context.history->push(std::make_unique<SelectionEdit>(
            name(), true, before_, context.doc->selection().isActive(), after));
        if (context.documentChanged)
            context.documentChanged();
    }

    void cancel(ToolContext& context) override
    {
        if (!dragging_)
            return;
        dragging_ = false;
        context.doc->selection().setMask(before_);
        context.repaintAll();
    }

    bool isBusy() const override { return dragging_; }

private:
    Mask before_;
    QPoint start_;
    QPoint offset_;
    bool dragging_ = false;
};

} // namespace

void registerSelectTools(ToolRegistry& registry)
{
    registry.add(std::make_unique<RectangleSelectTool>(false));
    registry.add(std::make_unique<MoveSelectedTool>());
    registry.add(std::make_unique<LassoSelectTool>());
    registry.add(std::make_unique<MoveSelectionTool>());
    registry.add(std::make_unique<RectangleSelectTool>(true));
    registry.add(std::make_unique<MagicWandTool>());
}

} // namespace tessel
