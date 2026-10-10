#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>

#include "core/Document.h"
#include "core/History.h"
#include "effects/Effect.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QMenu;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;

namespace tessel {

// The controls for a list of parameters, generated from the declaration alone:
// one row per spec, in the order the effect declares them. The effect dialog
// and the dialog that edits an effect stored on a layer share it, so a
// parameter type is only ever laid out in one place.
class ParamForm : public QWidget {
    Q_OBJECT

public:
    ParamForm(std::vector<ParamSpec> specs, ParamValues values,
              QWidget* parent = nullptr);

    const ParamValues& values() const { return values_; }
    void setValues(const ParamValues& values);

signals:
    void valuesChanged();

private:
    void addRow(QFormLayout* form, const ParamSpec& spec);

    std::vector<ParamSpec> specs_;
    ParamValues values_;
    QHash<QString, QWidget*> editors_;
};

// Fills `menu` with every adjustment and effect, grouped the way the
// Adjustments and Effects menus group them, and hands whichever one the user
// picks to `chosen`. Used by the New Adjustment Layer submenu and by the Add
// button of a layer's effect stack.
void fillEffectMenu(QMenu* menu, const std::function<void(Effect*)>& chosen);

// One effect computation handed to a worker thread. The job owns everything
// the worker writes, so the result can be dropped without touching the
// document when the job turns out to be stale.
struct EffectJob {
    ParamValues values;
    EffectProgress progress;
    bool draft = false;
    Image result;
    // Set with a release store once `result` is final, read with an acquire
    // load before the GUI thread touches it. Joining the worker already
    // orders the two, but Qt's own locks are opaque to race detectors and
    // this spells the handoff out.
    std::atomic<bool> done{false};
};

// Preview, progress, cancellation and commit plumbing for every effect
// dialog: the generated EffectDialog as well as the hand built Curves and
// Levels dialogs, which used to carry their own copy of this logic.
//
// Effects run on a worker thread against snapshots taken when the session is
// created, so the document is only ever read and written from the GUI thread.
// At most one job runs at a time, and a new request supersedes the running one
// instead of queueing behind it, which is what keeps a dragged slider from
// piling up full resolution recomputes.
class EffectSession : public QObject {
    Q_OBJECT

public:
    EffectSession(Effect* effect, Document* doc, History* history, Rgba primary,
                  Rgba secondary, QObject* parent = nullptr);
    ~EffectSession() override;

    void setRepaintCallback(std::function<void()> callback)
    {
        repaint_ = std::move(callback);
    }

    // Asks for a preview of `values`. A reduced resolution pass lands almost
    // at once, a full resolution one follows when the controls stop moving.
    void requestPreview(const ParamValues& values);
    void clearPreview();
    // Stops whatever is running and forgets what was owed.
    void cancel();
    bool isBusy() const { return running_ != nullptr; }

    // Full resolution apply, recorded in the history. Reuses the settled
    // preview when it already matches `values`, otherwise computes on a worker
    // thread; `uiParent` opts into a cancellable progress dialog. Returns
    // false when nothing was recorded, including when the user stopped it.
    bool commit(const ParamValues& values, QWidget* uiParent = nullptr);
    void restoreSource();

    const Image& source() const { return source_; }
    const Image& previewImage() const { return preview_; }
    Rect region() const { return region_; }
    const Mask* clip() const { return hasClip_ ? &clip_ : nullptr; }

    // Regions larger than this are previewed at reduced resolution first.
    void setDraftPixelBudget(size_t pixels) { draftBudget_ = pixels; }

    // The wait before a full resolution pass, which follows what the last one
    // cost. Exposed so the tests can check the pacing rather than the clock.
    int refineDelayMs() const { return refineTimer_.interval(); }

    // Spins the event loop until nothing is running or owed. Used by the
    // tests, which have no user to wait for.
    bool waitUntilIdle(int timeoutMs = 30000);

signals:
    void progressChanged(double fraction);
    void busyChanged(bool busy);
    // True once the preview on screen is the full resolution result.
    void previewUpdated(bool fullResolution);

private:
    void onDraftTick();
    void onRefineTick();
    void queueJob(bool draft);
    void supersede();
    void pump();
    void jobFinished(std::shared_ptr<EffectJob> job);
    void deliver(EffectJob& job);
    void joinWorker();
    void cancelAndWait();
    bool computeBlocking(const ParamValues& values, QWidget* uiParent, Image& out);
    // Runs on the worker thread. Reads only the snapshots below.
    Image compute(EffectJob& job);
    bool wantsDraft() const;
    Rect draftRegion() const;
    QString iconName() const;

