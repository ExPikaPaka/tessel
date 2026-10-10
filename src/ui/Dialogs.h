#pragma once

#include <functional>

#include <QDialog>
#include <QList>

#include "core/Document.h"
#include "core/History.h"
#include "core/Units.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;
class QSlider;
class QToolButton;

namespace tessel {

// File > New.
class NewImageDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewImageDialog(int defaultWidth, int defaultHeight, Rgba secondary,
                            QWidget* parent = nullptr);

    int imageWidth() const;
    int imageHeight() const;
    Rgba background() const;

private:
    void applyPreset(int index);
    void updateSizeLabel();

    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QComboBox* preset_ = nullptr;
    QComboBox* background_ = nullptr;
    QCheckBox* keepRatio_ = nullptr;
    QLabel* sizeLabel_ = nullptr;
    Rgba secondary_;
    double ratio_ = 1.0;
    bool updating_ = false;
};

// Image > Resize Image, and Image > Canvas Size when `withAnchor` is set.
//
// The dialog keeps the pixel dimensions and the resolution as its state and
// derives whatever the chosen unit shows, so switching units only re-labels
// the same image. Canvas Size gets the unit choice but no resolution field:
// padding a canvas cannot change how densely it prints.
class ResizeDialog : public QDialog {
    Q_OBJECT

public:
    ResizeDialog(const QString& title, int currentWidth, int currentHeight,
                 double currentResolution, Unit unit, bool withAnchor,
                 QWidget* parent = nullptr);

    int newWidth() const { return pixelWidth_; }
    int newHeight() const { return pixelHeight_; }
    double newResolution() const { return resolution_; }
    Anchor anchor() const { return anchor_; }
    bool smoothResampling() const;
    // False when the user cleared "Resample image", in which case the pixel
    // dimensions must be left exactly as they were.
    bool resamples() const;

    // Used by the tests to drive the dialog the way a user would.
    void setUnit(Unit unit);
    Unit unit() const { return unit_; }
    void setDisplayedWidth(double value);
    void setDisplayedHeight(double value);
    void setResolution(double resolution);
    void setResample(bool on);
    double displayedWidth() const;
    double displayedHeight() const;
    QString printSizeText() const;

private:
    void applyUnitToFields();
    void refreshFields();
    void refreshLabels();
    void commitWidth(bool widthLed);

    QComboBox* unitCombo_ = nullptr;
    QDoubleSpinBox* width_ = nullptr;
    QDoubleSpinBox* height_ = nullptr;
    QDoubleSpinBox* resolutionBox_ = nullptr;
    QCheckBox* keepRatio_ = nullptr;
    QCheckBox* resample_ = nullptr;
    QComboBox* resampling_ = nullptr;
    QLabel* printSize_ = nullptr;
    QLabel* summary_ = nullptr;
    QToolButton* anchorButtons_[9] = {};
    Anchor anchor_ = Anchor::Center;
    Unit unit_ = Unit::Pixels;
    int originalWidth_;
    int originalHeight_;
    double originalResolution_;
    int pixelWidth_;
    int pixelHeight_;
    double resolution_;
    bool updating_ = false;
};

// Layers > Layer Properties, with a live preview of the changes.
class LayerPropertiesDialog : public QDialog {
    Q_OBJECT

public:
    LayerPropertiesDialog(Document* doc, History* history, QWidget* parent = nullptr);

    void setRepaintCallback(std::function<void()> callback)
    {
        repaint_ = std::move(callback);
    }

protected:
    void done(int result) override;

private:
    void apply();

    Document* doc_;
    History* history_;
    QLineEdit* name_ = nullptr;
    QCheckBox* visible_ = nullptr;
    QComboBox* blend_ = nullptr;
    QSlider* opacity_ = nullptr;
    QSpinBox* opacityValue_ = nullptr;
    std::function<void()> repaint_;

    QString originalName_;
    bool originalVisible_ = true;
    BlendMode originalMode_ = BlendMode::Normal;
    uint8_t originalOpacity_ = 255;
    int layerUid_ = 0;
};

// Layers > Rotate / Zoom.
class RotateZoomDialog : public QDialog {
    Q_OBJECT

public:
    RotateZoomDialog(Document* doc, History* history, QWidget* parent = nullptr);

    void setRepaintCallback(std::function<void()> callback)
    {
        repaint_ = std::move(callback);
    }

protected:
    void done(int result) override;

private:
    Image render() const;
    void updatePreview();

    Document* doc_;
    History* history_;
    QDoubleSpinBox* angle_ = nullptr;
    QDoubleSpinBox* zoom_ = nullptr;
    QDoubleSpinBox* offsetX_ = nullptr;
    QDoubleSpinBox* offsetY_ = nullptr;
    QCheckBox* tile_ = nullptr;
    Image source_;
    Image preview_;
    int layerUid_ = 0;
    std::function<void()> repaint_;
    bool previewActive_ = false;
};

// Edit > Selection > Feather / Grow / Shrink / Border. One spin box, because
// every one of these operations is driven by a single radius in pixels.
class SelectionOpDialog : public QDialog {
    Q_OBJECT

public:
    enum class Op { Feather, Grow, Shrink, Border };

    SelectionOpDialog(Op op, QWidget* parent = nullptr);

    int amount() const;

    // Edits the selection the way accepting the dialog does, as one undo step.
    static void applyDirect(Op op, Document& doc, History& history, int amount);
    static QString title(Op op);

private:
    Op op_;
    QSpinBox* amount_ = nullptr;
};

// Asked for before writing a lossy format, with a live size estimate.
class SaveOptionsDialog : public QDialog {
    Q_OBJECT

public:
    SaveOptionsDialog(const Image& image, const QString& path, int quality,
                      QWidget* parent = nullptr);

    int quality() const;

private:
    void estimate();

    const Image& image_;
    QString path_;
    QSlider* slider_ = nullptr;
    QSpinBox* value_ = nullptr;
    QLabel* estimate_ = nullptr;
    class QTimer* debounce_ = nullptr;
};

void showAboutDialog(QWidget* parent);

// One row of the keyboard reference. The window builds these from the actions
// it actually installed, so the help can never drift from the bindings again.
struct ShortcutEntry {
    QString group;
    QString name;
    QString keys;
};

void showShortcutsDialog(QWidget* parent, const QList<ShortcutEntry>& entries);

} // namespace tessel
