// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// fly / flyV and the seamless cycle follow pixelfx's src/core/particles.js, MIT License,
// Copyright (c) 2026 Patch. See NOTICE.md.

#include "AnalyticParticles.h"

#include "particles/ParticleKernels.h"
#include "particles/ParticleModules.h"
#include "particles/ParticleProgram.h"
#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float kTwoPi = 6.28318530718f;
    constexpr float kNoEnd = 1.0e30f;

    // ---------------------------------------------------------------- validation (R15)

    struct Validator {
        std::vector<Diagnostic>& out;
        bool ok = true;
        void unsupported(const std::string& path, const std::string& what){
            out.push_back({Diagnostic::Severity::Error, path, what + " is not computed in Analytic mode", "analytic_unsupported",
                           "disable it or use Simulated"});
            ok = false;
        }
        static bool on(bool present, bool enabled){ return present && enabled; }
        static bool constantOrRange(const MinMaxCurve& c){ return c.mode == CurveMode::Constant || c.mode == CurveMode::TwoConstants; }
        static bool constant(const MinMaxCurve& c){ return c.isConstant(); }
    };

    // ---------------------------------------------------------------- shared maths (the GPU's twin)

    float hash01(uint32_t key, uint32_t index){
        return random01(key, index);
    }

    float encodeChannel(float linear){ return linear <= 0.0f ? 0.0f : std::pow(std::min(linear, 1.0f), 1.0f / 2.2f); }
    float decodeChannel(float srgb){ return srgb <= 0.0f ? 0.0f : std::pow(srgb, 2.2f); }

    uint32_t packRgba(const float c[4]){
        auto byte = [](float v){ return (uint32_t)std::min(255.0f, std::max(0.0f, v * 255.0f + 0.5f)); };
        return byte(c[0]) | (byte(c[1]) << 8) | (byte(c[2]) << 16) | (byte(c[3]) << 24);
    }

    // a uniform direction on the unit sphere from two randoms (Kernels::unitSphere)
    Vector3 unitSphere(float u, float v){
        const float cy = u * 2.0f - 1.0f;
        const float r = std::sqrt(std::max(1.0f - cy * cy, 0.0f));
        const float a = v * kTwoPi;
        return Vector3(r * std::cos(a), cy, r * std::sin(a));
    }

    // the size (0) and fade (1) curves: 32 samples, 12 bits over the curve's range, two to a float
    float curveSample(const AnalyticBlock& b, int curve, int i){
        const int j = i >> 1;
        const uint32_t word = (uint32_t)b.curves[curve * 4 + (j >> 2)][j & 3];
        const uint32_t q = (i & 1) ? (word >> 12) : (word & 4095u);
        return b.curveRange[curve * 2] + b.curveRange[curve * 2 + 1] * (float)q;
    }

    // linearly interpolated over normalized age
    float curve32(const AnalyticBlock& b, int curve, float t){
        const float x = std::clamp(t, 0.0f, 1.0f) * 31.0f;
        const int i = std::min((int)x, 30);
        const float f = x - (float)i;
        const float a = curveSample(b, curve, i);
        const float c = curveSample(b, curve, i + 1);
        return a + (c - a) * f;
    }

    void bake32(const MinMaxCurve& c, AnalyticBlock& b, int curve){
        float v[kAnalyticCurveSamples];
        float lo = 3.0e38f, hi = -3.0e38f;
        for (int i = 0; i < kAnalyticCurveSamples; i++){
            const float t = (float)i / 31.0f;
            v[i] = c.mode == CurveMode::Constant ? c.constant : c.curve.evaluate(t) * c.multiplier;
            lo = std::min(lo, v[i]);
            hi = std::max(hi, v[i]);
        }
        const float step = hi > lo ? (hi - lo) / 4095.0f : 0.0f;
        b.curveRange[curve * 2] = lo;
        b.curveRange[curve * 2 + 1] = step;
        for (int j = 0; j < kAnalyticCurveSamples / 2; j++){
            auto quantise = [&](float x){ return step > 0.0f ? (uint32_t)std::clamp(std::lround((x - lo) / step), 0l, 4095l) : 0u; };
            b.curves[curve * 4 + (j >> 2)][j & 3] = (float)(quantise(v[2 * j]) | (quantise(v[2 * j + 1]) << 12));
        }
    }

    // the colour table's entry e, RGBA8 as two floats
    void putColor(AnalyticBlock& b, int e, pixelart::Rgba8 c){
        float* f = &b.colorTable[0][0];
        f[2 * e] = (float)(c.r + 256 * c.g);
        f[2 * e + 1] = (float)(c.b + 256 * c.a);
    }

    void getColor(const AnalyticBlock& b, int e, float out[4]){
        const float* f = &b.colorTable[0][0];
        const uint32_t rg = (uint32_t)f[2 * e], ba = (uint32_t)f[2 * e + 1];
        out[0] = (float)(rg & 255u) / 255.0f;
        out[1] = (float)(rg >> 8) / 255.0f;
        out[2] = (float)(ba & 255u) / 255.0f;
        out[3] = (float)(ba >> 8) / 255.0f;
    }

    uint8_t toByte(float v){
        return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
    }

    // The shape's start position and direction (Kernels::spawnShape for the shapes Analytic
    // supports), simulation space before the emitter
    void shapeSample(const AnalyticBlock& b, uint32_t key, uint32_t index, Vector3& p, Vector3& d){
        auto rnd = [&](uint32_t stream){ return hash01(key ^ (stream * 0x27d4eb2fu), index); };
        const int kind = (int)b.shape[0];
        const float radius = b.shape[1], inner = b.shape[2], arc = b.shape[3];
        const float tanAngle = b.shape2[0], length = b.shape2[1], donut = b.shape2[2];
        const int emitFrom = (int)b.shape2[3];
        p = Vector3(0.0f, 0.0f, 0.0f);
        d = Vector3(0.0f, 1.0f, 0.0f);
        const float angle = rnd(1) * arc;
        const float sa = std::sin(angle), ca = std::cos(angle);
        switch ((SpawnShapeKind)kind){
            case SpawnShapeKind::Sphere:
            case SpawnShapeKind::Hemisphere: {
                const float u = rnd(2);
                const float cy = (SpawnShapeKind)kind == SpawnShapeKind::Sphere ? u * 2.0f - 1.0f : u;
                const float rr = std::sqrt(std::max(1.0f - cy * cy, 0.0f));
                d = Vector3(rr * ca, cy, rr * sa);
                const float inner3 = inner * inner * inner;
                const float r = std::cbrt((1.0f - inner3) * rnd(3) + inner3) * radius;
                p = d * r;
                break;
            }
            case SpawnShapeKind::Circle: {
                const float inner2 = inner * inner;
                const float r = std::sqrt((1.0f - inner2) * rnd(3) + inner2) * radius;
                d = Vector3(ca, 0.0f, sa);
                p = Vector3(ca * r, 0.0f, sa * r);
                break;
            }
            case SpawnShapeKind::Cone: {
                const float inner2 = inner * inner;
                const bool shell = emitFrom == (int)ShapeEmitFrom::Shell || emitFrom == (int)ShapeEmitFrom::Edge;
                const float rho = shell ? 1.0f : std::sqrt((1.0f - inner2) * rnd(3) + inner2);
                const float tilt = rho * tanAngle;
                const float inv = 1.0f / std::sqrt(tilt * tilt + 1.0f);
                d = Vector3(ca * tilt * inv, inv, sa * tilt * inv);
                float h = 0.0f;
                if (emitFrom == (int)ShapeEmitFrom::Volume){
                    const float u = rnd(4);
                    if (tanAngle > 1.0e-6f){
                        const float r0 = radius, r1 = radius + length * tanAngle;
                        const float r03 = r0 * r0 * r0;
                        h = (std::cbrt((r1 * r1 * r1 - r03) * u + r03) - r0) / tanAngle;
                    }else{
                        h = u * length;
                    }
                }
                const float at = h * tanAngle + radius;
                p = Vector3(ca * rho * at, h, sa * rho * at);
                break;
            }
            case SpawnShapeKind::Box: {
                const float ux = rnd(2) * 2.0f - 1.0f, uy = rnd(3) * 2.0f - 1.0f, uz = rnd(4) * 2.0f - 1.0f;
                const float hx = b.shape3[0], hy = b.shape3[1], hz = b.shape3[2];
                if (emitFrom == (int)ShapeEmitFrom::Volume){
                    p = Vector3(ux * hx, uy * hy, uz * hz);
                }else{
                    const float pick = rnd(5);
                    const float sign = rnd(6) < 0.5f ? -1.0f : 1.0f;
                    float sx = ux, sy = uy, sz = uz;
                    if (emitFrom == (int)ShapeEmitFrom::Shell){
                        const float ax = hy * hz, ay = hx * hz, az = hx * hy, total = ax + ay + az;
                        const bool onX = pick < (total > 0.0f ? ax / total : 1.0f / 3.0f);
                        const bool onY = !onX && pick < (total > 0.0f ? (ax + ay) / total : 2.0f / 3.0f);
                        const bool onZ = !onX && !onY;
                        const float depth = rnd(7);
                        if (onX) sx = sign * ((1.0f - b.shape4[0]) * depth + b.shape4[0]);
                        if (onY) sy = sign * ((1.0f - b.shape4[1]) * depth + b.shape4[1]);
                        if (onZ) sz = sign * ((1.0f - b.shape4[2]) * depth + b.shape4[2]);
                    }else{
                        const float total = hx + hy + hz;
                        const bool alongX = pick < (total > 0.0f ? hx / total : 1.0f / 3.0f);
                        const bool alongY = !alongX && pick < (total > 0.0f ? (hx + hy) / total : 2.0f / 3.0f);
                        const bool alongZ = !alongX && !alongY;
                        const float sign2 = rnd(7) < 0.5f ? -1.0f : 1.0f;
                        sx = alongX ? ux : sign;
                        sy = alongY ? uy : (alongX ? sign : sign2);
                        sz = alongZ ? uz : sign2;
                    }
                    p = Vector3(sx * hx, sy * hy, sz * hz);
                }
                break;
            }
            case SpawnShapeKind::Donut: {
                const float tube = rnd(2) * kTwoPi;
                const float st = std::sin(tube), ct = std::cos(tube);
                const float inner2 = inner * inner;
                const float rt = std::sqrt((1.0f - inner2) * rnd(3) + inner2) * donut;
                d = Vector3(ca * ct, st, sa * ct);
                p = Vector3(d.x * rt + ca * radius, d.y * rt, d.z * rt + sa * radius);
                break;
            }
            case SpawnShapeKind::Edge:
                p = Vector3((rnd(2) * 2.0f - 1.0f) * radius, 0.0f, 0.0f);
                break;
            default:
                break;
        }
        if (b.shape4[3] > 0.0f){
            const Vector3 r = unitSphere(rnd(10), rnd(11));
            p = p + r * (rnd(12) * b.shape4[3]);
        }
        if (b.shapeRot[0][3] > 0.0f){
            const float len = p.length();
            const Vector3 s = len > 1.0e-20f ? p * (1.0f / len) : Vector3(0.0f, 0.0f, 0.0f);
            d = d + (s - d) * b.shapeRot[0][3];
        }
        if (b.shape3[3] > 0.0f){
            const Vector3 r = unitSphere(rnd(13), rnd(14));
            d = d + (r - d) * b.shape3[3];
        }
        const float len = d.length();
        d = len > 1.0e-20f ? d * (1.0f / len) : Vector3(0.0f, 0.0f, 0.0f);
        const Vector3 point = p, dir = d;
        for (int r = 0; r < 3; r++){
            const float* m = b.shapeXform[r];
            const float* q = b.shapeRot[r];
            (&p.x)[r] = m[0] * point.x + m[1] * point.y + m[2] * point.z + m[3];
            (&d.x)[r] = q[0] * dir.x + q[1] * dir.y + q[2] * dir.z;
        }
    }

    // Which particle a slot holds at the block's time: its random identity, its age and whether
    // it exists (born, not past the emission's end); `life` follows from the identity
    bool slotParticle(const AnalyticBlock& b, uint32_t slot, uint32_t& index, float& age){
        const uint32_t rateSlots = (uint32_t)b.slots[0];
        const uint32_t nLoop = (uint32_t)b.slots[2];
        const float rate = b.timing[0], duration = b.timing[1];
        const float phase = b.frame[0], loops = b.frame[1], emitEnd = b.frame[2];
        const bool bornAlways = b.frame[3] > 0.5f;
        const bool looping = nLoop > 0;
        if (slot < rateSlots){
            if (looping){
                // pixelfx's cycle: particle m of the loop born at m / rate, a copy per loop it outlives
                const uint32_t m = slot % nLoop, copy = slot / nLoop;
                float a = phase - (float)m / rate;
                if (a < 0.0f) a += duration;
                a += (float)copy * duration;
                const float absolute = loops * duration + phase;
                if (!bornAlways && absolute - a < 0.0f)
                    return false;
                if (absolute - a >= emitEnd)
                    return false;
                index = m;
                age = a;
                return true;
            }
            // the newest particle of the slot: n = s + C floor((t rate - s) / C)
            const float tr = phase * rate;
            if (tr < (float)slot)
                return false;
            const int c = (int)rateSlots;
            int n = (int)slot + c * (int)std::floor((tr - (float)slot) / (float)c);
            const float end = std::min(duration, emitEnd);
            const int total = (int)std::ceil(rate * end);
            if (n >= total)
                n -= c * ((n - total + c) / c);
            if (n < 0)
                return false;
            index = (uint32_t)n;
            age = phase - (float)n / rate;
            return age >= 0.0f;
        }
        // a burst's particle: the latest firing of its copy, in this loop or one it outlives
        uint32_t s = slot - rateSlots;
        for (int bi = 0; bi < kAnalyticMaxBursts; bi++){
            const float* burst = b.bursts[bi];
            const float* info = b.burstSlots[bi];
            const uint32_t countMax = (uint32_t)info[1];
            const uint32_t copies = (uint32_t)info[3];
            const uint32_t loopCopies = std::max((uint32_t)info[2], 1u);
            const uint32_t size = countMax * copies * loopCopies;
            if (s >= size){
                s -= size;
                continue;
            }
            const uint32_t j = s % countMax;
            const uint32_t q = (s / countMax) % copies;
            const uint32_t lc = s / (countMax * copies);
            const float time = burst[0], interval = burst[1], probability = burst[3];
            const int cyclesN = burst[2] > 0.5f ? (int)burst[2] : std::numeric_limits<int>::max();
            const float tEff = phase + (float)lc * duration;
            if (tEff < time)
                return false;
            if (looping && !bornAlways && (float)lc > loops)
                return false;
            const int cNow = std::min((int)std::floor((tEff - time) / interval), cyclesN - 1);
            const int qn = (int)copies;
            const int cycle = (int)q + qn * (int)std::floor((float)(cNow - (int)q) / (float)qn);
            if (cycle < 0)
                return false;
            const float fired = time + (float)cycle * interval;
            // a firing belongs to its loop (a looping effect repeats its bursts each loop)
            if (fired >= duration)
                return false;
            const float absolute = looping ? (loops - (float)lc) * duration + fired : fired;
            if (absolute >= emitEnd)
                return false;
            const uint32_t probabilityKey = analyticKey(b, 2, 2), countKey = analyticKey(b, 2, 3);
            const uint32_t firing = (uint32_t)bi * 65536u + (uint32_t)cycle;
            if (probability < 1.0f && hash01(probabilityKey, firing) >= probability)
                return false;
            const float countMin = info[0];
            // uniform over min..max, as Simulated draws it (EmitterRuntime's bursts)
            const float count = countMin == (float)countMax ? countMin
                                                            : std::min(std::floor(countMin + ((float)countMax - countMin + 1.0f) * hash01(countKey, firing)), (float)countMax);
            if ((float)j >= count)
                return false;
            index = 0x40000000u + (uint32_t)bi * 0x01000000u + (((uint32_t)cycle * countMax + j) & 0x00ffffffu);
            age = tEff - fired;
            return true;
        }
        return false;
    }

}

