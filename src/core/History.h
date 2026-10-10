#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QString>

#include "core/Document.h"

namespace tessel {

// One reversible step. Pixel steps keep only the changed rectangle, structural
// steps keep whole document snapshots.
class HistoryItem {
public:
    HistoryItem(QString name, QString icon) : name_(std::move(name)), icon_(std::move(icon)) {}
    virtual ~HistoryItem() = default;

    virtual void undo(Document& doc) = 0;
    virtual void redo(Document& doc) = 0;
    virtual size_t byteCount() const { return 0; }

    const QString& name() const { return name_; }
    const QString& icon() const { return icon_; }

private:
    QString name_;
    QString icon_;
};

class PixelEdit : public HistoryItem {
public:
    PixelEdit(QString name, QString icon, int layerUid, Rect rect, Image before,
              Image after);

    void undo(Document& doc) override;
    void redo(Document& doc) override;
    size_t byteCount() const override;

private:
    void write(Document& doc, const Image& patch);

    int layerUid_;
    Rect rect_;
    Image before_;
    Image after_;
};

class SelectionEdit : public HistoryItem {
public:
    SelectionEdit(QString name, bool hadBefore, Mask before, bool hasAfter, Mask after);

    void undo(Document& doc) override;
    void redo(Document& doc) override;
    size_t byteCount() const override;

private:
    void write(Document& doc, bool active, const Mask& mask);

    bool hadBefore_;
    Mask before_;
    bool hasAfter_;
    Mask after_;
};

class DocumentEdit : public HistoryItem {
public:
    DocumentEdit(QString name, QString icon, DocumentState before, DocumentState after);

    void undo(Document& doc) override;
    void redo(Document& doc) override;
    size_t byteCount() const override;

private:
    DocumentState before_;
    DocumentState after_;
};

// Runs a callback on undo / redo, for edits that are cheaper to describe than
// to snapshot (layer visibility, opacity, blend mode, renames).
class PropertyEdit : public HistoryItem {
public:
    using Apply = std::function<void(Document&)>;
    PropertyEdit(QString name, QString icon, Apply undoFn, Apply redoFn)
        : HistoryItem(std::move(name), std::move(icon)),
          undo_(std::move(undoFn)), redo_(std::move(redoFn)) {}

    void undo(Document& doc) override { undo_(doc); }
    void redo(Document& doc) override { redo_(doc); }

private:
    Apply undo_;
    Apply redo_;
};

class History {
public:
    explicit History(Document* doc) : doc_(doc) {}

    void setChangedCallback(std::function<void()> callback)
    {
        changed_ = std::move(callback);
    }

    void push(std::unique_ptr<HistoryItem> item);
    bool undo();
    bool redo();
    void goTo(int index);
    void clear();

    bool canUndo() const { return index_ >= 0; }
    bool canRedo() const { return index_ + 1 < static_cast<int>(items_.size()); }
    QString undoName() const;
    QString redoName() const;

    int count() const { return static_cast<int>(items_.size()); }
    int currentIndex() const { return index_; }
    const HistoryItem* itemAt(int index) const;
    size_t byteCount() const;

    void markSaved();
    bool atSavePoint() const { return index_ == savePoint_; }

    void setLimits(int maxItems, size_t maxBytes)
    {
        maxItems_ = maxItems;
        maxBytes_ = maxBytes;
        trim();
    }

private:
    void trim();
    void notify();

    Document* doc_;
    std::vector<std::unique_ptr<HistoryItem>> items_;
    int index_ = -1;
    int savePoint_ = -1;
    int maxItems_ = 250;
    size_t maxBytes_ = size_t(2) * 1024 * 1024 * 1024;
    std::function<void()> changed_;
};

// Records the pixels of one layer before and after a block of edits.
//
//   EditScope scope(doc, history, layer, "Paintbrush", "brush", bounds);
//   ... paint into layer->image ...
//   scope.commit();
class EditScope {
public:
    EditScope(Document& doc, History& history, Layer* layer, QString name,
              QString icon, Rect rect = Rect());
    ~EditScope();

    bool commit();
    void cancel();
    const Rect& rect() const { return rect_; }

private:
    Document& doc_;
    History& history_;
    Layer* layer_;
    QString name_;
    QString icon_;
    Rect rect_;
    bool wholeLayer_;
    bool done_ = false;
    Image before_;
};

// Runs `action`, then records the whole document difference.
void recordDocumentEdit(Document& doc, History& history, const QString& name,
                        const QString& icon, const std::function<void()>& action);

// Records a selection change around `action`.
void recordSelectionEdit(Document& doc, History& history, const QString& name,
                         const std::function<void()>& action);

Rect changedRect(const Image& before, const Image& after);

} // namespace tessel
