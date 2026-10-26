// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LegacyParticleConverter.h"

#include "math/Matrix4.h"
#include "util/Angle.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float kRadToDeg = 57.29577951308232f;

    MinMaxCurve range(float lo, float hi){
        return lo == hi ? MinMaxCurve(lo) : MinMaxCurve::twoConstants(lo, hi);
    }

    bool active(float from, float to){ return from != to; }

    // the Euler angles (degrees, the Ry Rx Rz order of the particle start rotation) of a quaternion
    Vector3 eulerOf(const Quaternion& q){
        const Matrix4 m = q.getRotationMatrix();
        // column-major: m[col][row]
        const float r12 = m[2][1];
        const float x = std::asin(std::clamp(-r12, -1.0f, 1.0f));
        const float y = std::atan2(m[2][0], m[2][2]);
        const float z = std::atan2(m[0][1], m[1][1]);
        return Vector3(x * kRadToDeg, y * kRadToDeg, z * kRadToDeg);
    }

    float rollDegrees(const Quaternion& q){
        return Angle::defaultToRad(q.getRoll()) * kRadToDeg;
    }

    Vector4 v4(const Vector3& v, float w = 0.0f){ return Vector4(v.x, v.y, v.z, w); }

    void addEntry(std::vector<SetOverAgeEntry>& entries, SetProperty property, float from, float to, const Ease& ease,
                  const Vector4& a, const Vector4& b){
        SetOverAgeEntry e;
        e.property = property;
        e.fromTime = from;
        e.toTime = to;
        e.curve = LegacyParticleConverter::sampleEase(ease);
        e.from = a;
        e.to = b;
        entries.push_back(e);
    }

    // an ease that evaluates a curve (the old view of a Set over Age entry)
    Ease easeOf(const Curve& curve){
        if (curve == Curve::linear(0.0f, 1.0f))
            return Ease(EaseType::LINEAR);
        return Ease(std::function<float(float)>([curve](float t){ return curve.evaluate(t); }));
    }

    const SetOverAgeEntry* findEntry(const ParticleEffectDocument& d, SetProperty property){
        for (const SetOverAgeEntry& e : d.setOverAge.entries){
            if (e.property == property)
                return &e;
        }
        return nullptr;
    }

}

float LegacyParticleConverter::cycleDuration(const LegacyParticleSystem& p){
    // ActionSystem::getParticleCycleDuration before S20
    float cycle = 0.0f;
    if (p.rate > 0)
        cycle = std::max(cycle, (float)p.maxParticles / (float)p.rate);
    for (const ParticleBurst& b : p.bursts)
        cycle = std::max(cycle, b.time);
    if (cycle <= 0.0f && !p.bursts.empty())
        cycle = std::max(0.0001f, p.lifeInitializer.maxLife);
    return cycle;
}

Curve LegacyParticleConverter::sampleEase(const Ease& ease){
    if (ease.getType() == EaseType::LINEAR)
        return Curve::linear(0.0f, 1.0f);
    constexpr int kKeys = 16;
    constexpr float h = 1.0e-3f;
    Curve curve;
    auto f = [&](float t){ return const_cast<Ease&>(ease).call(std::clamp(t, 0.0f, 1.0f)); };
    for (int k = 0; k < kKeys; k++){
        const float t = (float)k / (float)(kKeys - 1);
        CurveKey key;
        key.time = t;
        key.value = f(t);
        // the slopes on each side, from the function
        key.inTangent = k > 0 ? (key.value - f(t - h)) / h : (f(t + h) - key.value) / h;
        key.outTangent = k + 1 < kKeys ? (f(t + h) - key.value) / h : key.inTangent;
        curve.keys.push_back(key);
    }
    // a jump inside a segment (STEP): hold the left value
    for (int k = 0; k + 1 < kKeys; k++){
        CurveKey& a = curve.keys[k];
        const CurveKey& b = curve.keys[k + 1];
        const float mid = f(0.5f * (a.time + b.time));
        if (std::fabs(b.value - a.value) > 1.0e-4f && std::fabs(mid - a.value) < 1.0e-6f && std::fabs(a.outTangent) < 1.0e-3f){
            a.outTangent = INFINITY;
            curve.keys[k + 1].inTangent = INFINITY;
        }
    }
    return curve;
}

