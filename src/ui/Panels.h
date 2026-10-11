// The floating panels: tools, colours, layers and history.
//
// Qt's own docking puts a panel in a strip down one side of the window and
// gives that strip the full height whatever the panel has in it, which left a
// column of dead space under a palette of sixteen buttons. These sit in the
// corners of the image window instead, the way Paint.NET's do: each one takes
// only the room its contents need, the canvas runs underneath, and dragging
// one by its title bar drops it into whichever corner the pointer is nearest.
#pragma once

#include <QPoint>
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
    void setTitle(const QString& title);
    void refreshIcons();
    bool isBeingDragged() const { return dragging_; }

    // The size it would like: whatever is in it, with the title bar on top.
    QSize wantedSize() const;

signals:
    void closeRequested();
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
    QToolButton* closeButton_ = nullptr;
    QWidget* content_ = nullptr;
    Corner corner_ = Corner::TopLeft;
    bool dragging_ = false;
    QPoint grab_;
};

// The four targets shown while a panel is being dragged, with the one it
// would land in picked out.
class PanelDropHint : public QWidget {
    Q_OBJECT

public:
    explicit PanelDropHint(QWidget* parent);

    void setActive(Corner corner);
    Corner active() const { return active_; }

    // Which corner of this rectangle a point belongs to.
    static Corner cornerAt(const QRect& area, const QPoint& point);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QRect targetFor(Corner corner) const;

    Corner active_ = Corner::TopLeft;
};

} // namespace tessel
