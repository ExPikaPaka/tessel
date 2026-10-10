#include "ui/EffectDialog.h"

#include <cmath>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDial>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include "core/ImageOps.h"
#include "ui/Icons.h"

namespace tessel {

namespace {

// A slider paired with a spin box, the control style these dialogs use.
template <class Spin>
QWidget* sliderRow(QWidget* parent, Spin* spin, QSlider* slider)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(slider, 1);
    layout->addWidget(spin, 0);
    return row;
}

} // namespace

EffectDialog::EffectDialog(Effect* effect, Document* doc, History* history, Rgba primary,
                           Rgba secondary, QWidget* parent)
    : QDialog(parent), effect_(effect), doc_(doc), history_(history), primary_(primary),
      secondary_(secondary)
{
    setWindowTitle(effect_->name());
    setWindowIcon(icons::get(effect_->isAdjustment() ? QStringLiteral("adjust")
                                                     : QStringLiteral("effect"), 22));
    setModal(false);
    defaults_ = ParamValues::defaults(effect_->params());
    values_ = defaults_;
    if (const Layer* layer = doc_->activeLayer())
        source_ = layer->image;

    debounce_.setSingleShot(true);
    debounce_.setInterval(40);
    connect(&debounce_, &QTimer::timeout, this, &EffectDialog::updatePreview);

    buildControls();
    schedulePreview();
}

EffectDialog::~EffectDialog() { clearPreview(); }

void EffectDialog::buildControls()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    for (const ParamSpec& spec : effect_->params()) {
        if (spec.type == ParamType::Curve)
            continue; // curves have their own dialog
        addParamRow(form, spec);
    }
    root->addLayout(form);

    if (effect_->params().empty())
        root->addWidget(new QLabel(QStringLiteral("This effect has no settings."), this));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                             | QDialogButtonBox::Reset,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this,
            [this] { setValues(defaults_); });
    root->addWidget(buttons);
    setMinimumWidth(360);
}

void EffectDialog::addParamRow(QFormLayout* form, const ParamSpec& spec)
{
    const QString key = spec.id;
    switch (spec.type) {
    case ParamType::Int: {
        auto* spin = new QSpinBox(this);
        spin->setRange(int(spec.minimum), int(spec.maximum));
        spin->setValue(values_.intValue(key, int(spec.defaultValue)));
        spin->setSuffix(spec.suffix);
        spin->setKeyboardTracking(false);
        auto* slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(int(spec.minimum), int(spec.maximum));
        slider->setValue(spin->value());
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, &QSpinBox::valueChanged, this, [this, slider, key](int value) {
            if (slider->value() != value)
                slider->setValue(value);
            values_.set(key, value);
            schedulePreview();
        });
        editors_.insert(key, spin);
        form->addRow(spec.label, sliderRow(this, spin, slider));
        break;
    }
    case ParamType::Double:
    case ParamType::Angle: {
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(spec.minimum, spec.maximum);
        spin->setDecimals(spec.decimals);
        spin->setSingleStep(spec.step);
        spin->setValue(values_.doubleValue(key, spec.defaultValue));
        spin->setSuffix(spec.suffix);
        spin->setKeyboardTracking(false);
        auto* slider = new QSlider(Qt::Horizontal, this);
        const double scale = 100.0;
        slider->setRange(int(spec.minimum * scale), int(spec.maximum * scale));
        slider->setValue(int(spin->value() * scale));
        connect(slider, &QSlider::valueChanged, this, [spin, scale](int value) {
            const double next = value / scale;
            if (std::abs(spin->value() - next) > 1.0e-9)
                spin->setValue(next);
        });
        connect(spin, &QDoubleSpinBox::valueChanged, this,
                [this, slider, scale, key](double value) {
                    const int ticks = int(std::lround(value * scale));
                    if (slider->value() != ticks)
                        slider->setValue(ticks);
                    values_.set(key, value);
                    schedulePreview();
                });
        editors_.insert(key, spin);
        form->addRow(spec.label, sliderRow(this, spin, slider));
        break;
    }
    case ParamType::Bool: {
        auto* box = new QCheckBox(this);
        box->setChecked(values_.boolValue(key, spec.defaultValue != 0.0));
        connect(box, &QCheckBox::toggled, this, [this, key](bool value) {
            values_.set(key, value);
            schedulePreview();
        });
        editors_.insert(key, box);
        form->addRow(spec.label, box);
        break;
    }
    case ParamType::Choice: {
        auto* combo = new QComboBox(this);
        combo->addItems(spec.choices);
        combo->setCurrentIndex(values_.choiceValue(key, int(spec.defaultValue)));
        connect(combo, &QComboBox::currentIndexChanged, this, [this, key](int index) {
            values_.set(key, index);
            schedulePreview();
        });
        editors_.insert(key, combo);
        form->addRow(spec.label, combo);
        break;
    }
    case ParamType::Color: {
        auto* button = new QPushButton(this);
        button->setMinimumHeight(24);
        const Rgba initial = values_.colorValue(key, spec.color);
        auto paintButton = [button](Rgba color) {
            button->setIcon(icons::colorSwatch(color, 18));
            button->setText(QStringLiteral("#%1%2%3")
                                .arg(color.r, 2, 16, QLatin1Char('0'))
                                .arg(color.g, 2, 16, QLatin1Char('0'))
                                .arg(color.b, 2, 16, QLatin1Char('0'))
                                .toUpper());
        };
        paintButton(initial);
        connect(button, &QPushButton::clicked, this, [this, key, button, paintButton] {
            const Rgba current = values_.colorValue(key);
            const QColor chosen = QColorDialog::getColor(
                current.toQColor(), this, QStringLiteral("Select colour"),
                QColorDialog::ShowAlphaChannel);
            if (!chosen.isValid())
                return;
            values_.set(key, chosen);
            paintButton(Rgba::fromQColor(chosen));
            schedulePreview();
        });
        editors_.insert(key, button);
        form->addRow(spec.label, button);
        break;
    }
    case ParamType::Offset: {
        auto* row = new QWidget(this);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        const QPointF initial = values_.pointValue(key, QPointF(spec.defaultValue,
                                                               spec.defaultValue2));
        auto* spinX = new QDoubleSpinBox(row);
        auto* spinY = new QDoubleSpinBox(row);
        for (QDoubleSpinBox* spin : {spinX, spinY}) {
            spin->setRange(-1.0, 1.0);
            spin->setSingleStep(0.02);
            spin->setDecimals(3);
            spin->setKeyboardTracking(false);
        }
        spinX->setValue(initial.x());
        spinY->setValue(initial.y());
        const auto update = [this, key, spinX, spinY] {
            values_.set(key, QPointF(spinX->value(), spinY->value()));
            schedulePreview();
        };
        connect(spinX, &QDoubleSpinBox::valueChanged, this, update);
        connect(spinY, &QDoubleSpinBox::valueChanged, this, update);
        layout->addWidget(new QLabel(QStringLiteral("X"), row));
        layout->addWidget(spinX, 1);
        layout->addWidget(new QLabel(QStringLiteral("Y"), row));
        layout->addWidget(spinY, 1);
        editors_.insert(key, row);
        form->addRow(spec.label, row);
        break;
    }
    default:
        break;
    }
}