// ---------------------------------------------------------------- the public interface

namespace {

    // f = (1 - e^-kt) / k and h = (t - f) / k: below k t = 1 their Taylor series (ten terms, by
    // Horner: the next term is under 2e-9 of the sum), where the closed form cancels; above, the
    // closed form. The float32 twin (the GPU's formulas, operation for operation) and a double one
    // (the reference of the bounds and of AC9)
    template<typename T>
    void flyTerms(T k, T t, T& f, T& h, T& e){
        const T kt = k * t;
        e = std::exp(-kt);
        if (kt < T(1)){
            const T a = -kt;
            f = t * (T(1) + a * (T(1.0 / 2) + a * (T(1.0 / 6) + a * (T(1.0 / 24) + a * (T(1.0 / 120) + a * (T(1.0 / 720) + a * (T(1.0 / 5040) +
                    a * (T(1.0 / 40320) + a * (T(1.0 / 362880) + a * T(1.0 / 3628800))))))))));
            h = t * t * (T(1.0 / 2) + a * (T(1.0 / 6) + a * (T(1.0 / 24) + a * (T(1.0 / 120) + a * (T(1.0 / 720) + a * (T(1.0 / 5040) + a * (T(1.0 / 40320) +
                    a * (T(1.0 / 362880) + a * (T(1.0 / 3628800) + a * T(1.0 / 39916800))))))))));
        }else{
            f = (T(1) - e) / k;
            h = (t - f) / k;
        }
    }

}

