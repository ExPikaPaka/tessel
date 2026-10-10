#include "core/Io.h"

#include <cmath>
#include <optional>

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "core/ImageOps.h"
#include "core/Units.h"
#include "core/Zip.h"

namespace tessel::io {

namespace {

struct OraEntry {
    QString src;
    QString name;
    int x = 0;
    int y = 0;
    bool visible = true;
    uint8_t opacity = 255;
    BlendMode mode = BlendMode::Normal;
};

QString blendToOra(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Multiply: return QStringLiteral("svg:multiply");
    case BlendMode::Screen: return QStringLiteral("svg:screen");
    case BlendMode::Overlay: return QStringLiteral("svg:overlay");
    case BlendMode::Darken: return QStringLiteral("svg:darken");
    case BlendMode::Lighten: return QStringLiteral("svg:lighten");
    case BlendMode::ColorDodge: return QStringLiteral("svg:color-dodge");
    case BlendMode::ColorBurn: return QStringLiteral("svg:color-burn");
    case BlendMode::Difference: return QStringLiteral("svg:difference");
    case BlendMode::Additive: return QStringLiteral("svg:plus");
    default: return QStringLiteral("svg:src-over");
    }
}

BlendMode blendFromOra(const QString& op)
{
    static const QHash<QString, BlendMode> table = {
        {QStringLiteral("svg:src-over"), BlendMode::Normal},
        {QStringLiteral("svg:dst-over"), BlendMode::Normal},
        {QStringLiteral("svg:multiply"), BlendMode::Multiply},
        {QStringLiteral("svg:screen"), BlendMode::Screen},
        {QStringLiteral("svg:overlay"), BlendMode::Overlay},
        {QStringLiteral("svg:darken"), BlendMode::Darken},
        {QStringLiteral("svg:lighten"), BlendMode::Lighten},
        {QStringLiteral("svg:color-dodge"), BlendMode::ColorDodge},
        {QStringLiteral("svg:color-burn"), BlendMode::ColorBurn},
        {QStringLiteral("svg:difference"), BlendMode::Difference},
        {QStringLiteral("svg:plus"), BlendMode::Additive},
        {QStringLiteral("svg:add"), BlendMode::Additive},
        {QStringLiteral("svg:hard-light"), BlendMode::Overlay},
        {QStringLiteral("svg:soft-light"), BlendMode::Overlay},
        {QStringLiteral("svg:exclusion"), BlendMode::Difference},
    };
    return table.value(op, BlendMode::Normal);
}

QByteArray encodePng(const Image& image)
{
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "PNG");
    writer.write(toQImage(image));
    return out;
}

// Qt stamps every QImage with the platform's default density, so a value on its
// own cannot tell a file that carried a pHYs chunk (or a JPEG density) from one
// that did not. A blank image of the same format reports that default, so any
// difference is the decoder having overwritten it.
std::optional<double> resolutionOf(const QImage& image)
{
    if (image.isNull())
        return std::nullopt;
    const QImage blank(1, 1, image.format());
    if (image.dotsPerMeterX() == blank.dotsPerMeterX())
        return std::nullopt;
    return units::sanitize(
        units::resolutionFromDotsPerMetre(image.dotsPerMeterX()));
}

// Stamps the document resolution onto an image about to be written. PNG turns
// this into pHYs and JPEG into its density fields; formats that cannot carry a
// density ignore it.
void applyResolution(QImage& image, double resolution)
{
    const int dpm = int(std::lround(units::dotsPerMetre(resolution)));
    image.setDotsPerMeterX(dpm);
    image.setDotsPerMeterY(dpm);
}

Image decodeImageBytes(const QByteArray& data, const char* format = nullptr)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format ? QByteArray(format) : QByteArray());
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    return fromQImage(image);
}

