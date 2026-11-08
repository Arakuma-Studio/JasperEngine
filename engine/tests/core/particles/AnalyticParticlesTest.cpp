// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 AC7 (R15-R17) and AC9 (R18, §4): Analytic's diagnostics (unsupported modules by path, the
// slot capacity), exact loops (a slot at t equals itself at t + D, bit for bit), the live count
// against a brute-force enumeration of every birth, a seek an hour in equal to its loop phase,
// and the closed-form flight against a direct transcription of pixelfx's fly (particles.js:10-20).

#include "ParticleTestUtil.h"
#include "AllocationCounter.h"

#include "particles/AnalyticParticles.h"
#include "particles/ParticleKernels.h"
#include "particles/ParticleModules.h"

#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <random>

using namespace particletest;

namespace {

    ParticleEffectDocument analyticEffect(bool looping){
        ParticleEffectDocument d;
        d.simulationMode = SimulationMode::Analytic;
        d.looping = looping;
        d.prewarm = looping;
        d.duration = 4.0f;
        d.main.startLifetime = MinMaxCurve::twoConstants(0.8f, 1.4f);
        d.main.startSpeed = MinMaxCurve::twoConstants(4.0f, 10.0f);
        d.main.startSize = MinMaxCurve::twoConstants(6.0f, 10.0f);
        d.main.startRotation = MinMaxCurve::twoConstants(0.0f, 90.0f);
        d.emission.rateOverTime = MinMaxCurve(6.0f);
        Burst b;
        b.time = 0.5f;
        b.count = MinMaxCurve::twoConstants(3.0f, 5.0f);
        b.cycles = 3;
        b.interval = 0.25f;
        b.probability = 0.8f;
        d.emission.bursts = {b};
        d.shape.shape = ShapeType::Cone;
        d.shape.angle = 20.0f;
        d.shape.radius = 2.0f;
        d.limitVelocity.present = true;
        d.limitVelocity.dampen = 0.0f;
        d.limitVelocity.multiplyBySize = false;
        d.limitVelocity.drag = MinMaxCurve(1.4f);
        d.forceOverLifetime.present = true;
        d.forceOverLifetime.y = MinMaxCurve(16.0f);
        d.colorOverLifetime.present = true;
        d.colorOverLifetime.color.mode = GradientValueMode::Ramp;
        d.colorOverLifetime.color.rampColors = {{123, 111, 114, 255}, {93, 86, 96, 255}, {67, 61, 72, 255}, {43, 38, 48, 255}};
        d.pixelArt.present = true;
        d.pixelArt.wobbleAmplitude = Vector2(3.0f, 0.0f);
        d.pixelArt.wobbleFrequency = Vector2(0.25f, 0.25f);
        d.renderer.renderMode = RenderMode::Pixel;
        d.renderer.pixelShape = PixelShape::Puff;
        return d;
    }

    AnalyticProgram programOf(const ParticleEffectDocument& d, uint32_t seed){
        auto effect = compileOrFail(d);
        AnalyticProgram p;
        std::vector<Diagnostic> diagnostics;
        EXPECT_TRUE(AnalyticParticles::compile(d, effect->program, seed, p, diagnostics));
        return p;
    }

    bool sameBits(const AnalyticParticle& a, const AnalyticParticle& b){
        return a.alive == b.alive && a.index == b.index && std::memcmp(&a.age, &b.age, sizeof(float)) == 0 &&
               std::memcmp(&a.position, &b.position, sizeof(Vector3)) == 0 && std::memcmp(&a.velocity, &b.velocity, sizeof(Vector3)) == 0 &&
               std::memcmp(&a.rotation, &b.rotation, sizeof(float)) == 0 && std::memcmp(&a.size, &b.size, sizeof(Vector2)) == 0 &&
               a.color == b.color && a.rampStep == b.rampStep && std::memcmp(&a.wobble, &b.wobble, sizeof(Vector2)) == 0;
    }

    bool hasDiagnostic(const std::vector<Diagnostic>& list, const std::string& code, const std::string& path){
        for (const Diagnostic& d : list){
            if (d.code == code && d.path.rfind(path, 0) == 0 && d.severity == Diagnostic::Severity::Error) return true;
        }
        return false;
    }

}

