#include "ui/Dialogs.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QBuffer>
#include <QImageWriter>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/ImageOps.h"
#include "core/Io.h"
#include "core/Strings.h"
#include "effects/Effect.h"
#include "tessel_version.h"
#include "ui/EffectDialog.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tessel {

namespace {

// The widest image the spin boxes offer, in pixels.
constexpr int kMaxPixels = 60000;

struct Preset {
    const char* name;
    int width;
    int height;
};

// The names are marked here and looked up where the box is filled, because a
// static table cannot hold a translation.
const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {QT_TRANSLATE_NOOP("Dialogs", "Custom"), 0, 0},
        {QT_TRANSLATE_NOOP("Dialogs", "Screen 1920 x 1080"), 1920, 1080},
        {QT_TRANSLATE_NOOP("Dialogs", "Screen 2560 x 1440"), 2560, 1440},
        {QT_TRANSLATE_NOOP("Dialogs", "Screen 3840 x 2160"), 3840, 2160},
        {QT_TRANSLATE_NOOP("Dialogs", "Square 512 x 512"), 512, 512},
        {QT_TRANSLATE_NOOP("Dialogs", "Square 1024 x 1024"), 1024, 1024},
        {QT_TRANSLATE_NOOP("Dialogs", "Icon 256 x 256"), 256, 256},
        {QT_TRANSLATE_NOOP("Dialogs", "Portrait A4 at 150 dpi"), 1240, 1754},
        {QT_TRANSLATE_NOOP("Dialogs", "Landscape A4 at 150 dpi"), 1754, 1240},
        {QT_TRANSLATE_NOOP("Dialogs", "Photo 4 x 6 at 300 dpi"), 1800, 1200},
        {QT_TRANSLATE_NOOP("Dialogs", "Social post 1080 x 1080"), 1080, 1080},
        {QT_TRANSLATE_NOOP("Dialogs", "Social story 1080 x 1920"), 1080, 1920},
    };
    return list;
}

QString describeSize(int width, int height)
{
    const double megapixels = double(width) * height / 1.0e6;
    const double megabytes = double(width) * height * 4 / (1024.0 * 1024.0);
    return QCoreApplication::translate(
               "Dialogs", "%1 x %2 pixels  -  %3 MP  -  %4 MB per layer")
        .arg(width).arg(height)
        .arg(megapixels, 0, 'f', 2)
        .arg(megabytes, 0, 'f', 1);
}

} // namespace

// ----------------------------------------------------------------- new image

NewImageDialog::NewImageDialog(int defaultWidth, int defaultHeight, Rgba secondary,
                               QWidget* parent)
    : QDialog(parent), secondary_(secondary)
{
    setWindowTitle(tr("New Image"));
    setWindowIcon(icons::get(QStringLiteral("new"), 22));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    preset_ = new QComboBox(this);
    for (const Preset& item : presets())
        preset_->addItem(QCoreApplication::translate("Dialogs", item.name));
    form->addRow(tr("Preset"), preset_);

    width_ = new QSpinBox(this);
    width_->setRange(1, 60000);
    width_->setValue(defaultWidth);
    width_->setSuffix(QStringLiteral(" px"));
    height_ = new QSpinBox(this);
    height_->setRange(1, 60000);
    height_->setValue(defaultHeight);
    height_->setSuffix(QStringLiteral(" px"));
    form->addRow(tr("Width"), width_);
    form->addRow(tr("Height"), height_);

    keepRatio_ = new QCheckBox(tr("Maintain aspect ratio"), this);
    form->addRow(QString(), keepRatio_);

    background_ = new QComboBox(this);
    background_->addItems({tr("White"), tr("Transparent"),
                           tr("Secondary colour"), tr("Black")});
    form->addRow(tr("Background"), background_);
    root->addLayout(form);

    sizeLabel_ = new QLabel(this);
    sizeLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 8pt;")
                                  .arg(theme::textColor().darker(140).name()));
    root->addWidget(sizeLabel_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    ratio_ = double(defaultWidth) / std::max(1, defaultHeight);
    connect(preset_, &QComboBox::currentIndexChanged, this, &NewImageDialog::applyPreset);
    connect(width_, &QSpinBox::valueChanged, this, [this](int value) {
        if (updating_)
            return;
        if (keepRatio_->isChecked()) {
            updating_ = true;
            height_->setValue(std::max(1, int(std::lround(value / ratio_))));
            updating_ = false;
        }
        updateSizeLabel();
    });
    connect(height_, &QSpinBox::valueChanged, this, [this](int value) {
        if (updating_)
            return;
        if (keepRatio_->isChecked()) {
            updating_ = true;
            width_->setValue(std::max(1, int(std::lround(value * ratio_))));
            updating_ = false;
        }
        updateSizeLabel();
    });
    connect(keepRatio_, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            ratio_ = double(width_->value()) / std::max(1, height_->value());
    });
    updateSizeLabel();
}

void NewImageDialog::applyPreset(int index)
{
    const std::vector<Preset>& list = presets();
    if (index <= 0 || index >= int(list.size()))
        return;
    updating_ = true;
    width_->setValue(list[size_t(index)].width);
    height_->setValue(list[size_t(index)].height);
    ratio_ = double(width_->value()) / std::max(1, height_->value());
    updating_ = false;
    updateSizeLabel();
}

void NewImageDialog::updateSizeLabel()
{
    sizeLabel_->setText(describeSize(width_->value(), height_->value()));
}

int NewImageDialog::imageWidth() const { return width_->value(); }
int NewImageDialog::imageHeight() const { return height_->value(); }