void collectOraLayers(QXmlStreamReader& xml, std::vector<OraEntry>& out, int ox,
                      int oy, int depth)
{
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == QLatin1String("stack"))
            return;
        if (!xml.isStartElement())
            continue;
        const QXmlStreamAttributes attrs = xml.attributes();
        const int x = ox + static_cast<int>(attrs.value(QLatin1String("x")).toDouble());
        const int y = oy + static_cast<int>(attrs.value(QLatin1String("y")).toDouble());
        if (xml.name() == QLatin1String("stack")) {
            if (depth < 8)
                collectOraLayers(xml, out, x, y, depth + 1);
            continue;
        }
        if (xml.name() != QLatin1String("layer"))
            continue;
        OraEntry entry;
        entry.src = attrs.value(QLatin1String("src")).toString();
        if (entry.src.isEmpty())
            continue;
        entry.name = attrs.value(QLatin1String("name")).toString();
        if (entry.name.isEmpty())
            entry.name = QStringLiteral("Layer");
        entry.x = x;
        entry.y = y;
        entry.visible = attrs.value(QLatin1String("visibility")) != QLatin1String("hidden");
        const double opacity = attrs.hasAttribute(QLatin1String("opacity"))
            ? attrs.value(QLatin1String("opacity")).toDouble()
            : 1.0;
        entry.opacity = static_cast<uint8_t>(std::clamp(int(std::lround(opacity * 255.0)), 0, 255));
        entry.mode = blendFromOra(attrs.value(QLatin1String("composite-op")).toString());
        out.push_back(entry);
    }
}

std::unique_ptr<Document> readFlat(const QString& path, QString* error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        if (error)
            *error = QStringLiteral("%1 is not an image Tessel can read (%2).")
                         .arg(QFileInfo(path).fileName(), reader.errorString());
        return nullptr;
    }
    const int frames = reader.imageCount();
    std::vector<QImage> images;
    QImage first = reader.read();
    if (first.isNull()) {
        if (error)
            *error = QStringLiteral("Could not decode %1: %2")
                         .arg(QFileInfo(path).fileName(), reader.errorString());
        return nullptr;
    }
    images.push_back(first);
    if (frames > 1 && reader.supportsAnimation()) {
        for (int i = 1; i < frames && i < 256; ++i) {
            if (!reader.jumpToImage(i))
                break;
            const QImage frame = reader.read();
            if (frame.isNull())
                break;
            images.push_back(frame);
        }
    }

    const int width = first.width();
    const int height = first.height();
    auto doc = std::make_unique<Document>(width, height);
    doc->takeLayer(0); // drop the placeholder background
    for (size_t i = 0; i < images.size(); ++i) {
        Image pixels = fromQImage(images[i]);
        if (pixels.width() != width || pixels.height() != height) {
            Image sized(width, height);
            sized.blit(pixels, 0, 0);
            pixels = std::move(sized);
        }
        const QString name = i == 0 ? QStringLiteral("Background")
                                    : QStringLiteral("Frame %1").arg(i + 1);
        auto layer = std::make_unique<Layer>(std::move(pixels), name);
        layer->visible = (i == 0);
        doc->addLayer(std::move(layer), static_cast<int>(i));
    }
    if (doc->layerCount() == 0) {
        if (error)
            *error = QStringLiteral("The file contains no usable image data.");
        return nullptr;
    }
    doc->layerAt(0)->visible = true;
    doc->setActiveIndex(0);
    if (const std::optional<double> dpi = resolutionOf(first))
        doc->setResolution(*dpi);
    return doc;
}

