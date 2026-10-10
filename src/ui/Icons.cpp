// Every icon is a small original line drawing produced with QPainter in a
// 24 by 24 coordinate space. Nothing is loaded from disk.
#include "ui/Icons.h"

#include <cmath>

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include "ui/Theme.h"

namespace tessel::icons {

namespace {

// ---------------------------------------------------------------- the grid
//
// Every glyph is drawn in a 24 by 24 box with a 2 unit margin, so the live
// area is 20 by 20. Primary strokes are 2.0 units and secondary detail 1.5,
// always with round caps and joins, which holds the whole set at one weight.
//
// Horizontal and vertical strokes sit on whole coordinates rather than half
// ones. A 2.0 unit stroke centred on a whole number has its edges on whole
// numbers too, which is what stops it smearing across two pixel columns; half
// coordinates would be right for a 1 unit hairline, and there are none left.
constexpr double kPrimary = 2.0;
constexpr double kSecondary = 1.5;

QPen strokePen(const QColor& color, double width = kPrimary)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

// Marching ants. Dash lengths are multiples of the pen width, and round caps
// add one pen width to every dash, so this pattern paints a 5 unit dash with
// a 2.4 unit gap: coarse enough to survive a 16 px rendering.
QPen antsPen(const QColor& color)
{
    QPen pen = strokePen(color);
    pen.setDashPattern({1.5, 2.2});
    return pen;
}

void fillShape(QPainter& p, const QPainterPath& path, const QColor& color)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(path);
    p.setBrush(Qt::NoBrush);
}

// Erases a channel around a shape before that shape is drawn on top, so a
// badge or a slash stays readable over whatever it crosses. Erasing rather
// than painting a second colour keeps the glyph right on a light theme, a
// dark theme and a highlighted button alike.
void punch(QPainter& p, const QPainterPath& path, double halo, bool solid = true)
{
    const QPainter::CompositionMode previous = p.compositionMode();
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.setPen(halo > 0.0 ? QPen(Qt::black, halo, Qt::SolidLine, Qt::RoundCap,
                               Qt::RoundJoin)
                        : QPen(Qt::NoPen));
    p.setBrush(solid ? QBrush(Qt::black) : QBrush(Qt::NoBrush));
    p.drawPath(path);
    p.setBrush(Qt::NoBrush);
    p.setCompositionMode(previous);
}

QPainterPath linePath(QPointF from, QPointF to)
{
    QPainterPath path;
    path.moveTo(from);
    path.lineTo(to);
    return path;
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
    p.setBrush(Qt::NoBrush);
}

// Shaft plus solid head. The shaft stops short of the tip so the two do not
// pile up into a blob at 16 px.
void arrow(QPainter& p, QPointF from, QPointF tip, const QColor& color,
           double head = 4.4, double width = kPrimary)
{
    const QPointF direction = tip - from;
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1.0e-6)
        return;
    const QPointF unit = direction / length;
    p.setPen(strokePen(color, width));
    p.drawLine(from, tip - unit * (head * 0.8));
    arrowHead(p, tip, from, head, color);
}

void plus(QPainter& p, QPointF at, double arm, const QColor& color,
          double width = kPrimary)
{
    p.setPen(strokePen(color, width));
    p.drawLine(QPointF(at.x() - arm, at.y()), QPointF(at.x() + arm, at.y()));
    p.drawLine(QPointF(at.x(), at.y() - arm), QPointF(at.x(), at.y() + arm));
}

void cross(QPainter& p, QPointF at, double arm, const QColor& color,
           double width = kPrimary)
{
    p.setPen(strokePen(color, width));
    p.drawLine(QPointF(at.x() - arm, at.y() - arm),
               QPointF(at.x() + arm, at.y() + arm));
    p.drawLine(QPointF(at.x() + arm, at.y() - arm),
               QPointF(at.x() - arm, at.y() + arm));
}