TEST(AnalyticParticles, UnsupportedModulesAreDiagnosedByPath){
    ParticleEffectDocument d = analyticEffect(true);
    EXPECT_TRUE(ParticleCompiler::compile(d)->valid);
    d.noise.present = true;
    d.collision.present = true;
    d.renderer.sortMode = SortMode::ByDistance;
    auto effect = ParticleCompiler::compile(d);
    EXPECT_FALSE(effect->valid);
    EXPECT_TRUE(hasDiagnostic(effect->diagnostics, "analytic_unsupported", "modules.noise"));
    EXPECT_TRUE(hasDiagnostic(effect->diagnostics, "analytic_unsupported", "modules.collision"));
    EXPECT_TRUE(hasDiagnostic(effect->diagnostics, "analytic_unsupported", "modules.renderer.sortMode"));
    for (const Diagnostic& diagnostic : effect->diagnostics){
        if (diagnostic.code == "analytic_unsupported"){
            EXPECT_EQ(diagnostic.fix, "disable it or use Simulated");
        }
    }
    // a disabled module is not in the way
    d.noise.enabled = false;
    d.collision.enabled = false;
    d.renderer.sortMode = SortMode::None;
    EXPECT_TRUE(ParticleCompiler::compile(d)->valid);
}

TEST(AnalyticParticles, CapacityOverTheSlotLimitIsRefused){
    ParticleEffectDocument d = analyticEffect(false);
    d.emission.bursts.clear();
    d.main.startLifetime = MinMaxCurve(1.0f);
    // ceil(65536 x 1) + 1 = 65537 slots
    d.emission.rateOverTime = MinMaxCurve(65536.0f);
    auto effect = ParticleCompiler::compile(d);
    EXPECT_FALSE(effect->valid);
    bool found = false;
    for (const Diagnostic& diagnostic : effect->diagnostics)
        found = found || (diagnostic.code == "analytic_capacity" && diagnostic.message.find("65537") != std::string::npos);
    EXPECT_TRUE(found) << ParticleEffectCodec::format(effect->diagnostics);
    d.emission.rateOverTime = MinMaxCurve(65534.0f);
    EXPECT_TRUE(ParticleCompiler::compile(d)->valid);
}

TEST(AnalyticParticles, LoopsAreExactlyPeriodic){
    const ParticleEffectDocument d = analyticEffect(true);
    const AnalyticProgram p = programOf(d, 11);
    ASSERT_GT(p.slotCount, 0u);
    EXPECT_EQ(p.loopCount, 24u);   // rate 6 x 4 s
    std::mt19937 rng(17);
    std::uniform_real_distribution<double> when(10.0, 1000.0);
    int alive = 0;
    for (int i = 0; i < 100; i++){
        const double t = when(rng);
        for (uint32_t s = 0; s < p.slotCount; s++){
            const AnalyticParticle a = AnalyticParticles::evaluateAt(p, s, t);
            const AnalyticParticle b = AnalyticParticles::evaluateAt(p, s, t + d.duration);
            ASSERT_TRUE(sameBits(a, b)) << "slot " << s << " at " << t;
            alive += a.alive ? 1 : 0;
        }
    }
    EXPECT_GT(alive, 100);
    // an hour in: the frame of its loop phase
    const double hour = 3600.37;
    for (uint32_t s = 0; s < p.slotCount; s++)
        ASSERT_TRUE(sameBits(AnalyticParticles::evaluateAt(p, s, hour), AnalyticParticles::evaluateAt(p, s, std::fmod(hour, (double)d.duration))));
}

