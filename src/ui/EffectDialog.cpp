#include "ui/EffectDialog.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDial>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QThread>
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

// How long a slider has to stand still before the draft and the full
// resolution passes go out. The draft timer is a throttle rather than a
// debounce, so a slow drag keeps getting fresh previews instead of waiting for
// the user to stop.
constexpr int kDraftIntervalMs = 40;
// The wait before the full resolution pass is there to keep an expensive
// effect from being started over at every pixel of a drag. A fixed quarter of
// a second spent that budget on effects that do not need it, and the sharp
// image arriving a beat after the drag stopped was the one thing that still
// felt slow. The wait now follows what the last full pass actually cost, so a
// cheap effect sharpens up as fast as the draft and only a genuinely heavy one
// keeps the long leash.
constexpr int kRefineMinMs = 60;
constexpr int kRefineMaxMs = 250;

// A full resolution pass this quick already feels immediate, so there is
// nothing to gain from resampling the image twice to draft it.
constexpr double kDraftWorthwhileMs = 150.0;

Rect effectRegion(const Document& doc)
{
    const Selection& selection = doc.selection();
    if (!selection.isActive())
        return doc.rect();
    const Rect bounds = selection.bounds();
    return bounds.isEmpty() ? doc.rect() : bounds;
}

} // namespace

// ----------------------------------------------------------------- session

EffectSession::EffectSession(Effect* effect, Document* doc, History* history,
                             Rgba primary, Rgba secondary, QObject* parent)
    : QObject(parent), effect_(effect), doc_(doc), history_(history), primary_(primary),
      secondary_(secondary)
{
    if (const Layer* layer = doc_->activeLayer())
        source_ = layer->image;
    region_ = effectRegion(*doc_);
    // The selection is copied rather than pointed at: the worker thread reads
    // it while the dialog is open, and the dialog is not modal.
    if (const Mask* mask = doc_->selection().clipMask()) {
        clip_ = *mask;
        hasClip_ = true;
    }

    draftTimer_.setSingleShot(true);
    draftTimer_.setInterval(kDraftIntervalMs);
    connect(&draftTimer_, &QTimer::timeout, this, &EffectSession::onDraftTick);

    refineTimer_.setSingleShot(true);
    refineTimer_.setInterval(kRefineMaxMs);
    connect(&refineTimer_, &QTimer::timeout, this, &EffectSession::onRefineTick);

    pollTimer_.setInterval(60);
    connect(&pollTimer_, &QTimer::timeout, this, [this] {
        if (running_)
            emit progressChanged(running_->progress.fraction());
    });
}

EffectSession::~EffectSession()
{
    cancelAndWait();
    // The document must not be left pointing at our preview buffer.
    if (previewActive_) {
        previewActive_ = false;
        doc_->clearPreview();
    }
}

QString EffectSession::iconName() const
{
    return effect_->isAdjustment() ? QStringLiteral("adjust") : QStringLiteral("effect");
}

bool EffectSession::wantsDraft() const
{
    if (lastFullMs_ >= 0.0 && lastFullMs_ < kDraftWorthwhileMs)
        return false;
    // The cost of an effect follows the region it has to fill, so that is what
    // decides whether a reduced pass is worth the extra resampling.
    const Rect area = region_.intersected(source_.rect());
    return size_t(area.w) * size_t(area.h) > draftBudget_;
}

Rect EffectSession::draftRegion() const
{
    const double sx = double(draftSource_.width()) / std::max(1, source_.width());
    const double sy = double(draftSource_.height()) / std::max(1, source_.height());
    const int x0 = int(std::floor(region_.x * sx));
    const int y0 = int(std::floor(region_.y * sy));
    const int x1 = int(std::ceil(region_.right() * sx));
    const int y1 = int(std::ceil(region_.bottom() * sy));
    return Rect(x0, y0, x1 - x0, y1 - y0).intersected(draftSource_.rect());
}

void EffectSession::requestPreview(const ParamValues& values)
{
    if (blocking_)
        return;
    wanted_ = values;
    supersede();
    // Throttle: keep the existing countdown if one is already running so a
    // continuous drag still gets a steady stream of draft previews.
    if (!draftTimer_.isActive())
        draftTimer_.start();
    refineTimer_.start();
}

void EffectSession::onDraftTick() { queueJob(wantsDraft()); }

void EffectSession::onRefineTick() { queueJob(false); }

void EffectSession::queueJob(bool draft)
{
    hasNext_ = true;
    nextDraft_ = draft;
    pump();
}

// Stops work that has become stale, rather than letting the new values queue
// up behind it. A full resolution pass is unbounded and goes at once. A draft
// is deliberately bounded by the draft budget, and letting it land puts a
// frame on the canvas now instead of after the drag ends, so it is left alone
// even when its values are already one step behind.
void EffectSession::supersede()
{
    if (running_ && !running_->draft && running_->values != wanted_)
        running_->progress.cancel();
}

