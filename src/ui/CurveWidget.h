#pragma once

#include <array>
#include <functional>

#include <QDialog>
#include <QPolygonF>
#include <QWidget>

#include "core/Document.h"
#include "core/History.h"
#include "core/ImageOps.h"
#include "effects/Effect.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace tessel {

// Shared preview plumbing for the two hand built adjustment dialogs.
class EffectPreview {
public:
    EffectPreview(Effect* effect, Document* doc, History* history, Rgba primary,
                  Rgba secondary);
    ~EffectPreview();

    void setRepaintCallback(std::function<void()> callback) { repaint_ = std::move(callback); }
    void update(const ParamValues& values);
    void clear();
    void commit(const ParamValues& values);
    void restoreSource();
    const Image& source() const { return source_; }
    Rect region() const;
    const Mask* clip() const { return doc_->selection().clipMask(); }

private:
    Effect* effect_;
    Document* doc_;
    History* history_;
    Rgba primary_;
    Rgba secondary_;
    Image source_;
    Image preview_;
    std::function<void()> repaint_;
    bool active_ = false;
};

// An editable transfer curve with the channel histogram behind it.
class CurveEditor : public QWidget {
    Q_OBJECT

public:
    explicit CurveEditor(QWidget* parent = nullptr);

    void setPoints(const QPolygonF& points);
    QPolygonF points() const { return points_; }
    void setHistogram(const std::array<uint32_t, 256>& histogram);
    void setCurveColor(const QColor& color);
    void reset();

signals:
    void curveChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override { return QSize(258, 258); }

private:
    QRectF plotRect() const;
    QPointF toWidget(QPointF value) const;
    QPointF toValue(QPointF widget) const;
    int pointAt(QPointF widget) const;
    void sortPoints();

    QPolygonF points_;
    std::array<uint32_t, 256> histogram_{};
    uint32_t histogramPeak_ = 0;
    QColor curveColor_;
    int dragging_ = -1;
};

// Curves, with a transfer map selector and one curve per channel.
class CurvesDialog : public QDialog {
    Q_OBJECT

public:
    CurvesDialog(Effect* effect, Document* doc, History* history, Rgba primary,
                 Rgba secondary, QWidget* parent = nullptr);

    void setRepaintCallback(std::function<void()> callback)
    {
        preview_.setRepaintCallback(std::move(callback));
    }
    ParamValues values() const { return values_; }

protected:
    void done(int result) override;

private:
    void channelChanged(int index);
    void curveEdited();

    Effect* effect_;
    EffectPreview preview_;
    ParamValues values_;
    CurveEditor* editor_ = nullptr;
    QComboBox* channel_ = nullptr;
    ops::Histogram histogram_;
    QPolygonF curves_[4];
    int current_ = 0;
};

// Levels, with the input histogram and the usual five controls.
class LevelsDialog : public QDialog {
    Q_OBJECT

public:
    LevelsDialog(Effect* effect, Document* doc, History* history, Rgba primary,
                 Rgba secondary, QWidget* parent = nullptr);

    void setRepaintCallback(std::function<void()> callback)
    {
        preview_.setRepaintCallback(std::move(callback));
    }
    ParamValues values() const { return values_; }

protected:
    void done(int result) override;

private:
    void pull();
    void autoLevel();

    Effect* effect_;
    EffectPreview preview_;
    ParamValues values_;
    ops::Histogram histogram_;
    QWidget* histogramView_ = nullptr;
    QSpinBox* inBlack_ = nullptr;
    QSpinBox* inWhite_ = nullptr;
    QDoubleSpinBox* gamma_ = nullptr;
    QSpinBox* outBlack_ = nullptr;
    QSpinBox* outWhite_ = nullptr;
    QCheckBox* red_ = nullptr;
    QCheckBox* green_ = nullptr;
    QCheckBox* blue_ = nullptr;
    bool updating_ = false;
};

} // namespace tessel