void LegacyParticleConverter::apply(const LegacyParticleSystem& p, ParticleEffectDocument& d, std::vector<std::string>* notes){
    auto note = [&](const std::string& line){ if (notes) notes->push_back(line); };
    d.maxParticles = std::max(p.maxParticles, 1u);
    d.looping = p.loop;
    d.duration = std::max(cycleDuration(p), 0.05f);
    d.simulationSpace = p.localSpace ? SimulationSpace::Local : SimulationSpace::World;
    // the old step: every frame, sizes in world units whatever the hierarchy's scale
    d.tickRate = 60;
    d.interpolate = true;
    d.scalingMode = ScalingMode::Shape;
    d.startDelay = 0.0f;
    d.prewarm = false;

    // emission
    d.emission.present = true;
    d.emission.enabled = true;
    d.emission.rateOverTime = MinMaxCurve((float)std::max(p.rate, 0));
    d.emission.rateOverDistance = MinMaxCurve(0.0f);
    d.emission.maxPerTick = std::max(p.maxPerUpdate, 0);
    d.emission.bursts.clear();
    std::vector<ParticleBurst> bursts = p.bursts;
    std::stable_sort(bursts.begin(), bursts.end(), [](const ParticleBurst& a, const ParticleBurst& b){ return a.time < b.time; });
    for (const ParticleBurst& b : bursts){
        if (d.emission.bursts.size() >= kMaxBursts){
            note("more than " + std::to_string(kMaxBursts) + " bursts: the later ones are left out");
            break;
        }
        Burst burst;
        burst.time = std::max(b.time, 0.0f);
        const int lo = std::max(0, b.minCount), hi = std::max(lo, b.maxCount);
        burst.count = range((float)lo, (float)hi);
        burst.cycles = 1;
        burst.probability = 1.0f;
        d.emission.bursts.push_back(burst);
    }

    // start values
    MainModule& m = d.main;
    m.startLifetime = range(std::max(p.lifeInitializer.minLife, 0.001f), std::max(p.lifeInitializer.maxLife, 0.001f));
    m.startSpeed = MinMaxCurve(0.0f);
    m.gravityModifier = MinMaxCurve(0.0f);
    m.inheritVelocity = MinMaxCurve(0.0f);
    m.startSizeFromTexture = p.sizeInitializer.minSize == 0.0f && p.sizeInitializer.maxSize == 0.0f;
    m.startSize3D = false;
    m.startSize = range(p.sizeInitializer.minSize, p.sizeInitializer.maxSize);
    // colour: sRGB values as they were (linear ones encoded)
    auto encoded = [&](const Vector3& c, bool srgb){ return srgb ? c : Color::linearTosRGB(c); };
    const Vector3 cMin = encoded(p.colorInitializer.minColor, p.colorInitializer.useSRGB);
    const Vector3 cMax = encoded(p.colorInitializer.maxColor, p.colorInitializer.useSRGB);
    m.startColor = MinMaxGradient();
    m.startColor.color = Vector4(cMin, p.alphaInitializer.minAlpha);
    m.startColor.colorMax = Vector4(cMax, p.alphaInitializer.maxAlpha);
    m.startColor.mode = m.startColor.color == m.startColor.colorMax ? GradientValueMode::Color : GradientValueMode::TwoColors;
    if (p.colorInitializer.minColor != p.colorInitializer.maxColor)
        note("the start colour picks one random value for its three channels (the old one picked each channel)");
    m.startRotation3D = false;
    m.startRotationX = MinMaxCurve(0.0f);
    m.startRotationY = MinMaxCurve(0.0f);
    m.startRotation = range(rollDegrees(p.rotationInitializer.minRotation), rollDegrees(p.rotationInitializer.maxRotation));

    // shape (the old shapes point along +Y, as ours do)
    ShapeModule& s = d.shape;
    s = ShapeModule();
    s.present = true;
    s.enabled = true;
    const ParticlePositionInitializer& pi = p.positionInitializer;
    const float radius = std::max(0.0f, pi.radius);
    const float inner = std::min(std::max(0.0f, pi.innerRadius), radius);
    s.radius = radius;
    s.radiusThickness = radius > 0.0f ? 1.0f - inner / radius : 1.0f;
    switch (pi.shape){
        case ParticleEmitterShape::Sphere: s.shape = ShapeType::Sphere; break;
        case ParticleEmitterShape::Hemisphere: s.shape = ShapeType::Hemisphere; break;
        case ParticleEmitterShape::Circle: s.shape = ShapeType::Circle; break;
        case ParticleEmitterShape::Cone:
            // the apex at the origin, filled by volume
            s.shape = ShapeType::Cone;
            s.radius = 0.0f;
            s.radiusThickness = 1.0f;
            s.angle = std::min(std::max(0.0f, pi.coneAngle), 89.0f);
            s.length = std::max(0.0f, pi.coneHeight);
            s.emitFrom = ShapeEmitFrom::Volume;
            break;
        case ParticleEmitterShape::Box:
        default:
            s.shape = ShapeType::Box;
            s.emitFrom = ShapeEmitFrom::Volume;
            s.position = (pi.minPosition + pi.maxPosition) * 0.5f;
            s.scale = pi.maxPosition - pi.minPosition;
            s.scale = Vector3(std::fabs(s.scale.x), std::fabs(s.scale.y), std::fabs(s.scale.z));
            break;
    }

    // velocities and the particle's own acceleration
    d.startVelocity.present = true;
    d.startVelocity.enabled = true;
    d.startVelocity.min = p.velocityInitializer.minVelocity;
    d.startVelocity.max = p.velocityInitializer.maxVelocity;
    d.startVelocity.uniform = false;
    const Vector3& aMin = p.accelerationInitializer.minAcceleration;
    const Vector3& aMax = p.accelerationInitializer.maxAcceleration;
    const bool hasAcceleration = aMin != Vector3(0, 0, 0) || aMax != Vector3(0, 0, 0) ||
                                 active(p.accelerationModifier.fromTime, p.accelerationModifier.toTime);
    d.forceOverLifetime = ForceOverLifetimeModule();
    if (hasAcceleration){
        d.forceOverLifetime.present = true;
        d.forceOverLifetime.x = range(aMin.x, aMax.x);
        d.forceOverLifetime.y = range(aMin.y, aMax.y);
        d.forceOverLifetime.z = range(aMin.z, aMax.z);
        d.forceOverLifetime.space = ValueSpace::Local;
    }

    // modifiers: Set over Age entries
    SetOverAgeModule& set = d.setOverAge;
    set = SetOverAgeModule();
    std::vector<SetOverAgeEntry>& e = set.entries;
    if (active(p.positionModifier.fromTime, p.positionModifier.toTime))
        addEntry(e, SetProperty::Position, p.positionModifier.fromTime, p.positionModifier.toTime, p.positionModifier.function,
                 v4(p.positionModifier.fromPosition), v4(p.positionModifier.toPosition));
    if (active(p.velocityModifier.fromTime, p.velocityModifier.toTime))
        addEntry(e, SetProperty::Velocity, p.velocityModifier.fromTime, p.velocityModifier.toTime, p.velocityModifier.function,
                 v4(p.velocityModifier.fromVelocity), v4(p.velocityModifier.toVelocity));
    if (active(p.accelerationModifier.fromTime, p.accelerationModifier.toTime))
        addEntry(e, SetProperty::Acceleration, p.accelerationModifier.fromTime, p.accelerationModifier.toTime, p.accelerationModifier.function,
                 v4(p.accelerationModifier.fromAcceleration), v4(p.accelerationModifier.toAcceleration));
    const bool gradient = !p.colorGradient.stops.empty();
    if (!gradient && active(p.colorModifier.fromTime, p.colorModifier.toTime)){
        addEntry(e, SetProperty::Color, p.colorModifier.fromTime, p.colorModifier.toTime, p.colorModifier.function,
                 v4(p.colorModifier.fromColor), v4(p.colorModifier.toColor));
        e.back().srgb = p.colorModifier.useSRGB;
    }
    if (active(p.alphaModifier.fromTime, p.alphaModifier.toTime))
        addEntry(e, SetProperty::Alpha, p.alphaModifier.fromTime, p.alphaModifier.toTime, p.alphaModifier.function,
                 Vector4(p.alphaModifier.fromAlpha, 0, 0, 0), Vector4(p.alphaModifier.toAlpha, 0, 0, 0));
    if (active(p.sizeModifier.fromTime, p.sizeModifier.toTime))
        addEntry(e, SetProperty::Size, p.sizeModifier.fromTime, p.sizeModifier.toTime, p.sizeModifier.function,
                 Vector4(p.sizeModifier.fromSize, 0, 0, 0), Vector4(p.sizeModifier.toSize, 0, 0, 0));
    if (active(p.spriteModifier.fromTime, p.spriteModifier.toTime) && !p.spriteModifier.frames.empty()){
        addEntry(e, SetProperty::Frame, p.spriteModifier.fromTime, p.spriteModifier.toTime, p.spriteModifier.function, Vector4(), Vector4(0, 0, 0, 0));
        e.back().to = Vector4(1, 0, 0, 0);
        e.back().frames = p.spriteModifier.frames;
    }
    if (active(p.rotationModifier.fromTime, p.rotationModifier.toTime)){
        const Quaternion& a = p.rotationModifier.fromRotation;
        const Quaternion& b = p.rotationModifier.toRotation;
        addEntry(e, SetProperty::Rotation, p.rotationModifier.fromTime, p.rotationModifier.toTime, p.rotationModifier.function,
                 Vector4(a.x, a.y, a.z, a.w), Vector4(b.x, b.y, b.z, b.w));
        e.back().shortestPath = p.rotationModifier.shortestPath;
    }
    if (active(p.scaleModifier.fromTime, p.scaleModifier.toTime))
        addEntry(e, SetProperty::Scale, p.scaleModifier.fromTime, p.scaleModifier.toTime, p.scaleModifier.function,
                 v4(p.scaleModifier.fromScale), v4(p.scaleModifier.toScale));
    set.present = !e.empty();
    set.enabled = true;

    // the colour gradient: Color over Lifetime replacing the start colour's RGB
    d.colorOverLifetime = ColorOverLifetimeModule();
    if (gradient){
        ParticleColorGradient g = p.colorGradient;
        g.normalize();
        Gradient out;
        out.linear = !g.useSRGB;
        // at most 8 keys: more are resampled evenly
        if (g.stops.size() > Gradient::kMaxKeys){
            note("the colour gradient had " + std::to_string(g.stops.size()) + " stops: resampled to 8 keys");
            for (size_t k = 0; k < Gradient::kMaxKeys; k++){
                const float t = (float)k / (float)(Gradient::kMaxKeys - 1);
                // the old piecewise-linear value at t
                Vector3 c = g.stops.front().color;
                for (size_t s2 = 0; s2 + 1 < g.stops.size(); s2++){
                    const ParticleColorGradientStop& a = g.stops[s2];
                    const ParticleColorGradientStop& b = g.stops[s2 + 1];
                    if (t >= a.time && t <= b.time){
                        const float span = b.time - a.time;
                        c = a.color + (b.color - a.color) * (span > 0.0f ? (t - a.time) / span : 0.0f);
                        break;
                    }
                    if (t > b.time) c = b.color;
                }
                out.colorKeys.push_back({t, out.linear ? Color::linearTosRGB(c) : c});
            }
        }else{
            for (const ParticleColorGradientStop& stop : g.stops)
                out.colorKeys.push_back({stop.time, out.linear ? Color::linearTosRGB(stop.color) : stop.color});
        }
        d.colorOverLifetime.present = true;
        d.colorOverLifetime.enabled = true;
        d.colorOverLifetime.setMode = true;
        d.colorOverLifetime.color.mode = GradientValueMode::Gradient;
        d.colorOverLifetime.color.gradient = out;
    }

    // sprite frames: the start frame from the old list
    TextureSheetAnimationModule& ts = d.textureSheetAnimation;
    ts.startFrameList = p.spriteInitializer.frames;
    if (!p.spriteInitializer.frames.empty()){
        ts.present = true;
        ts.enabled = true;
        ts.mode = TextureSheetMode::Sprites;
        ts.timeMode = TextureSheetTimeMode::Lifetime;
        ts.frameOverTime = MinMaxCurve(0.0f);
        ts.startFrame = MinMaxCurve(0.0f);
        ts.cycles = 1.0f;
        ts.frameBlending = false;
    }
}