void EffectDialog::setValues(const ParamValues& values)
{
    values_ = values;
    for (const ParamSpec& spec : effect_->params()) {
        QWidget* editor = editors_.value(spec.id);
        if (!editor)
            continue;
        if (auto* spin = qobject_cast<QSpinBox*>(editor))
            spin->setValue(values_.intValue(spec.id, int(spec.defaultValue)));
        else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(editor))
            dspin->setValue(values_.doubleValue(spec.id, spec.defaultValue));
        else if (auto* box = qobject_cast<QCheckBox*>(editor))
            box->setChecked(values_.boolValue(spec.id, spec.defaultValue != 0.0));
        else if (auto* combo = qobject_cast<QComboBox*>(editor))
            combo->setCurrentIndex(values_.choiceValue(spec.id, int(spec.defaultValue)));
    }
    schedulePreview();
}

Rect EffectDialog::regionFor(const Document& doc)
{
    const Selection& selection = doc.selection();
    if (!selection.isActive())
        return doc.rect();
    const Rect bounds = selection.bounds();
    return bounds.isEmpty() ? doc.rect() : bounds;
}

EffectContext EffectDialog::makeContext(const Image& source) const
{
    EffectContext context;
    context.source = &source;
    context.clip = doc_->selection().clipMask();
    context.region = regionFor(*doc_);
    context.primary = primary_;
    context.secondary = secondary_;
    context.document = doc_;
    return context;
}

void EffectDialog::schedulePreview() { debounce_.start(); }

void EffectDialog::updatePreview()
{
    const Layer* layer = doc_->activeLayer();
    if (!layer || source_.isNull())
        return;
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    const EffectContext context = makeContext(source_);
    const Image result = effect_->apply(context, values_);
    preview_ = source_;
    ops::mixThroughMask(preview_, result, context.clip, context.region);
    doc_->setPreview(layer->uid(), &preview_);
    previewActive_ = true;
    QGuiApplication::restoreOverrideCursor();
    if (repaint_)
        repaint_();
}

void EffectDialog::clearPreview()
{
    if (!previewActive_)
        return;
    previewActive_ = false;
    doc_->clearPreview();
    if (repaint_)
        repaint_();
}

void EffectDialog::applyToLayer()
{
    Layer* layer = doc_->activeLayer();
    if (!layer || source_.isNull())
        return;
    const EffectContext context = makeContext(source_);
    const Image result = effect_->apply(context, values_);
    layer->image = source_;
    EditScope scope(*doc_, *history_, layer, effect_->name(),
                    effect_->isAdjustment() ? QStringLiteral("adjust")
                                            : QStringLiteral("effect"),
                    context.region);
    ops::mixThroughMask(layer->image, result, context.clip, context.region);
    scope.commit();
    doc_->invalidateLayer(layer);
}

void EffectDialog::done(int result)
{
    clearPreview();
    if (result == QDialog::Accepted)
        applyToLayer();
    else if (Layer* layer = doc_->activeLayer(); layer && !source_.isNull())
        layer->image = source_;
    doc_->invalidate();
    if (repaint_)
        repaint_();
    QDialog::done(result);
}

bool EffectDialog::applyDirect(Effect* effect, Document* doc, History* history,
                               const ParamValues& values, Rgba primary, Rgba secondary)
{
    Layer* layer = doc->activeLayer();
    if (!effect || !layer)
        return false;
    const Image source = layer->image;
    EffectContext context;
    context.source = &source;
    context.clip = doc->selection().clipMask();
    context.region = regionFor(*doc);
    context.primary = primary;
    context.secondary = secondary;
    context.document = doc;
    const Image result = effect->apply(context, values);
    EditScope scope(*doc, *history, layer, effect->name(),
                    effect->isAdjustment() ? QStringLiteral("adjust")
                                           : QStringLiteral("effect"),
                    context.region);
    ops::mixThroughMask(layer->image, result, context.clip, context.region);
    const bool changed = scope.commit();
    doc->invalidateLayer(layer);
    return changed;
}

} // namespace tessel
