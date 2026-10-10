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
#include <QApplication>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QTimer>

#include "core/Document.h"
#include "core/History.h"
#include "core/ImageOps.h"
#include "effects/Effect.h"
#include "ui/CanvasView.h"
#include "ui/ColorPanel.h"
#include "ui/CurveWidget.h"
#include "ui/Dialogs.h"
#include "ui/EffectDialog.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

using namespace tessel;

namespace {

// A small composition with several layers, a selection and some brushwork, so
// the panels in the screenshots have something to show.
void buildSampleArtwork(Document& doc)
{
    const int w = doc.width(), h = doc.height();

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

    Layer* clouds = doc.addLayer(std::make_unique<Layer>(Image(w, h),
                                                         QStringLiteral("Clouds")));
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

    Layer* paint = doc.addLayer(std::make_unique<Layer>(Image(w, h),
                                                        QStringLiteral("Brush work")));
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
    doc.setActiveIndex(2);
    doc.setTitle(QStringLiteral("harbour.tessel"));
    doc.invalidate();
    doc.setModified(false);
}

void save(QWidget* widget, const QString& path)
{
    widget->grab().save(path);
    qInfo("wrote %s", qPrintable(path));
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
        window->resize(1280, 820);
        window->newDocument(900, 620, Rgba(255, 255, 255, 255));
        window->show();
        app.processEvents();

        CanvasView* view = window->findChildren<CanvasView*>().last();
        buildSampleArtwork(*view->document());
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
        }
        // Deliberately not closed: closing asks about unsaved changes, and in
        // a headless run there is nobody to answer the prompt. Letting the
        // window go out of scope destroys it without a close event.
        window->hide();
        app.processEvents();
    }
    return 0;
}
