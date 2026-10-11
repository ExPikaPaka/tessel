#include "ui/MainWindow.h"

#include <cmath>

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QBuffer>
#include <QMimeData>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QScreen>
#include <QHBoxLayout>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QShortcut>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QAbstractButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/Io.h"
#include "effects/EffectPluginHost.h"
#include "tessel_version.h"
#include "ui/CurveWidget.h"
#include "ui/Dialogs.h"
#include "ui/EffectDialog.h"
#include "ui/Icons.h"
#include "ui/RecoveryDialog.h"
#include "ui/Theme.h"

namespace tessel {

namespace {

constexpr int kMaxRecentFiles = 10;

// Bump this whenever the default panel arrangement changes, so saved layouts
// from older builds are ignored rather than restored into the wrong places.
constexpr int kLayoutVersion = 3;

const QStringList& effectCategoryOrder()
{
    static const QStringList order = {
        QStringLiteral("Artistic"), QStringLiteral("Blurs"), QStringLiteral("Color"),
        QStringLiteral("Distort"), QStringLiteral("Noise"), QStringLiteral("Object"),
        QStringLiteral("Photo"), QStringLiteral("Render"), QStringLiteral("Stylize"),
    };
    return order;
}

// The Effects submenus: the built in categories in their fixed order, then any
// category a plugin declared that is not one of them, so a plugin either joins
// an existing submenu or gets one of its own at the end.
QStringList effectMenuCategories()
{
    QStringList order = effectCategoryOrder();
    for (const QString& category : EffectRegistry::instance().categories()) {
        if (category == QStringLiteral("Adjustments") || order.contains(category))
            continue;
        order.append(category);
    }
    return order;
}

// Effects > About the Plugins. The text comes from the loader so that a plugin
// which failed has somewhere to be read about, rather than only flashing past
// in the status bar.
void showPluginsDialog(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("pluginsDialog"));
    dialog.setWindowTitle(QCoreApplication::translate("Dialogs", "About the Plugins"));
    dialog.setWindowIcon(icons::get(QStringLiteral("effect"), 22));
    auto* root = new QVBoxLayout(&dialog);
    auto* text = new QTextBrowser(&dialog);
    text->setObjectName(QStringLiteral("pluginsText"));
    text->setOpenExternalLinks(false);
    text->setHtml(plugins::report().html());
    text->setMinimumSize(520, 320);
    root->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    root->addWidget(buttons);
    dialog.exec();
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    registerBuiltinEffects();
    // Plugins come straight after, so everything below builds its menus from a
    // registry that is already complete.
    plugins::loadAll();
    registerBuiltinTools();

    setWindowIcon(icons::appIcon());
    setAcceptDrops(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks);

    // The canvas is not the central widget. It is a plain child laid out by
    // hand across the whole band between the tool bars and the status bar,
    // under the panels docked at the left edge, so that a panel which has
    // taken only the height it needs leaves workspace below it rather than a
    // dead strip. The central widget is an empty placeholder: it paints
    // nothing and takes no mouse, and its geometry is what the dock layout
    // leaves over, which is how the canvas knows how much of itself is
    // covered.
    canvasHost_ = new QWidget(this);
    canvasHost_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    canvasHost_->setAttribute(Qt::WA_NoSystemBackground, true);
    setCentralWidget(canvasHost_);
    canvasHost_->installEventFilter(this);

    stack_ = new QStackedWidget(this);

    createActions();
    createMenus();
    createToolBars();
    createPanels();
    createStatusBar();
    stack_->lower(); // the docked panels stand over the canvas, not under it
    layOutCanvas();
    installToolShortcuts();
    restoreSettings();
    setupAutosave();

