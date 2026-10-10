#pragma once

#include <QObject>

namespace tessel {

// Makes the menu shortcuts work while a non Latin keyboard layout is active.
// With a Cyrillic layout the Z key reports "Ya", so Qt finds nothing bound to
// it and Ctrl+Shift+Z appears to do nothing at all. This filter catches a key
// press that carries Ctrl or Alt, translates the letter back to the Latin one
// printed on the same physical key, and triggers the action bound to it.
// Installed on the application, so it covers every window.
class KeyLayoutFilter : public QObject {
    Q_OBJECT

public:
    explicit KeyLayoutFilter(QObject* parent = nullptr) : QObject(parent) {}

    // The Latin key on the same physical key as this one, or 0 if the key is
    // already Latin or is not a letter we know about.
    static int latinKeyFor(int key);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace tessel
