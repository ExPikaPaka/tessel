// The floating panels: tools, colours, layers and history.
//
// Qt's own docking puts a panel in a strip down one side of the window and
// gives that strip the full height whatever the panel has in it, which left a
// column of dead space under a palette of sixteen buttons. These sit in the
// corners of the image window instead, the way Paint.NET's do: each one takes
// only the room its contents need, the canvas runs underneath, and dragging
// one by its title bar drops it into whichever corner the pointer is nearest.
#pragma once

#include <optional>

#include <QPoint>
#include <QPointF>
#include <QWidget>

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace tessel {

enum class Corner { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

class Panel : public QWidget {
    Q_OBJECT

public:
    Panel(const QString& name, const QString& title, QWidget* content, QWidget* parent);

    QWidget* content() const { return content_; }
    Corner corner() const { return corner_; }
    void setCorner(Corner corner) { corner_ = corner; }
    // A floating panel stands wherever it was let go of rather than in a
    // corner. Its place is kept as a fraction of the workspace, so it is in
    // the same part of the window after the window is resized.
    bool isFloating() const { return floating_; }
    void setFloating(bool floating);
    QPointF anchor() const { return anchor_; }
    void setAnchor(const QPointF& anchor) { anchor_ = anchor; }
    void setTitle(const QString& title);
    void refreshIcons();
    bool isBeingDragged() const { return dragging_; }

    // The size it would like: whatever is in it, with the title bar on top.
    QSize wantedSize() const;

signals:
    void closeRequested();
    // The pin in the title bar: pinned back into a corner, or let go to
    // float where it stands.
    void floatToggled();
    // While the title bar is held, then once when it is let go. The point is
    // the pointer in the coordinates of whatever the panel sits in.
    void dragging(const QPoint& pointer);
    void dropped(const QPoint& pointer);
    // The contents have changed shape, or the panel was shown or hidden.
    void sizeChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool event(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QPoint pointerInParent(const QPoint& inBar) const;

    QVBoxLayout* column_ = nullptr;
    QWidget* bar_ = nullptr;
    QLabel* label_ = nullptr;
    QToolButton* pinButton_ = nullptr;
    QToolButton* closeButton_ = nullptr;
    QWidget* content_ = nullptr;
    Corner corner_ = Corner::TopLeft;
    bool floating_ = false;
    QPointF anchor_{0.25, 0.25};
    bool dragging_ = false;
    QPoint grab_;
};

// The four targets shown while a panel is being dragged, with the one it
// would land in picked out.
class PanelDropHint : public QWidget {
    Q_OBJECT

public:
    explicit PanelDropHint(QWidget* parent);

    // The corner a point would drop into, or nothing when it is not over one
    // of the targets, which leaves the panel floating where it was let go.
    std::optional<Corner> targetAt(const QPoint& point) const;
    void setActive(const std::optional<Corner>& corner);
    std::optional<Corner> active() const { return active_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QRect targetFor(Corner corner) const;

    std::optional<Corner> active_;
};

} // namespace tessel
