#include "core/Units.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tessel::units {

namespace {

// Trims the trailing zeros a fixed precision leaves behind, so a ruler reads
// "1" and "1.5" rather than "1.00" and "1.50".
QString trimmed(double value, int decimalPlaces)
{
    QString text = QString::number(value, 'f', decimalPlaces);
    if (!text.contains(QLatin1Char('.')))
        return text;
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    if (text == QLatin1String("-0"))
        text = QStringLiteral("0");
    return text;
}

// The candidate tick spacings for a physical unit, smallest first. Rulers walk
// the list and take the first entry that leaves room for a label.
const std::vector<double>& inchSteps()
{
    static const std::vector<double> steps = {
        1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0, 2.0, 5.0,
        10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
    return steps;
}

const std::vector<double>& centimetreSteps()
{
    static const std::vector<double> steps = {
        0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0,
        50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0};
    return steps;
}

// How many minor ticks subdivide one major tick. Halves and quarters of an
// inch are already fine enough on their own.
int inchSubdivisions(double step)
{
    if (step <= 0.25)
        return 1;
    if (step <= 1.0)
        return 4;
    return 2;
}

} // namespace

double sanitize(double resolution)
{
    if (!std::isfinite(resolution) || resolution < kMinResolution
        || resolution > kMaxResolution)
        return kDefaultResolution;
    return resolution;
}

double fromPixels(double pixels, Unit unit, double resolution, double reference)
{
    const double dpi = sanitize(resolution);
    switch (unit) {
    case Unit::Inches:
        return pixels / dpi;
    case Unit::Centimetres:
        return pixels / dpi * kCentimetresPerInch;
    case Unit::Percent:
        return reference > 0.0 ? pixels / reference * 100.0 : 100.0;
    case Unit::Pixels:
    default:
        return pixels;
    }
}

double toPixels(double value, Unit unit, double resolution, double reference)
{
    const double dpi = sanitize(resolution);
    switch (unit) {
    case Unit::Inches:
        return value * dpi;
    case Unit::Centimetres:
        return value / kCentimetresPerInch * dpi;
    case Unit::Percent:
        return reference > 0.0 ? reference * value / 100.0 : value;
    case Unit::Pixels:
    default:
        return value;
    }
}

double dotsPerMetre(double resolution)
{
    return sanitize(resolution) / kMetresPerInch;
}

double resolutionFromDotsPerMetre(double dpm)
{
    return sanitize(dpm * kMetresPerInch);
}

QString unitName(Unit unit)
{
    switch (unit) {
    case Unit::Inches: return QStringLiteral("Inches");
    case Unit::Centimetres: return QStringLiteral("Centimetres");
    case Unit::Percent: return QStringLiteral("Percent");
    case Unit::Pixels:
    default: return QStringLiteral("Pixels");
    }
}

QString unitAbbreviation(Unit unit)
{
    switch (unit) {
    case Unit::Inches: return QStringLiteral("in");
    case Unit::Centimetres: return QStringLiteral("cm");
    case Unit::Percent: return QStringLiteral("%");
    case Unit::Pixels:
    default: return QStringLiteral("px");
    }
}

QString unitSuffix(Unit unit)
{
    if (unit == Unit::Percent)
        return QStringLiteral("%");
    return QStringLiteral(" ") + unitAbbreviation(unit);
}

QString unitId(Unit unit)
{
    switch (unit) {
    case Unit::Inches: return QStringLiteral("inches");
    case Unit::Centimetres: return QStringLiteral("centimetres");
    case Unit::Percent: return QStringLiteral("percent");
    case Unit::Pixels:
    default: return QStringLiteral("pixels");
    }
}

Unit unitFromId(const QString& id)
{
    const QString key = id.trimmed().toLower();
    if (key == QLatin1String("inches"))
        return Unit::Inches;
    if (key == QLatin1String("centimetres") || key == QLatin1String("centimeters"))
        return Unit::Centimetres;
    if (key == QLatin1String("percent"))
        return Unit::Percent;
    return Unit::Pixels;
}

int decimals(Unit unit)
{
    switch (unit) {
    case Unit::Inches: return 3;
    case Unit::Centimetres: return 2;
    case Unit::Percent: return 2;
    case Unit::Pixels:
    default: return 0;
    }
}

double singleStep(Unit unit)
{
    switch (unit) {
    case Unit::Inches: return 0.25;
    case Unit::Centimetres: return 0.5;
    case Unit::Percent: return 5.0;
    case Unit::Pixels:
    default: return 1.0;
    }
}

QStringList measurableNames()
{
    return {unitName(Unit::Pixels), unitName(Unit::Inches),
            unitName(Unit::Centimetres)};
}

QString describeSize(int widthPixels, int heightPixels, double resolution,
                     Unit unit)
{
    if (unit == Unit::Pixels || unit == Unit::Percent) {
        return QStringLiteral("%1 x %2 px").arg(widthPixels).arg(heightPixels);
    }
    const int places = decimals(unit);
    return QStringLiteral("%1 x %2 %3")
        .arg(trimmed(fromPixels(widthPixels, unit, resolution), places),
             trimmed(fromPixels(heightPixels, unit, resolution), places),
             unitAbbreviation(unit));
}

QString describePrintSize(int widthPixels, int heightPixels, double resolution,
                          Unit unit)
{
    // A print size has to be physical, so pixels and percent borrow inches.
    const Unit physical = (unit == Unit::Centimetres) ? Unit::Centimetres
                                                      : Unit::Inches;
    return QStringLiteral("%1 at %2 ppi")
        .arg(describeSize(widthPixels, heightPixels, resolution, physical),
             trimmed(sanitize(resolution), 2));
}

RulerTicks rulerTicks(Unit unit, double resolution, double zoom)
{
    // Below this many screen pixels the labels would collide.
    constexpr double kMinLabelGap = 56.0;
    const double scale = (std::isfinite(zoom) && zoom > 1e-6) ? zoom : 1.0;
    const double dpi = sanitize(resolution);

    if (unit == Unit::Inches || unit == Unit::Centimetres) {
        const std::vector<double>& steps =
            unit == Unit::Inches ? inchSteps() : centimetreSteps();
        for (double step : steps) {
            const double pixels = toPixels(step, unit, dpi);
            if (pixels * scale >= kMinLabelGap) {
                RulerTicks ticks;
                ticks.major = pixels;
                ticks.minorPerMajor = unit == Unit::Inches
                    ? inchSubdivisions(step)
                    : (step <= 0.25 ? 1 : 10);
                // Minor ticks need their own breathing room.
                if (pixels * scale / ticks.minorPerMajor < 6.0)
                    ticks.minorPerMajor = 1;
                return ticks;
            }
        }
        RulerTicks ticks;
        ticks.major = toPixels(steps.back(), unit, dpi);
        return ticks;
    }

    // Pixels: climb the 1 / 5 / 10 ladder until the labels fit.
    double spacing = 10.0;
    while (spacing * scale < kMinLabelGap && spacing < 1.0e7) {
        const double decade = std::pow(10.0, std::floor(std::log10(spacing)));
        spacing = (spacing < decade * 1.5) ? decade * 5.0 : decade * 10.0;
    }
    RulerTicks ticks;
    ticks.major = spacing;
    ticks.minorPerMajor = (spacing * scale >= 60.0) ? 5 : 1;
    if (spacing * scale / ticks.minorPerMajor < 6.0)
        ticks.minorPerMajor = 1;
    return ticks;
}

QString rulerLabel(double pixels, Unit unit, double resolution)
{
    if (unit == Unit::Pixels || unit == Unit::Percent)
        return QString::number(static_cast<long long>(std::llround(pixels)));
    return trimmed(fromPixels(pixels, unit, resolution), decimals(unit));
}

} // namespace tessel::units
