#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "core/Document.h"
#include "core/ImageOps.h"
#include "core/Parallel.h"

namespace tessel {

enum class ParamType { Int, Double, Bool, Choice, Color, Angle, Offset, Curve };

// Describes one control in an effect's dialog. The dialog is generated from
// these, so an effect never needs its own widget code.
struct ParamSpec {
    ParamType type = ParamType::Int;
    QString id;
    QString label;
    double minimum = 0.0;
    double maximum = 100.0;
    double defaultValue = 0.0;
    double defaultValue2 = 0.0;
    double step = 1.0;
    int decimals = 2;
    QStringList choices;
    QString suffix;
    Rgba color{0, 0, 0, 255};

    static ParamSpec intParam(QString id, QString label, int minimum, int maximum,
                              int defaultValue, QString suffix = QString());
    static ParamSpec doubleParam(QString id, QString label, double minimum,
                                 double maximum, double defaultValue,
                                 double step = 0.1, int decimals = 2,
                                 QString suffix = QString());
    static ParamSpec boolParam(QString id, QString label, bool defaultValue);
    static ParamSpec choiceParam(QString id, QString label, QStringList choices,
                                 int defaultIndex = 0);
    static ParamSpec colorParam(QString id, QString label, Rgba defaultColor);
    static ParamSpec angleParam(QString id, QString label, double defaultDegrees);
    static ParamSpec offsetParam(QString id, QString label, double x = 0.0,
                                 double y = 0.0);
};

class ParamValues {
public:
    void set(const QString& id, const QVariant& value) { values_[id] = value; }
    bool has(const QString& id) const { return values_.contains(id); }

    int intValue(const QString& id, int fallback = 0) const;
    double doubleValue(const QString& id, double fallback = 0.0) const;
    bool boolValue(const QString& id, bool fallback = false) const;
    int choiceValue(const QString& id, int fallback = 0) const;
    Rgba colorValue(const QString& id, Rgba fallback = Rgba(0, 0, 0, 255)) const;
    QPointF pointValue(const QString& id, QPointF fallback = QPointF()) const;
    QVariant raw(const QString& id) const { return values_.value(id); }

    static ParamValues defaults(const std::vector<ParamSpec>& specs);

private:
    QHash<QString, QVariant> values_;
};

// Everything an effect is allowed to look at.
struct EffectContext {
    const Image* source = nullptr;   // the layer being processed
    const Mask* clip = nullptr;      // selection mask, or null for all of it
    Rect region;                     // bounds the result has to cover
    Rgba primary{0, 0, 0, 255};
    Rgba secondary{255, 255, 255, 255};
    unsigned seed = 12345;
    const Document* document = nullptr;
};

class Effect {
public:
    virtual ~Effect() = default;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual QString category() const = 0;
    virtual QString shortcut() const { return QString(); }
    virtual std::vector<ParamSpec> params() const { return {}; }
    virtual bool hasDialog() const { return !params().empty(); }
    virtual bool isAdjustment() const
    {
        return category() == QStringLiteral("Adjustments");
    }
    // Returns a full size image; pixels outside `region` must be left as they
    // are in the source, because the caller mixes the result through the
    // selection mask.
    virtual Image apply(const EffectContext& context,
                        const ParamValues& values) const = 0;
};

// An effect built from a lambda, which is how all the built in ones are made.
class LambdaEffect : public Effect {
public:
    using Fn = std::function<Image(const EffectContext&, const ParamValues&)>;

    LambdaEffect(QString effectId, QString effectName, QString effectCategory,
                 std::vector<ParamSpec> specs, Fn fn, QString keys = QString())
        : id_(std::move(effectId)), name_(std::move(effectName)),
          category_(std::move(effectCategory)), specs_(std::move(specs)),
          fn_(std::move(fn)), shortcut_(std::move(keys))
    {
    }

    QString id() const override { return id_; }
    QString name() const override { return name_; }
    QString category() const override { return category_; }
    QString shortcut() const override { return shortcut_; }
    std::vector<ParamSpec> params() const override { return specs_; }
    Image apply(const EffectContext& context, const ParamValues& values) const override
    {
        return fn_(context, values);
    }

private:
    QString id_;
    QString name_;
    QString category_;
    std::vector<ParamSpec> specs_;
    Fn fn_;
    QString shortcut_;
};

class EffectRegistry {
public:
    static EffectRegistry& instance();

    void add(std::unique_ptr<Effect> effect);
    const std::vector<Effect*>& all() const { return order_; }
    Effect* byId(const QString& id) const;
    QStringList categories() const;
    std::vector<Effect*> inCategory(const QString& category) const;

private:
    std::vector<std::unique_ptr<Effect>> effects_;
    std::vector<Effect*> order_;
    QHash<QString, Effect*> byId_;
};

void registerBuiltinEffects();

// Called by the individual effect modules.
void registerAdjustments(EffectRegistry& registry);
void registerBlurs(EffectRegistry& registry);
void registerDistortions(EffectRegistry& registry);
void registerStylize(EffectRegistry& registry);
void registerNoise(EffectRegistry& registry);
void registerRenderers(EffectRegistry& registry);
void registerPhoto(EffectRegistry& registry);
void registerColorEffects(EffectRegistry& registry);
void registerObjectEffects(EffectRegistry& registry);

// --------------------------------------------------------------- helpers

namespace fx {

// Registers a lambda effect in one line.
void add(EffectRegistry& registry, const char* id, const char* name,
         const char* category, std::vector<ParamSpec> specs,
         LambdaEffect::Fn fn, const char* shortcut = nullptr);

// Copies the source, then rewrites every pixel inside the region.
template <class F>
Image mapPixels(const EffectContext& context, F&& fn)
{
    Image out = *context.source;
    const Rect area = context.region.intersected(out.rect());
    if (area.isEmpty())
        return out;
    parallelFor(area.y, area.bottom(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const Rgba* src = context.source->row(y);
            Rgba* dst = out.row(y);
            for (int x = area.x; x < area.right(); ++x)
                dst[x] = fn(src[x], x, y);
        }
    });
    return out;
}

// Builds three lookup tables once, then applies them inside the region.
template <class F>
Image mapLut(const EffectContext& context, F&& build)
{
    ops::Lut r = ops::identityLut();
    ops::Lut g = r;
    ops::Lut b = r;
    build(r, g, b);
    Image out = *context.source;
    ops::applyLutRgb(out, r, g, b, context.region);
    return out;
}

// Replaces the region with pixels taken from `computed`, which must be the
// same size as the source.
Image replaceRegion(const EffectContext& context, const Image& computed);

// A cheap deterministic hash based noise source, so previews do not flicker.
inline unsigned hashNoise(unsigned x, unsigned y, unsigned seed)
{
    unsigned h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float hashFloat(unsigned x, unsigned y, unsigned seed)
{
    return float(hashNoise(x, y, seed) & 0xffffffu) / float(0xffffffu);
}

// Smooth value noise used by the cloud and texture renderers.
float valueNoise(float x, float y, unsigned seed);
float fractalNoise(float x, float y, unsigned seed, int octaves, float roughness);

} // namespace fx

} // namespace tessel
