#include "ui/ColorPanel.h"

#include <cmath>

#include <QCursor>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "core/ImageOps.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tessel {

// ----------------------------------------------------------------- wheel

ColorWheel::ColorWheel(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(120, 120);
    setCursor(Qt::CrossCursor);
}

void ColorWheel::setColor(Rgba color)
{
    color_ = color;
    float h = 0, s = 0, v = 0;
    ops::rgbToHsv(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, h, s, v);
    // Keep the previous hue when the colour is a pure grey, so the marker does
    // not jump back to red while dragging the value slider.
    if (s > 0.001f)
        hue_ = h;
    saturation_ = s;
    if (std::abs(v - value_) > 0.001f)
        value_ = v;
    update();
}

void ColorWheel::resizeEvent(QResizeEvent*)
{
    rebuild();
}

void ColorWheel::showEvent(QShowEvent* event)
{
    if (disc_.isNull())
        rebuild();
    QWidget::showEvent(event);
}

void ColorWheel::rebuild()
{
    const qreal ratio = devicePixelRatioF();
    const int side = std::max(16, std::min(width(), height()));
    const int target = std::max(16, int(std::lround(side * ratio)));

    // The disc is drawn at three times the final resolution and scaled back
    // down, which gives the rim proper antialiasing instead of the single
    // pixel feather that left it looking stair stepped.
    constexpr int kSupersample = 3;
    const int big = target * kSupersample;
    QImage canvas(big, big, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    const double radius = big / 2.0;

    for (int y = 0; y < big; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(canvas.scanLine(y));
        const double dy = y - radius + 0.5;
        for (int x = 0; x < big; ++x) {
            const double dx = x - radius + 0.5;
            const double distance = std::hypot(dx, dy);
            if (distance > radius) {
                line[x] = 0;
                continue;
            }
            const double angle = std::atan2(-dy, dx) * 180.0 / M_PI;
            float r = 0, g = 0, b = 0;
            // Full brightness keeps the disc readable; the value slider beside
            // it does the dimming.
            ops::hsvToRgb(float(angle), float(std::min(1.0, distance / radius)), 1.0f,
                          r, g, b);
            const double coverage = std::clamp(radius - distance, 0.0, 1.0);
            const int alpha = int(std::lround(coverage * 255.0));
            line[x] = qPremultiply(qRgba(toByte(r), toByte(g), toByte(b), alpha));
        }
    }

    disc_ = canvas.scaled(target, target, Qt::IgnoreAspectRatio,
                          Qt::SmoothTransformation);
    disc_.setDevicePixelRatio(ratio);
}

void ColorWheel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int side = std::min(width(), height());
    const QPointF offset((width() - side) / 2.0, (height() - side) / 2.0);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(offset, disc_);

    const double radius = side / 2.0;
    const double angle = hue_ * M_PI / 180.0;
    const QPointF centre = offset + QPointF(radius, radius);
    const QPointF marker(centre.x() + std::cos(angle) * saturation_ * radius,
                         centre.y() - std::sin(angle) * saturation_ * radius);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(saturation_ < 0.45f && hue_ >= 40.0f && hue_ <= 200.0f
                            ? Qt::black : Qt::white, 1.8));
    painter.drawEllipse(marker, 5.0, 5.0);
}

void ColorWheel::pick(QPointF pos)
{
    const int side = std::min(width(), height());
    const QPointF offset((width() - side) / 2.0, (height() - side) / 2.0);
    const double radius = side / 2.0;
    const QPointF local = pos - offset - QPointF(radius, radius);
    const double distance = std::hypot(local.x(), local.y());
    hue_ = float(std::atan2(-local.y(), local.x()) * 180.0 / M_PI);
    if (hue_ < 0)
        hue_ += 360.0f;
    saturation_ = float(std::clamp(distance / radius, 0.0, 1.0));
    // The disc is drawn at full brightness, so picking from it while the
    // current value is zero would hand back black and look like nothing had
    // happened. Starting from a black primary colour, which is the default,
    // that made the wheel appear dead. Pick what is actually shown instead.
    if (value_ <= 0.004f)
        value_ = 1.0f;
    float r = 0, g = 0, b = 0;
    ops::hsvToRgb(hue_, saturation_, value_, r, g, b);
    color_ = Rgba(toByte(r), toByte(g), toByte(b), color_.a);
    update();
    emit colorChanged(color_);
}

void ColorWheel::mousePressEvent(QMouseEvent* event) { pick(event->position()); }

void ColorWheel::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() != Qt::NoButton)
        pick(event->position());
}

// ----------------------------------------------------------------- ramp slider

RampSlider::RampSlider(Kind kind, QWidget* parent) : QWidget(parent), kind_(kind)
{
    setFixedHeight(18);
    setCursor(Qt::SizeHorCursor);
}

