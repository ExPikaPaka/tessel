#include "core/Blend.h"

#include "core/Parallel.h"

namespace tessel {

const QStringList& blendModeNames()
{
    static const QStringList names = {
        QStringLiteral("Normal"),     QStringLiteral("Multiply"),
        QStringLiteral("Additive"),   QStringLiteral("Color Burn"),
        QStringLiteral("Color Dodge"), QStringLiteral("Reflect"),
        QStringLiteral("Glow"),       QStringLiteral("Overlay"),
        QStringLiteral("Difference"), QStringLiteral("Negation"),
        QStringLiteral("Lighten"),    QStringLiteral("Darken"),
        QStringLiteral("Screen"),     QStringLiteral("Xor"),
    };
    return names;
}

QString blendModeName(BlendMode mode)
{
    const int index = static_cast<int>(mode);
    const QStringList& names = blendModeNames();
    if (index < 0 || index >= names.size())
        return names.first();
    return names.at(index);
}

BlendMode blendModeFromName(const QString& name, BlendMode fallback)
{
    // Compared by hand rather than with the case insensitive overload of
    // QStringList::indexOf, which only exists in newer Qt releases.
    const QStringList& names = blendModeNames();
    for (int i = 0; i < names.size(); ++i) {
        if (names.at(i).compare(name, Qt::CaseInsensitive) == 0)
            return static_cast<BlendMode>(i);
    }
    return fallback;
}

void compositeRegion(const std::vector<CompositeLayer>& layers, Image& out,
                     const Rect& region)
{
    const Rect area = region.intersected(out.rect());
    if (area.isEmpty())
        return;

    // Collect the layers that actually contribute anything.
    std::vector<const CompositeLayer*> active;
    active.reserve(layers.size());
    for (const CompositeLayer& layer : layers) {
        if (layer.visible && layer.opacity > 0 && layer.image && !layer.image->isNull())
            active.push_back(&layer);
    }

    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            Rgba* dstRow = out.row(y) + area.x;
            std::fill(dstRow, dstRow + area.w, Rgba(0, 0, 0, 0));
            bool started = false;
            for (const CompositeLayer* layer : active) {
                const Image& img = *layer->image;
                if (y >= img.height())
                    continue;
                const int cols = std::min(area.w, img.width() - area.x);
                if (cols <= 0)
                    continue;
                const Rgba* srcRow = img.row(y) + area.x;
                const float scale = layer->opacity * (1.0f / 255.0f);
                if (!started && layer->mode == BlendMode::Normal) {
                    if (layer->opacity == 255) {
                        std::memcpy(dstRow, srcRow, static_cast<size_t>(cols) * sizeof(Rgba));
                    } else {
                        for (int x = 0; x < cols; ++x) {
                            Rgba c = srcRow[x];
                            c.a = toByte(c.a * (1.0f / 255.0f) * scale);
                            dstRow[x] = c;
                        }
                    }
                    started = true;
                    continue;
                }
                for (int x = 0; x < cols; ++x)
                    dstRow[x] = blendOver(dstRow[x], srcRow[x], layer->mode, scale);
                started = true;
            }
        }
    });
}

Image compositeLayers(const std::vector<CompositeLayer>& layers, int width, int height)
{
    Image out(width, height);
    compositeRegion(layers, out, out.rect());
    return out;
}

} // namespace tessel
