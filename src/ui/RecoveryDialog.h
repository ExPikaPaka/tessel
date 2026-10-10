#pragma once

#include <vector>

#include <QDialog>

#include "core/Recovery.h"

class QTreeWidget;

namespace tessel {

// Offered at startup when a previous run left recovery files behind. Lists
// what was found and asks once what should happen to all of it: recovering
// one file and discarding another is not worth a second column of buttons.
class RecoveryDialog : public QDialog {
    Q_OBJECT

public:
    enum class Choice { Later, Recover, Discard };

    explicit RecoveryDialog(const std::vector<recovery::Entry>& entries,
                            QWidget* parent = nullptr);

    Choice choice() const { return choice_; }

    // Used by the tests to press the buttons the way a person would.
    void chooseRecover();
    void chooseDiscard();
    void chooseLater();

    static QString describe(const recovery::Entry& entry);

private:
    void finish(Choice choice);

    QTreeWidget* list_ = nullptr;
    Choice choice_ = Choice::Later;
};

} // namespace tessel
