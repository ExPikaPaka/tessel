#include "tools/Tool.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFrame>
#include <QMenu>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QImage>
#include <QToolButton>

#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tessel {

namespace {
// The tool icon and name stay in place; the rest of the bar is per tool.
constexpr int kFixedItems = 2;
} // namespace

QPen Tool::overlayPen(double zoom, const QColor& color, double widthPixels,
                      Qt::PenStyle style)
{
    QPen pen(color);
    pen.setWidthF(widthPixels / std::max(0.01, zoom));
    pen.setStyle(style);
    pen.setCosmetic(false);
    if (style != Qt::SolidLine) {
        // Keep dash lengths constant on screen whatever the zoom.
        pen.setDashPattern({4.0 / std::max(0.01, zoom) / pen.widthF(),
                            4.0 / std::max(0.01, zoom) / pen.widthF()});
    }
    return pen;
}

Mask maskFromPath(const QPainterPath& path, int width, int height, bool antialias)
{
    Mask mask(width, height);
    if (width <= 0 || height <= 0 || path.isEmpty())
        return mask;
    QImage canvas(width, height, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, antialias);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPath(path);
    }
    for (int y = 0; y < height; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(canvas.constScanLine(y));
        float* dst = mask.row(y);
        for (int x = 0; x < width; ++x)
            dst[x] = qAlpha(line[x]) * (1.0f / 255.0f);
    }
    return mask;
}

ToolRegistry& ToolRegistry::instance()
{
    static ToolRegistry registry;
    return registry;
}

void ToolRegistry::add(std::unique_ptr<Tool> tool)
{
    if (!tool)
        return;
    Tool* raw = tool.get();
    if (byId_.contains(raw->id()))
        return;
    byId_.insert(raw->id(), raw);
    order_.push_back(raw);
    tools_.push_back(std::move(tool));
}

Tool* ToolRegistry::byId(const QString& id) const
{
    return byId_.value(id, nullptr);
}

std::vector<Tool*> ToolRegistry::sharingShortcut(const QString& key) const
{
    std::vector<Tool*> out;
    for (Tool* tool : order_) {
        if (tool->shortcut().compare(key, Qt::CaseInsensitive) == 0)
            out.push_back(tool);
    }
    return out;
}

void registerBuiltinTools()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    ToolRegistry& registry = ToolRegistry::instance();
    registerSelectTools(registry);
    registerPaintTools(registry);
    registerShapeTools(registry);
    registerMiscTools(registry);
    registerTransformTools(registry);
    registerPlaceTool(registry);
}

// ----------------------------------------------------------------- options bar

ToolOptionsBar::ToolOptionsBar(QWidget* parent) : QWidget(parent)
{
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(8, 3, 8, 3);
    layout_->setSpacing(6);
    chooser_ = new QToolButton(this);
    chooser_->setPopupMode(QToolButton::InstantPopup);
    chooser_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    chooser_->setIconSize(QSize(20, 20));
    chooser_->setAutoRaise(true);
    chooser_->setToolTip(tr("Choose a tool (Alt+T)"));
    chooser_->setMenu(new QMenu(chooser_));
    layout_->addWidget(chooser_);
    title_ = new QLabel(this);
    QFont font = title_->font();
    font.setBold(true);
    title_->setFont(font);
    title_->setMinimumWidth(104);
    layout_->addWidget(title_);
    addSeparator();
    layout_->addStretch(1);
    setFixedHeight(36);
    setAutoFillBackground(true);
}

