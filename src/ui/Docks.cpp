#include "ui/Docks.h"

#include <QComboBox>
#include <QDropEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include "core/ImageOps.h"
#include "tools/Tool.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace tessel {

// ----------------------------------------------------------------- tools palette

QStringList ToolsPalette::toolOrder()
{
    // Grid order, left to right then down, matching the familiar layout.
    return {
        QStringLiteral("select-rect"), QStringLiteral("move-selected"),
        QStringLiteral("lasso"),       QStringLiteral("move-selection"),
        QStringLiteral("select-ellipse"), QStringLiteral("zoom"),
        QStringLiteral("wand"),        QStringLiteral("pan"),
        QStringLiteral("bucket"),      QStringLiteral("gradient"),
        QStringLiteral("brush"),       QStringLiteral("eraser"),
        QStringLiteral("pencil"),      QStringLiteral("picker"),
        QStringLiteral("clone"),       QStringLiteral("recolor"),
        QStringLiteral("text"),        QStringLiteral("line"),
        QStringLiteral("shapes"),
    };
}

ToolsPalette::ToolsPalette(QWidget* parent) : QWidget(parent)
{
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setSpacing(2);
    const QStringList order = toolOrder();
    int row = 0, column = 0;
    for (const QString& id : order) {
        Tool* tool = ToolRegistry::instance().byId(id);
        if (!tool)
            continue;
        auto* button = new QToolButton(this);
        button->setIcon(icons::get(tool->iconName(), 22));
        button->setProperty("iconName", tool->iconName());
        button->setIconSize(QSize(22, 22));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFixedSize(30, 30);
        const QString keys = tool->shortcut();
        button->setToolTip(keys.isEmpty()
                               ? tool->name()
                               : QStringLiteral("%1 (%2)").arg(tool->name(), keys));
        connect(button, &QToolButton::clicked, this, [this, id] {
            setCurrentTool(id);
            emit toolSelected(id);
        });
        buttons_.insert(id, button);
        grid->addWidget(button, row, column);
        if (++column == 2) {
            column = 0;
            ++row;
        }
    }
    grid->setRowStretch(row + 1, 1);
    // Two columns of 30 px buttons plus the grid margins. A little slack on
    // top of that, so the dock title bar has room for its name beside the
    // float and close buttons instead of eliding it away.
    setMinimumWidth(72);
    setMaximumWidth(118);
}

void ToolsPalette::setCurrentTool(const QString& toolId)
{
    currentId_ = toolId;
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it)
        it.value()->setChecked(it.key() == toolId);
}

// ----------------------------------------------------------------- layer rows

namespace {
constexpr int kRowHeight = 40;
constexpr int kThumbWidth = 36;
} // namespace

QRect LayerRowDelegate::visibilityRect(const QRect& row)
{
    return QRect(row.left() + 4, row.top() + (row.height() - 18) / 2, 18, 18);
}

QRect LayerRowDelegate::thumbnailRect(const QRect& row)
{
    return QRect(row.left() + 26, row.top() + 3, kThumbWidth, row.height() - 6);
}

QRect LayerRowDelegate::maskRect(const QRect& row)
{
    return thumbnailRect(row).translated(kThumbWidth + 4, 0);
}

QSize LayerRowDelegate::sizeHint(const QStyleOptionViewItem&,
                                 const QModelIndex& index) const
{
    // A row with a mask carries a second thumbnail, so it needs more room.
    const bool hasMask = !index.data(MaskThumbnail).value<QImage>().isNull();
    return QSize(hasMask ? 190 : 140, kRowHeight);
}

void LayerRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    painter->save();
    const bool selected = option.state & QStyle::State_Selected;
    if (selected)
        painter->fillRect(option.rect, theme::accentColor());
    else if (option.state & QStyle::State_MouseOver)
        painter->fillRect(option.rect, theme::panelColor().lighter(112));

    const bool visible = index.data(Visible).toBool();
    const QImage thumbnail = index.data(Thumbnail).value<QImage>();
    const QString name = index.data(Qt::DisplayRole).toString();
    const QString detail = index.data(Detail).toString();
    const QImage maskThumbnail = index.data(MaskThumbnail).value<QImage>();
    const bool maskSelected = index.data(MaskSelected).toBool();
    const bool maskEnabled = index.data(MaskEnabled).toBool();

    const QColor textColor = selected ? Qt::white : theme::textColor();
    painter->drawPixmap(visibilityRect(option.rect).topLeft(),
                        icons::pixmap(visible ? QStringLiteral("visible")
                                              : QStringLiteral("hidden"),
                                      18, textColor));

    // The thumbnail the paint target points at is ringed, the way a Paint.NET
    // style editor shows whether a stroke lands in the layer or in its mask.
    auto drawThumbnail = [&](const QRect& box, const QImage& image, bool target,
                             bool dimmed) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(icons::checkerPixmap(4)));
        painter->drawRect(box);
        if (!image.isNull()) {
            const QSize fitted = image.size().scaled(box.size(), Qt::KeepAspectRatio);
            const QRect area(box.x() + (box.width() - fitted.width()) / 2,
                             box.y() + (box.height() - fitted.height()) / 2,
                             fitted.width(), fitted.height());
            if (dimmed)
                painter->setOpacity(0.4);
            painter->drawImage(area, image);
            painter->setOpacity(1.0);
        }
        painter->setBrush(Qt::NoBrush);
        if (target) {
            painter->setPen(QPen(selected ? QColor(Qt::white) : theme::accentColor(), 2));
            painter->drawRect(box.adjusted(-1, -1, 1, 1));
        }
        painter->setPen(QPen(theme::gridColor(), 1));
        painter->drawRect(box);
    };

    const QRect box = thumbnailRect(option.rect);
    const bool hasMask = !maskThumbnail.isNull();
    drawThumbnail(box, thumbnail, hasMask && !maskSelected, false);
    int textLeft = box.right();
    if (hasMask) {
        const QRect maskBox = maskRect(option.rect);
        drawThumbnail(maskBox, maskThumbnail, maskSelected, !maskEnabled);
        textLeft = maskBox.right();
    }

    QRect textArea = option.rect.adjusted(textLeft - option.rect.left() + 8, 3, -4, -3);
    painter->setPen(textColor);
    QFont font = option.font;
    painter->setFont(font);
    painter->drawText(QRect(textArea.x(), textArea.y(), textArea.width(), 18),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(name, Qt::ElideRight,
                                                        textArea.width()));
    QColor dim = textColor;
    dim.setAlpha(selected ? 210 : 150);
    painter->setPen(dim);
    font.setPointSizeF(std::max(6.5, font.pointSizeF() - 1.5));
    painter->setFont(font);
    painter->drawText(QRect(textArea.x(), textArea.y() + 17, textArea.width(), 16),
                      Qt::AlignLeft | Qt::AlignVCenter, detail);
    painter->restore();
}

// ----------------------------------------------------------------- layers panel

// ----------------------------------------------------------------- layer tree