TEST(AnalyticParticles, AliveCountMatchesEveryBirth){
    ParticleEffectDocument d = analyticEffect(false);
    const uint32_t seed = 5;
    AnalyticProgram p = programOf(d, seed);
    auto effect = compileOrFail(d);
    const uint32_t lifeKey = analyticKey(p.block, 0, 0);
    auto lifeOf = [&](uint32_t index){
        return std::max(0.8f + (1.4f - 0.8f) * random01(lifeKey, index), 1.0e-4f);
    };
    for (double t : {0.1, 0.55, 0.8, 1.3, 2.7, 3.99, 4.5, 5.2, 6.0}){
        AnalyticParticles::setTime(p, t, 1.0e30);
        // brute force: every rate birth n / rate before the duration, and every burst firing
        uint32_t expected = 0;
        for (int n = 0; (double)n / 6.0 < d.duration; n++){
            const double age = t - (double)n / 6.0;
            if (age >= 0.0 && (float)age < lifeOf((uint32_t)n)) expected++;
        }
        const uint32_t probKey = analyticKey(p.block, 2, 2), countKey = analyticKey(p.block, 2, 3);
        for (int c = 0; c < 3; c++){
            const double fired = 0.5 + 0.25 * c;
            if (t < fired) continue;
            const uint32_t firing = (uint32_t)c;
            if (random01(probKey, firing) >= 0.8f) continue;
            // uniform over 3..5, as Simulated draws a burst's count
            const int count = std::min((int)std::floor(3.0f + 3.0f * random01(countKey, firing)), 5);
            for (int j = 0; j < count; j++){
                const uint32_t index = 0x40000000u + (uint32_t)(c * 5 + j);
                if ((float)(t - fired) < lifeOf(index)) expected++;
            }
        }
        EXPECT_EQ(AnalyticParticles::aliveCount(p), expected) << "at " << t;
    }
}

// A looping effect repeats its bursts each loop: a burst that fires for ever fires only inside one
// loop (no firing at or past the duration), each loop's the same; brute force over the loops
TEST(AnalyticParticles, LoopingBurstsMatchEveryFiring){
    ParticleEffectDocument d = analyticEffect(true);
    d.duration = 2.0f;
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    d.emission.bursts[0].time = 0.25f;
    d.emission.bursts[0].interval = 0.5f;
    d.emission.bursts[0].cycles = 0;   // for ever
    AnalyticProgram p = programOf(d, 9);
    const uint32_t lifeKey = analyticKey(p.block, 0, 0), probKey = analyticKey(p.block, 2, 2), countKey = analyticKey(p.block, 2, 3);
    auto lifeOf = [&](uint32_t index){
        return std::max(0.8f + (1.4f - 0.8f) * random01(lifeKey, index), 1.0e-4f);
    };
    for (double t : {0.1, 0.3, 1.0, 1.9, 2.05, 2.3, 3.74, 3.99, 10.6, 101.27}){
        AnalyticParticles::setTime(p, t, 1.0e30);
        uint32_t expected = 0;
        const int loop = (int)std::floor(t / d.duration);
        // prewarmed: the loops before the first one count too
        for (int l = loop - 2; l <= loop; l++){
            for (int c = 0; c < 4; c++){   // 0.25, 0.75, 1.25, 1.75 in each 2 s loop
                const double fired = l * (double)d.duration + 0.25 + 0.5 * c;
                const double age = t - fired;
                if (age < 0.0) continue;
                const uint32_t firing = (uint32_t)c;
                if (random01(probKey, firing) >= 0.8f) continue;
                const int count = std::min((int)std::floor(3.0f + 3.0f * random01(countKey, firing)), 5);
                for (int j = 0; j < count; j++){
                    const uint32_t index = 0x40000000u + (uint32_t)(c * 5 + j);
                    if ((float)age < lifeOf(index)) expected++;
                }
            }
        }
        EXPECT_EQ(AnalyticParticles::aliveCount(p), expected) << "at " << t;
    }
}

