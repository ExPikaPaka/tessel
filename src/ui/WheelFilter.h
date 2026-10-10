#pragma once

#include <QObject>

namespace tessel {

// Reverses the wheel direction over numeric inputs, so pushing the wheel
// forward lowers a value and pulling it back raises one. Installed on the
// application, so it covers the tool options bar, every dialog and the status
// bar alike.
class WheelFilter : public QObject {
    Q_OBJECT

public:
    explicit WheelFilter(QObject* parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace tessel
