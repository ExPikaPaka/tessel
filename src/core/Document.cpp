#include "core/Document.h"

#include <cmath>

#include <QFileInfo>

#include "core/ImageOps.h"
#include "core/Units.h"

namespace tessel {

namespace {

constexpr Rgba kReveal(255, 255, 255, 255);

// Scales an image's alpha by a grey mask, which is how a layer mask hides and
// reveals what the layer draws. Anything the mask does not cover is hidden,
// which only matters for a mask that somehow lost step with the canvas.
void applyMaskTo(Image& image, const Image& mask)
{
    for (int y = 0; y < image.height(); ++y) {
        Rgba* dst = image.row(y);
        const Rgba* src = y < mask.height() ? mask.row(y) : nullptr;
        for (int x = 0; x < image.width(); ++x) {
            const uint8_t value =
                (src && x < mask.width()) ? Layer::maskValue(src[x]) : 0;
            dst[x].a = static_cast<uint8_t>((int(dst[x].a) * value + 127) / 255);
        }
    }
}

} // namespace

// ----------------------------------------------------------------- Layer

Layer::Layer(Image img, QString layerName)
    : image(std::move(img)), name(std::move(layerName))
{
}

Layer::Layer(int width, int height, QString layerName, Rgba fill)
    : image(width, height, fill), name(std::move(layerName))
{
}

std::unique_ptr<Layer> Layer::makeGroup(QString groupName)
{
    auto group = std::make_unique<Layer>(Image(), std::move(groupName));
    group->group_ = true;
    return group;
}

uint8_t Layer::maskValue(Rgba pixel)
{
    // Luminance weighted the usual way, scaled by the alpha so that erasing a
    // hole in the mask hides the layer just like painting it black does.
    const float luma = 0.299f * pixel.r + 0.587f * pixel.g + 0.114f * pixel.b;
    return static_cast<uint8_t>(luma * (pixel.a / 255.0f) + 0.5f);
}

std::unique_ptr<Layer> Layer::clone(const QString& newName) const
{
    auto copy = std::make_unique<Layer>(image, newName.isEmpty() ? name : newName);
    copy->visible = visible;
    copy->opacity = opacity;
    copy->mode = mode;
    copy->expanded = expanded;
    copy->group_ = group_;
    copy->maskEnabled_ = maskEnabled_;
    copy->maskSelected_ = maskSelected_;
    if (mask_) {
        copy->mask_ = mask_->clone();
        copy->mask_->parent_ = copy.get();
    }
    for (const auto& child : children_) {
        auto childCopy = child->clone();
        childCopy->parent_ = copy.get();
        copy->children_.push_back(std::move(childCopy));
    }
    return copy;
}

bool Layer::isEmpty() const
{
    if (group_) {
        for (const auto& child : children_) {
            if (!child->isEmpty())
                return false;
        }
        return true;
    }
    for (size_t i = 0; i < image.pixelCount(); ++i) {
        if (image.data()[i].a != 0)
            return false;
    }
    return true;
}

Layer* Layer::childAt(int index)
{
    if (index < 0 || index >= childCount())
        return nullptr;
    return children_[static_cast<size_t>(index)].get();
}

const Layer* Layer::childAt(int index) const
{
    if (index < 0 || index >= childCount())
        return nullptr;
    return children_[static_cast<size_t>(index)].get();
}

int Layer::indexInParent() const
{
    if (!parent_)
        return -1;
    for (int i = 0; i < parent_->childCount(); ++i) {
        if (parent_->children_[static_cast<size_t>(i)].get() == this)
            return i;
    }
    return -1;
}

bool Layer::isDescendantOf(const Layer* other) const
{
    for (const Layer* walk = parent_; walk; walk = walk->parent_) {
        if (walk == other)
            return true;
    }
    return false;
}

void Layer::invertMask()
{
    if (!mask_)
        return;
    for (size_t i = 0; i < mask_->image.pixelCount(); ++i) {
        Rgba& pixel = mask_->image.data()[i];
        const uint8_t value = static_cast<uint8_t>(255 - maskValue(pixel));
        pixel = Rgba(value, value, value, 255);
    }
}

size_t DocumentState::byteCount() const
{
    size_t total = selection.byteCount();
    std::function<void(const std::vector<LayerState>&)> walk =
        [&](const std::vector<LayerState>& list) {
            for (const LayerState& layer : list) {
                total += layer.image.byteCount() + layer.mask.byteCount();
                walk(layer.children);
            }
        };
    walk(layers);
    return total;
}

// ----------------------------------------------------------------- Document

Document::Document(int width, int height, Rgba background)
    : width_(std::max(1, width)), height_(std::max(1, height)),
      selection_(width_, height_)
{
    root_ = std::make_unique<Layer>(Image(), QStringLiteral("Document"));
    root_->group_ = true;
    auto layer = std::make_unique<Layer>(width_, height_,
                                         QStringLiteral("Background"), background);
    addChild(root_.get(), std::move(layer));
}

void Document::setResolution(double resolution)
{
    const double next = units::sanitize(resolution);
    if (std::abs(next - resolution_) < 1e-9)
        return;
    resolution_ = next;
    modified_ = true;
}

// Only layers that have never been in a document get an identity, so moving a
// layer or dropping it into a group keeps the uid the history refers to.
void Document::assignUids(Layer* layer)
{
    if (layer->uid_ == 0)
        layer->setUid(++uidCounter_);
    if (layer->mask_ && layer->mask_->uid_ == 0)
        layer->mask_->setUid(++uidCounter_);
    for (auto& child : layer->children_)
        assignUids(child.get());
}

void Document::markStructureChanged()
{
    flatValid_ = false;
    invalidate();
}

const std::vector<Layer*>& Document::flat() const
{
    if (flatValid_)
        return flat_;
    flat_.clear();
    // Bottom to top, with the children of a group ahead of the group itself so
    // that the panel, which runs top first, shows a group above its contents.
    std::function<void(Layer*)> walk = [&](Layer* node) {
        for (auto& child : node->children_)
            walk(child.get());
        if (node != root_.get())
            flat_.push_back(node);
    };
    walk(root_.get());
    flatValid_ = true;
    return flat_;
}

int Document::layerCount() const
{
    return static_cast<int>(flat().size());
}

Layer* Document::layerAt(int index)
{
    const std::vector<Layer*>& list = flat();
    if (index < 0 || index >= static_cast<int>(list.size()))
        return nullptr;
    return list[static_cast<size_t>(index)];
}

const Layer* Document::layerAt(int index) const
{
    const std::vector<Layer*>& list = flat();
    if (index < 0 || index >= static_cast<int>(list.size()))
        return nullptr;
    return list[static_cast<size_t>(index)];
}

Layer* Document::layerByUid(int uid)
{
    for (Layer* layer : flat()) {
        if (layer->uid() == uid)
            return layer;
        if (layer->mask_ && layer->mask_->uid() == uid)
            return layer->mask_.get();
    }
    return nullptr;
}

int Document::indexOfUid(int uid) const
{
    const std::vector<Layer*>& list = flat();
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        if (list[static_cast<size_t>(i)]->uid() == uid)
            return i;
    }
    return -1;
}

