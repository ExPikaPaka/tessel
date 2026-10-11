// Renders the interface to PNG files for the documentation.
//
// Built only when TESSEL_BUILD_SCREENSHOT_TOOL is on, and run with the
// offscreen platform plugin so it needs no display:
//
//     tessel_screenshots docs/screenshots
//
// It also renders a contact sheet of every icon, which is how the icon set is
// reviewed:
//
//     tessel_screenshots --icons /tmp/icons.png
#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QTimer>

#include "core/Document.h"
#include "core/History.h"
#include "core/ImageOps.h"
#include "core/Recovery.h"
#include "effects/Effect.h"
#include "ui/CanvasView.h"
#include "ui/ColorPanel.h"
#include "ui/CurveWidget.h"
#include "ui/Dialogs.h"
#include "ui/EffectDialog.h"
#include "tools/Tool.h"
#include "ui/Docks.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"
#include "ui/RecoveryDialog.h"
#include "ui/Theme.h"

using namespace tessel;

namespace {

// The layers the shots reach back into after the artwork is built. Layers live
// in a tree now, so a position in a flat list is no longer a way to find one.
struct Sample {
    int paintUid = 0;       // the layer carrying the mask and the effect stack
    int adjustmentUid = 0;
};

// A small composition with several layers, a selection and some brushwork, so
// the panels in the screenshots have something to show. It carries one of
// everything the Layers panel can draw: a group with children, a layer with a
// mask and a non destructive effect stack, and an adjustment layer on top.
Sample buildSampleArtwork(Document& doc)
{
    const int w = doc.width(), h = doc.height();
    Sample sample;

    Layer* background = doc.layerAt(0);
    background->name = QStringLiteral("Background");
    for (int y = 0; y < h; ++y) {
        Rgba* line = background->image.row(y);
        for (int x = 0; x < w; ++x) {
            const float u = float(x) / w;
            const float v = float(y) / h;
            line[x] = Rgba(toByte(0.18f + 0.55f * u * (1.0f - v * 0.5f)),
                           toByte(0.22f + 0.30f * v),
                           toByte(0.55f - 0.35f * u + 0.25f * v), 255);
        }
    }

    // The sky is a group, so the panel has a group to show and the group's own
    // opacity and blend mode have something to act on.
    Layer* sky = doc.addGroup(QStringLiteral("Sky"));
    sky->opacity = 235;

    Layer* clouds = doc.addChild(sky, std::make_unique<Layer>(
                                          Image(w, h), QStringLiteral("Clouds")));
    EffectContext context;
    context.source = &clouds->image;
    context.region = clouds->image.rect();
    context.primary = Rgba(255, 255, 255, 220);
    context.secondary = Rgba(120, 170, 255, 0);
    Effect* cloudEffect = EffectRegistry::instance().byId(QStringLiteral("clouds"));
    ParamValues values = ParamValues::defaults(cloudEffect->params());
    values.set(QStringLiteral("scale"), 220.0);
    clouds->image = cloudEffect->apply(context, values);
    clouds->opacity = 150;
    clouds->mode = BlendMode::Screen;

    Layer* sun = doc.addChild(sky, std::make_unique<Layer>(Image(w, h),
                                                           QStringLiteral("Sun")));
    sun->mode = BlendMode::Screen;
    const double sunX = w * 0.74, sunY = h * 0.24, sunR = h * 0.34;
    for (int y = 0; y < h; ++y) {
        Rgba* line = sun->image.row(y);
        for (int x = 0; x < w; ++x) {
            const double dx = x - sunX, dy = y - sunY;
            const double d = std::sqrt(dx * dx + dy * dy) / sunR;
            if (d >= 1.0)
                continue;
            const float falloff = float((1.0 - d) * (1.0 - d));
            line[x] = Rgba(255, 228, 170, toByte(falloff * 0.85f));
        }
    }

    Layer* paint = doc.addChild(doc.rootGroup(),
                                std::make_unique<Layer>(Image(w, h),
                                                        QStringLiteral("Brush work")));
    sample.paintUid = paint->uid();
    const std::vector<QPointF> stroke = {
        {w * 0.12, h * 0.72}, {w * 0.28, h * 0.42}, {w * 0.44, h * 0.62},
        {w * 0.60, h * 0.30}, {w * 0.78, h * 0.55}, {w * 0.90, h * 0.34}};
    Mask mask = ops::strokeMask(w, h, stroke, 26.0, 0.65);
    ops::colorOver(paint->image, mask, Rgba(255, 214, 92, 255), BlendMode::Normal,
                   paint->image.rect());
    const std::vector<QPointF> second = {{w * 0.18, h * 0.30}, {w * 0.70, h * 0.78}};
    Mask mask2 = ops::strokeMask(w, h, second, 14.0, 0.5);
    ops::colorOver(paint->image, mask2, Rgba(255, 110, 92, 230), BlendMode::Normal,
                   paint->image.rect());

    // A mask that fades the strokes away towards the left edge. In the program
    // it would be painted with the ordinary tools; here the grey is written
    // straight in, which is the same thing by the time it is composited.
    Image maskPixels(w, h);
    for (int y = 0; y < h; ++y) {
        Rgba* line = maskPixels.row(y);
        for (int x = 0; x < w; ++x) {
            const double u = double(x) / w;
            const double ramp = std::clamp((u - 0.08) / 0.38, 0.0, 1.0);
            const uint8_t grey = toByte(float(ramp));
            line[x] = Rgba(grey, grey, grey, 255);
        }
    }
    doc.setMask(paint, std::move(maskPixels));

    // Two effects the layer carries rather than has baked in: the stored pixels
    // stay the bare strokes, and the stack runs on the way into the composite.
    EffectRegistry& registry = EffectRegistry::instance();
    std::vector<LayerEffect> stack;
    if (Effect* shadow = registry.byId(QStringLiteral("drop_shadow")))
        stack.push_back({shadow->id(), ParamValues::defaults(shadow->params()), true});
    if (Effect* glow = registry.byId(QStringLiteral("object_glow"))) {
        ParamValues glowValues = ParamValues::defaults(glow->params());
        glowValues.set(QStringLiteral("radius"), 18.0);
        stack.push_back({glow->id(), glowValues, true});
    }
    doc.setLayerEffects(paint, stack);

    // An adjustment layer on top, which acts on everything below it rather
    // than on pixels of its own.
    if (Effect* hue = registry.byId(QStringLiteral("hue_saturation"))) {
        ParamValues hueValues = ParamValues::defaults(hue->params());
        hueValues.set(QStringLiteral("saturation"), 145);
        hueValues.set(QStringLiteral("hue"), -8);
        Layer* adjustment = doc.addAdjustmentLayer(hue->id(), hueValues, hue->name());
        sample.adjustmentUid = adjustment->uid();
    }

    // An elliptical selection, so the marching ants and the status bar readout
    // both appear.
    QPainterPath path;
    path.addEllipse(QRectF(w * 0.30, h * 0.22, w * 0.42, h * 0.46));
    Mask selection(w, h);
    {
        QImage canvas(w, h, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::white);
        painter.setPen(Qt::NoPen);
        painter.drawPath(path);
        painter.end();
        for (int y = 0; y < h; ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(canvas.constScanLine(y));
            float* dst = selection.row(y);
            for (int x = 0; x < w; ++x)
                dst[x] = qAlpha(line[x]) * (1.0f / 255.0f);
        }
    }
    doc.selection().setMask(std::move(selection));
    doc.setActiveIndex(doc.indexOfUid(sample.paintUid));
    doc.setTitle(QStringLiteral("harbour.tessel"));
    doc.invalidate();
    doc.setModified(false);
    return sample;
}


// Grabs one widget with a little breathing room around it, so panel closeups do
// not look cropped to the pixel.
void shotWidget(QWidget* widget, const QString& path)
{
    if (!widget)
        return;
    widget->grab().save(path);
    qInfo("wrote %s", qPrintable(path));
}

// Lays several effect results out in a labelled grid, which says more about
// what the program can do than any single screenshot.
QImage buildEffectsGallery(const Image& subject)
{
    struct Entry {
        const char* id;
        const char* label;
    };
    const Entry entries[] = {
        {"", "Original"},          {"oil_painting", "Oil Painting"},
        {"pencil_sketch", "Pencil Sketch"}, {"gaussian_blur", "Gaussian Blur"},
        {"motion_blur", "Motion Blur"},     {"twist", "Twist"},
        {"bulge", "Bulge"},                 {"crystalize", "Crystalize"},
        {"emboss", "Emboss"},               {"edge_detect", "Edge Detect"},
        {"quantize", "Quantize"},           {"vignette", "Vignette"},
    };
    constexpr int kColumns = 4;
    const int cell = 210;
    const int labelHeight = 22;
    const int rows = (int(std::size(entries)) + kColumns - 1) / kColumns;

    QImage sheet(kColumns * cell, rows * (cell + labelHeight),
                 QImage::Format_ARGB32_Premultiplied);
    sheet.fill(theme::windowColor());
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QFont font = painter.font();
    font.setPointSizeF(9.0);
    painter.setFont(font);

    for (int i = 0; i < int(std::size(entries)); ++i) {
        const int column = i % kColumns;
        const int row = i / kColumns;
        const QRect box(column * cell, row * (cell + labelHeight), cell, cell);

        Image result = subject;
        if (entries[i].id[0] != '\0') {
            Effect* effect = EffectRegistry::instance().byId(
                QString::fromLatin1(entries[i].id));
            if (effect) {
                EffectContext context;
                context.source = &subject;
                context.region = subject.rect();
                context.primary = Rgba(255, 214, 92, 255);
                context.secondary = Rgba(40, 60, 120, 255);
                result = effect->apply(context, ParamValues::defaults(effect->params()));
            }
        }
        const QImage shown = result.toQImage().scaled(box.size(), Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation);
        painter.drawImage(box.x() + (box.width() - shown.width()) / 2,
                          box.y() + (box.height() - shown.height()) / 2, shown);
        painter.setPen(theme::gridColor());
        painter.drawRect(box.adjusted(0, 0, -1, -1));
        painter.setPen(theme::textColor());
        painter.drawText(QRect(box.x(), box.bottom(), box.width(), labelHeight),
                         Qt::AlignCenter, QString::fromLatin1(entries[i].label));
    }
    painter.end();
    return sheet;
}

void save(QWidget* widget, const QString& path)
{
    widget->grab().save(path);
    qInfo("wrote %s", qPrintable(path));
}

// The tool options bar is one long row, and the brush filled it out when it
// gained the tip, stabilisation and pressure controls: scaled down to the
// width of a page column its labels stop being readable. So a bar that is too
// wide to show at its own size is cut in two and the halves are stacked.
//
// The cut is made at a column that holds nothing but the bar's background, so
// it never falls through a control. If there is no such column the bar is left
// as it is, since a cut through the middle of a spin box would be worse than a
// wide picture.
QImage foldStrip(const QImage& strip)
{
    constexpr int kTooWide = 1200;
    if (strip.width() <= kTooWide)
        return strip;
    const QRgb background = strip.pixel(strip.width() - 2, 2);
    const int middle = strip.width() / 2;
    int cut = -1;
    for (int offset = 0; offset < strip.width() / 6; ++offset) {
        for (int x : {middle + offset, middle - offset}) {
            if (x < 1 || x >= strip.width() - 1)
                continue;
            bool clear = true;
            for (int y = 1; y < strip.height() - 1 && clear; ++y)
                clear = strip.pixel(x, y) == background;
            if (clear) {
                cut = x;
                break;
            }
        }
        if (cut >= 0)
            break;
    }
    if (cut < 0)
        return strip;

    const int gap = 4;
    const int width = std::max(cut, strip.width() - cut);
    QImage folded(width, strip.height() * 2 + gap, QImage::Format_ARGB32);
    folded.fill(background);
    QPainter painter(&folded);
    painter.drawImage(0, 0, strip, 0, 0, cut, strip.height());
    painter.drawImage(0, strip.height() + gap, strip, cut, 0,
                      strip.width() - cut, strip.height());
    painter.end();
    return folded;
}

// What a layer mask does, in three pictures: the pixels the layer stores, the
// grey the mask holds, and what reaches the canvas. A mask is the one feature
// here whose point is invisible in a screenshot of the window, because all the
// window shows is the result.
QImage buildMaskFigure(const Document& doc, const Layer& layer)
{
    struct Panel {
        QImage picture;
        const char* label;
        bool checkered;
    };
    const Panel panels[] = {
        {layer.image.toQImage(), "The layer", true},
        {layer.maskImage() ? layer.maskImage()->toQImage() : QImage(), "Its mask", false},
        {doc.renderLayer(layer).toQImage(), "What is composited", true},
    };

    const int cell = 280;
    const int labelHeight = 22;
    const int gap = 10;
    QImage sheet(cell * 3 + gap * 4, cell + labelHeight + gap * 2,
                 QImage::Format_ARGB32_Premultiplied);
    sheet.fill(theme::windowColor());
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QFont font = painter.font();
    font.setPointSizeF(9.0);
    painter.setFont(font);

    for (int i = 0; i < 3; ++i) {
        const QRect box(gap + i * (cell + gap), gap, cell, cell);
        const QImage shown = panels[i].picture.scaled(box.size(), Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation);
        const QRect at(box.x() + (box.width() - shown.width()) / 2,
                       box.y() + (box.height() - shown.height()) / 2,
                       shown.width(), shown.height());
        if (panels[i].checkered) {
            // The same checker the canvas uses, so transparent reads as
            // transparent rather than as a colour the layer does not have.
            for (int y = at.top(); y <= at.bottom(); y += 8) {
                for (int x = at.left(); x <= at.right(); x += 8) {
                    const bool odd = ((x - at.left()) / 8 + (y - at.top()) / 8) % 2;
                    painter.fillRect(QRect(x, y, 8, 8).intersected(at),
                                     odd ? QColor(0x4a, 0x4c, 0x52)
                                         : QColor(0x3a, 0x3c, 0x42));
                }
            }
        }
        painter.drawImage(at.topLeft(), shown);
        painter.setPen(theme::gridColor());
        painter.drawRect(at.adjusted(0, 0, -1, -1));
        painter.setPen(theme::textColor());
        painter.drawText(QRect(box.x(), box.bottom(), box.width(), labelHeight),
                         Qt::AlignCenter, QString::fromLatin1(panels[i].label));
    }
    painter.end();
    return sheet;
}


// One contact sheet of every glyph: each tile shows the icon at the sizes the
// interface actually asks for, over both themes, plus a four times nearest
// neighbour blow up of the 16 px rendering so the pixel grid can be judged.
void writeContactSheet(const QString& path)
{
    const QStringList names = icons::names();

    const int tileWidth = 176;
    const int bandHeight = 72;
    const int labelHeight = 18;
    const int tileHeight = bandHeight * 2 + labelHeight;
    const int columns = 8;
    const int rows = (names.size() + columns - 1) / columns;

    QImage sheet(tileWidth * columns, tileHeight * rows, QImage::Format_ARGB32);
    sheet.fill(QColor(0x15, 0x16, 0x1a));
    QPainter p(&sheet);
    QFont label = p.font();
    label.setPointSizeF(7.5);
    p.setFont(label);

    // The two bands use the real theme backgrounds and foregrounds, so a glyph
    // that disappears on one of them disappears here too.
    struct Band {
        QColor background;
        QColor foreground;
    };
    const Band bands[2] = {{QColor(0x2b, 0x2d, 0x31), QColor(0xe6, 0xe8, 0xeb)},
                           {QColor(0xef, 0xf1, 0xf3), QColor(0x1c, 0x1e, 0x21)}};
    const int sizes[3] = {16, 20, 32};

    for (int index = 0; index < names.size(); ++index) {
        const QString& name = names.at(index);
        const int originX = (index % columns) * tileWidth;
        const int originY = (index / columns) * tileHeight;

        for (int band = 0; band < 2; ++band) {
            const int top = originY + band * bandHeight;
            p.fillRect(QRect(originX, top, tileWidth, bandHeight),
                       bands[band].background);
            const int middle = top + bandHeight / 2;
            int x = originX + 10;
            for (int size : sizes) {
                p.drawPixmap(x, middle - size / 2,
                             icons::pixmap(name, size, bands[band].foreground));
                x += size + 8;
            }
            const QPixmap small = icons::pixmap(name, 16, bands[band].foreground);
            p.drawPixmap(originX + 102, middle - 32,
                         small.scaled(64, 64, Qt::IgnoreAspectRatio,
                                      Qt::FastTransformation));
        }

        p.fillRect(QRect(originX, originY + bandHeight * 2, tileWidth, labelHeight),
                   QColor(0x15, 0x16, 0x1a));
        p.setPen(QColor(0xcf, 0xd3, 0xd8));
        p.drawText(QRect(originX, originY + bandHeight * 2, tileWidth, labelHeight),
                   Qt::AlignCenter, name);

        p.setPen(QColor(0x15, 0x16, 0x1a));
        p.drawRect(QRect(originX, originY, tileWidth - 1, tileHeight - 1));
    }
    p.end();
    sheet.save(path);
    qInfo("wrote %s (%d glyphs)", qPrintable(path), int(names.size()));
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("TesselShots"));
    QApplication::setApplicationName(QStringLiteral("TesselShots"));
    registerBuiltinEffects();
    registerBuiltinTools();

    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--icons")) {
        theme::apply(app, theme::Mode::Dark);
        writeContactSheet(argc > 2 ? QString::fromLocal8Bit(argv[2])
                                   : QStringLiteral("icons.png"));
        return 0;
    }

    const QString outDir = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("docs/screenshots");
    QDir().mkpath(outDir);

    auto shot = [&outDir](QWidget* widget, const QString& name) {
        save(widget, outDir + QLatin1Char('/') + name + QStringLiteral(".png"));
    };

    for (int pass = 0; pass < 2; ++pass) {
        const bool dark = pass == 0;
        theme::apply(app, dark ? theme::Mode::Dark : theme::Mode::Light);
        icons::clearCache();

        auto window = std::make_unique<MainWindow>();
        // Wide enough for the whole tool options bar, which the brush filled
        // out when it gained the tip, stabilisation and pressure controls. A
        // narrower window only hides the very controls these pictures are for.
        window->resize(1760, 1000);
        window->newDocument(900, 620, Rgba(255, 255, 255, 255));
        window->show();
        app.processEvents();

        // The panels size themselves to what they hold and sit in the four
        // corners; the pictures should show the arrangement the program
        // ships with rather than whatever the last run was left in.
        window->resetPanelLayout();
        app.processEvents();

        CanvasView* view = window->findChildren<CanvasView*>().last();
        const Sample sample = buildSampleArtwork(*view->document());
        view->zoomToFit();
        view->refresh();
        // The artwork was built behind the window's back, so the layer list,
        // the thumbnail strip and the status bar need telling about it.
        view->notifyDocumentChanged();
        if (ColorPanel* colors = window->findChild<ColorPanel*>()) {
            colors->setPrimary(Rgba(255, 214, 92, 255));
            colors->setSecondary(Rgba(40, 60, 120, 255));
        }
        app.processEvents();
        shot(window.get(), dark ? QStringLiteral("main-dark")
                                : QStringLiteral("main-light"));

        if (dark) {
            // The two hand built adjustment dialogs and one generated effect
            // dialog, which between them show both dialog styles.
            Document* doc = view->document();
            History* history = view->history();
            CurvesDialog curves(EffectRegistry::instance().byId(QStringLiteral("curves")),
                                doc, history, Rgba(0, 0, 0, 255),
                                Rgba(255, 255, 255, 255));
            curves.show();
            app.processEvents();
            shot(&curves, QStringLiteral("dialog-curves"));
            curves.reject();

            LevelsDialog levels(EffectRegistry::instance().byId(QStringLiteral("levels")),
                                doc, history, Rgba(0, 0, 0, 255),
                                Rgba(255, 255, 255, 255));
            levels.show();
            app.processEvents();
            shot(&levels, QStringLiteral("dialog-levels"));
            levels.reject();

            EffectDialog effect(EffectRegistry::instance().byId(QStringLiteral("clouds")),
                                doc, history, Rgba(255, 214, 92, 255),
                                Rgba(40, 60, 120, 255));
            effect.show();
            app.processEvents();
            shot(&effect, QStringLiteral("dialog-effect"));
            effect.reject();

            // Layer Properties with the brush layer's effect stack in it. The
            // dialog reads the active layer, so the document is pointed at it
            // first and left there afterwards.
            doc->setActiveIndex(doc->indexOfUid(sample.paintUid));
            LayerPropertiesDialog properties(doc, history);
            properties.show();
            app.processEvents();
            shot(&properties, QStringLiteral("dialog-layer-properties"));
            properties.reject();

            // The parameters of the adjustment layer, reopened the way a double
            // click on its row reopens them.
            if (Layer* adjustment = doc->layerByUid(sample.adjustmentUid)) {
                Effect* stored = EffectRegistry::instance().byId(
                    adjustment->effects().front().effectId);
                StoredEffectDialog storedDialog(stored, doc, history, adjustment, 0);
                storedDialog.show();
                app.processEvents();
                shot(&storedDialog, QStringLiteral("dialog-adjustment-layer"));
                storedDialog.reject();
            }

            // The recovery offer, with the entries a crashed session would have
            // left behind. Nothing is read from disk: the dialog is handed the
            // same list findAbandoned() would have handed it.
            {
                std::vector<recovery::Entry> entries;
                recovery::Entry first;
                first.path = QStringLiteral("harbour.tessel");
                first.originalPath = QStringLiteral("/home/you/Pictures/harbour.tessel");
                first.title = QStringLiteral("harbour.tessel");
                first.size = 4 * 1024 * 1024 + 310 * 1024;
                first.savedAt = QDateTime::currentDateTime().addSecs(-180);
                entries.push_back(first);
                recovery::Entry second;
                second.path = QStringLiteral("untitled.tessel");
                second.title = QStringLiteral("Untitled 2");
                second.size = 820 * 1024;
                second.savedAt = QDateTime::currentDateTime().addSecs(-420);
                entries.push_back(second);
                RecoveryDialog recoveryDialog(entries);
                recoveryDialog.show();
                app.processEvents();
                shot(&recoveryDialog, QStringLiteral("dialog-recovery"));
                recoveryDialog.chooseLater();
            }

            // Panel closeups.
            const QString dir = outDir + QLatin1Char('/');
            shotWidget(window->findChild<ToolsPalette*>(), dir + "panel-tools.png");
            shotWidget(window->findChild<ColorPanel*>(), dir + "panel-colors.png");
            shotWidget(window->findChild<LayersPanel*>(), dir + "panel-layers.png");
            shotWidget(window->findChild<HistoryPanel*>(), dir + "panel-history.png");

            // What a mask does, which the window alone cannot show.
            if (const Layer* masked = doc->layerByUid(sample.paintUid)) {
                buildMaskFigure(*doc, *masked).save(dir + "layer-mask.png");
                qInfo("wrote %s", qPrintable(dir + "layer-mask.png"));
            }

            // The tool options bar changes completely per tool, so show several.
            auto* options = window->findChild<ToolOptionsBar*>();
            const char* optionTools[] = {"brush",  "pencil", "eraser",
                                         "gradient", "shapes", "text"};
            for (const char* id : optionTools) {
                Tool* tool = ToolRegistry::instance().byId(QString::fromLatin1(id));
                if (!tool || !options)
                    continue;
                options->startTool(tool);
                options->setToolList(ToolsPalette::toolOrder(), tool->id());
                // The bar drops the previous tool's controls with deleteLater,
                // which processEvents on its own does not act on, so without
                // this the widgets of the tool before are still on top of the
                // new ones when the grab is taken.
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                app.processEvents();
                const QString path = dir + QStringLiteral("options-%1.png")
                                               .arg(QString::fromLatin1(id));
                foldStrip(options->grab().toImage()).save(path);
                qInfo("wrote %s", qPrintable(path));
            }

            // The canvas on its own, with the selection showing.
            shotWidget(view->viewport(), dir + "canvas-selection.png");

            // The rotate and zoom gizmo, mid transform.
            if (Tool* gizmo = ToolRegistry::instance().byId(QStringLiteral("transform"))) {
                doc->selection().clear();
                view->setTool(gizmo);
                gizmo->optionChanged(QStringLiteral("angle"), 18.0);
                gizmo->optionChanged(QStringLiteral("zoom"), 0.78);
                ToolContext gizmoContext = view->makeContext();
                gizmo->press(gizmoContext, ToolEvent{});
                gizmo->release(gizmoContext, ToolEvent{});
                view->refresh();
                app.processEvents();
                shotWidget(view->viewport(), dir + "gizmo.png");
                gizmo->cancel(gizmoContext);
                view->setTool(ToolRegistry::instance().byId(QStringLiteral("brush")));
            }

            // A gallery of what the effects do to one subject.
            buildEffectsGallery(doc->composite()).save(dir + "effects-gallery.png");
            qInfo("wrote %s", qPrintable(dir + "effects-gallery.png"));

            // Frames for the animated demo: a stroke appearing, then an effect.
            // The options bar is put back on the Paintbrush first, since that
            // is the tool the frames are pretending to show.
            if (Tool* brush = ToolRegistry::instance().byId(QStringLiteral("brush"))) {
                if (options) {
                    options->startTool(brush);
                    options->setToolList(ToolsPalette::toolOrder(), brush->id());
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    app.processEvents();
                }
            }
            QDir().mkpath(outDir + QStringLiteral("/demo"));
            // The topmost layer is the adjustment layer now, and it has no
            // pixels to paint into, so the stroke goes where it was always
            // meant to go.
            Layer* canvasLayer = doc->layerByUid(sample.paintUid);
            const Image clean = canvasLayer->image;
            const std::vector<QPointF> path = {
                {120, 430}, {240, 250}, {380, 400}, {520, 210}, {680, 360}, {820, 200}};
            for (int frame = 0; frame < 14; ++frame) {
                if (frame < 10) {
                    const double progress = (frame + 1) / 10.0;
                    std::vector<QPointF> partial;
                    const double total = (path.size() - 1) * progress;
                    for (size_t i = 0; i < path.size(); ++i) {
                        if (double(i) <= total)
                            partial.push_back(path[i]);
                    }
                    if (partial.size() < 2)
                        partial = {path[0], path[0]};
                    canvasLayer->image = clean;
                    Mask stroke = ops::strokeMask(doc->width(), doc->height(), partial,
                                                  30.0, 0.55);
                    ops::colorOver(canvasLayer->image, stroke, Rgba(120, 220, 255, 255),
                                   BlendMode::Normal, canvasLayer->image.rect());
                } else {
                    // Then ripple an effect in over the last few frames.
                    Effect* twist = EffectRegistry::instance().byId(QStringLiteral("twist"));
                    EffectContext context;
                    context.source = &canvasLayer->image;
                    context.region = canvasLayer->image.rect();
                    ParamValues values = ParamValues::defaults(twist->params());
                    values.set(QStringLiteral("amount"), (frame - 9) * 22.0);
                    canvasLayer->image = twist->apply(context, values);
                }
                doc->invalidate();
                view->refresh();
                view->notifyDocumentChanged();
                app.processEvents();
                window->grab().save(outDir
                                    + QStringLiteral("/demo/frame%1.png")
                                          .arg(frame, 2, 10, QLatin1Char('0')));
            }
            canvasLayer->image = clean;
            doc->invalidate();
            qInfo("wrote demo frames");
        }
        // Deliberately not closed: closing asks about unsaved changes, and in
        // a headless run there is nobody to answer the prompt. Letting the
        // window go out of scope destroys it without a close event.
        window->hide();
        app.processEvents();
    }
    return 0;
}