std::unique_ptr<Document> readTessel(const QString& path, QString* error)
{
    ZipReader zip(path);
    if (!zip.isValid()) {
        if (error)
            *error = QStringLiteral("Not a Tessel image: %1").arg(zip.errorString());
        return nullptr;
    }
    const QByteArray manifestBytes = zip.read(QStringLiteral("document.json"));
    QJsonParseError parseError{};
    const QJsonDocument manifest = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (!manifest.isObject()) {
        if (error)
            *error = QStringLiteral("Damaged Tessel image: %1").arg(parseError.errorString());
        return nullptr;
    }
    const QJsonObject root = manifest.object();
    const int width = root.value(QStringLiteral("width")).toInt();
    const int height = root.value(QStringLiteral("height")).toInt();
    const QJsonArray layerArray = root.value(QStringLiteral("layers")).toArray();
    if (width <= 0 || height <= 0 || layerArray.isEmpty()) {
        if (error)
            *error = QStringLiteral("Damaged Tessel image: no layers.");
        return nullptr;
    }
    auto doc = std::make_unique<Document>(width, height);
    // A manifest written before resolutions existed leaves the default alone.
    doc->setResolution(units::sanitize(
        root.value(QStringLiteral("resolution")).toDouble(units::kDefaultResolution)));
    doc->takeLayer(0); // drop the placeholder background
    int index = 0;
    for (const QJsonValue& value : layerArray) {
        const QJsonObject entry = value.toObject();
        Image pixels = decodeImageBytes(
            zip.read(entry.value(QStringLiteral("src")).toString()), "PNG");
        if (pixels.isNull())
            pixels = Image(width, height);
        if (pixels.width() != width || pixels.height() != height) {
            Image sized(width, height);
            sized.blit(pixels, 0, 0);
            pixels = std::move(sized);
        }
        auto layer = std::make_unique<Layer>(
            std::move(pixels),
            entry.value(QStringLiteral("name")).toString(QStringLiteral("Layer")));
        layer->visible = entry.value(QStringLiteral("visible")).toBool(true);
        layer->opacity = static_cast<uint8_t>(
            std::clamp(entry.value(QStringLiteral("opacity")).toInt(255), 0, 255));
        layer->mode = blendModeFromName(
            entry.value(QStringLiteral("blendMode")).toString());
        doc->addLayer(std::move(layer), index++);
    }
    if (doc->layerCount() == 0) {
        if (error)
            *error = QStringLiteral("Damaged Tessel image: no layers could be read.");
        return nullptr;
    }
    doc->setActiveIndex(root.value(QStringLiteral("active")).toInt(doc->layerCount() - 1));
    if (zip.contains(QStringLiteral("selection.png"))) {
        const Image mask = decodeImageBytes(zip.read(QStringLiteral("selection.png")), "PNG");
        if (mask.width() == width && mask.height() == height) {
            Mask sel(width, height);
            bool partial = false;
            for (int y = 0; y < height; ++y) {
                const Rgba* src = mask.row(y);
                float* dst = sel.row(y);
                for (int x = 0; x < width; ++x) {
                    dst[x] = src[x].r * (1.0f / 255.0f);
                    if (dst[x] < 0.996f)
                        partial = true;
                }
            }
            if (partial)
                doc->selection().setMask(std::move(sel));
        }
    }
    return doc;
}

std::unique_ptr<Document> readOra(const QString& path, QString* error)
{
    ZipReader zip(path);
    if (!zip.isValid()) {
        if (error)
            *error = QStringLiteral("Not an OpenRaster image: %1").arg(zip.errorString());
        return nullptr;
    }
    const QByteArray stackBytes = zip.read(QStringLiteral("stack.xml"));
    if (stackBytes.isEmpty()) {
        if (error)
            *error = QStringLiteral("Damaged OpenRaster image: stack.xml is missing.");
        return nullptr;
    }
    QXmlStreamReader xml(stackBytes);
    int width = 0, height = 0;
    double resolution = units::kDefaultResolution;
    std::vector<OraEntry> entries;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == QLatin1String("image")) {
            const QXmlStreamAttributes imageAttrs = xml.attributes();
            width = imageAttrs.value(QLatin1String("w")).toInt();
            height = imageAttrs.value(QLatin1String("h")).toInt();
            // OpenRaster allows separate horizontal and vertical densities.
            // Tessel keeps a single value, so the horizontal one wins.
            bool ok = false;
            const double xres = imageAttrs.value(QLatin1String("xres")).toDouble(&ok);
            if (ok && xres > 0.0)
                resolution = units::sanitize(xres);
        } else if (xml.name() == QLatin1String("stack")) {
            collectOraLayers(xml, entries, 0, 0, 0);
        }
    }
    if (width <= 0 || height <= 0 || entries.empty()) {
        if (error)
            *error = QStringLiteral("Damaged OpenRaster image: no layers found.");
        return nullptr;
    }
    auto doc = std::make_unique<Document>(width, height);
    doc->setResolution(resolution);
    doc->takeLayer(0); // drop the placeholder background
    // stack.xml lists layers top first; Tessel stores them bottom first.
    int index = 0;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        Image tile = decodeImageBytes(zip.read(it->src), "PNG");
        Image pixels(width, height);
        if (!tile.isNull())
            pixels.blit(tile, it->x, it->y);
        auto layer = std::make_unique<Layer>(std::move(pixels), it->name);
        layer->visible = it->visible;
        layer->opacity = it->opacity;
        layer->mode = it->mode;
        doc->addLayer(std::move(layer), index++);
    }
    doc->setActiveIndex(doc->layerCount() - 1);
    return doc;
}

