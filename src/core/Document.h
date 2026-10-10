#pragma once

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
};

QPoint anchorOffset(Anchor anchor, int oldW, int oldH, int newW, int newH);
const char* anchorName(Anchor anchor);

} // namespace tessel
