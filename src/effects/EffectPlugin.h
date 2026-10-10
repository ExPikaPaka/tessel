// The contract between Tessel and an effect plugin.
//
// This header is the whole interface a plugin author needs. Copy it, or point
// your include path at the Tessel source tree, write one class, build a shared
// library and drop it in a plugins directory: Tessel picks it up at startup and
// the effects inside it appear in the Effects menu next to the built in ones,
// with the same generated dialog and the same live preview.
//
// A plugin registers its effects exactly the way an internal effect module
// does, through fx::add on the registry it is handed, so there is nothing new
// to learn beyond the three members of EffectPlugin below.
//
//     // ScanlinesPlugin.h
//     #include "effects/EffectPlugin.h"
//
//     class ScanlinesPlugin : public QObject, public tessel::EffectPlugin {
//         Q_OBJECT
//         Q_PLUGIN_METADATA(IID TesselEffectPluginIid)
//         Q_INTERFACES(tessel::EffectPlugin)
//
//     public:
//         TESSEL_DECLARE_EFFECT_PLUGIN()
//
//         QString name() const override { return QStringLiteral("Scanlines"); }
//         void registerEffects(tessel::EffectRegistry& registry) override;
//     };
//
// plugins/example holds a complete one, including its CMake target.
#pragma once

#include <QObject>
#include <QString>

#include "effects/Effect.h"

namespace tessel {

// The version of this header. A plugin reports the number it was compiled
// against and the host refuses to run anything else, because everything below
// this line is passed across a shared library boundary as C++ objects: a change
// to Effect, ParamSpec, EffectContext or Image changes their layout and a
// plugin built against the older layout would read the wrong bytes.
//
// Bumped on any such change, and only then.
constexpr int kEffectPluginVersion = 1;

// Implemented once per plugin, by a class that also derives from QObject.
//
// The object is created when the library is loaded and lives for as long as the
// program runs. A loaded plugin is never unloaded, because the lambdas it put
// in the registry are code inside its library.
class EffectPlugin {
public:
    virtual ~EffectPlugin() = default;

    // The version of this header the plugin was built against. Never written by
    // hand: use TESSEL_DECLARE_EFFECT_PLUGIN, which defines it correctly.
    virtual int interfaceVersion() const = 0;

    // Shown in Effects > About the Plugins. Name the plugin, not the effects
    // inside it, and keep it short enough to read in a list.
    virtual QString name() const = 0;

    // Both optional, both shown beside the name in that dialog.
    virtual QString version() const { return QString(); }
    virtual QString author() const { return QString(); }

    // Called once, at startup, after the built in effects are in place.
    //
    // Add effects with fx::add(registry, id, name, category, params, body), or
    // with registry.add(std::make_unique<YourEffect>()) if you would rather
    // write the Effect subclass out. The rules:
    //
    //   id        unique across the whole program, so prefix it with something
    //             of your own. An id that is already taken is ignored.
    //   category  the Effects submenu the effect appears under. An existing
    //             name such as "Blurs" or "Stylize" joins that submenu, a new
    //             name gets a submenu of its own after the built in ones.
    //             "Adjustments" puts it in the Adjustments menu instead.
    //   params    the controls the dialog is generated from, in the order they
    //             should appear. An effect with no parameters is applied
    //             straight away with no dialog.
    //   body      run on a worker thread, possibly many times a second while
    //             the dialog previews on the canvas. It must be deterministic
    //             for a given context and parameter set, must not touch the
    //             interface, and should feed context.progress through
    //             fx::forEachRow so a slow effect stays cancellable.
    //
    // Registering nothing is allowed and reported as a plugin that loaded with
    // no effects in it.
    virtual void registerEffects(EffectRegistry& registry) = 0;
};

} // namespace tessel

// Records the interface version inside the plugin's own class, so the number
// compiled into the plugin is the one the host compares against. A default
// implementation in EffectPlugin could not do this: the inlined copy in the
// plugin and the one in the host share a symbol name, and the dynamic linker is
// free to keep either of them.
#define TESSEL_DECLARE_EFFECT_PLUGIN()                                                   \
    int interfaceVersion() const override { return tessel::kEffectPluginVersion; }

// The interface id, passed to Q_PLUGIN_METADATA by the plugin.
//
// It deliberately carries no version number. A plugin built against a different
// kEffectPluginVersion still loads far enough to be refused with an explanation
// naming both versions, whereas a versioned id would fail the interface cast
// instead and be reported as something that is not a Tessel plugin at all.
#define TesselEffectPluginIid "org.tessel.EffectPlugin"

Q_DECLARE_INTERFACE(tessel::EffectPlugin, TesselEffectPluginIid)