LayerTree::LayerTree(QWidget* parent) : QTreeWidget(parent)
{
    setHeaderHidden(true);
    setColumnCount(1);
    setRootIsDecorated(true);
    setExpandsOnDoubleClick(false);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setMouseTracking(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setIndentation(14);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
}

void LayerTree::dropEvent(QDropEvent* event)
{
    QTreeWidgetItem* dragged = selectedItems().isEmpty() ? currentItem()
                                                         : selectedItems().first();
    const QPoint pos = event->position().toPoint();
    QTreeWidgetItem* onto = itemAt(pos);
    Drop where = Drop::Above;
    switch (dropIndicatorPosition()) {
    case OnItem: where = Drop::Into; break;
    case BelowItem: where = Drop::Below; break;
    case AboveItem: where = Drop::Above; break;
    case OnViewport: onto = nullptr; break;
    }
    // The document owns the layer order, so the move is only requested here.
    // The panel is rebuilt from the document once the move has gone through.
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
    if (dragged && dragged != onto)
        emit layerDropped(dragged, onto, where);
}

// ----------------------------------------------------------------- layers panel

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    list_ = new LayerTree(this);
    list_->setItemDelegate(new LayerRowDelegate(list_));
    root->addWidget(list_, 1);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    struct ButtonSpec {
        const char* icon;
        const char* tip;
        void (LayersPanel::*signal)();
    };
    const ButtonSpec specs[] = {
        {"layer-add", "Add New Layer (Ctrl+Shift+N)", &LayersPanel::addLayerRequested},
        {"layer", "Add Group (Ctrl+Shift+G)", &LayersPanel::addGroupRequested},
        {"layer-delete", "Delete Layer (Ctrl+Shift+Delete)", &LayersPanel::deleteLayerRequested},
        {"layer-duplicate", "Duplicate Layer (Ctrl+Shift+D)", &LayersPanel::duplicateLayerRequested},
        {"layer-merge", "Merge Layer Down (Ctrl+M)", &LayersPanel::mergeDownRequested},
        {"up", "Move Layer Up", &LayersPanel::moveUpRequested},
        {"down", "Move Layer Down", &LayersPanel::moveDownRequested},
        {"layer-props", "Layer Properties (F4)", &LayersPanel::propertiesRequested},
    };
    for (const ButtonSpec& spec : specs) {
        auto* button = new QToolButton(this);
        button->setIcon(icons::get(QString::fromLatin1(spec.icon), 18));
        button->setProperty("iconName", QString::fromLatin1(spec.icon));
        button->setIconSize(QSize(18, 18));
        button->setToolTip(QString::fromLatin1(spec.tip));
        button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, this, spec.signal);
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    root->addLayout(buttons);

    connect(list_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (updating_ || !current)
                    return;
                const int index = layerIndexOfItem(current);
                if (index >= 0)
                    emit activeLayerChanged(index);
            });
    connect(list_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem*, int) { emit propertiesRequested(); });
    // Expanding and collapsing is remembered on the layer itself, so it
    // survives a rebuild of the panel.
    auto rememberExpanded = [this](QTreeWidgetItem* item, bool expanded) {
        if (updating_ || !doc_ || !item)
            return;
        const int uid = item->data(0, LayerRowDelegate::LayerUid).toInt();
        if (Layer* layer = doc_->layerByUid(uid))
            layer->expanded = expanded;
    };
    connect(list_, &QTreeWidget::itemExpanded, this,
            [rememberExpanded](QTreeWidgetItem* item) { rememberExpanded(item, true); });
    connect(list_, &QTreeWidget::itemCollapsed, this,
            [rememberExpanded](QTreeWidgetItem* item) { rememberExpanded(item, false); });
    connect(list_, &LayerTree::layerDropped, this,
            [this](QTreeWidgetItem* dragged, QTreeWidgetItem* onto,
                   LayerTree::Drop where) {
                if (!doc_ || !dragged)
                    return;
                const int uid = dragged->data(0, LayerRowDelegate::LayerUid).toInt();
                if (!doc_->layerByUid(uid))
                    return;
                const Layer* parent = doc_->rootGroup();
                // Panel rows run top first, the document counts from the
                // bottom, so the row is turned round at the end.
                int rowFromTop = list_->topLevelItemCount();
                Layer* target = onto
                    ? doc_->layerByUid(onto->data(0, LayerRowDelegate::LayerUid).toInt())
                    : nullptr;
                if (target) {
                    if (where == LayerTree::Drop::Into && target->isGroup()) {
                        parent = target;
                        rowFromTop = 0;
                    } else {
                        parent = target->parent() ? target->parent() : doc_->rootGroup();
                        rowFromTop = parent->childCount() - 1 - target->indexInParent();
                        if (where == LayerTree::Drop::Below)
                            ++rowFromTop;
                    }
                }
                const int childIndex = parent->childCount() - rowFromTop;
                emit layerReparented(uid,
                                     parent == doc_->rootGroup() ? 0 : parent->uid(),
                                     childIndex);
            });
    // Clicking the eye toggles visibility without changing the selection, and
    // clicking either thumbnail picks what painting lands in.
    list_->viewport()->installEventFilter(this);
}