// A solid four pointed sparkle. Filled rather than stroked, because a star
// drawn in thin lines disappears at small sizes.
QPainterPath sparklePath(QPointF at, double radius)
{
    QPainterPath path;
    const double inner = radius * 0.28;
    for (int i = 0; i < 8; ++i) {
        const double angle = i * M_PI / 4.0;
        const double r = (i % 2 == 0) ? radius : inner;
        const QPointF point(at.x() + std::cos(angle) * r,
                            at.y() + std::sin(angle) * r);
        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    path.closeSubpath();
    return path;
}

// ------------------------------------------------------------ family bases

// Selection: one marquee rectangle that every selection glyph builds on.
QRectF marqueeRect() { return QRectF(4, 6, 16, 12); }

// Files: one page with a folded corner. The corners are square because the
// round joins soften them when the path is stroked.
QPainterPath pagePath()
{
    QPainterPath path;
    path.moveTo(5, 3);
    path.lineTo(14, 3);
    path.lineTo(19, 8);
    path.lineTo(19, 21);
    path.lineTo(5, 21);
    path.closeSubpath();
    path.moveTo(14, 3);
    path.lineTo(14, 8);
    path.lineTo(19, 8);
    return path;
}

// Layers: one sheet seen in perspective. Some commands stack it, the rest
// hang a symbol above it.
QPainterPath sheetPath(double centreY, double halfWidth = 8.0,
                       double halfHeight = 3.5)
{
    QPainterPath path;
    path.moveTo(12 - halfWidth, centreY);
    path.lineTo(12, centreY - halfHeight);
    path.lineTo(12 + halfWidth, centreY);
    path.lineTo(12, centreY + halfHeight);
    path.closeSubpath();
    return path;
}

QPainterPath eyePath()
{
    QPainterPath path;
    path.moveTo(3, 12);
    path.cubicTo(7.5, 6, 16.5, 6, 21, 12);
    path.cubicTo(16.5, 18, 7.5, 18, 3, 12);
    return path;
}

// A solid gear: eight teeth on a disc with the centre punched out. An
// outlined gear fills in completely at 16 px, a solid one does not.
QPainterPath gearPath()
{
    QPainterPath path;
    path.addEllipse(QPointF(12, 12), 7.0, 7.0);
    for (int i = 0; i < 8; ++i) {
        QPainterPath tooth;
        tooth.addRoundedRect(QRectF(-2.0, -10.0, 4.0, 5.0), 1.3, 1.3);
        QTransform transform;
        transform.translate(12, 12);
        transform.rotate(i * 45.0);
        path = path.united(transform.map(tooth));
    }
    return path;
}

void drawGlyph(QPainter& p, const QString& name, const QColor& color)
{
    p.setPen(strokePen(color));
    p.setBrush(Qt::NoBrush);

    // ---------------------------------------------------------------- tools
    if (name == QLatin1String("pencil") || name == QLatin1String("edit")) {
        // Solid, slim and pointed, so it never reads as the brush.
        p.save();
        p.translate(12, 12);
        p.rotate(-45);
        QPainterPath body;
        body.addRoundedRect(QRectF(-2.6, -9.0, 5.2, 13.2), 1.4, 1.4);
        QPainterPath nib;
        nib.moveTo(-2.6, 4.0);
        nib.lineTo(2.6, 4.0);
        nib.lineTo(0, 9.4);
        nib.closeSubpath();
        fillShape(p, body.united(nib), color);
        // The ferrule, cut out rather than drawn, so it suits any theme.
        punch(p, linePath(QPointF(-2.8, -4.2), QPointF(2.8, -4.2)), 1.4, false);
        p.restore();
    } else if (name == QLatin1String("brush")) {
        // A thin handle with a fat head: the opposite silhouette to the pencil.
        p.save();
        p.translate(12, 12);
        p.rotate(-45);
        QPainterPath handle;
        handle.addRoundedRect(QRectF(-1.6, -9.6, 3.2, 10.0), 1.5, 1.5);
        fillShape(p, handle, color);
        QPainterPath head;
        head.moveTo(-4.8, -1.0);
        head.lineTo(4.8, -1.0);
        head.cubicTo(4.8, 6.0, 2.0, 7.2, 0, 9.8);
        head.cubicTo(-2.0, 7.2, -4.8, 6.0, -4.8, -1.0);
        head.closeSubpath();
        fillShape(p, head, color);
        punch(p, linePath(QPointF(-5.2, 1.2), QPointF(5.2, 1.2)), 1.4, false);
        p.restore();
    } else if (name == QLatin1String("eraser")) {
        p.save();
        p.translate(11.8, 10.5);
        p.rotate(-35);
        QPainterPath block;
        block.addRoundedRect(QRectF(-7.0, -4.2, 14.0, 8.4), 1.8, 1.8);
        fillShape(p, block, color);
        punch(p, linePath(QPointF(1.0, -4.6), QPointF(1.0, 4.6)), 1.6, false);
        p.restore();
        p.setPen(strokePen(color));
        p.drawLine(QPointF(4, 21), QPointF(20, 21));
    } else if (name == QLatin1String("bucket")) {
        p.save();
        p.translate(10.5, 12.5);
        p.rotate(30);
        p.setPen(strokePen(color, kSecondary));
        QPainterPath handle;
        handle.moveTo(-5, -5);
        handle.cubicTo(-5, -11.5, 5, -11.5, 5, -5);
        p.drawPath(handle);
        QPainterPath can;
        can.moveTo(-6, -5);
        can.lineTo(6, -5);
        can.lineTo(4.2, 6);
        can.lineTo(-4.2, 6);
        can.closeSubpath();
        fillShape(p, can, color);
        p.restore();
        QPainterPath drop;
        drop.moveTo(19.3, 13.5);
        drop.cubicTo(22.0, 17.2, 21.4, 20.5, 19.3, 20.5);
        drop.cubicTo(17.2, 20.5, 16.6, 17.2, 19.3, 13.5);
        fillShape(p, drop, color);
    } else if (name == QLatin1String("gradient")) {
        // The ramp fades the foreground colour to nothing, so it is right on a
        // light theme and a dark one without naming a second colour.
        QLinearGradient ramp(4, 0, 20, 0);
        ramp.setColorAt(0.0, color);
        QColor fade = color;
        fade.setAlpha(0);
        ramp.setColorAt(1.0, fade);
        p.setBrush(ramp);
        p.setPen(strokePen(color));
        p.drawRoundedRect(QRectF(4, 6, 16, 12), 2, 2);
        p.setBrush(Qt::NoBrush);
    } else if (name == QLatin1String("line")) {
        p.drawLine(QPointF(6, 18), QPointF(18, 6));
        QPainterPath ends;
        ends.addEllipse(QPointF(6, 18), 2.6, 2.6);
        ends.addEllipse(QPointF(18, 6), 2.6, 2.6);
        fillShape(p, ends, color);
    } else if (name == QLatin1String("shapes")) {
        p.drawRoundedRect(QRectF(4, 10, 10, 10), 1.2, 1.2);
        QPainterPath disc;
        disc.addEllipse(QPointF(15.5, 8.5), 5.5, 5.5);
        fillShape(p, disc, color);
    } else if (name == QLatin1String("text")) {
        // The one glyph where a letterform is the metaphor.
        p.setPen(strokePen(color, 2.4));
        p.drawLine(QPointF(5, 6), QPointF(19, 6));
        p.drawLine(QPointF(12, 6), QPointF(12, 19));
    } else if (name == QLatin1String("clone")) {
        QPainterPath stamp;
        stamp.moveTo(10, 3.5);
        stamp.lineTo(14, 3.5);
        stamp.lineTo(14, 9);
        stamp.lineTo(19, 14.5);
        stamp.lineTo(19, 20);
        stamp.lineTo(5, 20);
        stamp.lineTo(5, 14.5);
        stamp.lineTo(10, 9);
        stamp.closeSubpath();
        fillShape(p, stamp, color);
        punch(p, linePath(QPointF(4, 16.6), QPointF(20, 16.6)), 1.6, false);
    } else if (name == QLatin1String("recolor")) {
        p.drawEllipse(QPointF(12, 12), 8, 8);
        QPainterPath half;
        half.moveTo(12, 4);
        half.arcTo(QRectF(4, 4, 16, 16), 90, -180);
        half.closeSubpath();
        fillShape(p, half, color);
    } else if (name == QLatin1String("picker")) {
        p.save();
        p.translate(12, 12);
        p.rotate(-45);
        QPainterPath bulb;
        bulb.addRoundedRect(QRectF(-4.3, -9.6, 8.6, 6.4), 3.0, 3.0);
        QPainterPath stem;
        stem.moveTo(-1.6, -4.0);
        stem.lineTo(1.6, -4.0);
        stem.lineTo(1.6, 4.2);
        stem.lineTo(0, 9.4);
        stem.lineTo(-1.6, 4.2);
        stem.closeSubpath();
        fillShape(p, bulb.united(stem), color);
        punch(p, linePath(QPointF(-4.7, -2.8), QPointF(4.7, -2.8)), 1.4, false);
        p.restore();
    }
    // ----------------------------------------------------------------- view
    else if (name == QLatin1String("zoom") || name == QLatin1String("zoom-in")
             || name == QLatin1String("zoom-out")) {
        p.drawEllipse(QPointF(10, 10), 6.5, 6.5);
        p.drawLine(QPointF(14.8, 14.8), QPointF(20.5, 20.5));
        if (name == QLatin1String("zoom-in"))
            plus(p, QPointF(10, 10), 3.4, color);
        else if (name == QLatin1String("zoom-out"))
            p.drawLine(QPointF(6.6, 10), QPointF(13.4, 10));
    } else if (name == QLatin1String("pan")) {
        p.drawLine(QPointF(12, 7), QPointF(12, 17));
        p.drawLine(QPointF(7, 12), QPointF(17, 12));
        arrowHead(p, QPointF(12, 3.2), QPointF(12, 8), 4.4, color);
        arrowHead(p, QPointF(12, 20.8), QPointF(12, 16), 4.4, color);
        arrowHead(p, QPointF(3.2, 12), QPointF(8, 12), 4.4, color);
        arrowHead(p, QPointF(20.8, 12), QPointF(16, 12), 4.4, color);
    } else if (name == QLatin1String("fit")) {
        // The two zoom commands share a viewport frame and differ by what sits
        // inside it: an image fitted to the frame, or one pixel at its size.
        p.drawRect(QRectF(3, 5, 18, 14));
        QPainterPath inner;
        inner.addRoundedRect(QRectF(7, 9, 10, 6), 1.2, 1.2);
        fillShape(p, inner, color);
    } else if (name == QLatin1String("actual")) {
        p.drawRect(QRectF(3, 5, 18, 14));
        QPainterPath pixel;
        pixel.addRoundedRect(QRectF(9, 9, 6, 6), 1.2, 1.2);
        fillShape(p, pixel, color);
    } else if (name == QLatin1String("grid")) {
        // Four solid cells read as a pixel grid; thin rules turn into moire.
        QPainterPath cells;
        cells.addRoundedRect(QRectF(3, 3, 8, 8), 1.4, 1.4);
        cells.addRoundedRect(QRectF(13, 3, 8, 8), 1.4, 1.4);
        cells.addRoundedRect(QRectF(3, 13, 8, 8), 1.4, 1.4);
        cells.addRoundedRect(QRectF(13, 13, 8, 8), 1.4, 1.4);
        fillShape(p, cells, color);
    }
    // ------------------------------------------------------------ selection
    else if (name == QLatin1String("select") || name == QLatin1String("select-rect")) {
        p.setPen(antsPen(color));
        p.drawRect(marqueeRect());
    } else if (name == QLatin1String("select-ellipse")) {
        p.setPen(antsPen(color));
        p.drawEllipse(marqueeRect());
    } else if (name == QLatin1String("lasso")) {
        p.setPen(antsPen(color));
        QPainterPath loop;
        loop.moveTo(9.5, 18);
        loop.cubicTo(2, 14.5, 4, 4, 12.5, 4.5);
        loop.cubicTo(21, 5, 20.5, 15.5, 11.5, 15.5);
        p.drawPath(loop);
        p.setPen(strokePen(color));
        p.drawLine(QPointF(9.5, 18), QPointF(8, 21));
    } else if (name == QLatin1String("wand")) {
        p.setPen(strokePen(color, 3.0));
        p.drawLine(QPointF(5.5, 19.5), QPointF(13, 12));
        fillShape(p, sparklePath(QPointF(16.6, 7.4), 4.8), color);
        fillShape(p, sparklePath(QPointF(19.8, 14.2), 2.2), color);
    } else if (name == QLatin1String("move-selected")
               || name == QLatin1String("move-selection")) {
        // The pair differs by what travels: a filled block means the pixels
        // move, an empty one means only the outline does.
        p.setPen(antsPen(color));
        p.drawRect(QRectF(3, 3, 12, 12));
        QPainterPath moved;
        moved.addRoundedRect(QRectF(11, 11, 10, 10), 1.6, 1.6);
        punch(p, moved, 3.2);
        if (name == QLatin1String("move-selected")) {
            fillShape(p, moved, color);
        } else {
            p.setPen(strokePen(color));
            p.drawPath(moved);
        }
    } else if (name == QLatin1String("select-all")) {
        QColor fill = color;
        fill.setAlpha(70);
        p.setBrush(fill);
        p.setPen(antsPen(color));
        p.drawRect(marqueeRect());
        p.setBrush(Qt::NoBrush);
    } else if (name == QLatin1String("deselect")) {
        p.setPen(antsPen(color));
        p.drawRect(marqueeRect());
        const QPainterPath slash = linePath(QPointF(5.5, 19), QPointF(18.5, 5));
        punch(p, slash, 4.6, false);
        p.setPen(strokePen(color, 2.2));
        p.drawPath(slash);
    } else if (name == QLatin1String("invert")) {
        // What lies outside the marquee is what is selected now.
        const QRectF canvas(3, 4, 18, 16);
        const QRectF inner(7.5, 8.5, 9, 7);
        QPainterPath ring;
        ring.setFillRule(Qt::OddEvenFill);
        ring.addRect(canvas);
        ring.addRect(inner);
        QColor fill = color;
        fill.setAlpha(90);
        fillShape(p, ring, fill);
        p.setPen(strokePen(color, kSecondary));
        p.drawRect(canvas);
        p.setPen(antsPen(color));
        p.drawRect(inner);
    }
    // ---------------------------------------------------------------- files
    else if (name == QLatin1String("new") || name == QLatin1String("open")
             || name == QLatin1String("save")) {
        // One page, three badges. The badge is punched out of the page before
        // it is drawn, so the two never touch at small sizes.
        p.drawPath(pagePath());
        if (name == QLatin1String("new")) {
            QPainterPath mark;
            mark.moveTo(12 - 3.6, 14);
            mark.lineTo(12 + 3.6, 14);
            mark.moveTo(12, 14 - 3.6);
            mark.lineTo(12, 14 + 3.6);
            punch(p, mark, 5.4, false);
            plus(p, QPointF(12, 14), 3.6, color, 2.4);
        } else {
            const bool out = name == QLatin1String("open");
            const QPointF from(12, out ? 17.5 : 9.5);
            const QPointF tip(12, out ? 8.5 : 18.5);
            punch(p, linePath(from, tip), 6.0, false);
            arrow(p, from, tip, color, 5.0, 2.2);
        }
    } else if (name == QLatin1String("undo") || name == QLatin1String("redo")) {
        const double sign = name == QLatin1String("redo") ? -1.0 : 1.0;
        const auto at = [sign](double x, double y) {
            return QPointF(12 + sign * (x - 12), y);
        };
        QPainterPath tail;
        tail.moveTo(at(17.5, 20));
        tail.cubicTo(at(17.5, 11), at(12.5, 7.5), at(8.2, 7.5));
        p.drawPath(tail);
        arrowHead(p, at(3.2, 7.5), at(8.2, 7.5), 5.0, color);
    } else if (name == QLatin1String("cut")) {
        p.drawLine(QPointF(7.5, 4), QPointF(15.5, 17.6));
        p.drawLine(QPointF(16.5, 4), QPointF(8.5, 17.6));
        QPainterPath handles;
        handles.addEllipse(QPointF(8.5, 18.4), 2.7, 2.7);
        handles.addEllipse(QPointF(15.5, 18.4), 2.7, 2.7);
        punch(p, handles, 1.6);
        fillShape(p, handles, color);
    } else if (name == QLatin1String("copy")) {
        p.drawRoundedRect(QRectF(4, 4, 11, 11), 2, 2);
        QPainterPath front;
        front.addRoundedRect(QRectF(9, 9, 11, 11), 2, 2);
        punch(p, front, 4.0);
        p.setPen(strokePen(color));
        p.drawPath(front);
    } else if (name == QLatin1String("paste")) {
        p.drawRoundedRect(QRectF(5, 5, 14, 16), 2.5, 2.5);
        p.setPen(strokePen(color, kSecondary));
        p.drawLine(QPointF(8.5, 12), QPointF(15.5, 12));
        p.drawLine(QPointF(8.5, 16), QPointF(15.5, 16));
        QPainterPath clip;
        clip.addRoundedRect(QRectF(9, 2.5, 6, 5), 1.6, 1.6);
        punch(p, clip, 2.4);
        fillShape(p, clip, color);
    } else if (name == QLatin1String("delete")) {
        p.setPen(strokePen(color, 2.2));
        p.drawLine(QPointF(4, 7), QPointF(20, 7));
        p.setPen(strokePen(color));
        p.drawLine(QPointF(9.5, 4), QPointF(14.5, 4));
        QPainterPath can;
        can.moveTo(6.5, 9);
        can.lineTo(8, 20.5);
        can.lineTo(16, 20.5);
        can.lineTo(17.5, 9);
        p.drawPath(can);
    }
    // ---------------------------------------------------------------- image
    else if (name == QLatin1String("image")) {
        p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
        p.save();
        p.setClipRect(QRectF(4, 6, 16, 12));
        QPainterPath scene;
        scene.addEllipse(QPointF(8.2, 10), 2.0, 2.0);
        QPainterPath hill;
        hill.moveTo(3.5, 18.5);
        hill.lineTo(10.5, 9.5);
        hill.lineTo(17.5, 18.5);
        hill.closeSubpath();
        QPainterPath ridge;
        ridge.moveTo(12.5, 18.5);
        ridge.lineTo(16.5, 13);
        ridge.lineTo(20.5, 18.5);
        ridge.closeSubpath();
        fillShape(p, scene.united(hill).united(ridge), color);
        p.restore();
    } else if (name == QLatin1String("crop")) {
        p.drawLine(QPointF(7, 3), QPointF(7, 17));
        p.drawLine(QPointF(3, 7), QPointF(17, 7));
        p.drawLine(QPointF(17, 7), QPointF(17, 21));
        p.drawLine(QPointF(7, 17), QPointF(21, 17));
    } else if (name == QLatin1String("resize")) {
        p.drawRect(QRectF(3, 3, 11, 11));
        arrow(p, QPointF(13, 13), QPointF(20.6, 20.6), color, 4.6);
    } else if (name == QLatin1String("canvas")) {
        p.setPen(antsPen(color));
        p.drawRect(QRectF(3, 3, 18, 18));
        QPainterPath image;
        image.addRoundedRect(QRectF(7, 7, 10, 10), 1.4, 1.4);
        punch(p, image, 3.0);
        fillShape(p, image, color);
    } else if (name == QLatin1String("rotate-cw") || name == QLatin1String("rotate-ccw")) {
        const bool clockwise = name == QLatin1String("rotate-cw");
        const double start = clockwise ? 70.0 : 110.0;
        p.drawArc(QRectF(5, 5, 14, 14), int(start * 16),
                  int((clockwise ? -290.0 : 290.0) * 16));
        const double radians = start * M_PI / 180.0;
        const QPointF end(12 + 7 * std::cos(radians), 12 - 7 * std::sin(radians));
        const QPointF step((clockwise ? 1.0 : -1.0) * std::sin(radians),
                           std::cos(radians));
        arrowHead(p, end + step * 3.6, end - step * 1.0, 5.0, color);
    } else if (name == QLatin1String("flip-h") || name == QLatin1String("flip-v")) {
        const bool horizontal = name == QLatin1String("flip-h");
        p.setPen(antsPen(color));
        if (horizontal)
            p.drawLine(QPointF(12, 3), QPointF(12, 21));
        else
            p.drawLine(QPointF(3, 12), QPointF(21, 12));
        QPolygonF solid, hollow;
        if (horizontal) {
            solid << QPointF(9.5, 5) << QPointF(9.5, 19) << QPointF(3, 12);
            hollow << QPointF(14.5, 5) << QPointF(14.5, 19) << QPointF(21, 12);
        } else {
            solid << QPointF(5, 9.5) << QPointF(19, 9.5) << QPointF(12, 3);
            hollow << QPointF(5, 14.5) << QPointF(19, 14.5) << QPointF(12, 21);
        }
        QPainterPath solidPath;
        solidPath.addPolygon(solid);
        solidPath.closeSubpath();
        fillShape(p, solidPath, color);
        QPainterPath hollowPath;
        hollowPath.addPolygon(hollow);
        hollowPath.closeSubpath();
        p.setPen(strokePen(color));
        p.drawPath(hollowPath);
    }
    // --------------------------------------------------------------- layers
    else if (name == QLatin1String("layer")) {
        p.drawPath(sheetPath(12, 8.5, 4.5));
    } else if (name == QLatin1String("layer-duplicate")) {
        p.drawPath(sheetPath(7.5));
        p.drawPath(sheetPath(16.5));
    } else if (name == QLatin1String("layer-add")) {
        p.drawPath(sheetPath(18, 8, 3));
        plus(p, QPointF(12, 7), 3.8, color, 2.4);
    } else if (name == QLatin1String("layer-delete")) {
        p.drawPath(sheetPath(18, 8, 3));
        cross(p, QPointF(12, 7), 2.8, color, 2.4);
    } else if (name == QLatin1String("layer-merge")) {
        p.drawPath(sheetPath(18, 8, 3));
        arrow(p, QPointF(12, 3), QPointF(12, 12), color, 4.8, 2.2);
    } else if (name == QLatin1String("flatten")) {
        p.drawPath(sheetPath(10, 8, 4));
        p.setPen(strokePen(color, 3.0));
        p.drawLine(QPointF(4, 19.5), QPointF(20, 19.5));
    } else if (name == QLatin1String("layer-props")) {
        p.drawPath(sheetPath(6.5, 8, 3.5));
        p.setPen(strokePen(color));
        p.drawLine(QPointF(4, 14.5), QPointF(20, 14.5));
        p.drawLine(QPointF(4, 19.5), QPointF(20, 19.5));
        QPainterPath knobs;
        knobs.addEllipse(QPointF(8, 14.5), 2.4, 2.4);
        knobs.addEllipse(QPointF(16, 19.5), 2.4, 2.4);
        punch(p, knobs, 1.4);
        fillShape(p, knobs, color);
    } else if (name == QLatin1String("visible") || name == QLatin1String("hidden")) {
        p.drawPath(eyePath());
        QPainterPath pupil;
        pupil.addEllipse(QPointF(12, 12), 2.5, 2.5);
        fillShape(p, pupil, color);
        if (name == QLatin1String("hidden")) {
            const QPainterPath slash = linePath(QPointF(5, 19.5), QPointF(19, 5.5));
            punch(p, slash, 4.8, false);
            p.setPen(strokePen(color, 2.2));
            p.drawPath(slash);
        }
    } else if (name == QLatin1String("up") || name == QLatin1String("down")) {
        p.setPen(strokePen(color, 2.4));
        if (name == QLatin1String("up")) {
            p.drawLine(QPointF(5.5, 15.5), QPointF(12, 9));
            p.drawLine(QPointF(12, 9), QPointF(18.5, 15.5));
        } else {
            p.drawLine(QPointF(5.5, 8.5), QPointF(12, 15));
            p.drawLine(QPointF(12, 15), QPointF(18.5, 8.5));
        }
    }
    // ----------------------------------------------------------------- misc
    else if (name == QLatin1String("effect")) {
        fillShape(p, sparklePath(QPointF(10, 10), 6.8), color);
        fillShape(p, sparklePath(QPointF(18, 17.5), 3.8), color);
    } else if (name == QLatin1String("adjust")) {
        p.drawLine(QPointF(3.5, 8), QPointF(20.5, 8));
        p.drawLine(QPointF(3.5, 16), QPointF(20.5, 16));
        QPainterPath knobs;
        knobs.addEllipse(QPointF(8, 8), 2.9, 2.9);
        knobs.addEllipse(QPointF(16, 16), 2.9, 2.9);
        fillShape(p, knobs, color);
    } else if (name == QLatin1String("settings")) {
        fillShape(p, gearPath(), color);
        QPainterPath hole;
        hole.addEllipse(QPointF(12, 12), 3.4, 3.4);
        punch(p, hole, 0.0);
    } else if (name == QLatin1String("palette")) {
        QPainterPath blob;
        blob.addEllipse(QPointF(12, 12), 9.0, 8.5);
        fillShape(p, blob, color);
        QPainterPath wells;
        wells.addEllipse(QPointF(8.5, 9), 2.1, 2.1);
        wells.addEllipse(QPointF(15.5, 10), 2.1, 2.1);
        wells.addEllipse(QPointF(9.5, 16.5), 2.1, 2.1);
        wells.addEllipse(QPointF(16.5, 16.8), 2.6, 2.6);
        punch(p, wells, 0.0);
    } else if (name == QLatin1String("about")) {
        QPainterPath disc;
        disc.addEllipse(QPointF(12, 12), 8.5, 8.5);
        fillShape(p, disc, color);
        QPainterPath mark;
        mark.addEllipse(QPointF(12, 7.8), 1.9, 1.9);
        mark.addRoundedRect(QRectF(10.6, 11, 2.8, 6.6), 1.4, 1.4);
        punch(p, mark, 0.0);
    } else if (name == QLatin1String("close")) {
        cross(p, QPointF(12, 12), 5.5, color, 2.4);
    } else {
        // Unknown name: a neutral placeholder beats an empty button.
        p.drawRoundedRect(QRectF(5, 5, 14, 14), 2.5, 2.5);
    }
}

QHash<QString, QPixmap>& cache()
{
    static QHash<QString, QPixmap> store;
    return store;
}

} // namespace