void ToolOptionsBar::clearControls()
{
    for (QWidget* widget : owned_) {
        layout_->removeWidget(widget);
        widget->deleteLater();
    }
    owned_.clear();
    // Everything after the fixed icon and title is rebuilt for each tool, so
    // drop it along with the trailing stretch.
    while (layout_->count() > kFixedItems) {
        QLayoutItem* item = layout_->takeAt(kFixedItems);
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

void ToolOptionsBar::startTool(Tool* tool)
{
    clearControls();
    values_.clear();
    if (!tool) {
        title_->clear();
        chooser_->setIcon(QIcon());
        layout_->addStretch(1);
        return;
    }
    currentToolId_ = tool->id();
    chooser_->setIcon(icons::get(tool->iconName(), 20));
    title_->setText(tool->name());
    addSeparator();
    tool->buildOptions(*this);
    addStretch();
}

void ToolOptionsBar::setToolList(const QStringList& toolIds, const QString& currentId)
{
    currentToolId_ = currentId;
    QMenu* menu = chooser_->menu();
    menu->clear();
    for (const QString& id : toolIds) {
        Tool* tool = ToolRegistry::instance().byId(id);
        if (!tool)
            continue;
        QAction* action = menu->addAction(icons::get(tool->iconName(), 18),
                                          tool->shortcut().isEmpty()
                                              ? tool->name()
                                              : QStringLiteral("%1\t%2").arg(
                                                    tool->name(), tool->shortcut()));
        action->setCheckable(true);
        action->setChecked(id == currentId);
        connect(action, &QAction::triggered, this, [this, id] { emit toolPicked(id); });
    }
}

void ToolOptionsBar::openToolList()
{
    if (chooser_)
        chooser_->showMenu();
}

QVariant ToolOptionsBar::value(const QString& key, const QVariant& fallback) const
{
    const QVariant found = values_.value(key);
    return found.isValid() ? found : fallback;
}

void ToolOptionsBar::setValue(const QString& key, const QVariant& value)
{
    values_[key] = value;
}

void ToolOptionsBar::publish(const QString& key, const QVariant& value)
{
    values_[key] = value;
    emit valueChanged(key, value);
}

void ToolOptionsBar::addWidget(QWidget* widget)
{
    widget->setParent(this);
    layout_->addWidget(widget);
    owned_.append(widget);
}

void ToolOptionsBar::addLabel(const QString& text)
{
    auto* label = new QLabel(text, this);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(
        theme::textColor().darker(130).name()));
    addWidget(label);
}

void ToolOptionsBar::addSeparator()
{
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::VLine);
    line->setStyleSheet(QStringLiteral("color: %1;").arg(theme::gridColor().name()));
    addWidget(line);
}

void ToolOptionsBar::addStretch() { layout_->addStretch(1); }

QSpinBox* ToolOptionsBar::addSpin(const QString& key, const QString& label, int minimum,
                                  int maximum, int value, const QString& suffix)
{
    if (!label.isEmpty())
        addLabel(label);
    auto* spin = new QSpinBox(this);
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    spin->setMinimumWidth(64);
    values_[key] = value;
    connect(spin, &QSpinBox::valueChanged, this,
            [this, key](int next) { publish(key, next); });
    addWidget(spin);
    return spin;
}

QSlider* ToolOptionsBar::addPercent(const QString& key, const QString& label,
                                    int value, int minimum, int maximum)
{
    if (!label.isEmpty())
        addLabel(label);
    auto* slider = new QSlider(Qt::Horizontal, this);
    slider->setRange(minimum, maximum);
    slider->setValue(value);
    slider->setMinimumWidth(96);
    slider->setMaximumWidth(140);
    auto* readout = new QSpinBox(this);
    readout->setRange(minimum, maximum);
    readout->setValue(value);
    readout->setSuffix(QStringLiteral("%"));
    readout->setKeyboardTracking(false);
    readout->setMinimumWidth(64);
    values_[key] = value;

    connect(slider, &QSlider::valueChanged, this, [this, key, readout](int next) {
        if (readout->value() != next)
            readout->setValue(next);
        publish(key, next);
    });
    connect(readout, &QSpinBox::valueChanged, this, [slider](int next) {
        if (slider->value() != next)
            slider->setValue(next);
    });
    addWidget(slider);
    addWidget(readout);
    return slider;
}

