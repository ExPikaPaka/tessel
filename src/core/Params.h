#pragma once

#include <vector>

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "core/Image.h"

// The declaration of an effect's controls and the bag of values that goes with
// it. They live in core rather than beside Effect because a layer can store an
// effect and its values, and the document may not depend on the registry.

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

    // `label` is a source string in the "Effects" context, marked at the call
    // site with QT_TRANSLATE_NOOP and looked up here, so that the dialog shows
    // it in the language the program is running in.
    static ParamSpec intParam(QString id, const char* label, int minimum, int maximum,
                              int defaultValue, QString suffix = QString());
    static ParamSpec doubleParam(QString id, const char* label, double minimum,
                                 double maximum, double defaultValue,
                                 double step = 0.1, int decimals = 2,
                                 QString suffix = QString());
    static ParamSpec boolParam(QString id, const char* label, bool defaultValue);
    static ParamSpec choiceParam(QString id, const char* label, QStringList choices,
                                 int defaultIndex = 0);
    static ParamSpec colorParam(QString id, const char* label, Rgba defaultColor);
    static ParamSpec angleParam(QString id, const char* label, double defaultDegrees);
    static ParamSpec offsetParam(QString id, const char* label, double x = 0.0,
                                 double y = 0.0);
};

class ParamValues {
public:
    void set(const QString& id, const QVariant& value) { values_[id] = value; }
    bool has(const QString& id) const { return values_.contains(id); }
    bool isEmpty() const { return values_.isEmpty(); }

    int intValue(const QString& id, int fallback = 0) const;
    double doubleValue(const QString& id, double fallback = 0.0) const;
    bool boolValue(const QString& id, bool fallback = false) const;
    int choiceValue(const QString& id, int fallback = 0) const;
    Rgba colorValue(const QString& id, Rgba fallback = Rgba(0, 0, 0, 255)) const;
    QPointF pointValue(const QString& id, QPointF fallback = QPointF()) const;
    QVariant raw(const QString& id) const { return values_.value(id); }

    static ParamValues defaults(const std::vector<ParamSpec>& specs);

    // Used to decide whether an already computed result still matches the
    // controls, so pressing OK does not recompute what is on screen.
    bool operator==(const ParamValues& other) const { return values_ == other.values_; }
    bool operator!=(const ParamValues& other) const { return !(*this == other); }

    // The shape a stored effect takes in document.json. Every value carries
    // the kind it was written as, so a colour does not come back as a string
    // and a curve does not come back as a list of plain numbers.
    QJsonObject toJson() const;
    static ParamValues fromJson(const QJsonObject& object);

    // Cheap digest of the values, which is what tells a cached effect result
    // that the controls it was computed for have moved.
    uint64_t hash() const;

private:
    QHash<QString, QVariant> values_;
};

} // namespace tessel