Rgba NewImageDialog::background() const
{
    switch (background_->currentIndex()) {
    case 1: return Rgba(0, 0, 0, 0);
    case 2: return secondary_;
    case 3: return Rgba(0, 0, 0, 255);
    default: return Rgba(255, 255, 255, 255);
    }
}

// ----------------------------------------------------------------- resize

namespace {

const Unit kResizeUnits[4] = {Unit::Pixels, Unit::Inches, Unit::Centimetres,
                              Unit::Percent};

int indexOfUnit(Unit unit)
{
    for (int i = 0; i < 4; ++i) {
        if (kResizeUnits[i] == unit)
            return i;
    }
    return 0;
}

} // namespace

ResizeDialog::ResizeDialog(const QString& title, int currentWidth, int currentHeight,
                           double currentResolution, Unit unit, bool withAnchor,
                           QWidget* parent)
    : QDialog(parent), unit_(unit),
      originalWidth_(std::max(1, currentWidth)),
      originalHeight_(std::max(1, currentHeight)),
      originalResolution_(units::sanitize(currentResolution)),
      pixelWidth_(std::max(1, currentWidth)),
      pixelHeight_(std::max(1, currentHeight)),
      resolution_(units::sanitize(currentResolution))
{
    setWindowTitle(title);
    setWindowIcon(icons::get(withAnchor ? QStringLiteral("canvas")
                                        : QStringLiteral("resize"), 22));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* form = new QFormLayout;
    form->setSpacing(8);

    unitCombo_ = new QComboBox(this);
    unitCombo_->setObjectName(QStringLiteral("units"));
    for (Unit candidate : kResizeUnits)
        unitCombo_->addItem(units::unitName(candidate));
    unitCombo_->setCurrentIndex(indexOfUnit(unit_));
    form->addRow(tr("Units"), unitCombo_);

    width_ = new QDoubleSpinBox(this);
    width_->setObjectName(QStringLiteral("width"));
    width_->setKeyboardTracking(false);
    height_ = new QDoubleSpinBox(this);
    height_->setObjectName(QStringLiteral("height"));
    height_->setKeyboardTracking(false);
    form->addRow(tr("Width"), width_);
    form->addRow(tr("Height"), height_);

    keepRatio_ = new QCheckBox(tr("Maintain aspect ratio"), this);
    keepRatio_->setObjectName(QStringLiteral("keepratio"));
    keepRatio_->setChecked(true);
    form->addRow(QString(), keepRatio_);

    if (!withAnchor) {
        resolutionBox_ = new QDoubleSpinBox(this);
        resolutionBox_->setObjectName(QStringLiteral("resolution"));
        resolutionBox_->setRange(units::kMinResolution, units::kMaxResolution);
        resolutionBox_->setDecimals(2);
        resolutionBox_->setSuffix(QStringLiteral(" ppi"));
        resolutionBox_->setKeyboardTracking(false);
        resolutionBox_->setValue(resolution_);
        form->addRow(tr("Resolution"), resolutionBox_);

        resample_ = new QCheckBox(tr("Resample image"), this);
        resample_->setObjectName(QStringLiteral("resample"));
        resample_->setChecked(true);
        resample_->setToolTip(tr("Off: changing the resolution only changes the "
                                 "print size, and the pixels are left alone."));
        form->addRow(QString(), resample_);

        resampling_ = new QComboBox(this);
        resampling_->setObjectName(QStringLiteral("resampling"));
        resampling_->addItems({tr("Smooth (best quality)"),
                               tr("Nearest neighbour (hard pixels)")});
        form->addRow(tr("Resampling"), resampling_);
    }
    root->addLayout(form);

    if (withAnchor) {
        auto* group = new QGroupBox(tr("Anchor"), this);
        auto* grid = new QGridLayout(group);
        grid->setSpacing(2);
        static const Anchor order[9] = {
            Anchor::TopLeft, Anchor::Top, Anchor::TopRight,
            Anchor::Left, Anchor::Center, Anchor::Right,
            Anchor::BottomLeft, Anchor::Bottom, Anchor::BottomRight};
        static const char* glyphs[9] = {"up", "up", "up", "pan", "pan", "pan",
                                        "down", "down", "down"};
        for (int i = 0; i < 9; ++i) {
            auto* button = new QToolButton(group);
            button->setCheckable(true);
            button->setFixedSize(30, 30);
            button->setIcon(icons::get(QString::fromLatin1(glyphs[i]), 16));
            button->setToolTip(
                QCoreApplication::translate("Core", anchorName(order[i])));
            button->setChecked(order[i] == Anchor::Center);
            connect(button, &QToolButton::clicked, this, [this, i] {
                anchor_ = order[i];
                for (int j = 0; j < 9; ++j)
                    anchorButtons_[j]->setChecked(j == i);
            });
            anchorButtons_[i] = button;
            grid->addWidget(button, i / 3, i % 3);
        }
        root->addWidget(group);
    }

    const QString quiet = QStringLiteral("color: %1; font-size: 8pt;")
                              .arg(theme::textColor().darker(140).name());
    printSize_ = new QLabel(this);
    printSize_->setObjectName(QStringLiteral("printsize"));
    printSize_->setStyleSheet(quiet);
    root->addWidget(printSize_);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("summary"));
    summary_->setStyleSheet(quiet);
    root->addWidget(summary_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(unitCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0 && index < 4)
            unit_ = kResizeUnits[index];
        // Only the presentation changes: the pixels stay exactly as they are.
        applyUnitToFields();
    });
    connect(width_, &QDoubleSpinBox::valueChanged, this, [this] { commitWidth(true); });
    connect(height_, &QDoubleSpinBox::valueChanged, this, [this] { commitWidth(false); });
    if (resolutionBox_) {
        connect(resolutionBox_, &QDoubleSpinBox::valueChanged, this,
                [this](double value) {
                    if (updating_)
                        return;
                    const double next = units::sanitize(value);
                    if (resamples()) {
                        // Resampling holds the print size, so a denser image
                        // needs proportionally more pixels.
                        const double inchesW = pixelWidth_ / resolution_;
                        const double inchesH = pixelHeight_ / resolution_;
                        resolution_ = next;
                        pixelWidth_ = std::clamp(int(std::lround(inchesW * next)),
                                                 1, kMaxPixels);
                        pixelHeight_ = std::clamp(int(std::lround(inchesH * next)),
                                                  1, kMaxPixels);
                    } else {
                        resolution_ = next;
                    }
                    refreshFields();
                });
    }
    if (resample_) {
        connect(resample_, &QCheckBox::toggled, this, [this] { refreshFields(); });
    }
    applyUnitToFields();
}