QDoubleSpinBox* ToolOptionsBar::addDouble(const QString& key, const QString& label,
                                          double minimum, double maximum, double value,
                                          double step, int decimals,
                                          const QString& suffix)
{
    if (!label.isEmpty())
        addLabel(label);
    auto* spin = new QDoubleSpinBox(this);
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    spin->setMinimumWidth(72);
    values_[key] = value;
    connect(spin, &QDoubleSpinBox::valueChanged, this,
            [this, key](double next) { publish(key, next); });
    addWidget(spin);
    return spin;
}

QCheckBox* ToolOptionsBar::addCheck(const QString& key, const QString& label, bool checked)
{
    auto* box = new QCheckBox(label, this);
    box->setChecked(checked);
    values_[key] = checked;
    connect(box, &QCheckBox::toggled, this,
            [this, key](bool next) { publish(key, next); });
    addWidget(box);
    return box;
}

QComboBox* ToolOptionsBar::addCombo(const QString& key, const QString& label,
                                    const QStringList& items, int index)
{
    if (!label.isEmpty())
        addLabel(label);
    auto* combo = new QComboBox(this);
    combo->addItems(items);
    combo->setCurrentIndex(std::clamp(index, 0, int(items.size()) - 1));
    values_[key] = combo->currentIndex();
    connect(combo, &QComboBox::currentIndexChanged, this,
            [this, key](int next) { publish(key, next); });
    addWidget(combo);
    return combo;
}

QToolButton* ToolOptionsBar::addToggle(const QString& key, const QString& iconName,
                                       const QString& tooltip, bool checked)
{
    auto* button = new QToolButton(this);
    button->setIcon(icons::get(iconName, 18));
    button->setProperty("iconName", iconName);
    button->setIconSize(QSize(18, 18));
    button->setCheckable(true);
    button->setChecked(checked);
    button->setToolTip(tooltip);
    values_[key] = checked;
    connect(button, &QToolButton::toggled, this,
            [this, key](bool next) { publish(key, next); });
    addWidget(button);
    return button;
}

QToolButton* ToolOptionsBar::addTextToggle(const QString& key, const QString& text,
                                           const QString& tooltip, bool checked)
{
    auto* button = new QToolButton(this);
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setCheckable(true);
    button->setChecked(checked);
    button->setToolTip(tooltip);
    button->setAutoRaise(true);
    values_[key] = checked;
    connect(button, &QToolButton::toggled, this,
            [this, key](bool next) { publish(key, next); });
    addWidget(button);
    return button;
}

QFontComboBox* ToolOptionsBar::addFontCombo(const QString& key, const QString& family)
{
    auto* combo = new QFontComboBox(this);
    combo->setCurrentFont(QFont(family));
    combo->setMinimumWidth(150);
    values_[key] = family;
    connect(combo, &QFontComboBox::currentFontChanged, this,
            [this, key](const QFont& font) { publish(key, font.family()); });
    addWidget(combo);
    return combo;
}

void ToolOptionsBar::addToggleGroup(const QString& key, const QString& label,
                                    const QList<QPair<QString, QString>>& iconsAndTips,
                                    int index)
{
    if (!label.isEmpty())
        addLabel(label);
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    for (int i = 0; i < iconsAndTips.size(); ++i) {
        auto* button = new QToolButton(this);
        button->setIcon(icons::get(iconsAndTips[i].first, 18));
        button->setProperty("iconName", iconsAndTips[i].first);
        button->setIconSize(QSize(18, 18));
        button->setToolTip(iconsAndTips[i].second);
        button->setCheckable(true);
        button->setChecked(i == index);
        group->addButton(button, i);
        addWidget(button);
    }
    values_[key] = index;
    connect(group, &QButtonGroup::idClicked, this,
            [this, key](int id) { publish(key, id); });
}

} // namespace tessel
