#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

#include "core/Image.h"

namespace tessel::icons {

// Icons are drawn with QPainter on demand, so Tessel ships no image assets and
// adapts automatically to the active theme.
QIcon get(const QString& name, int size = 22);
QPixmap pixmap(const QString& name, int size, const QColor& color);
QIcon appIcon();
QPixmap colorSwatch(Rgba color, int size, bool selected = false);
QPixmap checkerPixmap(int cell);
void clearCache();

} // namespace tessel::icons