TEST(AnalyticParticles, MatchesPixelfxFly){
    // pixelfx's fly(), transcribed in double (y here is up: g's sign is the caller's). Below k t = 1e-3
    // its form cancels (g / k is huge and 1 - e^-kt keeps few digits), so there the reference is the
    // same function's exact Taylor series, summed until its terms vanish
    auto fly = [](double p, double v, double g, double drag, double t){
        if (drag <= 0.0) return p + v * t + 0.5 * g * t * t;
        const double kt = drag * t;
        if (kt >= 1.0e-3){
            const double f = (1.0 - std::exp(-drag * t)) / drag, vt = g / drag;
            return p + vt * t + (v - vt) * f;
        }
        // f = sum (-kt)^n t / (n+1)!, h = (t - f) / k = sum (-kt)^n t^2 / (n+2)!
        double f = 0.0, h = 0.0, term = 1.0;
        for (int n = 0; n < 12; n++){
            f += term * t / (double)(n + 1);
            h += term * t * t / ((double)(n + 1) * (double)(n + 2));
            term *= -kt / (double)(n + 1);
        }
        return p + v * f + g * h;
    };
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> velocity(-300.0f, 300.0f), gravity(-1000.0f, 1000.0f), time(0.0f, 1.0f), drag(0.0f, 5.0f);
    for (int i = 0; i < 200; i++){
        const Vector3 v0(velocity(rng), velocity(rng), velocity(rng));
        const Vector3 g(gravity(rng), gravity(rng), gravity(rng));
        // every tenth a drag the series takes (k t below 1e-3), and two with none
        float k = drag(rng);
        if (i % 10 == 0) k = 1.0e-5f * (float)(1 + i % 7);
        if (i == 3 || i == 4) k = 0.0f;
        const float t = time(rng);
        const Vector3 p0(1.5f, -2.0f, 0.25f);
        const Vector3 p = AnalyticParticles::flyPosition(p0, v0, g, k, t);
        EXPECT_NEAR(p.x, fly(p0.x, v0.x, g.x, k, t), 1.0e-4) << "k " << k << " t " << t;
        EXPECT_NEAR(p.y, fly(p0.y, v0.y, g.y, k, t), 1.0e-4) << "k " << k << " t " << t;
        EXPECT_NEAR(p.z, fly(p0.z, v0.z, g.z, k, t), 1.0e-4) << "k " << k << " t " << t;
    }
}

// R19: the twin computes in float32 what the vertex stage computes (the same formulas in the same
// order) and stays within 1e-3 base px of the exact flight over a pixel-art effect's range, the
// small drags included (k t just above the old 1e-3 cut, where the closed form cancelled: k = 0.0006
// at t = 2 s was 65 px off)
TEST(AnalyticParticles, Float32TwinWithinAMilliPixel){
    auto exact = [](double p, double v, double g, double k, double t){
        if (k <= 0.0) return p + v * t + 0.5 * g * t * t;
        // the series to many terms (kt < 1 here or closed form well conditioned above)
        double f = 0.0, h = 0.0, term = 1.0;
        if (k * t < 1.0){
            for (int n = 0; n < 30; n++){
                f += term * t / (double)(n + 1);
                h += term * t * t / ((double)(n + 1) * (double)(n + 2));
                term *= -k * t / (double)(n + 1);
            }
        }else{
            f = -std::expm1(-k * t) / k;
            h = (t - f) / k;
        }
        return p + v * f + g * h;
    };
    std::mt19937 rng(23);
    std::uniform_real_distribution<float> velocity(-300.0f, 300.0f), gravity(-500.0f, 500.0f), time(0.0f, 2.0f), drag(0.0f, 4.0f), small(0.0f, 0.01f);
    double worst = 0.0;
    for (int i = 0; i < 2000; i++){
        const Vector3 v0(velocity(rng), velocity(rng), 0.0f);
        const Vector3 g(gravity(rng), gravity(rng), 0.0f);
        const float k = i % 2 == 0 ? small(rng) : drag(rng);
        const float t = time(rng);
        const Vector3 p0(3.25f, -1.5f, 0.0f);
        Vector3 p, v;
        AnalyticParticles::flyTwin(p0, v0, g, k, t, p, v);
        worst = std::max(worst, std::fabs(p.x - exact(p0.x, v0.x, g.x, k, t)));
        worst = std::max(worst, std::fabs(p.y - exact(p0.y, v0.y, g.y, k, t)));
    }
    EXPECT_LT(worst, 1.0e-3) << "the float32 twin's flight is " << worst << " px from the exact one";
    // the reported case
    Vector3 p, v;
    AnalyticParticles::flyTwin(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, -500.0f, 0.0f), 0.0006f, 2.0f, p, v);
    EXPECT_NEAR(p.y, exact(0.0, 0.0, -500.0, 0.0006, 2.0), 1.0e-3);
}