Vector3 AnalyticParticles::flyPosition(const Vector3& p0, const Vector3& v0, const Vector3& g, float k, float t){
    double f, h, e;
    flyTerms<double>(k, t, f, h, e);
    return Vector3((float)(p0.x + v0.x * f + g.x * h), (float)(p0.y + v0.y * f + g.y * h), (float)(p0.z + v0.z * f + g.z * h));
}

Vector3 AnalyticParticles::flyVelocity(const Vector3& v0, const Vector3& g, float k, float t){
    double f, h, e;
    flyTerms<double>(k, t, f, h, e);
    return Vector3((float)(v0.x * e + g.x * f), (float)(v0.y * e + g.y * f), (float)(v0.z * e + g.z * f));
}

void AnalyticParticles::flyTwin(const Vector3& p0, const Vector3& v0, const Vector3& g, float k, float t, Vector3& position, Vector3& velocity){
    float f, h, e;
    flyTerms<float>(k, t, f, h, e);
    // the GPU's order: p0 + v0 * f + g * h, v0 * e + g * f
    position = Vector3(p0.x + v0.x * f + g.x * h, p0.y + v0.y * f + g.y * h, p0.z + v0.z * f + g.z * h);
    velocity = Vector3(v0.x * e + g.x * f, v0.y * e + g.y * f, v0.z * e + g.z * f);
}

float AnalyticParticles::loopRate(float rate, float duration){
    if (duration <= 0.0f || rate <= 0.0f)
        return 0.0f;
    return std::max(1.0f, std::floor(rate * duration + 0.5f)) / duration;
}

