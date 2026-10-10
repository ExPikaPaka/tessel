// Renders the interface to PNG files for the documentation.
//
// Built only when TESSEL_BUILD_SCREENSHOT_TOOL is on, and run with the
// offscreen platform plugin so it needs no display:
//
//     tessel_screenshots docs/screenshots
#include <QApplication>
#include <QDir>
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

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("TesselShots"));
    QApplication::setApplicationName(QStringLiteral("TesselShots"));
    registerBuiltinEffects();
    registerBuiltinTools();

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