    Effect* effect_;
    Document* doc_;
    History* history_;
    Rgba primary_;
    Rgba secondary_;

    // Snapshots. Nothing may write to these once a job has been started,
    // which is what lets the worker read them without any locking.
    Image source_;
    Mask clip_;
    bool hasClip_ = false;
    Rect region_;
    Image draftSource_; // built by the first draft job, reused afterwards

    Image preview_;     // the buffer the document points at
    Image fullResult_;  // last settled full resolution preview
    ParamValues fullValues_;
    bool haveFull_ = false;

    ParamValues wanted_;
    bool hasNext_ = false;
    bool nextDraft_ = false;
    std::shared_ptr<EffectJob> running_;
    // One worker at a time, owned here so the session can always join it
    // before anything the worker reads goes away. A plain std::thread rather
    // than a pool: the effects fan out over scanlines internally, so a pool
    // would only add a queue in front of a single long task.
    std::thread worker_;

    QTimer draftTimer_;
    QTimer refineTimer_;
    QTimer pollTimer_;
    QElapsedTimer jobClock_;
    // How long the last full resolution pass took, so a cheap effect stops
    // paying for the reduced pass it does not need. Negative until measured.
    double lastFullMs_ = -1.0;

    std::function<void()> repaint_;
    // Half a megapixel keeps even the slowest effect's draft under about a
    // tenth of a second on a 20 megapixel canvas, which is what makes a drag
    // feel like it is following the slider.
    size_t draftBudget_ = size_t(1) << 19;
    bool previewActive_ = false;
    bool blocking_ = false;
};

// Builds a dialog from an effect's parameter list, shows a live preview on the
// canvas, and writes the result into the layer when accepted.
class EffectDialog : public QDialog {
    Q_OBJECT

public:
    EffectDialog(Effect* effect, Document* doc, History* history, Rgba primary,
                 Rgba secondary, QWidget* parent = nullptr);
    ~EffectDialog() override;

    void setRepaintCallback(std::function<void()> callback)
    {
        repaint_ = std::move(callback);
        session_.setRepaintCallback(repaint_);
    }
    const ParamValues& values() const { return values_; }
    void setValues(const ParamValues& values);
    EffectSession& session() { return session_; }

    // Applies an effect with the given values with no dialog at all, which is
    // what the parameterless adjustments and "repeat last effect" use. Pass a
    // parent widget to get a progress dialog for the slow ones.
    static bool applyDirect(Effect* effect, Document* doc, History* history,
                            const ParamValues& values, Rgba primary, Rgba secondary,
                            QWidget* uiParent = nullptr);

protected:
    void done(int result) override;

private:
    void buildControls();
    void schedulePreview();

    Effect* effect_;
    Document* doc_;
    ParamValues values_;
    ParamValues defaults_;
    EffectSession session_;
    std::function<void()> repaint_;
    ParamForm* form_ = nullptr;
    QProgressBar* progress_ = nullptr;
    // Holds the bar back until a pass is slow enough to be worth reporting,
    // so a quick draft does not make the dialog flicker.
    QTimer progressDelay_;
    bool finishing_ = false;
};

// Edits the parameters of an effect a layer stores rather than has applied to
// its pixels: an adjustment layer, or one entry of a layer's effect stack.
//
// There is no result to compute and nothing to write into a layer here. The
// values go straight into the entry and the document composites itself with
// them, which is both the preview and the final answer, so the dialog can be
// reopened on the same entry for as long as the entry exists. Cancel puts the
// values back; OK records the change as one undo step.
class StoredEffectDialog : public QDialog {
    Q_OBJECT

public:
    StoredEffectDialog(Effect* effect, Document* doc, History* history, Layer* layer,
                       int entryIndex, QWidget* parent = nullptr);

    void setRepaintCallback(std::function<void()> callback)
    {
        repaint_ = std::move(callback);
    }
    const ParamValues& values() const;
    // The name the undo step gets, which is what the Layers panel calls the
    // entry as well.
    QString effectName() const { return effectName_; }

protected:
    void done(int result) override;

private:
    // Null once the layer or the entry has gone, which can happen while the
    // dialog is open because it is not modal.
    LayerEffect* entry() const;
    void apply();

    Effect* effect_;
    Document* doc_;
    History* history_;
    int layerUid_;
    int entryIndex_;
    QString effectName_;
    ParamForm* form_ = nullptr;
    ParamValues original_;
    std::function<void()> repaint_;
    bool finishing_ = false;
};

} // namespace tessel