bool AnalyticParticles::validate(const ParticleEffectDocument& d, std::vector<Diagnostic>& out){
    Validator v{out};
    auto on = Validator::on;
    const MainModule& m = d.main;
    if (!Validator::constantOrRange(m.startLifetime)) v.unsupported("modules.main.startLifetime", "a Start Lifetime curve");
    if (!Validator::constantOrRange(m.startSpeed)) v.unsupported("modules.main.startSpeed", "a Start Speed curve");
    if (!Validator::constantOrRange(m.startSize)) v.unsupported("modules.main.startSize", "a Start Size curve");
    if (!Validator::constantOrRange(m.startRotation)) v.unsupported("modules.main.startRotation", "a Start Rotation curve");
    if (m.startSize3D) v.unsupported("modules.main.startSize3D", "3D Start Size");
    if (m.startRotation3D) v.unsupported("modules.main.startRotation3D", "3D Start Rotation");
    if (!m.inheritVelocity.isZero()) v.unsupported("modules.main.inheritVelocity", "Inherit Velocity");
    if (!Validator::constant(m.gravityModifier)) v.unsupported("modules.main.gravityModifier", "a Gravity Modifier that is not a constant");
    if (m.startColor.mode != GradientValueMode::Color && m.startColor.mode != GradientValueMode::TwoColors && m.startColor.mode != GradientValueMode::Ramp)
        v.unsupported("modules.main.startColor", "a Start Color gradient");
    if (!Validator::constant(d.startDelay)) v.unsupported("startDelay", "a random Start Delay");

    if (on(d.emission.present, d.emission.enabled)){
        if (!Validator::constant(d.emission.rateOverTime)) v.unsupported("modules.emission.rateOverTime", "a Rate over Time that is not a constant");
        if (!d.emission.rateOverDistance.isZero()) v.unsupported("modules.emission.rateOverDistance", "Rate over Distance");
        if (d.emission.bursts.size() > (size_t)kAnalyticMaxBursts) v.unsupported("modules.emission.bursts", "more than 4 bursts");
        for (size_t i = 0; i < d.emission.bursts.size(); i++){
            if (!Validator::constantOrRange(d.emission.bursts[i].count))
                v.unsupported("modules.emission.bursts[" + std::to_string(i) + "].count", "a burst count curve");
        }
    }
    if (on(d.shape.present, d.shape.enabled)){
        const ShapeModule& s = d.shape;
        if (s.shape == ShapeType::Mesh || s.shape == ShapeType::Rectangle) v.unsupported("modules.shape", "the Mesh and Rectangle shapes are");
        if (s.arcMode != ShapeMultiMode::Random || s.arcSpread > 0.0f) v.unsupported("modules.shape.arcMode", "an arc mode other than Random");
        if (s.radiusMode != ShapeMultiMode::Random || s.radiusSpread > 0.0f) v.unsupported("modules.shape.radiusMode", "a radius mode other than Random");
        if (s.alignToDirection) v.unsupported("modules.shape.alignToDirection", "Align to Direction");
    }
    const VelocityOverLifetimeModule& vol = d.velocityOverLifetime;
    if (on(vol.present, vol.enabled)){
        if (!Validator::constant(vol.x) || !Validator::constant(vol.y) || !Validator::constant(vol.z))
            v.unsupported("modules.velocityOverLifetime", "a Velocity over Lifetime that is not constant");
        if (!vol.orbitalX.isZero() || !vol.orbitalY.isZero() || !vol.orbitalZ.isZero() || !vol.radial.isZero() ||
                !(vol.speedModifier.isConstant() && vol.speedModifier.constant == 1.0f))
            v.unsupported("modules.velocityOverLifetime", "orbital, radial or speed-modified velocity");
    }
    const LimitVelocityModule& lv = d.limitVelocity;
    if (on(lv.present, lv.enabled)){
        if (lv.dampen > 0.0f || lv.separateAxes) v.unsupported("modules.limitVelocity.limit", "the speed limit (set Dampen to 0 to keep only the drag)");
        if (lv.quadraticDrag != 0.0f || lv.multiplyByVelocity) v.unsupported("modules.limitVelocity.quadraticDrag", "quadratic drag");
        if (!Validator::constant(lv.drag)) v.unsupported("modules.limitVelocity.drag", "a drag that is not a constant");
        if (lv.multiplyBySize && !lv.drag.isZero()) v.unsupported("modules.limitVelocity.multiplyBySize", "drag by size");
    }
    const ForceOverLifetimeModule& f = d.forceOverLifetime;
    if (on(f.present, f.enabled) && (!Validator::constant(f.x) || !Validator::constant(f.y) || !Validator::constant(f.z) || f.randomizePerTick))
        v.unsupported("modules.forceOverLifetime", "a Force over Lifetime that is not constant");
    const ColorOverLifetimeModule& col = d.colorOverLifetime;
    if (on(col.present, col.enabled)){
        const GradientValueMode mode = col.color.mode;
        if (col.setMode) v.unsupported("modules.colorOverLifetime.setMode", "Replace Start Color");
        if (mode != GradientValueMode::Color && mode != GradientValueMode::Gradient && mode != GradientValueMode::Ramp)
            v.unsupported("modules.colorOverLifetime.color", "a random colour over lifetime");
        if (col.color.driver != CurveDriver::NormalizedAge || col.color.wrap != CurveWrap::Clamp)
            v.unsupported("modules.colorOverLifetime.color.driver", "a driver other than Normalized Age");
        if (m.startColor.mode == GradientValueMode::Ramp && mode != GradientValueMode::Color)
            v.unsupported("modules.colorOverLifetime.color", "a second colour table (Start Color is a ramp already)");
    }
    const SizeOverLifetimeModule& sol = d.sizeOverLifetime;
    if (on(sol.present, sol.enabled) && (sol.separateAxes || (sol.size.mode != CurveMode::Constant && sol.size.mode != CurveMode::Curve) ||
                                         sol.size.driver != CurveDriver::NormalizedAge))
        v.unsupported("modules.sizeOverLifetime", "a random or per-axis Size over Lifetime");
    const RotationOverLifetimeModule& rol = d.rotationOverLifetime;
    if (on(rol.present, rol.enabled) && (rol.separateAxes || rol.angularVelocity.mode != CurveMode::Constant))
        v.unsupported("modules.rotationOverLifetime", "a Rotation over Lifetime that is not a constant");
    const TextureSheetAnimationModule& ts = d.textureSheetAnimation;
    if (on(ts.present, ts.enabled)){
        const bool linearFrames = ts.frameOverTime.mode == CurveMode::Curve && ts.frameOverTime == MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
        if (ts.mode != TextureSheetMode::Grid || ts.animation != TextureSheetAnimation::WholeSheet || ts.frameBlending ||
                ts.flipU > 0.0f || ts.flipV > 0.0f || !ts.startFrameList.empty() || !Validator::constant(ts.startFrame) ||
                ts.timeMode == TextureSheetTimeMode::Speed || (ts.timeMode == TextureSheetTimeMode::Lifetime && !linearFrames))
            v.unsupported("modules.textureSheetAnimation", "a sheet other than a whole Grid by Lifetime (linear) or FPS");
    }
    const struct { bool on; const char* key; const char* what; } others[] = {
        {on(d.noise.present, d.noise.enabled), "noise", "Noise"},
        {on(d.orbit.present, d.orbit.enabled), "orbit", "Orbit"},
        {on(d.externalForces.present, d.externalForces.enabled), "externalForces", "External Forces"},
        {on(d.collision.present, d.collision.enabled), "collision", "Collision"},
        {on(d.trails.present, d.trails.enabled), "trails", "Trails"},
        {on(d.subEmittersModule.present, d.subEmittersModule.enabled), "subEmitters", "Sub Emitters"},
        {on(d.customData.present, d.customData.enabled), "customData", "Custom Data"},
        {on(d.colorBySpeed.present, d.colorBySpeed.enabled), "colorBySpeed", "Color by Speed"},
        {on(d.sizeBySpeed.present, d.sizeBySpeed.enabled), "sizeBySpeed", "Size by Speed"},
        {on(d.rotationBySpeed.present, d.rotationBySpeed.enabled), "rotationBySpeed", "Rotation by Speed"},
        {on(d.setOverAge.present, d.setOverAge.enabled), "setOverAge", "Set over Age"},
        {on(d.startVelocity.present, d.startVelocity.enabled), "startVelocity", "Start Velocity"},
    };
    for (const auto& o : others){
        if (o.on)
            v.unsupported(std::string("modules.") + o.key, o.what);
    }
    const RendererModule& r = d.renderer;
    if (r.renderMode == RenderMode::Mesh || r.renderMode == RenderMode::None)
        v.unsupported("modules.renderer.renderMode", "Render Mode Mesh or None");
    if (r.sortMode != SortMode::None)
        v.unsupported("modules.renderer.sortMode", "sorting");
    return v.ok;
}

