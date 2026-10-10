#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

#include "core/Image.h"

class QCheckBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QWidget;

namespace tessel {

// Hue and saturation disc. Value is applied for display only.
class ColorWheel : public QWidget {
    Q_OBJECT

public:
    explicit ColorWheel(QWidget* parent = nullptr);

    void setColor(Rgba color);
    Rgba color() const { return color_; }

signals:
    void colorChanged(Rgba color);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    QSize sizeHint() const override { return QSize(150, 150); }

private:
    void rebuild();
    void pick(QPointF pos);

    QImage disc_;
    Rgba color_{255, 0, 0, 255};
    float hue_ = 0.0f;
    float saturation_ = 1.0f;
    float value_ = 1.0f;
};

// A slider painted with an arbitrary colour ramp, used for value and alpha.
class RampSlider : public QWidget {
    Q_OBJECT

public:
    enum Kind { Value, Alpha };

    explicit RampSlider(Kind kind, QWidget* parent = nullptr);

    void setColor(Rgba color);
    double position() const { return position_; }
    void setPosition(double position);

signals:
    void positionChanged(double position);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    QSize sizeHint() const override { return QSize(150, 18); }

private:
    void pick(QPointF pos);

    Kind kind_;
    Rgba color_{255, 0, 0, 255};
    double position_ = 1.0;
};

// The overlapping primary / secondary swatches.
class SwatchPair : public QWidget {
    Q_OBJECT

public:
    explicit SwatchPair(QWidget* parent = nullptr);

    void setColors(Rgba primary, Rgba secondary);
    void setActiveIsPrimary(bool primary);
    bool activeIsPrimary() const { return activeIsPrimary_; }

signals:
    void activeChanged(bool primary);
    void swapRequested();
    void resetRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override { return QSize(74, 58); }

private:
    QRectF primaryRect() const;
    QRectF secondaryRect() const;
    QRectF swapRect() const;
    QRectF resetRect() const;

    Rgba primary_{0, 0, 0, 255};
    Rgba secondary_{255, 255, 255, 255};
    bool activeIsPrimary_ = true;
};

// The palette grid at the bottom of the Colors window.
class PaletteGrid : public QWidget {
    Q_OBJECT

public:
    explicit PaletteGrid(QWidget* parent = nullptr);

    void setColors(const QVector<Rgba>& colors);
    const QVector<Rgba>& colors() const { return colors_; }
    static QVector<Rgba> defaultPalette();

signals:
    void colorChosen(Rgba color, bool primary);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    int indexAt(QPoint pos) const;
    int columns() const { return 16; }
    int cellSize() const;

    QVector<Rgba> colors_;
};

// The whole Colors window: swatches, wheel, sliders, numeric fields, palette.
class ColorPanel : public QWidget {
    Q_OBJECT

public:
    explicit ColorPanel(QWidget* parent = nullptr);

    Rgba primary() const { return primary_; }
    Rgba secondary() const { return secondary_; }
    void setPrimary(Rgba color);
    void setSecondary(Rgba color);
    void setActiveColor(Rgba color);
    Rgba activeColor() const { return activeIsPrimary_ ? primary_ : secondary_; }
    void swapColors();
    void resetColors();
    bool detailsVisible() const;
    void setDetailsVisible(bool visible);

signals:
    void primaryChanged(Rgba color);
    void secondaryChanged(Rgba color);

private:
    void applyActive(Rgba color, bool fromFields = false);
    void syncControls();

    SwatchPair* swatches_ = nullptr;
    ColorWheel* wheel_ = nullptr;
    RampSlider* valueSlider_ = nullptr;
    RampSlider* alphaSlider_ = nullptr;
    PaletteGrid* palette_ = nullptr;
    QPushButton* moreButton_ = nullptr;
    QWidget* details_ = nullptr;
    QSpinBox* spinR_ = nullptr;
    QSpinBox* spinG_ = nullptr;
    QSpinBox* spinB_ = nullptr;
    QSpinBox* spinH_ = nullptr;
    QSpinBox* spinS_ = nullptr;
    QSpinBox* spinV_ = nullptr;
    QSpinBox* spinA_ = nullptr;
    QLineEdit* hexField_ = nullptr;

    Rgba primary_{0, 0, 0, 255};
    Rgba secondary_{255, 255, 255, 255};
    bool activeIsPrimary_ = true;
    bool updating_ = false;
};

} // namespace tessel
