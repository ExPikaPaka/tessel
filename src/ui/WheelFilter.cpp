#include "ui/WheelFilter.h"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QEvent>
#include <QWheelEvent>

namespace tessel {

bool WheelFilter::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::Wheel)
        return QObject::eventFilter(watched, event);

    auto* wheel = static_cast<QWheelEvent*>(event);
    const int notches = wheel->angleDelta().y() / 120;
    if (notches == 0)
        return QObject::eventFilter(watched, event);

    // Spin boxes step by their own increment, which keeps decimals and ranges
    // right without this filter knowing anything about them.
    if (auto* spin = qobject_cast<QAbstractSpinBox*>(watched)) {
        if (!spin->isEnabled() || spin->isReadOnly())
            return QObject::eventFilter(watched, event);
        spin->stepBy(-notches);
        event->accept();
        return true;
    }

    // Sliders only respond when they already have focus or the pointer is over
    // them, which is the case here by definition.
    if (auto* slider = qobject_cast<QAbstractSlider*>(watched)) {
        if (!slider->isEnabled())
            return QObject::eventFilter(watched, event);
        const int step = slider->singleStep() > 0 ? slider->singleStep() : 1;
        slider->setValue(slider->value() - notches * step);
        event->accept();
        return true;
    }

    return QObject::eventFilter(watched, event);
}

} // namespace tessel
