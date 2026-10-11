#pragma once

#include <QListWidget>
#include <QStyledItemDelegate>
#include <QTreeWidget>
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

// One row of the layers tree: visibility box, thumbnail, mask thumbnail, name.
class LayerRowDelegate : public QStyledItemDelegate {
public:
    explicit LayerRowDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

    static QRect visibilityRect(const QRect& row);
    static QRect thumbnailRect(const QRect& row);
    // Only meaningful for a row whose layer carries a mask.
    static QRect maskRect(const QRect& row);

    // Roles the panel fills in for each row.
    enum Role {
        LayerUid = Qt::UserRole,
        Visible,
        Thumbnail,
        Detail,
        MaskThumbnail,
        MaskSelected,
        MaskEnabled,
        IsGroup,
    };
};

// The layers tree, which only exists to turn a drop into a request the panel
// can hand to the document.
class LayerTree : public QTreeWidget {
    Q_OBJECT

public:
    explicit LayerTree(QWidget* parent = nullptr);

    // Qt keeps its own drop indicator enum protected, so the signal carries
    // this one instead.
    enum class Drop { Into, Above, Below };
    Q_ENUM(Drop)

signals:
    // The dragged layer, the row it was dropped on (null for the empty area
    // below the tree) and where it landed relative to that row.
    void layerDropped(QTreeWidgetItem* dragged, QTreeWidgetItem* onto,
                      LayerTree::Drop where);

protected:
    void dropEvent(QDropEvent* event) override;
};

class LayersPanel : public QWidget {
    Q_OBJECT

public:
    explicit LayersPanel(QWidget* parent = nullptr);

    void setDocument(Document* doc, History* history);
    void refresh();

    // Asks for the room the rows actually need, so the dock around it can stop
    // where the list stops instead of filling the column.
    QSize sizeHint() const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void addLayerRequested();
    void addGroupRequested();
    void deleteLayerRequested();
    void duplicateLayerRequested();
    void mergeDownRequested();
    void propertiesRequested();
    // Double clicking an adjustment layer opens its parameters rather than the
    // layer properties, because the parameters are what it is.
    void editAdjustmentRequested();
    void moveUpRequested();
    void moveDownRequested();
    void activeLayerChanged(int index);
    void layerVisibilityToggled(int index, bool visible);
    // Dragged into or out of a group: the layer, the group it belongs to now
    // (0 for the top level) and its position among that group's children,
    // counted from the bottom as the document counts them.
    void layerReparented(int uid, int parentUid, int childIndex);
    void documentEdited();

private:
    void appendRows(QTreeWidgetItem* parentItem, const Layer* parent);
    QTreeWidgetItem* itemForUid(int uid) const;
    int layerIndexOfItem(const QTreeWidgetItem* item) const;

    LayerTree* list_ = nullptr;
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

    QSize sizeHint() const override;

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
