#pragma once

#include <functional>

#include <QDialog>

#include "core/Document.h"
#include "core/History.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QRadioButton;
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
class ResizeDialog : public QDialog {
    Q_OBJECT

public:
    ResizeDialog(const QString& title, int currentWidth, int currentHeight,
                 bool withAnchor, QWidget* parent = nullptr);

    int newWidth() const;
    int newHeight() const;
    Anchor anchor() const { return anchor_; }
    bool smoothResampling() const;

private:
    void syncFromPercent();
    void syncFromAbsolute(bool widthLed);
    void updateLabel();

    QRadioButton* byPercent_ = nullptr;
    QRadioButton* byAbsolute_ = nullptr;
    QSpinBox* percent_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QCheckBox* keepRatio_ = nullptr;
    QComboBox* resampling_ = nullptr;
    QLabel* summary_ = nullptr;
    QToolButton* anchorButtons_[9] = {};
    Anchor anchor_ = Anchor::Center;
    int originalWidth_;
    int originalHeight_;
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
void showShortcutsDialog(QWidget* parent);

} // namespace tessel
