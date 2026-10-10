#pragma once

#include <memory>
#include <vector>

#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include "core/Document.h"
#include "core/Units.h"
#include "effects/Effect.h"
#include "ui/CanvasView.h"
#include "ui/ColorPanel.h"
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

    // ---------------------------------------------------------- commands
    void chooseTool(const QString& toolId, bool fromShortcut = false);
    void cycleToolShortcut(const QString& key, int direction = 1);
    void applyEffect(Effect* effect);
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
    void moveLayer(int delta);
    void showLayerProperties();
    void flattenImage();
    void printImage();
    void copySelection();
    void pasteSelection();
    void setLayerVisible(int index, bool visible);
    void goToLayer(int index);
    void showSettings();
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
};

} // namespace tessel
