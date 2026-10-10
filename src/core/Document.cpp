#include "core/Document.h"

#include <cmath>

#include <QFileInfo>

#include "core/ImageOps.h"
#include "core/Units.h"

namespace tessel {

// ----------------------------------------------------------------- Layer

Layer::Layer(Image img, QString layerName)
    : image(std::move(img)), name(std::move(layerName))
{
}

Layer::Layer(int width, int height, QString layerName, Rgba fill)
    : image(width, height, fill), name(std::move(layerName))
{
}

std::unique_ptr<Layer> Layer::clone(const QString& newName) const
{
    auto copy = std::make_unique<Layer>(image, newName.isEmpty() ? name : newName);
    copy->visible = visible;
    copy->opacity = opacity;
    copy->mode = mode;
    return copy;
}

bool Layer::isEmpty() const
{
    for (size_t i = 0; i < image.pixelCount(); ++i) {
        if (image.data()[i].a != 0)
            return false;
    }
    return true;
}

size_t DocumentState::byteCount() const
{
    size_t total = selection.byteCount();
    for (const LayerState& layer : layers)
        total += layer.image.byteCount();
    return total;
}

// ----------------------------------------------------------------- Document

Document::Document(int width, int height, Rgba background)
    : width_(std::max(1, width)), height_(std::max(1, height)),
      selection_(width_, height_)
{
    auto layer = std::make_unique<Layer>(width_, height_,
                                         QStringLiteral("Background"), background);
    assignUid(layer.get());
    layers_.push_back(std::move(layer));
    activeIndex_ = 0;
}

void Document::setResolution(double resolution)
{
    const double next = units::sanitize(resolution);
    if (std::abs(next - resolution_) < 1e-9)
        return;
    resolution_ = next;
    modified_ = true;
}

void Document::assignUid(Layer* layer)
{
    layer->setUid(++uidCounter_);
}

Layer* Document::layerAt(int index)
{
    if (index < 0 || index >= layerCount())
        return nullptr;
    return layers_[static_cast<size_t>(index)].get();
}

const Layer* Document::layerAt(int index) const
{
    if (index < 0 || index >= layerCount())
        return nullptr;
    return layers_[static_cast<size_t>(index)].get();
}

Layer* Document::layerByUid(int uid)
{
    for (auto& layer : layers_) {
        if (layer->uid() == uid)
            return layer.get();
    }
    return nullptr;
}

int Document::indexOfUid(int uid) const
{
    for (int i = 0; i < layerCount(); ++i) {
        if (layers_[static_cast<size_t>(i)]->uid() == uid)
            return i;
    }
    return -1;
}

void Document::setActiveIndex(int index)
{
    activeIndex_ = std::clamp(index, 0, std::max(0, layerCount() - 1));
}

Layer* Document::activeLayer()
{
    setActiveIndex(activeIndex_);
    return layerAt(activeIndex_);
}

const Layer* Document::activeLayer() const
{
    return layerAt(std::clamp(activeIndex_, 0, std::max(0, layerCount() - 1)));
}

QString Document::uniqueLayerName(const QString& base) const
{
    auto taken = [this](const QString& candidate) {
        for (const auto& layer : layers_) {
            if (layer->name == candidate)
                return true;
        }
        return false;
    };
    if (!taken(base))
        return base;
    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(i);
        if (!taken(candidate))
            return candidate;
    }
    return base;
}

Layer* Document::addLayer(std::unique_ptr<Layer> layer, int index)
{
    if (!layer)
        return nullptr;
    if (layer->width() != width_ || layer->height() != height_) {
        Image sized(width_, height_);
        sized.blit(layer->image, 0, 0);
        layer->image = std::move(sized);
    }
    assignUid(layer.get());
    if (index < 0)
        index = activeIndex_ + 1;
    index = std::clamp(index, 0, layerCount());
    Layer* raw = layer.get();
    layers_.insert(layers_.begin() + index, std::move(layer));
    activeIndex_ = index;
    invalidate();
    return raw;
}

Layer* Document::addEmptyLayer(int index)
{
    auto layer = std::make_unique<Layer>(
        width_, height_,
        uniqueLayerName(QStringLiteral("Layer %1").arg(nextLayerSerial())));
    return addLayer(std::move(layer), index);
}

std::unique_ptr<Layer> Document::takeLayer(int index)
{
    if (index < 0 || index >= layerCount())
        return nullptr;
    auto layer = std::move(layers_[static_cast<size_t>(index)]);
    layers_.erase(layers_.begin() + index);
    activeIndex_ = std::clamp(index, 0, std::max(0, layerCount() - 1));
    invalidate();
    return layer;
}

bool Document::removeLayer(int index)
{
    if (layerCount() <= 1)
        return false;
    return takeLayer(index) != nullptr;
}