void RampSlider::setColor(Rgba color)
{
    color_ = color;
    if (kind_ == Value) {
        float h = 0, s = 0, v = 0;
        ops::rgbToHsv(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, h, s, v);
        position_ = v;
    } else {
        position_ = color.a / 255.0;
    }
    update();
}

void RampSlider::setPosition(double position)
{
    position_ = std::clamp(position, 0.0, 1.0);
    update();
}

void RampSlider::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box(1, 2, width() - 2.0, height() - 4.0);
    if (kind_ == Alpha) {
        QBrush checker(icons::checkerPixmap(4));
        painter.setPen(Qt::NoPen);
        painter.setBrush(checker);
        painter.drawRoundedRect(box, 3, 3);
    }
    QLinearGradient ramp(box.topLeft(), box.topRight());
    if (kind_ == Value) {
        float h = 0, s = 0, v = 0;
        ops::rgbToHsv(color_.r / 255.0f, color_.g / 255.0f, color_.b / 255.0f, h, s, v);
        for (int i = 0; i <= 8; ++i) {
            float r = 0, g = 0, b = 0;
            ops::hsvToRgb(h, s, i / 8.0f, r, g, b);
            ramp.setColorAt(i / 8.0, QColor::fromRgbF(r, g, b));
        }
    } else {
        ramp.setColorAt(0.0, QColor(color_.r, color_.g, color_.b, 0));
        ramp.setColorAt(1.0, QColor(color_.r, color_.g, color_.b, 255));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(ramp);
    painter.drawRoundedRect(box, 3, 3);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(theme::gridColor(), 1));
    painter.drawRoundedRect(box, 3, 3);

    const double at = box.left() + position_ * box.width();
    painter.setPen(QPen(Qt::white, 2.0));
    painter.drawLine(QPointF(at, box.top() - 1), QPointF(at, box.bottom() + 1));
    painter.setPen(QPen(Qt::black, 1.0));
    painter.drawLine(QPointF(at, box.top() - 1), QPointF(at, box.bottom() + 1));
}

void RampSlider::pick(QPointF pos)
{
    const double box = std::max(1.0, width() - 2.0);
    position_ = std::clamp((pos.x() - 1.0) / box, 0.0, 1.0);
    update();
    emit positionChanged(position_);
}

void RampSlider::mousePressEvent(QMouseEvent* event) { pick(event->position()); }

void RampSlider::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() != Qt::NoButton)
        pick(event->position());
}

// ----------------------------------------------------------------- swatches

SwatchPair::SwatchPair(QWidget* parent) : QWidget(parent)
{
    setFixedSize(78, 60);
    setToolTip(tr("Primary and secondary colours. Click a swatch to make "
                  "it active, use the arrows to swap or reset."));
}

void SwatchPair::setColors(Rgba primary, Rgba secondary)
{
    primary_ = primary;
    secondary_ = secondary;
    update();
}

void SwatchPair::setActiveIsPrimary(bool primary)
{
    activeIsPrimary_ = primary;
    update();
}

QRectF SwatchPair::primaryRect() const { return QRectF(1, 1, 32, 32); }
QRectF SwatchPair::secondaryRect() const { return QRectF(20, 20, 32, 32); }
QRectF SwatchPair::swapRect() const { return QRectF(58, 2, 18, 18); }
QRectF SwatchPair::resetRect() const { return QRectF(58, 24, 18, 18); }

void SwatchPair::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    auto drawSwatch = [&](const QRectF& box, Rgba color, bool active) {
        if (color.a < 255) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QBrush(icons::checkerPixmap(5)));
            painter.drawRect(box);
        }
        painter.setBrush(color.toQColor());
        painter.setPen(QPen(active ? theme::accentColor() : theme::gridColor(),
                            active ? 2.0 : 1.0));
        painter.drawRect(box);
    };
    drawSwatch(secondaryRect(), secondary_, !activeIsPrimary_);
    drawSwatch(primaryRect(), primary_, activeIsPrimary_);

    painter.setPen(Qt::NoPen);
    painter.drawPixmap(swapRect().topLeft(),
                       icons::pixmap(QStringLiteral("redo"), 18, theme::textColor()));
    painter.drawPixmap(resetRect().topLeft(),
                       icons::pixmap(QStringLiteral("undo"), 18, theme::textColor()));
}

void SwatchPair::mousePressEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (swapRect().contains(pos)) {
        emit swapRequested();
        return;
    }
    if (resetRect().contains(pos)) {
        emit resetRequested();
        return;
    }
    if (primaryRect().contains(pos)) {
        activeIsPrimary_ = true;
        update();
        emit activeChanged(true);
        return;
    }
    if (secondaryRect().contains(pos)) {
        activeIsPrimary_ = false;
        update();
        emit activeChanged(false);
    }
}

// ----------------------------------------------------------------- palette

PaletteGrid::PaletteGrid(QWidget* parent) : QWidget(parent)
{
    colors_ = defaultPalette();
    setToolTip(tr("Click for the primary colour, right click for the "
                  "secondary colour."));
}

