#pragma once

#include <memory>
#include <vector>

#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include "core/Document.h"
#include "core/Recovery.h"
#include "core/Units.h"
#include "effects/Effect.h"
#include "ui/CanvasView.h"
#include "ui/ColorPanel.h"
#include "ui/Dialogs.h"
#include "ui/Docks.h"

class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QSlider;
class QStackedWidget;
class QToolButton;
class QToolBar;

namespace tessel {

class ToolOptionsBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void newDocument(int width, int height, Rgba background);
    bool openPath(const QString& path);
    void openPaths(const QStringList& paths);

    // The startup sequence: offer whatever a crashed run left behind, offer
    // the documents that were open last time, then open `paths`. Ends with at
    // least one document, so the window is never empty.
    void startSession(const QStringList& paths);
    // Opens a recovery copy as a new document: modified, pointing back at the
    // file it was made from and never at the recovery copy itself, so saving
    // it cannot overwrite the copy or pass unnoticed.
    bool openRecovered(const recovery::Entry& entry);
    // One autosave pass, start to finish. The timer does the same thing
    // without waiting; this waits, which is what the tests need.
    void autosaveNow();

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // ---------------------------------------------------------- construction
    void createActions();
    void createMenus();
    void createToolBars();
    void createMenuBarIcons();
    void createDocks();
    void createStatusBar();
    void installToolShortcuts();
    void restoreSettings();
    void storeSettings();
    void setupAutosave();
    // A stable number per view, so the autosaver can name its files without
    // knowing what a window or a view is.
    int recoveryKey(CanvasView* view);
    void dropRecoveryFile(CanvasView* view);
    recovery::OpenDocuments captureOpenDocuments() const;
    void offerRecovery();
    void offerLastSession();

    // ---------------------------------------------------------- documents
    CanvasView* currentView() const;
    Document* currentDocument() const;
    History* currentHistory() const;
    CanvasView* viewAt(int index) const;
    void addView(CanvasView* view);
    void setCurrentView(int index);
    bool closeView(int index);
    bool promptToSave(int index);
    bool saveView(int index, bool saveAs);
    void updateEverything();
    void updateWindowTitle();
    void updateImageStrip();
    void updateStatusBar();
    void updateActionStates();
    void updateZoomControls();
    void refreshPanels();
    void refreshIcons();
    void styleDockButtons(QDockWidget* dock);

    // ---------------------------------------------------------- commands
    void chooseTool(const QString& toolId, bool fromShortcut = false);
    void cycleToolShortcut(const QString& key, int direction = 1);
    void applyEffect(Effect* effect);
    // Adds an adjustment layer holding `effect` and opens its parameters.
    void addAdjustmentLayer(Effect* effect);
    // Reopens the parameters of the adjustment layer that is active.
    void editAdjustmentLayer();
    void repeatLastEffect();
    void doCopy(bool merged);
    void doCut();
    void doPaste(bool intoNewLayer, bool intoNewImage);
    void eraseSelection();
    void fillSelection();
    void cropToSelection();
    void resizeImage();
    void changeCanvasSize();
    void addLayer();
    void deleteLayer();
    void duplicateLayer();
    void mergeLayerDown();
    void importLayerFromFile();
    void placePicture(Image picture, const QString& name, bool onNewLayer);
    void moveLayer(int delta);
    void showLayerProperties();
    void flattenImage();
    void printImage();
    void copySelection();
    void pasteSelection();
    void setLayerVisible(int index, bool visible);
    void goToLayer(int index);
    void showSettings();
    QList<ShortcutEntry> collectShortcuts() const;
    // The unit the rulers, the status bar and the resize dialogs measure in.
    void setMeasurementUnit(Unit unit);
    void structuralEdit(const QString& name, const QString& icon,
                        const std::function<void()>& action);

    // ---------------------------------------------------------- state
    QStackedWidget* stack_ = nullptr;
    ImageStrip* strip_ = nullptr;
    ToolsPalette* tools_ = nullptr;
    ColorPanel* colors_ = nullptr;
    LayersPanel* layers_ = nullptr;
    HistoryPanel* history_ = nullptr;
    ToolOptionsBar* options_ = nullptr;
    QDockWidget* toolsDock_ = nullptr;
    QDockWidget* colorsDock_ = nullptr;
    QDockWidget* layersDock_ = nullptr;
    QDockWidget* historyDock_ = nullptr;
    QToolButton* toolsButton_ = nullptr;
    QToolButton* historyButton_ = nullptr;
    QToolButton* layersButton_ = nullptr;
    QToolButton* colorsButton_ = nullptr;
    QToolBar* mainToolBar_ = nullptr;
    QToolBar* optionsToolBar_ = nullptr;

    QLabel* hintLabel_ = nullptr;
    QLabel* positionLabel_ = nullptr;
    QLabel* selectionLabel_ = nullptr;
    QLabel* sizeLabel_ = nullptr;
    QComboBox* zoomCombo_ = nullptr;
    QSlider* zoomSlider_ = nullptr;

    QHash<QString, QAction*> actions_;
    QMenu* recentMenu_ = nullptr;
    QStringList recentFiles_;
    QString lastDirectory_;
    QString previousToolId_;
    QString currentToolId_;
    Effect* lastEffect_ = nullptr;
    ParamValues lastEffectValues_;
    QAction* repeatAction_ = nullptr;
    bool updatingZoom_ = false;
    int untitledCounter_ = 1;
    int defaultWidth_ = 800;
    int defaultHeight_ = 600;
    Unit unit_ = Unit::Pixels;
    std::unique_ptr<recovery::Autosaver> autosaver_;
    recovery::Options recoveryOptions_;
    QHash<CanvasView*, int> recoveryKeys_;
    int recoveryCounter_ = 0;
    // The open documents as they were before the close started, because
    // closeEvent empties the window before the settings are written.
    recovery::OpenDocuments lastSession_;
};

} // namespace tessel