void ResizeDialog::applyUnitToFields()
{
    const int places = units::decimals(unit_);
    const double reference = std::max(originalWidth_, originalHeight_);
    double minimum = 0.0;
    double maximum = 0.0;
    if (unit_ == Unit::Percent) {
        minimum = 0.1;
        maximum = 10000.0;
    } else {
        minimum = units::fromPixels(1, unit_, resolution_, reference);
        maximum = units::fromPixels(kMaxPixels, unit_, resolution_, reference);
        if (places > 0)
            minimum = std::max(minimum, std::pow(10.0, -places));
    }
    // Narrowing the range clamps whatever value is still in the box, which
    // would otherwise be read back as a size in the new unit.
    updating_ = true;
    for (QDoubleSpinBox* box : {width_, height_}) {
        box->setDecimals(places);
        box->setRange(minimum, maximum);
        box->setSingleStep(units::singleStep(unit_));
        box->setSuffix(units::unitSuffix(unit_));
    }
    updating_ = false;
    refreshFields();
}

void ResizeDialog::refreshFields()
{
    updating_ = true;
    width_->setValue(units::fromPixels(pixelWidth_, unit_, resolution_, originalWidth_));
    height_->setValue(units::fromPixels(pixelHeight_, unit_, resolution_, originalHeight_));
    if (resolutionBox_)
        resolutionBox_->setValue(resolution_);
    updating_ = false;

    // With resampling off the pixel count is frozen, so the only way to change
    // the size is through a physical unit, which moves the resolution instead.
    const bool physical = unit_ == Unit::Inches || unit_ == Unit::Centimetres;
    const bool frozen = !resamples();
    width_->setEnabled(!frozen || physical);
    height_->setEnabled(!frozen || physical);
    keepRatio_->setEnabled(!frozen);
    if (resampling_)
        resampling_->setEnabled(!frozen);
    refreshLabels();
}

void ResizeDialog::refreshLabels()
{
    printSize_->setText(tr("Print size: %1")
                            .arg(units::describePrintSize(pixelWidth_, pixelHeight_,
                                                          resolution_, unit_)));
    summary_->setText(tr("Was %1 x %2 px at %3 ppi  ->  %4")
                          .arg(originalWidth_).arg(originalHeight_)
                          .arg(originalResolution_, 0, 'g', 6)
                          .arg(describeSize(pixelWidth_, pixelHeight_)));
}

void ResizeDialog::commitWidth(bool widthLed)
{
    if (updating_)
        return;
    const double value = widthLed ? width_->value() : height_->value();
    const bool physical = unit_ == Unit::Inches || unit_ == Unit::Centimetres;
    if (!resamples()) {
        if (!physical)
            return;
        // The pixels are fixed, so asking for a different printed size is
        // really asking for a different resolution.
        const double inches = unit_ == Unit::Centimetres
            ? value / units::kCentimetresPerInch
            : value;
        if (inches <= 0.0)
            return;
        const double pixels = widthLed ? pixelWidth_ : pixelHeight_;
        resolution_ = units::sanitize(pixels / inches);
        refreshFields();
        return;
    }

    const double reference = widthLed ? originalWidth_ : originalHeight_;
    const int pixels = std::clamp(
        int(std::lround(units::toPixels(value, unit_, resolution_, reference))),
        1, kMaxPixels);
    if (widthLed)
        pixelWidth_ = pixels;
    else
        pixelHeight_ = pixels;
    if (keepRatio_->isChecked()) {
        const double ratio = double(originalWidth_) / originalHeight_;
        if (widthLed)
            pixelHeight_ = std::clamp(int(std::lround(pixelWidth_ / ratio)), 1, kMaxPixels);
        else
            pixelWidth_ = std::clamp(int(std::lround(pixelHeight_ * ratio)), 1, kMaxPixels);
    }
    refreshFields();
}

bool ResizeDialog::smoothResampling() const
{
    return resampling_ ? resampling_->currentIndex() == 0 : true;
}

bool ResizeDialog::resamples() const
{
    return resample_ ? resample_->isChecked() : true;
}

void ResizeDialog::setUnit(Unit unit)
{
    unitCombo_->setCurrentIndex(indexOfUnit(unit));
}

void ResizeDialog::setDisplayedWidth(double value) { width_->setValue(value); }
void ResizeDialog::setDisplayedHeight(double value) { height_->setValue(value); }

void ResizeDialog::setResolution(double resolution)
{
    if (resolutionBox_)
        resolutionBox_->setValue(resolution);
}

void ResizeDialog::setResample(bool on)
{
    if (resample_)
        resample_->setChecked(on);
}

double ResizeDialog::displayedWidth() const { return width_->value(); }
double ResizeDialog::displayedHeight() const { return height_->value(); }
QString ResizeDialog::printSizeText() const { return printSize_->text(); }

// ----------------------------------------------------------------- layer props

