#include "core/Strings.h"

#include <QCoreApplication>

namespace tessel {

QStringList blendModeLabels()
{
    // Built on every call rather than kept in a static, so that a language
    // change is not frozen out by whichever run filled the cache first.
    return {
        QCoreApplication::translate("Core", "Normal"),
        QCoreApplication::translate("Core", "Multiply"),
        QCoreApplication::translate("Core", "Additive"),
        QCoreApplication::translate("Core", "Color Burn"),
        QCoreApplication::translate("Core", "Color Dodge"),
        QCoreApplication::translate("Core", "Reflect"),
        QCoreApplication::translate("Core", "Glow"),
        QCoreApplication::translate("Core", "Overlay"),
        QCoreApplication::translate("Core", "Difference"),
        QCoreApplication::translate("Core", "Negation"),
        QCoreApplication::translate("Core", "Lighten"),
        QCoreApplication::translate("Core", "Darken"),
        QCoreApplication::translate("Core", "Screen"),
        QCoreApplication::translate("Core", "Xor"),
    };
}

QString blendModeLabel(BlendMode mode)
{
    const QStringList labels = blendModeLabels();
    const int index = static_cast<int>(mode);
    if (index < 0 || index >= labels.size())
        return labels.first();
    return labels.at(index);
}

} // namespace tessel
