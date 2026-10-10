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
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
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
#include "tessel_version.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tessel {

namespace {

struct Preset {
    const char* name;
    int width;
    int height;
};

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"Custom", 0, 0},
        {"Screen 1920 x 1080", 1920, 1080},
        {"Screen 2560 x 1440", 2560, 1440},
        {"Screen 3840 x 2160", 3840, 2160},
        {"Square 512 x 512", 512, 512},
        {"Square 1024 x 1024", 1024, 1024},
        {"Icon 256 x 256", 256, 256},
        {"Portrait A4 at 150 dpi", 1240, 1754},
        {"Landscape A4 at 150 dpi", 1754, 1240},
        {"Photo 4 x 6 at 300 dpi", 1800, 1200},
        {"Social post 1080 x 1080", 1080, 1080},
        {"Social story 1080 x 1920", 1080, 1920},
    };
    return list;
}

QString describeSize(int width, int height)
{
    const double megapixels = double(width) * height / 1.0e6;
    const double megabytes = double(width) * height * 4 / (1024.0 * 1024.0);
    return QStringLiteral("%1 x %2 pixels  -  %3 MP  -  %4 MB per layer")
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
    setWindowTitle(QStringLiteral("New Image"));
    setWindowIcon(icons::get(QStringLiteral("new"), 22));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    preset_ = new QComboBox(this);
    for (const Preset& item : presets())
        preset_->addItem(QString::fromLatin1(item.name));
    form->addRow(QStringLiteral("Preset"), preset_);

    width_ = new QSpinBox(this);
    width_->setRange(1, 60000);
    width_->setValue(defaultWidth);
    width_->setSuffix(QStringLiteral(" px"));
    height_ = new QSpinBox(this);
    height_->setRange(1, 60000);
    height_->setValue(defaultHeight);
    height_->setSuffix(QStringLiteral(" px"));
    form->addRow(QStringLiteral("Width"), width_);
    form->addRow(QStringLiteral("Height"), height_);

    keepRatio_ = new QCheckBox(QStringLiteral("Maintain aspect ratio"), this);
    form->addRow(QString(), keepRatio_);

    background_ = new QComboBox(this);
    background_->addItems({QStringLiteral("White"), QStringLiteral("Transparent"),
                           QStringLiteral("Secondary colour"), QStringLiteral("Black")});
    form->addRow(QStringLiteral("Background"), background_);
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

ResizeDialog::ResizeDialog(const QString& title, int currentWidth, int currentHeight,
                           bool withAnchor, QWidget* parent)
    : QDialog(parent), originalWidth_(currentWidth), originalHeight_(currentHeight)
{
    setWindowTitle(title);
    setWindowIcon(icons::get(withAnchor ? QStringLiteral("canvas")
                                        : QStringLiteral("resize"), 22));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* mode = new QGroupBox(QStringLiteral("Resize by"), this);
    auto* modeLayout = new QGridLayout(mode);
    byPercent_ = new QRadioButton(QStringLiteral("Percentage"), mode);
    byAbsolute_ = new QRadioButton(QStringLiteral("Absolute size"), mode);
    byAbsolute_->setChecked(true);
    percent_ = new QSpinBox(mode);
    percent_->setRange(1, 2000);
    percent_->setValue(100);
    percent_->setSuffix(QStringLiteral("%"));
    percent_->setEnabled(false);
    modeLayout->addWidget(byPercent_, 0, 0);
    modeLayout->addWidget(percent_, 0, 1);
    modeLayout->addWidget(byAbsolute_, 1, 0, 1, 2);
    root->addWidget(mode);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    width_ = new QSpinBox(this);
    width_->setRange(1, 60000);
    width_->setValue(currentWidth);
    width_->setSuffix(QStringLiteral(" px"));
    height_ = new QSpinBox(this);
    height_->setRange(1, 60000);
    height_->setValue(currentHeight);
    height_->setSuffix(QStringLiteral(" px"));
    keepRatio_ = new QCheckBox(QStringLiteral("Maintain aspect ratio"), this);
    keepRatio_->setChecked(true);
    form->addRow(QStringLiteral("Width"), width_);
    form->addRow(QStringLiteral("Height"), height_);
    form->addRow(QString(), keepRatio_);
    if (!withAnchor) {
        resampling_ = new QComboBox(this);
        resampling_->addItems({QStringLiteral("Smooth (best quality)"),
                               QStringLiteral("Nearest neighbour (hard pixels)")});
        form->addRow(QStringLiteral("Resampling"), resampling_);
    }
    root->addLayout(form);

    if (withAnchor) {
        auto* group = new QGroupBox(QStringLiteral("Anchor"), this);
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
            button->setToolTip(QString::fromLatin1(anchorName(order[i])));
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

    summary_ = new QLabel(this);
    summary_->setStyleSheet(QStringLiteral("color: %1; font-size: 8pt;")
                                .arg(theme::textColor().darker(140).name()));
    root->addWidget(summary_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(byPercent_, &QRadioButton::toggled, this, [this](bool on) {
        percent_->setEnabled(on);
        width_->setEnabled(!on);
        height_->setEnabled(!on);
        if (on)
            syncFromPercent();
    });
    connect(percent_, &QSpinBox::valueChanged, this, [this] { syncFromPercent(); });
    connect(width_, &QSpinBox::valueChanged, this, [this] { syncFromAbsolute(true); });
    connect(height_, &QSpinBox::valueChanged, this, [this] { syncFromAbsolute(false); });
    updateLabel();
}

void ResizeDialog::syncFromPercent()
{
    if (updating_)
        return;
    updating_ = true;
    const double factor = percent_->value() / 100.0;
    width_->setValue(std::max(1, int(std::lround(originalWidth_ * factor))));
    height_->setValue(std::max(1, int(std::lround(originalHeight_ * factor))));
    updating_ = false;
    updateLabel();
}

void ResizeDialog::syncFromAbsolute(bool widthLed)
{
    if (updating_)
        return;
    if (keepRatio_->isChecked()) {
        updating_ = true;
        const double ratio = double(originalWidth_) / std::max(1, originalHeight_);
        if (widthLed)
            height_->setValue(std::max(1, int(std::lround(width_->value() / ratio))));
        else
            width_->setValue(std::max(1, int(std::lround(height_->value() * ratio))));
        updating_ = false;
    }
    updateLabel();
}

void ResizeDialog::updateLabel()
{
    summary_->setText(QStringLiteral("Was %1 x %2  ->  %3")
                          .arg(originalWidth_).arg(originalHeight_)
                          .arg(describeSize(width_->value(), height_->value())));
}

int ResizeDialog::newWidth() const { return width_->value(); }
int ResizeDialog::newHeight() const { return height_->value(); }

bool ResizeDialog::smoothResampling() const
{
    return resampling_ ? resampling_->currentIndex() == 0 : true;
}

// ----------------------------------------------------------------- layer props

LayerPropertiesDialog::LayerPropertiesDialog(Document* doc, History* history,
                                             QWidget* parent)
    : QDialog(parent), doc_(doc), history_(history)
{
    setWindowTitle(QStringLiteral("Layer Properties"));
    setWindowIcon(icons::get(QStringLiteral("layer-props"), 22));
    Layer* layer = doc_->activeLayer();
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
    form->addRow(QStringLiteral("Name"), name_);
    visible_ = new QCheckBox(QStringLiteral("Visible"), this);
    visible_->setChecked(originalVisible_);
    form->addRow(QString(), visible_);
    blend_ = new QComboBox(this);
    blend_->addItems(blendModeNames());
    blend_->setCurrentIndex(int(originalMode_));
    form->addRow(QStringLiteral("Blend mode"), blend_);

    auto* opacityRow = new QHBoxLayout;
    opacity_ = new QSlider(Qt::Horizontal, this);
    opacity_->setRange(0, 255);
    opacity_->setValue(originalOpacity_);
    opacityValue_ = new QSpinBox(this);
    opacityValue_->setRange(0, 255);
    opacityValue_->setValue(originalOpacity_);
    opacityRow->addWidget(opacity_, 1);
    opacityRow->addWidget(opacityValue_);
    form->addRow(QStringLiteral("Opacity"), opacityRow);
    root->addLayout(form);

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

void LayerPropertiesDialog::done(int result)
{
    Layer* layer = doc_->layerByUid(layerUid_);
    if (layer) {
        if (result == QDialog::Accepted) {
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
                    QStringLiteral("Layer Properties"), QStringLiteral("layer-props"),
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
    setWindowTitle(QStringLiteral("Rotate / Zoom Layer"));
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
    tile_ = new QCheckBox(QStringLiteral("Tile the result"), this);

    form->addRow(QStringLiteral("Angle"), angle_);
    form->addRow(QStringLiteral("Zoom"), zoom_);
    form->addRow(QStringLiteral("Pan X"), offsetX_);
    form->addRow(QStringLiteral("Pan Y"), offsetY_);
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
            EditScope scope(*doc_, *history_, layer, QStringLiteral("Rotate / Zoom Layer"),
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

SaveOptionsDialog::SaveOptionsDialog(const Image& image, const QString& path,
                                     int quality, QWidget* parent)
    : QDialog(parent), image_(image), path_(path)
{
    setWindowTitle(QStringLiteral("Save Configuration"));
    setWindowIcon(icons::get(QStringLiteral("save"), 22));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    root->addWidget(new QLabel(QStringLiteral("%1 stores the image with lossy "
                                              "compression.")
                                   .arg(QFileInfo(path).suffix().toUpper()),
                               this));

    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Quality"), this));
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
        estimate_->setText(QStringLiteral("File size estimate is not available."));
        return;
    }
    const double bytes = encoded.size() / (factor * factor);
    estimate_->setText(QStringLiteral("Estimated file size: %1 KB")
                           .arg(bytes / 1024.0, 0, 'f', bytes > 1024 * 100 ? 0 : 1));
}

// ----------------------------------------------------------------- about

void showAboutDialog(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("About Tessel"));
    dialog.setWindowIcon(icons::appIcon());
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(10);

    auto* header = new QHBoxLayout;
    auto* logo = new QLabel(&dialog);
    logo->setPixmap(icons::appIcon().pixmap(64, 64));
    header->addWidget(logo);
    auto* titles = new QVBoxLayout;
    auto* title = new QLabel(QStringLiteral("<b style='font-size:16pt'>Tessel</b>"), &dialog);
    auto* version = new QLabel(QStringLiteral("Version %1").arg(
                                   QString::fromLatin1(TESSEL_VERSION_STRING)),
                               &dialog);
    titles->addWidget(title);
    titles->addWidget(version);
    titles->addStretch(1);
    header->addLayout(titles, 1);
    root->addLayout(header);

    auto* text = new QTextBrowser(&dialog);
    text->setOpenExternalLinks(false);
    text->setHtml(QStringLiteral(
        "<p>A layered raster image editor for Linux, built with C++ and Qt 6.</p>"
        "<p>Tessel is an independent program written from scratch. It is not "
        "affiliated with, derived from, or endorsed by Paint.NET or dotPDN LLC; "
        "it simply follows a similar way of working.</p>"
        "<p><b>Formats</b><br>Native layered format (.tessel), OpenRaster (.ora) for "
        "exchanging layered images with Krita, GIMP and MyPaint, plus PNG, JPEG, "
        "BMP, TIFF, WebP, TGA, GIF and ICO.</p>"
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

void showShortcutsDialog(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    dialog.setWindowIcon(icons::get(QStringLiteral("settings"), 22));
    auto* root = new QVBoxLayout(&dialog);
    auto* text = new QTextBrowser(&dialog);
    text->setHtml(QStringLiteral(R"(
<h3>Tools</h3>
<table cellpadding='3'>
<tr><td><b>S</b></td><td>Cycle the selection tools: rectangle, lasso, ellipse, magic wand</td></tr>
<tr><td><b>M</b></td><td>Move Selected Pixels / Move Selection</td></tr>
<tr><td><b>B</b></td><td>Paintbrush</td></tr>
<tr><td><b>P</b></td><td>Pencil</td></tr>
<tr><td><b>E</b></td><td>Eraser</td></tr>
<tr><td><b>F</b></td><td>Paint Bucket</td></tr>
<tr><td><b>G</b></td><td>Gradient</td></tr>
<tr><td><b>K</b></td><td>Color Picker</td></tr>
<tr><td><b>L</b></td><td>Clone Stamp</td></tr>
<tr><td><b>R</b></td><td>Recolor</td></tr>
<tr><td><b>T</b></td><td>Text</td></tr>
<tr><td><b>O</b></td><td>Line / Curve and Shapes</td></tr>
<tr><td><b>Z</b></td><td>Zoom</td></tr>
<tr><td><b>H</b></td><td>Pan (or hold the space bar with any tool)</td></tr>
</table>
<h3>File and edit</h3>
<table cellpadding='3'>
<tr><td><b>Ctrl+N</b></td><td>New image</td></tr>
<tr><td><b>Ctrl+O</b></td><td>Open</td></tr>
<tr><td><b>Ctrl+S</b></td><td>Save</td></tr>
<tr><td><b>Ctrl+Shift+S</b></td><td>Save as</td></tr>
<tr><td><b>Ctrl+W</b></td><td>Close image</td></tr>
<tr><td><b>Ctrl+Z</b> / <b>Ctrl+Y</b></td><td>Undo / Redo</td></tr>
<tr><td><b>Ctrl+X</b> / <b>Ctrl+C</b> / <b>Ctrl+V</b></td><td>Cut / Copy / Paste</td></tr>
<tr><td><b>Ctrl+Shift+V</b></td><td>Paste into new layer</td></tr>
<tr><td><b>Ctrl+A</b> / <b>Ctrl+Shift+A</b></td><td>Select all / Deselect all</td></tr>
<tr><td><b>Ctrl+I</b></td><td>Invert selection</td></tr>
<tr><td><b>Delete</b> / <b>Backspace</b></td><td>Erase / fill the selection</td></tr>
</table>
<h3>View</h3>
<table cellpadding='3'>
<tr><td><b>Ctrl++</b> / <b>Ctrl+-</b></td><td>Zoom in / out</td></tr>
<tr><td><b>Ctrl+B</b></td><td>Fit the image in the window</td></tr>
<tr><td><b>Ctrl+Shift+B</b></td><td>Actual size</td></tr>
<tr><td><b>Ctrl+G</b></td><td>Pixel grid</td></tr>
<tr><td><b>Ctrl+R</b></td><td>Rulers</td></tr>
<tr><td><b>Ctrl+wheel</b></td><td>Zoom around the pointer</td></tr>
</table>
<h3>Image and layers</h3>
<table cellpadding='3'>
<tr><td><b>Ctrl+Shift+X</b></td><td>Crop to selection</td></tr>
<tr><td><b>Ctrl+R</b></td><td>Resize image</td></tr>
<tr><td><b>Ctrl+Shift+R</b></td><td>Canvas size</td></tr>
<tr><td><b>Ctrl+Shift+F</b></td><td>Flatten</td></tr>
<tr><td><b>Ctrl+Shift+N</b></td><td>Add new layer</td></tr>
<tr><td><b>Ctrl+Shift+D</b></td><td>Duplicate layer</td></tr>
<tr><td><b>Ctrl+M</b></td><td>Merge layer down</td></tr>
<tr><td><b>F4</b></td><td>Layer properties</td></tr>
</table>
<h3>Adjustments</h3>
<table cellpadding='3'>
<tr><td><b>Ctrl+Shift+C</b></td><td>Brightness / contrast</td></tr>
<tr><td><b>Ctrl+Shift+U</b></td><td>Hue / saturation</td></tr>
<tr><td><b>Ctrl+Shift+I</b></td><td>Invert colours</td></tr>
<tr><td><b>Ctrl+Shift+L</b></td><td>Auto level</td></tr>
<tr><td><b>Ctrl+Shift+G</b></td><td>Black and white</td></tr>
<tr><td><b>Ctrl+L</b></td><td>Levels</td></tr>
<tr><td><b>Ctrl+Shift+M</b></td><td>Curves</td></tr>
<tr><td><b>Ctrl+F</b></td><td>Repeat the last effect</td></tr>
</table>
)"));
    text->setMinimumSize(520, 460);
    root->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    root->addWidget(buttons);
    dialog.exec();
}

} // namespace tessel