LayerPropertiesDialog::LayerPropertiesDialog(Document* doc, History* history,
                                             QWidget* parent)
    : QDialog(parent), doc_(doc), history_(history)
{
    setWindowTitle(tr("Layer Properties"));
    setWindowIcon(icons::get(QStringLiteral("layer-props"), 22));
    // The properties belong to the layer or group itself, never to its mask.
    Layer* layer = doc_->activeNode();
    originalName_ = layer ? layer->name : QString();
    originalVisible_ = layer ? layer->visible : true;
    originalMode_ = layer ? layer->mode : BlendMode::Normal;
    originalOpacity_ = layer ? layer->opacity : 255;
    layerUid_ = layer ? layer->uid() : 0;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);
    auto* form = new QFormLayout;
    form->setSpacing(8);

    name_ = new QLineEdit(originalName_, this);
    form->addRow(tr("Name"), name_);
    visible_ = new QCheckBox(tr("Visible"), this);
    visible_->setChecked(originalVisible_);
    form->addRow(QString(), visible_);
    blend_ = new QComboBox(this);
    blend_->addItems(blendModeLabels());
    blend_->setCurrentIndex(int(originalMode_));
    form->addRow(tr("Blend mode"), blend_);

    auto* opacityRow = new QHBoxLayout;
    opacity_ = new QSlider(Qt::Horizontal, this);
    opacity_->setRange(0, 255);
    opacity_->setValue(originalOpacity_);
    opacityValue_ = new QSpinBox(this);
    opacityValue_->setRange(0, 255);
    opacityValue_->setValue(originalOpacity_);
    opacityRow->addWidget(opacity_, 1);
    opacityRow->addWidget(opacityValue_);
    form->addRow(tr("Opacity"), opacityRow);
    root->addLayout(form);
    buildEffectsSection(root);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    // Everything previews live on the canvas.
    connect(opacity_, &QSlider::valueChanged, this, [this](int value) {
        opacityValue_->setValue(value);
        apply();
    });
    connect(opacityValue_, &QSpinBox::valueChanged, this, [this](int value) {
        opacity_->setValue(value);
        apply();
    });
    connect(visible_, &QCheckBox::toggled, this, [this] { apply(); });
    connect(blend_, &QComboBox::currentIndexChanged, this, [this] { apply(); });
    connect(name_, &QLineEdit::textChanged, this, [this] { apply(); });
    setMinimumWidth(340);
}

void LayerPropertiesDialog::buildEffectsSection(QVBoxLayout* root)
{
    const Layer* layer = doc_->layerByUid(layerUid_);
    if (!layer)
        return;
    auto* box = new QGroupBox(layer->isAdjustment() ? tr("Adjustment")
                                                    : tr("Effects"),
                              this);
    auto* column = new QVBoxLayout(box);
    column->setSpacing(6);

    effects_ = new QListWidget(box);
    effects_->setSelectionMode(QAbstractItemView::SingleSelection);
    effects_->setMaximumHeight(110);
    effects_->setToolTip(layer->isAdjustment()
        ? tr("What this adjustment layer applies to everything below "
             "it inside its group.")
        : tr("Applied to this layer's pixels while it is composited. "
             "The stored pixels are left alone until the stack is "
             "flattened."));
    column->addWidget(effects_);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    const auto button = [buttons, box](const QString& icon, const QString& tip) {
        auto* widget = new QToolButton(box);
        widget->setIcon(icons::get(icon, 18));
        widget->setProperty("iconName", icon);
        widget->setIconSize(QSize(18, 18));
        widget->setToolTip(tip);
        widget->setAutoRaise(true);
        buttons->addWidget(widget);
        return widget;
    };
    if (!layer->isAdjustment()) {
        auto* add = button(QStringLiteral("layer-add"), tr("Add Effect"));
        add->setPopupMode(QToolButton::InstantPopup);
        add->setObjectName(QStringLiteral("addEffect"));
        auto* menu = new QMenu(add);
        fillEffectMenu(menu, [this](Effect* effect) {
            const Layer* target = doc_->layerByUid(layerUid_);
            if (!target)
                return;
            std::vector<LayerEffect> next = target->effects();
            next.push_back({effect->id(), ParamValues::defaults(effect->params()),
                            true});
            recordEffects(tr("Add Effect"), QStringLiteral("effect"),
                          next);
            effects_->setCurrentRow(int(next.size()) - 1);
            if (!effect->params().empty())
                editEffectEntry(int(next.size()) - 1);
        });
        add->setMenu(menu);
    }
    editEffect_ = button(QStringLiteral("settings"), tr("Edit Settings"));
    editEffect_->setObjectName(QStringLiteral("editEffect"));
    connect(editEffect_, &QToolButton::clicked, this,
            [this] { editEffectEntry(selectedEffect()); });
    if (!layer->isAdjustment()) {
        upEffect_ = button(QStringLiteral("up"), tr("Move Effect Up"));
        downEffect_ = button(QStringLiteral("down"), tr("Move Effect Down"));
        removeEffect_ = button(QStringLiteral("layer-delete"),
                               tr("Remove Effect"));
        removeEffect_->setObjectName(QStringLiteral("removeEffect"));
        flattenEffects_ = button(QStringLiteral("flatten"),
                                 tr("Flatten the effects into the pixels"));
        flattenEffects_->setObjectName(QStringLiteral("flattenEffects"));
        // The entries run bottom to top, and the list shows them in that same
        // order, so moving an entry up in the list moves it later in the run.
        const auto reorder = [this](int delta) {
            const int index = selectedEffect();
            const Layer* target = doc_->layerByUid(layerUid_);
            if (!target || index < 0)
                return;
            const int other = index + delta;
            if (other < 0 || other >= int(target->effects().size()))
                return;
            std::vector<LayerEffect> next = target->effects();
            std::swap(next[size_t(index)], next[size_t(other)]);
            recordEffects(tr("Reorder Effects"), QStringLiteral("effect"),
                          next);
            effects_->setCurrentRow(other);
        };
        connect(upEffect_, &QToolButton::clicked, this, [reorder] { reorder(1); });
        connect(downEffect_, &QToolButton::clicked, this, [reorder] { reorder(-1); });
        connect(removeEffect_, &QToolButton::clicked, this, [this] {
            const int index = selectedEffect();
            const Layer* target = doc_->layerByUid(layerUid_);
            if (!target || index < 0)
                return;
            std::vector<LayerEffect> next = target->effects();
            next.erase(next.begin() + index);
            recordEffects(tr("Remove Effect"),
                          QStringLiteral("layer-delete"), next);
        });
        connect(flattenEffects_, &QToolButton::clicked, this, [this] {
            Layer* target = doc_->layerByUid(layerUid_);
            if (!target || target->effects().empty())
                return;
            // The pixels change, so this is a snapshot step of its own rather
            // than a property one, and the fields are committed first so that
            // the snapshot does not hold values a later Cancel would revoke.
            commitProperties();
            const int uid = layerUid_;
            recordDocumentEdit(*doc_, *history_, tr("Flatten Effects"),
                               QStringLiteral("flatten"), [this, uid] {
                                   if (Layer* again = doc_->layerByUid(uid))
                                       doc_->flattenLayerEffects(again);
                               });
            refreshEffects();
            if (repaint_)
                repaint_();
        });
    }
    buttons->addStretch(1);
    column->addLayout(buttons);
    root->addWidget(box);

    connect(effects_, &QListWidget::currentRowChanged, this,
            [this] { updateEffectButtons(); });
    connect(effects_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                editEffectEntry(effects_->row(item));
            });
    connect(effects_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (updatingEffects_)
            return;
        const int index = effects_->row(item);
        const Layer* target = doc_->layerByUid(layerUid_);
        if (!target || index < 0 || index >= int(target->effects().size()))
            return;
        const bool on = item->checkState() == Qt::Checked;
        if (on == target->effects()[size_t(index)].enabled)
            return;
        std::vector<LayerEffect> next = target->effects();
        next[size_t(index)].enabled = on;
        recordEffects(on ? tr("Enable Effect")
                         : tr("Disable Effect"),
                      QStringLiteral("effect"), next);
    });
    refreshEffects();
}

