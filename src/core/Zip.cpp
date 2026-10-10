#include "core/Zip.h"

#include <cstring>

#include <QCoreApplication>

#include <zlib.h>

namespace tessel {

namespace {

constexpr quint32 kLocalSig = 0x04034b50;
constexpr quint32 kCentralSig = 0x02014b50;
constexpr quint32 kEndSig = 0x06054b50;

void put16(QByteArray& out, quint16 value)
{
    out.append(static_cast<char>(value & 0xff));
    out.append(static_cast<char>((value >> 8) & 0xff));
}

void put32(QByteArray& out, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xff));
}

quint16 get16(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 2 > data.size())
        return 0;
    return static_cast<quint16>(static_cast<quint8>(data[offset]))
        | static_cast<quint16>(static_cast<quint8>(data[offset + 1])) << 8;
}

quint32 get32(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 4 > data.size())
        return 0;
    quint32 value = 0;
    for (int i = 0; i < 4; ++i)
        value |= static_cast<quint32>(static_cast<quint8>(data[offset + i])) << (8 * i);
    return value;
}

} // namespace

QByteArray rawDeflate(const QByteArray& data, int level)
{
    z_stream stream{};
    if (deflateInit2(&stream, level, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return QByteArray();
    QByteArray out;
    out.resize(static_cast<int>(deflateBound(&stream, static_cast<uLong>(data.size()))) + 64);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const int written = static_cast<int>(stream.total_out);
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        return QByteArray();
    out.resize(written);
    return out;
}

QByteArray rawInflate(const QByteArray& data, int expectedSize)
{
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return QByteArray();
    QByteArray out;
    out.resize(std::max(expectedSize, 1));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    stream.avail_in = static_cast<uInt>(data.size());

    int total = 0;
    int result = Z_OK;
    while (true) {
        if (total >= out.size())
            out.resize(out.size() * 2 + 1024);
        stream.next_out = reinterpret_cast<Bytef*>(out.data() + total);
        stream.avail_out = static_cast<uInt>(out.size() - total);
        result = inflate(&stream, Z_NO_FLUSH);
        total = static_cast<int>(stream.total_out);
        if (result == Z_STREAM_END)
            break;
        if (result != Z_OK) {
            inflateEnd(&stream);
            return QByteArray();
        }
        if (stream.avail_in == 0 && stream.avail_out > 0)
            break;
    }
    inflateEnd(&stream);
    out.resize(total);
    return out;
}

// ----------------------------------------------------------------- writer

ZipWriter::ZipWriter(const QString& path) : file_(path)
{
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate))
        error_ = file_.errorString();
}

ZipWriter::~ZipWriter()
{
    if (!closed_)
        close();
}

bool ZipWriter::add(const QString& name, const QByteArray& data, bool compress)
{
    if (!file_.isOpen())
        return false;

    Entry entry;
    entry.name = name.toUtf8();
    entry.crc = static_cast<quint32>(
        crc32(0, reinterpret_cast<const Bytef*>(data.constData()),
              static_cast<uInt>(data.size())));
    entry.uncompressedSize = static_cast<quint32>(data.size());
    entry.offset = static_cast<quint32>(file_.pos());

    QByteArray payload = data;
    entry.method = 0;
    if (compress && data.size() > 64) {
        const QByteArray deflated = rawDeflate(data);
        if (!deflated.isEmpty() && deflated.size() < data.size()) {
            payload = deflated;
            entry.method = 8;
        }
    }
    entry.compressedSize = static_cast<quint32>(payload.size());

    QByteArray header;
    put32(header, kLocalSig);
    put16(header, 20);            // version needed
    put16(header, 0);             // flags
    put16(header, entry.method);
    put16(header, 0);             // mod time
    put16(header, 0);             // mod date
    put32(header, entry.crc);
    put32(header, entry.compressedSize);
    put32(header, entry.uncompressedSize);
    put16(header, static_cast<quint16>(entry.name.size()));
    put16(header, 0);             // extra length
    header.append(entry.name);

    if (file_.write(header) != header.size())
        return false;
    if (!payload.isEmpty() && file_.write(payload) != payload.size())
        return false;
    entries_.push_back(entry);
    return true;
}

