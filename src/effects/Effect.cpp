#include "effects/Effect.h"

#include <cmath>

namespace tessel {

ParamSpec ParamSpec::intParam(QString id, QString label, int minimum, int maximum,
                              int defaultValue, QString suffix)
{
    ParamSpec spec;
    spec.type = ParamType::Int;
    spec.id = std::move(id);
    spec.label = std::move(label);
    spec.minimum = minimum;
    spec.maximum = maximum;
    spec.defaultValue = defaultValue;
    spec.step = 1.0;
    spec.suffix = std::move(suffix);
    return spec;
}

ParamSpec ParamSpec::doubleParam(QString id, QString label, double minimum,
                                 double maximum, double defaultValue, double step,
                                 int decimals, QString suffix)
{
    ParamSpec spec;
    spec.type = ParamType::Double;
    spec.id = std::move(id);
    spec.label = std::move(label);
    spec.minimum = minimum;
    spec.maximum = maximum;
    spec.defaultValue = defaultValue;
    spec.step = step;
    spec.decimals = decimals;
    spec.suffix = std::move(suffix);
    return spec;
}

ParamSpec ParamSpec::boolParam(QString id, QString label, bool defaultValue)
{
    ParamSpec spec;
    spec.type = ParamType::Bool;
    spec.id = std::move(id);
    spec.label = std::move(label);
    spec.defaultValue = defaultValue ? 1.0 : 0.0;
    return spec;
}

ParamSpec ParamSpec::choiceParam(QString id, QString label, QStringList choices,
                                 int defaultIndex)
{
    ParamSpec spec;
    spec.type = ParamType::Choice;
    spec.id = std::move(id);
    spec.label = std::move(label);
    spec.choices = std::move(choices);
    spec.defaultValue = defaultIndex;
    spec.maximum = static_cast<double>(std::max<qsizetype>(0, spec.choices.size() - 1));
    return spec;
}

ParamSpec ParamSpec::colorParam(QString id, QString label, Rgba defaultColor)
{
    ParamSpec spec;
    spec.type = ParamType::Color;
    spec.id = std::move(id);
    spec.label = std::move(label);
    spec.color = defaultColor;
    return spec;
}

ParamSpec ParamSpec::angleParam(QString id, QString label, double defaultDegrees)
{
    ParamSpec spec;
    spec.type = ParamType::Angle;
    spec.id = std::move(id);
    spec.label = std::move(label);
    spec.minimum = -360.0;
    spec.maximum = 360.0;
    spec.defaultValue = defaultDegrees;
    spec.step = 1.0;
    spec.decimals = 1;
    spec.suffix = QStringLiteral(" deg");
    return spec;
}

ParamSpec ParamSpec::offsetParam(QString id, QString label, double x, double y)
{
    ParamSpec spec;
    spec.type = ParamType::Offset;
    spec.id = std::move(id);
    spec.label = std::move(label);
    spec.minimum = -1.0;
    spec.maximum = 1.0;
    spec.defaultValue = x;
    spec.defaultValue2 = y;
    spec.step = 0.01;
    return spec;
}

int ParamValues::intValue(const QString& id, int fallback) const
{
    const QVariant value = values_.value(id);
    return value.isValid() ? value.toInt() : fallback;
}

double ParamValues::doubleValue(const QString& id, double fallback) const
{
    const QVariant value = values_.value(id);
    return value.isValid() ? value.toDouble() : fallback;
}

bool ParamValues::boolValue(const QString& id, bool fallback) const
{
    const QVariant value = values_.value(id);
    return value.isValid() ? value.toBool() : fallback;
}

int ParamValues::choiceValue(const QString& id, int fallback) const
{
    return intValue(id, fallback);
}

Rgba ParamValues::colorValue(const QString& id, Rgba fallback) const
{
    const QVariant value = values_.value(id);
    if (!value.isValid())
        return fallback;
    const QColor color = value.value<QColor>();
    if (!color.isValid())
        return fallback;
    return Rgba::fromQColor(color);
}

QPointF ParamValues::pointValue(const QString& id, QPointF fallback) const
{
    const QVariant value = values_.value(id);
    return value.isValid() ? value.toPointF() : fallback;
}

ParamValues ParamValues::defaults(const std::vector<ParamSpec>& specs)
{
    ParamValues values;
    for (const ParamSpec& spec : specs) {
        switch (spec.type) {
        case ParamType::Bool:
            values.set(spec.id, spec.defaultValue != 0.0);
            break;
        case ParamType::Int:
        case ParamType::Choice:
            values.set(spec.id, static_cast<int>(std::lround(spec.defaultValue)));
            break;
        case ParamType::Color:
            values.set(spec.id, spec.color.toQColor());
            break;
        case ParamType::Offset:
            values.set(spec.id, QPointF(spec.defaultValue, spec.defaultValue2));
            break;
        case ParamType::Double:
        case ParamType::Angle:
        case ParamType::Curve:
        default:
            values.set(spec.id, spec.defaultValue);
            break;
        }
    }
    return values;
}

EffectRegistry& EffectRegistry::instance()
{
    static EffectRegistry registry;
    return registry;
}

void EffectRegistry::add(std::unique_ptr<Effect> effect)
{
    if (!effect)
        return;
    Effect* raw = effect.get();
    if (byId_.contains(raw->id()))
        return;
    byId_.insert(raw->id(), raw);
    order_.push_back(raw);
    effects_.push_back(std::move(effect));
}

Effect* EffectRegistry::byId(const QString& id) const
{
    return byId_.value(id, nullptr);
}

QStringList EffectRegistry::categories() const
{
    QStringList out;
    for (Effect* effect : order_) {
        if (!out.contains(effect->category()))
            out.append(effect->category());
    }
    return out;
}

std::vector<Effect*> EffectRegistry::inCategory(const QString& category) const
{
    std::vector<Effect*> out;
    for (Effect* effect : order_) {
        if (effect->category() == category)
            out.push_back(effect);
    }
    return out;
}

namespace fx {

void add(EffectRegistry& registry, const char* id, const char* name,
         const char* category, std::vector<ParamSpec> specs,
         LambdaEffect::Fn fn, const char* shortcut)
{
    registry.add(std::make_unique<LambdaEffect>(
        QString::fromLatin1(id), QString::fromLatin1(name),
        QString::fromLatin1(category), std::move(specs), std::move(fn),
        shortcut ? QString::fromLatin1(shortcut) : QString()));
}

Image replaceRegion(const EffectContext& context, const Image& computed)
{
    Image out = *context.source;
    const Rect area = context.region.intersected(out.rect()).intersected(computed.rect());
    if (area.isEmpty())
        return out;
    for (int y = area.y; y < area.bottom(); ++y) {
        std::memcpy(out.row(y) + area.x, computed.row(y) + area.x,
                    static_cast<size_t>(area.w) * sizeof(Rgba));
    }
    return out;
}

namespace {

inline float smoothStep(float t) { return t * t * (3.0f - 2.0f * t); }

inline float latticeValue(int x, int y, unsigned seed)
{
    return hashFloat(static_cast<unsigned>(x + 0x8000), static_cast<unsigned>(y + 0x8000),
                     seed);
}

} // namespace

float valueNoise(float x, float y, unsigned seed)
{
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = smoothStep(x - xi);
    const float fy = smoothStep(y - yi);
    const float v00 = latticeValue(xi, yi, seed);
    const float v10 = latticeValue(xi + 1, yi, seed);
    const float v01 = latticeValue(xi, yi + 1, seed);
    const float v11 = latticeValue(xi + 1, yi + 1, seed);
    const float top = v00 + (v10 - v00) * fx;
    const float bottom = v01 + (v11 - v01) * fx;
    return top + (bottom - top) * fy;
}

float fractalNoise(float x, float y, unsigned seed, int octaves, float roughness)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    float frequency = 1.0f;
    for (int i = 0; i < std::max(1, octaves); ++i) {
        sum += valueNoise(x * frequency, y * frequency, seed + unsigned(i) * 7919u)
            * amplitude;
        total += amplitude;
        amplitude *= std::clamp(roughness, 0.05f, 0.95f);
        frequency *= 2.0f;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

} // namespace fx

} // namespace tessel