void LayerPropertiesDialog::refreshEffects()
{
    if (!effects_)
        return;
    const Layer* layer = doc_->layerByUid(layerUid_);
    if (!layer)
        return;
    const int keep = effects_->currentRow();
    updatingEffects_ = true;
    effects_->clear();
    EffectRegistry& registry = EffectRegistry::instance();
    for (const LayerEffect& stored : layer->effects()) {
        Effect* effect = registry.byId(stored.effectId);
        auto* item = new QListWidgetItem(
            effect ? effect->name()
                   : tr("%1 (not installed)").arg(stored.effectId),
            effects_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(stored.enabled ? Qt::Checked : Qt::Unchecked);
        item->setIcon(icons::get(effect && effect->isAdjustment()
                                     ? QStringLiteral("adjust")
                                     : QStringLiteral("effect"),
                                 16));
    }
    if (keep >= 0 && keep < effects_->count())
        effects_->setCurrentRow(keep);
    else if (effects_->count() > 0)
        effects_->setCurrentRow(effects_->count() - 1);
    updatingEffects_ = false;
    updateEffectButtons();
}

void LayerPropertiesDialog::updateEffectButtons()
{
    const Layer* layer = doc_->layerByUid(layerUid_);
    if (!layer)
        return;
    EffectRegistry& registry = EffectRegistry::instance();
    const int index = selectedEffect();
    const int count = int(layer->effects().size());
    Effect* chosen = index >= 0
        ? registry.byId(layer->effects()[size_t(index)].effectId)
        : nullptr;
    if (editEffect_)
        editEffect_->setEnabled(chosen && !chosen->params().empty());
    if (upEffect_)
        upEffect_->setEnabled(index >= 0 && index + 1 < count);
    if (downEffect_)
        downEffect_->setEnabled(index > 0);
    if (removeEffect_)
        removeEffect_->setEnabled(index >= 0);
    if (flattenEffects_)
        flattenEffects_->setEnabled(count > 0);
}

int LayerPropertiesDialog::selectedEffect() const
{
    return effects_ ? effects_->currentRow() : -1;
}

void LayerPropertiesDialog::recordEffects(const QString& name, const QString& icon,
                                          std::vector<LayerEffect> after)
{
    Layer* layer = doc_->layerByUid(layerUid_);
    if (!layer)
        return;
    const std::vector<LayerEffect> before = layer->effects();
    const int uid = layerUid_;
    const auto write = [uid](Document& doc, const std::vector<LayerEffect>& list) {
        if (Layer* target = doc.layerByUid(uid))
            doc.setLayerEffects(target, list);
    };
    doc_->setLayerEffects(layer, after);
    history_->push(std::make_unique<PropertyEdit>(
        name, icon, [write, before](Document& doc) { write(doc, before); },
        [write, after](Document& doc) { write(doc, after); }));
    refreshEffects();
    if (repaint_)
        repaint_();
}

void LayerPropertiesDialog::editEffectEntry(int index)
{
    Layer* layer = doc_->layerByUid(layerUid_);
    if (!layer || index < 0 || index >= int(layer->effects().size()))
        return;
    Effect* effect =
        EffectRegistry::instance().byId(layer->effects()[size_t(index)].effectId);
    if (!effect || effect->params().empty())
        return;
    // Modal on purpose: it edits the entry this dialog is also showing, so the
    // two must not be driven at the same time.
    StoredEffectDialog dialog(effect, doc_, history_, layer, index, this);
    dialog.setRepaintCallback(repaint_);
    dialog.exec();
    refreshEffects();
    if (repaint_)
        repaint_();
}

void LayerPropertiesDialog::apply()
{
    Layer* layer = doc_->layerByUid(layerUid_);
    if (!layer)
        return;
    layer->name = name_->text();
    layer->visible = visible_->isChecked();
    layer->mode = static_cast<BlendMode>(blend_->currentIndex());
    layer->opacity = static_cast<uint8_t>(opacity_->value());
    doc_->invalidateLayer(layer);
    if (repaint_)
        repaint_();
}

void LayerPropertiesDialog::commitProperties()
{
    Layer* layer = doc_->layerByUid(layerUid_);
    if (!layer)
        return;
    const QString newName = name_->text();
    const bool newVisible = visible_->isChecked();
    const BlendMode newMode = static_cast<BlendMode>(blend_->currentIndex());
    const uint8_t newOpacity = static_cast<uint8_t>(opacity_->value());
    const bool changed = newName != originalName_ || newVisible != originalVisible_
        || newMode != originalMode_ || newOpacity != originalOpacity_;
    if (changed) {
        const int uid = layerUid_;
        const QString oldName = originalName_;
        const bool oldVisible = originalVisible_;
        const BlendMode oldMode = originalMode_;
        const uint8_t oldOpacity = originalOpacity_;
        history_->push(std::make_unique<PropertyEdit>(
            tr("Layer Properties"), QStringLiteral("layer-props"),
            [uid, oldName, oldVisible, oldMode, oldOpacity](Document& doc) {
                if (Layer* target = doc.layerByUid(uid)) {
                    target->name = oldName;
                    target->visible = oldVisible;
                    target->mode = oldMode;
                    target->opacity = oldOpacity;
                    doc.invalidate();
                }
            },
            [uid, newName, newVisible, newMode, newOpacity](Document& doc) {
                if (Layer* target = doc.layerByUid(uid)) {
                    target->name = newName;
                    target->visible = newVisible;
                    target->mode = newMode;
                    target->opacity = newOpacity;
                    doc.invalidate();
                }
            }));
    }
    // What the fields hold now is what Cancel would go back to, which is what
    // lets a flatten in the middle of the dialog keep a sound snapshot.
    originalName_ = newName;
    originalVisible_ = newVisible;
    originalMode_ = newMode;
    originalOpacity_ = newOpacity;
}

void LayerPropertiesDialog::done(int result)
{
    Layer* layer = doc_->layerByUid(layerUid_);
    if (layer) {
        if (result == QDialog::Accepted) {
            commitProperties();
        } else {
            layer->name = originalName_;
            layer->visible = originalVisible_;
            layer->mode = originalMode_;
            layer->opacity = originalOpacity_;
            doc_->invalidateLayer(layer);
        }
    }
    if (repaint_)
        repaint_();
    QDialog::done(result);
}

// ----------------------------------------------------------------- rotate / zoom

RotateZoomDialog::RotateZoomDialog(Document* doc, History* history, QWidget* parent)
    : QDialog(parent), doc_(doc), history_(history)
{
    setWindowTitle(tr("Rotate / Zoom Layer"));
    setWindowIcon(icons::get(QStringLiteral("rotate-cw"), 22));
    if (Layer* layer = doc_->activeLayer()) {
        source_ = layer->image;
        layerUid_ = layer->uid();
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);
    auto* form = new QFormLayout;
    form->setSpacing(8);

    angle_ = new QDoubleSpinBox(this);
    angle_->setRange(-360.0, 360.0);
    angle_->setDecimals(2);
    angle_->setSuffix(QStringLiteral(" deg"));
    angle_->setKeyboardTracking(false);
    zoom_ = new QDoubleSpinBox(this);
    zoom_->setRange(0.05, 20.0);
    zoom_->setValue(1.0);
    zoom_->setSingleStep(0.05);
    zoom_->setDecimals(3);
    zoom_->setKeyboardTracking(false);
    offsetX_ = new QDoubleSpinBox(this);
    offsetY_ = new QDoubleSpinBox(this);
    for (QDoubleSpinBox* spin : {offsetX_, offsetY_}) {
        spin->setRange(-10000, 10000);
        spin->setDecimals(1);
        spin->setSuffix(QStringLiteral(" px"));
        spin->setKeyboardTracking(false);
    }
    tile_ = new QCheckBox(tr("Tile the result"), this);

    form->addRow(tr("Angle"), angle_);
    form->addRow(tr("Zoom"), zoom_);
    form->addRow(tr("Pan X"), offsetX_);
    form->addRow(tr("Pan Y"), offsetY_);
    form->addRow(QString(), tile_);
    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                             | QDialogButtonBox::Reset,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this] {
        angle_->setValue(0);
        zoom_->setValue(1.0);
        offsetX_->setValue(0);
        offsetY_->setValue(0);
        tile_->setChecked(false);
    });
    root->addWidget(buttons);

    const auto refresh = [this] { updatePreview(); };
    connect(angle_, &QDoubleSpinBox::valueChanged, this, refresh);
    connect(zoom_, &QDoubleSpinBox::valueChanged, this, refresh);
    connect(offsetX_, &QDoubleSpinBox::valueChanged, this, refresh);
    connect(offsetY_, &QDoubleSpinBox::valueChanged, this, refresh);
    connect(tile_, &QCheckBox::toggled, this, refresh);
    setMinimumWidth(320);
}

