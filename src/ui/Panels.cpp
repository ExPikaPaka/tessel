#include "ui/Panels.h"

#include <algorithm>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tessel {

namespace {
constexpr int kTargetWidth = 150;
constexpr int kTargetHeight = 96;
constexpr int kTargetMargin = 14;
} // namespace

// ----------------------------------------------------------------- panel

Panel::Panel(const QString& name, const QString& title, QWidget* content,
             QWidget* parent)
    : QWidget(parent), content_(content)
{
    setObjectName(name);
    // The canvas runs under the panels, so every part of one has to paint
    // itself or the image shows through and the text stops being readable.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);

    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(1, 1, 1, 1); // room for the frame line
    column_->setSpacing(0);

    bar_ = new QWidget(this);
    bar_->setObjectName(QStringLiteral("panelTitleBar"));
    bar_->setAttribute(Qt::WA_StyledBackground, true);
    bar_->installEventFilter(this);
    auto* row = new QHBoxLayout(bar_);
    row->setContentsMargins(8, 3, 3, 3);
    row->setSpacing(2);

    label_ = new QLabel(title, bar_);
    label_->setObjectName(QStringLiteral("panelTitle"));
    // The label takes no mouse events, so the whole bar is one handle.
    label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    row->addWidget(label_, 1);

    closeButton_ = new QToolButton(bar_);
    closeButton_->setObjectName(QStringLiteral("panelCloseButton"));
    closeButton_->setAutoRaise(true);
    closeButton_->setFocusPolicy(Qt::NoFocus);
    closeButton_->setIconSize(QSize(16, 16));
    closeButton_->setFixedSize(22, 22);
    closeButton_->setToolTip(tr("Close this panel"));
    connect(closeButton_, &QToolButton::clicked, this, &Panel::closeRequested);
    row->addWidget(closeButton_, 0);

    column_->addWidget(bar_, 0);
    if (content_) {
        content_->setParent(this);
        content_->setAttribute(Qt::WA_StyledBackground, true);
        content_->setAutoFillBackground(true);
        column_->addWidget(content_, 1);
    }
    refreshIcons();
}

void Panel::setTitle(const QString& title)
{
    if (label_)
        label_->setText(title);
}

void Panel::refreshIcons()
{
    if (closeButton_)
        closeButton_->setIcon(icons::get(QStringLiteral("close"), 16));
}

QSize Panel::wantedSize() const
{
    const QSize inside = content_ ? content_->sizeHint() : QSize(120, 80);
    const int bar = bar_ ? bar_->sizeHint().height() : 24;
    return QSize(std::max(120, inside.width()), bar + std::max(40, inside.height()));
}

QPoint Panel::pointerInParent(const QPoint& inBar) const
{
    const QPoint inPanel = bar_ ? bar_->mapToParent(inBar) : inBar;
    return mapToParent(inPanel);
}

bool Panel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != bar_)
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton)
            break;
        dragging_ = true;
        raise();
        grab_ = pointerInParent(mouse->pos()) - pos();
        emit dragging(pointerInParent(mouse->pos()));
        return true;
    }
    case QEvent::MouseMove: {
        if (!dragging_)
            break;
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint pointer = pointerInParent(mouse->pos());
        move(pointer - grab_);
        emit dragging(pointer);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (!dragging_)
            break;
        auto* mouse = static_cast<QMouseEvent*>(event);
        dragging_ = false;
        emit dropped(pointerInParent(mouse->pos()));
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

bool Panel::event(QEvent* event)
{
    // What is inside has changed shape: a layer added, a step of history
    // recorded. The panel is measured again and put back in its corner.
    if (event->type() == QEvent::LayoutRequest)
        emit sizeChanged();
    return QWidget::event(event);
}

void Panel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    emit sizeChanged();
}

void Panel::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    emit sizeChanged();
}

// ----------------------------------------------------------------- drop hint

PanelDropHint::PanelDropHint(QWidget* parent) : QWidget(parent)
{
    // It is drawn over the canvas and never answers the pointer: the panel
    // being dragged is still the one under it.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    hide();
}

void PanelDropHint::setActive(Corner corner)
{
    if (active_ == corner && isVisible())
        return;
    active_ = corner;
    update();
}

Corner PanelDropHint::cornerAt(const QRect& area, const QPoint& point)
{
    const bool right = point.x() > area.center().x();
    const bool bottom = point.y() > area.center().y();
    if (bottom)
        return right ? Corner::BottomRight : Corner::BottomLeft;
    return right ? Corner::TopRight : Corner::TopLeft;
}

QRect PanelDropHint::targetFor(Corner corner) const
{
    const int w = std::min(kTargetWidth, std::max(40, width() / 3));
    const int h = std::min(kTargetHeight, std::max(30, height() / 3));
    const int left = kTargetMargin;
    const int top = kTargetMargin;
    const int right = width() - kTargetMargin - w;
    const int bottom = height() - kTargetMargin - h;
    switch (corner) {
    case Corner::TopLeft:
        return QRect(left, top, w, h);
    case Corner::TopRight:
        return QRect(right, top, w, h);
    case Corner::BottomLeft:
        return QRect(left, bottom, w, h);
    case Corner::BottomRight:
        break;
    }
    return QRect(right, bottom, w, h);
}

void PanelDropHint::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const Corner corner : {Corner::TopLeft, Corner::TopRight, Corner::BottomLeft,
                                Corner::BottomRight}) {
        const QRect target = targetFor(corner);
        const bool chosen = corner == active_;
        QColor fill = chosen ? theme::accentColor() : theme::panelColor();
        fill.setAlpha(chosen ? 150 : 90);
        QColor line = chosen ? theme::accentColor() : theme::gridColor();
        line.setAlpha(chosen ? 255 : 170);
        painter.setBrush(fill);
        painter.setPen(QPen(line, chosen ? 2 : 1));
        painter.drawRoundedRect(target, 6, 6);
    }
}

} // namespace tessel
