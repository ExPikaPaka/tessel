#include "effects/EffectPluginHost.h"

#include <mutex>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>
#include <QStandardPaths>

#include "effects/EffectPlugin.h"

namespace tessel {

namespace plugins {

namespace {

QString escaped(const QString& text) { return text.toHtmlEscaped(); }

QString plural(int count, const QString& singular, const QString& many)
{
    return count == 1 ? singular : many;
}

} // namespace

QString LoadReport::summary() const
{
    const int ok = int(loaded.size());
    const int bad = int(failed.size());
    if (bad > 0) {
        return QStringLiteral("%1 effect %2 failed to load. See Effects > About the "
                              "Plugins.")
            .arg(bad)
            .arg(plural(bad, QStringLiteral("plugin"), QStringLiteral("plugins")));
    }
    if (ok > 0) {
        return QStringLiteral("%1 effect %2 loaded.")
            .arg(ok)
            .arg(plural(ok, QStringLiteral("plugin"), QStringLiteral("plugins")));
    }
    return QString();
}

QString LoadReport::html() const
{
    QString out;
    out += QStringLiteral("<p>Effects can be added to Tessel by dropping a plugin "
                          "library into one of these directories, which are read at "
                          "startup:</p><ul>");
    for (const QString& dir : directories)
        out += QStringLiteral("<li><code>%1</code></li>").arg(escaped(dir));
    out += QStringLiteral("</ul>");

    if (loaded.empty() && failed.empty()) {
        out += QStringLiteral("<p>No plugins were found.</p>");
    } else if (!loaded.empty()) {
        out += QStringLiteral("<p><b>Loaded</b></p><ul>");
        for (const PluginInfo& info : loaded) {
            QString line = QStringLiteral("<b>%1</b>").arg(escaped(info.name));
            if (!info.version.isEmpty())
                line += QStringLiteral(" %1").arg(escaped(info.version));
            if (!info.author.isEmpty())
                line += QStringLiteral(" by %1").arg(escaped(info.author));
            line += QStringLiteral("<br><code>%1</code>")
                        .arg(escaped(QFileInfo(info.path).fileName()));
            line += info.effects.isEmpty()
                ? QStringLiteral("<br>No effects.")
                : QStringLiteral("<br>%1").arg(escaped(info.effects.join(
                      QStringLiteral(", "))));
            out += QStringLiteral("<li>%1</li>").arg(line);
        }
        out += QStringLiteral("</ul>");
    }

    if (!failed.empty()) {
        out += QStringLiteral("<p><b>Did not load</b></p><ul>");
        for (const PluginFailure& failure : failed) {
            out += QStringLiteral("<li><b><code>%1</code></b><br>%2</li>")
                       .arg(escaped(QFileInfo(failure.path).fileName()),
                            escaped(failure.reason));
        }
        out += QStringLiteral("</ul>");
    }

    out += QStringLiteral("<p>This build hosts effect plugin interface version "
                          "%1.</p>")
               .arg(kEffectPluginVersion);
    return out;
}

QStringList directories()
{
    QStringList out;
    const auto append = [&out](const QString& base) {
        if (base.isEmpty())
            return;
        const QString dir = QDir::cleanPath(base + QStringLiteral("/plugins"));
        if (!out.contains(dir))
            out.append(dir);
    };
    // The per user directory comes first, so a plugin installed by hand wins
    // over a packaged one with the same effect ids.
    const QStringList data =
        QStandardPaths::standardLocations(QStandardPaths::AppDataLocation);
    for (const QString& base : data)
        append(base);
    append(QCoreApplication::applicationDirPath());
    return out;
}

bool acceptPlugin(EffectPlugin* plugin, const QString& path, EffectRegistry& registry,
                  LoadReport& into)
{
    if (!plugin) {
        into.failed.push_back({path, QStringLiteral("no plugin object")});
        return false;
    }
    const int version = plugin->interfaceVersion();
    if (version != kEffectPluginVersion) {
        into.failed.push_back(
            {path,
             QStringLiteral("built against effect plugin interface version %1, this "
                            "build of Tessel hosts version %2. Rebuild the plugin "
                            "against this version of Tessel.")
                 .arg(version)
                 .arg(kEffectPluginVersion)});
        return false;
    }

    // The registry keeps its effects in registration order, so whatever the
    // plugin appends is exactly the tail it added.
    const size_t before = registry.all().size();
    plugin->registerEffects(registry);

    PluginInfo info;
    info.path = path;
    info.name = plugin->name().trimmed();
    if (info.name.isEmpty())
        info.name = QFileInfo(path).fileName();
    info.version = plugin->version().trimmed();
    info.author = plugin->author().trimmed();
    const std::vector<Effect*>& all = registry.all();
    for (size_t i = before; i < all.size(); ++i)
        info.effects.append(all[i]->name());
    into.loaded.push_back(std::move(info));
    return true;
}

void loadFile(const QString& path, EffectRegistry& registry, LoadReport& into)
{
    // Anything that is not a shared library is somebody's README or licence
    // file, not a plugin that went wrong, so it is not reported.
    if (!QLibrary::isLibrary(path))
        return;

    QPluginLoader loader(path);
    QObject* object = loader.instance();
    if (!object) {
        into.failed.push_back({path, loader.errorString()});
        return;
    }
    auto* plugin = qobject_cast<EffectPlugin*>(object);
    if (!plugin) {
        into.failed.push_back(
            {path, QStringLiteral("a Qt plugin, but not a Tessel effect plugin: it "
                                  "does not implement " TesselEffectPluginIid ".")});
        loader.unload();
        return;
    }
    // A plugin that registered effects has to stay loaded for as long as the
    // program runs, because those effects are code inside its library. One that
    // was refused goes straight back out.
    if (!acceptPlugin(plugin, path, registry, into))
        loader.unload();
}

void loadDirectory(const QString& path, EffectRegistry& registry, LoadReport& into)
{
    QDir dir(path);
    if (!dir.exists())
        return;
    // Sorted by name, so the order effects appear in does not depend on the
    // order the file system happens to hand the entries over.
    const QStringList entries = dir.entryList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QString& entry : entries)
        loadFile(dir.absoluteFilePath(entry), registry, into);
}

LoadReport& report()
{
    static LoadReport instance;
    return instance;
}

void loadAll()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        LoadReport& into = report();
        into.directories = directories();
        for (const QString& dir : into.directories)
            loadDirectory(dir, EffectRegistry::instance(), into);
    });
}

} // namespace plugins

} // namespace tessel
