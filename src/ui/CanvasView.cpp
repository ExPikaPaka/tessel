#include "ui/CanvasView.h"

#include <cmath>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tessel {

namespace {

// The zoom steps the view walks through, matching the familiar percentages.
const std::vector<double>& zoomSteps()
{
    static const std::vector<double> steps = {
        0.01, 0.02, 0.03, 0.04, 0.05, 0.0625, 0.0833, 0.125, 0.1667, 0.25,
        0.3333, 0.5, 0.6667, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0,
        16.0, 20.0, 32.0,
    };
    return steps;
}

constexpr double kMinZoom = 0.01;
constexpr double kMaxZoom = 32.0;
constexpr int kRulerSize = 18;

} // namespace

CanvasView::CanvasView(std::unique_ptr<Document> doc, QWidget* parent)
    : QAbstractScrollArea(parent), doc_(std::move(doc))
{
    history_ = std::make_unique<History>(doc_.get());
    history_->setChangedCallback([this] {
        viewport()->update();
        emit documentChanged();
        emit modifiedChanged();
    });
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFrameShape(QFrame::NoFrame);
    horizontalScrollBar()->setSingleStep(32);
    verticalScrollBar()->setSingleStep(32);

    antsTimer_.setInterval(120);
    connect(&antsTimer_, &QTimer::timeout, this, [this] {
        antsPhase_ = (antsPhase_ + 1) % 8;
        if (doc_->selection().isActive() && showSelection_)
            viewport()->update();
    });
    antsTimer_.start();
    updateScrollBars();
}

CanvasView::~CanvasView() = default;

// ----------------------------------------------------------------- geometry

QSizeF CanvasView::scaledCanvasSize() const
{
    return QSizeF(doc_->width() * zoom_, doc_->height() * zoom_);
}

QPointF CanvasView::canvasOrigin() const
{
    const QSizeF scaled = scaledCanvasSize();
    const QSize port = viewport()->size();
    const double extraLeft = showRulers_ ? kRulerSize : 0;
    double x = extraLeft;
    double y = showRulers_ ? kRulerSize : 0;
    if (scaled.width() + extraLeft < port.width())
        x = (port.width() - scaled.width()) / 2.0;
    else
        x = extraLeft - horizontalScrollBar()->value();
    if (scaled.height() + y < port.height())
        y = (port.height() - scaled.height()) / 2.0;
    else
        y = (showRulers_ ? kRulerSize : 0) - verticalScrollBar()->value();
    return QPointF(std::floor(x), std::floor(y));
}

QPointF CanvasView::docFromView(QPointF viewPos) const
{
    const QPointF origin = canvasOrigin();
    return QPointF((viewPos.x() - origin.x()) / zoom_, (viewPos.y() - origin.y()) / zoom_);
}

QPointF CanvasView::viewFromDoc(QPointF docPos) const
{
    const QPointF origin = canvasOrigin();
    return QPointF(origin.x() + docPos.x() * zoom_, origin.y() + docPos.y() * zoom_);
}

void CanvasView::updateScrollBars()
{
    const QSizeF scaled = scaledCanvasSize();
    const QSize port = viewport()->size();
    const int rulers = showRulers_ ? kRulerSize : 0;
    const int maxX = std::max(0, int(std::ceil(scaled.width())) + rulers - port.width());
    const int maxY = std::max(0, int(std::ceil(scaled.height())) + rulers - port.height());
    horizontalScrollBar()->setRange(0, maxX);
    horizontalScrollBar()->setPageStep(port.width());
    verticalScrollBar()->setRange(0, maxY);
    verticalScrollBar()->setPageStep(port.height());
}

void CanvasView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    if (fitMode_)
        zoomToFit();
    else
        updateScrollBars();
}

void CanvasView::scrollContentsBy(int, int) { viewport()->update(); }

// ----------------------------------------------------------------- zoom

void CanvasView::setZoom(double zoom, QPointF anchorDoc)
{
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (std::abs(zoom - zoom_) < 1.0e-9)
        return;
    if (anchorDoc.x() < 0)
        anchorDoc = QPointF(doc_->width() / 2.0, doc_->height() / 2.0);
    const QPointF anchorView = viewFromDoc(anchorDoc);
    fitMode_ = false;
    zoom_ = zoom;
    updateScrollBars();
    // Keep the anchor point under the same screen position.
    const QPointF newView = viewFromDoc(anchorDoc);
    horizontalScrollBar()->setValue(horizontalScrollBar()->value()
                                    + int(std::lround(newView.x() - anchorView.x())));
    verticalScrollBar()->setValue(verticalScrollBar()->value()
                                  + int(std::lround(newView.y() - anchorView.y())));
    viewport()->update();
    emit zoomChanged(zoom_);
}

