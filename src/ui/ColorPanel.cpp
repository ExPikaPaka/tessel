#include "ui/ColorPanel.h"

#include <cmath>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
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
    setToolTip(QStringLiteral("Primary and secondary colours. Click a swatch to make "
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
    setToolTip(QStringLiteral("Click for the primary colour, right click for the "
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

int PaletteGrid::cellSize() const
{
    return std::max(10, std::min(18, width() / std::max(1, columns())));
}

QSize PaletteGrid::sizeHint() const
{
    const int rows = (colors_.size() + columns() - 1) / columns();
    return QSize(columns() * 14, rows * 14 + 2);
}

void PaletteGrid::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const int cell = cellSize();
    for (int i = 0; i < colors_.size(); ++i) {
        const int column = i % columns();
        const int row = i / columns();
        const QRect box(column * cell, row * cell, cell - 1, cell - 1);
        painter.setPen(QPen(theme::gridColor(), 1));
        painter.setBrush(colors_[i].toQColor());
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
    valueRow->addWidget(new QLabel(QStringLiteral("V"), this));
    valueRow->addWidget(valueSlider_, 1);
    auto* alphaRow = new QHBoxLayout;
    alphaRow->addWidget(new QLabel(QStringLiteral("A"), this));
    alphaRow->addWidget(alphaSlider_, 1);
    root->addLayout(valueRow);
    root->addLayout(alphaRow);

    moreButton_ = new QPushButton(QStringLiteral("More  >>"), this);
    moreButton_->setCheckable(true);
    moreButton_->setFlat(true);
    root->addWidget(moreButton_, 0, Qt::AlignLeft);

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
    hexField_->setPlaceholderText(QStringLiteral("#RRGGBB"));

    grid->addWidget(new QLabel(QStringLiteral("R"), details_), 0, 0);
    grid->addWidget(spinR_, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("H"), details_), 0, 2);
    grid->addWidget(spinH_, 0, 3);
    grid->addWidget(new QLabel(QStringLiteral("G"), details_), 1, 0);
    grid->addWidget(spinG_, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("S"), details_), 1, 2);
    grid->addWidget(spinS_, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("B"), details_), 2, 0);
    grid->addWidget(spinB_, 2, 1);
    grid->addWidget(new QLabel(QStringLiteral("V"), details_), 2, 2);
    grid->addWidget(spinV_, 2, 3);
    grid->addWidget(new QLabel(QStringLiteral("Alpha"), details_), 3, 0);
    grid->addWidget(spinA_, 3, 1);
    grid->addWidget(new QLabel(QStringLiteral("Hex"), details_), 3, 2);
    grid->addWidget(hexField_, 3, 3);
    details_->setVisible(false);
    root->addWidget(details_);

    palette_ = new PaletteGrid(this);
    root->addWidget(palette_);
    root->addStretch(1);

    // ---------------------------------------------------------- wiring
    connect(moreButton_, &QPushButton::toggled, this, [this](bool on) {
        details_->setVisible(on);
        moreButton_->setText(on ? QStringLiteral("Less  <<") : QStringLiteral("More  >>"));
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
    connect(palette_, &PaletteGrid::colorChosen, this, [this](Rgba color, bool primary) {
        if (primary)
            setPrimary(color);
        else
            setSecondary(color);
    });

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

    syncControls();
}

void ColorPanel::applyActive(Rgba color, bool fromFields)
{
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
    primary_ = color;
    syncControls();
    emit primaryChanged(primary_);
}

void ColorPanel::setSecondary(Rgba color)
{
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
    syncControls();
    emit primaryChanged(primary_);
    emit secondaryChanged(secondary_);
}

void ColorPanel::resetColors()
{
    primary_ = Rgba(0, 0, 0, 255);
    secondary_ = Rgba(255, 255, 255, 255);
    syncControls();
    emit primaryChanged(primary_);
    emit secondaryChanged(secondary_);
}

bool ColorPanel::detailsVisible() const { return details_->isVisible(); }

void ColorPanel::setDetailsVisible(bool visible) { moreButton_->setChecked(visible); }

} // namespace tessel
