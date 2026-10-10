#pragma once

#include <QListWidget>
#include <QStyledItemDelegate>
#include <QVector>
#include <QWidget>

#include "core/Document.h"
#include "core/History.h"

class QGridLayout;
class QLabel;
class QScrollArea;
class QSlider;
class QToolButton;
class QComboBox;

namespace tessel {

class Tool;

// The tool palette: a two column grid of tool buttons, in the usual order.
class ToolsPalette : public QWidget {
    Q_OBJECT

public:
    explicit ToolsPalette(QWidget* parent = nullptr);

    void setCurrentTool(const QString& toolId);
    QString currentTool() const { return currentId_; }
    static QStringList toolOrder();

signals:
    void toolSelected(const QString& toolId);

private:
    QHash<QString, QToolButton*> buttons_;
    QString currentId_;
};

// One row of the layers list: visibility box, thumbnail, name.
class LayerRowDelegate : public QStyledItemDelegate {
public:
    explicit LayerRowDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

    static QRect visibilityRect(const QRect& row);
    static QRect thumbnailRect(const QRect& row);
};

class LayersPanel : public QWidget {
    Q_OBJECT

public:
    explicit LayersPanel(QWidget* parent = nullptr);

    void setDocument(Document* doc, History* history);
    void refresh();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void addLayerRequested();
    void deleteLayerRequested();
    void duplicateLayerRequested();
    void mergeDownRequested();
    void propertiesRequested();
    void moveUpRequested();
    void moveDownRequested();
    void activeLayerChanged(int index);
    void layerVisibilityToggled(int index, bool visible);
    void documentEdited();

private:
    int listRowToLayerIndex(int row) const;

    QListWidget* list_ = nullptr;
    Document* doc_ = nullptr;
    History* history_ = nullptr;
    bool updating_ = false;
};

class HistoryPanel : public QWidget {
    Q_OBJECT

public:
    explicit HistoryPanel(QWidget* parent = nullptr);

    void setDocument(Document* doc, History* history);
    void refresh();

signals:
    void undoRequested();
    void redoRequested();
    void jumpRequested(int index);

private:
    QListWidget* list_ = nullptr;
    QToolButton* undoButton_ = nullptr;
    QToolButton* redoButton_ = nullptr;
    QLabel* info_ = nullptr;
    History* history_ = nullptr;
    bool updating_ = false;
};

// The strip of open image thumbnails that sits at the top right.
class ImageStrip : public QWidget {
    Q_OBJECT

public:
    explicit ImageStrip(QWidget* parent = nullptr);

    struct Entry {
        QString title;
        QString tooltip;
        QImage thumbnail;
        bool modified = false;
    };

    void setEntries(const QVector<Entry>& entries, int current);
    int currentIndex() const { return current_; }

signals:
    void imageActivated(int index);
    void closeRequested(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    QSize sizeHint() const override;

private:
    QRect entryRect(int index) const;
    QRect closeRect(int index) const;
    int entryAt(QPoint pos) const;

    QVector<Entry> entries_;
    int current_ = -1;
    int hovered_ = -1;
};

} // namespace tessel
