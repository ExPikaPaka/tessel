#include "ui/CurveWidget.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tessel {

// ----------------------------------------------------------------- preview

EffectPreview::EffectPreview(Effect* effect, Document* doc, History* history, Rgba primary,
                             Rgba secondary)
    : effect_(effect), doc_(doc), history_(history), primary_(primary),
      secondary_(secondary)
{
    if (const Layer* layer = doc_->activeLayer())
        source_ = layer->image;
}

EffectPreview::~EffectPreview() { clear(); }

Rect EffectPreview::region() const
{
    const Selection& selection = doc_->selection();
    if (!selection.isActive())
        return doc_->rect();
    const Rect bounds = selection.bounds();
    return bounds.isEmpty() ? doc_->rect() : bounds;
}

void EffectPreview::update(const ParamValues& values)
{
    const Layer* layer = doc_->activeLayer();
    if (!layer || source_.isNull())
        return;
    EffectContext context;
    context.source = &source_;
    context.clip = clip();
    context.region = region();
    context.primary = primary_;
    context.secondary = secondary_;
    context.document = doc_;
    const Image result = effect_->apply(context, values);
    preview_ = source_;
    ops::mixThroughMask(preview_, result, context.clip, context.region);
    doc_->setPreview(layer->uid(), &preview_);
    active_ = true;
    if (repaint_)
        repaint_();
}

void EffectPreview::clear()
{
    if (!active_)
        return;
    active_ = false;
    doc_->clearPreview();
    if (repaint_)
        repaint_();
}

void EffectPreview::restoreSource()
{
    if (Layer* layer = doc_->activeLayer(); layer && !source_.isNull())
        layer->image = source_;
    doc_->invalidate();
}

void EffectPreview::commit(const ParamValues& values)
{
    Layer* layer = doc_->activeLayer();
    if (!layer || source_.isNull())
        return;
    EffectContext context;
    context.source = &source_;
    context.clip = clip();
    context.region = region();
    context.primary = primary_;
    context.secondary = secondary_;
    context.document = doc_;
    const Image result = effect_->apply(context, values);
    layer->image = source_;
    EditScope scope(*doc_, *history_, layer, effect_->name(), QStringLiteral("adjust"),
                    context.region);
    ops::mixThroughMask(layer->image, result, context.clip, context.region);
    scope.commit();
    doc_->invalidateLayer(layer);
}

