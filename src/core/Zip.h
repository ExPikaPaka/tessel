#pragma once

#include <vector>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>

namespace tessel {

// Minimal zip support, enough for the layered image containers (.tessel and
// .ora). Only the store and deflate methods are used, which is all those
// formats need.
class ZipWriter {
public:
    explicit ZipWriter(const QString& path);
    ~ZipWriter();

    bool isOpen() const { return file_.isOpen(); }
    bool add(const QString& name, const QByteArray& data, bool compress = true);
    bool close();
    QString errorString() const { return error_; }

private:
    struct Entry {
        QByteArray name;
        quint32 crc = 0;
        quint32 compressedSize = 0;
        quint32 uncompressedSize = 0;
        quint32 offset = 0;
        quint16 method = 0;
    };

    QFile file_;
    std::vector<Entry> entries_;
    QString error_;
    bool closed_ = false;
};

class ZipReader {
public:
    explicit ZipReader(const QString& path);

    bool isValid() const { return valid_; }
    bool contains(const QString& name) const;
    QByteArray read(const QString& name) const;
    QStringList names() const;
    QString errorString() const { return error_; }

private:
    struct Entry {
        QString name;
        quint16 method = 0;
        quint32 compressedSize = 0;
        quint32 uncompressedSize = 0;
        quint32 localOffset = 0;
    };

    bool parse();

    QString path_;
    QByteArray blob_;
    std::vector<Entry> entries_;
    bool valid_ = false;
    QString error_;
};

QByteArray rawDeflate(const QByteArray& data, int level = 6);
QByteArray rawInflate(const QByteArray& data, int expectedSize);

} // namespace tessel