bool LayersPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == list_->viewport() && event->type() == QEvent::MouseButtonPress
        && doc_) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint pos = mouse->position().toPoint();
        const QModelIndex index = list_->indexAt(pos);
        Layer* layer = index.isValid()
            ? doc_->layerByUid(index.data(LayerRowDelegate::LayerUid).toInt())
            : nullptr;
        if (layer) {
            const QRect row = list_->visualRect(index);
            const int layerIndex = doc_->indexOfUid(layer->uid());
            if (LayerRowDelegate::visibilityRect(row).contains(pos)) {
                emit layerVisibilityToggled(layerIndex, !layer->visible);
                return true;
            }
            const bool onMask = layer->hasMask()
                && LayerRowDelegate::maskRect(row).contains(pos);
            const bool onThumbnail =
                onMask || LayerRowDelegate::thumbnailRect(row).contains(pos);
            // A click that would change nothing is left alone, so a drag can
            // still start on the thumbnail of the layer already being painted.
            if (onThumbnail
                && (layer->maskSelected() != onMask
                    || layerIndex != doc_->activeIndex())) {
                layer->setMaskSelected(onMask);
                if (layerIndex >= 0)
                    emit activeLayerChanged(layerIndex);
                refresh();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

int LayersPanel::layerIndexOfItem(const QTreeWidgetItem* item) const
{
    if (!doc_ || !item)
        return -1;
    return doc_->indexOfUid(item->data(0, LayerRowDelegate::LayerUid).toInt());
}

QTreeWidgetItem* LayersPanel::itemForUid(int uid) const
{
    for (QTreeWidgetItemIterator it(list_); *it; ++it) {
        if ((*it)->data(0, LayerRowDelegate::LayerUid).toInt() == uid)
            return *it;
    }
    return nullptr;
}

void LayersPanel::setDocument(Document* doc, History* history)
{
    doc_ = doc;
    history_ = history;
    refresh();
}

void LayersPanel::appendRows(QTreeWidgetItem* parentItem, const Layer* parent)
{
    // Top layer first, as in the layer stack itself.
    for (int i = parent->childCount() - 1; i >= 0; --i) {
        const Layer* layer = parent->childAt(i);
        auto* item = parentItem ? new QTreeWidgetItem(parentItem)
                                : new QTreeWidgetItem(list_);
        item->setText(0, layer->name);
        item->setData(0, LayerRowDelegate::LayerUid, layer->uid());
        item->setData(0, LayerRowDelegate::Visible, layer->visible);
        item->setData(0, LayerRowDelegate::IsGroup, layer->isGroup());
        // A group shows what it composites to, which is the only picture of
        // itself it has.
        const Image source = layer->isGroup() ? doc_->renderLayer(*layer) : layer->image;
        item->setData(0, LayerRowDelegate::Thumbnail,
                      QVariant::fromValue(ops::resize(source, 36, 36, true).toQImage()));
        const int percent = int(std::lround(layer->opacity / 2.55));
        QString detail = QStringLiteral("%1%  %2").arg(percent).arg(
            blendModeName(layer->mode));
        if (layer->isGroup())
            detail = QStringLiteral("group  %1").arg(detail);
        item->setData(0, LayerRowDelegate::Detail, detail);
        QString tip = QStringLiteral("%1\nOpacity %2%\nBlend mode: %3")
                          .arg(layer->name)
                          .arg(percent)
                          .arg(blendModeName(layer->mode));
        if (layer->hasMask()) {
            item->setData(0, LayerRowDelegate::MaskThumbnail,
                          QVariant::fromValue(
                              ops::resize(*layer->maskImage(), 36, 36, true).toQImage()));
            item->setData(0, LayerRowDelegate::MaskSelected, layer->maskSelected());
            item->setData(0, LayerRowDelegate::MaskEnabled, layer->maskEnabled());
            tip += layer->maskEnabled() ? QStringLiteral("\nMask on")
                                        : QStringLiteral("\nMask off");
            tip += layer->maskSelected() ? QStringLiteral("\nPainting goes to the mask")
                                         : QStringLiteral("\nPainting goes to the layer");
        }
        item->setToolTip(0, tip);
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable
            | Qt::ItemIsDragEnabled;
        if (layer->isGroup())
            flags |= Qt::ItemIsDropEnabled;
        item->setFlags(flags);
        if (layer->isGroup()) {
            appendRows(item, layer);
            item->setExpanded(layer->expanded);
        }
    }
}

void LayersPanel::refresh()
{
    updating_ = true;
    list_->clear();
    if (doc_) {
        appendRows(nullptr, doc_->rootGroup());
        const Layer* active = doc_->activeNode();
        if (QTreeWidgetItem* item = active ? itemForUid(active->uid()) : nullptr)
            list_->setCurrentItem(item);
    }
    updating_ = false;
}

// ----------------------------------------------------------------- history panel

HistoryPanel::HistoryPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    undoButton_ = new QToolButton(this);
    undoButton_->setIcon(icons::get(QStringLiteral("undo"), 18));
    undoButton_->setProperty("iconName", QStringLiteral("undo"));
    undoButton_->setIconSize(QSize(18, 18));
    undoButton_->setToolTip(QStringLiteral("Undo (Ctrl+Z)"));
    undoButton_->setAutoRaise(true);
    redoButton_ = new QToolButton(this);
    redoButton_->setIcon(icons::get(QStringLiteral("redo"), 18));
    redoButton_->setProperty("iconName", QStringLiteral("redo"));
    redoButton_->setIconSize(QSize(18, 18));
    redoButton_->setToolTip(QStringLiteral("Redo (Ctrl+Y)"));
    redoButton_->setAutoRaise(true);
    buttons->addWidget(undoButton_);
    buttons->addWidget(redoButton_);
    info_ = new QLabel(this);
    info_->setStyleSheet(QStringLiteral("color: %1; font-size: 8pt;")
                             .arg(theme::textColor().darker(140).name()));
    buttons->addWidget(info_, 1);
    root->addLayout(buttons);

    list_ = new QListWidget(this);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setIconSize(QSize(16, 16));
    root->addWidget(list_, 1);

    connect(undoButton_, &QToolButton::clicked, this, &HistoryPanel::undoRequested);
    connect(redoButton_, &QToolButton::clicked, this, &HistoryPanel::redoRequested);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (updating_ || row < 0)
            return;
        emit jumpRequested(row - 1); // row 0 is the starting state
    });
}

