#include "effects/Effect.h"

#include <cmath>

#include <QJsonArray>
#include <QJsonDocument>
#include <QPolygonF>

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

namespace {

// One value, tagged with the kind it was stored as. A plain QVariant round
// trips through JSON as a string, which loses a colour's alpha and a curve
// altogether, so the kind travels with it.
QJsonObject taggedValue(const QVariant& value)
{
    QJsonObject out;
    switch (value.userType()) {
    case QMetaType::Bool:
        out[QStringLiteral("t")] = QStringLiteral("bool");
        out[QStringLiteral("v")] = value.toBool();
        return out;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
        out[QStringLiteral("t")] = QStringLiteral("int");
        out[QStringLiteral("v")] = value.toInt();
        return out;
    case QMetaType::Double:
    case QMetaType::Float:
        out[QStringLiteral("t")] = QStringLiteral("double");
        out[QStringLiteral("v")] = value.toDouble();
        return out;
    case QMetaType::QColor:
        out[QStringLiteral("t")] = QStringLiteral("color");
        out[QStringLiteral("v")] = value.value<QColor>().name(QColor::HexArgb);
        return out;
    case QMetaType::QPoint:
    case QMetaType::QPointF: {
        const QPointF point = value.toPointF();
        out[QStringLiteral("t")] = QStringLiteral("point");
        out[QStringLiteral("v")] = QJsonArray{point.x(), point.y()};
        return out;
    }
    default:
        break;
    }
    // A curve arrives as a polygon, which is written as a flat list of
    // coordinates.
    if (value.canConvert<QPolygonF>()) {
        const QPolygonF polygon = value.value<QPolygonF>();
        QJsonArray array;
        for (const QPointF& point : polygon) {
            array.append(point.x());
            array.append(point.y());
        }
        out[QStringLiteral("t")] = QStringLiteral("points");
        out[QStringLiteral("v")] = array;
        return out;
    }
    out[QStringLiteral("t")] = QStringLiteral("string");
    out[QStringLiteral("v")] = value.toString();
    return out;
}

QVariant untaggedValue(const QJsonObject& entry)
{
    const QString type = entry.value(QStringLiteral("t")).toString();
    const QJsonValue value = entry.value(QStringLiteral("v"));
    if (type == QLatin1String("bool"))
        return value.toBool();
    if (type == QLatin1String("int"))
        return value.toInt();
    if (type == QLatin1String("double"))
        return value.toDouble();
    if (type == QLatin1String("color"))
        return QColor(value.toString());
    if (type == QLatin1String("point")) {
        const QJsonArray array = value.toArray();
        return QPointF(array.size() > 0 ? array.at(0).toDouble() : 0.0,
                       array.size() > 1 ? array.at(1).toDouble() : 0.0);
    }
    if (type == QLatin1String("points")) {
        const QJsonArray array = value.toArray();
        QPolygonF polygon;
        for (int i = 0; i + 1 < array.size(); i += 2)
            polygon << QPointF(array.at(i).toDouble(), array.at(i + 1).toDouble());
        return QVariant::fromValue(polygon);
    }
    return value.toString();
}

} // namespace

QJsonObject ParamValues::toJson() const
{
    QJsonObject out;
    for (auto it = values_.constBegin(); it != values_.constEnd(); ++it)
        out[it.key()] = taggedValue(it.value());
    return out;
}

ParamValues ParamValues::fromJson(const QJsonObject& object)
{
    ParamValues values;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        values.set(it.key(), untaggedValue(it.value().toObject()));
    return values;
}

uint64_t ParamValues::hash() const
{
    // Taken over the JSON form because QJsonObject keeps its keys sorted: the
    // digest then cannot depend on the order the values were set in.
    const QByteArray bytes = QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
    uint64_t hash = 1469598103934665603ull;
    for (char byte : bytes)
        hash = (hash ^ uint8_t(byte)) * 1099511628211ull;
    return hash;
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
