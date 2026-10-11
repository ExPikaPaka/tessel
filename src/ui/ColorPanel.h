#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

#include "core/Image.h"
#include "core/Palette.h"

class QCheckBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
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

// The palette grid at the bottom of the Colors window. Also used for the row
// of recently used colours, which is the same thing with a single row.
class PaletteGrid : public QWidget {
    Q_OBJECT

public:
    explicit PaletteGrid(QWidget* parent = nullptr);

    void setColors(const QVector<Rgba>& colors);
    const QVector<Rgba>& colors() const { return colors_; }
    static QVector<Rgba> defaultPalette();

    int columns() const { return columns_; }
    void setColumns(int columns);
    // Empty slots are kept visible up to this many rows, so a row that fills
    // up over time does not shift the layout under the pointer.
    void setMinimumRows(int rows);

signals:
    void colorChosen(Rgba color, bool primary);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    int indexAt(QPoint pos) const;
    int cellSize() const;
    int rowCount() const;

    QVector<Rgba> colors_;
    int columns_ = 16;
    int minimumRows_ = 0;
};

// Full screen overlay for the screen wide colour picker. It shows a frozen
// grab of the desktop, follows the pointer, commits on a click and cancels on
// Escape or a right click.
class ScreenPickOverlay : public QWidget {
    Q_OBJECT

public:
    ScreenPickOverlay(const QImage& grab, QRect bounds, QWidget* parent = nullptr);

signals:
    void picked(Rgba color);
    void cancelled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    Rgba colorAt(QPointF pos) const;

    QImage grab_;
    QRect bounds_;
    QPointF cursor_;
    // Grab pixels per logical pixel. The grab is kept at its native
    // resolution so sampling reports the exact pixel under the pointer.
    double ratio_ = 1.0;
};

// The whole Colors window: swatches, wheel, sliders, numeric fields, palette.
class ColorPanel : public QWidget {
    Q_OBJECT

public:
    // The recent row holds this many colours, most recent first.
    static constexpr int kRecentLimit = 16;

    explicit ColorPanel(QWidget* parent = nullptr);
    ~ColorPanel() override;

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

    // Palette files. The buttons wrap these with file dialogs; the work itself
    // takes a path so it can be driven headlessly.
    QVector<Rgba> paletteColors() const;
    bool loadPaletteFile(const QString& path, QString* error = nullptr);
    bool savePaletteFile(const QString& path, QString* error = nullptr) const;
    void resetPalette();

    // Recently used colours, most recent first and without duplicates.
    const QVector<Rgba>& recentColors() const { return recent_; }
    void clearRecentColors();
    void loadRecentColors();
    void saveRecentColors() const;

    // Screen wide picking. beginScreenPick() returns false and fills `error`
    // when the platform will not hand over a screen grab.
    static QRect screenBounds();
    static QImage grabScreens();
    static bool isUsableGrab(const QImage& grab);
    bool beginScreenPick(QString* error = nullptr);
    void cancelScreenPick();
    bool screenPickActive() const { return overlay_ != nullptr; }

signals:
    void primaryChanged(Rgba color);
    void secondaryChanged(Rgba color);

private:
    void applyActive(Rgba color, bool fromFields = false);
    void syncControls();
    void noteUsed(Rgba color);
    void showPaletteError(const QString& title, const QString& message);

    SwatchPair* swatches_ = nullptr;
    ColorWheel* wheel_ = nullptr;
    RampSlider* valueSlider_ = nullptr;
    RampSlider* alphaSlider_ = nullptr;
    PaletteGrid* palette_ = nullptr;
    PaletteGrid* recentGrid_ = nullptr;
    QPushButton* moreButton_ = nullptr;
    QPushButton* pickButton_ = nullptr;
    QPushButton* loadPaletteButton_ = nullptr;
    QPushButton* savePaletteButton_ = nullptr;
    QPushButton* resetPaletteButton_ = nullptr;
    ScreenPickOverlay* overlay_ = nullptr;
    QTimer* recentSaveTimer_ = nullptr;
    QWidget* details_ = nullptr;
    QSpinBox* spinR_ = nullptr;
    QSpinBox* spinG_ = nullptr;
    QSpinBox* spinB_ = nullptr;
    QSpinBox* spinH_ = nullptr;
    QSpinBox* spinS_ = nullptr;
    QSpinBox* spinV_ = nullptr;
    QSpinBox* spinA_ = nullptr;
    QLineEdit* hexField_ = nullptr;

    QVector<Rgba> recent_;
    QString paletteName_;
    Rgba primary_{0, 0, 0, 255};
    Rgba secondary_{255, 255, 255, 255};
    bool activeIsPrimary_ = true;
    bool updating_ = false;
};

} // namespace tessel
