// Every icon is a small original line drawing produced with QPainter in a
// 24 by 24 coordinate space. Nothing is loaded from disk.
#include "ui/Icons.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>

#include "ui/Theme.h"

namespace tessel::icons {

namespace {

void dashedRect(QPainter& p, const QRectF& rect, const QColor& color)
{
    QPen pen(color, 1.6);
    pen.setDashPattern({3, 2});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect);
}

void arrowHead(QPainter& p, QPointF tip, QPointF from, double size,
               const QColor& color)
{
    const QPointF direction = tip - from;
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1.0e-6)
        return;
    const QPointF unit = direction / length;
    const QPointF normal(-unit.y(), unit.x());
    QPolygonF head;
    head << tip << tip - unit * size + normal * size * 0.55
         << tip - unit * size - normal * size * 0.55;
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawPolygon(head);
}

// One sheet of a layer stack, drawn flat so it stays legible when small.
QRectF layerSheet(double y, double halfWidth = 7.5, double height = 4.5)
{
    return QRectF(12 - halfWidth, y - height / 2.0, halfWidth * 2.0, height);
}

void plus(QPainter& p, QPointF at, double size, const QColor& color)
{
    p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(at.x() - size, at.y()), QPointF(at.x() + size, at.y()));
    p.drawLine(QPointF(at.x(), at.y() - size), QPointF(at.x(), at.y() + size));
}

void star(QPainter& p, QPointF at, double radius, const QColor& color)
{
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < 4; ++i) {
        const double angle = i * M_PI / 4.0;
        const double scale = (i % 2 == 0) ? 1.0 : 0.62;
        const QPointF offset(std::cos(angle) * radius * scale,
                             std::sin(angle) * radius * scale);
        p.drawLine(at - offset, at + offset);
    }
}

