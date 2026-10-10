#pragma once

#include <memory>

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QPoint>
#include <QTimer>

#include "core/Document.h"
#include "core/History.h"
#include "core/Units.h"
#include "tools/Tool.h"

class QSinglePointEvent;
class QTabletEvent;

namespace tessel {

// The scrolling, zooming image view. One instance per open document.
class CanvasView : public QAbstractScrollArea {
    Q_OBJECT

public:
    CanvasView(std::unique_ptr<Document> doc, QWidget* parent = nullptr);
    ~CanvasView() override;

    Document* document() const { return doc_.get(); }
    History* history() const { return history_.get(); }

    double zoom() const { return zoom_; }
    void setZoom(double zoom, QPointF anchorDoc = QPointF(-1, -1));
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToActualSize();
    void zoomToSelection();
    bool isFitMode() const { return fitMode_; }

    void setTool(Tool* tool);
    Tool* tool() const { return tool_; }
    void setPrimaryColor(Rgba color) { primary_ = color; }
    void setSecondaryColor(Rgba color) { secondary_ = color; }
    Rgba primaryColor() const { return primary_; }
    Rgba secondaryColor() const { return secondary_; }

    void setShowGrid(bool show);
    bool showGrid() const { return showGrid_; }
    void setShowSelection(bool show);
    bool showSelection() const { return showSelection_; }
    void setShowRulers(bool show);
    bool showRulers() const { return showRulers_; }
    // The unit the rulers are marked in; the document resolution turns it into
    // pixels.
    void setRulerUnit(Unit unit);
    Unit rulerUnit() const { return rulerUnit_; }

    // Builds the context that menu commands use to touch the document.
    ToolContext makeContext();
    // Moves the view by a delta in widget pixels. Uses the scroll bars where
    // the image is larger than the viewport, and a free offset where it fits,
    // so panning and zoom anchoring behave the same at every zoom level.
    void panBy(QPointF delta);
    void commitPendingTool();
    void cancelPendingTool();
    void refresh();              // the whole document changed
    void invalidateDocRect(const Rect& area);
    void notifyDocumentChanged(); // layer stack or history changed
    QPoint lastCursorPixel() const { return lastPixel_; }

signals:
    void cursorMoved(QPoint pixel, bool inside);
    void zoomChanged(double zoom);
    void documentChanged();
    void statusMessage(const QString& text);
    void colorPicked(Rgba color, bool primary);
    void toolRequested(const QString& toolId);
    void modifiedChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    // Tablet events arrive addressed to the viewport, and the default
    // implementation hands those straight to the viewport widget, so they have
    // to be picked out here to reach tabletEvent() at all.
    bool viewportEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    QPointF docFromView(QPointF viewPos) const;
    QPointF viewFromDoc(QPointF docPos) const;
    QPointF canvasOrigin() const;
    QSizeF scaledCanvasSize() const;
    void updateScrollBars();
    void clampPanOffset();
    // Mouse and tablet events share everything but the pressure, so they are
    // built and then delivered through the same three steps.
    ToolEvent buildEvent(const QSinglePointEvent* event) const;
    void deliverPress(const ToolEvent& event);
    void deliverMove(const ToolEvent& event);
    void deliverRelease(const ToolEvent& event);
    void drawCanvas(QPainter& painter);
    void drawSelection(QPainter& painter);
    void drawPixelGrid(QPainter& painter);
    void drawRulers(QPainter& painter);
    void applyCursor();

    std::unique_ptr<Document> doc_;
    std::unique_ptr<History> history_;
    Tool* tool_ = nullptr;
    double zoom_ = 1.0;
    bool fitMode_ = false;
    bool showGrid_ = false;
    bool showSelection_ = true;
    bool showRulers_ = false;
    Unit rulerUnit_ = Unit::Pixels;
    bool panning_ = false;
    bool spaceHeld_ = false;
    QPoint panAnchor_;
    QPointF panOffset_;
    QPoint lastPixel_;
    // A pen's double tap, which Qt no longer turns into a double click once
    // the tablet events are accepted.
    QElapsedTimer tapTimer_;
    QPointF lastTap_;
    QTimer antsTimer_;
    int antsPhase_ = 0;
    Rgba primary_{0, 0, 0, 255};
    Rgba secondary_{255, 255, 255, 255};
};

} // namespace tessel