std::unique_ptr<Document> readPdn(const QString& path, QString* error)
{
    Q_UNUSED(path);
    if (error) {
        *error = QStringLiteral(
            "Paint.NET's .pdn format is an undocumented serialisation of .NET "
            "objects, so Tessel cannot read it.\n\n"
            "In Paint.NET, use Save As and pick OpenRaster (.ora) to keep the "
            "layers, or PNG to export the flattened image. Tessel reads both.");
    }
    return nullptr;
}

bool writeFlat(const Image& image, double resolution, const QString& path,
               QString* error, int quality)
{
    const QString extension = extensionOf(path);
    Image source = image;
    if (extension == QLatin1String("jpg") || extension == QLatin1String("jpeg")
        || extension == QLatin1String("bmp") || extension == QLatin1String("ppm")) {
        source = flattenOnColor(image, Rgba(255, 255, 255, 255));
    }
    QImage out = toQImage(source);
    if (extension == QLatin1String("jpg") || extension == QLatin1String("jpeg")
        || extension == QLatin1String("ppm")) {
        out = out.convertToFormat(QImage::Format_RGB888);
    } else if (extension == QLatin1String("bmp")) {
        out = out.convertToFormat(QImage::Format_RGB32);
    } else if (extension == QLatin1String("ico")) {
        const int side = std::min(256, std::max(out.width(), out.height()));
        out = out.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    // Last, because every conversion above hands back a fresh image.
    applyResolution(out, resolution);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Could not write %1: %2")
                         .arg(QFileInfo(path).fileName(), file.errorString());
        return false;
    }
    QImageWriter writer(&file, extension.toLatin1().toUpper());
    if (isLossyFormat(path))
        writer.setQuality(std::clamp(quality, 1, 100));
    else if (extension == QLatin1String("png"))
        writer.setCompression(6);
    if (!writer.write(out)) {
        if (error)
            *error = QStringLiteral("Could not write %1: %2")
                         .arg(QFileInfo(path).fileName(), writer.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Could not finish writing %1: %2")
                         .arg(QFileInfo(path).fileName(), file.errorString());
        return false;
    }
    return true;
}

// `previews` writes the merged image and the thumbnail, which only other
// programs and file browsers look at. The autosave path leaves them out: both
// are a full composite of the document, and a recovery copy is read back by
// Tessel alone.
bool writeTessel(const Document& doc, const QString& path, QString* error,
                 bool previews = true)
{
    const QString temp = path + QStringLiteral(".tesselpart");
    {
        ZipWriter zip(temp);
        if (!zip.isOpen()) {
            if (error)
                *error = QStringLiteral("Could not write %1: %2")
                             .arg(QFileInfo(path).fileName(), zip.errorString());
            return false;
        }
        zip.add(QStringLiteral("mimetype"), QByteArray(kNativeMime), false);

        QJsonObject root;
        root[QStringLiteral("format")] = QStringLiteral("tessel");
        root[QStringLiteral("version")] = kFormatVersion;
        root[QStringLiteral("width")] = doc.width();
        root[QStringLiteral("height")] = doc.height();
        root[QStringLiteral("resolution")] = doc.resolution();
        root[QStringLiteral("active")] = doc.activeIndex();
        QJsonArray layerArray;
        for (int i = 0; i < doc.layerCount(); ++i) {
            const Layer* layer = doc.layerAt(i);
            const QString src = QStringLiteral("layers/layer%1.png")
                                    .arg(i, 3, 10, QLatin1Char('0'));
            QJsonObject entry;
            entry[QStringLiteral("name")] = layer->name;
            entry[QStringLiteral("src")] = src;
            entry[QStringLiteral("visible")] = layer->visible;
            entry[QStringLiteral("opacity")] = int(layer->opacity);
            entry[QStringLiteral("blendMode")] = blendModeName(layer->mode);
            layerArray.append(entry);
            zip.add(src, encodePng(layer->image));
        }
        root[QStringLiteral("layers")] = layerArray;
        zip.add(QStringLiteral("document.json"),
                QJsonDocument(root).toJson(QJsonDocument::Indented));
        if (previews) {
            zip.add(QStringLiteral("mergedimage.png"), encodePng(doc.composite()));
            zip.add(QStringLiteral("thumbnail.png"), encodePng(doc.thumbnail(256)));
        }
        if (const Mask* mask = doc.selection().clipMask()) {
            Image grey(mask->width(), mask->height());
            for (int y = 0; y < mask->height(); ++y) {
                const float* src = mask->row(y);
                Rgba* dst = grey.row(y);
                for (int x = 0; x < mask->width(); ++x) {
                    const uint8_t v = toByte(src[x]);
                    dst[x] = Rgba(v, v, v, 255);
                }
            }
            zip.add(QStringLiteral("selection.png"), encodePng(grey));
        }
        if (!zip.close()) {
            if (error)
                *error = QStringLiteral("Could not finish writing %1.")
                             .arg(QFileInfo(path).fileName());
            QFile::remove(temp);
            return false;
        }
    }
    QFile::remove(path);
    if (!QFile::rename(temp, path)) {
        if (error)
            *error = QStringLiteral("Could not replace %1.").arg(QFileInfo(path).fileName());
        QFile::remove(temp);
        return false;
    }
    return true;
}

bool writeOra(const Document& doc, const QString& path, QString* error)
{
    const QString temp = path + QStringLiteral(".orapart");
    {
        ZipWriter zip(temp);
        if (!zip.isOpen()) {
            if (error)
                *error = QStringLiteral("Could not write %1: %2")
                             .arg(QFileInfo(path).fileName(), zip.errorString());
            return false;
        }
        zip.add(QStringLiteral("mimetype"), QByteArray(kOraMime), false);

        QByteArray stackXml;
        QXmlStreamWriter xml(&stackXml);
        xml.setAutoFormatting(true);
        xml.writeStartDocument();
        xml.writeStartElement(QStringLiteral("image"));
        xml.writeAttribute(QStringLiteral("w"), QString::number(doc.width()));
        xml.writeAttribute(QStringLiteral("h"), QString::number(doc.height()));
        xml.writeAttribute(QStringLiteral("version"), QStringLiteral("0.0.3"));
        // OpenRaster specifies xres / yres as whole pixels per inch.
        const QString density = QString::number(std::lround(doc.resolution()));
        xml.writeAttribute(QStringLiteral("xres"), density);
        xml.writeAttribute(QStringLiteral("yres"), density);
        xml.writeStartElement(QStringLiteral("stack"));
        int index = 0;
        for (int i = doc.layerCount() - 1; i >= 0; --i) {
            const Layer* layer = doc.layerAt(i);
            const QString src = QStringLiteral("data/layer%1.png")
                                    .arg(index++, 3, 10, QLatin1Char('0'));
            xml.writeStartElement(QStringLiteral("layer"));
            xml.writeAttribute(QStringLiteral("name"), layer->name);
            xml.writeAttribute(QStringLiteral("src"), src);
            xml.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
            xml.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
            xml.writeAttribute(QStringLiteral("opacity"),
                               QString::number(layer->opacity / 255.0, 'f', 4));
            xml.writeAttribute(QStringLiteral("visibility"),
                               layer->visible ? QStringLiteral("visible")
                                              : QStringLiteral("hidden"));
            xml.writeAttribute(QStringLiteral("composite-op"), blendToOra(layer->mode));
            xml.writeEndElement();
            zip.add(src, encodePng(layer->image));
        }
        xml.writeEndElement(); // stack
        xml.writeEndElement(); // image
        xml.writeEndDocument();

        zip.add(QStringLiteral("stack.xml"), stackXml);
        zip.add(QStringLiteral("mergedimage.png"), encodePng(doc.composite()));
        zip.add(QStringLiteral("Thumbnails/thumbnail.png"), encodePng(doc.thumbnail(256)));
        if (!zip.close()) {
            if (error)
                *error = QStringLiteral("Could not finish writing %1.")
                             .arg(QFileInfo(path).fileName());
            QFile::remove(temp);
            return false;
        }
    }
    QFile::remove(path);
    if (!QFile::rename(temp, path)) {
        if (error)
            *error = QStringLiteral("Could not replace %1.").arg(QFileInfo(path).fileName());
        QFile::remove(temp);
        return false;
    }
    return true;
}

} // namespace