QVector<Rgba> PaletteGrid::defaultPalette()
{
    QVector<Rgba> out;
    // Row 1 and 2: a neutral ramp from black to white.
    for (int i = 0; i < 16; ++i) {
        const uint8_t v = static_cast<uint8_t>(std::lround(i * 255.0 / 15.0));
        out.append(Rgba(v, v, v, 255));
    }
    // Rows 3 and 4: saturated hues and their lighter tints.
    for (int i = 0; i < 16; ++i) {
        float r = 0, g = 0, b = 0;
        ops::hsvToRgb(i * 360.0f / 16.0f, 1.0f, 1.0f, r, g, b);
        out.append(Rgba(toByte(r), toByte(g), toByte(b), 255));
    }
    for (int i = 0; i < 16; ++i) {
        float r = 0, g = 0, b = 0;
        ops::hsvToRgb(i * 360.0f / 16.0f, 0.45f, 1.0f, r, g, b);
        out.append(Rgba(toByte(r), toByte(g), toByte(b), 255));
    }
    for (int i = 0; i < 16; ++i) {
        float r = 0, g = 0, b = 0;
        ops::hsvToRgb(i * 360.0f / 16.0f, 1.0f, 0.55f, r, g, b);
        out.append(Rgba(toByte(r), toByte(g), toByte(b), 255));
    }
    return out;
}

void PaletteGrid::setColors(const QVector<Rgba>& colors)
{
    colors_ = colors;
    updateGeometry();
    update();
}

void PaletteGrid::setColumns(int columns)
{
    columns_ = std::max(1, columns);
    updateGeometry();
    update();
}

void PaletteGrid::setMinimumRows(int rows)
{
    minimumRows_ = std::max(0, rows);
    updateGeometry();
    update();
}

int PaletteGrid::cellSize() const
{
    return std::max(10, std::min(18, width() / std::max(1, columns())));
}

int PaletteGrid::rowCount() const
{
    const int filled = (int(colors_.size()) + columns() - 1) / columns();
    return std::max(minimumRows_, filled);
}

QSize PaletteGrid::sizeHint() const
{
    return QSize(columns() * 14, rowCount() * 14 + 2);
}

// The height a row of swatches is worth is not negotiable: a short panel
// scrolls rather than squeezing the rows away to nothing, which is what used
// to happen to the recent colours and the palette in a crowded column. The
// width still is, since a cell is sized from the width that is going.
QSize PaletteGrid::minimumSizeHint() const
{
    // Reserved at the largest a cell is ever drawn, since the panel asks for
    // this before it knows how wide it will be, and a row half cut off is
    // exactly the thing being fixed.
    return QSize(columns() * 10, rowCount() * 18 + 2);
}

void PaletteGrid::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const int cell = cellSize();
    const int cells = std::max(int(colors_.size()), minimumRows_ * columns());
    for (int i = 0; i < cells; ++i) {
        const int column = i % columns();
        const int row = i / columns();
        const QRect box(column * cell, row * cell, cell - 1, cell - 1);
        painter.setPen(QPen(theme::gridColor(), 1));
        if (i < colors_.size()) {
            if (colors_[i].a < 255) {
                painter.setBrush(QBrush(icons::checkerPixmap(4)));
                painter.drawRect(box);
            }
            painter.setBrush(colors_[i].toQColor());
        } else {
            // An empty slot in the recent row: outline only.
            painter.setBrush(Qt::NoBrush);
        }
        painter.drawRect(box);
    }
}

int PaletteGrid::indexAt(QPoint pos) const
{
    const int cell = cellSize();
    const int column = pos.x() / cell;
    const int row = pos.y() / cell;
    if (column < 0 || column >= columns())
        return -1;
    const int index = row * columns() + column;
    return index >= 0 && index < colors_.size() ? index : -1;
}

void PaletteGrid::mousePressEvent(QMouseEvent* event)
{
    const int index = indexAt(event->position().toPoint());
    if (index < 0)
        return;
    emit colorChosen(colors_[index], event->button() != Qt::RightButton);
}

// ----------------------------------------------------------------- screen pick

ScreenPickOverlay::ScreenPickOverlay(const QImage& grab, QRect bounds, QWidget* parent)
    : QWidget(parent), grab_(grab), bounds_(bounds)
{
    // Frameless, always on top and bypassing the window manager where that is
    // possible, so the frozen desktop image lines up with the real screen.
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                   | Qt::X11BypassWindowManagerHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Pick a colour"));
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    if (!bounds_.isEmpty() && !grab_.isNull())
        ratio_ = double(grab_.width()) / bounds_.width();
    if (ratio_ <= 0.0)
        ratio_ = 1.0;
    setGeometry(bounds_);
    cursor_ = QCursor::pos() - bounds_.topLeft();
}

