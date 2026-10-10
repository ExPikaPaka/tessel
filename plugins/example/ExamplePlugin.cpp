#include "ExamplePlugin.h"

#include <algorithm>

#include "core/Blend.h"

namespace tessel_example {

using namespace tessel;

void ScanlinesPlugin::registerEffects(EffectRegistry& registry)
{
    // The same one liner the built in effects use. The three parameters are
    // what the dialog is built from, in this order, and the body below runs on
    // every preview as the user drags them.
    fx::add(registry, "example_scanlines", "Scanlines", "Examples",
            {
                ParamSpec::intParam("spacing", "Line spacing", 2, 64, 4, " px"),
                ParamSpec::doubleParam("strength", "Strength", 0.0, 1.0, 0.6, 0.05, 2),
                ParamSpec::colorParam("tint", "Line colour", Rgba(0, 0, 0, 255)),
            },
            [](const EffectContext& context, const ParamValues& values) {
                const int spacing = std::max(2, values.intValue("spacing", 4));
                const Rgba tint = values.colorValue("tint", Rgba(0, 0, 0, 255));
                // The tint's own alpha scales the strength, so a half
                // transparent colour draws a fainter line without touching the
                // slider.
                const float weight = float(values.doubleValue("strength", 0.6))
                    * float(tint.a) / 255.0f;

                // mapPixels copies the source, rewrites the region and feeds
                // the progress channel, which is what makes a slow effect
                // cancellable from the dialog.
                return fx::mapPixels(context, [&](Rgba c, int, int y) {
                    if (y % spacing != 0)
                        return c;
                    const float keep = 1.0f - weight;
                    return Rgba(toByte((c.r * keep + tint.r * weight) / 255.0f),
                                toByte((c.g * keep + tint.g * weight) / 255.0f),
                                toByte((c.b * keep + tint.b * weight) / 255.0f), c.a);
                });
            });
}

} // namespace tessel_example
