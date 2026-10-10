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
    const double rulers = showRulers_ ? kRulerSize : 0;

    // On each axis the image is either larger than the viewport, in which case
    // the scroll bar positions it, or it fits, in which case it is centred and
    // nudged by the free pan offset.
    double x = 0.0;
    if (scaled.width() + rulers < port.width())
        x = (port.width() - scaled.width()) / 2.0 + panOffset_.x();
    else
        x = rulers - horizontalScrollBar()->value();

    double y = 0.0;
    if (scaled.height() + rulers < port.height())
        y = (port.height() - scaled.height()) / 2.0 + panOffset_.y();
    else
        y = rulers - verticalScrollBar()->value();

    return QPointF(std::floor(x), std::floor(y));
}

void CanvasView::clampPanOffset()
{
    // Keep a strip of the image reachable so it can never be flung out of view.
    const QSizeF scaled = scaledCanvasSize();
    const QSize port = viewport()->size();
    const double slackX = std::max(0.0, (port.width() + scaled.width()) / 2.0 - 48.0);
    const double slackY = std::max(0.0, (port.height() + scaled.height()) / 2.0 - 48.0);
    panOffset_.setX(std::clamp(panOffset_.x(), -slackX, slackX));
    panOffset_.setY(std::clamp(panOffset_.y(), -slackY, slackY));
}

void CanvasView::panBy(QPointF delta)
{
    const QSizeF scaled = scaledCanvasSize();
    const QSize port = viewport()->size();
    const double rulers = showRulers_ ? kRulerSize : 0;

    if (scaled.width() + rulers < port.width())
        panOffset_.setX(panOffset_.x() - delta.x());
    else
        horizontalScrollBar()->setValue(horizontalScrollBar()->value()
                                        + int(std::lround(delta.x())));

    if (scaled.height() + rulers < port.height())
        panOffset_.setY(panOffset_.y() - delta.y());
    else
        verticalScrollBar()->setValue(verticalScrollBar()->value()
                                      + int(std::lround(delta.y())));

    clampPanOffset();
    viewport()->update();
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
    // Keep the anchor point under the same screen position. Routed through
    // panBy so this also works when the image is smaller than the viewport.
    const QPointF newView = viewFromDoc(anchorDoc);
    panBy(newView - anchorView);
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
    panOffset_ = QPointF();
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
    context.scrollBy = [this](QPointF delta) { panBy(delta); };
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

void CanvasView::setRulerUnit(Unit unit)
{
    if (rulerUnit_ == unit)
        return;
    rulerUnit_ = unit;
    if (showRulers_)
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

    // Tool overlay and selection are drawn in document space. The clip is the
    // viewport rather than the canvas, because a tool may legitimately draw
    // outside the image: placing a picture larger than the canvas puts its
    // corner handles beyond the edge, and they have to be reachable.
    painter.save();
    painter.setClipRect(viewport()->rect());
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

    // The spacing is chosen in the ruler unit, so inches step in halves and
    // quarters rather than in round pixel counts.
    const double dpi = doc_->resolution();
    const units::RulerTicks ticks = units::rulerTicks(rulerUnit_, dpi, zoom_);
    const double minor = ticks.major / std::max(1, ticks.minorPerMajor);
    QFont font = painter.font();
    font.setPointSizeF(7.0);
    painter.setFont(font);
    painter.setPen(theme::textColor());

    // Walks only the ticks that can land in the viewport: zoomed into a large
    // document there are far more of them than there are pixels to show. The
    // position is recomputed from the index rather than accumulated, so no
    // rounding error creeps along the ruler.
    const auto forEachTick = [&](double step, double extent, double originPos,
                                 double limit, const auto& draw) {
        const double screenStep = step * zoom_;
        if (!(screenStep > 0.0))
            return;
        double index = std::max(0.0, std::floor((kRulerSize - originPos) / screenStep));
        for (; index * step <= extent + 0.5; ++index) {
            const double at = originPos + index * screenStep;
            if (at > limit)
                return;
            if (at >= kRulerSize)
                draw(index * step, at);
        }
    };

    if (ticks.minorPerMajor > 1) {
        forEachTick(minor, doc_->width(), origin.x(), port.width(),
                    [&](double, double at) {
                        painter.drawLine(QPointF(at, kRulerSize - 3),
                                         QPointF(at, kRulerSize));
                    });
        forEachTick(minor, doc_->height(), origin.y(), port.height(),
                    [&](double, double at) {
                        painter.drawLine(QPointF(kRulerSize - 3, at),
                                         QPointF(kRulerSize, at));
                    });
    }
    forEachTick(ticks.major, doc_->width(), origin.x(), port.width(),
                [&](double value, double at) {
                    painter.drawLine(QPointF(at, kRulerSize - 7),
                                     QPointF(at, kRulerSize));
                    painter.drawText(QPointF(at + 2, kRulerSize - 8),
                                     units::rulerLabel(value, rulerUnit_, dpi));
                });
    forEachTick(ticks.major, doc_->height(), origin.y(), port.height(),
                [&](double value, double at) {
                    painter.drawLine(QPointF(kRulerSize - 7, at),
                                     QPointF(kRulerSize, at));
                    painter.save();
                    painter.translate(QPointF(kRulerSize - 8, at - 2));
                    painter.rotate(-90);
                    painter.drawText(QPointF(0, 0),
                                     units::rulerLabel(value, rulerUnit_, dpi));
                    painter.restore();
                });
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
        panBy(QPointF(delta));
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
    // The wheel zooms about the pointer, with or without Ctrl: on a canvas that
    // is what the wheel is for. Shift scrolls sideways and the plain scroll
    // bars, the middle button and the space bar still move the view.
    if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
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
    // Escape and Enter work in layers: they first finish or abandon whatever
    // the tool has in progress, and only once there is nothing in progress do
    // they act on the selection. Escape used to stop at the first layer, so it
    // appeared to do nothing at all whenever a selection was simply sitting
    // there.
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        const bool abandon = event->key() == Qt::Key_Escape;
        if (tool_ && tool_->isBusy()) {
            ToolContext context = makeContext();
            // The tool gets first say. For most of them Escape means throw the
            // unfinished thing away, but a tool holding something the user has
            // already positioned, such as moved pixels, drops it where it
            // stands instead of snatching the move back.
            if (!tool_->keyPress(context, event)) {
                if (abandon)
                    tool_->cancel(context);
                else
                    tool_->commit(context);
            }
            event->accept();
            return;
        }
        if (doc_->selection().isActive()) {
            recordSelectionEdit(*doc_, *history_, QStringLiteral("Deselect"),
                                [this] { doc_->selection().clear(); });
            viewport()->update();
            emit documentChanged();
            event->accept();
            return;
        }
    }

    if (tool_) {
        ToolContext context = makeContext();
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