void drawGlyph(QPainter& p, const QString& name, const QColor& color)
{
    QPen stroke(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(stroke);
    p.setBrush(Qt::NoBrush);

    // ---------------------------------------------------------------- tools
    if (name == QLatin1String("pencil") || name == QLatin1String("edit")) {
        p.drawLine(QPointF(6, 18), QPointF(16, 8));
        p.drawLine(QPointF(17.2, 6.8), QPointF(19, 8.6));
        p.drawLine(QPointF(16, 8), QPointF(17.2, 6.8));
        p.drawLine(QPointF(19, 8.6), QPointF(17.6, 10));
        QPolygonF tip;
        tip << QPointF(4.4, 19.6) << QPointF(6.6, 18.9) << QPointF(5.1, 17.4);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPolygon(tip);
    } else if (name == QLatin1String("brush")) {
        QPolygonF handle;
        handle << QPointF(16.5, 4.5) << QPointF(20, 8) << QPointF(12, 15)
               << QPointF(9.5, 12.5);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPolygon(handle);
        p.setPen(stroke);
        QPainterPath bristles;
        bristles.moveTo(9.5, 13.5);
        bristles.cubicTo(7.5, 16.5, 8.5, 18.5, 5, 20);
        bristles.cubicTo(6.5, 16.5, 5.5, 15.5, 8.5, 12.5);
        p.drawPath(bristles);
    } else if (name == QLatin1String("eraser")) {
        QPolygonF body;
        body << QPointF(5, 15) << QPointF(13, 6) << QPointF(19.5, 11)
             << QPointF(11.5, 19.5);
        p.drawPolygon(body);
        p.drawLine(QPointF(8.5, 18.6), QPointF(16.2, 9.4));
        p.drawLine(QPointF(4, 20.6), QPointF(20, 20.6));
    } else if (name == QLatin1String("bucket")) {
        QPolygonF can;
        can << QPointF(5, 9) << QPointF(14, 5) << QPointF(19, 13)
            << QPointF(10, 18);
        p.drawPolygon(can);
        p.drawLine(QPointF(5.6, 9.8), QPointF(11.5, 7));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        QPainterPath drop;
        drop.moveTo(19.5, 14);
        drop.cubicTo(22, 17, 21.5, 20, 19.3, 20);
        drop.cubicTo(17.2, 20, 17, 17, 19.5, 14);
        p.drawPath(drop);
    } else if (name == QLatin1String("gradient")) {
        QLinearGradient gradient(4, 0, 20, 0);
        gradient.setColorAt(0.0, color);
        QColor fade = color;
        fade.setAlpha(25);
        gradient.setColorAt(1.0, fade);
        p.setPen(QPen(color, 1.4));
        p.setBrush(gradient);
        p.drawRect(QRectF(4, 6, 16, 12));
    } else if (name == QLatin1String("line")) {
        p.drawLine(QPointF(5.5, 18.5), QPointF(18.5, 5.5));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(5.5, 18.5), 2.2, 2.2);
        p.drawEllipse(QPointF(18.5, 5.5), 2.2, 2.2);
    } else if (name == QLatin1String("shapes")) {
        p.drawRect(QRectF(4, 8, 10, 10));
        p.drawEllipse(QRectF(10, 4, 10, 10));
    } else if (name == QLatin1String("text")) {
        p.drawLine(QPointF(5, 6), QPointF(19, 6));
        p.drawLine(QPointF(12, 6), QPointF(12, 19));
        p.drawLine(QPointF(8.5, 19), QPointF(15.5, 19));
    } else if (name == QLatin1String("clone")) {
        p.drawRect(QRectF(4.5, 11, 8, 8));
        p.drawEllipse(QRectF(11, 4.5, 8.5, 8.5));
        p.drawLine(QPointF(10, 12), QPointF(13.5, 9));
    } else if (name == QLatin1String("recolor")) {
        p.drawEllipse(QRectF(5, 5, 14, 14));
        QPainterPath half;
        half.moveTo(12, 5);
        half.arcTo(QRectF(5, 5, 14, 14), 90, -180);
        half.closeSubpath();
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPath(half);
    } else if (name == QLatin1String("picker")) {
        p.drawLine(QPointF(14, 7), QPointF(19, 12));
        QPolygonF cup;
        cup << QPointF(13.2, 7.8) << QPointF(18.2, 12.8) << QPointF(9, 20)
            << QPointF(5.5, 16.5);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPolygon(cup);
        p.setPen(stroke);
        p.drawLine(QPointF(16, 4.5), QPointF(20.5, 9));
    } else if (name == QLatin1String("zoom") || name == QLatin1String("zoom-in")
               || name == QLatin1String("zoom-out")) {
        p.drawEllipse(QRectF(4.5, 4.5, 12, 12));
        p.drawLine(QPointF(15.5, 15.5), QPointF(20, 20));
        if (name == QLatin1String("zoom-in"))
            plus(p, QPointF(10.5, 10.5), 3.2, color);
        else if (name == QLatin1String("zoom-out"))
            p.drawLine(QPointF(7.3, 10.5), QPointF(13.7, 10.5));
    } else if (name == QLatin1String("pan")) {
        p.drawLine(QPointF(12, 5), QPointF(12, 19));
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
        arrowHead(p, QPointF(12, 4), QPointF(12, 8), 3.4, color);
        arrowHead(p, QPointF(12, 20), QPointF(12, 16), 3.4, color);
        arrowHead(p, QPointF(4, 12), QPointF(8, 12), 3.4, color);
        arrowHead(p, QPointF(20, 12), QPointF(16, 12), 3.4, color);
    } else if (name == QLatin1String("select-rect") || name == QLatin1String("select")) {
        dashedRect(p, QRectF(4.5, 6.5, 15, 11), color);
    } else if (name == QLatin1String("select-ellipse")) {
        QPen pen(color, 1.6);
        pen.setDashPattern({3, 2});
        p.setPen(pen);
        p.drawEllipse(QRectF(4.5, 6, 15, 12));
    } else if (name == QLatin1String("lasso")) {
        QPen pen(color, 1.6);
        pen.setDashPattern({3, 2});
        p.setPen(pen);
        QPainterPath loop;
        loop.moveTo(9, 19);
        loop.cubicTo(2, 15, 5, 4, 13, 5);
        loop.cubicTo(21, 6, 21, 16, 12, 16);
        p.drawPath(loop);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(9, 19), QPointF(8, 21.5));
    } else if (name == QLatin1String("wand")) {
        p.drawLine(QPointF(5, 20), QPointF(15, 10));
        star(p, QPointF(17, 7), 4.5, color);
        star(p, QPointF(20, 14), 2.4, color);
    } else if (name == QLatin1String("move-selected")) {
        dashedRect(p, QRectF(4.5, 4.5, 11, 11), color);
        p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(12, 19), QPointF(20, 19));
        p.drawLine(QPointF(19, 11), QPointF(19, 19));
        arrowHead(p, QPointF(21, 19), QPointF(17, 19), 3.2, color);
    } else if (name == QLatin1String("move-selection")) {
        dashedRect(p, QRectF(4.5, 4.5, 11, 11), color);
        p.setPen(QPen(color, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(9.5, 9.5, 11, 11));
    }
    // ---------------------------------------------------------------- files
    else if (name == QLatin1String("new")) {
        QPolygonF page;
        page << QPointF(6, 3.5) << QPointF(14, 3.5) << QPointF(18, 7.5)
             << QPointF(18, 20.5) << QPointF(6, 20.5);
        p.drawPolygon(page);
        p.drawLine(QPointF(14, 3.5), QPointF(14, 7.5));
        p.drawLine(QPointF(14, 7.5), QPointF(18, 7.5));
    } else if (name == QLatin1String("open")) {
        QPolygonF folder;
        folder << QPointF(3.5, 18.5) << QPointF(3.5, 6.5) << QPointF(9.5, 6.5)
               << QPointF(11.5, 9) << QPointF(18.5, 9) << QPointF(18.5, 18.5);
        p.drawPolygon(folder);
        p.drawLine(QPointF(3.5, 12), QPointF(18.5, 12));
    } else if (name == QLatin1String("save")) {
        p.drawRoundedRect(QRectF(4.5, 4.5, 15, 15), 2, 2);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(8, 4.5, 8, 5.5));
        p.setPen(stroke);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(7.5, 13, 9, 6.5));
    } else if (name == QLatin1String("undo") || name == QLatin1String("redo")) {
        const bool forward = name == QLatin1String("redo");
        QPainterPath arc;
        arc.moveTo(forward ? 6 : 18, 16);
        arc.cubicTo(forward ? 8 : 16, 7, forward ? 16 : 8, 7, forward ? 18 : 6, 12);
        p.drawPath(arc);
        const QPointF tip(forward ? 18 : 6, 12);
        arrowHead(p, tip, QPointF(forward ? 15 : 9, 8.5), 3.6, color);
    } else if (name == QLatin1String("cut")) {
        p.drawLine(QPointF(7, 4), QPointF(15, 15));
        p.drawLine(QPointF(17, 4), QPointF(9, 15));
        p.drawEllipse(QRectF(5, 15, 5, 5));
        p.drawEllipse(QRectF(14, 15, 5, 5));
    } else if (name == QLatin1String("copy")) {
        p.drawRect(QRectF(4.5, 4.5, 11, 11));
        p.drawRect(QRectF(8.5, 8.5, 11, 11));
    } else if (name == QLatin1String("paste")) {
        p.drawRoundedRect(QRectF(5, 5, 14, 15), 2, 2);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(9, 3, 6, 4), 1.4, 1.4);
        p.setPen(stroke);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(8, 11), QPointF(16, 11));
        p.drawLine(QPointF(8, 14.5), QPointF(16, 14.5));
    } else if (name == QLatin1String("delete")) {
        p.drawLine(QPointF(4, 7), QPointF(20, 7));
        QPolygonF can;
        can << QPointF(6, 7) << QPointF(18, 7) << QPointF(16.5, 20)
            << QPointF(7.5, 20);
        p.drawPolygon(can);
        p.drawLine(QPointF(10, 4.5), QPointF(14, 4.5));
        p.drawLine(QPointF(10.5, 10.5), QPointF(10.5, 17));
        p.drawLine(QPointF(13.5, 10.5), QPointF(13.5, 17));
    }
    // ---------------------------------------------------------------- image
    else if (name == QLatin1String("crop")) {
        p.drawLine(QPointF(7, 3.5), QPointF(7, 17));
        p.drawLine(QPointF(3.5, 7), QPointF(17, 7));
        p.drawLine(QPointF(17, 7), QPointF(17, 20.5));
        p.drawLine(QPointF(7, 17), QPointF(20.5, 17));
    } else if (name == QLatin1String("deselect")) {
        dashedRect(p, QRectF(4.5, 6.5, 15, 11), color);
        p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(5.5, 19.5), QPointF(18.5, 4.5));
    } else if (name == QLatin1String("select-all")) {
        QColor fill = color;
        fill.setAlpha(45);
        p.setBrush(fill);
        dashedRect(p, QRectF(4.5, 6.5, 15, 11), color);
        p.setBrush(fill);
        p.drawRect(QRectF(4.5, 6.5, 15, 11));
    } else if (name == QLatin1String("invert")) {
        QColor fill = color;
        fill.setAlpha(45);
        dashedRect(p, QRectF(4.5, 6.5, 15, 11), color);
        p.setBrush(fill);
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(4.5, 6.5, 7.5, 11));
    } else if (name == QLatin1String("fit")) {
        p.drawRect(QRectF(4.5, 6.5, 15, 11));
        p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(7.5, 9.5), QPointF(11, 12));
        p.drawLine(QPointF(16.5, 14.5), QPointF(13, 12));
        arrowHead(p, QPointF(7, 9), QPointF(10, 11.5), 2.6, color);
        arrowHead(p, QPointF(17, 15), QPointF(14, 12.5), 2.6, color);
    } else if (name == QLatin1String("actual")) {
        p.drawRect(QRectF(4.5, 6.5, 15, 11));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(10, 10, 4, 4));
    } else if (name == QLatin1String("resize")) {
        p.drawRect(QRectF(4.5, 4.5, 11, 11));
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(13, 13), QPointF(20, 20));
        arrowHead(p, QPointF(20.5, 20.5), QPointF(17, 17), 3.4, color);
    } else if (name == QLatin1String("canvas")) {
        dashedRect(p, QRectF(3.5, 3.5, 17, 17), color);
        p.setPen(stroke);
        p.drawRect(QRectF(7.5, 7.5, 9, 9));
    } else if (name == QLatin1String("rotate-cw") || name == QLatin1String("rotate-ccw")) {
        const bool clockwise = name == QLatin1String("rotate-cw");
        QRectF box(5, 5, 14, 14);
        p.drawArc(box, clockwise ? 60 * 16 : 120 * 16, 240 * 16);
        const QPointF tip = clockwise ? QPointF(18.5, 9.2) : QPointF(5.5, 9.2);
        arrowHead(p, tip, clockwise ? QPointF(15.5, 12) : QPointF(8.5, 12), 3.6, color);
    } else if (name == QLatin1String("flip-h") || name == QLatin1String("flip-v")) {
        const bool horizontal = name == QLatin1String("flip-h");
        QPen dash(color, 1.5);
        dash.setDashPattern({3, 2});
        p.setPen(dash);
        if (horizontal)
            p.drawLine(QPointF(12, 3), QPointF(12, 21));
        else
            p.drawLine(QPointF(3, 12), QPointF(21, 12));
        p.setPen(stroke);
        p.setBrush(color);
        QPolygonF left, right;
        if (horizontal) {
            left << QPointF(10, 6) << QPointF(10, 18) << QPointF(4, 12);
            right << QPointF(14, 6) << QPointF(14, 18) << QPointF(20, 12);
        } else {
            left << QPointF(6, 10) << QPointF(18, 10) << QPointF(12, 4);
            right << QPointF(6, 14) << QPointF(18, 14) << QPointF(12, 20);
        }
        p.drawPolygon(left);
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(right);
    } else if (name == QLatin1String("image")) {
        p.drawRect(QRectF(3.5, 5.5, 17, 13));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(8, 9.5), 1.8, 1.8);
        QPolygonF hill;
        hill << QPointF(5, 18) << QPointF(11, 11) << QPointF(15, 18);
        p.drawPolygon(hill);
        QPolygonF hill2;
        hill2 << QPointF(12, 18) << QPointF(16, 13.5) << QPointF(19.5, 18);
        p.drawPolygon(hill2);
    }
    // ---------------------------------------------------------------- layers
    else if (name == QLatin1String("layer")) {
        p.drawRect(layerSheet(12, 8, 6));
    } else if (name == QLatin1String("layer-add")) {
        p.drawRect(layerSheet(17));
        p.drawRect(layerSheet(11));
        plus(p, QPointF(12, 5), 3.4, color);
    } else if (name == QLatin1String("layer-delete")) {
        p.drawRect(layerSheet(17));
        p.drawRect(layerSheet(11));
        p.setPen(QPen(color, 1.9, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(9.5, 2.5), QPointF(14.5, 7.5));
        p.drawLine(QPointF(14.5, 2.5), QPointF(9.5, 7.5));
    } else if (name == QLatin1String("layer-duplicate")) {
        p.drawRect(layerSheet(18));
        p.drawRect(layerSheet(12));
        p.drawRect(layerSheet(6));
    } else if (name == QLatin1String("layer-merge")) {
        p.drawRect(layerSheet(6));
        p.drawRect(layerSheet(18));
        p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(12, 9.5), QPointF(12, 13));
        arrowHead(p, QPointF(12, 14.6), QPointF(12, 10), 3.0, color);
    } else if (name == QLatin1String("layer-props")) {
        p.drawRect(layerSheet(7, 7.5, 5));
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4.5, 14.5), QPointF(19.5, 14.5));
        p.drawLine(QPointF(4.5, 19.5), QPointF(19.5, 19.5));
        p.setBrush(theme::panelColor());
        p.setPen(QPen(color, 1.4));
        p.drawEllipse(QPointF(9, 14.5), 2.3, 2.3);
        p.drawEllipse(QPointF(15, 19.5), 2.3, 2.3);
    } else if (name == QLatin1String("flatten")) {
        p.drawRect(layerSheet(7));
        p.drawRect(layerSheet(13));
        p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4, 19.5), QPointF(20, 19.5));
    } else if (name == QLatin1String("visible")) {
        QPainterPath eye;
        eye.moveTo(3.5, 12);
        eye.cubicTo(7, 6.5, 17, 6.5, 20.5, 12);
        eye.cubicTo(17, 17.5, 7, 17.5, 3.5, 12);
        p.drawPath(eye);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 12), 2.6, 2.6);
    } else if (name == QLatin1String("hidden")) {
        QColor faded = color;
        faded.setAlpha(110);
        p.setPen(QPen(faded, 1.6));
        QPainterPath eye;
        eye.moveTo(3.5, 12);
        eye.cubicTo(7, 6.5, 17, 6.5, 20.5, 12);
        eye.cubicTo(17, 17.5, 7, 17.5, 3.5, 12);
        p.drawPath(eye);
        p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(5, 19), QPointF(19, 5));
    } else if (name == QLatin1String("up") || name == QLatin1String("down")) {
        const bool up = name == QLatin1String("up");
        p.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (up) {
            p.drawLine(QPointF(6, 14), QPointF(12, 8));
            p.drawLine(QPointF(12, 8), QPointF(18, 14));
        } else {
            p.drawLine(QPointF(6, 10), QPointF(12, 16));
            p.drawLine(QPointF(12, 16), QPointF(18, 10));
        }
    }
    // ---------------------------------------------------------------- misc
    else if (name == QLatin1String("effect")) {
        star(p, QPointF(10, 10), 6.0, color);
        star(p, QPointF(18, 17), 3.2, color);
    } else if (name == QLatin1String("adjust")) {
        p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4, 8), QPointF(20, 8));
        p.drawLine(QPointF(4, 16), QPointF(20, 16));
        p.setBrush(theme::panelColor());
        p.drawEllipse(QPointF(9, 8), 2.8, 2.8);
        p.drawEllipse(QPointF(16, 16), 2.8, 2.8);
    } else if (name == QLatin1String("grid")) {
        p.setPen(QPen(color, 1.4));
        for (int i = 0; i < 3; ++i) {
            const double at = 7.5 + i * 4.5;
            p.drawLine(QPointF(at, 4), QPointF(at, 20));
            p.drawLine(QPointF(4, at), QPointF(20, at));
        }
    } else if (name == QLatin1String("about")) {
        p.drawEllipse(QRectF(4.5, 4.5, 15, 15));
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(12, 8.5), 1.5, 1.5);
        p.drawRoundedRect(QRectF(10.8, 11, 2.4, 7), 1.2, 1.2);
    } else if (name == QLatin1String("settings")) {
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 3; ++i) {
            const double y = 7.0 + i * 5.0;
            p.drawLine(QPointF(4, y), QPointF(20, y));
        }
        p.setBrush(theme::panelColor());
        p.drawEllipse(QPointF(8, 7), 2.4, 2.4);
        p.drawEllipse(QPointF(15, 12), 2.4, 2.4);
        p.drawEllipse(QPointF(10, 17), 2.4, 2.4);
    } else if (name == QLatin1String("close")) {
        p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(7, 7), QPointF(17, 17));
        p.drawLine(QPointF(17, 7), QPointF(7, 17));
    } else if (name == QLatin1String("palette")) {
        QPainterPath blob;
        blob.addEllipse(QRectF(3.5, 3.5, 17, 17));
        p.setPen(QPen(color, 1.6));
        p.drawPath(blob);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(9, 8.5), 1.9, 1.9);
        p.drawEllipse(QPointF(15.5, 9.5), 1.9, 1.9);
        p.drawEllipse(QPointF(8, 15), 1.9, 1.9);
        p.drawEllipse(QPointF(14.5, 15.5), 1.9, 1.9);
    } else {
        // Unknown name: a neutral placeholder beats an empty button.
        p.drawRoundedRect(QRectF(5.5, 5.5, 13, 13), 2, 2);
    }
}