Image RotateZoomDialog::render() const
{
    const double radians = -angle_->value() * M_PI / 180.0;
    const double scale = std::max(0.01, zoom_->value());
    const double cx = source_.width() / 2.0;
    const double cy = source_.height() / 2.0;
    const double cosA = std::cos(radians) / scale;
    const double sinA = std::sin(radians) / scale;
    const double dx = offsetX_->value();
    const double dy = offsetY_->value();
    const ops::EdgeMode edge = tile_->isChecked() ? ops::EdgeMode::Wrap
                                                  : ops::EdgeMode::Transparent;
    return ops::remap(source_, source_.width(), source_.height(),
                      [&](int x, int y, float& sx, float& sy) {
                          const double px = x - cx - dx;
                          const double py = y - cy - dy;
                          sx = float(cx + px * cosA - py * sinA);
                          sy = float(cy + px * sinA + py * cosA);
                      },
                      edge);
}

void RotateZoomDialog::updatePreview()
{
    if (source_.isNull())
        return;
    preview_ = render();
    doc_->setPreview(layerUid_, &preview_);
    previewActive_ = true;
    if (repaint_)
        repaint_();
}

void RotateZoomDialog::done(int result)
{
    if (previewActive_) {
        doc_->clearPreview();
        previewActive_ = false;
    }
    if (result == QDialog::Accepted && !source_.isNull()) {
        if (Layer* layer = doc_->layerByUid(layerUid_)) {
            const Image rotated = render();
            layer->image = source_;
            EditScope scope(*doc_, *history_, layer, tr("Rotate / Zoom Layer"),
                            QStringLiteral("rotate-cw"));
            layer->image = rotated;
            scope.commit();
            doc_->invalidateLayer(layer);
        }
    }
    doc_->invalidate();
    if (repaint_)
        repaint_();
    QDialog::done(result);
}