int Document::activeIndex() const
{
    const int index = indexOfUid(activeUid_);
    if (index >= 0)
        return index;
    return std::max(0, layerCount() - 1);
}

void Document::setActiveIndex(int index)
{
    const int count = layerCount();
    if (count == 0) {
        activeUid_ = 0;
        return;
    }
    const Layer* layer = layerAt(std::clamp(index, 0, count - 1));
    activeUid_ = layer ? layer->uid() : 0;
}

Layer* Document::activeNode()
{
    return layerAt(activeIndex());
}

const Layer* Document::activeNode() const
{
    return layerAt(activeIndex());
}

Layer* Document::activeLayer()
{
    Layer* node = activeNode();
    if (node && node->maskSelected())
        return node->maskLayer();
    return node;
}

const Layer* Document::activeLayer() const
{
    const Layer* node = activeNode();
    if (node && node->maskSelected())
        return node->maskLayer();
    return node;
}

QString Document::uniqueLayerName(const QString& base) const
{
    auto taken = [this](const QString& candidate) {
        for (const Layer* layer : flat()) {
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

void Document::fitToCanvas(Layer* layer)
{
    if (!layer->isGroup() && (layer->width() != width_ || layer->height() != height_)) {
        Image sized(width_, height_);
        sized.blit(layer->image, 0, 0);
        layer->image = std::move(sized);
    }
    if (layer->mask_) {
        Layer* mask = layer->mask_.get();
        if (mask->width() != width_ || mask->height() != height_) {
            Image sized(width_, height_, kReveal);
            sized.blit(mask->image, 0, 0);
            mask->image = std::move(sized);
        }
    }
    for (auto& child : layer->children_)
        fitToCanvas(child.get());
}

Layer* Document::addChild(Layer* parent, std::unique_ptr<Layer> layer, int childIndex)
{
    if (!layer)
        return nullptr;
    if (!parent || !parent->isGroup())
        parent = root_.get();
    fitToCanvas(layer.get());
    assignUids(layer.get());
    layer->parent_ = parent;
    const int count = parent->childCount();
    if (childIndex < 0 || childIndex > count)
        childIndex = count;
    Layer* raw = layer.get();
    parent->children_.insert(parent->children_.begin() + childIndex, std::move(layer));
    activeUid_ = raw->uid();
    markStructureChanged();
    return raw;
}

bool Document::resolveInsertion(int index, Layer** parent, int* childIndex) const
{
    const Layer* target = layerAt(index);
    if (!target)
        return false;
    *parent = target->parent_ ? target->parent_ : root_.get();
    *childIndex = std::max(0, target->indexInParent());
    return true;
}

Layer* Document::addLayer(std::unique_ptr<Layer> layer, int index)
{
    if (!layer)
        return nullptr;
    Layer* parent = root_.get();
    int childIndex = -1;
    if (index < 0) {
        // Above the active layer, or inside it when the active layer is a group.
        if (Layer* active = activeNode()) {
            if (active->isGroup()) {
                parent = active;
                childIndex = -1;
            } else {
                parent = active->parent_ ? active->parent_ : root_.get();
                childIndex = active->indexInParent() + 1;
            }
        }
    } else {
        resolveInsertion(index, &parent, &childIndex);
    }
    return addChild(parent, std::move(layer), childIndex);
}

Layer* Document::addEmptyLayer(int index)
{
    auto layer = std::make_unique<Layer>(
        width_, height_,
        uniqueLayerName(QStringLiteral("Layer %1").arg(nextLayerSerial())));
    return addLayer(std::move(layer), index);
}

Layer* Document::addGroup(const QString& name, int index)
{
    auto group = Layer::makeGroup(
        name.isEmpty() ? uniqueLayerName(QStringLiteral("Group %1").arg(nextLayerSerial()))
                       : name);
    Layer* parent = root_.get();
    int childIndex = -1;
    if (index >= 0) {
        resolveInsertion(index, &parent, &childIndex);
    } else if (Layer* active = activeNode()) {
        // A new group lands beside the active layer rather than inside it, so
        // that nesting only ever happens because the user asked for it.
        parent = active->parent_ ? active->parent_ : root_.get();
        childIndex = active->indexInParent() + 1;
    }
    return addChild(parent, std::move(group), childIndex);
}

Layer* Document::duplicateLayer(int index)
{
    Layer* source = layerAt(index);
    if (!source)
        return nullptr;
    auto copy = source->clone(
        uniqueLayerName(source->name + QStringLiteral(" copy")));
    Layer* parent = source->parent() ? source->parent() : root_.get();
    return addChild(parent, std::move(copy), source->indexInParent() + 1);
}

std::unique_ptr<Layer> Document::detach(Layer* layer)
{
    Layer* parent = layer ? layer->parent_ : nullptr;
    if (!parent)
        return nullptr;
    const int position = layer->indexInParent();
    if (position < 0)
        return nullptr;
    auto owned = std::move(parent->children_[static_cast<size_t>(position)]);
    parent->children_.erase(parent->children_.begin() + position);
    owned->parent_ = nullptr;
    markStructureChanged();
    return owned;
}

std::unique_ptr<Layer> Document::takeLayer(int index)
{
    Layer* layer = layerAt(index);
    if (!layer)
        return nullptr;
    auto owned = detach(layer);
    setActiveIndex(index);
    return owned;
}

bool Document::removeLayer(int index)
{
    if (layerCount() <= 1)
        return false;
    return takeLayer(index) != nullptr;
}

void Document::moveLayer(int from, int to)
{
    Layer* node = layerAt(from);
    if (!node)
        return;
    const int count = layerCount();
    to = std::clamp(to, 0, count - 1);
    if (from == to)
        return;
    Layer* target = layerAt(to);
    if (!target)
        return;
    const bool upwards = to > from;
    Layer* nodeParent = node->parent_ ? node->parent_ : root_.get();
    Layer* parent = nodeParent;
    int childIndex = 0;
    if (target->isDescendantOf(node)) {
        // A group cannot move inside itself, so it steps over its own contents.
        childIndex = node->indexInParent() + (upwards ? 1 : -1);
        if (childIndex < 0)
            return;
    } else {
        // Moving up or down walks the layer's own level, so a group is stepped
        // over as a whole rather than entered. Dragging in the panel is what
        // puts a layer inside a group.
        Layer* sibling = target;
        while (sibling && sibling->parent_ != nodeParent)
            sibling = sibling->parent_;
        if (sibling) {
            childIndex = sibling->indexInParent();
        } else {
            // The layer has run out of room inside its group, so it leaves it.
            Layer* targetParent = target->parent_ ? target->parent_ : root_.get();
            Layer* outer = node;
            while (outer->parent_ && outer->parent_ != targetParent)
                outer = outer->parent_;
            if (!outer->parent_)
                return;
            parent = targetParent;
            childIndex = outer->indexInParent() + (upwards ? 1 : 0);
        }
    }
    auto owned = detach(node);
    if (!owned)
        return;
    addChild(parent, std::move(owned), childIndex);
}

bool Document::canReparent(const Layer* layer, const Layer* parent) const
{
    if (!layer)
        return false;
    const Layer* target = parent ? parent : root_.get();
    if (!target->isGroup())
        return false;
    if (layer == target)
        return false;
    return !target->isDescendantOf(layer);
}

bool Document::reparentLayer(Layer* layer, Layer* parent, int childIndex)
{
    if (!canReparent(layer, parent))
        return false;
    Layer* target = parent ? parent : root_.get();
    if (layer->parent_ == target) {
        const int position = layer->indexInParent();
        if (position == childIndex || position + 1 == childIndex)
            return false;
        if (childIndex > position)
            --childIndex;
    }
    auto owned = detach(layer);
    if (!owned)
        return false;
    addChild(target, std::move(owned), childIndex);
    return true;
}

bool Document::mergeLayerDown(int index)
{
    Layer* upper = layerAt(index);
    if (!upper || !upper->parent_)
        return false;
    const int position = upper->indexInParent();
    if (position <= 0)
        return false;
    Layer* parent = upper->parent_;
    Layer* lower = parent->children_[static_cast<size_t>(position - 1)].get();
    Image lowerPixels = renderLayer(*lower);
    Image upperPixels = renderLayer(*upper);
    std::vector<CompositeLayer> pair = {
        {&lowerPixels, lower->opacity, lower->mode, true},
        {&upperPixels, upper->opacity, upper->mode, true},
    };
    Image merged = compositeLayers(pair, width_, height_);
    // The result is flat pixels, so a group that takes part in a merge
    // collapses into an ordinary layer and keeps the lower layer's name.
    lower->image = std::move(merged);
    lower->group_ = false;
    lower->children_.clear();
    lower->mask_.reset();
    lower->maskEnabled_ = true;
    lower->maskSelected_ = false;
    lower->opacity = 255;
    lower->mode = BlendMode::Normal;
    lower->visible = true;
    const int keep = lower->uid();
    detach(upper);
    activeUid_ = keep;
    return true;
}

void Document::flatten()
{
    Image merged = composite();
    const QString name = root_->children_.empty()
                             ? QStringLiteral("Background")
                             : root_->children_.front()->name;
    root_->children_.clear();
    auto layer = std::make_unique<Layer>(std::move(merged), name);
    markStructureChanged();
    addChild(root_.get(), std::move(layer));
}

// ----------------------------------------------------------------- masks

Layer* Document::addMask(Layer* layer, bool reveal)
{
    if (!layer || layer->hasMask())
        return layer ? layer->maskLayer() : nullptr;
    Image pixels(width_, height_, reveal ? kReveal : Rgba(0, 0, 0, 255));
    return setMask(layer, std::move(pixels));
}

Layer* Document::setMask(Layer* layer, Image mask, bool enabled)
{
    if (!layer)
        return nullptr;
    auto maskLayer = std::make_unique<Layer>(std::move(mask), QStringLiteral("Mask"));
    maskLayer->parent_ = layer;
    maskLayer->setUid(++uidCounter_);
    layer->mask_ = std::move(maskLayer);
    layer->maskEnabled_ = enabled;
    fitToCanvas(layer);
    invalidate();
    return layer->mask_.get();
}

void Document::removeMask(Layer* layer)
{
    if (!layer || !layer->mask_)
        return;
    layer->mask_.reset();
    layer->maskSelected_ = false;
    layer->maskEnabled_ = true;
    invalidate();
}

bool Document::applyMask(Layer* layer)
{
    if (!layer || !layer->mask_)
        return false;
    if (layer->isGroup()) {
        // A group has no pixels of its own, so baking means flattening the
        // children into one masked layer in the group's place.
        Image merged = renderLayer(*layer);
        layer->group_ = false;
        layer->children_.clear();
        layer->image = std::move(merged);
        markStructureChanged();
    } else if (layer->maskEnabled()) {
        applyMaskTo(layer->image, layer->mask_->image);
    }
    removeMask(layer);
    return true;
}

// ----------------------------------------------------------------- pixels

const Image* Document::rasterOf(const Layer& layer, std::deque<Image>& owned) const
{
    const Image* base = nullptr;
    if (layer.isGroup()) {
        std::deque<Image> children;
        std::vector<CompositeLayer> entries;
        entries.reserve(layer.children_.size());
        for (const auto& child : layer.children_) {
            if (!child->visible || child->opacity == 0)
                continue;
            const Image* raster = rasterOf(*child, children);
            if (!raster || raster->isNull())
                continue;
            entries.push_back({raster, child->opacity, child->mode, true});
        }
        owned.push_back(compositeLayers(entries, width_, height_));
        base = &owned.back();
    } else {
        base = &layer.image;
        if (previewUid_ >= 0 && previewImage_ && layer.uid() == previewUid_)
            base = previewImage_;
    }
    if (!base || base->isNull() || !layer.hasMask() || !layer.maskEnabled())
        return base;
    const Image* mask = layer.maskImage();
    if (previewUid_ >= 0 && previewImage_ && layer.maskLayer()->uid() == previewUid_)
        mask = previewImage_;
    owned.push_back(*base);
    applyMaskTo(owned.back(), *mask);
    return &owned.back();
}

Image Document::renderLayer(const Layer& layer) const
{
    std::deque<Image> owned;
    const Image* raster = rasterOf(layer, owned);
    if (!raster || raster->isNull())
        return Image(width_, height_);
    return *raster;
}

const Image& Document::composite() const
{
    if (compositeValid_ && composite_.width() == width_
        && composite_.height() == height_)
        return composite_;
    std::deque<Image> owned;
    std::vector<CompositeLayer> entries;
    entries.reserve(root_->children_.size());
    for (const auto& child : root_->children_) {
        if (!child->visible || child->opacity == 0)
            continue;
        const Image* raster = rasterOf(*child, owned);
        if (!raster || raster->isNull())
            continue;
        entries.push_back({raster, child->opacity, child->mode, true});
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

void Document::forEachRaster(const std::function<void(Image&, bool)>& action)
{
    std::function<void(Layer*)> walk = [&](Layer* node) {
        if (node != root_.get() && !node->isGroup())
            action(node->image, false);
        if (node->mask_)
            action(node->mask_->image, true);
        for (auto& child : node->children_)
            walk(child.get());
    };
    walk(root_.get());
}

// ----------------------------------------------------------------- canvas

void Document::resizeCanvas(int width, int height, Anchor anchor, bool scaleContent)
{
    width = std::max(1, width);
    height = std::max(1, height);
    const QPoint offset = anchorOffset(anchor, width_, height_, width, height);
    forEachRaster([&](Image& image, bool isMask) {
        if (scaleContent) {
            image = ops::resize(image, width, height, true);
            return;
        }
        // New room in a mask reveals, so growing the canvas never hides what
        // the user then paints into the fresh area.
        Image canvas = isMask ? Image(width, height, kReveal) : Image(width, height);
        canvas.blit(image, offset.x(), offset.y());
        image = std::move(canvas);
    });
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
    forEachRaster([&](Image& image, bool) { image = image.cropped(target); });
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
    forEachRaster([&](Image& image, bool) { image = ops::rotate90(image, turns); });
    if (turns % 2)
        std::swap(width_, height_);
    selection_.transform(false, false, turns);
    invalidate();
}

void Document::flip(bool horizontal)
{
    forEachRaster([&](Image& image, bool) { image = ops::flip(image, horizontal); });
    selection_.transform(horizontal, !horizontal, 0);
    invalidate();
}

// ----------------------------------------------------------------- state

void Document::snapshotLayer(const Layer& layer, DocumentState::LayerState& out) const
{
    out.image = layer.image;
    out.name = layer.name;
    out.visible = layer.visible;
    out.opacity = layer.opacity;
    out.mode = layer.mode;
    out.uid = layer.uid();
    out.group = layer.isGroup();
    out.expanded = layer.expanded;
    out.hasMask = layer.hasMask();
    out.maskEnabled = layer.maskEnabled();
    out.maskSelected = layer.maskSelected();
    if (layer.maskLayer()) {
        out.maskUid = layer.maskLayer()->uid();
        out.mask = layer.maskLayer()->image;
    }
    out.children.resize(layer.children_.size());
    for (size_t i = 0; i < layer.children_.size(); ++i)
        snapshotLayer(*layer.children_[i], out.children[i]);
}

DocumentState Document::snapshot() const
{
    DocumentState state;
    state.width = width_;
    state.height = height_;
    state.resolution = resolution_;
    state.active = activeIndex();
    state.layers.resize(root_->children_.size());
    for (size_t i = 0; i < root_->children_.size(); ++i)
        snapshotLayer(*root_->children_[i], state.layers[i]);
    if (const Mask* mask = selection_.clipMask()) {
        state.hasSelection = true;
        state.selection = *mask;
    }
    return state;
}

std::unique_ptr<Layer> Document::restoreLayer(const DocumentState::LayerState& state)
{
    auto layer = std::make_unique<Layer>(state.image, state.name);
    layer->visible = state.visible;
    layer->opacity = state.opacity;
    layer->mode = state.mode;
    layer->expanded = state.expanded;
    layer->group_ = state.group;
    layer->setUid(state.uid);
    uidCounter_ = std::max(uidCounter_, state.uid);
    if (state.hasMask) {
        auto mask = std::make_unique<Layer>(state.mask, QStringLiteral("Mask"));
        mask->setUid(state.maskUid);
        mask->parent_ = layer.get();
        uidCounter_ = std::max(uidCounter_, state.maskUid);
        layer->mask_ = std::move(mask);
        layer->maskEnabled_ = state.maskEnabled;
        layer->maskSelected_ = state.maskSelected;
    }
    for (const DocumentState::LayerState& childState : state.children) {
        auto child = restoreLayer(childState);
        child->parent_ = layer.get();
        layer->children_.push_back(std::move(child));
    }
    return layer;
}

void Document::restore(const DocumentState& state)
{
    width_ = state.width;
    height_ = state.height;
    resolution_ = units::sanitize(state.resolution);
    root_->children_.clear();
    for (const DocumentState::LayerState& ls : state.layers) {
        auto layer = restoreLayer(ls);
        layer->parent_ = root_.get();
        root_->children_.push_back(std::move(layer));
    }
    markStructureChanged();
    setActiveIndex(state.active);
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

} // namespace tessel
