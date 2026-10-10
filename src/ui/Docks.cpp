#include "ui/Docks.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
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
    setMinimumWidth(72);
    setMaximumWidth(96);
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

QSize LayerRowDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const
{
    return QSize(140, kRowHeight);
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

    const bool visible = index.data(Qt::UserRole + 1).toBool();
    const QImage thumbnail = index.data(Qt::UserRole + 2).value<QImage>();
    const QString name = index.data(Qt::DisplayRole).toString();
    const QString detail = index.data(Qt::UserRole + 3).toString();

    const QColor textColor = selected ? Qt::white : theme::textColor();
    painter->drawPixmap(visibilityRect(option.rect).topLeft(),
                        icons::pixmap(visible ? QStringLiteral("visible")
                                              : QStringLiteral("hidden"),
                                      18, textColor));

    const QRect box = thumbnailRect(option.rect);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QBrush(icons::checkerPixmap(4)));
    painter->drawRect(box);
    if (!thumbnail.isNull()) {
        const QSize fitted = thumbnail.size().scaled(box.size(), Qt::KeepAspectRatio);
        const QRect target(box.x() + (box.width() - fitted.width()) / 2,
                           box.y() + (box.height() - fitted.height()) / 2,
                           fitted.width(), fitted.height());
        painter->drawImage(target, thumbnail);
    }
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(theme::gridColor(), 1));
    painter->drawRect(box);

    QRect textArea = option.rect.adjusted(kThumbWidth + 34, 3, -4, -3);
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

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    list_ = new QListWidget(this);
    list_->setItemDelegate(new LayerRowDelegate(list_));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMouseTracking(true);
    list_->setUniformItemSizes(true);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
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

    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (updating_ || row < 0)
            return;
        emit activeLayerChanged(listRowToLayerIndex(row));
    });
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { emit propertiesRequested(); });
    // Clicking the eye toggles visibility without changing the selection.
    list_->viewport()->installEventFilter(this);
}

bool LayersPanel::eventFilter(QObject* watched, QEvent* event)
{
    // A click on the eye toggles that layer without selecting it.
    if (watched == list_->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint pos = mouse->position().toPoint();
        const QModelIndex index = list_->indexAt(pos);
        if (index.isValid()) {
            const QRect row = list_->visualRect(index);
            if (LayerRowDelegate::visibilityRect(row).contains(pos)) {
                const int layerIndex = listRowToLayerIndex(index.row());
                const bool visible = index.data(Qt::UserRole + 1).toBool();
                emit layerVisibilityToggled(layerIndex, !visible);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

int LayersPanel::listRowToLayerIndex(int row) const
{
    if (!doc_)
        return 0;
    return doc_->layerCount() - 1 - row;
}

void LayersPanel::setDocument(Document* doc, History* history)
{
    doc_ = doc;
    history_ = history;
    refresh();
}

void LayersPanel::refresh()
{
    updating_ = true;
    list_->clear();
    if (doc_) {
        // Top layer first, as in the layer stack itself.
        for (int i = doc_->layerCount() - 1; i >= 0; --i) {
            const Layer* layer = doc_->layerAt(i);
            auto* item = new QListWidgetItem(layer->name, list_);
            item->setData(Qt::UserRole, i);
            item->setData(Qt::UserRole + 1, layer->visible);
            const Image thumb = ops::resize(layer->image, 36, 36, true);
            item->setData(Qt::UserRole + 2, QVariant::fromValue(thumb.toQImage()));
            QString detail = QStringLiteral("%1%  %2")
                                 .arg(int(std::lround(layer->opacity / 2.55)))
                                 .arg(blendModeName(layer->mode));
            item->setData(Qt::UserRole + 3, detail);
            item->setToolTip(QStringLiteral("%1\nOpacity %2%\nBlend mode: %3")
                                 .arg(layer->name)
                                 .arg(int(std::lround(layer->opacity / 2.55)))
                                 .arg(blendModeName(layer->mode)));
        }
        const int row = doc_->layerCount() - 1 - doc_->activeIndex();
        list_->setCurrentRow(std::clamp(row, 0, std::max(0, list_->count() - 1)));
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