bool ZipWriter::close()
{
    if (closed_)
        return true;
    closed_ = true;
    if (!file_.isOpen())
        return false;

    const quint32 centralOffset = static_cast<quint32>(file_.pos());
    QByteArray central;
    for (const Entry& entry : entries_) {
        put32(central, kCentralSig);
        put16(central, 20);       // version made by
        put16(central, 20);       // version needed
        put16(central, 0);        // flags
        put16(central, entry.method);
        put16(central, 0);
        put16(central, 0);
        put32(central, entry.crc);
        put32(central, entry.compressedSize);
        put32(central, entry.uncompressedSize);
        put16(central, static_cast<quint16>(entry.name.size()));
        put16(central, 0);        // extra
        put16(central, 0);        // comment
        put16(central, 0);        // disk number
        put16(central, 0);        // internal attrs
        put32(central, 0);        // external attrs
        put32(central, entry.offset);
        central.append(entry.name);
    }
    QByteArray end;
    put32(end, kEndSig);
    put16(end, 0);
    put16(end, 0);
    put16(end, static_cast<quint16>(entries_.size()));
    put16(end, static_cast<quint16>(entries_.size()));
    put32(end, static_cast<quint32>(central.size()));
    put32(end, centralOffset);
    put16(end, 0);

    const bool ok = file_.write(central) == central.size()
        && file_.write(end) == end.size();
    file_.flush();
    file_.close();
    return ok;
}

// ----------------------------------------------------------------- reader

ZipReader::ZipReader(const QString& path) : path_(path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error_ = file.errorString();
        return;
    }
    blob_ = file.readAll();
    file.close();
    valid_ = parse();
}

bool ZipReader::parse()
{
    // Find the end of central directory record, scanning back over any comment.
    int eocd = -1;
    const int limit = static_cast<int>(std::max<qsizetype>(0, blob_.size() - 66000));
    for (int i = blob_.size() - 22; i >= limit; --i) {
        if (get32(blob_, i) == kEndSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        error_ = QCoreApplication::translate("Core", "not a zip archive");
        return false;
    }
    const int count = get16(blob_, eocd + 10);
    int offset = static_cast<int>(get32(blob_, eocd + 16));
    for (int i = 0; i < count; ++i) {
        if (get32(blob_, offset) != kCentralSig) {
            error_ = QCoreApplication::translate("Core", "damaged central directory");
            return false;
        }
        Entry entry;
        entry.method = get16(blob_, offset + 10);
        entry.compressedSize = get32(blob_, offset + 20);
        entry.uncompressedSize = get32(blob_, offset + 24);
        const int nameLen = get16(blob_, offset + 28);
        const int extraLen = get16(blob_, offset + 30);
        const int commentLen = get16(blob_, offset + 32);
        entry.localOffset = get32(blob_, offset + 42);
        entry.name = QString::fromUtf8(blob_.mid(offset + 46, nameLen));
        entries_.push_back(entry);
        offset += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

bool ZipReader::contains(const QString& name) const
{
    for (const Entry& entry : entries_) {
        if (entry.name == name)
            return true;
    }
    return false;
}

QStringList ZipReader::names() const
{
    QStringList out;
    out.reserve(static_cast<int>(entries_.size()));
    for (const Entry& entry : entries_)
        out.append(entry.name);
    return out;
}

QByteArray ZipReader::read(const QString& name) const
{
    for (const Entry& entry : entries_) {
        if (entry.name != name)
            continue;
        const int base = static_cast<int>(entry.localOffset);
        if (get32(blob_, base) != kLocalSig)
            return QByteArray();
        const int nameLen = get16(blob_, base + 26);
        const int extraLen = get16(blob_, base + 28);
        const int dataStart = base + 30 + nameLen + extraLen;
        const QByteArray payload = blob_.mid(dataStart,
                                             static_cast<int>(entry.compressedSize));
        if (entry.method == 0)
            return payload;
        if (entry.method == 8)
            return rawInflate(payload, static_cast<int>(entry.uncompressedSize));
        return QByteArray();
    }
    return QByteArray();
}

} // namespace tessel
