#pragma once

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

    Image image;
    QString name;
    bool visible = true;
    uint8_t opacity = 255;
    BlendMode mode = BlendMode::Normal;

    int uid() const { return uid_; }
    int width() const { return image.width(); }
    int height() const { return image.height(); }
    std::unique_ptr<Layer> clone(const QString& newName = QString()) const;
    bool isEmpty() const;

private:
    friend class Document;
    int uid_ = 0;
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
        bool visible;
        uint8_t opacity;
        BlendMode mode;
        int uid;
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
    int layerCount() const { return static_cast<int>(layers_.size()); }
    Layer* layerAt(int index);
    const Layer* layerAt(int index) const;
    Layer* layerByUid(int uid);
    int indexOfUid(int uid) const;
    int activeIndex() const { return activeIndex_; }
    void setActiveIndex(int index);
    Layer* activeLayer();
    const Layer* activeLayer() const;

    Layer* addLayer(std::unique_ptr<Layer> layer, int index = -1);
    Layer* addEmptyLayer(int index = -1);
    std::unique_ptr<Layer> takeLayer(int index);
    bool removeLayer(int index);
    void moveLayer(int from, int to);
    bool mergeLayerDown(int index);
    void flatten();
    QString uniqueLayerName(const QString& base) const;

    // ---------------------------------------------------------- pixels
    const Image& composite() const;
    Image flattenedCopy() const;
    Image thumbnail(int maxSize) const;
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

    // ---------------------------------------------------------- text items
    // Text that can still be edited, see TextItem. The text tool owns the
    // policy; the document only carries the items so they can be saved.
    const std::vector<TextItem>& textItems() const { return textItems_; }
    std::vector<TextItem>& textItems() { return textItems_; }

private:
    void assignUid(Layer* layer);

    int width_ = 0;
    int height_ = 0;
    double resolution_ = units::kDefaultResolution;
    int activeIndex_ = 0;
    int uidCounter_ = 0;
    int layerSerial_ = 1;
    std::vector<std::unique_ptr<Layer>> layers_;
    Selection selection_;
    QString filePath_;
    QString title_ = QStringLiteral("Untitled");
    bool modified_ = false;

    mutable Image composite_;
    mutable bool compositeValid_ = false;
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