// ----------------------------------------------------------------- save options

// ----------------------------------------------------------------- selection ops

namespace {

struct SelectionOpInfo {
    const char* title;
    const char* label;
    const char* hint;
    const char* icon;
    int maximum;
};

// Marked rather than translated, because the window title doubles as the name
// of the undo step and is asked for long before the dialog is built.
SelectionOpInfo selectionOpInfo(SelectionOpDialog::Op op)
{
    switch (op) {
    case SelectionOpDialog::Op::Feather:
        return {QT_TRANSLATE_NOOP("Dialogs", "Feather Selection"),
                QT_TRANSLATE_NOOP("Dialogs", "Radius"),
                QT_TRANSLATE_NOOP("Dialogs", "Softens the selection edge so edits "
                                             "fade out across the radius."),
                "select-ellipse", 250};
    case SelectionOpDialog::Op::Grow:
        return {QT_TRANSLATE_NOOP("Dialogs", "Grow Selection"),
                QT_TRANSLATE_NOOP("Dialogs", "Amount"),
                QT_TRANSLATE_NOOP("Dialogs", "Pushes the selection edge outwards."),
                "select-all", 500};
    case SelectionOpDialog::Op::Shrink:
        return {QT_TRANSLATE_NOOP("Dialogs", "Shrink Selection"),
                QT_TRANSLATE_NOOP("Dialogs", "Amount"),
                QT_TRANSLATE_NOOP("Dialogs", "Pulls the selection edge inwards."),
                "deselect", 500};
    default:
        return {QT_TRANSLATE_NOOP("Dialogs", "Border Selection"),
                QT_TRANSLATE_NOOP("Dialogs", "Width"),
                QT_TRANSLATE_NOOP("Dialogs", "Replaces the selection with a band "
                                             "along the inside of its edge."),
                "select-rect", 500};
    }
}

} // namespace