void CanvasView::zoomIn()
{
    for (double step : zoomSteps()) {
        if (step > zoom_ * 1.001) {
            setZoom(step, docFromView(QPointF(viewport()->width() / 2.0,
                                              viewport()->height() / 2.0)));
            return;
        }
    }
    setZoom(kMaxZoom);
}

void CanvasView::zoomOut()
{
    const std::vector<double>& steps = zoomSteps();
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (*it < zoom_ * 0.999) {
            setZoom(*it, docFromView(QPointF(viewport()->width() / 2.0,
                                             viewport()->height() / 2.0)));
            return;
        }
    }
    setZoom(kMinZoom);
}

void CanvasView::zoomToFit()
{
    const int rulers = showRulers_ ? kRulerSize : 0;
    const double availableW = std::max(32, viewport()->width() - rulers - 16);
    const double availableH = std::max(32, viewport()->height() - rulers - 16);
    const double fit = std::min(availableW / std::max(1, doc_->width()),
                                availableH / std::max(1, doc_->height()));
    zoom_ = std::clamp(fit, kMinZoom, kMaxZoom);
    fitMode_ = true;
    updateScrollBars();
    viewport()->update();
    emit zoomChanged(zoom_);
}

void CanvasView::zoomToActualSize() { setZoom(1.0); }

void CanvasView::zoomToSelection()
{
    const Rect bounds = doc_->selection().bounds();
    if (bounds.isEmpty())
        return;
    const double availableW = std::max(32, viewport()->width() - 24);
    const double availableH = std::max(32, viewport()->height() - 24);
    const double target = std::min(availableW / std::max(1, bounds.w),
                                   availableH / std::max(1, bounds.h));
    setZoom(target, QPointF(bounds.x + bounds.w / 2.0, bounds.y + bounds.h / 2.0));
}

// ----------------------------------------------------------------- tools

void CanvasView::setTool(Tool* tool)
{
    if (tool_ == tool)
        return;
    ToolContext context = makeContext();
    if (tool_) {
        tool_->commit(context);
        tool_->deactivated(context);
    }
    tool_ = tool;
    if (tool_) {
        ToolContext fresh = makeContext();
        tool_->activated(fresh);
        emit statusMessage(tool_->hint());
    }
    applyCursor();
    viewport()->update();
}

void CanvasView::applyCursor()
{
    if (panning_ || spaceHeld_) {
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    viewport()->setCursor(tool_ ? tool_->cursor() : QCursor(Qt::ArrowCursor));
}

ToolContext CanvasView::makeContext()
{
    ToolContext context;
    context.doc = doc_.get();
    context.history = history_.get();
    context.primary = primary_;
    context.secondary = secondary_;
    context.zoom = zoom_;
    context.invalidate = [this](const Rect& area) {
        if (area.isEmpty())
            viewport()->update();
        else
            invalidateDocRect(area);
    };
    context.documentChanged = [this] {
        emit documentChanged();
        emit modifiedChanged();
    };
    context.status = [this](const QString& text) { emit statusMessage(text); };
    context.pickedColor = [this](Rgba color, bool isPrimary) {
        emit colorPicked(color, isPrimary);
    };
    context.zoomTo = [this](double zoom, QPointF anchor) { setZoom(zoom, anchor); };
    context.scrollBy = [this](QPointF delta) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value()
                                        + int(std::lround(delta.x())));
        verticalScrollBar()->setValue(verticalScrollBar()->value()
                                      + int(std::lround(delta.y())));
    };
    context.requestTool = [this](const QString& id) { emit toolRequested(id); };
    return context;
}

void CanvasView::commitPendingTool()
{
    if (!tool_)
        return;
    ToolContext context = makeContext();
    tool_->commit(context);
}

void CanvasView::cancelPendingTool()
{
    if (!tool_)
        return;
    ToolContext context = makeContext();
    tool_->cancel(context);
}

void CanvasView::refresh()
{
    updateScrollBars();
    viewport()->update();
}

void CanvasView::notifyDocumentChanged()
{
    emit documentChanged();
    emit modifiedChanged();
}

void CanvasView::invalidateDocRect(const Rect& area)
{
    const QPointF topLeft = viewFromDoc(QPointF(area.x, area.y));
    const QPointF bottomRight = viewFromDoc(QPointF(area.right(), area.bottom()));
    QRect box(topLeft.toPoint(), bottomRight.toPoint());
    viewport()->update(box.adjusted(-2, -2, 2, 2));
}

void CanvasView::setShowGrid(bool show)
{
    showGrid_ = show;
    viewport()->update();
}

void CanvasView::setShowSelection(bool show)
{
    showSelection_ = show;
    viewport()->update();
}

