#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include "core/Image.h"

namespace tessel {

// A named list of colours with readers and writers for the GIMP palette format
// (.gpl) and a plain list of hex triplets. Nothing here touches the interface,
// so the parsers can be exercised by the headless tests.
//
// Every reader is written to survive whatever a file happens to contain: lines
// that do not look like colours are skipped rather than rejected, so a single
// bad line never costs the rest of the palette.
class Palette {
public:
    enum class Format { Gpl, Hex };

    // Parsing stops at this many entries. No hand made palette comes close, and
    // the limit keeps a corrupt or hostile file from exhausting memory.
    static constexpr int kMaxColors = 65536;
    // Reading stops after this many bytes, which also bounds the work when the
    // path turns out to be an endless special file such as /dev/zero.
    static constexpr qint64 kMaxFileBytes = 16 * 1024 * 1024;

    Palette() = default;
    explicit Palette(QVector<Rgba> colors, QString name = QString())
        : colors_(std::move(colors)), name_(std::move(name)) {}

    const QVector<Rgba>& colors() const { return colors_; }
    void setColors(QVector<Rgba> colors) { colors_ = std::move(colors); }
    const QString& name() const { return name_; }
    void setName(const QString& name) { name_ = name; }
    int columns() const { return columns_; }
    void setColumns(int columns);

    int size() const { return int(colors_.size()); }
    bool isEmpty() const { return colors_.isEmpty(); }
    void append(Rgba color);
    void clear();

    // Text conversion. The parsers never fail: an unreadable text yields an
    // empty palette, which the callers report as "no colours found".
    static Palette parseGpl(const QString& text);
    static Palette parseHex(const QString& text);
    QString toGpl() const;
    QString toHex() const;

    // `#RRGGBB` or `#RRGGBBAA`, with the hash optional on input.
    static bool parseHexColor(QStringView text, Rgba& out);
    static QString toHexString(Rgba color);

    // Picks the format from the file extension; anything but .gpl is read and
    // written as the hex list.
    static Format formatForPath(const QString& path);

    // Returns an empty palette and fills `error` when the file cannot be read
    // or holds no colours.
    static Palette load(const QString& path, QString* error = nullptr);
    bool save(const QString& path, QString* error = nullptr) const;

    static QString openFilter();
    static QString saveFilter();

private:
    QVector<Rgba> colors_;
    QString name_;
    int columns_ = 16;
};

} // namespace tessel
