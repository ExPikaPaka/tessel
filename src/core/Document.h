#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QString>
#include <QStringList>

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

// A piece of text that the text tool rendered into a layer and kept whole, so
// it can be picked up later and have its words, font, colour or position
// changed.
//
// When a piece of text stops being editable: an item is live only while the
// pixels of `box` on the layer it names are still exactly the ones the text
// tool wrote there. `fingerprint` is a hash of that rectangle taken right
// after the render, and `backdrop` is what the rectangle held before the
// glyphs went in, which is what lets the words be rendered again in place.
// Anything that changes those pixels (a stroke across the box, a fill, an
// effect, a crop, a canvas resize, a merge) makes the hash disagree and the
// item is dropped: the glyphs stay behind as ordinary pixels and the status
// bar says so. Edits elsewhere on the layer, or on another layer, leave the
// item alone, and so does undoing them, because the pixels come back as they
// were. Undoing the commit that made the item is a change like any other, so
// the item goes with it. The text tool draws a dashed box around every item
// that is still live, so what is on screen says what can be picked up.
struct TextItem {
    int layerUid = 0;
    QStringList lines;
    double x = 0.0;          // top left corner, in document coordinates
    double y = 0.0;
    QString family;
    double pointSize = 24.0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    int align = 0;           // 0 left, 1 centre, 2 right
    Rgba color{0, 0, 0, 255};
    double opacity = 1.0;
    bool antialias = true;
    BlendMode mode = BlendMode::Normal;
    Rect box;                // the rectangle the render covered, and is hashed
    Image backdrop;          // `box` as it was before the glyphs were drawn
    uint32_t fingerprint = 0;
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
    // Only `area`, in document coordinates, has to be composited again. An
    // empty rectangle means the whole canvas, the way a tool's repaint does.
    void invalidateRect(const Rect& area);
    void invalidateLayer(Layer* layer, const Rect& area = Rect());

    // ---------------------------------------------------------- tiles
    // The cached composite is tracked by a grid of fixed tiles. A change to a
    // layer dirties the tiles its rectangle touches and the next composite
    // rebuilds only those, so editing a corner of a large document costs what
    // the corner costs.
    //
    // 64 is where the two costs meet. A brush dab costs the area of the tiles
    // it lands on, so painting wants the tile small, while the setup every
    // tile pays for itself (walking the layer tree, and a tile sized buffer
    // for every group and every masked layer) wants it large. On a 4000 by
    // 3000 document with eight layers, three of them masked and two of them
    // inside a masked group, a 40 pixel dab and a full rebuild cost:
    //
    //      tile    dab        full rebuild
    //      32      0.14 ms    70 ms
    //      64      0.26 ms    65 ms
    //      128     0.40 ms    65 ms
    //      256     0.83 ms    68 ms
    //
    // The rebuild stops improving below 128 and turns back up at 32, where the
    // per tile setup starts to outweigh the pixels, so 64 is the smallest tile
    // that costs a full rebuild nothing. It also keeps a tile's temporaries at
    // 16 KB each, so the ones a dozen layers need still sit in L2.
    static constexpr int kTileSize = 64;
    int tileColumns() const;
    int tileRows() const;
    int tileCount() const { return tileColumns() * tileRows(); }
    // The tile grid position of one document rectangle, as a tile index range.
    Rect tilesFor(const Rect& area) const;
    // How many tiles the last call to composite() had to rebuild, and how many
    // are waiting for the next one. Both are there for the tests and the
    // benchmarks; nothing in the program steers by them.
    int tilesRebuilt() const { return tilesRebuilt_; }
    int dirtyTileCount() const;
    // What the tile cache costs: one canvas sized raster, which is the size of
    // one more layer, plus a byte per tile.
    size_t tileCacheBytes() const;

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

    // ---------------------------------------------------------- text items
    // Text that can still be edited, see TextItem. The text tool owns the
    // policy; the document only carries the items so they can be saved.
    const std::vector<TextItem>& textItems() const { return textItems_; }
    std::vector<TextItem>& textItems() { return textItems_; }

private:
    void assignUids(Layer* layer);
    void markStructureChanged();
    // The pixels one layer contributes to `area`, without its own opacity and
    // mode. The raster handed back either covers the whole canvas, and then
    // the offsets are zero, or exactly the part of `area` the layer reaches,
    // and then they say where its top left pixel sits. Rasters that have to be
    // built for the call are parked in `owned`, which is a deque so that the
    // pointers handed out stay valid.
    const Image* rasterOf(const Layer& layer, const Rect& area,
                          std::deque<Image>& owned, int* ox, int* oy,
                          bool parallel) const;
    // Composites one tile, or one band of one tile, into the cached raster.
    void buildTile(const Rect& region) const;
    Rect tileRect(int index) const;
    // Sizes the dirty grid to the canvas, dirtying all of it when the canvas
    // has changed shape under it.
    void ensureTileGrid() const;
    void dirtyAllTiles() const;
    void markTiles(const Rect& area);
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
    // One byte per tile of composite_, row major over tileColumns().
    mutable std::vector<uint8_t> tileDirty_;
    // The box the dirty tiles fall in, in tile coordinates, so that finding
    // them again costs the box and not the grid.
    mutable Rect dirtyBox_;
    mutable int gridColumns_ = 0;
    mutable int tilesRebuilt_ = 0;
    int previewUid_ = -1;
    const Image* previewImage_ = nullptr;
    std::vector<TextItem> textItems_;
};

QPoint anchorOffset(Anchor anchor, int oldW, int oldH, int newW, int newH);
const char* anchorName(Anchor anchor);

// Hash of one rectangle of a layer, used to tell whether a TextItem is still
// live. Zero for an empty rectangle or one that hangs off the image, and zero
// never counts as a match.
uint32_t textFingerprint(const Image& image, const Rect& box);
// True while the item's layer is still there and its pixels are untouched.
bool textItemLive(const Document& doc, const TextItem& item);

} // namespace tessel