ParticleEffectDocument LegacyParticleConverter::convert(const LegacyParticleSystem& p, const LegacyTarget& target, std::vector<std::string>* notes){
    auto note = [&](const std::string& line){ if (notes) notes->push_back(line); };
    ParticleEffectDocument d;
    apply(p, d, notes);
    RendererModule& r = d.renderer;
    if (target.kind == LegacyTarget::Kind::Mesh){
        // the instanced mesh draws the particles with its own material
        r.renderMode = RenderMode::Mesh;
        MeshSource self;
        self.type = MeshSourceType::GameObject;
        r.meshes = {self};
        r.meshSelection = MeshSelection::Index;
        // 3D start rotation and scale
        d.main.startRotation3D = true;
        const Vector3 eMin = eulerOf(p.rotationInitializer.minRotation);
        const Vector3 eMax = eulerOf(p.rotationInitializer.maxRotation);
        d.main.startRotationX = range(eMin.x, eMax.x);
        d.main.startRotationY = range(eMin.y, eMax.y);
        d.main.startRotation = range(eMin.z, eMax.z);
        if (p.rotationInitializer.minRotation != p.rotationInitializer.maxRotation)
            note("the 3D start rotation is random per axis (the old one blended two quaternions)");
        d.main.startSize3D = true;
        d.main.startSize = range(p.scaleInitializer.minScale.x, p.scaleInitializer.maxScale.x);
        d.main.startSizeY = range(p.scaleInitializer.minScale.y, p.scaleInitializer.maxScale.y);
        d.main.startSizeZ = range(p.scaleInitializer.minScale.z, p.scaleInitializer.maxScale.z);
        d.main.startSizeLinked = p.scaleInitializer.linearSort;
        d.main.startSizeFromTexture = false;
    }else{
        r.renderMode = RenderMode::Billboard;
        // the points had no size cap but the device's point-size limit
        r.maxScreenSize = 10.0f;
        if (!target.shader.empty() && target.shader != "library:particle")
            note("the custom points shader \"" + target.shader + "\" is not a particles shader: library:particles draws them");
    }
    r.texture = target.texture;
    r.textureGuid = target.textureGuid;
    r.shader = "library:particles";
    r.shaderFeatures = target.shaderFeatures;
    r.shaderProperties = target.shaderProperties;
    r.shaderTextures = target.shaderTextures;
    if (target.blendOpaque){
        r.blend = ParticleBlend::Alpha;
        note("an opaque Points blend draws alpha blended");
    }else{
        r.blend = target.blend < 0 ? ParticleBlend::Alpha : (ParticleBlend)target.blend;
    }
    // the built-in points shader wrote depth; library:particle did not
    r.depthWrite = target.depthWrite == 1 || (target.depthWrite < 0 && target.shader.empty() && target.kind == LegacyTarget::Kind::Points);
    r.sortMode = (target.transparent && target.kind == LegacyTarget::Kind::Points &&
                  (r.blend == ParticleBlend::Alpha || r.blend == ParticleBlend::Premultiplied)) ? SortMode::ByDistance : SortMode::None;
    if (!target.frames.empty() && (d.textureSheetAnimation.present || (d.setOverAge.present && findEntry(d, SetProperty::Frame)))){
        d.textureSheetAnimation.sprites.assign(target.frames.begin(), target.frames.begin() + std::min(target.frames.size(), kMaxSpriteFrames));
        if (target.frames.size() > kMaxSpriteFrames)
            note("more than 64 sprite frames: the first 64 are kept");
        if (!d.textureSheetAnimation.present){
            // the frames only Set over Age picks: the module holds their rects, its animation off
            d.textureSheetAnimation.present = true;
            d.textureSheetAnimation.enabled = false;
            d.textureSheetAnimation.mode = TextureSheetMode::Sprites;
        }
    }
    return d;
}

