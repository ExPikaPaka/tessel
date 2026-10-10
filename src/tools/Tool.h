#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QCursor>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <QVariant>
#include <QWidget>

#include "core/Document.h"
#include "core/History.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QSlider;
class QSpinBox;
class QToolButton;

namespace tessel {

class ToolOptionsBar;

// Everything a tool may touch, handed to it by the canvas.
struct ToolContext {
    Document* doc = nullptr;
    History* history = nullptr;
    Rgba primary{0, 0, 0, 255};
    Rgba secondary{255, 255, 255, 255};
    double zoom = 1.0;

    // Repaint the given document rectangle; an empty rectangle means all of it.
    std::function<void(const Rect&)> invalidate;
    // The layer stack or history changed and the panels need rebuilding.
    std::function<void()> documentChanged;
    std::function<void(const QString&)> status;
    std::function<void(Rgba, bool)> pickedColor;
    std::function<void(double, QPointF)> zoomTo;
    std::function<void(QPointF)> scrollBy;
    std::function<void(const QString&)> requestTool;

    bool valid() const { return doc != nullptr && history != nullptr; }
    void repaintAll() const
    {
        if (invalidate)
            invalidate(Rect());
    }
    void repaint(const Rect& area) const
    {
        if (invalidate)
            invalidate(area);
    }
    void say(const QString& text) const
    {
        if (status)
            status(text);
    }
};

struct ToolEvent {
    QPointF pos;        // document coordinates, sub pixel
    QPoint pixel;       // floor of pos
    QPointF viewPos;    // widget coordinates
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    // A mouse has no pressure sensor and no tilt, so it reports a full press
    // and a pen held upright. Every tool can therefore read these without
    // asking where the event came from.
    double pressure = 1.0;
    double tiltX = 0.0;   // degrees from vertical, positive to the right
    double tiltY = 0.0;   // degrees from vertical, positive downwards
    bool fromTablet = false;

    bool isSecondary() const
    {
        return button == Qt::RightButton || buttons.testFlag(Qt::RightButton);
    }
};

class Tool {
public:
    virtual ~Tool() = default;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual QString iconName() const { return id(); }
    virtual QString shortcut() const { return QString(); }
    virtual QString hint() const { return QString(); }
    virtual QCursor cursor() const { return QCursor(Qt::CrossCursor); }

    virtual void activated(ToolContext& context) {}
    virtual void deactivated(ToolContext& context) { cancel(context); }

    virtual void press(ToolContext& context, const ToolEvent& event) {}
    virtual void move(ToolContext& context, const ToolEvent& event) {}
    virtual void release(ToolContext& context, const ToolEvent& event) {}
    virtual void doubleClick(ToolContext& context, const ToolEvent& event) {}
    virtual bool keyPress(ToolContext& context, QKeyEvent* event) { return false; }

    // Finish or throw away work in progress (tool change, Enter, Escape).
    virtual void commit(ToolContext& context) {}
    virtual void cancel(ToolContext& context) {}
    virtual bool isBusy() const { return false; }

    // Drawn on top of the canvas. The painter maps document to view already,
    // so use overlayPen() for hairlines that stay one pixel wide.
    virtual void paintOverlay(QPainter& painter, const ToolContext& context) const {}

    virtual void buildOptions(ToolOptionsBar& bar) {}
    virtual void optionChanged(const QString& key, const QVariant& value) {}

    static QPen overlayPen(double zoom, const QColor& color, double widthPixels = 1.0,
                           Qt::PenStyle style = Qt::SolidLine);
};

class ToolRegistry {
public:
    static ToolRegistry& instance();

