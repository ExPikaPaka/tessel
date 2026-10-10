// Finding, loading and reporting on effect plugins. The contract the plugins
// themselves are written against is in EffectPlugin.h.
#pragma once

#include <vector>

#include <QString>
#include <QStringList>

#include "effects/Effect.h"

namespace tessel {

class EffectPlugin;

namespace plugins {

// One plugin that loaded, in the order it was found.
struct PluginInfo {
    QString path;
    QString name;
    QString version;
    QString author;
    QStringList effects; // display names, in registration order
};

// One file that looked like a plugin and did not become one. Everything the
// loader gives up on lands here rather than in a log nobody reads.
struct PluginFailure {
    QString path;
    QString reason;
};

// What a scan found. Held for the lifetime of the program so the window can
// show it long after startup.
struct LoadReport {
    QStringList directories; // scanned, whether they existed or not
    std::vector<PluginInfo> loaded;
    std::vector<PluginFailure> failed;

    // One line for the status bar, empty when there is nothing worth saying.
    // A failure is always worth saying, a clean scan of an empty machine is
    // not.
    QString summary() const;

    // The body of the About the Plugins dialog, as rich text. Built here rather
    // than in the dialog so it can be tested without a window, and so the
    // effects library stays free of any dependency on QtWidgets.
    QString html() const;
};

// The directories scanned at startup, in scan order: the per user and system
// wide application data directories, then one beside the executable, each with
// "plugins" appended. Directories that do not exist are still listed, because
// the dialog showing them is also the answer to "where do I put it".
QStringList directories();

// Loads every plugin in directories() into the global registry. Safe to call
// more than once: the work happens on the first call only, so the report does
// not grow a second copy of everything.
void loadAll();

// What loadAll() found, empty until it has run.
//
// Not const, because the tests inject entries to drive the window's reporting
// without needing a real plugin on disk.
LoadReport& report();

// Loads one directory, or one file, appending to `into`. A file that is not a
// library is skipped silently; a library that cannot be loaded, does not
// implement the interface, or was built against a different
// kEffectPluginVersion is recorded as a failure.
void loadDirectory(const QString& path, EffectRegistry& registry, LoadReport& into);
void loadFile(const QString& path, EffectRegistry& registry, LoadReport& into);

// Checks the interface version and, if it matches, lets the plugin register its
// effects. Returns false and records a failure otherwise. Separate from
// loadFile so the version gate can be tested without building a library.
bool acceptPlugin(EffectPlugin* plugin, const QString& path, EffectRegistry& registry,
                  LoadReport& into);

} // namespace plugins

} // namespace tessel