void CanvasView::setShowRulers(bool show)
{
    showRulers_ = show;
    updateScrollBars();
    viewport()->update();
}

// ----------------------------------------------------------------- painting

void CanvasView::paintEvent(QPaintEvent*)
{
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), theme::canvasBackdropColor());
    drawCanvas(painter);
    if (showRulers_)
        drawRulers(painter);
}

void CanvasView::drawCanvas(QPainter& painter)
{
    const QPointF origin = canvasOrigin();
    const QSizeF scaled = scaledCanvasSize();
    const QRectF target(origin, scaled);

    // Transparency checkerboard, anchored to the canvas so it does not crawl.
    painter.save();
    painter.setClipRect(target);
    QBrush checker(icons::checkerPixmap(8));
    checker.setTransform(QTransform::fromTranslate(origin.x(), origin.y()));
    painter.fillRect(target, checker);
    painter.restore();

    const QImage image = doc_->composite().view();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
    painter.drawImage(target, image, QRectF(0, 0, doc_->width(), doc_->height()));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    // Canvas edge.
    painter.setPen(QPen(theme::gridColor(), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(target.x() - 0.5, target.y() - 0.5, target.width() + 1,
                            target.height() + 1));

    if (showGrid_ && zoom_ >= 4.0)
        drawPixelGrid(painter);

    // Tool overlay and selection are drawn in document space.
    painter.save();
    painter.setClipRect(target.adjusted(-1, -1, 1, 1));
    painter.translate(origin);
    painter.scale(zoom_, zoom_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (showSelection_)
        drawSelection(painter);
    if (tool_) {
        ToolContext context = makeContext();
        tool_->paintOverlay(painter, context);
    }
    painter.restore();
}

void CanvasView::drawSelection(QPainter& painter)
{
    const Selection& selection = doc_->selection();
    if (!selection.isActive())
        return;
    const QPainterPath& path = selection.outline();
    if (path.isEmpty())
        return;
    // Marching ants: a black dashed line over a white one, offset over time.
    QPen under(QColor(255, 255, 255, 235));
    under.setWidthF(1.0 / zoom_);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(under);
    painter.drawPath(path);

    QPen over(QColor(20, 20, 20, 235));
    over.setWidthF(1.0 / zoom_);
    const double dash = 4.0 / zoom_ / over.widthF();
    over.setDashPattern({dash, dash});
    over.setDashOffset(antsPhase_ * dash / 4.0);
    painter.setPen(over);
    painter.drawPath(path);
}

void CanvasView::drawPixelGrid(QPainter& painter)
{
    const QPointF origin = canvasOrigin();
    QColor color = theme::gridColor();
    color.setAlpha(110);
    painter.setPen(QPen(color, 1));
    const int firstX = std::max(0, int((0 - origin.x()) / zoom_));
    const int lastX = std::min(doc_->width(), int((viewport()->width() - origin.x()) / zoom_) + 1);
    const int firstY = std::max(0, int((0 - origin.y()) / zoom_));
    const int lastY = std::min(doc_->height(), int((viewport()->height() - origin.y()) / zoom_) + 1);
    for (int x = firstX; x <= lastX; ++x) {
        const double at = origin.x() + x * zoom_;
        painter.drawLine(QPointF(at, origin.y() + firstY * zoom_),
                         QPointF(at, origin.y() + lastY * zoom_));
    }
    for (int y = firstY; y <= lastY; ++y) {
        const double at = origin.y() + y * zoom_;
        painter.drawLine(QPointF(origin.x() + firstX * zoom_, at),
                         QPointF(origin.x() + lastX * zoom_, at));
    }
}

void CanvasView::drawRulers(QPainter& painter)
{
    const QPointF origin = canvasOrigin();
    const QRect port = viewport()->rect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::panelColor());
    painter.drawRect(QRect(0, 0, port.width(), kRulerSize));
    painter.drawRect(QRect(0, 0, kRulerSize, port.height()));
    painter.setPen(QPen(theme::gridColor(), 1));
    painter.drawLine(0, kRulerSize, port.width(), kRulerSize);
    painter.drawLine(kRulerSize, 0, kRulerSize, port.height());

    // Pick a tick spacing that stays readable at any zoom.
    double spacing = 10.0;
    while (spacing * zoom_ < 48.0)
        spacing *= (std::abs(spacing - 10.0) < 1e-9 || std::abs(spacing - 100.0) < 1e-9)
            ? 5.0 : 2.0;
    QFont font = painter.font();
    font.setPointSizeF(7.0);
    painter.setFont(font);
    painter.setPen(theme::textColor());
    for (double x = 0; x <= doc_->width(); x += spacing) {
        const double at = origin.x() + x * zoom_;
        if (at < kRulerSize || at > port.width())
            continue;
        painter.drawLine(QPointF(at, kRulerSize - 5), QPointF(at, kRulerSize));
        painter.drawText(QPointF(at + 2, kRulerSize - 7), QString::number(int(x)));
    }
    for (double y = 0; y <= doc_->height(); y += spacing) {
        const double at = origin.y() + y * zoom_;
        if (at < kRulerSize || at > port.height())
            continue;
        painter.drawLine(QPointF(kRulerSize - 5, at), QPointF(kRulerSize, at));
        painter.save();
        painter.translate(QPointF(kRulerSize - 7, at - 2));
        painter.rotate(-90);
        painter.drawText(QPointF(0, 0), QString::number(int(y)));
        painter.restore();
    }
    // Cursor markers.
    painter.setPen(QPen(theme::accentColor(), 1));
    const QPointF cursor = viewFromDoc(QPointF(lastPixel_.x(), lastPixel_.y()));
    painter.drawLine(QPointF(cursor.x(), 0), QPointF(cursor.x(), kRulerSize));
    painter.drawLine(QPointF(0, cursor.y()), QPointF(kRulerSize, cursor.y()));
}

// ----------------------------------------------------------------- input

ToolEvent CanvasView::buildEvent(QMouseEvent* event) const
{
    ToolEvent out;
    out.viewPos = event->position();
    out.pos = docFromView(event->position());
    out.pixel = QPoint(int(std::floor(out.pos.x())), int(std::floor(out.pos.y())));
    out.button = event->button();
    out.buttons = event->buttons();
    out.modifiers = event->modifiers();
    return out;
}

void CanvasView::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::MiddleButton || spaceHeld_) {
        panning_ = true;
        panAnchor_ = event->position().toPoint();
        applyCursor();
        return;
    }
    if (!tool_)
        return;
    ToolContext context = makeContext();
    tool_->press(context, buildEvent(event));
}