Rgba ScreenPickOverlay::colorAt(QPointF pos) const
{
    if (grab_.isNull())
        return Rgba(0, 0, 0, 255);
    const int x = std::clamp(int(std::lround(pos.x() * ratio_)), 0, grab_.width() - 1);
    const int y = std::clamp(int(std::lround(pos.y() * ratio_)), 0, grab_.height() - 1);
    return Rgba::fromQColor(grab_.pixelColor(x, y));
}

void ScreenPickOverlay::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.drawImage(QRectF(0, 0, width(), height()), grab_, QRectF(grab_.rect()));

    const Rgba color = colorAt(cursor_);
    constexpr int kSpan = 11;        // grab pixels across the loupe
    constexpr double kZoom = 9.0;
    const double side = kSpan * kZoom;
    QRectF box(cursor_.x() + 20, cursor_.y() + 20, side, side + 20);
    if (box.right() > width())
        box.moveLeft(cursor_.x() - 20 - box.width());
    if (box.bottom() > height())
        box.moveTop(cursor_.y() - 20 - box.height());

    const QRectF source(std::lround(cursor_.x() * ratio_) - kSpan / 2.0,
                        std::lround(cursor_.y() * ratio_) - kSpan / 2.0, kSpan, kSpan);
    const QRectF zoom(box.left(), box.top(), side, side);
    // Nearest neighbour, so the magnified pixels stay square and the centre
    // one is unmistakably the colour that will be taken.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(zoom, grab_, source);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawRect(zoom);
    // Outline the centre cell, which is the pixel a click takes.
    const double centre = kZoom * (kSpan / 2);
    painter.setPen(QPen(Qt::black, 1));
    painter.drawRect(zoom.adjusted(centre, centre, -centre, -centre));

    const QRectF label(box.left(), zoom.bottom(), side, 20);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 200));
    painter.drawRect(label);
    painter.setPen(Qt::white);
    painter.drawText(label, Qt::AlignCenter, Palette::toHexString(color));
}

void ScreenPickOverlay::mouseMoveEvent(QMouseEvent* event)
{
    cursor_ = event->position();
    update();
}

void ScreenPickOverlay::mousePressEvent(QMouseEvent* event)
{
    cursor_ = event->position();
    if (event->button() == Qt::RightButton) {
        emit cancelled();
        return;
    }
    emit picked(colorAt(cursor_));
}

void ScreenPickOverlay::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        emit cancelled();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ----------------------------------------------------------------- panel