LegacyParticleSystem LegacyParticleConverter::extract(const ParticleEffectDocument& d){
    LegacyParticleSystem p;
    p.maxParticles = d.maxParticles;
    p.loop = d.looping;
    p.localSpace = d.simulationSpace == SimulationSpace::Local;
    p.rate = (int)std::lround(d.emission.rateOverTime.constant);
    p.maxPerUpdate = d.emission.maxPerTick;
    for (const Burst& b : d.emission.bursts){
        const int lo = (int)std::lround(b.count.constant);
        const int hi = b.count.mode == CurveMode::TwoConstants ? (int)std::lround(b.count.constantMax) : lo;
        p.bursts.push_back({b.time, lo, hi});
    }
    auto lo = [](const MinMaxCurve& c){ return c.constant; };
    auto hi = [](const MinMaxCurve& c){ return c.mode == CurveMode::TwoConstants ? c.constantMax : c.constant; };
    p.lifeInitializer.minLife = lo(d.main.startLifetime);
    p.lifeInitializer.maxLife = hi(d.main.startLifetime);
    p.sizeInitializer.minSize = d.main.startSizeFromTexture ? 0.0f : lo(d.main.startSize);
    p.sizeInitializer.maxSize = d.main.startSizeFromTexture ? 0.0f : hi(d.main.startSize);
    const MinMaxGradient& c = d.main.startColor;
    const Vector4 cMax = c.mode == GradientValueMode::TwoColors ? c.colorMax : c.color;
    p.colorInitializer.minColor = Vector3(c.color.x, c.color.y, c.color.z);
    p.colorInitializer.maxColor = Vector3(cMax.x, cMax.y, cMax.z);
    p.colorInitializer.useSRGB = true;
    p.alphaInitializer.minAlpha = c.color.w;
    p.alphaInitializer.maxAlpha = cMax.w;
    p.rotationInitializer.minRotation = Quaternion(Angle::radToDefault(lo(d.main.startRotation) / kRadToDeg), Vector3(0, 0, 1));
    p.rotationInitializer.maxRotation = Quaternion(Angle::radToDefault(hi(d.main.startRotation) / kRadToDeg), Vector3(0, 0, 1));
    if (d.main.startSize3D){
        p.scaleInitializer.minScale = Vector3(lo(d.main.startSize), lo(d.main.startSizeY), lo(d.main.startSizeZ));
        p.scaleInitializer.maxScale = Vector3(hi(d.main.startSize), hi(d.main.startSizeY), hi(d.main.startSizeZ));
        p.scaleInitializer.linearSort = d.main.startSizeLinked;
    }

    const ShapeModule& s = d.shape;
    ParticlePositionInitializer& pi = p.positionInitializer;
    pi.radius = s.radius;
    pi.innerRadius = s.radius * (1.0f - s.radiusThickness);
    switch (s.shape){
        case ShapeType::Sphere: pi.shape = ParticleEmitterShape::Sphere; break;
        case ShapeType::Hemisphere: pi.shape = ParticleEmitterShape::Hemisphere; break;
        case ShapeType::Circle: pi.shape = ParticleEmitterShape::Circle; break;
        case ShapeType::Cone:
            pi.shape = ParticleEmitterShape::Cone;
            pi.coneAngle = s.angle;
            pi.coneHeight = s.length;
            break;
        default:
            pi.shape = ParticleEmitterShape::Box;
            pi.minPosition = s.position - s.scale * 0.5f;
            pi.maxPosition = s.position + s.scale * 0.5f;
            break;
    }
    if (d.startVelocity.present){
        p.velocityInitializer.minVelocity = d.startVelocity.min;
        p.velocityInitializer.maxVelocity = d.startVelocity.max;
    }
    if (d.forceOverLifetime.present){
        const ForceOverLifetimeModule& f = d.forceOverLifetime;
        p.accelerationInitializer.minAcceleration = Vector3(lo(f.x), lo(f.y), lo(f.z));
        p.accelerationInitializer.maxAcceleration = Vector3(hi(f.x), hi(f.y), hi(f.z));
    }
    auto readWindow = [&](SetProperty property, auto& modifier){
        const SetOverAgeEntry* e = findEntry(d, property);
        if (!e) return (const SetOverAgeEntry*)nullptr;
        modifier.fromTime = e->fromTime;
        modifier.toTime = e->toTime;
        modifier.function = easeOf(e->curve);
        return e;
    };
    auto v3 = [](const Vector4& v){ return Vector3(v.x, v.y, v.z); };
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Position, p.positionModifier)){ p.positionModifier.fromPosition = v3(e->from); p.positionModifier.toPosition = v3(e->to); }
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Velocity, p.velocityModifier)){ p.velocityModifier.fromVelocity = v3(e->from); p.velocityModifier.toVelocity = v3(e->to); }
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Acceleration, p.accelerationModifier)){ p.accelerationModifier.fromAcceleration = v3(e->from); p.accelerationModifier.toAcceleration = v3(e->to); }
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Color, p.colorModifier)){ p.colorModifier.fromColor = v3(e->from); p.colorModifier.toColor = v3(e->to); p.colorModifier.useSRGB = e->srgb; }
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Alpha, p.alphaModifier)){ p.alphaModifier.fromAlpha = e->from.x; p.alphaModifier.toAlpha = e->to.x; }
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Size, p.sizeModifier)){ p.sizeModifier.fromSize = e->from.x; p.sizeModifier.toSize = e->to.x; }
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Frame, p.spriteModifier)){ p.spriteModifier.frames = e->frames; }
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Rotation, p.rotationModifier)){
        p.rotationModifier.fromRotation = Quaternion(e->from.w, e->from.x, e->from.y, e->from.z);
        p.rotationModifier.toRotation = Quaternion(e->to.w, e->to.x, e->to.y, e->to.z);
        p.rotationModifier.shortestPath = e->shortestPath;
    }
    if (const SetOverAgeEntry* e = readWindow(SetProperty::Scale, p.scaleModifier)){ p.scaleModifier.fromScale = v3(e->from); p.scaleModifier.toScale = v3(e->to); }
    if (d.colorOverLifetime.present && d.colorOverLifetime.setMode && d.colorOverLifetime.color.mode == GradientValueMode::Gradient){
        const Gradient& g = d.colorOverLifetime.color.gradient;
        p.colorGradient.useSRGB = !g.linear;
        for (const GradientColorKey& k : g.colorKeys)
            p.colorGradient.stops.push_back({k.time, g.linear ? Color::sRGBToLinear(k.color) : k.color});
    }
    p.spriteInitializer.frames = d.textureSheetAnimation.startFrameList;
    return p;
}
