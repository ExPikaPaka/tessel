#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include "core/Blend.h"
#include "core/Image.h"
#include "core/Params.h"
#include "core/Selection.h"
#include "core/Units.h"

namespace tessel {

// One stored effect: the registry id of an effect and the parameters it was
// last given. It is what an adjustment layer holds instead of pixels, and what
// one entry of an ordinary layer's non destructive stack is.
//
// The id is kept rather than a pointer into the registry so that a document
// which names an effect this build does not have (a plugin that is not
// installed) still loads, saves and composites: an unknown entry passes its
// pixels through untouched instead of dropping them.
struct LayerEffect {
    QString effectId;
    ParamValues values;
    bool enabled = true;
};

class Layer {
public:
    Layer(Image image, QString name);
    Layer(int width, int height, QString name, Rgba fill = Rgba(0, 0, 0, 0));
    // An empty group, ready to be handed to Document::addChild.
    static std::unique_ptr<Layer> makeGroup(QString name);
    // An adjustment layer, ready to be handed to Document::addChild.
    static std::unique_ptr<Layer> makeAdjustment(QString name, QString effectId,
                                                 ParamValues values);

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

    // ---------------------------------------------------------- effects
    // An ordinary layer carries a stack of effects that are applied to its
    // pixels on the way into the composite, with the stored pixels left alone.
    // An adjustment layer carries exactly one entry and no pixels of its own:
    // the group that holds it applies that entry to everything composited
    // below it, see Document::flattenChildren.
    //
    // Editing the entries in place is allowed, but the cached result has to be
    // dropped afterwards, which is what Document::effectsChanged does.
    bool isAdjustment() const { return adjustment_; }
    const std::vector<LayerEffect>& effects() const { return effects_; }
    std::vector<LayerEffect>& effects() { return effects_; }
    // True when at least one entry would actually change the pixels.
    bool hasActiveEffects() const;

private:
    friend class Document;
    int uid_ = 0;
    bool group_ = false;
    bool adjustment_ = false;
    bool maskEnabled_ = true;
    bool maskSelected_ = false;
    // Bumped whenever the pixels change, so a cached effect result can tell
    // that it was computed from an older version of them.
    uint32_t revision_ = 0;
    Layer* parent_ = nullptr;
    std::unique_ptr<Layer> mask_;
    std::vector<std::unique_ptr<Layer>> children_;
    std::vector<LayerEffect> effects_;
    // One cached image per effect entry, holding the result of the entries up
    // to and including that one, with the key it was computed for. Keeping the
    // intermediate steps is what lets a dragged slider on the last entry of a
    // stack rerun that entry alone.
    mutable std::vector<Image> fxCache_;
    mutable std::vector<uint64_t> fxKeys_;
    // The layer's whole canvas contribution, with its stack and its mask
    // already in it, for the layers the tile compositor may not build a piece
    // at a time, see Document::wholeRasterOf.
    mutable Image whole_;
    mutable uint64_t wholeKey_ = 0;
    mutable bool wholeValid_ = false;
    void setUid(int uid) { uid_ = uid; }
    void dropEffectCache() const;
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
        bool adjustment = false;
        std::vector<LayerEffect> effects;
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

    // ---------------------------------------------------------- effects
    // A new adjustment layer holding `effectId`, above the active layer, the
    // same way addLayer places an ordinary one.
    Layer* addAdjustmentLayer(const QString& effectId, const ParamValues& values,
                              const QString& name = QString(), int index = -1);
    // Replaces a layer's effect stack as one step.
    void setLayerEffects(Layer* layer, std::vector<LayerEffect> effects);
    // Drops the result cached for a layer's stack, which has to happen after
    // its entries are edited through Layer::effects().
    void effectsChanged(Layer* layer);
    // Writes the result of a layer's stack into its pixels and empties the
    // stack. The composite does not change; what was non destructive simply
    // stops being undoable without the history.
    bool flattenLayerEffects(Layer* layer);
    // Runs one stored effect over `source`. Unknown ids pass the pixels
    // through, see LayerEffect.
    Image runLayerEffect(const LayerEffect& entry, const Image& source) const;

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
    // The children of `parent` flattened into one image, with the adjustment
    // layers among them already applied to what lies below them. `count` is
    // how many children from the bottom to take, -1 for all of them. The
    // formats that have no adjustment layers of their own use it to bake them
    // into the pixels they write.
    Image flattenChildren(const Layer& parent, int count = -1) const;
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
    //
    // Effects are the exception, and they are kept out of the tiles entirely.
    // A blur, a distortion or a glow reads neighbouring pixels, so a tile
    // computed from its own backdrop alone gives a different answer either
    // side of every tile border. A layer carrying an enabled stack, and a
    // level holding an adjustment layer, are therefore built once at canvas
    // size and cached on the layer, and the tiles window into that one
    // result; see needsWholeCanvas and wholeRasterOf. A document with such an
    // effect in it also pays the whole grid for a change anywhere, because
    // the rectangle an edit reports says nothing about how far the effect
    // carried it. A document without one, which is the common case, is
    // untouched by any of that.
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
    // one more layer, plus a byte per tile. The canvas sized results an effect
    // is built into are not part of it; those are one raster per layer that
    // runs an effect, and they are the price of a seam free one.
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
    // The whole canvas contribution of a layer the tiles may not build a piece
    // at a time, cached on the layer so that every tile windows into the one
    // result. An effect reads neighbouring pixels, so a tile computed from its
    // own backdrop alone disagrees with its neighbours along the seam between
    // them; building the level once at canvas size is what keeps the seams out.
    const Image* wholeRasterOf(const Layer& layer) const;
    // True when `layer` has to go through wholeRasterOf: it carries an enabled
    // effect of its own, or it is a group holding an adjustment layer that
    // would act on what lies below it.
    static bool needsWholeCanvas(const Layer& layer);
    // Builds every whole canvas result the tiles are about to read. The caches
    // are filled lazily and shared between the tiles, so they have to be in
    // place before the tiles go out to the pool.
    void warmWholeRasters(const Layer& parent) const;
    // True when some layer in the document carries an enabled effect, which is
    // what decides whether the rectangle an edit reports is worth anything.
    bool anyActiveEffects() const;
    // Applies a layer's effect stack to `base`, which `baseKey` identifies:
    // the layer's own pixels for a stack, the backdrop for an adjustment
    // layer. The result is cached per entry on the layer, so only the entries
    // at and above a change are recomputed. Returns `base` itself when there
    // is nothing enabled to run.
    const Image* applyEffects(const Layer& layer, const Image& base,
                              uint64_t baseKey) const;
    // The identity of the pixels a layer starts from, before its own stack.
    uint64_t sourceKey(const Layer& layer) const;
    // The identity of the raster rasterOf hands back: the source, the layer's
    // own stack and its mask, but not its opacity or its blend mode, which the
    // parent applies afterwards.
    uint64_t rasterKey(const Layer& layer) const;
    // The identity of what a layer contributes to its parent, stack included.
    uint64_t contributionKey(const Layer& layer) const;
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
    QString title_ = QCoreApplication::translate("Core", "Untitled");
    bool modified_ = false;

    // Bumped by invalidate(), which says that pixels somewhere changed without
    // saying where, so every cached effect result has to be rebuilt.
    uint32_t pixelEpoch_ = 1;
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
    // Bumped every time a preview is put in place or taken away. It stands for
    // the contents of the preview buffer in a cache key, because the dialogs
    // refill one buffer rather than handing over a new one.
    uint32_t previewSerial_ = 0;
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