void HistoryPanel::setDocument(Document*, History* history)
{
    history_ = history;
    refresh();
}

void HistoryPanel::refresh()
{
    updating_ = true;
    list_->clear();
    auto* start = new QListWidgetItem(icons::get(QStringLiteral("image"), 16),
                                      QStringLiteral("Original image"), list_);
    start->setToolTip(QStringLiteral("The image as it was opened or created"));
    if (history_) {
        for (int i = 0; i < history_->count(); ++i) {
            const HistoryItem* item = history_->itemAt(i);
            auto* row = new QListWidgetItem(icons::get(item->icon(), 16), item->name(),
                                            list_);
            row->setToolTip(item->name());
        }
        list_->setCurrentRow(history_->currentIndex() + 1);
        undoButton_->setEnabled(history_->canUndo());
        redoButton_->setEnabled(history_->canRedo());
        // Grey out the entries that have been undone.
        for (int i = history_->currentIndex() + 2; i < list_->count(); ++i) {
            QColor dim = theme::textColor();
            dim.setAlpha(110);
            list_->item(i)->setForeground(dim);
        }
        const double megabytes = history_->byteCount() / (1024.0 * 1024.0);
        info_->setText(QStringLiteral("%1 steps  -  %2 MB")
                           .arg(history_->count())
                           .arg(megabytes, 0, 'f', 1));
    }
    updating_ = false;
}