ColorPanel::ColorPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* top = new QHBoxLayout;
    top->setSpacing(8);
    swatches_ = new SwatchPair(this);
    top->addWidget(swatches_, 0, Qt::AlignTop);

    auto* sliders = new QVBoxLayout;
    sliders->setSpacing(4);
    wheel_ = new ColorWheel(this);
    wheel_->setFixedHeight(118);
    top->addWidget(wheel_, 1);
    root->addLayout(top);

    valueSlider_ = new RampSlider(RampSlider::Value, this);
    alphaSlider_ = new RampSlider(RampSlider::Alpha, this);
    auto* valueRow = new QHBoxLayout;
    valueRow->addWidget(new QLabel(tr("V"), this));
    valueRow->addWidget(valueSlider_, 1);
    auto* alphaRow = new QHBoxLayout;
    alphaRow->addWidget(new QLabel(tr("A"), this));
    alphaRow->addWidget(alphaSlider_, 1);
    root->addLayout(valueRow);
    root->addLayout(alphaRow);

    const auto makeSmallButton = [this](const QString& text, const QString& name,
                                        const QString& tip) {
        auto* button = new QPushButton(text, this);
        button->setObjectName(name);
        button->setFlat(true);
        button->setToolTip(tip);
        button->setMaximumHeight(22);
        // Hug the text, so the row keeps the buttons small and grouped.
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        return button;
    };

    moreButton_ = new QPushButton(tr("More  >>"), this);
    moreButton_->setCheckable(true);
    moreButton_->setFlat(true);
    pickButton_ = makeSmallButton(
        tr("Screen pick"), QStringLiteral("screenPick"),
        tr("Take a colour from anywhere on screen. Click to pick, "
           "Escape to cancel."));
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(4);
    actionRow->addWidget(moreButton_);
    actionRow->addStretch(1);
    actionRow->addWidget(pickButton_);
    root->addLayout(actionRow);

    details_ = new QWidget(this);
    auto* grid = new QGridLayout(details_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);
    auto makeSpin = [this](int maximum) {
        auto* spin = new QSpinBox(details_);
        spin->setRange(0, maximum);
        spin->setKeyboardTracking(false);
        spin->setMinimumWidth(58);
        return spin;
    };
    spinR_ = makeSpin(255);
    spinG_ = makeSpin(255);
    spinB_ = makeSpin(255);
    spinH_ = makeSpin(359);
    spinS_ = makeSpin(100);
    spinV_ = makeSpin(100);
    spinA_ = makeSpin(255);
    hexField_ = new QLineEdit(details_);
    hexField_->setMaxLength(9);
    hexField_->setPlaceholderText(tr("#RRGGBB"));

    grid->addWidget(new QLabel(tr("R"), details_), 0, 0);
    grid->addWidget(spinR_, 0, 1);
    grid->addWidget(new QLabel(tr("H"), details_), 0, 2);
    grid->addWidget(spinH_, 0, 3);
    grid->addWidget(new QLabel(tr("G"), details_), 1, 0);
    grid->addWidget(spinG_, 1, 1);
    grid->addWidget(new QLabel(tr("S"), details_), 1, 2);
    grid->addWidget(spinS_, 1, 3);
    grid->addWidget(new QLabel(tr("B"), details_), 2, 0);
    grid->addWidget(spinB_, 2, 1);
    grid->addWidget(new QLabel(tr("V"), details_), 2, 2);
    grid->addWidget(spinV_, 2, 3);
    grid->addWidget(new QLabel(tr("Alpha"), details_), 3, 0);
    grid->addWidget(spinA_, 3, 1);
    grid->addWidget(new QLabel(tr("Hex"), details_), 3, 2);
    grid->addWidget(hexField_, 3, 3);
    details_->setVisible(false);
    root->addWidget(details_);

    auto* recentLabel = new QLabel(tr("Recent"), this);
    root->addWidget(recentLabel);
    recentGrid_ = new PaletteGrid(this);
    recentGrid_->setColors({});
    recentGrid_->setColumns(kRecentLimit);
    recentGrid_->setMinimumRows(1);
    recentGrid_->setToolTip(tr("Recently used colours. Click for the "
                               "primary colour, right click for the "
                               "secondary colour."));
    root->addWidget(recentGrid_);

    auto* paletteRow = new QHBoxLayout;
    paletteRow->setSpacing(4);
    paletteRow->addWidget(new QLabel(tr("Palette"), this));
    paletteRow->addStretch(1);
    loadPaletteButton_ = makeSmallButton(
        tr("Load"), QStringLiteral("paletteLoad"),
        tr("Load a .gpl or hex palette."));
    savePaletteButton_ = makeSmallButton(
        tr("Save"), QStringLiteral("paletteSave"),
        tr("Save the palette as .gpl or hex."));
    resetPaletteButton_ = makeSmallButton(
        tr("Reset"), QStringLiteral("paletteReset"),
        tr("Restore the built in palette."));
    paletteRow->addWidget(loadPaletteButton_);
    paletteRow->addWidget(savePaletteButton_);
    paletteRow->addWidget(resetPaletteButton_);
    root->addLayout(paletteRow);

    palette_ = new PaletteGrid(this);
    root->addWidget(palette_);
    root->addStretch(1);

    // ---------------------------------------------------------- wiring
    connect(moreButton_, &QPushButton::toggled, this, [this](bool on) {
        details_->setVisible(on);
        moreButton_->setText(on ? tr("Less  <<") : tr("More  >>"));
    });
    connect(swatches_, &SwatchPair::activeChanged, this, [this](bool primary) {
        activeIsPrimary_ = primary;
        syncControls();
    });
    connect(swatches_, &SwatchPair::swapRequested, this, &ColorPanel::swapColors);
    connect(swatches_, &SwatchPair::resetRequested, this, &ColorPanel::resetColors);
    connect(wheel_, &ColorWheel::colorChanged, this,
            [this](Rgba color) { applyActive(color); });
    connect(valueSlider_, &RampSlider::positionChanged, this, [this](double position) {
        const Rgba current = activeColor();
        float h = 0, s = 0, v = 0;
        ops::rgbToHsv(current.r / 255.0f, current.g / 255.0f, current.b / 255.0f, h, s, v);
        float r = 0, g = 0, b = 0;
        ops::hsvToRgb(h, s, float(position), r, g, b);
        applyActive(Rgba(toByte(r), toByte(g), toByte(b), current.a));
    });
    connect(alphaSlider_, &RampSlider::positionChanged, this, [this](double position) {
        Rgba current = activeColor();
        current.a = toByte(float(position));
        applyActive(current);
    });
    const auto chooseColor = [this](Rgba color, bool primary) {
        if (primary)
            setPrimary(color);
        else
            setSecondary(color);
    };
    connect(palette_, &PaletteGrid::colorChosen, this, chooseColor);
    connect(recentGrid_, &PaletteGrid::colorChosen, this, chooseColor);

    connect(loadPaletteButton_, &QPushButton::clicked, this, [this] {
        QSettings settings;
        const QString last = settings.value(QStringLiteral("paletteDir")).toString();
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Load palette"), last, Palette::openFilter());
        if (path.isEmpty())
            return;
        settings.setValue(QStringLiteral("paletteDir"), QFileInfo(path).absolutePath());
        QString error;
        if (!loadPaletteFile(path, &error))
            showPaletteError(tr("Cannot load palette"), error);
    });
    connect(savePaletteButton_, &QPushButton::clicked, this, [this] {
        QSettings settings;
        const QString last = settings.value(QStringLiteral("paletteDir")).toString();
        QString path = QFileDialog::getSaveFileName(
            this, tr("Save palette"), last, Palette::saveFilter());
        if (path.isEmpty())
            return;
        // The dialog may return a bare name, and a palette with no extension
        // would be read back as a hex list, so settle on .gpl by default.
        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".gpl");
        settings.setValue(QStringLiteral("paletteDir"), QFileInfo(path).absolutePath());
        QString error;
        if (!savePaletteFile(path, &error))
            showPaletteError(tr("Cannot save palette"), error);
    });
    connect(resetPaletteButton_, &QPushButton::clicked, this, &ColorPanel::resetPalette);
    connect(pickButton_, &QPushButton::clicked, this, [this] {
        QString error;
        if (!beginScreenPick(&error))
            showPaletteError(tr("Cannot pick from the screen"), error);
    });

    // Recent colours are written back on a timer so that dragging across the
    // wheel does not touch the settings file on every pixel.
    recentSaveTimer_ = new QTimer(this);
    recentSaveTimer_->setSingleShot(true);
    recentSaveTimer_->setInterval(800);
    connect(recentSaveTimer_, &QTimer::timeout, this, &ColorPanel::saveRecentColors);

    const auto onSpin = [this] {
        if (updating_)
            return;
        applyActive(Rgba(static_cast<uint8_t>(spinR_->value()),
                         static_cast<uint8_t>(spinG_->value()),
                         static_cast<uint8_t>(spinB_->value()),
                         static_cast<uint8_t>(spinA_->value())),
                    true);
    };
    for (QSpinBox* spin : {spinR_, spinG_, spinB_, spinA_})
        connect(spin, &QSpinBox::valueChanged, this, onSpin);

    const auto onHsvSpin = [this] {
        if (updating_)
            return;
        float r = 0, g = 0, b = 0;
        ops::hsvToRgb(float(spinH_->value()), spinS_->value() / 100.0f,
                      spinV_->value() / 100.0f, r, g, b);
        applyActive(Rgba(toByte(r), toByte(g), toByte(b),
                         static_cast<uint8_t>(spinA_->value())), true);
    };
    for (QSpinBox* spin : {spinH_, spinS_, spinV_})
        connect(spin, &QSpinBox::valueChanged, this, onHsvSpin);

    connect(hexField_, &QLineEdit::editingFinished, this, [this] {
        if (updating_)
            return;
        QString text = hexField_->text().trimmed();
        text.remove(QLatin1Char('#'));
        bool ok = false;
        if (text.size() == 6) {
            const unsigned value = text.toUInt(&ok, 16);
            if (ok) {
                applyActive(Rgba(uint8_t((value >> 16) & 0xff), uint8_t((value >> 8) & 0xff),
                                 uint8_t(value & 0xff), activeColor().a), true);
            }
        } else if (text.size() == 8) {
            const unsigned value = text.toUInt(&ok, 16);
            if (ok) {
                applyActive(Rgba(uint8_t((value >> 24) & 0xff), uint8_t((value >> 16) & 0xff),
                                 uint8_t((value >> 8) & 0xff), uint8_t(value & 0xff)), true);
            }
        }
        syncControls();
    });

    loadRecentColors();
    syncControls();
}

