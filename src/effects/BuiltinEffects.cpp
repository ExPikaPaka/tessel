// The one place that names every effect module.
//
// It is a file of its own so that nothing else in the registry refers to the
// effect modules: a plugin links against Effect.cpp for fx::add and the
// parameter specs, and linking that must not drag all forty built in effects
// into the plugin's own library.
#include "effects/Effect.h"

#include <mutex>

namespace tessel {

void registerBuiltinEffects()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        EffectRegistry& registry = EffectRegistry::instance();
        registerAdjustments(registry);
        registerBlurs(registry);
        registerColorEffects(registry);
        registerDistortions(registry);
        registerNoise(registry);
        registerObjectEffects(registry);
        registerPhoto(registry);
        registerRenderers(registry);
        registerStylize(registry);
    });
}

} // namespace tessel
