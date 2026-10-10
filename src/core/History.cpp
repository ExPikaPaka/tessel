#include "core/History.h"

namespace tessel {

// ----------------------------------------------------------------- items

PixelEdit::PixelEdit(QString name, QString icon, int layerUid, Rect rect,
                     Image before, Image after)
    : HistoryItem(std::move(name), std::move(icon)), layerUid_(layerUid),
      rect_(rect), before_(std::move(before)), after_(std::move(after))
{
}

void PixelEdit::write(Document& doc, const Image& patch)
{
    Layer* layer = doc.layerByUid(layerUid_);
    if (!layer)
        return;
    layer->image.blit(patch, rect_.x, rect_.y);
    doc.invalidateLayer(layer);
}

void PixelEdit::undo(Document& doc) { write(doc, before_); }
void PixelEdit::redo(Document& doc) { write(doc, after_); }

size_t PixelEdit::byteCount() const
{
    return before_.byteCount() + after_.byteCount();
}

SelectionEdit::SelectionEdit(QString name, bool hadBefore, Mask before,
                             bool hasAfter, Mask after)
    : HistoryItem(std::move(name), QStringLiteral("select")),
      hadBefore_(hadBefore), before_(std::move(before)), hasAfter_(hasAfter),
      after_(std::move(after))
{
}

void SelectionEdit::write(Document& doc, bool active, const Mask& mask)
{
    if (active)
        doc.selection().setMask(mask);
    else
        doc.selection().clear();
    doc.invalidate();
}

void SelectionEdit::undo(Document& doc) { write(doc, hadBefore_, before_); }
void SelectionEdit::redo(Document& doc) { write(doc, hasAfter_, after_); }

size_t SelectionEdit::byteCount() const
{
    return before_.byteCount() + after_.byteCount();
}

DocumentEdit::DocumentEdit(QString name, QString icon, DocumentState before,
                           DocumentState after)
    : HistoryItem(std::move(name), std::move(icon)), before_(std::move(before)),
      after_(std::move(after))
{
}

void DocumentEdit::undo(Document& doc) { doc.restore(before_); }
void DocumentEdit::redo(Document& doc) { doc.restore(after_); }

size_t DocumentEdit::byteCount() const
{
    return before_.byteCount() + after_.byteCount();
}

// ----------------------------------------------------------------- History

void History::push(std::unique_ptr<HistoryItem> item)
{
    if (!item)
        return;
    items_.erase(items_.begin() + (index_ + 1), items_.end());
    if (savePoint_ > index_)
        savePoint_ = -2; // the saved state is no longer reachable
    items_.push_back(std::move(item));
    index_ = static_cast<int>(items_.size()) - 1;
    trim();
    if (doc_)
        doc_->setModified(index_ != savePoint_);
    notify();
}

void History::trim()
{
    while (static_cast<int>(items_.size()) > maxItems_ && !items_.empty()) {
        items_.erase(items_.begin());
        --index_;
        --savePoint_;
    }
    size_t total = byteCount();
    while (total > maxBytes_ && items_.size() > 1) {
        total -= items_.front()->byteCount();
        items_.erase(items_.begin());
        --index_;
        --savePoint_;
    }
    index_ = std::max(index_, -1);
}

void History::notify()
{
    if (changed_)
        changed_();
}

bool History::undo()
{
    if (!canUndo())
        return false;
    items_[static_cast<size_t>(index_)]->undo(*doc_);
    --index_;
    doc_->setModified(index_ != savePoint_);
    notify();
    return true;
}

bool History::redo()
{
    if (!canRedo())
        return false;
    ++index_;
    items_[static_cast<size_t>(index_)]->redo(*doc_);
    doc_->setModified(index_ != savePoint_);
    notify();
    return true;
}

void History::goTo(int target)
{
    target = std::clamp(target, -1, static_cast<int>(items_.size()) - 1);
    while (index_ > target) {
        items_[static_cast<size_t>(index_)]->undo(*doc_);
        --index_;
    }
    while (index_ < target) {
        ++index_;
        items_[static_cast<size_t>(index_)]->redo(*doc_);
    }
    doc_->setModified(index_ != savePoint_);
    notify();
}

void History::clear()
{
    items_.clear();
    index_ = -1;
    savePoint_ = -1;
    notify();
}

QString History::undoName() const
{
    return canUndo() ? items_[static_cast<size_t>(index_)]->name() : QString();
}

QString History::redoName() const
{
    return canRedo() ? items_[static_cast<size_t>(index_ + 1)]->name() : QString();
}

const HistoryItem* History::itemAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(items_.size()))
        return nullptr;
    return items_[static_cast<size_t>(index)].get();
}