ColorPanel::~ColorPanel()
{
    // The debounce timer may still be pending when the window closes.
    if (recentSaveTimer_ && recentSaveTimer_->isActive())
        saveRecentColors();
}

void ColorPanel::applyActive(Rgba color, bool fromFields)
{
    noteUsed(color);
    if (activeIsPrimary_) {
        primary_ = color;
        emit primaryChanged(primary_);
    } else {
        secondary_ = color;
        emit secondaryChanged(secondary_);
    }
    if (fromFields) {
        // Field edits must refresh the wheel too.
        syncControls();
    } else {
        updating_ = true;
        swatches_->setColors(primary_, secondary_);
        valueSlider_->setColor(color);
        alphaSlider_->setColor(color);
        const Rgba active = activeColor();
        spinR_->setValue(active.r);
        spinG_->setValue(active.g);
        spinB_->setValue(active.b);
        spinA_->setValue(active.a);
        float h = 0, s = 0, v = 0;
        ops::rgbToHsv(active.r / 255.0f, active.g / 255.0f, active.b / 255.0f, h, s, v);
        spinH_->setValue(int(std::lround(h)));
        spinS_->setValue(int(std::lround(s * 100.0)));
        spinV_->setValue(int(std::lround(v * 100.0)));
        hexField_->setText(QStringLiteral("#%1%2%3")
                               .arg(active.r, 2, 16, QLatin1Char('0'))
                               .arg(active.g, 2, 16, QLatin1Char('0'))
                               .arg(active.b, 2, 16, QLatin1Char('0'))
                               .toUpper());
        updating_ = false;
    }
}

