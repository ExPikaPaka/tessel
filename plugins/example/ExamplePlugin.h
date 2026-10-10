// An effect plugin, complete, in two files. Build it with
// -DTESSEL_BUILD_EXAMPLE_PLUGIN=ON and it lands in build/plugins, which is one
// of the directories Tessel reads at startup.
#pragma once

#include "effects/EffectPlugin.h"

namespace tessel_example {

class ScanlinesPlugin : public QObject, public tessel::EffectPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID TesselEffectPluginIid)
    Q_INTERFACES(tessel::EffectPlugin)

public:
    TESSEL_DECLARE_EFFECT_PLUGIN()

    QString name() const override { return QStringLiteral("Scanlines Example"); }
    QString version() const override { return QStringLiteral("1.0"); }
    QString author() const override { return QStringLiteral("The Tessel authors"); }

    void registerEffects(tessel::EffectRegistry& registry) override;
};

} // namespace tessel_example
