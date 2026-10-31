// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleUpgrade.h"
#include "ParticleSimulation.h"
#include "util/Color.h"
#include "util/Angle.h"
#include "action/Ease.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    // keys of a modifier's curve (spec 6.7: 17 of normalized time, plus its ends)
    constexpr int MODIFIER_KEYS = 17;
    // a value counts as exact below this (colours, sizes, alphas are all near unit scale)
    constexpr float EXACT_TOLERANCE = 1e-4f;

    MinMaxCurve range(float a, float b){
        return a == b ? MinMaxCurve(a) : MinMaxCurve::between(a, b);
    }

    void add(UpgradeReport& report, const std::string& field, UpgradeStatus status, const std::string& message, float error = 0.0f){
        UpgradeItem item;
        item.field = field;
        item.status = status;
        item.message = message;
        item.maxError = error;
        report.items.push_back(item);
    }

    std::string number(float value){
        std::ostringstream out;
        out.precision(4);
        out << value;
        return out.str();
    }

    // a legacy modifier: its value at an absolute particle time (`initial` before it starts,
    // its end value after it), the ease applied as the legacy code does (an ease out of 0..1
    // leaves the value unchanged, so the one before stays)
    struct Modifier {
        float fromTime = 0, toTime = 0;
        Ease function;

        bool active() const { return fromTime != toTime && toTime > fromTime; }

        // the eased fraction, or -1 outside the modifier (the value before then holds)
        float fraction(float time) const{
            if (!active()) return -1.0f;
            if (time < fromTime) return -2.0f;
            if (time > toTime) return 2.0f;
            Ease ease = function;
            const float v = ease.call((time - fromTime) / (toTime - fromTime));
            return (v >= 0.0f && v <= 1.0f) ? v : -1.0f;
        }
    };

    // the key times of a modifier's curve: both ends, its start and end at `life`, and evenly
    // spaced ones in between
    std::vector<float> keyTimes(const Modifier& m, float life){
        std::vector<float> times;
        for (int i = 0; i < MODIFIER_KEYS - 2; i++){
            times.push_back((float)i / (float)(MODIFIER_KEYS - 3));
        }
        if (life > 0.0f){
            times.push_back(std::min(1.0f, std::max(0.0f, m.fromTime / life)));
            times.push_back(std::min(1.0f, std::max(0.0f, m.toTime / life)));
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](float a, float b){ return std::fabs(a - b) < 1e-5f; }), times.end());
        return times;
    }

    // a piecewise linear curve through (times, values): each key's in and out tangents are the
    // slopes of its segments, so the Hermite segments are the straight lines between them
    AnimationCurve linearCurve(const std::vector<float>& times, const std::vector<float>& values){
        AnimationCurve curve;
        const size_t n = times.size();
        for (size_t i = 0; i < n; i++){
            CurveKey key(times[i], values[i]);
            if (i > 0 && times[i] > times[i - 1]) key.inTangent = (values[i] - values[i - 1]) / (times[i] - times[i - 1]);
            if (i + 1 < n && times[i + 1] > times[i]) key.outTangent = (values[i + 1] - values[i]) / (times[i + 1] - times[i]);
            if (i == 0) key.inTangent = key.outTangent;
            if (i + 1 == n) key.outTangent = key.inTangent;
            curve.keys.push_back(key);
        }
        curve.normalize();
        return curve;
    }

    // the legacy value of a modifier over normalized time for a particle living `life` seconds
    float legacyValue(const Modifier& m, float t, float life, float initial, float from, float to){
        const float f = m.fraction(t * life);
        if (f == -2.0f) return initial;
        if (f == 2.0f) return to;
        if (f < 0.0f) return initial;
        return from + (to - from) * f;
    }

    // a modifier as a curve over normalized time at the mean life; the largest difference
    // from the legacy values over the life range
    AnimationCurve modifierCurve(const Modifier& m, float minLife, float maxLife, float initial, float from, float to, float& maxError){
        const float life = (minLife + maxLife) * 0.5f;
        const std::vector<float> times = keyTimes(m, life);
        std::vector<float> values;
        for (float t : times) values.push_back(legacyValue(m, t, life, initial, from, to));
        AnimationCurve curve = linearCurve(times, values);
        maxError = 0.0f;
        for (float l : {minLife, life, maxLife}){
            for (int i = 0; i <= 64; i++){
                const float t = (float)i / 64.0f;
                maxError = std::max(maxError, std::fabs(curve.evaluate(t) - legacyValue(m, t, l, initial, from, to)));
            }
        }
        return curve;
    }

    template<typename M>
    Modifier modifier(const M& m){
        Modifier out;
        out.fromTime = m.fromTime;
        out.toTime = m.toTime;
        out.function = m.function;
        return out;
    }

    // a list of (time, value) keys reduced to Gradient::MAX_KEYS: the ends and the ones the
    // linear blend misses most, a key at a time
    std::vector<std::pair<float, Vector4>> reduceKeys(std::vector<std::pair<float, Vector4>> keys){
        while (keys.size() > Gradient::MAX_KEYS){
            size_t best = 1;
            float bestError = INFINITY;
            for (size_t i = 1; i + 1 < keys.size(); i++){
                const float span = keys[i + 1].first - keys[i - 1].first;
                const float f = span > 0.0f ? (keys[i].first - keys[i - 1].first) / span : 0.0f;
                const Vector4 blend = keys[i - 1].second + (keys[i + 1].second - keys[i - 1].second) * f;
                const Vector4 d = blend - keys[i].second;
                const float error = std::max(std::max(std::fabs(d.x), std::fabs(d.y)), std::max(std::fabs(d.z), std::fabs(d.w)));
                if (error < bestError){
                    bestError = error;
                    best = i;
                }
            }
            keys.erase(keys.begin() + (std::ptrdiff_t)best);
        }
        return keys;
    }

    // the legacy cycle (ActionSystem::getParticleCycleDuration)
    float legacyCycle(const ParticleSystemComponent& p){
        float cycle = 0.0f;
        if (p.rate > 0) cycle = std::max(cycle, (float)p.maxParticles / (float)p.rate);
        for (const ParticleBurst& burst : p.bursts) cycle = std::max(cycle, burst.time);
        if (cycle <= 0.0f && !p.bursts.empty()) cycle = std::max(0.0001f, p.lifeInitializer.maxLife);
        return cycle;
    }

    // Euler angles in radians of a quaternion, applied X then Y then Z (eulerRadians' order)
    Vector3 eulerOf(const Quaternion& q){
        // from the rotation matrix R = Rz * Ry * Rx, gimbal lock (Y at +-90 degrees) included
        const float r00 = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
        const float r01 = 2.0f * (q.x * q.y - q.w * q.z);
        const float r02 = 2.0f * (q.x * q.z + q.w * q.y);
        const float r10 = 2.0f * (q.x * q.y + q.w * q.z);
        const float r20 = 2.0f * (q.x * q.z - q.w * q.y);
        const float r21 = 2.0f * (q.y * q.z + q.w * q.x);
        const float r22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        const float halfPi = 3.14159265f * 0.5f;
        if (std::fabs(r20) < 0.99999f) return Vector3(std::atan2(r21, r22), std::asin(-r20), std::atan2(r10, r00));
        if (r20 < 0.0f) return Vector3(std::atan2(r01, r02), halfPi, 0.0f);
        return Vector3(std::atan2(-r01, -r02), -halfPi, 0.0f);
    }

    // the legacy gradient's colour at t (its stops blended in the space they are written in)
    Vector3 legacyGradient(const ParticleColorGradient& gradient, float t){
        const std::vector<ParticleColorGradientStop>& stops = gradient.stops;
        if (stops.empty()) return Vector3(1, 1, 1);
        Vector3 color;
        if (t <= stops.front().time) color = stops.front().color;
        else if (t >= stops.back().time) color = stops.back().color;
        else{
            size_t next = 1;
            while (next < stops.size() && stops[next].time < t) next++;
            const ParticleColorGradientStop& a = stops[next - 1];
            const ParticleColorGradientStop& b = stops[next];
            const float span = b.time - a.time;
            const float f = span > 0.0f ? (t - a.time) / span : 0.0f;
            color = a.color + (b.color - a.color) * f;
        }
        return gradient.useSRGB ? Color::sRGBToLinear(color) : color;
    }

}