    void add(std::unique_ptr<Tool> tool);
    const std::vector<Tool*>& all() const { return order_; }
    Tool* byId(const QString& id) const;
    // Tools sharing a shortcut letter, in registration order, so the key
    // cycles through them the way Paint.NET does.
    std::vector<Tool*> sharingShortcut(const QString& key) const;

private:
    std::vector<std::unique_ptr<Tool>> tools_;
    std::vector<Tool*> order_;
    QHash<QString, Tool*> byId_;
};

// Rasterises a path into a coverage mask, used by the selection and shape
// tools. Qt does the scan conversion, including antialiasing.
Mask maskFromPath(const QPainterPath& path, int width, int height,
                  bool antialias = true);

void registerBuiltinTools();
void registerPaintTools(ToolRegistry& registry);
void registerSelectTools(ToolRegistry& registry);
void registerShapeTools(ToolRegistry& registry);
void registerMiscTools(ToolRegistry& registry);
void registerTransformTools(ToolRegistry& registry);
void registerPlaceTool(ToolRegistry& registry);

// Hands a picture to the placement tool, which lets the user position and
// scale it before anything is written. Returns false if there is nothing to
// place. The picture keeps its own size: whatever hangs off the canvas is
// trimmed when the placement is committed, not when it is loaded.
bool startPlacement(ToolContext& context, Image picture, const QString& layerName,
                    bool onNewLayer);

// Custom brush tips. A tip is a PNG whose alpha, or whose luminance when it
// has none, is the shape the brush stamps. The list of tips and the one in use
// outlive the session through QSettings; an empty path means the built in round
// tip, which keeps its own swept distance field code path.
namespace brushtips {

QStringList remembered();
QString chosen();
// Loads the file, keeps it in the list and makes it the current tip. False if
// nothing usable could be read out of it, and then nothing is remembered.
bool remember(const QString& path);
void choose(const QString& path);
void forgetAll();
// Re-reads the settings, which is how a fresh run picks up the last tip.
void reload();
// The stamp itself, trimmed to what it actually marks. Null if unreadable.
Mask load(const QString& path);

} // namespace brushtips

// The row of tool specific controls under the main toolbar.
class ToolOptionsBar : public QWidget {
    Q_OBJECT

public:
    explicit ToolOptionsBar(QWidget* parent = nullptr);

    void startTool(Tool* tool);
    // The drop down at the left of the bar that also picks the active tool.
    void setToolList(const QStringList& toolIds, const QString& currentId);
    void openToolList();
    QVariant value(const QString& key, const QVariant& fallback = QVariant()) const;
    void setValue(const QString& key, const QVariant& value);

    QSpinBox* addSpin(const QString& key, const QString& label, int minimum,
                      int maximum, int value, const QString& suffix = QString());
    // A slider with its value beside it, for the settings that are a
    // percentage of something.
    QSlider* addPercent(const QString& key, const QString& label, int value,
                        int minimum = 1, int maximum = 100);
    QDoubleSpinBox* addDouble(const QString& key, const QString& label, double minimum,
                              double maximum, double value, double step = 0.1,
                              int decimals = 2, const QString& suffix = QString());
    QCheckBox* addCheck(const QString& key, const QString& label, bool checked);
    QComboBox* addCombo(const QString& key, const QString& label,
                        const QStringList& items, int index);
    QToolButton* addToggle(const QString& key, const QString& iconName,
                           const QString& tooltip, bool checked);
    // A small checkable button labelled with a word rather than a glyph, for
    // the switches that have nothing worth drawing an icon for.
    QToolButton* addTextToggle(const QString& key, const QString& text,
                               const QString& tooltip, bool checked);
    QFontComboBox* addFontCombo(const QString& key, const QString& family);
    void addLabel(const QString& text);
    void addSeparator();
    void addStretch();
    void addWidget(QWidget* widget);
    // A set of mutually exclusive icon buttons, such as the selection modes.
    void addToggleGroup(const QString& key, const QString& label,
                        const QList<QPair<QString, QString>>& iconsAndTips,
                        int index);

signals:
    void valueChanged(const QString& key, const QVariant& value);
    void toolPicked(const QString& toolId);

private:
    void clearControls();
    void publish(const QString& key, const QVariant& value);

    QHBoxLayout* layout_ = nullptr;
    class QToolButton* chooser_ = nullptr;
    QLabel* title_ = nullptr;
    QHash<QString, QVariant> values_;
    QString currentToolId_;
    QList<QWidget*> owned_;
};

} // namespace tessel