bool AnalyticParticles::compile(const ParticleEffectDocument& d, const ParticleProgram& prog, uint32_t seed, AnalyticProgram& out,
                                std::vector<Diagnostic>& diagnostics){
    out = AnalyticProgram();
    AnalyticBlock& b = out.block;
    const ShapeParams* shape = nullptr;
    const VelocityParams* velocity = nullptr;
    const StartParams* start = nullptr;
    for (const Op& op : prog.spawnOps){
        if (op.code == OpCode::SpawnShape) shape = &prog.param<ShapeParams>(op.params);
        if (op.code == OpCode::SpawnVelocity) velocity = &prog.param<VelocityParams>(op.params);
        if (op.code == OpCode::SpawnStartValues) start = &prog.param<StartParams>(op.params);
    }
    if (!shape || !velocity || !start)
        return false;
    auto key = [&](uint32_t stream){ return (int32_t)(seed ^ stream); };
    auto range = [](const CurveParam& c, float& lo, float& hi){ lo = c.c0; hi = c.kind == CurveParam::Range ? c.c1 : c.c0; };

    // timing: the rate (snapped for an exact loop) and the lifetimes
    float lifeMin, lifeMax;
    range(start->lifetime, lifeMin, lifeMax);
    lifeMin = std::max(lifeMin, 1.0e-4f);
    lifeMax = std::max(lifeMax, 1.0e-4f);
    const float longest = std::max(lifeMin, lifeMax);
    out.looping = d.looping;
    out.prewarm = d.looping && d.prewarm;
    out.duration = std::max(d.duration, 0.05f);
    float rate = (prog.emission.enabled && prog.emission.rate.kind == CurveParam::Const) ? std::max(prog.emission.rate.c0, 0.0f) : 0.0f;
    uint64_t rateSlots = 0;
    if (out.looping){
        rate = loopRate(rate, out.duration);
        out.loopCount = rate > 0.0f ? (uint32_t)std::floor(rate * out.duration + 0.5f) : 0u;
        out.copies = (uint32_t)std::max(1.0f, std::ceil(longest / out.duration));
        rateSlots = (uint64_t)out.loopCount * out.copies;
    }else if (rate > 0.0f){
        rateSlots = (uint64_t)std::ceil(rate * longest) + 1u;
    }
    out.rate = rate;
    // bursts: a copy for each firing still alive, and in a loop each loop it outlives
    uint64_t burstSlots = 0;
    for (size_t i = 0; i < prog.emission.bursts.size() && i < (size_t)kAnalyticMaxBursts; i++){
        const BurstParam& bp = prog.emission.bursts[i];
        float* burst = b.bursts[i];
        float* info = b.burstSlots[i];
        const float interval = std::max(bp.interval, 1.0e-4f);
        burst[0] = bp.time;
        burst[1] = interval;
        burst[2] = (float)std::max(bp.cycles, 0);
        burst[3] = std::clamp(bp.probability, 0.0f, 1.0f);
        const uint32_t countMax = (uint32_t)std::max(0.0f, std::floor(bp.countMax + 0.5f));
        info[0] = std::floor(std::max(bp.countMin, 0.0f) + 0.5f);
        info[1] = (float)countMax;
        double firings = bp.cycles > 0 ? (double)bp.cycles : 1.0e9;
        if (out.looping)
            firings = std::min(firings, bp.time < out.duration ? std::ceil((out.duration - bp.time) / interval) : 0.0);
        const double alive = std::ceil(longest / interval) + 1.0;
        const uint32_t copies = (uint32_t)std::max(1.0, std::min(firings, alive));
        const uint32_t loopCopies = out.looping ? (uint32_t)std::ceil(longest / out.duration) + 1u : 1u;
        if (firings <= 0.0){
            info[1] = 0.0f;
            continue;
        }
        // in a loop, only the firings inside one loop: the next loop's are its own
        if (out.looping)
            burst[2] = (float)firings;
        info[2] = (float)loopCopies;
        info[3] = (float)copies;
        burstSlots += (uint64_t)countMax * copies * loopCopies;
    }
    const uint64_t total = rateSlots + burstSlots;
    if (total > kAnalyticMaxSlots){
        diagnostics.push_back({Diagnostic::Severity::Error, "modules.emission", "the effect needs " + std::to_string(total) +
                               " Analytic slots, more than 65536", "analytic_capacity",
                               "lower the rate, the lifetime or the burst counts, or use Simulated"});
        return false;
    }
    out.rateSlots = (uint32_t)rateSlots;
    out.burstSlots = (uint32_t)burstSlots;
    out.slotCount = (uint32_t)total;
    b.slots[0] = (float)out.rateSlots;
    b.slots[1] = (float)out.burstSlots;
    b.slots[2] = (float)out.loopCount;
    b.slots[3] = (float)out.copies;
    b.timing[0] = rate;
    b.timing[1] = out.duration;
    b.timing[2] = start->lifetime.c0;
    b.timing[3] = start->lifetime.kind == CurveParam::Range ? start->lifetime.c1 : start->lifetime.c0;
    b.frame[2] = kNoEnd;
    b.frame[3] = out.prewarm ? 1.0f : 0.0f;

    // keys: the compiled streams' (S20's salts)
    setAnalyticKey(b, 0, 0, (uint32_t)key(start->lifetime.stream));
    setAnalyticKey(b, 0, 1, (uint32_t)key(velocity->startSpeed.stream));
    setAnalyticKey(b, 0, 2, (uint32_t)key(start->size.stream));
    setAnalyticKey(b, 0, 3, (uint32_t)key(start->rotation.stream));
    setAnalyticKey(b, 1, 0, (uint32_t)key(start->color.stream));
    setAnalyticKey(b, 1, 1, (uint32_t)key(randomSalt((uint32_t)ModuleId::Shape, 0)));
    setAnalyticKey(b, 1, 2, (uint32_t)key(randomSalt((uint32_t)ModuleId::Main, 60)));
    setAnalyticKey(b, 1, 3, (uint32_t)key(randomSalt((uint32_t)ModuleId::PixelArt, 1)));
    setAnalyticKey(b, 2, 0, (uint32_t)key(randomSalt((uint32_t)ModuleId::PixelArt, 2)));
    setAnalyticKey(b, 2, 1, (uint32_t)key(randomSalt((uint32_t)ModuleId::PixelArt, 3)));
    setAnalyticKey(b, 2, 2, (uint32_t)key(randomSalt((uint32_t)ModuleId::Emission, 120)));
    setAnalyticKey(b, 2, 3, (uint32_t)key(randomSalt((uint32_t)ModuleId::Emission, 121)));

    // start values
    range(velocity->startSpeed, b.start[0], b.start[1]);
    range(start->size, b.start[2], b.start[3]);
    range(start->rotation, b.start2[0], b.start2[1]);
    b.start2[2] = start->flipRotation;
    const RotationOverLifetimeModule& rol = d.rotationOverLifetime;
    b.start2[3] = (rol.present && rol.enabled) ? rol.angularVelocity.constant * (3.14159265358979f / 180.0f) : 0.0f;
    out.maxSize = std::max({std::fabs(b.start[2]), std::fabs(b.start[3]), 1.0f});

    // motion: g (gravity x modifier + constant force) and the drag, simulation space; the
    // runtime turns world-space parts into it each frame (setGravity)
    const LimitVelocityModule& lv = d.limitVelocity;
    b.motion[3] = (lv.present && lv.enabled) ? std::max(lv.drag.constant, 0.0f) : 0.0f;
    out.gravityModifier = d.main.gravityModifier.constant;
    const ForceOverLifetimeModule& fol = d.forceOverLifetime;
    if (fol.present && fol.enabled){
        out.force = Vector3(fol.x.constant, fol.y.constant, fol.z.constant);
        out.forceWorld = fol.space == ValueSpace::World;
    }
    const VelocityOverLifetimeModule& vol = d.velocityOverLifetime;
    if (vol.present && vol.enabled){
        out.velocity = Vector3(vol.x.constant, vol.y.constant, vol.z.constant);
        out.velocityWorld = vol.space == ValueSpace::World;
    }
    // world gravity down Y until the runtime gives the scene's (setMotion)
    {
        const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const float gravity[3] = {0.0f, -9.81f, 0.0f};
        setMotion(out, gravity, identity);
    }

    // colour: Start Color's mode, and the table Color over Lifetime or a ramp start reads
    const MinMaxGradient& sc = d.main.startColor;
    const ColorOverLifetimeModule& col = d.colorOverLifetime;
    float startKind = 0.0f, lifetimeKind = 0.0f;
    std::memcpy(b.color0, &sc.color.x, sizeof(b.color0));
    std::memcpy(b.color1, &sc.colorMax.x, sizeof(b.color1));
    auto putRamp = [&](const MinMaxGradient& g){
        const std::vector<pixelart::Rgba8> colors = g.resolveRamp();
        const int n = std::min((int)colors.size(), kAnalyticColorEntries);
        for (int i = 0; i < n; i++)
            putColor(b, i, colors[(size_t)i]);
        b.ramp[0] = (float)n;
        b.ramp[1] = std::clamp(g.rampBias, 0.1f, 10.0f);
        b.ramp[2] = g.rampReverse ? 1.0f : 0.0f;
        b.lifetimeRemap[2] = 1.0f;
        return n > 0;
    };
    if (sc.mode == GradientValueMode::TwoColors && sc.color != sc.colorMax){
        startKind = 1.0f;
    }else if (sc.mode == GradientValueMode::Ramp){
        if (putRamp(sc)){
            startKind = 2.0f;
            b.lifetimeRemap[3] = 1.0f;
        }else{
            b.color0[0] = b.color0[1] = b.color0[2] = b.color0[3] = 1.0f;
        }
    }
    if (col.present && col.enabled){
        const float span = col.color.driverMax - col.color.driverMin;
        b.lifetimeRemap[0] = span != 0.0f ? 1.0f / span : 0.0f;
        b.lifetimeRemap[1] = span != 0.0f ? -col.color.driverMin / span : 0.0f;
        if (col.color.mode == GradientValueMode::Ramp){
            if (putRamp(col.color)){
                lifetimeKind = 2.0f;
                b.lifetimeRemap[3] = 2.0f;
            }
        }else if (col.color.mode == GradientValueMode::Gradient){
            // the gradient's samples, sRGB-encoded bytes; a Fixed gradient is stepped (each
            // sample at the middle of its 1/32 of the life), a Blend one interpolated in linear
            const bool fixed = col.color.gradient.mode == GradientMode::Fixed;
            lifetimeKind = 1.0f;
            b.ramp[0] = (float)kAnalyticColorEntries;
            b.ramp[1] = 1.0f;
            b.ramp[2] = 0.0f;
            b.lifetimeRemap[2] = fixed ? 1.0f : 0.0f;
            for (int i = 0; i < kAnalyticColorEntries; i++){
                const float t = fixed ? ((float)i + 0.5f) / (float)kAnalyticColorEntries : (float)i / (float)(kAnalyticColorEntries - 1);
                const Vector4 c = col.color.gradient.evaluate(t);
                putColor(b, i, pixelart::Rgba8{toByte(encodeChannel(c.x)), toByte(encodeChannel(c.y)), toByte(encodeChannel(c.z)), toByte(c.w)});
            }
        }else if (!(col.color.color == Vector4(1.0f, 1.0f, 1.0f, 1.0f))){
            // a constant: a one-entry stepped table
            lifetimeKind = 2.0f;
            b.ramp[0] = 1.0f;
            b.ramp[1] = 1.0f;
            b.lifetimeRemap[2] = 1.0f;
            const Vector4& c = col.color.color;
            putColor(b, 0, pixelart::Rgba8{toByte(c.x), toByte(c.y), toByte(c.z), toByte(c.w)});
        }
    }
    b.velocity[3] = startKind + 4.0f * lifetimeKind;

    // the shape
    b.shape[0] = (float)(int)shape->kind;
    b.shape[1] = shape->radius;
    b.shape[2] = shape->innerRatio;
    b.shape[3] = shape->arc;
    b.shape2[0] = shape->tanAngle;
    b.shape2[1] = shape->length;
    b.shape2[2] = shape->donutRadius;
    b.shape2[3] = (float)(int)shape->emitFrom;
    for (int c = 0; c < 3; c++){
        b.shape3[c] = shape->boxHalf[c];
        b.shape4[c] = shape->boxInner[c];
        for (int k = 0; k < 4; k++) b.shapeXform[c][k] = shape->transform[k * 3 + c];
        for (int k = 0; k < 3; k++) b.shapeRot[c][k] = shape->rotation[k * 3 + c];
    }
    b.shape3[3] = shape->randomizeDirection;
    b.shape4[3] = shape->randomizePosition;
    b.shapeRot[0][3] = shape->spherizeDirection;

    b.extra[0] = d.renderer.emissionIntensity;

    // the sheet
    const TextureSheetAnimationModule& ts = d.textureSheetAnimation;
    if (ts.present && ts.enabled){
        b.sheet[0] = ts.timeMode == TextureSheetTimeMode::FPS ? 2.0f : 1.0f;
        b.sheet[1] = (float)std::max(ts.tilesX * ts.tilesY, 1);
        b.sheet[2] = ts.timeMode == TextureSheetTimeMode::FPS ? ts.fps : ts.cycles;
        b.sheet[3] = ts.startFrame.constant;
    }

    // curves
    const SizeOverLifetimeModule& sol = d.sizeOverLifetime;
    if (sol.present && sol.enabled){
        b.ramp[3] = 1.0f;
        bake32(sol.size, b, 0);
    }
    const PixelArtModule& pa = d.pixelArt;
    if (pa.present && pa.enabled){
        b.wobble[0] = pa.wobbleAmplitude.x;
        b.wobble[1] = pa.wobbleAmplitude.y;
        b.wobble[2] = pa.wobbleFrequency.x;
        b.wobble[3] = pa.wobbleFrequency.y;
        b.pixel[0] = std::clamp(pa.flickerBelow, 0.0f, 1.0f);
        b.pixel[1] = std::max(pa.flickerRate, 0.1f);
        b.pixel[2] = pa.wobbleRandomPhase ? 1.0f : 0.0f;
        if (!(pa.fadeOverLifetime.isConstant() && pa.fadeOverLifetime.constant == 1.0f)){
            b.pixel[3] = 1.0f;
            bake32(pa.fadeOverLifetime, b, 1);
        }
        out.holdFrameRate = std::max(pa.holdFrameRate, 0.0f);
    }

    // conservative bounds: the shape's reach plus the farthest flight within the longest life
    float reach = 0.0f;
    switch (shape->kind){
        case SpawnShapeKind::Cone: reach = shape->radius + shape->length * (1.0f + shape->tanAngle); break;
        case SpawnShapeKind::Box: reach = std::sqrt(shape->boxHalf[0] * shape->boxHalf[0] + shape->boxHalf[1] * shape->boxHalf[1] + shape->boxHalf[2] * shape->boxHalf[2]); break;
        case SpawnShapeKind::Donut: reach = shape->radius + shape->donutRadius; break;
        case SpawnShapeKind::Point: reach = 0.0f; break;
        default: reach = shape->radius; break;
    }
    float scale = 0.0f;
    for (int c = 0; c < 3; c++)
        scale = std::max(scale, std::sqrt(shape->transform[c * 3] * shape->transform[c * 3] + shape->transform[c * 3 + 1] * shape->transform[c * 3 + 1] +
                                          shape->transform[c * 3 + 2] * shape->transform[c * 3 + 2]));
    reach = reach * scale + shape->randomizePosition;
    out.boundsSpeed = std::max(std::fabs(b.start[0]), std::fabs(b.start[1])) + Vector3(b.velocity[0], b.velocity[1], b.velocity[2]).length();
    out.boundsPad = reach + out.maxSize + std::max(std::fabs(b.wobble[0]), std::fabs(b.wobble[1]));
    out.longest = longest;
    for (int c = 0; c < 3; c++) out.boundsCentre[c] = shape->transform[9 + c];
    // until setMotion has the scene's gravity: the document's forces and Earth's times the modifier
    float g = std::fabs(d.main.gravityModifier.constant) * 9.81f;
    if (d.forceOverLifetime.present && d.forceOverLifetime.enabled)
        g += Vector3(d.forceOverLifetime.x.constant, d.forceOverLifetime.y.constant, d.forceOverLifetime.z.constant).length();
    updateBounds(out, g);
    out.valid = true;
    return true;
}

