#pragma once

#include <functional>
#include <memory>

#include <QDialog>
#include <QHash>
#include <QTimer>

#include "core/Document.h"
#include "core/History.h"
#include "effects/Effect.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QPushButton;
class QSlider;
class QSpinBox;

namespace tessel {

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
    }
    const ParamValues& values() const { return values_; }
    void setValues(const ParamValues& values);

    // Applies an effect with the given values with no dialog at all, which is
    // what the parameterless adjustments and "repeat last effect" use.
    static bool applyDirect(Effect* effect, Document* doc, History* history,
                            const ParamValues& values, Rgba primary, Rgba secondary);

protected:
    void done(int result) override;

private:
    void buildControls();
    void addParamRow(QFormLayout* form, const ParamSpec& spec);
    void schedulePreview();
    void updatePreview();
    void applyToLayer();
    void clearPreview();
    EffectContext makeContext(const Image& source) const;
    static Rect regionFor(const Document& doc);

    Effect* effect_;
    Document* doc_;
    History* history_;
    Rgba primary_;
    Rgba secondary_;
    ParamValues values_;
    ParamValues defaults_;
    Image source_;
    Image preview_;
    QTimer debounce_;
    std::function<void()> repaint_;
    QHash<QString, QWidget*> editors_;
    bool previewActive_ = false;
};

} // namespace tessel