void Document::moveLayer(int from, int to)
{
    if (from < 0 || from >= layerCount())
        return;
    to = std::clamp(to, 0, layerCount() - 1);
    if (from == to)
        return;
    auto layer = std::move(layers_[static_cast<size_t>(from)]);
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + to, std::move(layer));
    activeIndex_ = to;
    invalidate();
}

bool Document::mergeLayerDown(int index)
{
    if (index <= 0 || index >= layerCount())
        return false;
    Layer* upper = layerAt(index);
    Layer* lower = layerAt(index - 1);
    std::vector<CompositeLayer> pair = {
        {&lower->image, lower->opacity, lower->mode, lower->visible},
        {&upper->image, upper->opacity, upper->mode, upper->visible},
    };
    Image merged = compositeLayers(pair, width_, height_);
    lower->image = std::move(merged);
    lower->opacity = 255;
    lower->mode = BlendMode::Normal;
    lower->visible = true;
    layers_.erase(layers_.begin() + index);
    activeIndex_ = index - 1;
    invalidate();
    return true;
}

void Document::flatten()
{
    Image merged = composite();
    const QString name = layers_.empty() ? QStringLiteral("Background")
                                         : layers_.front()->name;
    auto layer = std::make_unique<Layer>(std::move(merged), name);
    assignUid(layer.get());
    layers_.clear();
    layers_.push_back(std::move(layer));
    activeIndex_ = 0;
    invalidate();
}

const Image& Document::composite() const
{
    if (compositeValid_ && composite_.width() == width_
        && composite_.height() == height_)
        return composite_;
    std::vector<CompositeLayer> entries;
    entries.reserve(layers_.size());
    for (const auto& layer : layers_) {
        const Image* image = &layer->image;
        if (previewUid_ >= 0 && previewImage_ && layer->uid() == previewUid_)
            image = previewImage_;
        entries.push_back({image, layer->opacity, layer->mode, layer->visible});
    }
    composite_ = compositeLayers(entries, width_, height_);
    compositeValid_ = previewUid_ < 0;
    return composite_;
}

Image Document::flattenedCopy() const
{
    return composite();
}

Image Document::thumbnail(int maxSize) const
{
    const Image& full = composite();
    if (full.isNull())
        return full;
    const double scale = std::min(double(maxSize) / std::max(1, width_),
                                  double(maxSize) / std::max(1, height_));
    const int w = std::max(1, int(std::lround(width_ * scale)));
    const int h = std::max(1, int(std::lround(height_ * scale)));
    return ops::resize(full, w, h, true);
}

void Document::invalidate()
{
    compositeValid_ = false;
    modified_ = true;
}

void Document::invalidateLayer(Layer*)
{
    invalidate();
}

void Document::setPreview(int layerUid, const Image* image)
{
    previewUid_ = layerUid;
    previewImage_ = image;
    compositeValid_ = false;
}

void Document::clearPreview()
{
    previewUid_ = -1;
    previewImage_ = nullptr;
    compositeValid_ = false;
}

void Document::resizeCanvas(int width, int height, Anchor anchor, bool scaleContent)
{
    width = std::max(1, width);
    height = std::max(1, height);
    if (scaleContent) {
        for (auto& layer : layers_)
            layer->image = ops::resize(layer->image, width, height, true);
    } else {
        const QPoint offset = anchorOffset(anchor, width_, height_, width, height);
        for (auto& layer : layers_) {
            Image canvas(width, height);
            canvas.blit(layer->image, offset.x(), offset.y());
            layer->image = std::move(canvas);
        }
    }
    selection_.resizeCanvas(width, height, scaleContent);
    width_ = width;
    height_ = height;
    invalidate();
}

void Document::cropTo(const Rect& area)
{
    Rect target = area.intersected(rect());
    if (target.isEmpty())
        return;
    for (auto& layer : layers_)
        layer->image = layer->image.cropped(target);
    Selection next(target.w, target.h);
    if (const Mask* mask = selection_.clipMask()) {
        Mask cropped(target.w, target.h);
        bool partial = false;
        for (int y = 0; y < target.h; ++y) {
            const float* src = mask->row(target.y + y);
            float* dst = cropped.row(y);
            for (int x = 0; x < target.w; ++x) {
                dst[x] = src[target.x + x];
                if (dst[x] < 0.996f)
                    partial = true;
            }
        }
        if (partial && cropped.anyAbove(0.004f))
            next.setMask(std::move(cropped));
    }
    selection_ = std::move(next);
    width_ = target.w;
    height_ = target.h;
    invalidate();
}