// ----------------------------------------------------------------- image strip

namespace {
constexpr int kStripThumb = 46;
constexpr int kStripPadding = 4;
constexpr int kStripWidth = 62;
} // namespace

ImageStrip::ImageStrip(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFixedHeight(kStripThumb + 2 * kStripPadding);
}

QSize ImageStrip::sizeHint() const
{
    return QSize(std::max<int>(1, int(entries_.size())) * kStripWidth,
                 kStripThumb + 2 * kStripPadding);
}

void ImageStrip::setEntries(const QVector<Entry>& entries, int current)
{
    entries_ = entries;
    current_ = current;
    updateGeometry();
    update();
}

QRect ImageStrip::entryRect(int index) const
{
    return QRect(kStripPadding + index * kStripWidth, kStripPadding, kStripThumb,
                 kStripThumb);
}

QRect ImageStrip::closeRect(int index) const
{
    const QRect box = entryRect(index);
    return QRect(box.right() - 11, box.top() - 1, 13, 13);
}

int ImageStrip::entryAt(QPoint pos) const
{
    for (int i = 0; i < entries_.size(); ++i) {
        if (entryRect(i).adjusted(-2, -2, 2, 2).contains(pos))
            return i;
    }
    return -1;
}

void ImageStrip::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < entries_.size(); ++i) {
        const QRect box = entryRect(i);
        const bool active = i == current_;
        painter.setPen(Qt::NoPen);
        painter.setBrush(active ? theme::accentColor() : theme::panelColor());
        painter.drawRoundedRect(box.adjusted(-2, -2, 2, 2), 4, 4);
        painter.setBrush(QBrush(icons::checkerPixmap(4)));
        painter.drawRect(box);
        const QImage& thumb = entries_[i].thumbnail;
        if (!thumb.isNull()) {
            const QSize fitted = thumb.size().scaled(box.size(), Qt::KeepAspectRatio);
            painter.drawImage(QRect(box.x() + (box.width() - fitted.width()) / 2,
                                    box.y() + (box.height() - fitted.height()) / 2,
                                    fitted.width(), fitted.height()),
                              thumb);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme::gridColor(), 1));
        painter.drawRect(box);
        if (entries_[i].modified) {
            // An unsaved image gets a dot in the corner.
            painter.setBrush(QColor(0xff, 0xc9, 0x3c));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QPointF(box.left() + 5, box.top() + 5), 3.2, 3.2);
        }
        if (i == hovered_) {
            painter.setBrush(theme::panelColor());
            painter.setPen(QPen(theme::gridColor(), 1));
            const QRect close = closeRect(i);
            painter.drawEllipse(close);
            painter.drawPixmap(close.topLeft() + QPoint(1, 1),
                               icons::pixmap(QStringLiteral("close"), 11,
                                             theme::textColor()));
        }
    }
}

void ImageStrip::mousePressEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    const int index = entryAt(pos);
    if (index < 0)
        return;
    if (closeRect(index).contains(pos) || event->button() == Qt::MiddleButton) {
        emit closeRequested(index);
        return;
    }
    emit imageActivated(index);
}

void ImageStrip::mouseMoveEvent(QMouseEvent* event)
{
    const int index = entryAt(event->position().toPoint());
    if (index != hovered_) {
        hovered_ = index;
        setToolTip(index >= 0 ? entries_[index].tooltip : QString());
        update();
    }
}

void ImageStrip::leaveEvent(QEvent*)
{
    hovered_ = -1;
    update();
}

} // namespace tessel