QHash<QString, QPixmap>& cache()
{
    static QHash<QString, QPixmap> store;
    return store;
}

} // namespace

QPixmap pixmap(const QString& name, int size, const QColor& color)
{
    const QString key = QStringLiteral("%1/%2/%3").arg(name).arg(size).arg(color.name(QColor::HexArgb));
    auto it = cache().find(key);
    if (it != cache().end())
        return it.value();

    QPixmap canvas(size, size);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(size / 24.0, size / 24.0);
    drawGlyph(painter, name, color);
    painter.end();
    cache().insert(key, canvas);
    return canvas;
}

QIcon get(const QString& name, int size)
{
    QIcon icon;
    const QColor normal = theme::textColor();
    QColor disabled = normal;
    disabled.setAlpha(96);
    icon.addPixmap(pixmap(name, size, normal), QIcon::Normal);
    icon.addPixmap(pixmap(name, size * 2, normal), QIcon::Normal);
    icon.addPixmap(pixmap(name, size, disabled), QIcon::Disabled);
    return icon;
}

QIcon appIcon()
{
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap canvas(size, size);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.scale(size / 24.0, size / 24.0);
        // A paint blob with four wells, drawn fresh at each size.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x3d, 0x8b, 0xfd));
        painter.drawRoundedRect(QRectF(2, 2, 20, 20), 5, 5);
        painter.setBrush(QColor(255, 255, 255, 235));
        painter.drawEllipse(QPointF(8.6, 8.6), 2.4, 2.4);
        painter.setBrush(QColor(0xff, 0x6b, 0x4a));
        painter.drawEllipse(QPointF(15.4, 8.6), 2.4, 2.4);
        painter.setBrush(QColor(0xff, 0xc9, 0x3c));
        painter.drawEllipse(QPointF(8.6, 15.4), 2.4, 2.4);
        painter.setBrush(QColor(0x2f, 0xd1, 0x8b));
        painter.drawEllipse(QPointF(15.4, 15.4), 2.4, 2.4);
        painter.end();
        icon.addPixmap(canvas);
    }
    return icon;
}

