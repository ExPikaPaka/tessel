#pragma once

#include <QString>
#include <QStringList>

#include "core/Blend.h"

// Display spellings for the core enumerations.
//
// They live apart from the enumerations themselves because the names in
// Blend.h are matched on: blendModeName() is what the saved file carries and
// what blendModeFromName() reads back, so it has to stay in English for ever.
// The labels below are only ever read by a person, which is why these are the
// translated ones.

namespace tessel {

// The blend modes in enum order, so an index into this list is a BlendMode.
QStringList blendModeLabels();
QString blendModeLabel(BlendMode mode);

} // namespace tessel
