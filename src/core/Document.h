#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QString>

#include "core/Blend.h"
#include "core/Image.h"
#include "core/Selection.h"
#include "core/Units.h"

namespace tessel {

class Layer {
public:
    Layer(Image image, QString name);
    Layer(int width, int height, QString name, Rgba fill = Rgba(0, 0, 0, 0));
    // An empty group, ready to be handed to Document::addChild.
    static std::unique_ptr<Layer> makeGroup(QString name);

    Image image;
    QString name;
    bool visible = true;
    uint8_t opacity = 255;
    BlendMode mode = BlendMode::Normal;
    // Whether the Layers panel shows the children of a group. Pure interface
    // state, but it belongs to the layer so it survives undo and saving.
    bool expanded = true;

    int uid() const { return uid_; }
    int width() const { return image.width(); }
    int height() const { return image.height(); }
    std::unique_ptr<Layer> clone(const QString& newName = QString()) const;
    bool isEmpty() const;

    // ---------------------------------------------------------- groups
    // A group holds child layers instead of pixels. Its own image stays empty:
    // compositing flattens the children and then applies the group's own
    // opacity, blend mode and mask to the result.
    bool isGroup() const { return group_; }
    Layer* parent() const { return parent_; }
    int childCount() const { return static_cast<int>(children_.size()); }
    Layer* childAt(int index);
    const Layer* childAt(int index) const;
    // Position among its siblings, or -1 when the layer has no parent.
    int indexInParent() const;
    bool isDescendantOf(const Layer* other) const;

    // ---------------------------------------------------------- mask
    // An 8 bit mask that scales the layer's alpha while compositing. It is held
    // as an ordinary grey raster wrapped in a Layer of its own, so the painting
    // tools and the history can treat it exactly like any other layer.
    bool hasMask() const { return mask_ != nullptr; }
    Layer* maskLayer() { return mask_.get(); }
    const Layer* maskLayer() const { return mask_.get(); }
    Image* maskImage() { return mask_ ? &mask_->image : nullptr; }
    const Image* maskImage() const { return mask_ ? &mask_->image : nullptr; }
    bool maskEnabled() const { return maskEnabled_; }
    void setMaskEnabled(bool on) { maskEnabled_ = on; }
    // True when painting should land in the mask instead of the pixels.
    bool maskSelected() const { return mask_ && maskSelected_; }
    void setMaskSelected(bool on) { maskSelected_ = on && mask_ != nullptr; }
    void invertMask();

    // Grey value of one mask pixel: 0 hides the layer, 255 reveals it.
    static uint8_t maskValue(Rgba pixel);

private:
    friend class Document;
    int uid_ = 0;
    bool group_ = false;
    bool maskEnabled_ = true;
    bool maskSelected_ = false;
    Layer* parent_ = nullptr;
    std::unique_ptr<Layer> mask_;
    std::vector<std::unique_ptr<Layer>> children_;
    void setUid(int uid) { uid_ = uid; }
};

// A snapshot of the whole document, used by structural undo entries.
struct DocumentState {
    int width = 0;
    int height = 0;
    double resolution = units::kDefaultResolution;
    int active = 0;
    struct LayerState {
        Image image;
        QString name;
        bool visible = true;
        uint8_t opacity = 255;
        BlendMode mode = BlendMode::Normal;
        int uid = 0;
        bool group = false;
        bool expanded = true;
        bool hasMask = false;
        bool maskEnabled = true;
        bool maskSelected = false;
        int maskUid = 0;
        Image mask;
        std::vector<LayerState> children;
    };
    std::vector<LayerState> layers;
    bool hasSelection = false;
    Mask selection;

    size_t byteCount() const;
};

enum class Anchor {
    TopLeft, Top, TopRight,
    Left, Center, Right,
    BottomLeft, Bottom, BottomRight
};

class Document {
public:
    Document(int width, int height, Rgba background = Rgba(255, 255, 255, 255));

    int width() const { return width_; }
    int height() const { return height_; }
    Rect rect() const { return Rect(0, 0, width_, height_); }
    QSize size() const { return QSize(width_, height_); }

    // Pixels per inch. Purely metadata: it never changes a pixel, it only
    // tells the interface and the file formats how large the image prints.
    double resolution() const { return resolution_; }
    void setResolution(double resolution);

    // ---------------------------------------------------------- layers
    // Layers live in a tree, but they are also addressed by a flat index that
    // runs bottom to top with the children of a group listed before the group
    // itself, which is the order the Layers panel shows upside down.
    int layerCount() const;
    Layer* layerAt(int index);
    const Layer* layerAt(int index) const;
    Layer* layerByUid(int uid);
    int indexOfUid(int uid) const;
    int activeIndex() const;
    void setActiveIndex(int index);
    // The layer the interface points at, which may be a group.
    Layer* activeNode();
    const Layer* activeNode() const;
    // Where painting lands: the mask of the active layer when the mask is the
    // selected target, the active layer otherwise.
    Layer* activeLayer();
    const Layer* activeLayer() const;

