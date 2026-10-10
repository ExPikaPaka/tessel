#pragma once

#include <QString>
#include <QStringList>

namespace tessel {

// The unit the interface measures in. Percent is only an entry convenience for
// the resize dialogs: it has no fixed size, so rulers and the status bar treat
// it as pixels.
enum class Unit {
    Pixels,
    Inches,
    Centimetres,
    Percent,
};

namespace units {

// Matches the historical screen resolution and what most editors assume for a
// file that carries no density of its own.
constexpr double kDefaultResolution = 96.0;
constexpr double kCentimetresPerInch = 2.54;
constexpr double kMetresPerInch = 0.0254;
// Wide enough for anything printable, narrow enough that a damaged file cannot
// turn a print size into infinity.
constexpr double kMinResolution = 1.0;
constexpr double kMaxResolution = 10000.0;

// Falls back to the default for anything non-finite or out of range, so a
// resolution read from a file can be used without further checking.
double sanitize(double resolution);

// `reference` is the pixel count a percentage is measured against; it is
// ignored by the absolute units.
double fromPixels(double pixels, Unit unit, double resolution,
                  double reference = 0.0);
double toPixels(double value, Unit unit, double resolution,
                double reference = 0.0);

// QImage stores density per metre, which is how PNG (pHYs) and JPEG carry it.
double dotsPerMetre(double resolution);
double resolutionFromDotsPerMetre(double dotsPerMetre);

QString unitName(Unit unit);     // menu and combo box label
QString unitAbbreviation(Unit unit); // "px", "in", "cm", "%"
QString unitSuffix(Unit unit);   // spin box suffix, with a leading space
QString unitId(Unit unit);       // stable key for QSettings
Unit unitFromId(const QString& id);
int decimals(Unit unit);
double singleStep(Unit unit);
// The units the view can measure in, in menu order. Percent is left out
// because it cannot label a ruler or a status bar.
QStringList measurableNames();

// "8.5 x 11 in", and plain pixels for Pixels and Percent.
QString describeSize(int widthPixels, int heightPixels, double resolution,
                     Unit unit);
// "8.5 x 11 in at 300 ppi", always in a physical unit.
QString describePrintSize(int widthPixels, int heightPixels, double resolution,
                          Unit unit);

// How far apart ruler ticks sit, in document pixels, for the current zoom.
struct RulerTicks {
    double major = 10.0;   // spacing of the labelled ticks
    int minorPerMajor = 1; // 1 means no minor ticks
};
RulerTicks rulerTicks(Unit unit, double resolution, double zoom);
// Label for a tick at `pixels` from the origin.
QString rulerLabel(double pixels, Unit unit, double resolution);

} // namespace units

} // namespace tessel