void ColorPanel::syncControls()
{
    updating_ = true;
    const Rgba active = activeColor();
    swatches_->setColors(primary_, secondary_);
    swatches_->setActiveIsPrimary(activeIsPrimary_);
    wheel_->setColor(active);
    valueSlider_->setColor(active);
    alphaSlider_->setColor(active);
    spinR_->setValue(active.r);
    spinG_->setValue(active.g);
    spinB_->setValue(active.b);
    spinA_->setValue(active.a);
    float h = 0, s = 0, v = 0;
    ops::rgbToHsv(active.r / 255.0f, active.g / 255.0f, active.b / 255.0f, h, s, v);
    spinH_->setValue(int(std::lround(h)));
    spinS_->setValue(int(std::lround(s * 100.0)));
    spinV_->setValue(int(std::lround(v * 100.0)));
    hexField_->setText(QStringLiteral("#%1%2%3")
                           .arg(active.r, 2, 16, QLatin1Char('0'))
                           .arg(active.g, 2, 16, QLatin1Char('0'))
                           .arg(active.b, 2, 16, QLatin1Char('0'))
                           .toUpper());
    updating_ = false;
}

void ColorPanel::setPrimary(Rgba color)
{
    noteUsed(color);
    primary_ = color;
    syncControls();
    emit primaryChanged(primary_);
}

void ColorPanel::setSecondary(Rgba color)
{
    noteUsed(color);
    secondary_ = color;
    syncControls();
    emit secondaryChanged(secondary_);
}

void ColorPanel::setActiveColor(Rgba color)
{
    if (activeIsPrimary_)
        setPrimary(color);
    else
        setSecondary(color);
}

void ColorPanel::swapColors()
{
    std::swap(primary_, secondary_);
    // Most recent last, so the new primary colour ends up at the front.
    noteUsed(secondary_);
    noteUsed(primary_);
    syncControls();
    emit primaryChanged(primary_);
    emit secondaryChanged(secondary_);
}

void ColorPanel::resetColors()
{
    primary_ = Rgba(0, 0, 0, 255);
    secondary_ = Rgba(255, 255, 255, 255);
    noteUsed(secondary_);
    noteUsed(primary_);
    syncControls();
    emit primaryChanged(primary_);
    emit secondaryChanged(secondary_);
}

bool ColorPanel::detailsVisible() const { return details_->isVisible(); }

void ColorPanel::setDetailsVisible(bool visible) { moreButton_->setChecked(visible); }

// ----------------------------------------------------------------- recent

void ColorPanel::noteUsed(Rgba color)
{
    if (!recentGrid_)
        return;
    for (int i = 0; i < recent_.size(); ++i) {
        if (recent_[i] != color)
            continue;
        if (i == 0)
            return;                 // already the most recent entry
        recent_.remove(i);
        break;
    }
    recent_.prepend(color);
    while (recent_.size() > kRecentLimit)
        recent_.removeLast();
    recentGrid_->setColors(recent_);
    if (recentSaveTimer_)
        recentSaveTimer_->start();
}

void ColorPanel::clearRecentColors()
{
    recent_.clear();
    if (recentGrid_)
        recentGrid_->setColors(recent_);
    saveRecentColors();
}

void ColorPanel::loadRecentColors()
{
    QSettings settings;
    const QStringList stored =
        settings.value(QStringLiteral("recentColors")).toStringList();
    recent_.clear();
    for (const QString& entry : stored) {
        if (recent_.size() >= kRecentLimit)
            break;
        Rgba color;
        // Anything unparsable is from an older or hand edited settings file and
        // is simply dropped.
        if (Palette::parseHexColor(entry, color) && !recent_.contains(color))
            recent_.append(color);
    }
    if (recentGrid_)
        recentGrid_->setColors(recent_);
}

void ColorPanel::saveRecentColors() const
{
    QStringList stored;
    stored.reserve(int(recent_.size()));
    for (Rgba color : recent_)
        stored.append(Palette::toHexString(color));
    QSettings settings;
    settings.setValue(QStringLiteral("recentColors"), stored);
}

// ----------------------------------------------------------------- palette files

QVector<Rgba> ColorPanel::paletteColors() const
{
    return palette_ ? palette_->colors() : QVector<Rgba>();
}

bool ColorPanel::loadPaletteFile(const QString& path, QString* error)
{
    const Palette loaded = Palette::load(path, error);
    if (loaded.isEmpty())
        return false;
    paletteName_ = loaded.name();
    palette_->setColumns(loaded.columns());
    palette_->setColors(loaded.colors());
    palette_->setToolTip(tr("%1 (%2 colours). Click for the primary "
                            "colour, right click for the secondary "
                            "colour.")
                             .arg(paletteName_)
                             .arg(loaded.size()));
    return true;
}