QPixmap colorSwatch(Rgba color, int size, bool selected)
{
    QPixmap canvas(size, size);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box(0.5, 0.5, size - 1.0, size - 1.0);
    if (color.a < 255) {
        painter.setBrush(QBrush(checkerPixmap(std::max(3, size / 4))));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(box, 2, 2);
    }
    painter.setBrush(color.toQColor());
    painter.setPen(QPen(selected ? theme::accentColor() : theme::gridColor(),
                        selected ? 2.0 : 1.0));
    painter.drawRoundedRect(box, 2, 2);
    painter.end();
    return canvas;
}

QPixmap checkerPixmap(int cell)
{
    cell = std::max(2, cell);
    const QString key = QStringLiteral("checker/%1/%2").arg(cell).arg(
        theme::checkerLight().name());
    auto it = cache().find(key);
    if (it != cache().end())
        return it.value();
    QPixmap canvas(cell * 2, cell * 2);
    QPainter painter(&canvas);
    painter.fillRect(0, 0, cell * 2, cell * 2, theme::checkerLight());
    painter.fillRect(0, 0, cell, cell, theme::checkerDark());
    painter.fillRect(cell, cell, cell, cell, theme::checkerDark());
    painter.end();
    cache().insert(key, canvas);
    return canvas;
}

void clearCache() { cache().clear(); }

} // namespace tessel::icons