bool UpgradeReport::hasStatus(UpgradeStatus status) const{
    for (const UpgradeItem& item : items){
        if (item.status == status) return true;
    }
    return false;
}

const UpgradeItem* UpgradeReport::find(const std::string& field) const{
    for (const UpgradeItem& item : items){
        if (item.field == field) return &item;
    }
    return nullptr;
}

const char* particles::upgradeStatusName(UpgradeStatus status){
    switch (status){
        case UpgradeStatus::Exact: return "exact";
        case UpgradeStatus::Approximated: return "approximated";
        case UpgradeStatus::Dropped: return "dropped";
    }
    return "exact";
}

UpgradeResult particles::upgradeLegacy(const ParticleSystemComponent& legacy, bool targetIsPoints, bool force){
    return upgradeLegacy(legacy, targetIsPoints ? LegacyTarget::Points : LegacyTarget::Billboard, force);
}

UpgradeResult particles::upgradeLegacy(const ParticleSystemComponent& legacy, LegacyTarget target, bool force){
    const bool targetIsPoints = target == LegacyTarget::Points;
    UpgradeResult result;
    UpgradeReport& report = result.report;
    ParticleSystemComponent& c = result.component;
    c = legacy;
    c.version = 2;
    c.state = ParticleSimState();
    // the modules start from Unity's defaults with every optional module off
    c.main = ParticleMainModule();
    c.emission = ParticleEmissionModule();
    c.shape = ParticleShapeModule();
    c.velocityOverLifetime = ParticleVelocityOverLifetimeModule();
    c.limitVelocityOverLifetime = ParticleLimitVelocityOverLifetimeModule();
    c.inheritVelocity = ParticleInheritVelocityModule();
    c.forceOverLifetime = ParticleForceOverLifetimeModule();
    c.colorOverLifetime = ParticleColorOverLifetimeModule();
    c.colorBySpeed = ParticleColorBySpeedModule();
    c.sizeOverLifetime = ParticleSizeOverLifetimeModule();
    c.sizeBySpeed = ParticleSizeBySpeedModule();
    c.rotationOverLifetime = ParticleRotationOverLifetimeModule();
    c.rotationBySpeed = ParticleRotationBySpeedModule();
    c.noise = ParticleNoiseModule();
    c.textureSheetAnimation = ParticleTextureSheetAnimationModule();
    c.renderer = ParticleRendererModule();
    // how the target drew its particles (version 1 did not clamp sizes on screen)
    switch (target){
        case LegacyTarget::Points:
        case LegacyTarget::Billboard:
            c.renderer.renderMode = ParticleSystemRenderMode::Billboard;
            break;
        case LegacyTarget::VerticalBillboard:
            c.renderer.renderMode = ParticleSystemRenderMode::VerticalBillboard;
            break;
        case LegacyTarget::Mesh:
            // its own mesh at each particle, turned by the particle's rotation in the system's space
            c.renderer.renderMode = ParticleSystemRenderMode::Mesh;
            c.renderer.mesh = "model";
            c.renderer.alignment = legacy.localSpace ? ParticleSystemRenderSpace::Local : ParticleSystemRenderSpace::World;
            break;
    }
    if (target != LegacyTarget::Points) c.renderer.maxParticleSize = 0.0f;

    const ParticleSystemComponent& L = legacy;
    ParticleMainModule& main = c.main;
    const float minLife = std::max(0.0f, std::min(L.lifeInitializer.minLife, L.lifeInitializer.maxLife));
    const float maxLife = std::max(L.lifeInitializer.minLife, L.lifeInitializer.maxLife);

    // ---- the system
    main.maxParticles = L.maxParticles;
    main.looping = L.loop;
    main.playOnAwake = true;
    main.startSpeed = MinMaxCurve(0.0f);
    main.simulationSpace = L.localSpace ? ParticleSystemSimulationSpace::Local : ParticleSystemSimulationSpace::World;
    // a local system scaled its positions, speeds and sizes with the target; a world one only
    // its spawn positions
    main.scalingMode = L.localSpace ? ParticleSystemScalingMode::Hierarchy : ParticleSystemScalingMode::Shape;
    add(report, "localSpace", UpgradeStatus::Exact, std::string("simulationSpace ") + (L.localSpace ? "Local" : "World"));
    switch (target){
        case LegacyTarget::Points:
            add(report, "renderer", UpgradeStatus::Exact, "Billboard points");
            break;
        case LegacyTarget::Billboard:
            add(report, "renderer", UpgradeStatus::Exact, "renderer.renderMode Billboard (the instanced billboard quad), no on-screen size limit");
            break;
        case LegacyTarget::VerticalBillboard:
            add(report, "renderer", UpgradeStatus::Exact, "renderer.renderMode VerticalBillboard (the cylindrical billboard), no on-screen size limit");
            break;
        case LegacyTarget::Mesh:
            add(report, "renderer", UpgradeStatus::Exact, std::string("renderer.renderMode Mesh (the GameObject's own mesh), alignment ") +
                (L.localSpace ? "Local" : "World") + ", no on-screen size limit");
            break;
    }

    if (L.loop){
        const float cycle = legacyCycle(L);
        main.duration = std::max(0.05f, cycle > 0.0f ? cycle : 5.0f);
        add(report, "loop", UpgradeStatus::Exact, "looping, duration " + number(main.duration) + " s (the legacy cycle)");
    }else if (L.rate > 0){
        // the legacy pool ran out after maxParticles spawns
        main.duration = std::max(0.05f, (float)L.maxParticles / (float)L.rate);
        add(report, "loop", UpgradeStatus::Exact, "not looping, duration " + number(main.duration) + " s (maxParticles / rate)");
    }else{
        float last = 0.0f;
        for (const ParticleBurst& burst : L.bursts) last = std::max(last, burst.time);
        main.duration = std::max(0.05f, last + 0.05f);
        add(report, "loop", UpgradeStatus::Approximated,
            "not looping without a rate: it never stopped before; now it stops when its particles are gone (duration " + number(main.duration) + " s)");
    }

    c.emission.rateOverTime = MinMaxCurve((float)std::max(0, L.rate));
    add(report, "rate", UpgradeStatus::Exact, "emission.rateOverTime " + std::to_string(std::max(0, L.rate)));
    if (L.maxPerUpdate != ParticleSystemComponent().maxPerUpdate && L.rate > L.maxPerUpdate * 30){
        add(report, "maxPerUpdate", UpgradeStatus::Dropped, "the per-frame spawn cap has no module equivalent");
    }

    for (const ParticleBurst& burst : L.bursts){
        ParticleEmissionBurst b;
        b.time = burst.time;
        const int lo = std::max(0, std::min(burst.minCount, burst.maxCount));
        const int hi = std::max(0, std::max(burst.minCount, burst.maxCount));
        // a rounded lerp over [lo - 0.5, hi + 0.5) picks the whole numbers evenly, as the legacy one
        b.count = lo == hi ? MinMaxCurve((float)lo) : MinMaxCurve::between((float)lo - 0.4999f, (float)hi + 0.4999f);
        b.cycles = 1;
        c.emission.bursts.push_back(b);
    }
    if (!L.bursts.empty()){
        add(report, "bursts", UpgradeStatus::Exact, std::to_string(L.bursts.size()) + " burst(s)");
    }

    main.startLifetime = range(L.lifeInitializer.minLife, L.lifeInitializer.maxLife);
    add(report, "lifeInitializer", UpgradeStatus::Exact, "main.startLifetime");

    // ---- the shape (the start speed is 0: only the positions matter)
    const ParticlePositionInitializer& pos = L.positionInitializer;
    ParticleShapeModule& shape = c.shape;
    shape.enabled = true;
    shape.arc = 360.0f;
    const float outer = std::max(0.0f, pos.radius);
    const float inner = std::min(std::max(0.0f, pos.innerRadius), outer);
    switch (pos.shape){
        case ParticleEmitterShape::Box: {
            shape.shapeType = ParticleSystemShapeType::Box;
            shape.position = (pos.minPosition + pos.maxPosition) * 0.5f;
            const Vector3 size = pos.maxPosition - pos.minPosition;
            shape.scale = Vector3(std::fabs(size.x), std::fabs(size.y), std::fabs(size.z));
            add(report, "positionInitializer", UpgradeStatus::Exact, "shape Box");
            break;
        }
        case ParticleEmitterShape::Sphere:
        case ParticleEmitterShape::Hemisphere:
        case ParticleEmitterShape::Circle:
            shape.shapeType = pos.shape == ParticleEmitterShape::Sphere ? ParticleSystemShapeType::Sphere
                            : pos.shape == ParticleEmitterShape::Hemisphere ? ParticleSystemShapeType::Hemisphere
                            : ParticleSystemShapeType::Circle;
            shape.radius = outer;
            shape.radiusThickness = outer > 0.0f ? 1.0f - inner / outer : 1.0f;
            add(report, "positionInitializer", UpgradeStatus::Exact,
                std::string("shape ") + (pos.shape == ParticleEmitterShape::Sphere ? "Sphere" : pos.shape == ParticleEmitterShape::Hemisphere ? "Hemisphere" : "Circle"));
            break;
        case ParticleEmitterShape::Cone:
            shape.shapeType = ParticleSystemShapeType::ConeVolume;
            shape.radius = 0.0f;
            shape.angle = std::min(std::max(0.0f, pos.coneAngle), 89.0f);
            shape.length = std::max(0.0f, pos.coneHeight);
            add(report, "positionInitializer", UpgradeStatus::Approximated,
                "shape Cone Volume (radius 0, the same angle and height): the legacy cone filled its volume evenly, a cone volume spreads along its rays");
            break;
    }

    // ---- velocity and acceleration: additive per particle values
    const ParticleVelocityInitializer& vel = L.velocityInitializer;
    if (vel.minVelocity != Vector3(0, 0, 0) || vel.maxVelocity != Vector3(0, 0, 0)){
        ParticleVelocityOverLifetimeModule& v = c.velocityOverLifetime;
        v.enabled = true;
        v.space = ParticleSystemSimulationSpace::Local;
        v.x = range(vel.minVelocity.x, vel.maxVelocity.x);
        v.y = range(vel.minVelocity.y, vel.maxVelocity.y);
        v.z = range(vel.minVelocity.z, vel.maxVelocity.z);
        add(report, "velocityInitializer", UpgradeStatus::Exact,
            "velocityOverLifetime x/y/z (Local); a World system whose emitter turns sees them turn with it");
    }
    const ParticleAccelerationInitializer& acc = L.accelerationInitializer;
    if (acc.minAcceleration != Vector3(0, 0, 0) || acc.maxAcceleration != Vector3(0, 0, 0)){
        ParticleForceOverLifetimeModule& f = c.forceOverLifetime;
        f.enabled = true;
        f.space = ParticleSystemSimulationSpace::Local;
        f.x = range(acc.minAcceleration.x, acc.maxAcceleration.x);
        f.y = range(acc.minAcceleration.y, acc.maxAcceleration.y);
        f.z = range(acc.minAcceleration.z, acc.maxAcceleration.z);
        add(report, "accelerationInitializer", UpgradeStatus::Exact, "forceOverLifetime x/y/z (Local)");
    }

    // ---- colour and alpha
    const ParticleColorInitializer& col = L.colorInitializer;
    const Vector3 cmin = col.useSRGB ? Color::sRGBToLinear(col.minColor) : col.minColor;
    const Vector3 cmax = col.useSRGB ? Color::sRGBToLinear(col.maxColor) : col.maxColor;
    const float amin = L.alphaInitializer.minAlpha, amax = L.alphaInitializer.maxAlpha;
    const bool hasGradient = !L.colorGradient.stops.empty();
    const Modifier colorMod = modifier(L.colorModifier);
    const Modifier alphaMod = modifier(L.alphaModifier);
    // the gradient and the colour modifier replace the start colour's rgb
    const bool rgbReplaced = hasGradient || colorMod.active();
    const Vector3 startMin = rgbReplaced ? Vector3(1, 1, 1) : cmin;
    const Vector3 startMax = rgbReplaced ? Vector3(1, 1, 1) : cmax;
    const bool alphaReplaced = alphaMod.active() && alphaMod.fromTime <= 0.0f;
    const float startAMin = alphaReplaced ? 1.0f : amin, startAMax = alphaReplaced ? 1.0f : amax;
    if (startMin == startMax && startAMin == startAMax){
        main.startColor = MinMaxGradient::fromColor(Vector4(startMin.x, startMin.y, startMin.z, startAMin));
        add(report, "colorInitializer", UpgradeStatus::Exact, "main.startColor");
    }else{
        main.startColor = MinMaxGradient::between(Vector4(startMin.x, startMin.y, startMin.z, startAMin), Vector4(startMax.x, startMax.y, startMax.z, startAMax));
        int varying = 0;
        varying += startMin.x != startMax.x;
        varying += startMin.y != startMax.y;
        varying += startMin.z != startMax.z;
        varying += startAMin != startAMax;
        const bool exact = varying <= 1 && (startMin == startMax || !col.useSRGB);
        add(report, "colorInitializer", exact ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
            exact ? "main.startColor (two colours)"
                  : "main.startColor two colours: one random blend for every channel (the legacy one was per channel, in sRGB)");
    }

    // colour over lifetime: the gradient, or the colour modifier, and the alpha modifier
    std::vector<GradientColorKey> colorKeys;
    std::vector<GradientAlphaKey> alphaKeys;
    float colorError = 0.0f, alphaError = 0.0f;
    if (hasGradient){
        ParticleColorGradient sorted = L.colorGradient;
        sorted.normalize();
        std::vector<std::pair<float, Vector4>> keys;
        for (const ParticleColorGradientStop& stop : sorted.stops){
            const Vector3 v = sorted.useSRGB ? Color::sRGBToLinear(stop.color) : stop.color;
            keys.push_back({stop.time, Vector4(v.x, v.y, v.z, 1.0f)});
        }
        keys = reduceKeys(keys);
        for (const auto& key : keys) colorKeys.push_back(GradientColorKey(key.first, Vector3(key.second.x, key.second.y, key.second.z)));
    }else if (colorMod.active()){
        const Modifier& m = colorMod;
        const float life = (minLife + maxLife) * 0.5f;
        const Vector3 from = L.colorModifier.useSRGB ? Color::sRGBToLinear(L.colorModifier.fromColor) : L.colorModifier.fromColor;
        const Vector3 to = L.colorModifier.useSRGB ? Color::sRGBToLinear(L.colorModifier.toColor) : L.colorModifier.toColor;
        const Vector3 initial = (cmin + cmax) * 0.5f;
        std::vector<std::pair<float, Vector4>> keys;
        for (float t : keyTimes(m, life)){
            const float f = m.fraction(t * life);
            Vector3 v = f == -2.0f || f < -0.5f ? initial : (f == 2.0f ? to : from + (to - from) * f);
            keys.push_back({t, Vector4(v.x, v.y, v.z, 1.0f)});
        }
        keys = reduceKeys(keys);
        for (const auto& key : keys) colorKeys.push_back(GradientColorKey(key.first, Vector3(key.second.x, key.second.y, key.second.z)));
    }
    if (alphaMod.active()){
        const float initial = (amin + amax) * 0.5f;
        AnimationCurve curve = modifierCurve(alphaMod, minLife, maxLife, initial, L.alphaModifier.fromAlpha, L.alphaModifier.toAlpha, alphaError);
        std::vector<std::pair<float, Vector4>> keys;
        for (const CurveKey& key : curve.keys) keys.push_back({key.time, Vector4(key.value, 0, 0, 0)});
        keys = reduceKeys(keys);
        for (const auto& key : keys) alphaKeys.push_back(GradientAlphaKey(key.first, key.second.x));
    }
    if (!colorKeys.empty() || !alphaKeys.empty()){
        Gradient gradient(colorKeys.empty() ? std::vector<GradientColorKey>{GradientColorKey(0, Vector3(1, 1, 1)), GradientColorKey(1, Vector3(1, 1, 1))} : colorKeys,
                          alphaKeys.empty() ? std::vector<GradientAlphaKey>{GradientAlphaKey(0, 1), GradientAlphaKey(1, 1)} : alphaKeys);
        gradient.normalize();
        c.colorOverLifetime.enabled = true;
        c.colorOverLifetime.color = MinMaxGradient::fromGradient(gradient);

        // the colour's difference from the legacy values over the life range
        for (float life : {minLife, (minLife + maxLife) * 0.5f, maxLife}){
            for (int i = 0; i <= 64; i++){
                const float t = (float)i / 64.0f;
                const Vector3 now = gradient.evaluateColor(t);
                Vector3 before(1, 1, 1);
                if (hasGradient){
                    before = legacyGradient(L.colorGradient, t);
                }else if (colorMod.active()){
                    const Vector3 from = L.colorModifier.useSRGB ? Color::sRGBToLinear(L.colorModifier.fromColor) : L.colorModifier.fromColor;
                    const Vector3 to = L.colorModifier.useSRGB ? Color::sRGBToLinear(L.colorModifier.toColor) : L.colorModifier.toColor;
                    const Vector3 initial = (cmin + cmax) * 0.5f;
                    before = Vector3(legacyValue(colorMod, t, life, initial.x, from.x, to.x), legacyValue(colorMod, t, life, initial.y, from.y, to.y),
                                     legacyValue(colorMod, t, life, initial.z, from.z, to.z));
                }
                const Vector3 d = now - before;
                colorError = std::max(colorError, std::max(std::fabs(d.x), std::max(std::fabs(d.y), std::fabs(d.z))));
            }
        }
        if (!colorKeys.empty()){
            const bool exact = colorError <= 1e-3f && (hasGradient || minLife == maxLife);
            add(report, hasGradient ? "colorGradient" : "colorModifier", exact ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
                exact ? "colorOverLifetime gradient" : "colorOverLifetime gradient (max colour difference " + number(colorError) + ")", exact ? 0.0f : colorError);
        }
        if (!alphaKeys.empty()){
            float gradientAlphaError = 0.0f;
            for (float life : {minLife, (minLife + maxLife) * 0.5f, maxLife}){
                for (int i = 0; i <= 64; i++){
                    const float t = (float)i / 64.0f;
                    gradientAlphaError = std::max(gradientAlphaError, std::fabs(gradient.evaluateAlpha(t) -
                        legacyValue(alphaMod, t, life, (amin + amax) * 0.5f, L.alphaModifier.fromAlpha, L.alphaModifier.toAlpha)));
                }
            }
            alphaError = std::max(alphaError, gradientAlphaError);
            const bool exact = alphaError <= EXACT_TOLERANCE * 10.0f;
            add(report, "alphaModifier", exact ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
                exact ? "colorOverLifetime alpha" : "colorOverLifetime alpha (max difference " + number(alphaError) + ")", exact ? 0.0f : alphaError);
        }
    }

    // ---- size (Points: the size; meshes: the scale)
    if (targetIsPoints){
        const Modifier m = modifier(L.sizeModifier);
        if (m.active()){
            float error = 0.0f;
            const float initial = (L.sizeInitializer.minSize + L.sizeInitializer.maxSize) * 0.5f;
            AnimationCurve curve = modifierCurve(m, minLife, maxLife, initial, L.sizeModifier.fromSize, L.sizeModifier.toSize, error);
            c.sizeOverLifetime.enabled = true;
            c.sizeOverLifetime.size = MinMaxCurve::fromCurve(curve, 1.0f);
            if (m.fromTime <= 0.0f){
                main.startSize = MinMaxCurve(1.0f);
                add(report, "sizeInitializer", UpgradeStatus::Exact, "replaced by the size modifier from the start");
            }else{
                // the modifier sets absolute sizes: they are multiplied by the start size
                main.startSize = MinMaxCurve(1.0f);
                error = std::max(error, std::fabs(L.sizeInitializer.maxSize - L.sizeInitializer.minSize) * 0.5f);
            }
            const bool exact = error <= EXACT_TOLERANCE;
            add(report, "sizeModifier", exact ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
                exact ? "sizeOverLifetime curve" : "sizeOverLifetime curve at the mean life (max difference " + number(error) + ")", exact ? 0.0f : error);
        }else{
            main.startSize = range(L.sizeInitializer.minSize, L.sizeInitializer.maxSize);
            add(report, "sizeInitializer", UpgradeStatus::Exact, "main.startSize");
        }
    }else{
        const ParticleScaleInitializer& s = L.scaleInitializer;
        const bool uniform = s.minScale.x == s.minScale.y && s.minScale.y == s.minScale.z && s.maxScale.x == s.maxScale.y && s.maxScale.y == s.maxScale.z;
        if (uniform){
            main.startSize = range(s.minScale.x, s.maxScale.x);
            add(report, "scaleInitializer", UpgradeStatus::Exact, "main.startSize");
        }else{
            main.startSize3D = true;
            main.startSize = range(s.minScale.x, s.maxScale.x);
            main.startSizeY = range(s.minScale.y, s.maxScale.y);
            main.startSizeZ = range(s.minScale.z, s.maxScale.z);
            const bool exact = s.minScale == s.maxScale || !s.linearSort;
            add(report, "scaleInitializer", exact ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
                exact ? "main.startSize 3D" : "main.startSize 3D: each axis has its own random (the legacy one shared one)");
        }
        const Modifier m = modifier(L.scaleModifier);
        if (m.active()){
            float ex = 0, ey = 0, ez = 0;
            const Vector3 initial = (s.minScale + s.maxScale) * 0.5f;
            const ParticleScaleModifier& sm = L.scaleModifier;
            AnimationCurve cx = modifierCurve(m, minLife, maxLife, initial.x, sm.fromScale.x, sm.toScale.x, ex);
            AnimationCurve cy = modifierCurve(m, minLife, maxLife, initial.y, sm.fromScale.y, sm.toScale.y, ey);
            AnimationCurve cz = modifierCurve(m, minLife, maxLife, initial.z, sm.fromScale.z, sm.toScale.z, ez);
            c.sizeOverLifetime.enabled = true;
            c.sizeOverLifetime.separateAxes = true;
            c.sizeOverLifetime.size = MinMaxCurve::fromCurve(cx, 1.0f);
            c.sizeOverLifetime.y = MinMaxCurve::fromCurve(cy, 1.0f);
            c.sizeOverLifetime.z = MinMaxCurve::fromCurve(cz, 1.0f);
            main.startSize3D = false;
            main.startSize = MinMaxCurve(1.0f);
            float error = std::max(ex, std::max(ey, ez));
            if (m.fromTime > 0.0f) error = std::max(error, (s.maxScale - s.minScale).length() * 0.5f);
            const bool exact = error <= EXACT_TOLERANCE;
            add(report, "scaleModifier", exact ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
                exact ? "sizeOverLifetime (separate axes)" : "sizeOverLifetime (separate axes) at the mean life (max difference " + number(error) + ")", exact ? 0.0f : error);
        }
    }

    // ---- rotation
    {
        const ParticleRotationInitializer& r = L.rotationInitializer;
        if (targetIsPoints){
            const float lo = Angle::defaultToRad(r.minRotation.getRoll());
            const float hi = Angle::defaultToRad(r.maxRotation.getRoll());
            main.startRotation = range(lo, hi);
            add(report, "rotationInitializer", UpgradeStatus::Exact, "main.startRotation");
        }else if (!(r.minRotation == Quaternion()) || !(r.maxRotation == Quaternion())){
            const Vector3 lo = eulerOf(r.minRotation), hi = eulerOf(r.maxRotation);
            main.startRotation3D = true;
            main.startRotationX = range(lo.x, hi.x);
            main.startRotationY = range(lo.y, hi.y);
            main.startRotation = range(lo.z, hi.z);
            int axes = (lo.x != hi.x) + (lo.y != hi.y) + (lo.z != hi.z);
            add(report, "rotationInitializer", axes <= 1 ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
                axes <= 1 ? "main.startRotation 3D" : "main.startRotation 3D: per-axis randoms instead of one slerp between the two rotations");
        }
        const Modifier m = modifier(L.rotationModifier);
        if (m.active()){
            // an absolute rotation over time becomes its angular speed (the roll's, or each axis')
            const ParticleRotationModifier& rm = L.rotationModifier;
            const float life = (minLife + maxLife) * 0.5f;
            const Vector3 from = targetIsPoints ? Vector3(0, 0, Angle::defaultToRad(rm.fromRotation.getRoll())) : eulerOf(rm.fromRotation);
            const Vector3 to = targetIsPoints ? Vector3(0, 0, Angle::defaultToRad(rm.toRotation.getRoll())) : eulerOf(rm.toRotation);
            const float span = std::max(1e-4f, m.toTime - m.fromTime);
            std::vector<float> times = keyTimes(m, life);
            std::vector<float> vx, vy, vz;
            for (float t : times){
                const float time = t * life;
                const float h = 1e-3f;
                float rate = 0.0f;
                if (time >= m.fromTime && time <= m.toTime){
                    const float u0 = std::max(0.0f, (time - m.fromTime) / span - h), u1 = std::min(1.0f, (time - m.fromTime) / span + h);
                    Ease e0 = m.function, e1 = m.function;
                    rate = (e1.call(u1) - e0.call(u0)) / ((u1 - u0) * span);
                }
                vx.push_back((to.x - from.x) * rate);
                vy.push_back((to.y - from.y) * rate);
                vz.push_back((to.z - from.z) * rate);
            }
            c.rotationOverLifetime.enabled = true;
            c.rotationOverLifetime.separateAxes = !targetIsPoints && (from.x != to.x || from.y != to.y);
            c.rotationOverLifetime.x = MinMaxCurve::fromCurve(linearCurve(times, vx), 1.0f);
            c.rotationOverLifetime.y = MinMaxCurve::fromCurve(linearCurve(times, vy), 1.0f);
            c.rotationOverLifetime.z = MinMaxCurve::fromCurve(linearCurve(times, vz), 1.0f);
            if (m.fromTime <= 0.0f){
                main.startRotation3D = !targetIsPoints && (from.x != 0.0f || from.y != 0.0f);
                main.startRotation = MinMaxCurve(from.z);
                main.startRotationX = MinMaxCurve(from.x);
                main.startRotationY = MinMaxCurve(from.y);
            }
            add(report, "rotationModifier", UpgradeStatus::Approximated,
                "rotationOverLifetime: the modifier's angular speed at the mean life (a slerp between two rotations is not an Euler speed)");
        }
    }

    // ---- sprite frames
    if (!L.spriteInitializer.frames.empty() || modifier(L.spriteModifier).active()){
        ParticleTextureSheetAnimationModule& sheet = c.textureSheetAnimation;
        sheet.enabled = true;
        sheet.mode = ParticleSystemAnimationMode::Sprites;
        const Modifier m = modifier(L.spriteModifier);
        if (m.active() && !L.spriteModifier.frames.empty()){
            sheet.sprites = L.spriteModifier.frames;
            float error = 0.0f;
            AnimationCurve curve = modifierCurve(m, minLife, maxLife, 0.0f, 0.0f, 0.9999f, error);
            sheet.frameOverTime = MinMaxCurve::fromCurve(curve, 1.0f);
            sheet.timeMode = ParticleSystemAnimationTimeMode::Lifetime;
            const bool exact = error <= 1.0f / (float)std::max<size_t>(1, sheet.sprites.size()) * 0.5f && L.spriteInitializer.frames.empty();
            add(report, "spriteModifier", exact ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
                exact ? "textureSheetAnimation (sprites)" : "textureSheetAnimation (sprites) at the mean life", exact ? 0.0f : error);
        }else{
            sheet.sprites = L.spriteInitializer.frames;
            sheet.frameOverTime = MinMaxCurve(0.0f);
            sheet.startFrame = MinMaxCurve::between(0.0f, (float)sheet.sprites.size() - 0.001f);
            add(report, "spriteInitializer", UpgradeStatus::Exact, "textureSheetAnimation (sprites, a random start frame)");
        }
    }

    // ---- what has no module
    const Modifier positionMod = modifier(L.positionModifier);
    if (positionMod.active()){
        add(report, "positionModifier", UpgradeStatus::Dropped, "a position over time has no Unity module equivalent");
        if (!force){
            report.upgraded = false;
        }
    }
    const Modifier velocityMod = modifier(L.velocityModifier);
    if (velocityMod.active()){
        // an absolute velocity over time: the velocity over lifetime curves, the initializer's
        // values before it starts
        const ParticleVelocityModifier& vm = L.velocityModifier;
        const Vector3 initial = (vel.minVelocity + vel.maxVelocity) * 0.5f;
        float ex = 0, ey = 0, ez = 0;
        ParticleVelocityOverLifetimeModule& v = c.velocityOverLifetime;
        v.enabled = true;
        v.space = ParticleSystemSimulationSpace::Local;
        v.x = MinMaxCurve::fromCurve(modifierCurve(velocityMod, minLife, maxLife, initial.x, vm.fromVelocity.x, vm.toVelocity.x, ex), 1.0f);
        v.y = MinMaxCurve::fromCurve(modifierCurve(velocityMod, minLife, maxLife, initial.y, vm.fromVelocity.y, vm.toVelocity.y, ey), 1.0f);
        v.z = MinMaxCurve::fromCurve(modifierCurve(velocityMod, minLife, maxLife, initial.z, vm.fromVelocity.z, vm.toVelocity.z, ez), 1.0f);
        add(report, "velocityModifier", UpgradeStatus::Approximated,
            "velocityOverLifetime curves (an animated velocity: the acceleration no longer adds to it)", std::max(ex, std::max(ey, ez)));
    }
    const Modifier accelerationMod = modifier(L.accelerationModifier);
    if (accelerationMod.active()){
        const ParticleAccelerationModifier& am = L.accelerationModifier;
        const Vector3 initial = (acc.minAcceleration + acc.maxAcceleration) * 0.5f;
        float ex = 0, ey = 0, ez = 0;
        ParticleForceOverLifetimeModule& f = c.forceOverLifetime;
        f.enabled = true;
        f.space = ParticleSystemSimulationSpace::Local;
        f.x = MinMaxCurve::fromCurve(modifierCurve(accelerationMod, minLife, maxLife, initial.x, am.fromAcceleration.x, am.toAcceleration.x, ex), 1.0f);
        f.y = MinMaxCurve::fromCurve(modifierCurve(accelerationMod, minLife, maxLife, initial.y, am.fromAcceleration.y, am.toAcceleration.y, ey), 1.0f);
        f.z = MinMaxCurve::fromCurve(modifierCurve(accelerationMod, minLife, maxLife, initial.z, am.fromAcceleration.z, am.toAcceleration.z, ez), 1.0f);
        const float error = std::max(ex, std::max(ey, ez));
        const bool exact = error <= EXACT_TOLERANCE;
        add(report, "accelerationModifier", exact ? UpgradeStatus::Exact : UpgradeStatus::Approximated,
            exact ? "forceOverLifetime curves" : "forceOverLifetime curves at the mean life (max difference " + number(error) + ")", exact ? 0.0f : error);
    }

    if (!report.upgraded){
        result.component = legacy;
    }
    return result;
}