bool ColorPanel::savePaletteFile(const QString& path, QString* error) const
{
    Palette out(palette_ ? palette_->colors() : QVector<Rgba>(),
                paletteName_.isEmpty() ? QStringLiteral("Tessel") : paletteName_);
    out.setColumns(palette_ ? palette_->columns() : 16);
    return out.save(path, error);
}

void ColorPanel::resetPalette()
{
    paletteName_.clear();
    palette_->setColumns(16);
    palette_->setColors(PaletteGrid::defaultPalette());
    palette_->setToolTip(tr("Click for the primary colour, right click "
                            "for the secondary colour."));
}

void ColorPanel::showPaletteError(const QString& title, const QString& message)
{
    QMessageBox::warning(this, title,
                         message.isEmpty() ? tr("Unknown error.") : message);
}

// ----------------------------------------------------------------- screen pick

QRect ColorPanel::screenBounds()
{
    QRect bounds;
    for (QScreen* screen : QGuiApplication::screens())
        bounds = bounds.united(screen->geometry());
    return bounds;
}

QImage ColorPanel::grabScreens()
{
    const QList<QScreen*> screens = QGuiApplication::screens();
    const QRect bounds = screenBounds();
    if (screens.isEmpty() || bounds.isEmpty())
        return QImage();

    double ratio = 1.0;
    for (QScreen* screen : screens)
        ratio = std::max(ratio, double(screen->devicePixelRatio()));
    QImage out(QSize(int(std::lround(bounds.width() * ratio)),
                     int(std::lround(bounds.height() * ratio))),
               QImage::Format_RGB32);
    if (out.isNull())
        return QImage();
    out.fill(Qt::black);

    QPainter painter(&out);
    // Nearest neighbour keeps every sampled colour exact when one screen's own
    // scale factor differs from the one the composite is built at.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    bool any = false;
    for (QScreen* screen : screens) {
        // A window id of zero asks for the whole screen rather than a window.
        const QPixmap shot = screen->grabWindow(0);
        if (shot.isNull())
            continue;
        const QRect geometry = screen->geometry();
        const QRectF target((geometry.x() - bounds.x()) * ratio,
                            (geometry.y() - bounds.y()) * ratio,
                            geometry.width() * ratio, geometry.height() * ratio);
        painter.drawPixmap(target, shot, QRectF(shot.rect()));
        any = true;
    }
    painter.end();
    return any ? out : QImage();
}

bool ColorPanel::isUsableGrab(const QImage& grab)
{
    // Why this check exists: QScreen::grabWindow only works where the platform
    // lets a client read the framebuffer, which means X11, Windows and macOS.
    // Wayland deliberately does not: a client cannot see anything it did not
    // draw itself, and capture has to go through the compositor or the portal.
    // Qt's Wayland plugin has no screen capture, so the grab comes back either
    // null or as a blank, fully black image. Without this test every pick on
    // such a session would quietly hand back black and look like a bug in the
    // picker, so the panel reports the limitation instead.
    //
    // A genuinely all black desktop is reported as unusable too. That is the
    // right trade: the message is clear and a black screen is nothing anybody
    // picks a colour from.
    if (grab.isNull() || grab.width() < 2 || grab.height() < 2)
        return false;
    const int stepX = std::max(1, grab.width() / 64);
    const int stepY = std::max(1, grab.height() / 64);
    for (int y = 0; y < grab.height(); y += stepY) {
        for (int x = 0; x < grab.width(); x += stepX) {
            if ((grab.pixel(x, y) & 0x00ffffffu) != 0u)
                return true;
        }
    }
    return false;
}

bool ColorPanel::beginScreenPick(QString* error)
{
    if (error)
        error->clear();
    if (overlay_)
        return true;

    const QImage grab = grabScreens();
    if (!isUsableGrab(grab)) {
        if (error) {
            *error = tr("Tessel could not read the screen.\n\nThis session does "
                        "not allow an application to capture the desktop, which is "
                        "the case on most Wayland compositors. Use the colour picker "
                        "tool on the canvas instead, or run Tessel on an X11 "
                        "session.");
        }
        return false;
    }

    auto* overlay = new ScreenPickOverlay(grab, screenBounds());
    overlay_ = overlay;
    connect(overlay, &ScreenPickOverlay::picked, this, [this](Rgba color) {
        setActiveColor(color);
        cancelScreenPick();
    });
    connect(overlay, &ScreenPickOverlay::cancelled, this,
            [this] { cancelScreenPick(); });
    connect(overlay, &QObject::destroyed, this, [this] { overlay_ = nullptr; });
    overlay->show();
    overlay->raise();
    overlay->activateWindow();
    overlay->setFocus();
    return true;
}

void ColorPanel::cancelScreenPick()
{
    if (!overlay_)
        return;
    ScreenPickOverlay* overlay = overlay_;
    overlay_ = nullptr;
    // Deferred, because this is reached from inside the overlay's own event
    // handlers.
    overlay->hide();
    overlay->deleteLater();
}

} // namespace tessel