void AnalyticParticles::setMotion(AnalyticProgram& p, const float worldGravity[3], const float worldToSim[9]){
    auto toSim = [&](const float v[3], float out[3]){
        for (int r = 0; r < 3; r++) out[r] = worldToSim[r] * v[0] + worldToSim[3 + r] * v[1] + worldToSim[6 + r] * v[2];
    };
    float g[3], force[3], velocity[3];
    toSim(worldGravity, g);
    const float f[3] = {p.force.x, p.force.y, p.force.z};
    const float v[3] = {p.velocity.x, p.velocity.y, p.velocity.z};
    if (p.forceWorld) toSim(f, force); else std::memcpy(force, f, sizeof(force));
    if (p.velocityWorld) toSim(v, velocity); else std::memcpy(velocity, v, sizeof(velocity));
    for (int c = 0; c < 3; c++){
        p.block.motion[c] = g[c] * p.gravityModifier + force[c];
        p.block.velocity[c] = velocity[c];
    }
    // the bounds under the acceleration it now has (the project's gravity, not Earth's)
    updateBounds(p, Vector3(p.block.motion[0], p.block.motion[1], p.block.motion[2]).length());
}

bool AnalyticParticles::liveBounds(const AnalyticProgram& p, float outMin[3], float outMax[3]){
    bool any = false;
    for (int k = 0; k < 3; k++){ outMin[k] = 3.0e38f; outMax[k] = -3.0e38f; }
    for (uint32_t s = 0; s < p.slotCount; s++){
        const AnalyticParticle a = evaluate(p, s);
        if (!a.alive) continue;
        const float pad = 0.5f * std::max(a.size.x, a.size.y);
        const float c[3] = {a.position.x, a.position.y, a.position.z};
        for (int k = 0; k < 3; k++){
            outMin[k] = std::min(outMin[k], c[k] - pad);
            outMax[k] = std::max(outMax[k], c[k] + pad);
        }
        any = true;
    }
    return any;
}