void EffectSession::pump()
{
    if (blocking_)
        return;
    if (running_) {
        // pump() runs again as soon as whatever is running has stopped.
        supersede();
        return;
    }
    if (!hasNext_)
        return;
    hasNext_ = false;

    if (!nextDraft_ && haveFull_ && fullValues_ == wanted_) {
        // Already on screen at full resolution, nothing to compute.
        return;
    }
    if (source_.isNull() || !doc_->activeLayer())
        return;

    joinWorker();
    auto job = std::make_shared<EffectJob>();
    job->values = wanted_;
    job->draft = nextDraft_;
    running_ = job;
    emit busyChanged(true);
    emit progressChanged(0.0);
    pollTimer_.start();
    jobClock_.start();
    worker_ = std::thread([this, job] {
        job->result = compute(*job);
        job->done.store(true, std::memory_order_release);
        // Hop back to the GUI thread. The session outlives this call because
        // its destructor cancels the job and joins the worker.
        QMetaObject::invokeMethod(this, [this, job] { jobFinished(job); },
                                  Qt::QueuedConnection);
    });
}

void EffectSession::joinWorker()
{
    if (worker_.joinable())
        worker_.join();
}

Image EffectSession::compute(EffectJob& job)
{
    if (job.draft && draftSource_.isNull()) {
        // Only one job runs at a time, so the lazily built reduced copy needs
        // no locking.
        const Rect area = region_.intersected(source_.rect());
        const double pixels = double(area.w) * double(area.h);
        const double scale = std::sqrt(double(draftBudget_) / std::max(1.0, pixels));
        const int w = std::max(1, int(source_.width() * std::min(1.0, scale)));
        const int h = std::max(1, int(source_.height() * std::min(1.0, scale)));
        draftSource_ = ops::resize(source_, w, h, true);
    }
    const bool draft = job.draft && !draftSource_.isNull();
    const Image& src = draft ? draftSource_ : source_;

    EffectContext context;
    context.source = &src;
    // The draft runs unclipped and is mixed through the real mask at full
    // resolution below, so the selection edge stays exact even in a draft.
    context.clip = draft ? nullptr : clip();
    context.region = draft ? draftRegion() : region_;
    context.primary = primary_;
    context.secondary = secondary_;
    context.document = doc_;
    context.progress = &job.progress;

    Image result = effect_->apply(context, job.values);
    if (job.progress.isCancelled())
        return Image();
    if (draft)
        result = ops::resize(result, source_.width(), source_.height(), true);
    Image out = source_;
    ops::mixThroughMask(out, result, clip(), region_);
    return out;
}

void EffectSession::jobFinished(std::shared_ptr<EffectJob> job)
{
    if (job != running_)
        return; // a stale completion, its result is dropped with the job
    // Pairs with the worker's release store, which it made before posting
    // this call, so the result below is fully published.
    const bool finished = job->done.load(std::memory_order_acquire);
    running_.reset();
    pollTimer_.stop();
    if (finished && !job->progress.isCancelled()) {
        if (!job->draft) {
            lastFullMs_ = double(jobClock_.nsecsElapsed()) / 1.0e6;
            // Wait about as long as the pass costs: that is enough to stop a
            // drag from queueing a pass per pixel, and no longer.
            refineTimer_.setInterval(std::clamp(int(lastFullMs_ + 0.5),
                                                kRefineMinMs, kRefineMaxMs));
        }
        deliver(*job);
        emit progressChanged(1.0);
    }
    emit busyChanged(false);
    pump();
}

void EffectSession::deliver(EffectJob& job)
{
    const Layer* layer = doc_->activeLayer();
    if (!layer)
        return;
    preview_ = std::move(job.result);
    if (!job.draft) {
        fullResult_ = preview_;
        fullValues_ = job.values;
        haveFull_ = true;
    }
    doc_->setPreview(layer->uid(), &preview_);
    previewActive_ = true;
    emit previewUpdated(!job.draft);
    if (repaint_)
        repaint_();
}

void EffectSession::cancel()
{
    draftTimer_.stop();
    refineTimer_.stop();
    hasNext_ = false;
    cancelAndWait();
}

void EffectSession::cancelAndWait()
{
    if (running_)
        running_->progress.cancel();
    // An effect that ignores the progress channel cannot be stopped early, so
    // this join can take as long as one of its passes; it is never longer than
    // the single threaded code used to block for.
    joinWorker();
    running_.reset();
    pollTimer_.stop();
}

void EffectSession::clearPreview()
{
    if (!previewActive_)
        return;
    previewActive_ = false;
    doc_->clearPreview();
    if (repaint_)
        repaint_();
}

void EffectSession::restoreSource()
{
    if (Layer* layer = doc_->activeLayer(); layer && !source_.isNull())
        layer->image = source_;
    doc_->invalidate();
}

