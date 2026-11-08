// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 AC7 (R15-R17) and AC9 (R18, §4): Analytic's diagnostics (unsupported modules by path, the
// slot capacity), exact loops (a slot at t equals itself at t + D, bit for bit), the live count
// against a brute-force enumeration of every birth, a seek an hour in equal to its loop phase,
// and the closed-form flight against a direct transcription of pixelfx's fly (particles.js:10-20).

#include "ParticleTestUtil.h"

#include "particles/AnalyticParticles.h"
#include "particles/ParticleKernels.h"
#include "particles/ParticleModules.h"

#include <cmath>
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
    const uint32_t lifeKey = (uint32_t)p.block.keys0[0];
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
        const uint32_t probKey = (uint32_t)p.block.keys2[2], countKey = (uint32_t)p.block.keys2[3];
        for (int c = 0; c < 3; c++){
            const double fired = 0.5 + 0.25 * c;
            if (t < fired) continue;
            const uint32_t firing = (uint32_t)c;
            if (random01(probKey, firing) >= 0.8f) continue;
            const int count = (int)std::floor(3.0f + 2.0f * random01(countKey, firing) + 0.5f);
            for (int j = 0; j < count; j++){
                const uint32_t index = 0x40000000u + (uint32_t)(c * 5 + j);
                if ((float)(t - fired) < lifeOf(index)) expected++;
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