void AnalyticParticles::updateBounds(AnalyticProgram& p, float gravity){
    // the farthest flight within the longest life, every push along one axis (conservative)
    const Vector3 far = flyPosition(Vector3(0.0f, 0.0f, 0.0f), Vector3(p.boundsSpeed, 0.0f, 0.0f), Vector3(gravity, 0.0f, 0.0f), p.block.motion[3], p.longest);
    const float pad = p.boundsPad + far.x;
    for (int c = 0; c < 3; c++){
        p.boundsMin[c] = p.boundsCentre[c] - pad;
        p.boundsMax[c] = p.boundsCentre[c] + pad;
    }
}

void AnalyticParticles::setTime(AnalyticProgram& p, double emitterTime, double emissionEnd){
    double t = emitterTime;
    // Hold Frames: the state of the last step boundary (R5)
    if (p.holdFrameRate > 0.0f)
        t = std::floor(t * (double)p.holdFrameRate + 1.0e-9) / (double)p.holdFrameRate;
    AnalyticBlock& b = p.block;
    if (p.looping && p.duration > 0.0f){
        // the loop's phase and count, so float32 never loses the seconds of a long session (R17)
        const double loops = std::floor(t / (double)p.duration);
        b.frame[0] = (float)(t - loops * (double)p.duration);
        b.frame[1] = (float)loops;
    }else{
        b.frame[0] = (float)t;
        b.frame[1] = 0.0f;
    }
    b.frame[2] = emissionEnd < 1.0e29 ? (float)emissionEnd : kNoEnd;
}