// ----------------------------------------------------------------- public

Image fromQImage(const QImage& image)
{
    return Image::fromQImage(image);
}

QImage toQImage(const Image& image)
{
    return image.toQImage();
}

Image flattenOnColor(const Image& image, Rgba background)
{
    Image out(image.width(), image.height(), background);
    ops::imageOver(out, image, nullptr, BlendMode::Normal, out.rect());
    return out;
}

QString extensionOf(const QString& path)
{
    return QFileInfo(path).suffix().toLower();
}

bool isLayeredFormat(const QString& path)
{
    const QString extension = extensionOf(path);
    return extension == QLatin1String(kNativeExtension)
        || extension == QLatin1String("ora");
}

bool flattensLayers(const QString& path) { return !isLayeredFormat(path); }

bool isLossyFormat(const QString& path)
{
    static const QStringList lossy = {QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                      QStringLiteral("webp"), QStringLiteral("gif"),
                                      QStringLiteral("avif"), QStringLiteral("jp2")};
    return lossy.contains(extensionOf(path));
}

QStringList readableExtensions()
{
    QStringList out = {QStringLiteral("tessel"), QStringLiteral("ora")};
    for (const QByteArray& format : QImageReader::supportedImageFormats()) {
        const QString name = QString::fromLatin1(format).toLower();
        if (!out.contains(name))
            out.append(name);
    }
    return out;
}

