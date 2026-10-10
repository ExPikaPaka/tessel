#include "core/Palette.h"

#include <algorithm>

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace tessel {

namespace {

uint8_t clampByte(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

bool isHexDigits(QStringView text)
{
    for (QChar c : text) {
        const char16_t u = c.unicode();
        const bool digit = (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f')
            || (u >= u'A' && u <= u'F');
        if (!digit)
            return false;
    }
    return !text.isEmpty();
}

// Splits on newlines after folding carriage returns away, so files written on
// any platform read the same.
QStringList splitLines(const QString& text)
{
    QString normalised = text;
    normalised.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return normalised.split(QLatin1Char('\n'));
}

// Strips a `Prefix:` label and hands back what follows, or a null string when
// the line does not carry that label.
QString afterLabel(const QString& line, QLatin1String label)
{
    if (!line.startsWith(label, Qt::CaseInsensitive))
        return QString();
    return line.mid(label.size()).trimmed();
}

} // namespace

void Palette::setColumns(int columns)
{
    columns_ = std::clamp(columns, 1, 64);
}

void Palette::append(Rgba color)
{
    if (colors_.size() < kMaxColors)
        colors_.append(color);
}

void Palette::clear()
{
    colors_.clear();
    name_.clear();
}

bool Palette::parseHexColor(QStringView text, Rgba& out)
{
    QStringView digits = text.trimmed();
    if (digits.startsWith(u'#'))
        digits = digits.sliced(1);
    if (digits.size() != 6 && digits.size() != 8)
        return false;
    if (!isHexDigits(digits))
        return false;
    bool ok = false;
    const uint value = digits.toString().toUInt(&ok, 16);
    if (!ok)
        return false;
    if (digits.size() == 6) {
        out = Rgba(uint8_t((value >> 16) & 0xff), uint8_t((value >> 8) & 0xff),
                   uint8_t(value & 0xff), 255);
    } else {
        out = Rgba(uint8_t((value >> 24) & 0xff), uint8_t((value >> 16) & 0xff),
                   uint8_t((value >> 8) & 0xff), uint8_t(value & 0xff));
    }
    return true;
}

QString Palette::toHexString(Rgba color)
{
    // Alpha is only written when it carries information, which keeps the common
    // case readable and compatible with other tools.
    QString out = QStringLiteral("#%1%2%3")
                      .arg(color.r, 2, 16, QLatin1Char('0'))
                      .arg(color.g, 2, 16, QLatin1Char('0'))
                      .arg(color.b, 2, 16, QLatin1Char('0'));
    if (color.a != 255)
        out += QStringLiteral("%1").arg(color.a, 2, 16, QLatin1Char('0'));
    return out.toUpper();
}

Palette Palette::parseGpl(const QString& text)
{
    Palette out;
    bool sawHeader = false;
    for (const QString& raw : splitLines(text)) {
        if (out.colors_.size() >= kMaxColors)
            break;
        // simplified() folds tabs and runs of spaces into single spaces, which
        // is what separates the three channel columns in practice.
        const QString line = raw.simplified();
        if (line.isEmpty())
            continue;
        if (!sawHeader
            && line.startsWith(QLatin1String("GIMP Palette"), Qt::CaseInsensitive)) {
            sawHeader = true;
            continue;
        }
        if (line.startsWith(QLatin1Char('#')))
            continue;
        const QString name = afterLabel(line, QLatin1String("Name:"));
        if (!name.isNull()) {
            out.name_ = name;
            continue;
        }
        const QString columns = afterLabel(line, QLatin1String("Columns:"));
        if (!columns.isNull()) {
            bool ok = false;
            const int value = columns.toInt(&ok);
            if (ok)
                out.setColumns(value);
            continue;
        }
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 3)
            continue;
        bool okR = false, okG = false, okB = false;
        const int r = parts[0].toInt(&okR);
        const int g = parts[1].toInt(&okG);
        const int b = parts[2].toInt(&okB);
        if (!okR || !okG || !okB)
            continue;
        // Out of range channels are clamped rather than dropped: the entry is
        // clearly meant to be a colour, it is just written badly.
        out.colors_.append(Rgba(clampByte(r), clampByte(g), clampByte(b), 255));
    }
    return out;
}

Palette Palette::parseHex(const QString& text)
{
    Palette out;
    for (const QString& raw : splitLines(text)) {
        if (out.colors_.size() >= kMaxColors)
            break;
        const QString line = raw.simplified();
        if (line.isEmpty())
            continue;
        // A colour and a comment both start with a hash, so the digits decide
        // which one this is. `# Name: ...` is how toHex() keeps the name.
        if (line.startsWith(QLatin1Char('#'))) {
            const QString name = afterLabel(line.mid(1).trimmed(),
                                            QLatin1String("Name:"));
            if (!name.isNull()) {
                out.name_ = name;
                continue;
            }
        }
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        Rgba color;
        if (!parts.isEmpty() && parseHexColor(parts.front(), color))
            out.colors_.append(color);
    }
    return out;
}

QString Palette::toGpl() const
{
    // The GIMP format carries no alpha, so translucent entries are written as
    // their opaque colour and the name column keeps the full hex for readers
    // that show entry names.
    QString out = QStringLiteral("GIMP Palette\n");
    out += QStringLiteral("Name: %1\n")
               .arg(name_.isEmpty() ? QStringLiteral("Tessel") : name_);
    out += QStringLiteral("Columns: %1\n#\n").arg(columns_);
    for (Rgba color : colors_) {
        out += QStringLiteral("%1 %2 %3\t%4\n")
                   .arg(color.r, 3)
                   .arg(color.g, 3)
                   .arg(color.b, 3)
                   .arg(toHexString(color));
    }
    return out;
}

QString Palette::toHex() const
{
    QString out;
    if (!name_.isEmpty())
        out += QStringLiteral("# Name: %1\n").arg(name_);
    for (Rgba color : colors_)
        out += toHexString(color) + QLatin1Char('\n');
    return out;
}

Palette::Format Palette::formatForPath(const QString& path)
{
    return path.endsWith(QLatin1String(".gpl"), Qt::CaseInsensitive) ? Format::Gpl
                                                                     : Format::Hex;
}

Palette Palette::load(const QString& path, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return Palette();
    };
    if (error)
        error->clear();

    const QString shown = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot read %1: %2").arg(shown, file.errorString()));
    if (file.size() > kMaxFileBytes) {
        return fail(QStringLiteral("%1 is too large to be a palette (%2 MB).")
                        .arg(shown)
                        .arg(file.size() / (1024 * 1024)));
    }
    // Reading with a limit also covers pipes and character devices, whose
    // reported size is zero but whose contents may never end.
    const QByteArray bytes = file.read(kMaxFileBytes);
    const QString text = QString::fromUtf8(bytes);

    // The header wins over the extension, so a GIMP palette saved under another
    // name still reads correctly.
    const bool gpl = text.trimmed().startsWith(QLatin1String("GIMP Palette"),
                                               Qt::CaseInsensitive)
        || formatForPath(path) == Format::Gpl;
    Palette out = gpl ? parseGpl(text) : parseHex(text);
    if (out.isEmpty())
        return fail(QStringLiteral("No colours found in %1.").arg(shown));
    if (out.name_.isEmpty())
        out.name_ = QFileInfo(path).completeBaseName();
    return out;
}

bool Palette::save(const QString& path, QString* error) const
{
    if (error)
        error->clear();
    const auto fail = [error, &path](const QString& reason) {
        if (error) {
            *error = QStringLiteral("Cannot write %1: %2")
                         .arg(QFileInfo(path).fileName(), reason);
        }
        return false;
    };
    const QString text = formatForPath(path) == Format::Gpl ? toGpl() : toHex();
    // QSaveFile keeps a failed write from truncating an existing palette.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(file.errorString());
    const QByteArray bytes = text.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return fail(file.errorString());
    return true;
}

QString Palette::openFilter()
{
    return QStringLiteral("Palettes (*.gpl *.txt *.hex);;GIMP palette (*.gpl);;"
                          "Hex list (*.txt *.hex);;All files (*)");
}

QString Palette::saveFilter()
{
    return QStringLiteral("GIMP palette (*.gpl);;Hex list (*.txt);;All files (*)");
}

} // namespace tessel