SelectionOpDialog::SelectionOpDialog(Op op, QWidget* parent)
    : QDialog(parent), op_(op)
{
    const SelectionOpInfo info = selectionOpInfo(op);
    setWindowTitle(QCoreApplication::translate("Dialogs", info.title));
    setWindowIcon(icons::get(QString::fromLatin1(info.icon), 22));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* hint = new QLabel(QCoreApplication::translate("Dialogs", info.hint), this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* row = new QHBoxLayout;
    row->addWidget(
        new QLabel(QCoreApplication::translate("Dialogs", info.label), this));
    amount_ = new QSpinBox(this);
    amount_->setRange(op == Op::Border ? 1 : 0, info.maximum);
    amount_->setValue(op == Op::Feather ? 4 : (op == Op::Border ? 4 : 2));
    amount_->setSuffix(QStringLiteral(" px"));
    row->addWidget(amount_, 1);
    root->addLayout(row);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                        this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    amount_->setFocus();
    setMinimumWidth(320);
}

int SelectionOpDialog::amount() const
{
    return amount_ ? amount_->value() : 0;
}

QString SelectionOpDialog::title(Op op)
{
    return QCoreApplication::translate("Dialogs", selectionOpInfo(op).title);
}

void SelectionOpDialog::applyDirect(Op op, Document& doc, History& history, int amount)
{
    if (amount <= 0 || !doc.selection().isActive())
        return;
    recordSelectionEdit(doc, history, title(op), [&] {
        Selection& selection = doc.selection();
        switch (op) {
        case Op::Feather: selection.feather(amount); break;
        case Op::Grow: selection.grow(amount); break;
        case Op::Shrink: selection.grow(-amount); break;
        case Op::Border: selection.border(amount); break;
        }
    });
}

SaveOptionsDialog::SaveOptionsDialog(const Image& image, const QString& path,
                                     int quality, QWidget* parent)
    : QDialog(parent), image_(image), path_(path)
{
    setWindowTitle(tr("Save Configuration"));
    setWindowIcon(icons::get(QStringLiteral("save"), 22));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    root->addWidget(new QLabel(tr("%1 stores the image with lossy "
                                  "compression.")
                                   .arg(QFileInfo(path).suffix().toUpper()),
                               this));

    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Quality"), this));
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(1, 100);
    slider_->setValue(quality);
    value_ = new QSpinBox(this);
    value_->setRange(1, 100);
    value_->setValue(quality);
    row->addWidget(slider_, 1);
    row->addWidget(value_);
    root->addLayout(row);

    estimate_ = new QLabel(this);
    estimate_->setStyleSheet(QStringLiteral("color: %1; font-size: 8pt;")
                                 .arg(theme::textColor().darker(140).name()));
    root->addWidget(estimate_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(220);
    connect(debounce_, &QTimer::timeout, this, &SaveOptionsDialog::estimate);
    connect(slider_, &QSlider::valueChanged, this, [this](int next) {
        value_->setValue(next);
        debounce_->start();
    });
    connect(value_, &QSpinBox::valueChanged, this, [this](int next) {
        slider_->setValue(next);
        debounce_->start();
    });
    setMinimumWidth(360);
    estimate();
}

int SaveOptionsDialog::quality() const { return value_->value(); }

void SaveOptionsDialog::estimate()
{
    // Encoding a very large image on every slider move would stutter, so the
    // estimate is measured on a scaled down copy and extrapolated.
    const double pixels = double(image_.width()) * image_.height();
    const double limit = 2.0e6;
    Image sample = image_;
    double factor = 1.0;
    if (pixels > limit) {
        factor = std::sqrt(limit / pixels);
        sample = ops::resize(image_, std::max(1, int(image_.width() * factor)),
                             std::max(1, int(image_.height() * factor)), true);
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, QFileInfo(path_).suffix().toLatin1().toUpper());
    writer.setQuality(quality());
    QImage out = sample.toQImage();
    if (QFileInfo(path_).suffix().compare(QLatin1String("jpg"), Qt::CaseInsensitive) == 0
        || QFileInfo(path_).suffix().compare(QLatin1String("jpeg"), Qt::CaseInsensitive) == 0)
        out = out.convertToFormat(QImage::Format_RGB888);
    if (!writer.write(out)) {
        estimate_->setText(tr("File size estimate is not available."));
        return;
    }
    const double bytes = encoded.size() / (factor * factor);
    estimate_->setText(tr("Estimated file size: %1 KB")
                           .arg(bytes / 1024.0, 0, 'f', bytes > 1024 * 100 ? 0 : 1));
}

// ----------------------------------------------------------------- about

void showAboutDialog(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("Dialogs", "About Tessel"));
    dialog.setWindowIcon(icons::appIcon());
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(10);

    auto* header = new QHBoxLayout;
    auto* logo = new QLabel(&dialog);
    logo->setPixmap(icons::appIcon().pixmap(64, 64));
    header->addWidget(logo);
    auto* titles = new QVBoxLayout;
    auto* title = new QLabel(
        QCoreApplication::translate("Dialogs", "<b style='font-size:16pt'>Tessel</b>"),
        &dialog);
    auto* version = new QLabel(QCoreApplication::translate("Dialogs", "Version %1").arg(
                                   QString::fromLatin1(TESSEL_VERSION_STRING)),
                               &dialog);
    titles->addWidget(title);
    titles->addWidget(version);
    titles->addStretch(1);
    header->addLayout(titles, 1);
    root->addLayout(header);

    auto* text = new QTextBrowser(&dialog);
    text->setOpenExternalLinks(false);
    text->setHtml(
        QCoreApplication::translate(
            "Dialogs",
            "<p>A layered raster image editor for Linux, built with C++ and "
            "Qt 6.</p>"
            "<p>Tessel is an independent program written from scratch. It is not "
            "affiliated with, derived from, or endorsed by Paint.NET or dotPDN LLC; "
            "it simply follows a similar way of working.</p>"
            "<p><b>Formats</b><br>Native layered format (.tessel), OpenRaster (.ora) "
            "for exchanging layered images with Krita, GIMP and MyPaint, plus PNG, "
            "JPEG, BMP, TIFF, WebP, TGA, GIF and ICO.</p>"
            "<p><b>Built with</b><br>Qt %1, zlib, and the C++ standard library.</p>")
            .arg(QString::fromLatin1(qVersion())));
    text->setMinimumSize(430, 220);
    root->addWidget(text);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    root->addWidget(buttons);
    dialog.exec();
}

void showShortcutsDialog(QWidget* parent, const QList<ShortcutEntry>& entries)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("Dialogs", "Keyboard Shortcuts"));
    dialog.setWindowIcon(icons::get(QStringLiteral("settings"), 22));
    auto* root = new QVBoxLayout(&dialog);
    auto* text = new QTextBrowser(&dialog);

    // Group in first appearance order so the sections follow the menu bar.
    QStringList order;
    QHash<QString, QList<ShortcutEntry>> grouped;
    for (const ShortcutEntry& entry : entries) {
        if (!grouped.contains(entry.group))
            order.append(entry.group);
        grouped[entry.group].append(entry);
    }

    QString html;
    for (const QString& group : order) {
        html += QStringLiteral("<h3>%1</h3><table cellpadding='3'>").arg(group.toHtmlEscaped());
        for (const ShortcutEntry& entry : grouped.value(group)) {
            html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                        .arg(entry.keys.toHtmlEscaped(), entry.name.toHtmlEscaped());
        }
        html += QStringLiteral("</table>");
    }
    text->setHtml(html);
    text->setMinimumSize(560, 520);
    root->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    root->addWidget(buttons);
    dialog.exec();
}

} // namespace tessel