QStringList writableExtensions()
{
    QStringList out = {QStringLiteral("tessel"), QStringLiteral("ora")};
    for (const QByteArray& format : QImageWriter::supportedImageFormats()) {
        const QString name = QString::fromLatin1(format).toLower();
        if (!out.contains(name))
            out.append(name);
    }
    return out;
}

bool canWrite(const QString& path)
{
    return writableExtensions().contains(extensionOf(path));
}

namespace {

QString patternsFor(const QStringList& extensions)
{
    QStringList patterns;
    for (const QString& extension : extensions)
        patterns.append(QStringLiteral("*.%1").arg(extension));
    return patterns.join(QLatin1Char(' '));
}

struct NamedFilter {
    const char* label;
    const char* extensions;
};

const std::vector<NamedFilter>& namedFilters()
{
    static const std::vector<NamedFilter> filters = {
        {"Tessel layered image", "tessel"},
        {"OpenRaster", "ora"},
        {"PNG image", "png"},
        {"JPEG image", "jpg jpeg"},
        {"Bitmap", "bmp"},
        {"TIFF image", "tif tiff"},
        {"WebP image", "webp"},
        {"Targa", "tga"},
        {"Windows icon", "ico"},
        {"GIF image", "gif"},
    };
    return filters;
}

} // namespace