QStringList names()
{
    // Grouped the way the interface uses them, so the contact sheet reads as
    // families rather than an alphabetical jumble.
    return {
        // tools
        QStringLiteral("pencil"), QStringLiteral("brush"), QStringLiteral("eraser"),
        QStringLiteral("bucket"), QStringLiteral("gradient"), QStringLiteral("line"),
        QStringLiteral("shapes"), QStringLiteral("text"), QStringLiteral("clone"),
        QStringLiteral("recolor"), QStringLiteral("picker"), QStringLiteral("edit"),
        // view
        QStringLiteral("zoom"), QStringLiteral("zoom-in"), QStringLiteral("zoom-out"),
        QStringLiteral("pan"), QStringLiteral("fit"), QStringLiteral("actual"),
        QStringLiteral("grid"),
        // selection
        QStringLiteral("select"), QStringLiteral("select-rect"),
        QStringLiteral("select-ellipse"), QStringLiteral("lasso"),
        QStringLiteral("wand"), QStringLiteral("move-selected"),
        QStringLiteral("move-selection"), QStringLiteral("select-all"),
        QStringLiteral("deselect"), QStringLiteral("invert"),
        // files and clipboard
        QStringLiteral("new"), QStringLiteral("open"), QStringLiteral("save"),
        QStringLiteral("undo"), QStringLiteral("redo"), QStringLiteral("cut"),
        QStringLiteral("copy"), QStringLiteral("paste"), QStringLiteral("delete"),
        // image
        QStringLiteral("image"), QStringLiteral("crop"), QStringLiteral("resize"),
        QStringLiteral("canvas"), QStringLiteral("rotate-cw"),
        QStringLiteral("rotate-ccw"), QStringLiteral("flip-h"),
        QStringLiteral("flip-v"),
        // layers
        QStringLiteral("layer"), QStringLiteral("layer-add"),
        QStringLiteral("layer-delete"), QStringLiteral("layer-duplicate"),
        QStringLiteral("layer-merge"), QStringLiteral("layer-props"),
        QStringLiteral("flatten"), QStringLiteral("visible"),
        QStringLiteral("hidden"), QStringLiteral("up"), QStringLiteral("down"),
        // misc
        QStringLiteral("effect"), QStringLiteral("adjust"),
        QStringLiteral("palette"), QStringLiteral("settings"),
        QStringLiteral("about"), QStringLiteral("close"),
    };
}

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