    lastDirectory_ = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    chooseTool(QStringLiteral("brush"));
    updateEverything();
}

MainWindow::~MainWindow() = default;

// ----------------------------------------------------------------- actions

void MainWindow::createActions()
{
    auto make = [this](const QString& key, const QString& text, const QString& icon,
                       const QKeySequence& shortcut, auto slot) {
        auto* action = new QAction(text, this);
        if (!icon.isEmpty()) {
            action->setIcon(icons::get(icon, 20));
            action->setProperty("iconName", icon);
        }
        if (!shortcut.isEmpty())
            action->setShortcut(shortcut);
        action->setObjectName(key);
        connect(action, &QAction::triggered, this, slot);
        actions_.insert(key, action);
        addAction(action);
        return action;
    };

    // ---------------------------------------------------------------- file
    make(QStringLiteral("new"), tr("&New..."), QStringLiteral("new"),
         QKeySequence::New, [this] {
             Document* doc = currentDocument();
             NewImageDialog dialog(doc ? doc->width() : defaultWidth_,
                                   doc ? doc->height() : defaultHeight_,
                                   colors_ ? colors_->secondary() : Rgba(255, 255, 255, 255),
                                   this);
             if (dialog.exec() == QDialog::Accepted)
                 newDocument(dialog.imageWidth(), dialog.imageHeight(), dialog.background());
         });
    make(QStringLiteral("open"), tr("&Open..."), QStringLiteral("open"),
         QKeySequence::Open, [this] {
             const QStringList paths = QFileDialog::getOpenFileNames(
                 this, tr("Open Image"), lastDirectory_, io::openFilter());
             openPaths(paths);
         });
    make(QStringLiteral("save"), tr("&Save"), QStringLiteral("save"),
         QKeySequence::Save, [this] { saveView(stack_->currentIndex(), false); });
    make(QStringLiteral("saveas"), tr("Save &As..."), QStringLiteral("save"),
         QKeySequence::SaveAs, [this] { saveView(stack_->currentIndex(), true); });
    QAction* closeAction = make(QStringLiteral("close"), tr("&Close"),
                                QStringLiteral("close"), QKeySequence::Close,
                                [this] { closeView(stack_->currentIndex()); });
    closeAction->setShortcuts({QKeySequence(QStringLiteral("Ctrl+W")),
                               QKeySequence(QStringLiteral("Ctrl+F4"))});
    make(QStringLiteral("saveall"), tr("Save A&ll"), QStringLiteral("save"),
         QKeySequence(QStringLiteral("Ctrl+Alt+S")), [this] {
             int saved = 0;
             for (int i = 0; i < stack_->count(); ++i) {
                 if (viewAt(i)->document()->isModified() && saveView(i, false))
                     ++saved;
             }
             hintLabel_->setText(tr("Saved %n image(s)", "", saved));
         });
    make(QStringLiteral("print"), tr("&Print..."), QStringLiteral("image"),
         QKeySequence::Print, [this] { printImage(); });
    make(QStringLiteral("quit"), tr("E&xit"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Q")), [this] { close(); });

    // ---------------------------------------------------------------- edit
    make(QStringLiteral("undo"), tr("&Undo"), QStringLiteral("undo"),
         QKeySequence::Undo, [this] {
             if (CanvasView* view = currentView()) {
                 view->cancelPendingTool();
                 view->history()->undo();
                 view->refresh();
                 updateEverything();
             }
         });
    make(QStringLiteral("redo"), tr("&Redo"), QStringLiteral("redo"),
         QKeySequence(QStringLiteral("Ctrl+Y")), [this] {
             if (CanvasView* view = currentView()) {
                 view->cancelPendingTool();
                 view->history()->redo();
                 view->refresh();
                 updateEverything();
             }
         });
    QAction* cutAction = make(QStringLiteral("cut"), tr("Cu&t"),
                              QStringLiteral("cut"), QKeySequence::Cut,
                              [this] { doCut(); });
    cutAction->setShortcuts({QKeySequence::Cut,
                             QKeySequence(QStringLiteral("Shift+Delete"))});
    QAction* copyAction = make(QStringLiteral("copy"), tr("&Copy"),
                               QStringLiteral("copy"), QKeySequence::Copy,
                               [this] { doCopy(false); });
    copyAction->setShortcuts({QKeySequence::Copy,
                              QKeySequence(QStringLiteral("Ctrl+Insert"))});
    make(QStringLiteral("copymerged"), tr("Copy &Merged"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Shift+C")), [this] { doCopy(true); });
    QAction* pasteAction = make(QStringLiteral("paste"), tr("&Paste"),
                                QStringLiteral("paste"), QKeySequence::Paste,
                                [this] { doPaste(false, false); });
    pasteAction->setShortcuts({QKeySequence::Paste,
                               QKeySequence(QStringLiteral("Shift+Insert"))});
    make(QStringLiteral("pastelayer"), tr("Paste Into New &Layer"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Shift+V")), [this] { doPaste(true, false); });
    make(QStringLiteral("pasteimage"), tr("Paste Into New &Image"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Alt+V")), [this] { doPaste(false, true); });
    make(QStringLiteral("copyselection"), tr("Cop&y Selection"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Alt+Shift+C")), [this] { copySelection(); });
    make(QStringLiteral("pasteselection"), tr("Paste Selection"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Alt+Shift+V")), [this] { pasteSelection(); });
    make(QStringLiteral("selectall"), tr("Select &All"), QStringLiteral("select-all"),
         QKeySequence::SelectAll, [this] {
             if (CanvasView* view = currentView()) {
                 recordSelectionEdit(*view->document(), *view->history(),
                                     tr("Select All"),
                                     [&] { view->document()->selection().selectAll(); });
                 view->refresh();
                 updateEverything();
             }
         });
    QAction* deselectAction = make(QStringLiteral("deselect"), tr("&Deselect All"),
                                   QStringLiteral("deselect"),
                                   QKeySequence(QStringLiteral("Ctrl+D")), [this] {
             if (CanvasView* view = currentView()) {
                 view->commitPendingTool();
                 recordSelectionEdit(*view->document(), *view->history(),
                                     tr("Deselect All"),
                                     [&] { view->document()->selection().clear(); });
                 view->refresh();
                 updateEverything();
             }
         });
    deselectAction->setShortcuts({QKeySequence(QStringLiteral("Ctrl+D")),
                                  QKeySequence(QStringLiteral("Ctrl+Shift+A"))});
    make(QStringLiteral("invertselection"), tr("&Invert Selection"),
         QStringLiteral("invert"), QKeySequence(QStringLiteral("Ctrl+I")), [this] {
             if (CanvasView* view = currentView()) {
                 recordSelectionEdit(*view->document(), *view->history(),
                                     tr("Invert Selection"),
                                     [&] { view->document()->selection().invert(); });
                 view->refresh();
                 updateEverything();
             }
         });
    auto selectionOp = [this](SelectionOpDialog::Op op) {
        CanvasView* view = currentView();
        if (!view || !view->document()->selection().isActive())
            return;
        view->commitPendingTool();
        SelectionOpDialog dialog(op, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        SelectionOpDialog::applyDirect(op, *view->document(), *view->history(),
                                       dialog.amount());
        view->refresh();
        updateEverything();
    };
    make(QStringLiteral("feather"), tr("&Feather..."),
         QStringLiteral("select-ellipse"), QKeySequence(QStringLiteral("Ctrl+Alt+F")),
         [selectionOp] { selectionOp(SelectionOpDialog::Op::Feather); });
    make(QStringLiteral("grow"), tr("&Grow..."),
         QStringLiteral("select-all"), QKeySequence(),
         [selectionOp] { selectionOp(SelectionOpDialog::Op::Grow); });
    make(QStringLiteral("shrink"), tr("S&hrink..."),
         QStringLiteral("deselect"), QKeySequence(),
         [selectionOp] { selectionOp(SelectionOpDialog::Op::Shrink); });
    make(QStringLiteral("border"), tr("&Border..."),
         QStringLiteral("select-rect"), QKeySequence(),
         [selectionOp] { selectionOp(SelectionOpDialog::Op::Border); });
    make(QStringLiteral("freetransform"), tr("Free &Transform"),
         QStringLiteral("move-selected"), QKeySequence(QStringLiteral("Ctrl+T")),
         [this] { chooseTool(QStringLiteral("move-selected")); });
    make(QStringLiteral("erase"), tr("&Erase Selection"), QStringLiteral("delete"),
         QKeySequence::Delete, [this] { eraseSelection(); });
    make(QStringLiteral("fill"), tr("&Fill Selection"), QStringLiteral("bucket"),
         QKeySequence(QStringLiteral("Backspace")), [this] { fillSelection(); });

    // ---------------------------------------------------------------- view
    make(QStringLiteral("zoomin"), tr("Zoom &In"), QStringLiteral("zoom-in"),
         QKeySequence::ZoomIn, [this] {
             if (CanvasView* view = currentView())
                 view->zoomIn();
         });
    make(QStringLiteral("zoomout"), tr("Zoom &Out"), QStringLiteral("zoom-out"),
         QKeySequence::ZoomOut, [this] {
             if (CanvasView* view = currentView())
                 view->zoomOut();
         });
    make(QStringLiteral("zoomfit"), tr("&Fit to Window"), QStringLiteral("fit"),
         QKeySequence(QStringLiteral("Ctrl+B")), [this] {
             if (CanvasView* view = currentView())
                 view->zoomToFit();
         });
    make(QStringLiteral("zoomactual"), tr("&Actual Size"), QStringLiteral("actual"),
         QKeySequence(QStringLiteral("Ctrl+0")), [this] {
             if (CanvasView* view = currentView())
                 view->zoomToActualSize();
         });
    make(QStringLiteral("zoomselection"), tr("Zoom to &Selection"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Shift+B")), [this] {
             if (CanvasView* view = currentView())
                 view->zoomToSelection();
         });

    auto* grid = new QAction(tr("Pixel &Grid"), this);
    grid->setCheckable(true);
    grid->setIcon(icons::get(QStringLiteral("grid"), 20));
    grid->setProperty("iconName", QStringLiteral("grid"));
    connect(grid, &QAction::toggled, this, [this](bool on) {
        for (int i = 0; i < stack_->count(); ++i)
            viewAt(i)->setShowGrid(on);
    });
    grid->setObjectName(QStringLiteral("grid"));
    actions_.insert(QStringLiteral("grid"), grid);
    addAction(grid);

    auto* rulers = new QAction(tr("&Rulers"), this);
    rulers->setCheckable(true);
    connect(rulers, &QAction::toggled, this, [this](bool on) {
        for (int i = 0; i < stack_->count(); ++i)
            viewAt(i)->setShowRulers(on);
    });
    rulers->setObjectName(QStringLiteral("rulers"));
    actions_.insert(QStringLiteral("rulers"), rulers);
    addAction(rulers);

    // One checkable action per measurable unit, in an exclusive group.
    auto* unitGroup = new QActionGroup(this);
    unitGroup->setExclusive(true);
    unitGroup->setObjectName(QStringLiteral("unitgroup"));
    for (Unit candidate : {Unit::Pixels, Unit::Inches, Unit::Centimetres}) {
        auto* action = new QAction(units::unitName(candidate), this);
        action->setCheckable(true);
        action->setChecked(candidate == unit_);
        const QString key = QStringLiteral("unit-") + units::unitId(candidate);
        action->setObjectName(key);
        unitGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, candidate] { setMeasurementUnit(candidate); });
        actions_.insert(key, action);
        addAction(action);
    }

    auto* selectionOutline = new QAction(tr("Selection &Outline"), this);
    selectionOutline->setCheckable(true);
    selectionOutline->setChecked(true);
    connect(selectionOutline, &QAction::toggled, this, [this](bool on) {
        for (int i = 0; i < stack_->count(); ++i)
            viewAt(i)->setShowSelection(on);
    });
    selectionOutline->setObjectName(QStringLiteral("selectionoutline"));
    actions_.insert(QStringLiteral("selectionoutline"), selectionOutline);

    auto* fullScreen = new QAction(tr("F&ull Screen"), this);
    fullScreen->setCheckable(true);
    fullScreen->setShortcut(QKeySequence(QStringLiteral("F11")));
    connect(fullScreen, &QAction::toggled, this, [this](bool on) {
        if (on)
            showFullScreen();
        else
            showNormal();
    });
    fullScreen->setObjectName(QStringLiteral("fullscreen"));
    actions_.insert(QStringLiteral("fullscreen"), fullScreen);
    addAction(fullScreen);

    // ---------------------------------------------------------------- image
    make(QStringLiteral("crop"), tr("&Crop to Selection"), QStringLiteral("crop"),
         QKeySequence(QStringLiteral("Ctrl+Shift+X")), [this] { cropToSelection(); });
    make(QStringLiteral("resize"), tr("&Resize Image..."), QStringLiteral("resize"),
         QKeySequence(QStringLiteral("Ctrl+R")), [this] { resizeImage(); });
    make(QStringLiteral("canvassize"), tr("Canvas &Size..."),
         QStringLiteral("canvas"), QKeySequence(QStringLiteral("Ctrl+Shift+R")),
         [this] { changeCanvasSize(); });
    make(QStringLiteral("fliph"), tr("Flip &Horizontal"), QStringLiteral("flip-h"),
         QKeySequence(), [this] {
             structuralEdit(tr("Flip Horizontal"), QStringLiteral("flip-h"),
                            [this] { currentDocument()->flip(true); });
         });
    make(QStringLiteral("flipv"), tr("Flip &Vertical"), QStringLiteral("flip-v"),
         QKeySequence(), [this] {
             structuralEdit(tr("Flip Vertical"), QStringLiteral("flip-v"),
                            [this] { currentDocument()->flip(false); });
         });
    make(QStringLiteral("rotatecw"), tr("Rotate 90 degrees &Clockwise"),
         QStringLiteral("rotate-cw"), QKeySequence(QStringLiteral("Ctrl+H")), [this] {
             structuralEdit(tr("Rotate 90 Clockwise"),
                            QStringLiteral("rotate-cw"),
                            [this] { currentDocument()->rotate90(1); });
         });
    make(QStringLiteral("rotateccw"), tr("Rotate 90 degrees Counter-Clock&wise"),
         QStringLiteral("rotate-ccw"), QKeySequence(QStringLiteral("Ctrl+G")), [this] {
             structuralEdit(tr("Rotate 90 Counter-Clockwise"),
                            QStringLiteral("rotate-ccw"),
                            [this] { currentDocument()->rotate90(3); });
         });
    make(QStringLiteral("rotate180"), tr("Rotate &180 degrees"), QString(),
         QKeySequence(), [this] {
             structuralEdit(tr("Rotate 180"), QStringLiteral("rotate-cw"),
                            [this] { currentDocument()->rotate90(2); });
         });
    make(QStringLiteral("flatten"), tr("&Flatten"), QStringLiteral("flatten"),
         QKeySequence(QStringLiteral("Ctrl+Shift+F")), [this] { flattenImage(); });

    // ---------------------------------------------------------------- layers
    make(QStringLiteral("addlayer"), tr("&Add New Layer"),
         QStringLiteral("layer-add"), QKeySequence(QStringLiteral("Ctrl+Shift+N")),
         [this] { addLayer(); });
    make(QStringLiteral("addgroup"), tr("Add &Group"),
         QStringLiteral("layer"), QKeySequence(QStringLiteral("Ctrl+Alt+G")), [this] {
             if (Document* doc = currentDocument()) {
                 structuralEdit(tr("Add Group"), QStringLiteral("layer"),
                                [doc] { doc->addGroup(); });
             }
         });
    make(QStringLiteral("deletelayer"), tr("&Delete Layer"),
         QStringLiteral("layer-delete"), QKeySequence(QStringLiteral("Ctrl+Shift+Delete")),
         [this] { deleteLayer(); });
    make(QStringLiteral("duplicatelayer"), tr("D&uplicate Layer"),
         QStringLiteral("layer-duplicate"), QKeySequence(QStringLiteral("Ctrl+Shift+D")),
         [this] { duplicateLayer(); });
    make(QStringLiteral("mergedown"), tr("&Merge Layer Down"),
         QStringLiteral("layer-merge"), QKeySequence(QStringLiteral("Ctrl+M")),
         [this] { mergeLayerDown(); });
    make(QStringLiteral("importlayer"), tr("&Import from File..."),
         QStringLiteral("open"), QKeySequence(), [this] { importLayerFromFile(); });
    make(QStringLiteral("layerfliph"), tr("Flip Layer &Horizontal"),
         QStringLiteral("flip-h"), QKeySequence(), [this] {
             if (CanvasView* view = currentView()) {
                 Layer* layer = view->document()->activeLayer();
                 EditScope scope(*view->document(), *view->history(), layer,
                                 tr("Flip Layer Horizontal"),
                                 QStringLiteral("flip-h"));
                 layer->image = ops::flip(layer->image, true);
                 scope.commit();
                 view->refresh();
                 updateEverything();
             }
         });
    make(QStringLiteral("layerflipv"), tr("Flip Layer &Vertical"),
         QStringLiteral("flip-v"), QKeySequence(), [this] {
             if (CanvasView* view = currentView()) {
                 Layer* layer = view->document()->activeLayer();
                 EditScope scope(*view->document(), *view->history(), layer,
                                 tr("Flip Layer Vertical"),
                                 QStringLiteral("flip-v"));
                 layer->image = ops::flip(layer->image, false);
                 scope.commit();
                 view->refresh();
                 updateEverything();
             }
         });
    make(QStringLiteral("rotatezoom"), tr("&Rotate / Zoom Layer"),
         QStringLiteral("rotate-cw"), QKeySequence(QStringLiteral("Ctrl+Shift+Z")),
         [this] { chooseTool(QStringLiteral("transform")); });
    // Layer masks. Adding, inverting and applying all change pixels, so they go
    // through the document snapshot; which of the two thumbnails painting lands
    // in is interface state, like the active layer, and is not recorded.
    make(QStringLiteral("addmask"), tr("Add Layer &Mask"),
         QStringLiteral("layer-props"), QKeySequence(), [this] {
             Document* doc = currentDocument();
             Layer* layer = doc ? doc->activeNode() : nullptr;
             if (!layer || layer->hasMask())
                 return;
             const int uid = layer->uid();
             structuralEdit(tr("Add Layer Mask"),
                            QStringLiteral("layer-props"), [doc, uid] {
                                if (Layer* target = doc->layerByUid(uid)) {
                                    doc->addMask(target);
                                    target->setMaskSelected(true);
                                }
                            });
         });
    make(QStringLiteral("editmask"), tr("&Paint in Mask"),
         QStringLiteral("brush"), QKeySequence(), [this] {
             Document* doc = currentDocument();
             Layer* layer = doc ? doc->activeNode() : nullptr;
             if (!layer || !layer->hasMask())
                 return;
             layer->setMaskSelected(!layer->maskSelected());
             refreshPanels();
             updateEverything();
         });
    make(QStringLiteral("togglemask"), tr("&Enable Layer Mask"),
         QStringLiteral("visible"), QKeySequence(), [this] {
             Document* doc = currentDocument();
             Layer* layer = doc ? doc->activeNode() : nullptr;
             if (!layer || !layer->hasMask())
                 return;
             const int uid = layer->uid();
             const bool next = !layer->maskEnabled();
             structuralEdit(next ? tr("Enable Layer Mask")
                                 : tr("Disable Layer Mask"),
                            QStringLiteral("visible"), [doc, uid, next] {
                                if (Layer* target = doc->layerByUid(uid)) {
                                    target->setMaskEnabled(next);
                                    doc->invalidate();
                                }
                            });
         });
    make(QStringLiteral("invertmask"), tr("&Invert Layer Mask"),
         QStringLiteral("invert"), QKeySequence(), [this] {
             Document* doc = currentDocument();
             Layer* layer = doc ? doc->activeNode() : nullptr;
             if (!layer || !layer->hasMask())
                 return;
             const int uid = layer->uid();
             structuralEdit(tr("Invert Layer Mask"),
                            QStringLiteral("invert"), [doc, uid] {
                                if (Layer* target = doc->layerByUid(uid)) {
                                    target->invertMask();
                                    doc->invalidate();
                                }
                            });
         });
    make(QStringLiteral("applymask"), tr("App&ly Layer Mask"),
         QStringLiteral("flatten"), QKeySequence(), [this] {
             Document* doc = currentDocument();
             Layer* layer = doc ? doc->activeNode() : nullptr;
             if (!layer || !layer->hasMask())
                 return;
             const int uid = layer->uid();
             structuralEdit(tr("Apply Layer Mask"),
                            QStringLiteral("flatten"), [doc, uid] {
                                if (Layer* target = doc->layerByUid(uid))
                                    doc->applyMask(target);
                            });
         });
    make(QStringLiteral("deletemask"), tr("Delete Layer Mas&k"),
         QStringLiteral("layer-delete"), QKeySequence(), [this] {
             Document* doc = currentDocument();
             Layer* layer = doc ? doc->activeNode() : nullptr;
             if (!layer || !layer->hasMask())
                 return;
             const int uid = layer->uid();
             structuralEdit(tr("Delete Layer Mask"),
                            QStringLiteral("layer-delete"), [doc, uid] {
                                if (Layer* target = doc->layerByUid(uid))
                                    doc->removeMask(target);
                            });
         });
    make(QStringLiteral("layerup"), tr("Move Layer &Up"), QStringLiteral("up"),
         QKeySequence(), [this] { moveLayer(1); });
    make(QStringLiteral("layerdown"), tr("Move Layer &Down"),
         QStringLiteral("down"), QKeySequence(), [this] { moveLayer(-1); });
    make(QStringLiteral("togglevisible"), tr("Toggle Layer &Visibility"),
         QStringLiteral("visible"), QKeySequence(QStringLiteral("Ctrl+,")), [this] {
             Document* doc = currentDocument();
             if (Layer* layer = doc ? doc->activeNode() : nullptr)
                 setLayerVisible(doc->activeIndex(), !layer->visible);
         });
    make(QStringLiteral("gotop"), tr("Go to &Top Layer"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Alt+PgUp")),
         [this] { goToLayer(currentDocument() ? currentDocument()->layerCount() - 1 : 0); });
    make(QStringLiteral("goabove"), tr("Go to Layer &Above"), QString(),
         QKeySequence(QStringLiteral("Alt+PgUp")), [this] {
             if (Document* doc = currentDocument())
                 goToLayer(doc->activeIndex() + 1);
         });
    make(QStringLiteral("gobelow"), tr("Go to Layer &Below"), QString(),
         QKeySequence(QStringLiteral("Alt+PgDown")), [this] {
             if (Document* doc = currentDocument())
                 goToLayer(doc->activeIndex() - 1);
         });
    make(QStringLiteral("gobottom"), tr("Go to &Bottom Layer"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Alt+PgDown")), [this] { goToLayer(0); });
    make(QStringLiteral("layerprops"), tr("Layer &Properties..."),
         QStringLiteral("layer-props"), QKeySequence(QStringLiteral("F4")),
         [this] { showLayerProperties(); });
    make(QStringLiteral("editadjustment"), tr("Edit Ad&justment..."),
         QStringLiteral("adjust"), QKeySequence(), [this] { editAdjustmentLayer(); });

    // ---------------------------------------------------------------- window
    make(QStringLiteral("nextimage"), tr("&Next Image"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Tab")), [this] {
             if (stack_->count() > 1)
                 setCurrentView((stack_->currentIndex() + 1) % stack_->count());
         });
    make(QStringLiteral("previmage"), tr("&Previous Image"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), [this] {
             if (stack_->count() > 1)
                 setCurrentView((stack_->currentIndex() + stack_->count() - 1)
                                % stack_->count());
         });

    // ---------------------------------------------------------------- help
    make(QStringLiteral("shortcuts"), tr("&Keyboard Shortcuts"),
         QStringLiteral("settings"), QKeySequence(QStringLiteral("F1")),
         [this] { showShortcutsDialog(this, collectShortcuts()); });
    make(QStringLiteral("about"), tr("&About Tessel"), QStringLiteral("about"),
         QKeySequence(), [this] { showAboutDialog(this); });
}

// ----------------------------------------------------------------- menus

void MainWindow::createMenus()
{
    auto add = [this](QMenu* menu, const QString& key) {
        if (QAction* action = actions_.value(key))
            menu->addAction(action);
    };

    QMenu* file = menuBar()->addMenu(tr("&File"));
    add(file, QStringLiteral("new"));
    add(file, QStringLiteral("open"));
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    file->addSeparator();
    add(file, QStringLiteral("save"));
    add(file, QStringLiteral("saveas"));
    add(file, QStringLiteral("saveall"));
    file->addSeparator();
    add(file, QStringLiteral("print"));
    file->addSeparator();
    add(file, QStringLiteral("close"));
    add(file, QStringLiteral("quit"));

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    add(edit, QStringLiteral("undo"));
    add(edit, QStringLiteral("redo"));
    edit->addSeparator();
    add(edit, QStringLiteral("cut"));
    add(edit, QStringLiteral("copy"));
    add(edit, QStringLiteral("copymerged"));
    add(edit, QStringLiteral("paste"));
    add(edit, QStringLiteral("pastelayer"));
    add(edit, QStringLiteral("pasteimage"));
    edit->addSeparator();
    add(edit, QStringLiteral("selectall"));
    add(edit, QStringLiteral("deselect"));
    add(edit, QStringLiteral("invertselection"));
    add(edit, QStringLiteral("copyselection"));
    add(edit, QStringLiteral("pasteselection"));
    QMenu* selectionMenu = edit->addMenu(tr("Se&lection"));
    add(selectionMenu, QStringLiteral("feather"));
    selectionMenu->addSeparator();
    add(selectionMenu, QStringLiteral("grow"));
    add(selectionMenu, QStringLiteral("shrink"));
    add(selectionMenu, QStringLiteral("border"));
    selectionMenu->addSeparator();
    add(selectionMenu, QStringLiteral("freetransform"));
    edit->addSeparator();
    add(edit, QStringLiteral("erase"));
    add(edit, QStringLiteral("fill"));

    QMenu* view = menuBar()->addMenu(tr("&View"));
    add(view, QStringLiteral("zoomin"));
    add(view, QStringLiteral("zoomout"));
    add(view, QStringLiteral("zoomfit"));
    add(view, QStringLiteral("zoomactual"));
    add(view, QStringLiteral("zoomselection"));
    view->addSeparator();
    add(view, QStringLiteral("grid"));
    add(view, QStringLiteral("rulers"));
    add(view, QStringLiteral("selectionoutline"));
    QMenu* unitsMenu = view->addMenu(tr("&Units"));
    unitsMenu->setObjectName(QStringLiteral("unitsmenu"));
    for (Unit candidate : {Unit::Pixels, Unit::Inches, Unit::Centimetres})
        add(unitsMenu, QStringLiteral("unit-") + units::unitId(candidate));
    view->addSeparator();
    add(view, QStringLiteral("fullscreen"));

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    add(image, QStringLiteral("crop"));
    add(image, QStringLiteral("resize"));
    add(image, QStringLiteral("canvassize"));
    image->addSeparator();
    add(image, QStringLiteral("fliph"));
    add(image, QStringLiteral("flipv"));
    add(image, QStringLiteral("rotatecw"));
    add(image, QStringLiteral("rotateccw"));
    add(image, QStringLiteral("rotate180"));
    image->addSeparator();
    add(image, QStringLiteral("flatten"));

    QMenu* layersMenu = menuBar()->addMenu(tr("&Layers"));
    add(layersMenu, QStringLiteral("addlayer"));
    add(layersMenu, QStringLiteral("addgroup"));
    // Every adjustment and effect can be had as a layer of its own, listed the
    // way the Adjustments and Effects menus list them.
    QMenu* newAdjustment = layersMenu->addMenu(tr("New Ad&justment Layer"));
    newAdjustment->setObjectName(QStringLiteral("newadjustmentmenu"));
    newAdjustment->setIcon(icons::get(QStringLiteral("adjust"), 20));
    fillEffectMenu(newAdjustment, [this](Effect* effect) { addAdjustmentLayer(effect); });
    add(layersMenu, QStringLiteral("deletelayer"));
    add(layersMenu, QStringLiteral("duplicatelayer"));
    add(layersMenu, QStringLiteral("mergedown"));
    add(layersMenu, QStringLiteral("importlayer"));
    layersMenu->addSeparator();
    QMenu* maskMenu = layersMenu->addMenu(tr("Layer &Mask"));
    add(maskMenu, QStringLiteral("addmask"));
    add(maskMenu, QStringLiteral("editmask"));
    maskMenu->addSeparator();
    add(maskMenu, QStringLiteral("togglemask"));
    add(maskMenu, QStringLiteral("invertmask"));
    add(maskMenu, QStringLiteral("applymask"));
    add(maskMenu, QStringLiteral("deletemask"));
    layersMenu->addSeparator();
    add(layersMenu, QStringLiteral("layerfliph"));
    add(layersMenu, QStringLiteral("layerflipv"));
    add(layersMenu, QStringLiteral("rotatezoom"));
    layersMenu->addSeparator();
    add(layersMenu, QStringLiteral("layerup"));
    add(layersMenu, QStringLiteral("layerdown"));
    add(layersMenu, QStringLiteral("togglevisible"));
    layersMenu->addSeparator();
    add(layersMenu, QStringLiteral("gotop"));
    add(layersMenu, QStringLiteral("goabove"));
    add(layersMenu, QStringLiteral("gobelow"));
    add(layersMenu, QStringLiteral("gobottom"));
    layersMenu->addSeparator();
    add(layersMenu, QStringLiteral("layerprops"));
    add(layersMenu, QStringLiteral("editadjustment"));

    // Adjustments and effects come straight from the registry.
    QMenu* adjustments = menuBar()->addMenu(tr("&Adjustments"));
    for (Effect* effect : EffectRegistry::instance().inCategory(
             QStringLiteral("Adjustments"))) {
        auto* action = new QAction(effect->params().empty()
                                       ? effect->name()
                                       : effect->name() + QStringLiteral("..."),
                                   this);
        action->setIcon(icons::get(QStringLiteral("adjust"), 20));
        if (!effect->shortcut().isEmpty())
            action->setShortcut(QKeySequence(effect->shortcut()));
        connect(action, &QAction::triggered, this, [this, effect] { applyEffect(effect); });
        adjustments->addAction(action);
        addAction(action);
    }

    QMenu* effects = menuBar()->addMenu(tr("Effec&ts"));
    repeatAction_ = new QAction(tr("Repeat Last Effect"), this);
    repeatAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
    repeatAction_->setEnabled(false);
    connect(repeatAction_, &QAction::triggered, this, [this] { repeatLastEffect(); });
    effects->addAction(repeatAction_);
    addAction(repeatAction_);
    effects->addSeparator();
    for (const QString& category : effectMenuCategories()) {
        const std::vector<Effect*> list = EffectRegistry::instance().inCategory(category);
        if (list.empty())
            continue;
        QMenu* submenu = effects->addMenu(effectCategoryLabel(category));
        submenu->setIcon(icons::get(QStringLiteral("effect"), 20));
        for (Effect* effect : list) {
            auto* action = new QAction(effect->params().empty()
                                           ? effect->name()
                                           : effect->name() + QStringLiteral("..."),
                                       this);
            if (!effect->shortcut().isEmpty())
                action->setShortcut(QKeySequence(effect->shortcut()));
            connect(action, &QAction::triggered, this,
                    [this, effect] { applyEffect(effect); });
            submenu->addAction(action);
            addAction(action);
        }
    }
    effects->addSeparator();
    auto* pluginsAction = new QAction(tr("About the Plugins..."), this);
    pluginsAction->setObjectName(QStringLiteral("aboutplugins"));
    connect(pluginsAction, &QAction::triggered, this,
            [this] { showPluginsDialog(this); });
    effects->addAction(pluginsAction);
    addAction(pluginsAction);

    // A plugin that did not load has to be noticeable without opening that
    // dialog first. The status bar is built after the menus, so the message
    // waits for the event loop rather than creating it early here.
    const QString pluginSummary = plugins::report().summary();
    if (!pluginSummary.isEmpty()) {
        QTimer::singleShot(0, this, [this, pluginSummary] {
            statusBar()->showMessage(pluginSummary, 15000);
        });
    }

    // The four panel toggles, settings and help sit as icons at the right hand
    // end of the menu bar rather than in menus of their own.
    createMenuBarIcons();
}

void MainWindow::createMenuBarIcons()
{
    auto* corner = new QWidget(menuBar());
    auto* layout = new QHBoxLayout(corner);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(2);

    auto addButton = [&](const QString& icon, const QString& tip, bool checkable) {
        auto* button = new QToolButton(corner);
        button->setIcon(icons::get(icon, 18));
        button->setProperty("iconName", icon);
        button->setIconSize(QSize(18, 18));
        button->setToolTip(tip);
        button->setCheckable(checkable);
        button->setAutoRaise(true);
        layout->addWidget(button);
        return button;
    };

    toolsButton_ = addButton(QStringLiteral("brush"), tr("Tools Window"), true);
    historyButton_ = addButton(QStringLiteral("undo"), tr("History Window"), true);
    layersButton_ = addButton(QStringLiteral("layer-duplicate"),
                              tr("Layers Window"), true);
    colorsButton_ = addButton(QStringLiteral("palette"), tr("Colors Window"),
                              true);

    auto* settingsButton = addButton(QStringLiteral("settings"),
                                     tr("Settings"), false);
    settingsButton->setObjectName(QStringLiteral("settingsButton"));
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::showSettings);

    auto* helpButton = addButton(QStringLiteral("about"), tr("Help"), false);
    auto* helpMenu = new QMenu(helpButton);
    if (QAction* shortcuts = actions_.value(QStringLiteral("shortcuts")))
        helpMenu->addAction(shortcuts);
    helpMenu->addSeparator();
    if (QAction* about = actions_.value(QStringLiteral("about")))
        helpMenu->addAction(about);
    helpButton->setMenu(helpMenu);
    helpButton->setPopupMode(QToolButton::InstantPopup);

    menuBar()->setCornerWidget(corner, Qt::TopRightCorner);
}

// ----------------------------------------------------------------- toolbars

void MainWindow::createToolBars()
{
    mainToolBar_ = addToolBar(tr("Main"));
    mainToolBar_->setObjectName(QStringLiteral("mainToolBar"));
    mainToolBar_->setMovable(false);
    mainToolBar_->setIconSize(QSize(20, 20));
    for (const QString& key : {QStringLiteral("new"), QStringLiteral("open"),
                               QStringLiteral("save")}) {
        mainToolBar_->addAction(actions_.value(key));
    }
    mainToolBar_->addSeparator();
    for (const QString& key : {QStringLiteral("undo"), QStringLiteral("redo")})
        mainToolBar_->addAction(actions_.value(key));
    mainToolBar_->addSeparator();
    for (const QString& key : {QStringLiteral("cut"), QStringLiteral("copy"),
                               QStringLiteral("paste")}) {
        mainToolBar_->addAction(actions_.value(key));
    }
    mainToolBar_->addSeparator();
    for (const QString& key : {QStringLiteral("crop"), QStringLiteral("deselect")})
        mainToolBar_->addAction(actions_.value(key));
    mainToolBar_->addSeparator();
    for (const QString& key : {QStringLiteral("resize"), QStringLiteral("canvassize")})
        mainToolBar_->addAction(actions_.value(key));

    // The open image thumbnails live at the right hand end of the toolbar.
    auto* spacer = new QWidget(mainToolBar_);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mainToolBar_->addWidget(spacer);
    strip_ = new ImageStrip(mainToolBar_);
    mainToolBar_->addWidget(strip_);
    connect(strip_, &ImageStrip::imageActivated, this, &MainWindow::setCurrentView);
    connect(strip_, &ImageStrip::closeRequested, this,
            [this](int index) { closeView(index); });

    addToolBarBreak();
    optionsToolBar_ = addToolBar(tr("Tool Options"));
    optionsToolBar_->setObjectName(QStringLiteral("toolOptionsToolBar"));
    optionsToolBar_->setMovable(false);
    options_ = new ToolOptionsBar(optionsToolBar_);
    optionsToolBar_->addWidget(options_);
    connect(options_, &ToolOptionsBar::toolPicked, this,
            [this](const QString& id) { chooseTool(id); });
    auto* toolListShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+T")), this);
    connect(toolListShortcut, &QShortcut::activated, this,
            [this] { options_->openToolList(); });
    connect(options_, &ToolOptionsBar::valueChanged, this,
            [this](const QString& key, const QVariant& value) {
                if (CanvasView* view = currentView()) {
                    if (Tool* tool = view->tool()) {
                        tool->optionChanged(key, value);
                        view->refresh();
                    }
                }
            });
}

// ----------------------------------------------------------------- panels

void MainWindow::createPanels()
{
    auto makePanel = [this](const QString& name, const QString& title, QWidget* content,
                            Corner corner) {
        auto* panel = new Panel(name, title, content, this);
        panel->setCorner(corner);
        connect(panel, &Panel::closeRequested, panel, &QWidget::hide);
        // Queued, because the panel says this from inside a layout pass and
        // moving it there would start another one.
        connect(panel, &Panel::sizeChanged, this, [this] { layOutPanels(); },
                Qt::QueuedConnection);
        connect(panel, &Panel::dragging, this, [this, panel](const QPoint& pointer) {
            showDropHint(panel, pointer);
        });
        connect(panel, &Panel::dropped, this, [this, panel](const QPoint& pointer) {
            dropPanel(panel, pointer);
        });
        panels_.append(panel);
        return panel;
    };

    // Paint.NET keeps these four in the corners of the image window, and the
    // corners are where they belong: a panel that takes only the room it
    // needs has no business owning a column down the side of the window.
    tools_ = new ToolsPalette(this);
    toolsPanel_ = makePanel(QStringLiteral("toolsPanel"), tr("Tools"), tools_,
                            Corner::TopLeft);

    colors_ = new ColorPanel(this);
    // The Colors panel is the tall one: wheel, value and alpha, the recent
    // row and the palette. It keeps its own height and scrolls when the
    // window is too short for it, rather than squeezing the rows of swatches
    // down to nothing.
    auto* colorsScroll = new QScrollArea(this);
    colorsScroll->setWidget(colors_);
    colorsScroll->setWidgetResizable(true);
    // Without this a scroll area asks for the same default size whatever is
    // in it, and the panel would be sized to that instead of to the contents.
    colorsScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    colorsScroll->setFrameShape(QFrame::NoFrame);
    colorsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    colorsPanel_ = makePanel(QStringLiteral("colorsPanel"), tr("Colors"), colorsScroll,
                             Corner::BottomLeft);

    history_ = new HistoryPanel(this);
    historyPanel_ = makePanel(QStringLiteral("historyPanel"), tr("History"), history_,
                              Corner::TopRight);

    layers_ = new LayersPanel(this);
    layersPanel_ = makePanel(QStringLiteral("layersPanel"), tr("Layers"), layers_,
                             Corner::BottomRight);

    dropHint_ = new PanelDropHint(this);

    // Wayland refuses the mouse grabs Qt asks for, and a drag that ends
    // without Qt hearing of it leaves the grab behind: every later click goes
    // to whatever grabbed rather than to the button under the pointer, which
    // is why a panel's buttons would look dead until something else cleared
    // it. Nothing holds the mouse with no button down except a popup, so this
    // hands it back as soon as the pointer moves.
    qApp->installEventFilter(this);

    // The menu bar icons mirror whether each panel is showing.
    const std::pair<QToolButton*, Panel*> buttons[] = {
        {toolsButton_, toolsPanel_}, {historyButton_, historyPanel_},
        {layersButton_, layersPanel_}, {colorsButton_, colorsPanel_}};
    for (const auto& [button, panel] : buttons) {
        if (!button || !panel)
            continue;
        button->setChecked(panel->isVisible());
        connect(button, &QToolButton::toggled, panel, &QWidget::setVisible);
        connect(panel, &Panel::sizeChanged, button, [button, panel] {
            QSignalBlocker blocker(button);
            button->setChecked(panel->isVisible());
        });
    }

    connect(tools_, &ToolsPalette::toolSelected, this,
            [this](const QString& id) { chooseTool(id); });
    connect(colors_, &ColorPanel::primaryChanged, this, [this](Rgba color) {
        for (int i = 0; i < stack_->count(); ++i)
            viewAt(i)->setPrimaryColor(color);
    });
    connect(colors_, &ColorPanel::secondaryChanged, this, [this](Rgba color) {
        for (int i = 0; i < stack_->count(); ++i)
            viewAt(i)->setSecondaryColor(color);
    });

    connect(layers_, &LayersPanel::addLayerRequested, this, &MainWindow::addLayer);
    connect(layers_, &LayersPanel::addGroupRequested, this, [this] {
        if (QAction* action = actions_.value(QStringLiteral("addgroup")))
            action->trigger();
    });
    connect(layers_, &LayersPanel::layerReparented, this,
            [this](int uid, int parentUid, int childIndex) {
                Document* doc = currentDocument();
                if (!doc)
                    return;
                Layer* layer = doc->layerByUid(uid);
                Layer* parent = parentUid == 0 ? doc->rootGroup()
                                               : doc->layerByUid(parentUid);
                if (!layer || !parent || !doc->canReparent(layer, parent))
                    return;
                structuralEdit(tr("Move Layer"), QStringLiteral("up"),
                               [doc, layer, parent, childIndex] {
                                   doc->reparentLayer(layer, parent, childIndex);
                               });
            });
    connect(layers_, &LayersPanel::deleteLayerRequested, this, &MainWindow::deleteLayer);
    connect(layers_, &LayersPanel::duplicateLayerRequested, this,
            &MainWindow::duplicateLayer);
    connect(layers_, &LayersPanel::mergeDownRequested, this, &MainWindow::mergeLayerDown);
    connect(layers_, &LayersPanel::propertiesRequested, this,
            &MainWindow::showLayerProperties);
    connect(layers_, &LayersPanel::editAdjustmentRequested, this,
            &MainWindow::editAdjustmentLayer);
    connect(layers_, &LayersPanel::moveUpRequested, this, [this] { moveLayer(1); });
    connect(layers_, &LayersPanel::moveDownRequested, this, [this] { moveLayer(-1); });
    connect(layers_, &LayersPanel::activeLayerChanged, this, [this](int index) {
        if (Document* doc = currentDocument()) {
            doc->setActiveIndex(index);
            updateStatusBar();
        }
    });
    connect(layers_, &LayersPanel::layerVisibilityToggled, this,
            [this](int index, bool visible) { setLayerVisible(index, visible); });

    connect(history_, &HistoryPanel::undoRequested, this,
            [this] { actions_.value(QStringLiteral("undo"))->trigger(); });
    connect(history_, &HistoryPanel::redoRequested, this,
            [this] { actions_.value(QStringLiteral("redo"))->trigger(); });
    connect(history_, &HistoryPanel::jumpRequested, this, [this](int index) {
        if (CanvasView* view = currentView()) {
            view->cancelPendingTool();
            view->history()->goTo(index);
            view->refresh();
            updateEverything();
        }
    });
}

// ----------------------------------------------------------------- status bar

void MainWindow::createStatusBar()
{
    hintLabel_ = new QLabel(this);
    positionLabel_ = new QLabel(this);
    selectionLabel_ = new QLabel(this);
    sizeLabel_ = new QLabel(this);
    sizeLabel_->setObjectName(QStringLiteral("imagesize"));
    hintLabel_->setMinimumWidth(260);
    positionLabel_->setMinimumWidth(104);
    selectionLabel_->setMinimumWidth(130);
    sizeLabel_->setMinimumWidth(104);

    zoomCombo_ = new QComboBox(this);
    zoomCombo_->setEditable(true);
    zoomCombo_->setFixedWidth(92);
    zoomCombo_->addItem(tr("Window"));
    for (const char* label : {"1%", "5%", "12%", "25%", "50%", "66%", "100%", "200%",
                              "400%", "800%", "1600%", "3200%"}) {
        zoomCombo_->addItem(QString::fromLatin1(label));
    }
    zoomCombo_->setCurrentText(QStringLiteral("100%"));
    connect(zoomCombo_, &QComboBox::activated, this, [this](int index) {
        CanvasView* view = currentView();
        if (!view)
            return;
        if (index == 0) {
            view->zoomToFit();
            return;
        }
        QString text = zoomCombo_->itemText(index);
        text.remove(QLatin1Char('%'));
        view->setZoom(text.toDouble() / 100.0);
    });
    connect(zoomCombo_->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        if (updatingZoom_)
            return;
        CanvasView* view = currentView();
        if (!view)
            return;
        QString text = zoomCombo_->currentText().trimmed();
        text.remove(QLatin1Char('%'));
        bool ok = false;
        const double percent = text.toDouble(&ok);
        if (ok && percent > 0)
            view->setZoom(percent / 100.0);
        updateZoomControls();
    });

    zoomSlider_ = new QSlider(Qt::Horizontal, this);
    zoomSlider_->setFixedWidth(120);
    zoomSlider_->setRange(-200, 200); // logarithmic: 0 is 100%
    connect(zoomSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (updatingZoom_)
            return;
        if (CanvasView* view = currentView())
            view->setZoom(std::pow(10.0, value / 100.0));
    });

    statusBar()->addWidget(hintLabel_, 1);
    statusBar()->addPermanentWidget(positionLabel_);
    statusBar()->addPermanentWidget(selectionLabel_);
    statusBar()->addPermanentWidget(sizeLabel_);
    statusBar()->addPermanentWidget(zoomCombo_);
    statusBar()->addPermanentWidget(zoomSlider_);
}

void MainWindow::installToolShortcuts()
{
    // Single letter keys cycle through the tools that share them.
    QStringList keys;
    for (Tool* tool : ToolRegistry::instance().all()) {
        if (!tool->shortcut().isEmpty() && !keys.contains(tool->shortcut()))
            keys.append(tool->shortcut());
    }
    for (const QString& key : keys) {
        auto* forward = new QShortcut(QKeySequence(key), this);
        forward->setContext(Qt::WindowShortcut);
        connect(forward, &QShortcut::activated, this, [this, key] {
            cycleToolShortcut(key, 1);
        });
        auto* backward = new QShortcut(QKeySequence(QStringLiteral("Shift+%1").arg(key)),
                                       this);
        backward->setContext(Qt::WindowShortcut);
        connect(backward, &QShortcut::activated, this, [this, key] {
            cycleToolShortcut(key, -1);
        });
    }
    // Swap and reset the palette colours, as the Colors window buttons do.
    auto* swap = new QShortcut(QKeySequence(QStringLiteral("X")), this);
    connect(swap, &QShortcut::activated, this, [this] { colors_->swapColors(); });
    auto* reset = new QShortcut(QKeySequence(QStringLiteral("D")), this);
    connect(reset, &QShortcut::activated, this, [this] { colors_->resetColors(); });
}

void MainWindow::layOutCanvas()
{
    if (!stack_ || !canvasHost_)
        return;
    // The canvas takes the whole width between the tool bars and the status
    // bar. The panels stand on top of it in the corners, so the workspace
    // carries on under them and beyond whatever height they end at.
    QRect band = canvasHost_->geometry();
    band.setLeft(0);
    band.setRight(width() - 1);
    if (stack_->geometry() != band)
        stack_->setGeometry(band);
    if (dropHint_ && dropHint_->geometry() != band)
        dropHint_->setGeometry(band);
    layOutPanels();
}

QRect MainWindow::panelArea() const
{
    if (stack_)
        return stack_->geometry();
    if (canvasHost_)
        return canvasHost_->geometry();
    return rect();
}

void MainWindow::layOutPanels()
{
    if (panels_.isEmpty() || layingOutPanels_)
        return;
    // Placing a panel changes its shape, which is what asked for this in the
    // first place: without the latch the two call each other for ever.
    layingOutPanels_ = true;
    const QScopeGuard done([this] { layingOutPanels_ = false; });
    const QRect area = panelArea();
    if (area.width() < 80 || area.height() < 80)
        return;
    constexpr int kMargin = 8;
    constexpr int kGap = 6;

    for (const Corner corner : {Corner::TopLeft, Corner::TopRight, Corner::BottomLeft,
                                Corner::BottomRight}) {
        QList<Panel*> here;
        for (Panel* panel : panels_) {
            if (panel->isVisible() && !panel->isBeingDragged() && panel->corner() == corner)
                here.append(panel);
        }
        if (here.isEmpty())
            continue;
        const bool fromTop = corner == Corner::TopLeft || corner == Corner::TopRight;
        const bool fromLeft = corner == Corner::TopLeft || corner == Corner::BottomLeft;
        // Panels in one corner are stacked away from it, and share the height
        // when they ask for more of it than there is.
        int free = area.height() - 2 * kMargin - kGap * (here.size() - 1);
        int edge = fromTop ? area.top() + kMargin : area.bottom() - kMargin;
        for (int i = 0; i < here.size(); ++i) {
            Panel* panel = here[i];
            const QSize want = panel->wantedSize();
            const int left = here.size() - i - 1; // panels still to place
            const int width = std::clamp(want.width(), 120,
                                         std::max(120, area.width() / 2 - kMargin));
            const int height = std::clamp(want.height(), 48,
                                          std::max(48, free - left * 48));
            const int x = fromLeft ? area.left() + kMargin
                                   : area.right() - kMargin - width + 1;
            const int y = fromTop ? edge : edge - height + 1;
            panel->setGeometry(x, y, width, height);
            panel->raise();
            free -= height;
            edge += fromTop ? height + kGap : -(height + kGap);
        }
    }
}

void MainWindow::showDropHint(Panel* panel, const QPoint& pointer)
{
    if (!dropHint_ || !panel)
        return;
    dropHint_->setGeometry(panelArea());
    dropHint_->setActive(PanelDropHint::cornerAt(dropHint_->rect(),
                                                 pointer - dropHint_->pos()));
    if (!dropHint_->isVisible()) {
        dropHint_->show();
        dropHint_->raise();
        panel->raise();
    }
}

void MainWindow::dropPanel(Panel* panel, const QPoint& pointer)
{
    if (!panel)
        return;
    if (dropHint_) {
        dropHint_->hide();
        panel->setCorner(PanelDropHint::cornerAt(dropHint_->rect(),
                                                 pointer - dropHint_->pos()));
    }
    // Nearest the corner it was let go in: the panels already there move
    // aside rather than the new one landing behind them.
    panels_.removeOne(panel);
    panels_.prepend(panel);
    layOutPanels();
}

// The panels are ours rather than Qt's, so where they sit is written down
// here: the corner each one is in, the order they are stacked in it, and
// whether it is showing at all.
void MainWindow::savePanels(QSettings& settings) const
{
    QStringList entries;
    for (Panel* panel : panels_) {
        entries.append(QStringLiteral("%1:%2:%3")
                           .arg(panel->objectName())
                           .arg(int(panel->corner()))
                           .arg(panel->isVisible() ? 1 : 0));
    }
    settings.setValue(QStringLiteral("panels"), entries.join(QLatin1Char(';')));
}

void MainWindow::restorePanels(const QSettings& settings)
{
    const QString stored = settings.value(QStringLiteral("panels")).toString();
    if (stored.isEmpty())
        return;
    QList<Panel*> order;
    for (const QString& entry : stored.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList parts = entry.split(QLatin1Char(':'));
        if (parts.size() != 3)
            continue;
        Panel* panel = nullptr;
        for (Panel* candidate : panels_) {
            if (candidate->objectName() == parts[0])
                panel = candidate;
        }
        if (!panel || order.contains(panel))
            continue;
        const int corner = std::clamp(parts[1].toInt(), 0, 3);
        panel->setCorner(Corner(corner));
        panel->setVisible(parts[2].toInt() != 0);
        order.append(panel);
    }
    // Anything the settings did not mention keeps the place it was given.
    for (Panel* panel : panels_) {
        if (!order.contains(panel))
            order.append(panel);
    }
    panels_ = order;
    layOutPanels();
}

void MainWindow::resetPanelLayout()
{
    const std::pair<Panel*, Corner> home[] = {{toolsPanel_, Corner::TopLeft},
                                              {historyPanel_, Corner::TopRight},
                                              {colorsPanel_, Corner::BottomLeft},
                                              {layersPanel_, Corner::BottomRight}};
    panels_.clear();
    for (const auto& [panel, corner] : home) {
        if (!panel)
            continue;
        panel->setCorner(corner);
        panel->show();
        panels_.append(panel);
    }
    layOutPanels();
}

void MainWindow::refreshIcons()
{
    for (Panel* panel : panels_)
        panel->refreshIcons();

    // Icons are drawn in the current foreground colour, so after a theme change
    // every one of them has to be produced again.
    for (QAction* action : findChildren<QAction*>()) {
        const QString name = action->property("iconName").toString();
        if (!name.isEmpty())
            action->setIcon(icons::get(name, 20));
    }
    for (QAbstractButton* button : findChildren<QAbstractButton*>()) {
        const QString name = button->property("iconName").toString();
        if (!name.isEmpty())
            button->setIcon(icons::get(name, button->iconSize().width()));
    }
    setWindowIcon(icons::appIcon());
    if (Tool* tool = currentView() ? currentView()->tool() : nullptr)
        options_->startTool(tool);
    refreshPanels();
    updateImageStrip();
}

// ----------------------------------------------------------------- documents

CanvasView* MainWindow::viewAt(int index) const
{
    return qobject_cast<CanvasView*>(stack_->widget(index));
}

CanvasView* MainWindow::currentView() const
{
    return qobject_cast<CanvasView*>(stack_->currentWidget());
}

Document* MainWindow::currentDocument() const
{
    CanvasView* view = currentView();
    return view ? view->document() : nullptr;
}

History* MainWindow::currentHistory() const
{
    CanvasView* view = currentView();
    return view ? view->history() : nullptr;
}

void MainWindow::addView(CanvasView* view)
{
    view->setPrimaryColor(colors_->primary());
    view->setSecondaryColor(colors_->secondary());
    view->setShowGrid(actions_.value(QStringLiteral("grid"))->isChecked());
    view->setShowRulers(actions_.value(QStringLiteral("rulers"))->isChecked());
    view->setRulerUnit(unit_);
    view->setShowSelection(
        actions_.value(QStringLiteral("selectionoutline"))->isChecked());

    connect(view, &CanvasView::cursorMoved, this, [this](QPoint pixel, bool inside) {
        positionLabel_->setText(inside ? QStringLiteral("%1, %2").arg(pixel.x()).arg(pixel.y())
                                       : QString());
    });
    connect(view, &CanvasView::zoomChanged, this, [this](double) { updateZoomControls(); });
    connect(view, &CanvasView::documentChanged, this, [this] {
        refreshPanels();
        updateActionStates();
        updateImageStrip();
        updateStatusBar();
    });
    connect(view, &CanvasView::modifiedChanged, this, [this] { updateWindowTitle(); });
    connect(view, &CanvasView::statusMessage, this,
            [this](const QString& text) { hintLabel_->setText(text); });
    connect(view, &CanvasView::colorPicked, this, [this](Rgba color, bool primary) {
        if (primary)
            colors_->setPrimary(color);
        else
            colors_->setSecondary(color);
    });
    connect(view, &CanvasView::toolRequested, this, [this](const QString& id) {
        chooseTool(id.isEmpty() ? previousToolId_ : id);
    });

    stack_->addWidget(view);
    setCurrentView(stack_->count() - 1);
    view->zoomToFit();
}

void MainWindow::setCurrentView(int index)
{
    if (index < 0 || index >= stack_->count())
        return;
    if (CanvasView* previous = currentView(); previous && previous != viewAt(index))
        previous->commitPendingTool();
    stack_->setCurrentIndex(index);
    CanvasView* view = viewAt(index);
    if (view) {
        view->setTool(ToolRegistry::instance().byId(currentToolId_));
        view->setFocus();
    }
    updateEverything();
}

void MainWindow::newDocument(int width, int height, Rgba background)
{
    auto doc = std::make_unique<Document>(width, height, background);
    doc->setTitle(tr("Untitled %1").arg(untitledCounter_++));
    doc->setModified(false);
    addView(new CanvasView(std::move(doc), this));
    hintLabel_->setText(tr("Created a new %1 x %2 image")
                            .arg(width).arg(height));
}

bool MainWindow::openPath(const QString& path)
{
    // If the file is already open, just switch to it.
    for (int i = 0; i < stack_->count(); ++i) {
        if (viewAt(i)->document()->filePath() == path) {
            setCurrentView(i);
            return true;
        }
    }
    QString error;
    std::unique_ptr<Document> doc = io::openDocument(path, &error);
    if (!doc) {
        QMessageBox::warning(this, tr("Cannot open image"), error);
        return false;
    }
    lastDirectory_ = QFileInfo(path).absolutePath();
    recentFiles_.removeAll(path);
    recentFiles_.prepend(path);
    while (recentFiles_.size() > kMaxRecentFiles)
        recentFiles_.removeLast();
    addView(new CanvasView(std::move(doc), this));
    hintLabel_->setText(tr("Opened %1").arg(QFileInfo(path).fileName()));
    updateEverything();
    return true;
}

void MainWindow::openPaths(const QStringList& paths)
{
    for (const QString& path : paths)
        openPath(path);
}

bool MainWindow::promptToSave(int index)
{
    CanvasView* view = viewAt(index);
    if (!view || !view->document()->isModified())
        return true;
    const QString name = view->document()->displayName();
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Save changes?"),
        tr("Save changes to %1 before closing it?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;
    return saveView(index, false);
}

bool MainWindow::saveView(int index, bool saveAs)
{
    CanvasView* view = viewAt(index);
    if (!view)
        return false;
    view->commitPendingTool();
    Document* doc = view->document();
    QString path = doc->filePath();
    if (saveAs || path.isEmpty() || !io::canWrite(path)) {
        QString selectedFilter = io::filterForExtension(QStringLiteral("tessel"));
        QString suggestion = path.isEmpty()
            ? QStringLiteral("%1/%2.tessel").arg(lastDirectory_, doc->title())
            : path;
        path = QFileDialog::getSaveFileName(this, tr("Save Image"), suggestion,
                                            io::saveFilter(), &selectedFilter);
        if (path.isEmpty())
            return false;
        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".tessel");
    }
    // Warn once when the chosen format cannot hold everything in the image.
    if (io::flattensLayers(path) && doc->layerCount() > 1) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("Flatten image?"),
            tr("%1 cannot store layers, so the image will be saved "
               "flattened.\n\nThe layers stay intact in this window. "
               "Save as .tessel or .ora to keep them in the file.")
                .arg(QFileInfo(path).fileName()),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel)
            return false;
    }
    int quality = 92;
    if (io::isLossyFormat(path)) {
        SaveOptionsDialog options(doc->composite(), path, quality, this);
        if (options.exec() != QDialog::Accepted)
            return false;
        quality = options.quality();
    }
    QString error;
    if (!io::saveDocument(*doc, path, &error, quality)) {
        QMessageBox::warning(this, tr("Cannot save image"), error);
        return false;
    }
    doc->setFilePath(path);
    doc->setTitle(QFileInfo(path).fileName());
    view->history()->markSaved();
    dropRecoveryFile(view);
    lastDirectory_ = QFileInfo(path).absolutePath();
    recentFiles_.removeAll(path);
    recentFiles_.prepend(path);
    hintLabel_->setText(tr("Saved %1").arg(QFileInfo(path).fileName()));
    updateEverything();
    return true;
}

bool MainWindow::closeView(int index)
{
    if (index < 0 || index >= stack_->count())
        return false;
    if (!promptToSave(index))
        return false;
    CanvasView* view = viewAt(index);
    dropRecoveryFile(view);
    stack_->removeWidget(view);
    view->deleteLater();
    if (stack_->count() == 0) {
        newDocument(800, 600, Rgba(255, 255, 255, 255));
        return true;
    }
    setCurrentView(std::min(index, stack_->count() - 1));
    return true;
}

// ----------------------------------------------------------------- updates

void MainWindow::updateEverything()
{
    refreshPanels();
    updateWindowTitle();
    updateImageStrip();
    updateStatusBar();
    updateActionStates();
    updateZoomControls();

    // Keep the recent files menu in step.
    if (recentMenu_) {
        recentMenu_->clear();
        for (const QString& path : recentFiles_) {
            auto* action = recentMenu_->addAction(QFileInfo(path).fileName());
            action->setToolTip(path);
            connect(action, &QAction::triggered, this, [this, path] { openPath(path); });
        }
        if (recentFiles_.isEmpty()) {
            recentMenu_->addAction(tr("No recent files"))->setEnabled(false);
        } else {
            recentMenu_->addSeparator();
            connect(recentMenu_->addAction(tr("Clear list")),
                    &QAction::triggered, this, [this] {
                        recentFiles_.clear();
                        updateEverything();
                    });
        }
    }
}

void MainWindow::refreshPanels()
{
    CanvasView* view = currentView();
    layers_->setDocument(view ? view->document() : nullptr,
                         view ? view->history() : nullptr);
    history_->setDocument(view ? view->document() : nullptr,
                          view ? view->history() : nullptr);
}

void MainWindow::updateWindowTitle()
{
    Document* doc = currentDocument();
    if (!doc) {
        setWindowTitle(QStringLiteral(TESSEL_APP_NAME));
        return;
    }
    setWindowTitle(tr("%1%2 - %3")
                       .arg(doc->displayName(),
                            doc->isModified() ? QStringLiteral("*") : QString(),
                            QStringLiteral(TESSEL_APP_NAME)));
    updateImageStrip();
}

void MainWindow::updateImageStrip()
{
    QVector<ImageStrip::Entry> entries;
    entries.reserve(stack_->count());
    for (int i = 0; i < stack_->count(); ++i) {
        Document* doc = viewAt(i)->document();
        ImageStrip::Entry entry;
        entry.title = doc->displayName();
        entry.tooltip = tr("%1\n%2 x %3  -  %n layer(s)", "", doc->layerCount())
                            .arg(doc->filePath().isEmpty() ? doc->title()
                                                           : doc->filePath())
                            .arg(doc->width())
                            .arg(doc->height());
        entry.thumbnail = doc->thumbnail(44).toQImage();
        entry.modified = doc->isModified();
        entries.append(entry);
    }
    strip_->setEntries(entries, stack_->currentIndex());
}

void MainWindow::updateStatusBar()
{
    Document* doc = currentDocument();
    if (!doc) {
        selectionLabel_->clear();
        sizeLabel_->clear();
        return;
    }
    const Selection& selection = doc->selection();
    if (selection.isActive()) {
        const Rect bounds = selection.bounds();
        selectionLabel_->setText(tr("Selection %1 x %2 at %3, %4")
                                     .arg(bounds.w).arg(bounds.h)
                                     .arg(bounds.x).arg(bounds.y));
    } else {
        selectionLabel_->setText(tr("No selection"));
    }
    sizeLabel_->setText(
        units::describeSize(doc->width(), doc->height(), doc->resolution(), unit_));
    sizeLabel_->setToolTip(
        units::describePrintSize(doc->width(), doc->height(), doc->resolution(), unit_));
}

void MainWindow::updateActionStates()
{
    Document* doc = currentDocument();
    History* history = currentHistory();
    const bool hasDoc = doc != nullptr;
    const bool hasSelection = hasDoc && doc->selection().isActive();
    const bool manyLayers = hasDoc && doc->layerCount() > 1;

    auto enable = [this](const QString& key, bool on) {
        if (QAction* action = actions_.value(key))
            action->setEnabled(on);
    };

    enable(QStringLiteral("undo"), history && history->canUndo());
    enable(QStringLiteral("redo"), history && history->canRedo());
    if (QAction* undo = actions_.value(QStringLiteral("undo"))) {
        undo->setText(history && history->canUndo()
                          ? tr("&Undo %1").arg(history->undoName())
                          : tr("&Undo"));
    }
    if (QAction* redo = actions_.value(QStringLiteral("redo"))) {
        redo->setText(history && history->canRedo()
                          ? tr("&Redo %1").arg(history->redoName())
                          : tr("&Redo"));
    }
    enable(QStringLiteral("crop"), hasSelection);
    for (const QString& key : {QStringLiteral("feather"), QStringLiteral("grow"),
                               QStringLiteral("shrink"), QStringLiteral("border")}) {
        enable(key, hasSelection);
    }
    enable(QStringLiteral("freetransform"), hasDoc);
    enable(QStringLiteral("zoomselection"), hasSelection);
    const Layer* active = hasDoc ? doc->activeNode() : nullptr;
    const bool hasMask = active && active->hasMask();
    enable(QStringLiteral("addgroup"), hasDoc);
    enable(QStringLiteral("addmask"), active && !hasMask);
    for (const QString& key : {QStringLiteral("editmask"), QStringLiteral("togglemask"),
                               QStringLiteral("invertmask"), QStringLiteral("applymask"),
                               QStringLiteral("deletemask")}) {
        enable(key, hasMask);
    }
    if (QAction* toggle = actions_.value(QStringLiteral("togglemask"))) {
        toggle->setText(hasMask && active->maskEnabled()
                            ? tr("&Disable Layer Mask")
                            : tr("&Enable Layer Mask"));
    }
    if (QAction* paint = actions_.value(QStringLiteral("editmask"))) {
        paint->setText(hasMask && active->maskSelected()
                           ? tr("&Paint in Layer")
                           : tr("&Paint in Mask"));
    }
    enable(QStringLiteral("editadjustment"), active && active->isAdjustment());
    enable(QStringLiteral("deletelayer"), manyLayers);
    enable(QStringLiteral("mergedown"), manyLayers && active && active->indexInParent() > 0);
    enable(QStringLiteral("flatten"), manyLayers);
    enable(QStringLiteral("layerup"), manyLayers && doc->activeIndex() < doc->layerCount() - 1);
    enable(QStringLiteral("layerdown"), manyLayers && doc->activeIndex() > 0);
    enable(QStringLiteral("nextimage"), stack_->count() > 1);
    enable(QStringLiteral("previmage"), stack_->count() > 1);
    const bool clipboardHasImage = !QGuiApplication::clipboard()->image().isNull();
    enable(QStringLiteral("paste"), clipboardHasImage);
    enable(QStringLiteral("pastelayer"), clipboardHasImage);
    enable(QStringLiteral("pasteimage"), clipboardHasImage);
    if (repeatAction_) {
        repeatAction_->setEnabled(lastEffect_ != nullptr);
        repeatAction_->setText(lastEffect_
                                   ? tr("Repeat %1").arg(lastEffect_->name())
                                   : tr("Repeat Last Effect"));
    }
}

void MainWindow::updateZoomControls()
{
    CanvasView* view = currentView();
    if (!view)
        return;
    updatingZoom_ = true;
    const double percent = view->zoom() * 100.0;
    zoomCombo_->setCurrentText(view->isFitMode()
                                   ? tr("Window")
                                   : QStringLiteral("%1%").arg(percent, 0, 'f',
                                                               percent < 10 ? 1 : 0));
    zoomSlider_->setValue(int(std::lround(std::log10(view->zoom()) * 100.0)));
    updatingZoom_ = false;
}

// ----------------------------------------------------------------- tools

void MainWindow::chooseTool(const QString& toolId, bool)
{
    Tool* tool = ToolRegistry::instance().byId(toolId);
    if (!tool)
        return;
    if (currentToolId_ != toolId) {
        previousToolId_ = currentToolId_;
        currentToolId_ = toolId;
    }
    tools_->setCurrentTool(toolId);
    options_->startTool(tool);
    options_->setToolList(ToolsPalette::toolOrder(), toolId);
    for (int i = 0; i < stack_->count(); ++i)
        viewAt(i)->setTool(tool);
    hintLabel_->setText(tool->hint());
}

void MainWindow::cycleToolShortcut(const QString& key, int direction)
{
    const std::vector<Tool*> group = ToolRegistry::instance().sharingShortcut(key);
    if (group.empty())
        return;
    const int count = int(group.size());
    int index = direction > 0 ? 0 : count - 1;
    for (int i = 0; i < count; ++i) {
        if (group[size_t(i)]->id() == currentToolId_) {
            index = ((i + direction) % count + count) % count;
            break;
        }
    }
    chooseTool(group[size_t(index)]->id(), true);
}

// ----------------------------------------------------------------- effects

void MainWindow::applyEffect(Effect* effect)
{
    CanvasView* view = currentView();
    if (!view || !effect)
        return;
    view->commitPendingTool();
    Document* doc = view->document();
    History* history = view->history();
    const Rgba primary = colors_->primary();
    const Rgba secondary = colors_->secondary();

    const auto finish = [this, view](Effect* applied, const ParamValues& values) {
        lastEffect_ = applied;
        lastEffectValues_ = values;
        view->refresh();
        updateEverything();
    };

    if (effect->id() == QLatin1String("curves")) {
        auto* dialog = new CurvesDialog(effect, doc, history, primary, secondary, this);
        dialog->setRepaintCallback([view] { view->refresh(); });
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this, dialog, effect, finish](int result) {
            if (result == QDialog::Accepted)
                finish(effect, dialog->values());
            else
                updateEverything();
        });
        dialog->show();
        return;
    }
    if (effect->id() == QLatin1String("levels")) {
        auto* dialog = new LevelsDialog(effect, doc, history, primary, secondary, this);
        dialog->setRepaintCallback([view] { view->refresh(); });
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this, dialog, effect, finish](int result) {
            if (result == QDialog::Accepted)
                finish(effect, dialog->values());
            else
                updateEverything();
        });
        dialog->show();
        return;
    }
    if (effect->params().empty()) {
        const ParamValues values;
        EffectDialog::applyDirect(effect, doc, history, values, primary, secondary, this);
        finish(effect, values);
        return;
    }

    auto* dialog = new EffectDialog(effect, doc, history, primary, secondary, this);
    dialog->setRepaintCallback([view] { view->refresh(); });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, dialog, effect, finish](int result) {
        if (result == QDialog::Accepted)
            finish(effect, dialog->values());
        else
            updateEverything();
    });
    dialog->show();
}

void MainWindow::addAdjustmentLayer(Effect* effect)
{
    CanvasView* view = currentView();
    Document* doc = view ? view->document() : nullptr;
    if (!doc || !effect)
        return;
    const QString id = effect->id();
    const QString name = effect->name();
    const ParamValues values = ParamValues::defaults(effect->params());
    structuralEdit(tr("New Adjustment Layer"), QStringLiteral("adjust"),
                   [doc, id, name, values] {
                       doc->addAdjustmentLayer(id, values, name);
                   });
    // Straight on into the parameters, which is what the submenu was picked
    // for; the layer itself is already there if the dialog is cancelled.
    if (!effect->params().empty())
        editAdjustmentLayer();
}

void MainWindow::editAdjustmentLayer()
{
    CanvasView* view = currentView();
    Document* doc = view ? view->document() : nullptr;
    Layer* layer = doc ? doc->activeNode() : nullptr;
    if (!layer || !layer->isAdjustment() || layer->effects().empty())
        return;
    Effect* effect =
        EffectRegistry::instance().byId(layer->effects().front().effectId);
    if (!effect || effect->params().empty()) {
        hintLabel_->setText(tr("%1 has no settings")
                                .arg(effect ? effect->name() : layer->name));
        return;
    }
    auto* dialog = new StoredEffectDialog(effect, doc, view->history(), layer, 0, this);
    dialog->setRepaintCallback([view] { view->refresh(); });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this] { updateEverything(); });
    dialog->show();
}

void MainWindow::repeatLastEffect()
{
    CanvasView* view = currentView();
    if (!view || !lastEffect_)
        return;
    view->commitPendingTool();
    EffectDialog::applyDirect(lastEffect_, view->document(), view->history(),
                              lastEffectValues_, colors_->primary(), colors_->secondary(),
                              this);
    view->refresh();
    updateEverything();
}

// ----------------------------------------------------------------- clipboard

void MainWindow::doCopy(bool merged)
{
    Document* doc = currentDocument();
    if (!doc)
        return;
    if (CanvasView* view = currentView())
        view->commitPendingTool();
    const Image& source = merged ? doc->composite() : doc->activeLayer()->image;
    const Selection& selection = doc->selection();
    const Rect bounds = selection.isActive() ? selection.bounds() : doc->rect();
    if (bounds.isEmpty())
        return;
    Image cut = source.cropped(bounds);
    if (const Mask* mask = selection.clipMask()) {
        for (int y = 0; y < bounds.h; ++y) {
            Rgba* line = cut.row(y);
            const float* sel = mask->row(bounds.y + y);
            for (int x = 0; x < bounds.w; ++x)
                line[x].a = toByte(line[x].a * (1.0f / 255.0f) * sel[bounds.x + x]);
        }
    }
    QGuiApplication::clipboard()->setImage(cut.toQImage());
    hintLabel_->setText(tr("Copied %1 x %2 pixels")
                            .arg(bounds.w).arg(bounds.h));
    updateActionStates();
}

void MainWindow::doCut()
{
    doCopy(false);
    eraseSelection();
}

void MainWindow::doPaste(bool intoNewLayer, bool intoNewImage)
{
    const QImage clip = QGuiApplication::clipboard()->image();
    if (clip.isNull()) {
        hintLabel_->setText(tr("The clipboard holds no image"));
        return;
    }
    Image pasted = Image::fromQImage(clip);
    if (intoNewImage) {
        auto doc = std::make_unique<Document>(pasted.width(), pasted.height(),
                                              Rgba(0, 0, 0, 0));
        doc->activeLayer()->image = pasted;
        doc->setTitle(tr("Untitled %1").arg(untitledCounter_++));
        doc->invalidate();
        doc->setModified(true);
        addView(new CanvasView(std::move(doc), this));
        return;
    }

    CanvasView* view = currentView();
    if (!view)
        return;
    view->commitPendingTool();
    Document* doc = view->document();
    History* history = view->history();

    if (pasted.width() > doc->width() || pasted.height() > doc->height()) {
        // Too big to drop straight in, so hand it to the placement tool rather
        // than trimming it on the way and leaving nothing to drag back.
        placePicture(std::move(pasted), tr("Pasted"), intoNewLayer);
        return;
    }

    // The pasted pixels land at the top left of the selection, or at the origin.
    const Rect bounds = doc->selection().isActive() ? doc->selection().bounds()
                                                    : Rect(0, 0, doc->width(), doc->height());
    const int x = bounds.x;
    const int y = bounds.y;
    const QString name = intoNewLayer ? tr("Paste Into New Layer")
                                      : tr("Paste");
    DocumentState before = doc->snapshot();
    if (intoNewLayer) {
        auto layer = std::make_unique<Layer>(Image(doc->width(), doc->height()),
                                             doc->uniqueLayerName(tr("Pasted")));
        layer->image.blit(pasted, x, y);
        doc->addLayer(std::move(layer));
    } else {
        Layer* layer = doc->activeLayer();
        Image staged(doc->width(), doc->height());
        staged.blit(pasted, x, y);
        ops::imageOver(layer->image, staged, nullptr, BlendMode::Normal,
                       Rect(x, y, pasted.width(), pasted.height()));
    }
    // Select what was just pasted so it can be moved straight away.
    Mask mask(doc->width(), doc->height());
    for (int row = 0; row < pasted.height(); ++row) {
        const int ty = y + row;
        if (ty < 0 || ty >= doc->height())
            continue;
        for (int column = 0; column < pasted.width(); ++column) {
            const int tx = x + column;
            if (tx >= 0 && tx < doc->width())
                mask.setAt(tx, ty, 1.0f);
        }
    }
    doc->selection().setMask(std::move(mask));
    DocumentState after = doc->snapshot();
    history->push(std::make_unique<DocumentEdit>(name, QStringLiteral("paste"),
                                                 std::move(before), std::move(after)));
    doc->invalidate();
    view->refresh();
    chooseTool(QStringLiteral("move-selected"));
    updateEverything();
    hintLabel_->setText(tr("Pasted %1 x %2 pixels at %3, %4")
                            .arg(pasted.width()).arg(pasted.height()).arg(x).arg(y));
}

// ----------------------------------------------------------------- editing

void MainWindow::eraseSelection()
{
    CanvasView* view = currentView();
    if (!view)
        return;
    view->commitPendingTool();
    Document* doc = view->document();
    Layer* layer = doc->activeLayer();
    const Selection& selection = doc->selection();
    const Rect bounds = selection.isActive() ? selection.bounds() : doc->rect();
    if (bounds.isEmpty())
        return;
    EditScope scope(*doc, *view->history(), layer, tr("Erase Selection"),
                    QStringLiteral("delete"), bounds);
    if (const Mask* mask = selection.clipMask()) {
        Mask copy = *mask;
        ops::eraseMask(layer->image, copy, 1.0f, bounds);
    } else {
        ops::fillRegion(layer->image, bounds, Rgba(0, 0, 0, 0));
    }
    scope.commit();
    doc->invalidateLayer(layer);
    view->refresh();
    updateEverything();
}

void MainWindow::fillSelection()
{
    CanvasView* view = currentView();
    if (!view)
        return;
    view->commitPendingTool();
    Document* doc = view->document();
    Layer* layer = doc->activeLayer();
    const Selection& selection = doc->selection();
    const Rect bounds = selection.isActive() ? selection.bounds() : doc->rect();
    if (bounds.isEmpty())
        return;
    EditScope scope(*doc, *view->history(), layer, tr("Fill Selection"),
                    QStringLiteral("bucket"), bounds);
    Mask mask = selection.isActive() ? *selection.clipMask()
                                     : Mask(doc->width(), doc->height(), 1.0f);
    ops::colorOver(layer->image, mask, colors_->primary(), BlendMode::Normal, bounds);
    scope.commit();
    doc->invalidateLayer(layer);
    view->refresh();
    updateEverything();
}

void MainWindow::structuralEdit(const QString& name, const QString& icon,
                                const std::function<void()>& action)
{
    CanvasView* view = currentView();
    if (!view)
        return;
    view->commitPendingTool();
    recordDocumentEdit(*view->document(), *view->history(), name, icon, action);
    view->refresh();
    updateEverything();
}

void MainWindow::cropToSelection()
{
    Document* doc = currentDocument();
    if (!doc || !doc->selection().isActive())
        return;
    const Rect bounds = doc->selection().bounds();
    if (bounds.isEmpty())
        return;
    structuralEdit(tr("Crop to Selection"), QStringLiteral("crop"),
                   [doc, bounds] { doc->cropTo(bounds); });
    if (CanvasView* view = currentView())
        view->zoomToFit();
}

void MainWindow::resizeImage()
{
    Document* doc = currentDocument();
    if (!doc)
        return;
    ResizeDialog dialog(tr("Resize Image"), doc->width(), doc->height(),
                        doc->resolution(), unit_, false, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const int width = dialog.newWidth();
    const int height = dialog.newHeight();
    const double resolution = dialog.newResolution();
    const bool smooth = dialog.smoothResampling();
    // With resampling off the pixels are left alone: only the print size, that
    // is the resolution, changes.
    const bool resample = dialog.resamples()
        && (width != doc->width() || height != doc->height());
    if (!resample && std::abs(resolution - doc->resolution()) < 1e-9)
        return;
    structuralEdit(tr("Resize Image"), QStringLiteral("resize"),
                   [doc, width, height, resolution, smooth, resample] {
                       doc->setResolution(resolution);
                       if (!resample)
                           return;
                       if (smooth) {
                           doc->resizeCanvas(width, height, Anchor::Center, true);
                       } else {
                           for (int i = 0; i < doc->layerCount(); ++i) {
                               Layer* layer = doc->layerAt(i);
                               layer->image = ops::resize(layer->image, width, height,
                                                          false);
                           }
                           doc->resizeCanvas(width, height, Anchor::TopLeft, false);
                       }
                   });
    if (CanvasView* view = currentView())
        view->zoomToFit();
}

void MainWindow::changeCanvasSize()
{
    Document* doc = currentDocument();
    if (!doc)
        return;
    ResizeDialog dialog(tr("Canvas Size"), doc->width(), doc->height(),
                        doc->resolution(), unit_, true, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const int width = dialog.newWidth();
    const int height = dialog.newHeight();
    const Anchor anchor = dialog.anchor();
    structuralEdit(tr("Canvas Size"), QStringLiteral("canvas"),
                   [doc, width, height, anchor] {
                       doc->resizeCanvas(width, height, anchor, false);
                   });
    if (CanvasView* view = currentView())
        view->zoomToFit();
}

void MainWindow::setMeasurementUnit(Unit unit)
{
    unit_ = unit;
    if (QAction* action = actions_.value(QStringLiteral("unit-") + units::unitId(unit)))
        action->setChecked(true);
    for (int i = 0; i < stack_->count(); ++i)
        viewAt(i)->setRulerUnit(unit);
    updateStatusBar();
}

void MainWindow::flattenImage()
{
    Document* doc = currentDocument();
    if (!doc || doc->layerCount() < 2)
        return;
    structuralEdit(tr("Flatten"), QStringLiteral("flatten"),
                   [doc] { doc->flatten(); });
}

void MainWindow::showSettings()
{
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Settings"));
    dialog.setWindowIcon(icons::get(QStringLiteral("settings"), 22));
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* appearance = new QGroupBox(tr("Appearance"), &dialog);
    auto* appearanceForm = new QFormLayout(appearance);
    auto* themeCombo = new QComboBox(appearance);
    themeCombo->addItems({tr("Dark"), tr("Light")});
    themeCombo->setCurrentIndex(theme::current() == theme::Mode::Dark ? 0 : 1);
    appearanceForm->addRow(tr("Theme"), themeCombo);
    // The locale name travels as the item's data, so the labels can be
    // translated without the setting changing with them.
    auto* languageCombo = new QComboBox(appearance);
    languageCombo->setObjectName(QStringLiteral("language"));
    languageCombo->addItem(tr("System"), QStringLiteral("system"));
    languageCombo->addItem(tr("English"), QStringLiteral("en"));
    languageCombo->addItem(tr("Russian"), QStringLiteral("ru"));
    const QString language =
        settings.value(QStringLiteral("language"), QStringLiteral("system")).toString();
    const int languageIndex = languageCombo->findData(language);
    languageCombo->setCurrentIndex(languageIndex < 0 ? 0 : languageIndex);
    appearanceForm->addRow(tr("Language"), languageCombo);
    root->addWidget(appearance);

    auto* defaults = new QGroupBox(tr("New images"), &dialog);
    auto* defaultsForm = new QFormLayout(defaults);
    auto* width = new QSpinBox(defaults);
    width->setRange(1, 60000);
    width->setValue(settings.value(QStringLiteral("newWidth"), 800).toInt());
    width->setSuffix(tr(" px"));
    auto* height = new QSpinBox(defaults);
    height->setRange(1, 60000);
    height->setValue(settings.value(QStringLiteral("newHeight"), 600).toInt());
    height->setSuffix(tr(" px"));
    defaultsForm->addRow(tr("Default width"), width);
    defaultsForm->addRow(tr("Default height"), height);
    root->addWidget(defaults);

    auto* historyBox = new QGroupBox(tr("History"), &dialog);
    auto* historyForm = new QFormLayout(historyBox);
    auto* steps = new QSpinBox(historyBox);
    steps->setRange(10, 10000);
    steps->setValue(settings.value(QStringLiteral("historySteps"), 250).toInt());
    auto* memory = new QSpinBox(historyBox);
    memory->setRange(64, 64000);
    memory->setSingleStep(64);
    memory->setSuffix(tr(" MB"));
    memory->setValue(settings.value(QStringLiteral("historyMemory"), 2048).toInt());
    historyForm->addRow(tr("Maximum steps"), steps);
    historyForm->addRow(tr("Memory budget"), memory);
    root->addWidget(historyBox);

    auto* savingBox = new QGroupBox(tr("Saving work"), &dialog);
    auto* savingForm = new QFormLayout(savingBox);
    auto* autosave = new QCheckBox(
        tr("Keep a recovery copy of every changed image"), savingBox);
    autosave->setObjectName(QStringLiteral("autosave"));
    autosave->setChecked(recoveryOptions_.autosave);
    auto* autosaveMinutes = new QSpinBox(savingBox);
    autosaveMinutes->setObjectName(QStringLiteral("autosaveMinutes"));
    autosaveMinutes->setRange(recovery::kMinAutosaveMinutes,
                              recovery::kMaxAutosaveMinutes);
    autosaveMinutes->setValue(recoveryOptions_.autosaveMinutes);
    autosaveMinutes->setSuffix(tr(" min"));
    autosaveMinutes->setEnabled(recoveryOptions_.autosave);
    connect(autosave, &QCheckBox::toggled, autosaveMinutes, &QWidget::setEnabled);
    auto* restoreSession = new QCheckBox(
        tr("Offer to reopen the images from the last session"), savingBox);
    restoreSession->setObjectName(QStringLiteral("restoreSession"));
    restoreSession->setChecked(recoveryOptions_.restoreSession);
    auto* keepDays = new QSpinBox(savingBox);
    keepDays->setObjectName(QStringLiteral("keepDays"));
    keepDays->setRange(recovery::kMinKeepDays, recovery::kMaxKeepDays);
    keepDays->setValue(recoveryOptions_.keepDays);
    keepDays->setSuffix(tr(" days"));
    savingForm->addRow(autosave);
    savingForm->addRow(tr("Autosave every"), autosaveMinutes);
    savingForm->addRow(restoreSession);
    savingForm->addRow(tr("Keep recovery files for"), keepDays);
    root->addWidget(savingBox);

    auto* layoutBox = new QGroupBox(tr("Window"), &dialog);
    auto* layoutRow = new QHBoxLayout(layoutBox);
    auto* resetLayout = new QPushButton(tr("Reset panel layout"), layoutBox);
    layoutRow->addWidget(resetLayout);
    layoutRow->addStretch(1);
    root->addWidget(layoutBox);

    bool resetRequested = false;
    connect(resetLayout, &QPushButton::clicked, &dialog, [&] {
        resetRequested = true;
        dialog.accept();
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const bool dark = themeCombo->currentIndex() == 0;
    if ((theme::current() == theme::Mode::Dark) != dark) {
        theme::apply(*qApp, dark ? theme::Mode::Dark : theme::Mode::Light);
        icons::clearCache();
        refreshIcons();
        settings.setValue(QStringLiteral("theme"), dark ? QStringLiteral("dark")
                                                        : QStringLiteral("light"));
    }
    // Everything the window shows was built in the language it started in,
    // from the menus to the names of the undo steps already recorded, so the
    // change waits for the next run rather than leaving the two mixed.
    const QString chosenLanguage = languageCombo->currentData().toString();
    if (chosenLanguage != language) {
        settings.setValue(QStringLiteral("language"), chosenLanguage);
        QMessageBox::information(
            this, tr("Language"),
            tr("Tessel will be in the new language the next time it starts."));
    }
    settings.setValue(QStringLiteral("newWidth"), width->value());
    settings.setValue(QStringLiteral("newHeight"), height->value());
    settings.setValue(QStringLiteral("historySteps"), steps->value());
    settings.setValue(QStringLiteral("historyMemory"), memory->value());
    recoveryOptions_.autosave = autosave->isChecked();
    recoveryOptions_.autosaveMinutes = autosaveMinutes->value();
    recoveryOptions_.restoreSession = restoreSession->isChecked();
    recoveryOptions_.keepDays = keepDays->value();
    recovery::storeOptions(settings, recoveryOptions_);
    if (autosaver_) {
        autosaver_->setIntervalMinutes(recoveryOptions_.autosaveMinutes);
        autosaver_->setEnabled(recoveryOptions_.autosave);
    }
    defaultWidth_ = width->value();
    defaultHeight_ = height->value();
    for (int i = 0; i < stack_->count(); ++i) {
        viewAt(i)->history()->setLimits(steps->value(),
                                        size_t(memory->value()) * 1024 * 1024);
    }
    if (resetRequested) {
        resetPanelLayout();
    }
    updateEverything();
}

QList<ShortcutEntry> MainWindow::collectShortcuts() const
{
    QList<ShortcutEntry> entries;
    auto clean = [](QString text) {
        text.remove(QLatin1Char('&'));
        if (text.endsWith(QStringLiteral("...")))
            text.chop(3);
        return text.trimmed();
    };

    // Tool letters come from the registry, including the groups that cycle.
    QHash<QString, QStringList> byKey;
    QStringList keyOrder;
    for (Tool* tool : ToolRegistry::instance().all()) {
        if (tool->shortcut().isEmpty())
            continue;
        if (!byKey.contains(tool->shortcut()))
            keyOrder.append(tool->shortcut());
        byKey[tool->shortcut()].append(tool->name());
    }
    for (const QString& key : keyOrder) {
        const QStringList names = byKey.value(key);
        entries.append({tr("Tools"),
                        names.size() > 1
                            ? tr("%1 (press again to cycle, Shift to go back)")
                                  .arg(names.join(QStringLiteral(", ")))
                            : names.first(),
                        key});
    }
    entries.append({tr("Tools"), tr("Open the tool list"),
                    QStringLiteral("Alt+T")});
    entries.append({tr("Tools"), tr("Swap the palette colours"),
                    QStringLiteral("X")});
    entries.append({tr("Tools"), tr("Reset the palette colours"),
                    QStringLiteral("D")});

    // Everything else is read back out of the menus that were actually built.
    std::function<void(const QString&, QMenu*)> walk = [&](const QString& group,
                                                           QMenu* menu) {
        for (QAction* action : menu->actions()) {
            if (action->isSeparator())
                continue;
            if (QMenu* submenu = action->menu()) {
                walk(group, submenu);
                continue;
            }
            if (action->shortcut().isEmpty())
                continue;
            entries.append({group, clean(action->text()),
                            action->shortcut().toString(QKeySequence::NativeText)});
        }
    };
    for (QAction* menuAction : menuBar()->actions()) {
        if (QMenu* menu = menuAction->menu())
            walk(clean(menuAction->text()), menu);
    }

    entries.append({tr("Canvas"), tr("Zoom around the pointer"),
                    QStringLiteral("Ctrl+Wheel")});
    entries.append({tr("Canvas"), tr("Pan"),
                    tr("Middle drag, or hold Space")});
    entries.append({tr("Canvas"),
                    tr("Paint with the secondary colour"),
                    tr("Right drag")});
    entries.append({tr("Canvas"),
                    tr("Finish, then deselect"), QStringLiteral("Enter")});
    entries.append({tr("Canvas"),
                    tr("Abandon, then deselect"), QStringLiteral("Esc")});
    return entries;
}

void MainWindow::printImage()
{
    Document* doc = currentDocument();
    if (!doc)
        return;
    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(doc->displayName());
    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Print Image"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    QPainter painter;
    if (!painter.begin(&printer)) {
        QMessageBox::warning(this, tr("Cannot print"),
                             tr("The printer could not be opened."));
        return;
    }
    // White paper shows through anything transparent.
    const Image flat = io::flattenOnColor(doc->composite(), Rgba(255, 255, 255, 255));
    const QImage image = flat.toQImage();
    const QRect page = printer.pageLayout().paintRectPixels(printer.resolution());
    const QSize scaled = image.size().scaled(page.size(), Qt::KeepAspectRatio);
    const QRect target(page.x() + (page.width() - scaled.width()) / 2,
                       page.y() + (page.height() - scaled.height()) / 2,
                       scaled.width(), scaled.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, image);
    painter.end();
    hintLabel_->setText(tr("Sent %1 to the printer").arg(doc->displayName()));
}

void MainWindow::copySelection()
{
    Document* doc = currentDocument();
    if (!doc || !doc->selection().isActive()) {
        hintLabel_->setText(tr("There is no selection to copy"));
        return;
    }
    // The outline travels as a greyscale PNG under a private MIME type, so it
    // can be pasted into any image open in Tessel.
    const Mask& mask = *doc->selection().clipMask();
    Image grey(mask.width(), mask.height());
    for (int y = 0; y < mask.height(); ++y) {
        const float* line = mask.row(y);
        Rgba* out = grey.row(y);
        for (int x = 0; x < mask.width(); ++x) {
            const uint8_t v = toByte(line[x]);
            out[x] = Rgba(v, v, v, 255);
        }
    }
    QByteArray payload;
    QBuffer buffer(&payload);
    buffer.open(QIODevice::WriteOnly);
    grey.toQImage().save(&buffer, "PNG");
    auto* data = new QMimeData;
    data->setData(QStringLiteral("application/x-tessel-selection"), payload);
    QGuiApplication::clipboard()->setMimeData(data);
    hintLabel_->setText(tr("Copied the selection outline"));
}

void MainWindow::pasteSelection()
{
    CanvasView* view = currentView();
    if (!view)
        return;
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    if (!data || !data->hasFormat(QStringLiteral("application/x-tessel-selection"))) {
        hintLabel_->setText(tr("The clipboard holds no selection"));
        return;
    }
    QImage grey;
    if (!grey.loadFromData(data->data(QStringLiteral("application/x-tessel-selection")),
                           "PNG"))
        return;
    Document* doc = view->document();
    Mask mask(doc->width(), doc->height());
    for (int y = 0; y < std::min(grey.height(), doc->height()); ++y) {
        float* line = mask.row(y);
        for (int x = 0; x < std::min(grey.width(), doc->width()); ++x)
            line[x] = qRed(grey.pixel(x, y)) / 255.0f;
    }
    recordSelectionEdit(*doc, *view->history(), tr("Paste Selection"),
                        [&] { doc->selection().setMask(std::move(mask)); });
    view->refresh();
    updateEverything();
}

void MainWindow::setLayerVisible(int index, bool visible)
{
    CanvasView* view = currentView();
    if (!view)
        return;
    Layer* layer = view->document()->layerAt(index);
    if (!layer || layer->visible == visible)
        return;
    const int uid = layer->uid();
    const bool before = layer->visible;
    view->history()->push(std::make_unique<PropertyEdit>(
        visible ? tr("Show Layer") : tr("Hide Layer"),
        QStringLiteral("visible"),
        [uid, before](Document& doc) {
            if (Layer* target = doc.layerByUid(uid)) {
                target->visible = before;
                doc.invalidate();
            }
        },
        [uid, visible](Document& doc) {
            if (Layer* target = doc.layerByUid(uid)) {
                target->visible = visible;
                doc.invalidate();
            }
        }));
    layer->visible = visible;
    view->document()->invalidate();
    view->refresh();
    updateEverything();
}

void MainWindow::goToLayer(int index)
{
    Document* doc = currentDocument();
    if (!doc)
        return;
    const int target = std::clamp(index, 0, doc->layerCount() - 1);
    if (target == doc->activeIndex())
        return;
    doc->setActiveIndex(target);
    refreshPanels();
    updateStatusBar();
    if (const Layer* layer = doc->activeNode())
        hintLabel_->setText(tr("Active layer: %1").arg(layer->name));
}

// ----------------------------------------------------------------- layers

void MainWindow::addLayer()
{
    Document* doc = currentDocument();
    if (!doc)
        return;
    structuralEdit(tr("Add New Layer"), QStringLiteral("layer-add"),
                   [doc] { doc->addEmptyLayer(); });
}

void MainWindow::deleteLayer()
{
    Document* doc = currentDocument();
    if (!doc || doc->layerCount() < 2)
        return;
    const int index = doc->activeIndex();
    structuralEdit(tr("Delete Layer"), QStringLiteral("layer-delete"),
                   [doc, index] { doc->removeLayer(index); });
}

void MainWindow::duplicateLayer()
{
    Document* doc = currentDocument();
    if (!doc)
        return;
    structuralEdit(tr("Duplicate Layer"), QStringLiteral("layer-duplicate"),
                   [doc] { doc->duplicateLayer(doc->activeIndex()); });
}

void MainWindow::mergeLayerDown()
{
    Document* doc = currentDocument();
    const Layer* active = doc ? doc->activeNode() : nullptr;
    // Merging down goes into the layer below inside the same group, so a layer
    // at the bottom of its group has nothing to merge with.
    if (!doc || !active || active->indexInParent() <= 0)
        return;
    const int index = doc->activeIndex();
    structuralEdit(tr("Merge Layer Down"), QStringLiteral("layer-merge"),
                   [doc, index] { doc->mergeLayerDown(index); });
}

void MainWindow::importLayerFromFile()
{
    CanvasView* view = currentView();
    Document* doc = currentDocument();
    if (!view || !doc)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import as Layer"), lastDirectory_, io::openFilter());
    if (path.isEmpty())
        return;
    QString error;
    // Loaded at its own size. Nothing is trimmed until the user says where it
    // goes, so an oversized picture can still be dragged around afterwards.
    Image picture = io::openFlattened(path, &error);
    if (picture.isNull()) {
        QMessageBox::warning(this, tr("Cannot import image"), error);
        return;
    }
    lastDirectory_ = QFileInfo(path).absolutePath();
    placePicture(std::move(picture), QFileInfo(path).completeBaseName(), true);
}

// Shared by Import and Paste. A picture that fits simply lands; one that does
// not lets the user choose between growing the canvas and positioning it by
// hand, in which case only the part over the canvas is kept.
void MainWindow::placePicture(Image picture, const QString& name, bool onNewLayer)
{
    CanvasView* view = currentView();
    Document* doc = currentDocument();
    if (!view || !doc || picture.isNull())
        return;

    const bool fits = picture.width() <= doc->width()
        && picture.height() <= doc->height();
    if (!fits) {
        QMessageBox question(this);
        question.setWindowTitle(tr("Picture is larger than the canvas"));
        question.setText(tr("%1 is %2 x %3, and the canvas is %4 x %5.")
                             .arg(name)
                             .arg(picture.width()).arg(picture.height())
                             .arg(doc->width()).arg(doc->height()));
        question.setInformativeText(
            tr("Grow the canvas to fit it, or place it by hand and keep only the "
               "part that lands on the canvas."));
        QPushButton* grow = question.addButton(tr("Grow Canvas"),
                                               QMessageBox::AcceptRole);
        QPushButton* place = question.addButton(tr("Place by Hand"),
                                                QMessageBox::ActionRole);
        question.addButton(QMessageBox::Cancel);
        question.setDefaultButton(grow);
        question.exec();
        if (question.clickedButton() == grow) {
            const int width = std::max(doc->width(), picture.width());
            const int height = std::max(doc->height(), picture.height());
            structuralEdit(tr("Grow Canvas"), QStringLiteral("canvas"),
                           [doc, width, height] {
                               doc->resizeCanvas(width, height, Anchor::TopLeft, false);
                           });
            view->zoomToFit();
        } else if (question.clickedButton() != place) {
            return;
        }
    }

    chooseTool(QStringLiteral("place"));
    ToolContext context = view->makeContext();
    if (!startPlacement(context, std::move(picture), name, onNewLayer)) {
        hintLabel_->setText(tr("Nothing to place"));
        return;
    }
    view->setFocus();
    updateEverything();
}

void MainWindow::moveLayer(int delta)
{
    Document* doc = currentDocument();
    if (!doc)
        return;
    const int from = doc->activeIndex();
    const int to = from + delta;
    if (to < 0 || to >= doc->layerCount())
        return;
    structuralEdit(delta > 0 ? tr("Move Layer Up")
                             : tr("Move Layer Down"),
                   delta > 0 ? QStringLiteral("up") : QStringLiteral("down"),
                   [doc, from, to] { doc->moveLayer(from, to); });
}

void MainWindow::showLayerProperties()
{
    CanvasView* view = currentView();
    if (!view)
        return;
    auto* dialog = new LayerPropertiesDialog(view->document(), view->history(), this);
    dialog->setRepaintCallback([view] { view->refresh(); });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this] { updateEverything(); });
    dialog->show();
}

// ----------------------------------------------------------------- recovery

void MainWindow::setupAutosave()
{
    autosaver_ = std::make_unique<recovery::Autosaver>(this);
    autosaver_->setSource([this] {
        std::vector<recovery::Autosaver::Request> requests;
        for (int i = 0; i < stack_->count(); ++i) {
            CanvasView* view = viewAt(i);
            if (!view || !view->document()->isModified())
                continue; // a saved document has nothing to lose
            Document* doc = view->document();
            History* history = view->history();
            recovery::Autosaver::Request request;
            request.key = recoveryKey(view);
            request.originalPath = doc->filePath();
            request.title = doc->displayName();
            // The length of the history together with the position in it moves
            // on every edit, and on an undo back to a state that was already
            // written as well.
            request.revision = (qint64(history->count()) << 32)
                | qint64(uint32_t(history->currentIndex() + 1));
            request.snapshot = [doc] { return doc->snapshot(); };
            requests.push_back(std::move(request));
        }
        return requests;
    });
    // The status bar rather than a message box: autosaving is a background
    // courtesy, and a modal error in the middle of a brush stroke would be
    // worse than the failure it reports.
    connect(autosaver_.get(), &recovery::Autosaver::failed, this,
            [this](const QString& message) {
                if (hintLabel_)
                    hintLabel_->setText(message);
            });
    autosaver_->setIntervalMinutes(recoveryOptions_.autosaveMinutes);
    autosaver_->setEnabled(recoveryOptions_.autosave);
}

int MainWindow::recoveryKey(CanvasView* view)
{
    const auto known = recoveryKeys_.constFind(view);
    if (known != recoveryKeys_.constEnd())
        return known.value();
    const int key = ++recoveryCounter_;
    recoveryKeys_.insert(view, key);
    return key;
}

void MainWindow::dropRecoveryFile(CanvasView* view)
{
    if (!autosaver_ || !view)
        return;
    const auto known = recoveryKeys_.constFind(view);
    if (known == recoveryKeys_.constEnd())
        return;
    autosaver_->forget(known.value());
    recoveryKeys_.remove(view);
}

void MainWindow::autosaveNow()
{
    if (!autosaver_)
        return;
    autosaver_->saveNow();
    autosaver_->waitForIdle();
}

recovery::OpenDocuments MainWindow::captureOpenDocuments() const
{
    recovery::OpenDocuments documents;
    const int current = stack_->currentIndex();
    for (int i = 0; i < stack_->count(); ++i) {
        CanvasView* view = viewAt(i);
        if (!view || view->document()->filePath().isEmpty())
            continue; // an unnamed document is the recovery case, not this one
        if (i == current)
            documents.current = static_cast<int>(documents.paths.size());
        documents.paths.append(view->document()->filePath());
    }
    return documents;
}

bool MainWindow::openRecovered(const recovery::Entry& entry)
{
    QString error;
    std::unique_ptr<Document> doc = io::openDocument(entry.path, &error);
    if (!doc) {
        QMessageBox::warning(this, tr("Cannot recover image"), error);
        return false;
    }
    // openDocument names a document after the file it read, which here is the
    // recovery copy. The document is given back its own path instead, and is
    // left modified, so that saving it cannot touch the copy and closing it
    // cannot happen without a prompt.
    doc->setFilePath(entry.originalPath);
    doc->setTitle(tr("Recovered %1").arg(untitledCounter_++));
    doc->setModified(true);
    addView(new CanvasView(std::move(doc), this));
    hintLabel_->setText(tr("Recovered %1").arg(entry.displayName()));
    return true;
}

void MainWindow::offerRecovery()
{
    recovery::pruneOlderThan(recoveryOptions_.keepDays);
    std::vector<recovery::Abandoned> abandoned = recovery::findAbandoned();
    std::vector<recovery::Entry> entries = recovery::allEntries(abandoned);
    if (entries.empty()) {
        recovery::discard(abandoned); // directories with nothing in them
        return;
    }
    RecoveryDialog dialog(entries, this);
    dialog.exec();
    switch (dialog.choice()) {
    case RecoveryDialog::Choice::Recover: {
        bool all = true;
        for (const recovery::Entry& entry : entries)
            all = openRecovered(entry) && all;
        // A copy that could not be read is kept, so there is still something
        // to try again with or to look at by hand.
        if (all)
            recovery::discard(abandoned);
        break;
    }
    case RecoveryDialog::Choice::Discard:
        recovery::discard(abandoned);
        break;
    case RecoveryDialog::Choice::Later:
        break; // left alone, and offered again next time
    }
}

void MainWindow::offerLastSession()
{
    if (!recoveryOptions_.restoreSession)
        return;
    const recovery::OpenDocuments last = recovery::existingOnly(lastSession_);
    if (last.paths.isEmpty())
        return;
    const QString question = last.paths.size() == 1
        ? tr("Reopen %1 from the last session?")
              .arg(QFileInfo(last.paths.first()).fileName())
        : tr("Reopen the %n images from the last session?", "",
             int(last.paths.size()));
    if (QMessageBox::question(this, tr("Reopen last session?"), question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
        != QMessageBox::Yes) {
        return;
    }
    openPaths(last.paths);
    if (last.current < 0 || last.current >= last.paths.size())
        return;
    const QString front = last.paths.at(last.current);
    for (int i = 0; i < stack_->count(); ++i) {
        if (viewAt(i)->document()->filePath() == front) {
            setCurrentView(i);
            break;
        }
    }
}

void MainWindow::startSession(const QStringList& paths)
{
    // Crashed work first: it is the only thing here that cannot be opened
    // again from somewhere else.
    offerRecovery();
    // Files named on the command line are what the user asked for just now,
    // so they replace the reopen offer rather than queueing behind it.
    if (paths.isEmpty())
        offerLastSession();
    openPaths(paths);
    if (stack_->count() == 0)
        newDocument(defaultWidth_, defaultHeight_, Rgba(255, 255, 255, 255));
}

// ----------------------------------------------------------------- settings

void MainWindow::restoreSettings()
{
    QSettings settings;
    recentFiles_ = settings.value(QStringLiteral("recentFiles")).toStringList();
    recoveryOptions_ = recovery::loadOptions(settings);
    lastSession_ = recovery::loadOpenDocuments(settings);
    defaultWidth_ = settings.value(QStringLiteral("newWidth"), 800).toInt();
    defaultHeight_ = settings.value(QStringLiteral("newHeight"), 600).toInt();
    const QByteArray geometry = settings.value(QStringLiteral("geometry")).toByteArray();
    if (geometry.isEmpty()) {
        const QRect available = screen() ? screen()->availableGeometry()
                                         : QRect(0, 0, 1280, 800);
        resize(std::min(1500, int(available.width() * 0.9)),
               std::min(950, int(available.height() * 0.9)));
    } else {
        restoreGeometry(geometry);
        restoreState(settings.value(QStringLiteral("windowState")).toByteArray(),
                     kLayoutVersion);
    }
    restorePanels(settings);
    if (QAction* grid = actions_.value(QStringLiteral("grid")))
        grid->setChecked(settings.value(QStringLiteral("pixelGrid"), false).toBool());
    if (QAction* rulers = actions_.value(QStringLiteral("rulers")))
        rulers->setChecked(settings.value(QStringLiteral("rulers"), false).toBool());
    setMeasurementUnit(units::unitFromId(
        settings.value(QStringLiteral("unit"), units::unitId(Unit::Pixels)).toString()));
}

void MainWindow::storeSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("recentFiles"), recentFiles_);
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("windowState"), saveState(kLayoutVersion));
    savePanels(settings);
    settings.setValue(QStringLiteral("pixelGrid"),
                      actions_.value(QStringLiteral("grid"))->isChecked());
    settings.setValue(QStringLiteral("rulers"),
                      actions_.value(QStringLiteral("rulers"))->isChecked());
    settings.setValue(QStringLiteral("unit"), units::unitId(unit_));
    recovery::storeOptions(settings, recoveryOptions_);
    recovery::storeOpenDocuments(
        settings, recoveryOptions_.restoreSession ? lastSession_
                                                  : recovery::OpenDocuments());
}

// ----------------------------------------------------------------- events

void MainWindow::closeEvent(QCloseEvent* event)
{
    lastSession_ = captureOpenDocuments();
    while (stack_->count() > 0) {
        const int index = stack_->count() - 1;
        if (!promptToSave(index)) {
            event->ignore();
            return;
        }
        CanvasView* view = viewAt(index);
        stack_->removeWidget(view);
        delete view;
    }
    storeSettings();
    // Reached only once every document is dealt with, which is what makes the
    // leftover files at the next startup mean "this run did not finish".
    if (autosaver_)
        autosaver_->discardAll();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    if (!paths.isEmpty()) {
        openPaths(paths);
        event->acceptProposedAction();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == canvasHost_
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        // The dock layout has just handed the central widget a new rectangle,
        // because the window was resized or a panel came or went.
        layOutCanvas();
    } else if (event->type() == QEvent::MouseMove
               && QApplication::mouseButtons() == Qt::NoButton
               && !QApplication::activePopupWidget()) {
        // A grab left behind, see createPanels.
        if (QWidget* grabber = QWidget::mouseGrabber())
            grabber->releaseMouse();
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace tessel