// ----------------------------------------------------------------- curve editor

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent)
{
    curveColor_ = theme::textColor();
    reset();
    setMinimumSize(220, 220);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void CurveEditor::reset()
{
    points_.clear();
    points_ << QPointF(0, 0) << QPointF(255, 255);
    update();
}

void CurveEditor::setPoints(const QPolygonF& points)
{
    points_ = points;
    if (points_.size() < 2)
        reset();
    sortPoints();
    update();
}

void CurveEditor::setHistogram(const std::array<uint32_t, 256>& histogram)
{
    histogram_ = histogram;
    histogramPeak_ = 0;
    for (uint32_t value : histogram_)
        histogramPeak_ = std::max(histogramPeak_, value);
    update();
}

void CurveEditor::setCurveColor(const QColor& color)
{
    curveColor_ = color;
    update();
}

QRectF CurveEditor::plotRect() const
{
    const double margin = 10.0;
    const double side = std::min(width(), height()) - 2 * margin;
    return QRectF((width() - side) / 2.0, (height() - side) / 2.0, side, side);
}

QPointF CurveEditor::toWidget(QPointF value) const
{
    const QRectF box = plotRect();
    return QPointF(box.left() + value.x() / 255.0 * box.width(),
                   box.bottom() - value.y() / 255.0 * box.height());
}

QPointF CurveEditor::toValue(QPointF widget) const
{
    const QRectF box = plotRect();
    const double x = (widget.x() - box.left()) / std::max(1.0, box.width()) * 255.0;
    const double y = (box.bottom() - widget.y()) / std::max(1.0, box.height()) * 255.0;
    return QPointF(std::clamp(x, 0.0, 255.0), std::clamp(y, 0.0, 255.0));
}

void CurveEditor::sortPoints()
{
    std::sort(points_.begin(), points_.end(),
              [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
}

int CurveEditor::pointAt(QPointF widget) const
{
    for (int i = 0; i < points_.size(); ++i) {
        const QPointF at = toWidget(points_[i]);
        if (std::hypot(widget.x() - at.x(), widget.y() - at.y()) <= 7.0)
            return i;
    }
    return -1;
}

void CurveEditor::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = plotRect();

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::panelColor().darker(115));
    painter.drawRect(box);

    // Histogram behind the curve.
    if (histogramPeak_ > 0) {
        QColor fill = theme::textColor();
        fill.setAlpha(55);
        painter.setBrush(fill);
        painter.setPen(Qt::NoPen);
        const double barWidth = box.width() / 256.0;
        for (int i = 0; i < 256; ++i) {
            const double scaled = std::sqrt(double(histogram_[size_t(i)]) / histogramPeak_);
            const double height = scaled * box.height();
            painter.drawRect(QRectF(box.left() + i * barWidth, box.bottom() - height,
                                    std::max(1.0, barWidth), height));
        }
    }

    // Grid and the identity diagonal.
    QColor grid = theme::gridColor();
    grid.setAlpha(140);
    painter.setPen(QPen(grid, 1));
    for (int i = 1; i < 4; ++i) {
        const double t = i / 4.0;
        painter.drawLine(QPointF(box.left() + t * box.width(), box.top()),
                         QPointF(box.left() + t * box.width(), box.bottom()));
        painter.drawLine(QPointF(box.left(), box.top() + t * box.height()),
                         QPointF(box.right(), box.top() + t * box.height()));
    }
    painter.setPen(QPen(grid, 1, Qt::DashLine));
    painter.drawLine(box.bottomLeft(), box.topRight());
    painter.setPen(QPen(theme::gridColor(), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box);

    // The curve itself, sampled through the same interpolation the effect uses.
    std::vector<QPointF> controls(points_.begin(), points_.end());
    const ops::Lut lut = ops::lutFromControlPoints(controls);
    QPainterPath path;
    for (int i = 0; i < 256; ++i) {
        const QPointF at = toWidget(QPointF(i, lut[size_t(i)]));
        if (i == 0)
            path.moveTo(at);
        else
            path.lineTo(at);
    }
    painter.setPen(QPen(curveColor_, 1.8));
    painter.drawPath(path);

    for (int i = 0; i < points_.size(); ++i) {
        const QPointF at = toWidget(points_[i]);
        painter.setBrush(i == dragging_ ? theme::accentColor() : QColor(255, 255, 255));
        painter.setPen(QPen(QColor(20, 20, 20), 1.2));
        painter.drawRect(QRectF(at.x() - 3.5, at.y() - 3.5, 7, 7));
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* event)
{
    const int index = pointAt(event->position());
    if (event->button() == Qt::RightButton) {
        // Right click removes a point, but the two ends always stay.
        if (index > 0 && index < points_.size() - 1) {
            points_.remove(index);
            emit curveChanged();
            update();
        }
        return;
    }
    if (index >= 0) {
        dragging_ = index;
    } else {
        points_ << toValue(event->position());
        sortPoints();
        dragging_ = pointAt(event->position());
        emit curveChanged();
    }
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_ < 0 || dragging_ >= points_.size())
        return;
    QPointF value = toValue(event->position());
    // The endpoints keep their x position so the curve always spans 0..255.
    if (dragging_ == 0)
        value.setX(0);
    else if (dragging_ == points_.size() - 1)
        value.setX(255);
    else {
        const double low = points_[dragging_ - 1].x() + 1.0;
        const double high = points_[dragging_ + 1].x() - 1.0;
        value.setX(std::clamp(value.x(), low, std::max(low, high)));
    }
    points_[dragging_] = value;
    emit curveChanged();
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*)
{
    dragging_ = -1;
    update();
}

// ----------------------------------------------------------------- curves dialog

CurvesDialog::CurvesDialog(Effect* effect, Document* doc, History* history, Rgba primary,
                           Rgba secondary, QWidget* parent)
    : QDialog(parent), effect_(effect), preview_(effect, doc, history, primary, secondary)
{
    setWindowTitle(QStringLiteral("Curves"));
    setWindowIcon(icons::get(QStringLiteral("adjust"), 22));
    setModal(false);
    values_ = ParamValues::defaults(effect_->params());
    for (QPolygonF& curve : curves_)
        curve << QPointF(0, 0) << QPointF(255, 255);

    const Layer* layer = doc->activeLayer();
    if (layer) {
        const Rect region = preview_.region();
        histogram_ = ops::computeHistogram(layer->image, doc->selection().clipMask(),
                                           region);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Transfer map"), this));
    channel_ = new QComboBox(this);
    channel_->addItems({QStringLiteral("Luminosity"), QStringLiteral("Red"),
                        QStringLiteral("Green"), QStringLiteral("Blue")});
    top->addWidget(channel_, 1);
    auto* resetChannel = new QPushButton(QStringLiteral("Reset channel"), this);
    top->addWidget(resetChannel);
    root->addLayout(top);

    editor_ = new CurveEditor(this);
    root->addWidget(editor_, 1);

    auto* hint = new QLabel(QStringLiteral("Click to add a control point, drag to move "
                                           "it, right click to remove it."),
                            this);
    hint->setStyleSheet(QStringLiteral("color: %1; font-size: 8pt;")
                            .arg(theme::textColor().darker(140).name()));
    root->addWidget(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                             | QDialogButtonBox::Reset,
                                         this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this] {
        for (QPolygonF& curve : curves_) {
            curve.clear();
            curve << QPointF(0, 0) << QPointF(255, 255);
        }
        channelChanged(current_);
        curveEdited();
    });
    connect(resetChannel, &QPushButton::clicked, this, [this] {
        curves_[current_].clear();
        curves_[current_] << QPointF(0, 0) << QPointF(255, 255);
        channelChanged(current_);
        curveEdited();
    });
    connect(channel_, &QComboBox::currentIndexChanged, this, &CurvesDialog::channelChanged);
    connect(editor_, &CurveEditor::curveChanged, this, &CurvesDialog::curveEdited);

    channelChanged(0);
    preview_.update(values_);
}

void CurvesDialog::channelChanged(int index)
{
    // Remember the curve being left behind before switching.
    curves_[current_] = editor_->points();
    current_ = std::clamp(index, 0, 3);
    editor_->setPoints(curves_[current_]);
    static const std::array<const char*, 4> names = {"luma", "red", "green", "blue"};
    Q_UNUSED(names);
    switch (current_) {
    case 1:
        editor_->setHistogram(histogram_.red);
        editor_->setCurveColor(QColor(0xff, 0x6b, 0x6b));
        break;
    case 2:
        editor_->setHistogram(histogram_.green);
        editor_->setCurveColor(QColor(0x6b, 0xdd, 0x8b));
        break;
    case 3:
        editor_->setHistogram(histogram_.blue);
        editor_->setCurveColor(QColor(0x6b, 0xa8, 0xff));
        break;
    default:
        editor_->setHistogram(histogram_.luma);
        editor_->setCurveColor(theme::textColor());
        break;
    }
    values_.set(QStringLiteral("channel"), current_);
}

void CurvesDialog::curveEdited()
{
    curves_[current_] = editor_->points();
    static const char* keys[4] = {"curve_rgb", "curve_r", "curve_g", "curve_b"};
    for (int i = 0; i < 4; ++i)
        values_.set(QString::fromLatin1(keys[i]), QVariant::fromValue(curves_[i]));
    preview_.update(values_);
}

void CurvesDialog::done(int result)
{
    preview_.clear();
    if (result == QDialog::Accepted)
        preview_.commit(values_);
    else
        preview_.restoreSource();
    QDialog::done(result);
}

// ----------------------------------------------------------------- levels dialog

namespace {

// Small histogram strip with the input and output markers drawn on it.
class HistogramStrip : public QWidget {
public:
    explicit HistogramStrip(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(98);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setHistogram(const std::array<uint32_t, 256>& histogram)
    {
        histogram_ = histogram;
        peak_ = 0;
        for (uint32_t value : histogram_)
            peak_ = std::max(peak_, value);
        update();
    }

    void setMarkers(int black, int white, double gamma)
    {
        black_ = black;
        white_ = white;
        gamma_ = gamma;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box(0.5, 0.5, width() - 1.0, height() - 16.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::panelColor().darker(115));
        painter.drawRect(box);
        if (peak_ > 0) {
            QColor fill = theme::textColor();
            fill.setAlpha(80);
            painter.setBrush(fill);
            const double barWidth = box.width() / 256.0;
            for (int i = 0; i < 256; ++i) {
                const double scaled = std::sqrt(double(histogram_[size_t(i)]) / peak_);
                const double h = scaled * box.height();
                painter.drawRect(QRectF(box.left() + i * barWidth, box.bottom() - h,
                                        std::max(1.0, barWidth), h));
            }
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme::gridColor(), 1));
        painter.drawRect(box);

        auto markerX = [&](int value) {
            return box.left() + value / 255.0 * box.width();
        };
        painter.setPen(QPen(QColor(40, 40, 40), 1.5));
        painter.drawLine(QPointF(markerX(black_), box.top()),
                         QPointF(markerX(black_), box.bottom()));
        painter.setPen(QPen(QColor(240, 240, 240), 1.5));
        painter.drawLine(QPointF(markerX(white_), box.top()),
                         QPointF(markerX(white_), box.bottom()));
        // The midpoint marker follows the gamma value.
        const double mid = black_ + (white_ - black_) * std::pow(0.5, gamma_);
        painter.setPen(QPen(theme::accentColor(), 1.5, Qt::DashLine));
        painter.drawLine(QPointF(markerX(int(std::lround(mid))), box.top()),
                         QPointF(markerX(int(std::lround(mid))), box.bottom()));

        // Triangles below for the black and white points.
        auto triangle = [&](double x, const QColor& color) {
            QPolygonF marker;
            marker << QPointF(x, box.bottom() + 2) << QPointF(x - 4, box.bottom() + 11)
                   << QPointF(x + 4, box.bottom() + 11);
            painter.setBrush(color);
            painter.setPen(QPen(theme::gridColor(), 1));
            painter.drawPolygon(marker);
        };
        triangle(markerX(black_), QColor(30, 30, 30));
        triangle(markerX(white_), QColor(245, 245, 245));
    }

private:
    std::array<uint32_t, 256> histogram_{};
    uint32_t peak_ = 0;
    int black_ = 0;
    int white_ = 255;
    double gamma_ = 1.0;
};

} // namespace

LevelsDialog::LevelsDialog(Effect* effect, Document* doc, History* history, Rgba primary,
                           Rgba secondary, QWidget* parent)
    : QDialog(parent), effect_(effect), preview_(effect, doc, history, primary, secondary)
{
    setWindowTitle(QStringLiteral("Levels"));
    setWindowIcon(icons::get(QStringLiteral("adjust"), 22));
    setModal(false);
    values_ = ParamValues::defaults(effect_->params());

    if (const Layer* layer = doc->activeLayer()) {
        histogram_ = ops::computeHistogram(layer->image, doc->selection().clipMask(),
                                           preview_.region());
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* strip = new HistogramStrip(this);
    strip->setHistogram(histogram_.luma);
    histogramView_ = strip;
    root->addWidget(strip);

    auto* form = new QFormLayout;
    form->setSpacing(6);
    inBlack_ = new QSpinBox(this);
    inBlack_->setRange(0, 254);
    inWhite_ = new QSpinBox(this);
    inWhite_->setRange(1, 255);
    inWhite_->setValue(255);
    gamma_ = new QDoubleSpinBox(this);
    gamma_->setRange(0.1, 10.0);
    gamma_->setSingleStep(0.05);
    gamma_->setDecimals(2);
    gamma_->setValue(1.0);
    outBlack_ = new QSpinBox(this);
    outBlack_->setRange(0, 255);
    outWhite_ = new QSpinBox(this);
    outWhite_->setRange(0, 255);
    outWhite_->setValue(255);
    for (QSpinBox* spin : {inBlack_, inWhite_, outBlack_, outWhite_})
        spin->setKeyboardTracking(false);
    gamma_->setKeyboardTracking(false);

    form->addRow(QStringLiteral("Input black"), inBlack_);
    form->addRow(QStringLiteral("Input white"), inWhite_);
    form->addRow(QStringLiteral("Gamma"), gamma_);
    form->addRow(QStringLiteral("Output black"), outBlack_);
    form->addRow(QStringLiteral("Output white"), outWhite_);

    auto* channels = new QHBoxLayout;
    red_ = new QCheckBox(QStringLiteral("Red"), this);
    green_ = new QCheckBox(QStringLiteral("Green"), this);
    blue_ = new QCheckBox(QStringLiteral("Blue"), this);
    for (QCheckBox* box : {red_, green_, blue_}) {
        box->setChecked(true);
        channels->addWidget(box);
    }
    channels->addStretch(1);
    auto* autoButton = new QPushButton(QStringLiteral("Auto"), this);
    channels->addWidget(autoButton);
    form->addRow(QStringLiteral("Channels"), channels);
    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                             | QDialogButtonBox::Reset,
                                         this);
    root->addWidget(buttons);

    const auto onChanged = [this] { pull(); };
    for (QSpinBox* spin : {inBlack_, inWhite_, outBlack_, outWhite_})
        connect(spin, &QSpinBox::valueChanged, this, onChanged);
    connect(gamma_, &QDoubleSpinBox::valueChanged, this, onChanged);
    for (QCheckBox* box : {red_, green_, blue_})
        connect(box, &QCheckBox::toggled, this, onChanged);
    connect(autoButton, &QPushButton::clicked, this, &LevelsDialog::autoLevel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this] {
        updating_ = true;
        inBlack_->setValue(0);
        inWhite_->setValue(255);
        gamma_->setValue(1.0);
        outBlack_->setValue(0);
        outWhite_->setValue(255);
        red_->setChecked(true);
        green_->setChecked(true);
        blue_->setChecked(true);
        updating_ = false;
        pull();
    });

    pull();
}

void LevelsDialog::pull()
{
    if (updating_)
        return;
    if (inWhite_->value() <= inBlack_->value())
        inWhite_->setValue(std::min(255, inBlack_->value() + 1));
    values_.set(QStringLiteral("in_black"), inBlack_->value());
    values_.set(QStringLiteral("in_white"), inWhite_->value());
    values_.set(QStringLiteral("gamma"), gamma_->value());
    values_.set(QStringLiteral("out_black"), outBlack_->value());
    values_.set(QStringLiteral("out_white"), outWhite_->value());
    values_.set(QStringLiteral("red"), red_->isChecked());
    values_.set(QStringLiteral("green"), green_->isChecked());
    values_.set(QStringLiteral("blue"), blue_->isChecked());
    static_cast<HistogramStrip*>(histogramView_)
        ->setMarkers(inBlack_->value(), inWhite_->value(), gamma_->value());
    preview_.update(values_);
}

void LevelsDialog::autoLevel()
{
    updating_ = true;
    inBlack_->setValue(histogram_.percentile(histogram_.luma, 0.005));
    inWhite_->setValue(std::max(inBlack_->value() + 1,
                                histogram_.percentile(histogram_.luma, 0.995)));
    gamma_->setValue(1.0);
    outBlack_->setValue(0);
    outWhite_->setValue(255);
    updating_ = false;
    pull();
}

void LevelsDialog::done(int result)
{
    preview_.clear();
    if (result == QDialog::Accepted)
        preview_.commit(values_);
    else
        preview_.restoreSource();
    QDialog::done(result);
}

} // namespace tessel