AnalyticParticle AnalyticParticles::evaluate(const AnalyticProgram& p, uint32_t slot){
    AnalyticParticle out;
    const AnalyticBlock& b = p.block;
    if (!p.valid || slot >= p.slotCount)
        return out;
    uint32_t index = 0;
    float age = 0.0f;
    if (!slotParticle(b, slot, index, age))
        return out;
    const uint32_t lifeKey = analyticKey(b, 0, 0);
    const float lifeMin = b.timing[2], lifeMax = b.timing[3];
    const float life = std::max(lifeMin == lifeMax ? lifeMin : lifeMin + (lifeMax - lifeMin) * hash01(lifeKey, index), 1.0e-4f);
    if (age >= life)
        return out;
    const float normAge = age / life;
    // Flicker: hidden on alternate half periods near the end of life (pixelfx EMBER)
    // (a loop's phase: periodic with it)
    if (b.pixel[0] > 0.0f && 1.0f - normAge < b.pixel[0]){
        const float cycle = b.frame[0] * b.pixel[1] + hash01(analyticKey(b, 1, 3), index);
        if (std::sin((cycle - std::floor(cycle)) * kTwoPi) < 0.0f)
            return out;
    }
    out.alive = true;
    out.index = index;
    out.age = age;
    out.life = life;

    // start values
    const float speedMin = b.start[0], speedMax = b.start[1];
    const float speed = speedMin == speedMax ? speedMin : speedMin + (speedMax - speedMin) * hash01(analyticKey(b, 0, 1), index);
    const float sizeMin = b.start[2], sizeMax = b.start[3];
    float size = sizeMin == sizeMax ? sizeMin : sizeMin + (sizeMax - sizeMin) * hash01(analyticKey(b, 0, 2), index);
    size = std::max(size, 0.0f);
    const float rotMin = b.start2[0], rotMax = b.start2[1];
    float rotation = rotMin == rotMax ? rotMin : rotMin + (rotMax - rotMin) * hash01(analyticKey(b, 0, 3), index);
    if (b.start2[2] > 0.0f && hash01(analyticKey(b, 1, 2), index) < b.start2[2])
        rotation = -rotation;
    rotation += b.start2[3] * age;
    out.rotation = rotation;

    // the flight (pixelfx fly / flyV)
    Vector3 p0, dir;
    shapeSample(b, analyticKey(b, 1, 1), index, p0, dir);
    const Vector3 v0 = dir * speed + Vector3(b.velocity[0], b.velocity[1], b.velocity[2]);
    const Vector3 g(b.motion[0], b.motion[1], b.motion[2]);
    flyTwin(p0, v0, g, b.motion[3], age, out.position, out.velocity);

    // size over lifetime
    if (b.ramp[3] > 0.5f)
        size *= curve32(b, 0, normAge);
    out.size = Vector2(size, size);

    // colour (sRGB rgb, as the record holds it)
    const int startKind = (int)b.velocity[3] & 3;
    const int lifetimeKind = (int)b.velocity[3] >> 2;
    const int entries = (int)b.ramp[0];
    auto rampIndex = [&](float t){
        if (b.ramp[1] != 1.0f && t > 0.0f) t = std::pow(t, b.ramp[1]);
        const int step = (int)std::min(std::max(std::floor(t * (float)entries), 0.0f), (float)(entries - 1));
        return b.ramp[2] > 0.5f ? entries - 1 - step : step;
    };
    float c[4] = {b.color0[0], b.color0[1], b.color0[2], b.color0[3]};
    if (startKind == 1){
        const float r = hash01(analyticKey(b, 1, 0), index);
        for (int k = 0; k < 3; k++)
            c[k] = encodeChannel(decodeChannel(b.color0[k]) + (decodeChannel(b.color1[k]) - decodeChannel(b.color0[k])) * r);
        c[3] = b.color0[3] + (b.color1[3] - b.color0[3]) * r;
    }else if (startKind == 2){
        const int step = rampIndex(hash01(analyticKey(b, 1, 0) ^ 0x68e31da4u, index));
        getColor(b, step, c);
        out.rampStep = (uint8_t)step;
    }
    if (lifetimeKind > 0){
        const float t = std::clamp(normAge * b.lifetimeRemap[0] + b.lifetimeRemap[1], 0.0f, 1.0f);
        float e[4];
        bool srgb = true;
        if (b.lifetimeRemap[2] > 0.5f){
            const int step = rampIndex(t);
            getColor(b, step, e);
            if (b.lifetimeRemap[3] > 1.5f) out.rampStep = (uint8_t)step;
        }else{
            const float x = t * (float)(entries - 1);
            const int i = std::min((int)x, entries - 2);
            const float f = x - (float)i;
            // interpolated in linear between the encoded samples
            float e0[4], e1[4];
            getColor(b, i, e0);
            getColor(b, i + 1, e1);
            for (int k = 0; k < 3; k++) e[k] = decodeChannel(e0[k]) + (decodeChannel(e1[k]) - decodeChannel(e0[k])) * f;
            e[3] = e0[3] + (e1[3] - e0[3]) * f;
            srgb = false;
        }
        const bool white = c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f;
        for (int k = 0; k < 3; k++){
            if (white && srgb)
                c[k] = e[k];
            else
                c[k] = encodeChannel(decodeChannel(c[k]) * (srgb ? decodeChannel(e[k]) : e[k]));
        }
        c[3] *= e[3];
    }
    if (b.pixel[3] > 0.5f)
        c[3] *= std::clamp(curve32(b, 1, normAge), 0.0f, 1.0f);
    out.color = packRgba(c);

    // the sheet's frame
    if (b.sheet[0] > 0.5f){
        const float frames = b.sheet[1];
        float f;
        if (b.sheet[0] > 1.5f){
            f = age * b.sheet[2];
        }else{
            const float cyc = normAge * b.sheet[2];
            const float t = cyc >= b.sheet[2] ? 1.0f : cyc - std::floor(cyc);
            f = t * frames;
        }
        f += b.sheet[3];
        f -= std::floor(f / frames) * frames;
        out.frame01 = (std::floor(f) + 0.5f) / frames;
    }

    // Wobble: the drawn sway, base pixels (R7)
    if (b.wobble[0] != 0.0f || b.wobble[1] != 0.0f){
        const float phx = b.pixel[2] > 0.5f ? hash01(analyticKey(b, 2, 0), index) * kTwoPi : 0.0f;
        const float phy = b.pixel[2] > 0.5f ? hash01(analyticKey(b, 2, 1), index) * kTwoPi : 0.0f;
        out.wobble = Vector2(std::sin(age * b.wobble[2] * kTwoPi + phx) * b.wobble[0], std::sin(age * b.wobble[3] * kTwoPi + phy) * b.wobble[1]);
    }
    return out;
}

AnalyticParticle AnalyticParticles::evaluateAt(const AnalyticProgram& program, uint32_t slot, double emitterTime){
    AnalyticProgram copy = program;
    setTime(copy, emitterTime, 1.0e30);
    return evaluate(copy, slot);
}

uint32_t AnalyticParticles::aliveCount(const AnalyticProgram& p){
    uint32_t n = 0;
    for (uint32_t s = 0; s < p.slotCount; s++)
        n += evaluate(p, s).alive ? 1u : 0u;
    return n;
}