    Layer* addLayer(std::unique_ptr<Layer> layer, int index = -1);
    Layer* addEmptyLayer(int index = -1);
    // An empty group, above the active layer or inside it when it is a group.
    Layer* addGroup(const QString& name = QString(), int index = -1);
    // A copy of one layer, with its mask and its children, right above it.
    Layer* duplicateLayer(int index);
    std::unique_ptr<Layer> takeLayer(int index);
    bool removeLayer(int index);
    void moveLayer(int from, int to);
    bool mergeLayerDown(int index);
    void flatten();
    QString uniqueLayerName(const QString& base) const;

    // ---------------------------------------------------------- tree
    // The root is a group that is never shown and never composited on its own.
    Layer* rootGroup() { return root_.get(); }
    const Layer* rootGroup() const { return root_.get(); }
    // Inserts `layer` as a child of `parent` (null means the root) at
    // `childIndex`, counted from the bottom; -1 appends on top.
    Layer* addChild(Layer* parent, std::unique_ptr<Layer> layer, int childIndex = -1);
    // Moves an existing layer, with its children, into another group. Fails
    // when the move would put a group inside itself.
    bool reparentLayer(Layer* layer, Layer* parent, int childIndex);
    bool canReparent(const Layer* layer, const Layer* parent) const;

    // ---------------------------------------------------------- masks
    // `reveal` starts the mask white, which shows the layer unchanged.
    Layer* addMask(Layer* layer, bool reveal = true);
    Layer* setMask(Layer* layer, Image mask, bool enabled = true);
    void removeMask(Layer* layer);
    // Bakes the mask into the pixels and drops it.
    bool applyMask(Layer* layer);

    // ---------------------------------------------------------- pixels
    const Image& composite() const;
    Image flattenedCopy() const;
    Image thumbnail(int maxSize) const;
    // What one layer contributes to the composite: the flattened children for
    // a group, the pixels for a layer, with its mask already applied.
    Image renderLayer(const Layer& layer) const;
    void invalidate();
    void invalidateLayer(Layer* layer);

    // A temporary stand in image for one layer, so effect dialogs can show a
    // live preview without modifying the document.
    void setPreview(int layerUid, const Image* image);
    void clearPreview();
    bool hasPreview() const { return previewUid_ >= 0; }

    // ---------------------------------------------------------- canvas
    void resizeCanvas(int width, int height, Anchor anchor, bool scaleContent);
    void cropTo(const Rect& rect);
    void rotate90(int turns);
    void flip(bool horizontal);

    // ---------------------------------------------------------- selection
    Selection& selection() { return selection_; }
    const Selection& selection() const { return selection_; }

    // ---------------------------------------------------------- state
    DocumentState snapshot() const;
    void restore(const DocumentState& state);

    QString filePath() const { return filePath_; }
    void setFilePath(const QString& path) { filePath_ = path; }
    QString title() const { return title_; }
    void setTitle(const QString& title) { title_ = title; }
    QString displayName() const;
    bool isModified() const { return modified_; }
    void setModified(bool modified) { modified_ = modified; }
    int nextLayerSerial() { return ++layerSerial_; }

private:
    void assignUids(Layer* layer);
    void markStructureChanged();
    // The pixels one layer contributes, without its own opacity and mode.
    // Rasters that have to be built for the call are parked in `owned`, which
    // is a deque so that the pointers handed out stay valid.
    const Image* rasterOf(const Layer& layer, std::deque<Image>& owned) const;
    const std::vector<Layer*>& flat() const;
    // Every raster the document owns: layer pixels and mask pixels, but not
    // the empty images of groups.
    void forEachRaster(const std::function<void(Image&, bool isMask)>& action);
    // Maps a flat index onto a parent and a position among its children.
    bool resolveInsertion(int index, Layer** parent, int* childIndex) const;
    std::unique_ptr<Layer> detach(Layer* layer);
    void fitToCanvas(Layer* layer);
    void snapshotLayer(const Layer& layer, DocumentState::LayerState& out) const;
    std::unique_ptr<Layer> restoreLayer(const DocumentState::LayerState& state);

    int width_ = 0;
    int height_ = 0;
    double resolution_ = units::kDefaultResolution;
    int activeUid_ = 0;
    int uidCounter_ = 0;
    int layerSerial_ = 1;
    std::unique_ptr<Layer> root_;
    Selection selection_;
    QString filePath_;
    QString title_ = QStringLiteral("Untitled");
    bool modified_ = false;

    mutable std::vector<Layer*> flat_;
    mutable bool flatValid_ = false;
    mutable Image composite_;
    mutable bool compositeValid_ = false;
    int previewUid_ = -1;
    const Image* previewImage_ = nullptr;
};

QPoint anchorOffset(Anchor anchor, int oldW, int oldH, int newW, int newH);
const char* anchorName(Anchor anchor);

} // namespace tessel