bool EffectSession::computeBlocking(const ParamValues& values, QWidget* uiParent,
                                    Image& out)
{
    auto job = std::make_shared<EffectJob>();
    job->values = values;
    blocking_ = true;
    std::thread worker([this, job] {
        job->result = compute(*job);
        job->done.store(true, std::memory_order_release);
    });
    if (uiParent) {
        QProgressDialog box(QStringLiteral("Applying %1...").arg(effect_->name()),
                            QStringLiteral("Cancel"), 0, 1000, uiParent);
        box.setWindowTitle(effect_->name());
        // Application modal on purpose: the loop below keeps delivering events
        // so the window carries on painting, and nothing else may edit the
        // document while the worker is reading a snapshot of it.
        box.setWindowModality(Qt::ApplicationModal);
        box.setMinimumDuration(400);
        box.setAutoClose(false);
        box.setAutoReset(false);
        while (!job->done.load(std::memory_order_acquire)) {
            box.setValue(int(job->progress.fraction() * 1000.0));
            if (box.wasCanceled())
                job->progress.cancel();
            // Keep painting and keep the Cancel button alive while we wait.
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    }
    worker.join();
    const bool finished = job->done.load(std::memory_order_acquire);
    blocking_ = false;
    if (!finished || job->progress.isCancelled())
        return false;
    out = std::move(job->result);
    return true;
}

bool EffectSession::commit(const ParamValues& values, QWidget* uiParent)
{
    Layer* layer = doc_->activeLayer();
    if (!layer || source_.isNull())
        return false;
    draftTimer_.stop();
    refineTimer_.stop();
    hasNext_ = false;
    // Nothing may still be in flight: a late preview would otherwise point the
    // document at a stale buffer after the layer has been written.
    cancelAndWait();

    Image committed;
    if (haveFull_ && fullValues_ == values) {
        // The settled preview already is the full resolution answer.
        committed = fullResult_;
    } else {
        if (!computeBlocking(values, uiParent, committed))
            return false; // stopped by the user, so nothing is recorded
    }

    layer->image = source_;
    EditScope scope(*doc_, *history_, layer, effect_->name(), iconName(), region_);
    // `committed` is the source with the result mixed in through the mask, so
    // this writes exactly the pixels the single threaded path produced.
    layer->image = std::move(committed);
    const bool changed = scope.commit();
    doc_->invalidateLayer(layer);
    return changed;
}

bool EffectSession::waitUntilIdle(int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (!running_ && !hasNext_ && !draftTimer_.isActive() && !refineTimer_.isActive())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return false;
}

// ----------------------------------------------------------------- dialog

EffectDialog::EffectDialog(Effect* effect, Document* doc, History* history, Rgba primary,
                           Rgba secondary, QWidget* parent)
    : QDialog(parent), effect_(effect), doc_(doc),
      session_(effect, doc, history, primary, secondary)
{
    setWindowTitle(effect_->name());
    setWindowIcon(icons::get(effect_->isAdjustment() ? QStringLiteral("adjust")
                                                     : QStringLiteral("effect"), 22));
    setModal(false);
    defaults_ = ParamValues::defaults(effect_->params());
    values_ = defaults_;

    buildControls();
    progressDelay_.setSingleShot(true);
    progressDelay_.setInterval(300);
    connect(&progressDelay_, &QTimer::timeout, this, [this] {
        if (progress_ && session_.isBusy())
            progress_->setVisible(true);
    });
    connect(&session_, &EffectSession::busyChanged, this, [this](bool busy) {
        if (busy) {
            progressDelay_.start();
            return;
        }
        progressDelay_.stop();
        if (progress_)
            progress_->setVisible(false);
    });
    connect(&session_, &EffectSession::progressChanged, this, [this](double fraction) {
        if (progress_)
            progress_->setValue(int(fraction * 1000.0));
    });
    schedulePreview();
}

EffectDialog::~EffectDialog() = default;

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

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1000);
    progress_->setTextVisible(false);
    progress_->setMaximumHeight(6);
    progress_->setVisible(false);
    root->addWidget(progress_);

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

void EffectDialog::schedulePreview() { session_.requestPreview(values_); }

void EffectDialog::done(int result)
{
    if (finishing_) {
        QDialog::done(result);
        return;
    }
    finishing_ = true;
    // Stop the preview first: Cancel has to give up the work, not finish it.
    session_.cancel();
    session_.clearPreview();
    if (result == QDialog::Accepted)
        session_.commit(values_, this);
    else
        session_.restoreSource();
    doc_->invalidate();
    if (repaint_)
        repaint_();
    QDialog::done(result);
}

bool EffectDialog::applyDirect(Effect* effect, Document* doc, History* history,
                               const ParamValues& values, Rgba primary, Rgba secondary,
                               QWidget* uiParent)
{
    if (!effect || !doc->activeLayer())
        return false;
    EffectSession session(effect, doc, history, primary, secondary);
    return session.commit(values, uiParent);
}

} // namespace tessel