// The bounds the renderer culls by hold every particle the twin places under the scene's own
// gravity (a pixel-art project's is hundreds of px/s², not 9.81): setMotion works them out again
TEST(AnalyticParticles, BoundsFollowTheScenesGravity){
    ParticleEffectDocument d = analyticEffect(true);
    d.main.gravityModifier = MinMaxCurve(1.0f);
    AnalyticProgram p = programOf(d, 4);
    const float gravity[3] = {0.0f, -950.0f, 0.0f};
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    AnalyticParticles::setMotion(p, gravity, identity);
    size_t inside = 0, outside = 0;
    for (double t : {0.2, 0.9, 1.7, 2.6, 3.3}){
        AnalyticParticles::setTime(p, t, 1.0e30);
        for (uint32_t s = 0; s < p.slotCount; s++){
            const AnalyticParticle a = AnalyticParticles::evaluate(p, s);
            if (!a.alive) continue;
            bool in = true;
            const float c[3] = {a.position.x, a.position.y, a.position.z};
            for (int k = 0; k < 3; k++) in = in && c[k] >= p.boundsMin[k] && c[k] <= p.boundsMax[k];
            (in ? inside : outside)++;
        }
    }
    EXPECT_GT(inside, 50u);
    EXPECT_EQ(outside, 0u) << "particles outside the bounds under 950 px/s²";
}

// R28, measured: an Analytic emitter's CPU per frame (its motion and time for the uniform block)
// and a seek at any time, whatever the particle count (65,536 slots here); its uniform block <= 1 KB.
// Budgets enforced with the benchmark server's perfSlack(), printed always
TEST(AnalyticParticles, CostIsPerEmitterNotPerParticle){
    EXPECT_LE(sizeof(AnalyticBlock), 1024u);
    ParticleEffectDocument d = analyticEffect(true);
    d.emission.bursts.clear();
    d.main.startLifetime = MinMaxCurve(4.0f);
    d.emission.rateOverTime = MinMaxCurve(16384.0f);   // 4 s x 16384: 65,536 slots
    auto effect = compileOrFail(d);
    EmitterRuntime runtime;
    runtime.bind(effect);
    runtime.reset(3);
    runtime.play();
    ASSERT_TRUE(runtime.isAnalytic());
    EXPECT_EQ(runtime.analytic()->slotCount, 65536u);
    EmitterInput input;
    const int frames = 2000;
    auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; f++) runtime.advance(input, 1.0 / 60.0);
    const double frameMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / frames;
    start = std::chrono::steady_clock::now();
    const int seeks = 200;
    for (int s = 0; s < seeks; s++) runtime.seek(input, 37.0 + 911.0 * s);
    const double seekMillis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / seeks;
    std::printf("[bench] Analytic, 65536 slots: %.2f us per frame (budget 5 us desktop), seek %.4f ms (budget 0.1 ms), block %zu B\n",
                frameMicros, seekMillis, sizeof(AnalyticBlock));
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LT(frameMicros, 5.0 * testsupport::perfSlack());
        EXPECT_LT(seekMillis, 0.1 * testsupport::perfSlack());
    }
}

// R19: Scene-view picking takes the twin's live particles (liveBounds), not the culling bounds that
// hold every particle the effect may ever draw: it holds each live particle, inside the culling box
TEST(AnalyticParticles, PickingBoundsAreTheLiveParticles){
    AnalyticProgram p = programOf(analyticEffect(true), 6);
    AnalyticParticles::setTime(p, 2.3, 1.0e30);
    float lo[3], hi[3];
    ASSERT_TRUE(AnalyticParticles::liveBounds(p, lo, hi));
    for (uint32_t s = 0; s < p.slotCount; s++){
        const AnalyticParticle a = AnalyticParticles::evaluate(p, s);
        if (!a.alive) continue;
        EXPECT_GE(a.position.x, lo[0]); EXPECT_LE(a.position.x, hi[0]);
        EXPECT_GE(a.position.y, lo[1]); EXPECT_LE(a.position.y, hi[1]);
    }
    for (int k = 0; k < 3; k++){
        EXPECT_GE(lo[k], p.boundsMin[k]);
        EXPECT_LE(hi[k], p.boundsMax[k]);
    }
    EXPECT_LT(hi[0] - lo[0], p.boundsMax[0] - p.boundsMin[0]) << "tighter than the culling box";
}