void CanvasView::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF docPos = docFromView(event->position());
    const QPoint pixel(int(std::floor(docPos.x())), int(std::floor(docPos.y())));
    if (pixel != lastPixel_) {
        lastPixel_ = pixel;
        emit cursorMoved(pixel, doc_->rect().contains(pixel.x(), pixel.y()));
        if (showRulers_)
            viewport()->update(QRect(0, 0, viewport()->width(), kRulerSize));
    }
    if (panning_) {
        const QPoint delta = panAnchor_ - event->position().toPoint();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() + delta.y());
        panAnchor_ = event->position().toPoint();
        return;
    }
    if (!tool_)
        return;
    ToolContext context = makeContext();
    tool_->move(context, buildEvent(event));
}

void CanvasView::mouseReleaseEvent(QMouseEvent* event)
{
    if (panning_) {
        panning_ = false;
        applyCursor();
        return;
    }
    if (!tool_)
        return;
    ToolContext context = makeContext();
    tool_->release(context, buildEvent(event));
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!tool_)
        return;
    ToolContext context = makeContext();
    tool_->doubleClick(context, buildEvent(event));
}

void CanvasView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const double steps = event->angleDelta().y() / 120.0;
        const QPointF anchor = docFromView(event->position());
        if (steps > 0) {
            for (int i = 0; i < int(std::ceil(steps)); ++i) {
                const double before = zoom_;
                for (double step : zoomSteps()) {
                    if (step > zoom_ * 1.001) {
                        setZoom(step, anchor);
                        break;
                    }
                }
                if (std::abs(before - zoom_) < 1e-12)
                    break;
            }
        } else if (steps < 0) {
            for (int i = 0; i < int(std::ceil(-steps)); ++i) {
                const double before = zoom_;
                const std::vector<double>& list = zoomSteps();
                for (auto it = list.rbegin(); it != list.rend(); ++it) {
                    if (*it < zoom_ * 0.999) {
                        setZoom(*it, anchor);
                        break;
                    }
                }
                if (std::abs(before - zoom_) < 1e-12)
                    break;
            }
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void CanvasView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = true;
        applyCursor();
        event->accept();
        return;
    }
    if (tool_) {
        ToolContext context = makeContext();
        if (event->key() == Qt::Key_Escape) {
            tool_->cancel(context);
            event->accept();
            return;
        }
        if (tool_->keyPress(context, event)) {
            event->accept();
            return;
        }
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void CanvasView::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = false;
        applyCursor();
        event->accept();
        return;
    }
    QAbstractScrollArea::keyReleaseEvent(event);
}

void CanvasView::leaveEvent(QEvent* event)
{
    emit cursorMoved(lastPixel_, false);
    QAbstractScrollArea::leaveEvent(event);
}

} // namespace tessel
