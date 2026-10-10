#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace tessel::theme {

enum class Mode { Dark, Light };

void apply(QApplication& app, Mode mode);
Mode current();

QColor windowColor();
QColor panelColor();
QColor accentColor();
QColor textColor();
QColor canvasBackdropColor();
QColor checkerLight();
QColor checkerDark();
QColor gridColor();

} // namespace tessel::theme