void Document::rotate90(int turns)
{
    turns = ((turns % 4) + 4) % 4;
    if (turns == 0)
        return;
    for (auto& layer : layers_)
        layer->image = ops::rotate90(layer->image, turns);
    if (turns % 2)
        std::swap(width_, height_);
    selection_.transform(false, false, turns);
    invalidate();
}

void Document::flip(bool horizontal)
{
    for (auto& layer : layers_)
        layer->image = ops::flip(layer->image, horizontal);
    selection_.transform(horizontal, !horizontal, 0);
    invalidate();
}

DocumentState Document::snapshot() const
{
    DocumentState state;
    state.width = width_;
    state.height = height_;
    state.resolution = resolution_;
    state.active = activeIndex_;
    state.layers.reserve(layers_.size());
    for (const auto& layer : layers_) {
        state.layers.push_back({layer->image, layer->name, layer->visible,
                                layer->opacity, layer->mode, layer->uid()});
    }
    if (const Mask* mask = selection_.clipMask()) {
        state.hasSelection = true;
        state.selection = *mask;
    }
    return state;
}

void Document::restore(const DocumentState& state)
{
    width_ = state.width;
    height_ = state.height;
    resolution_ = units::sanitize(state.resolution);
    layers_.clear();
    layers_.reserve(state.layers.size());
    for (const DocumentState::LayerState& ls : state.layers) {
        auto layer = std::make_unique<Layer>(ls.image, ls.name);
        layer->visible = ls.visible;
        layer->opacity = ls.opacity;
        layer->mode = ls.mode;
        layer->setUid(ls.uid);
        uidCounter_ = std::max(uidCounter_, ls.uid);
        layers_.push_back(std::move(layer));
    }
    activeIndex_ = std::clamp(state.active, 0, std::max(0, layerCount() - 1));
    Selection next(width_, height_);
    if (state.hasSelection)
        next.setMask(state.selection);
    selection_ = std::move(next);
    invalidate();
}

QString Document::displayName() const
{
    if (!filePath_.isEmpty())
        return QFileInfo(filePath_).fileName();
    return title_;
}

QPoint anchorOffset(Anchor anchor, int oldW, int oldH, int newW, int newH)
{
    int dx = (newW - oldW) / 2;
    int dy = (newH - oldH) / 2;
    switch (anchor) {
    case Anchor::TopLeft: dx = 0; dy = 0; break;
    case Anchor::Top: dy = 0; break;
    case Anchor::TopRight: dx = newW - oldW; dy = 0; break;
    case Anchor::Left: dx = 0; break;
    case Anchor::Right: dx = newW - oldW; break;
    case Anchor::BottomLeft: dx = 0; dy = newH - oldH; break;
    case Anchor::Bottom: dy = newH - oldH; break;
    case Anchor::BottomRight: dx = newW - oldW; dy = newH - oldH; break;
    case Anchor::Center: break;
    }
    return QPoint(dx, dy);
}

const char* anchorName(Anchor anchor)
{
    switch (anchor) {
    case Anchor::TopLeft: return "Top left";
    case Anchor::Top: return "Top";
    case Anchor::TopRight: return "Top right";
    case Anchor::Left: return "Left";
    case Anchor::Right: return "Right";
    case Anchor::BottomLeft: return "Bottom left";
    case Anchor::Bottom: return "Bottom";
    case Anchor::BottomRight: return "Bottom right";
    case Anchor::Center:
    default: return "Center";
    }
}

uint32_t textFingerprint(const Image& image, const Rect& box)
{
    if (box.isEmpty() || box.x < 0 || box.y < 0 || box.right() > image.width()
        || box.bottom() > image.height())
        return 0;
    // FNV-1a over the pixels of the box, with the size mixed in so a box of
    // the same bytes at another size cannot collide.
    uint32_t hash = 2166136261u;
    auto mix = [&hash](uint8_t byte) {
        hash = (hash ^ byte) * 16777619u;
    };
    for (int shift = 0; shift < 32; shift += 8) {
        mix(uint8_t((box.w >> shift) & 0xff));
        mix(uint8_t((box.h >> shift) & 0xff));
    }
    for (int y = box.y; y < box.bottom(); ++y) {
        const Rgba* line = image.row(y);
        for (int x = box.x; x < box.right(); ++x) {
            mix(line[x].r);
            mix(line[x].g);
            mix(line[x].b);
            mix(line[x].a);
        }
    }
    // Zero is reserved for "no fingerprint", so never hand it back.
    return hash == 0 ? 1u : hash;
}

bool textItemLive(const Document& doc, const TextItem& item)
{
    const int index = doc.indexOfUid(item.layerUid);
    if (index < 0)
        return false;
    const Layer* layer = doc.layerAt(index);
    if (!layer)
        return false;
    const uint32_t current = textFingerprint(layer->image, item.box);
    return current != 0 && current == item.fingerprint;
}

} // namespace tessel