QString openFilter()
{
    QStringList parts;
    parts.append(QStringLiteral("All supported images (%1)")
                     .arg(patternsFor(readableExtensions())));
    const QStringList readable = readableExtensions();
    for (const NamedFilter& filter : namedFilters()) {
        const QStringList extensions = QString::fromLatin1(filter.extensions)
                                           .split(QLatin1Char(' '));
        bool supported = false;
        for (const QString& extension : extensions)
            supported = supported || readable.contains(extension);
        if (supported)
            parts.append(QStringLiteral("%1 (%2)")
                             .arg(QLatin1String(filter.label), patternsFor(extensions)));
    }
    parts.append(QStringLiteral("Paint.NET image (*.pdn)"));
    parts.append(QStringLiteral("All files (*)"));
    return parts.join(QStringLiteral(";;"));
}

QString saveFilter()
{
    QStringList parts;
    const QStringList writable = writableExtensions();
    for (const NamedFilter& filter : namedFilters()) {
        const QStringList extensions = QString::fromLatin1(filter.extensions)
                                           .split(QLatin1Char(' '));
        bool supported = false;
        for (const QString& extension : extensions)
            supported = supported || writable.contains(extension);
        if (supported)
            parts.append(QStringLiteral("%1 (%2)")
                             .arg(QLatin1String(filter.label), patternsFor(extensions)));
    }
    return parts.join(QStringLiteral(";;"));
}

QString filterForExtension(const QString& extension)
{
    for (const NamedFilter& filter : namedFilters()) {
        const QStringList extensions = QString::fromLatin1(filter.extensions)
                                           .split(QLatin1Char(' '));
        if (extensions.contains(extension.toLower()))
            return QStringLiteral("%1 (%2)")
                .arg(QLatin1String(filter.label), patternsFor(extensions));
    }
    return QString();
}

std::unique_ptr<Document> openDocument(const QString& path, QString* error)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        if (error)
            *error = QStringLiteral("%1 does not exist.").arg(info.fileName());
        return nullptr;
    }
    const QString extension = info.suffix().toLower();
    std::unique_ptr<Document> doc;
    if (extension == QLatin1String(kNativeExtension))
        doc = readTessel(path, error);
    else if (extension == QLatin1String("ora"))
        doc = readOra(path, error);
    else if (extension == QLatin1String("pdn"))
        doc = readPdn(path, error);
    else
        doc = readFlat(path, error);
    if (!doc)
        return nullptr;
    doc->setFilePath(path);
    doc->setTitle(info.fileName());
    doc->setModified(false);
    return doc;
}

Image openFlattened(const QString& path, QString* error)
{
    std::unique_ptr<Document> source = openDocument(path, error);
    if (!source)
        return Image();
    return source->composite();
}

std::unique_ptr<Layer> openAsLayer(const Document& doc, const QString& path,
                                   QString* error)
{
    std::unique_ptr<Document> source = openDocument(path, error);
    if (!source)
        return nullptr;
    Image pixels = source->composite();
    if (pixels.width() != doc.width() || pixels.height() != doc.height()) {
        Image sized(doc.width(), doc.height());
        sized.blit(pixels, 0, 0);
        pixels = std::move(sized);
    }
    return std::make_unique<Layer>(std::move(pixels), QFileInfo(path).completeBaseName());
}

bool saveRecoveryCopy(const Document& doc, const QString& path, QString* error)
{
    return writeTessel(doc, path, error, false);
}

bool saveDocument(const Document& doc, const QString& path, QString* error,
                  int quality)
{
    const QString extension = extensionOf(path);
    if (extension == QLatin1String(kNativeExtension))
        return writeTessel(doc, path, error);
    if (extension == QLatin1String("ora"))
        return writeOra(doc, path, error);
    if (!canWrite(path)) {
        if (error)
            *error = QStringLiteral("Tessel cannot write '.%1' files.").arg(extension);
        return false;
    }
    return writeFlat(doc.composite(), doc.resolution(), path, error, quality);
}

} // namespace tessel::io