size_t History::byteCount() const
{
    size_t total = 0;
    for (const auto& item : items_)
        total += item->byteCount();
    return total;
}

void History::markSaved()
{
    savePoint_ = index_;
    if (doc_)
        doc_->setModified(false);
    notify();
}

// ----------------------------------------------------------------- scopes

Rect changedRect(const Image& before, const Image& after)
{
    if (before.width() != after.width() || before.height() != after.height())
        return after.rect();
    int minX = after.width(), minY = after.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < after.height(); ++y) {
        const Rgba* a = before.row(y);
        const Rgba* b = after.row(y);
        int first = -1, last = -1;
        for (int x = 0; x < after.width(); ++x) {
            if (a[x] != b[x]) {
                if (first < 0)
                    first = x;
                last = x;
            }
        }
        if (first >= 0) {
            minX = std::min(minX, first);
            maxX = std::max(maxX, last);
            minY = std::min(minY, y);
            maxY = y;
        }
    }
    if (maxX < 0)
        return Rect();
    return Rect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

EditScope::EditScope(Document& doc, History& history, Layer* layer, QString name,
                     QString icon, Rect rect)
    : doc_(doc), history_(history), layer_(layer), name_(std::move(name)),
      icon_(std::move(icon)), rect_(rect), wholeLayer_(rect.isEmpty())
{
    if (!layer_)
        return;
    if (wholeLayer_) {
        before_ = layer_->image;
    } else {
        rect_ = rect_.intersected(layer_->image.rect());
        before_ = layer_->image.cropped(rect_);
    }
}

EditScope::~EditScope()
{
    if (!done_)
        commit();
}

void EditScope::cancel()
{
    done_ = true;
}

bool EditScope::commit()
{
    if (done_ || !layer_)
        return false;
    done_ = true;
    Rect area = rect_;
    if (wholeLayer_) {
        area = changedRect(before_, layer_->image);
        if (area.isEmpty())
            return false;
        Image before = before_.cropped(area);
        Image after = layer_->image.cropped(area);
        history_.push(std::make_unique<PixelEdit>(name_, icon_, layer_->uid(),
                                                  area, std::move(before),
                                                  std::move(after)));
    } else {
        if (area.isEmpty())
            return false;
        Image after = layer_->image.cropped(area);
        bool identical = true;
        for (size_t i = 0; i < after.pixelCount() && identical; ++i)
            identical = before_.data()[i] == after.data()[i];
        if (identical)
            return false;
        history_.push(std::make_unique<PixelEdit>(name_, icon_, layer_->uid(),
                                                  area, before_,
                                                  std::move(after)));
    }
    doc_.invalidateLayer(layer_);
    return true;
}

void recordDocumentEdit(Document& doc, History& history, const QString& name,
                        const QString& icon, const std::function<void()>& action)
{
    DocumentState before = doc.snapshot();
    action();
    DocumentState after = doc.snapshot();
    history.push(std::make_unique<DocumentEdit>(name, icon, std::move(before),
                                                std::move(after)));
}

void recordSelectionEdit(Document& doc, History& history, const QString& name,
                         const std::function<void()>& action)
{
    const Mask* beforeMask = doc.selection().clipMask();
    const bool hadBefore = beforeMask != nullptr;
    Mask before = hadBefore ? *beforeMask : Mask();
    action();
    const Mask* afterMask = doc.selection().clipMask();
    const bool hasAfter = afterMask != nullptr;
    Mask after = hasAfter ? *afterMask : Mask();
    history.push(std::make_unique<SelectionEdit>(name, hadBefore, std::move(before),
                                                 hasAfter, std::move(after)));
}

} // namespace tessel
