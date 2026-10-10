#pragma once

#include <memory>

#include <QString>
#include <QStringList>

#include "core/Document.h"

namespace tessel::io {

constexpr const char* kNativeExtension = "tessel";
constexpr const char* kNativeMime = "image/x-tessel";
constexpr const char* kOraMime = "image/openraster";
constexpr int kFormatVersion = 1;

// Returns null and fills `error` when the file cannot be read.
std::unique_ptr<Document> openDocument(const QString& path, QString* error);
std::unique_ptr<Layer> openAsLayer(const Document& doc, const QString& path,
                                   QString* error);
// The flattened contents of a file at its own size, with nothing cropped away.
// Placing it is the caller's business.
Image openFlattened(const QString& path, QString* error);
bool saveDocument(const Document& doc, const QString& path, QString* error,
                  int quality = 92);

bool isLayeredFormat(const QString& path);
bool flattensLayers(const QString& path);
bool isLossyFormat(const QString& path);
bool canWrite(const QString& path);

QString openFilter();
QString saveFilter();
QString filterForExtension(const QString& extension);
QStringList readableExtensions();
QStringList writableExtensions();
QString extensionOf(const QString& path);

// Shared helpers also used by the clipboard and thumbnail code.
Image fromQImage(const QImage& image);
QImage toQImage(const Image& image);
Image flattenOnColor(const Image& image, Rgba background);

} // namespace tessel::io
