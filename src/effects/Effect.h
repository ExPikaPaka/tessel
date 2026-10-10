#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "core/Document.h"
#include "core/ImageOps.h"
#include "core/Params.h"
#include "core/Parallel.h"

namespace tessel {

// Cooperative progress and cancellation channel.
//
// The owner lives on the thread that started the effect; the effect polls the
// channel from its own worker threads, so every member is atomic and `report`
// is safe to call from several threads at once. An effect that ignores the
// channel still runs correctly, it just cannot be stopped early.
class EffectProgress {
public:
    void report(double fraction) const
    {
        const double clamped = std::clamp(fraction, 0.0, 1.0);
        permille_.store(unsigned(clamped * 1000.0 + 0.5), std::memory_order_relaxed);
    }
    double fraction() const
    {
        return double(permille_.load(std::memory_order_relaxed)) / 1000.0;
    }

    bool isCancelled() const { return cancelled_.load(std::memory_order_relaxed); }
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }

private:
    // Written from worker threads while the owner reads it, hence mutable:
    // reporting progress is logically const for the effect.
    mutable std::atomic<unsigned> permille_{0};
    std::atomic<bool> cancelled_{false};
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
    // Optional. When present the effect should feed it and give up early once
    // it reports a cancellation; a partially written result is then thrown
    // away by the caller.
    const EffectProgress* progress = nullptr;
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

// Runs `body(y)` for every row in [begin, end) across the thread pool, feeding
// the context's progress channel and stopping early once it reports a
// cancellation. Returns false if the work was abandoned part way through, in
// which case the output buffer holds a mixture of finished and untouched rows.
//
// Row order inside a range is irrelevant because the rows are independent, so
// a run that is not cancelled produces exactly the same pixels as a plain
// parallelFor would.
template <class Body>
bool forEachRow(const EffectContext& context, int begin, int end, Body&& body)
{
    const int total = end - begin;
    if (total <= 0)
        return true;
    const EffectProgress* progress = context.progress;
    if (!progress) {
        parallelFor(begin, end, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                body(y);
        });
        return true;
    }
    std::atomic<int> done{0};
    parallelFor(begin, end, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            if (progress->isCancelled())
                return;
            body(y);
            const int finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
            // Reporting on every row would be pure overhead on small images.
            if ((finished & 0xf) == 0 || finished == total)
                progress->report(double(finished) / double(total));
        }
    });
    return !progress->isCancelled();
}

// Copies the source, then rewrites every pixel inside the region.
template <class F>
Image mapPixels(const EffectContext& context, F&& fn)
{
    Image out = *context.source;
    const Rect area = context.region.intersected(out.rect());
    if (area.isEmpty())
        return out;
    forEachRow(context, area.y, area.bottom(), [&](int y) {
        const Rgba* src = context.source->row(y);
        Rgba* dst = out.row(y);
        for (int x = area.x; x < area.right(); ++x)
            dst[x] = fn(src[x], x, y);
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
