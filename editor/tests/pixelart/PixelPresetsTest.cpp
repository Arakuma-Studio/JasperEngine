// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 AC10 (R14, R20, §6.6): the pixel presets validate and keep pixelfx's motion: pixel_spark's
// flight at 120 Hz against a transcription of sim.js:97-106 (within a pixel over half a second),
// its first floor bounce leaving at 0.45 of its speed in; pixel_grit and pixel_debris's shards come
// to rest (Settle Speed) and stay until they die; pixel_fire leaves smoke at 35 % of its deaths.
// The presets need no project here: their ramps are the built-in palette's colours inline.

#include "particles/EmitterRuntime.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticleEffectCodec.h"
#include "pixelart/particles/PixelParticlePresets.h"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::particles;
using namespace jasperengine::editor::pixelart;

namespace {

    ParticleEffectDocument preset(const std::string& name){
        ParticleEffectDocument d;
        std::string error;
        EXPECT_TRUE(buildPixelParticlePreset(name, PixelPresetBuild(), d, error)) << name << ": " << error;
        return d;
    }

    std::shared_ptr<const CompiledEffect> compiled(const ParticleEffectDocument& d){
        auto effect = ParticleCompiler::compile(d);
        EXPECT_TRUE(effect->valid) << ParticleEffectCodec::format(effect->diagnostics);
        return effect;
    }

    struct PresetRun {
        EmitterRuntime runtime;
        EmitterInput input;
        float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        explicit PresetRun(std::shared_ptr<const CompiledEffect> effect, uint32_t seed = 1){
            runtime.bind(effect);
            runtime.reset(seed);
            input.seed = seed;
            input.transform = transform;
            input.gravity[1] = 0.0f;   // the presets carry their own forces
            runtime.play();
        }
        float ch(Channel c, uint32_t i) const { return runtime.particleChannels().get(c)[i]; }
        uint32_t id(uint32_t i) const { return runtime.particleChannels().getU(ChSpawnIndex)[i]; }
    };

    // the spark alone: one particle at a known velocity (px/s, y up), its floor on or off
    ParticleEffectDocument oneSpark(float vx, float vy, bool floor){
        ParticleEffectDocument d = preset("pixel_spark");
        d.emission.bursts[0].count = MinMaxCurve(1.0f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.main.startLifetime = MinMaxCurve(5.0f);
        d.shape.enabled = false;
        d.startVelocity.present = true;
        d.startVelocity.min = d.startVelocity.max = Vector3(vx, vy, 0.0f);
        d.collision.enabled = floor;
        d.interpolate = false;
        return d;
    }

}

TEST(PixelPresets, EveryPresetValidates){
    for (const std::string& name : pixelParticlePresetNames()){
        const ParticleEffectDocument d = preset(name);
        EXPECT_TRUE(ParticleCompiler::compile(d)->valid) << name;
        EXPECT_EQ(d.designPixelsPerUnit, 1.0f) << name;
        EXPECT_FALSE(pixelParticlePresetLabel(name).empty());
        for (const std::string& child : pixelParticlePresetChildren(name)) EXPECT_TRUE(ParticleCompiler::compile(preset(child))->valid) << child;
    }
    EXPECT_EQ(pixelParticlePresetNames().size(), 9u);
    EXPECT_EQ(pixelParticlePresetChildren("pixel_fire"), std::vector<std::string>{"pixel_smoke"});
    // the two loops are Analytic
    EXPECT_EQ(preset("pixel_smoke_loop").simulationMode, SimulationMode::Analytic);
    EXPECT_EQ(preset("pixel_ember_loop").simulationMode, SimulationMode::Analytic);
    // made for PPU 16: lengths and speeds over 16, sizes still in pixels
    PixelPresetBuild sixteen;
    sixteen.pixelsPerUnit = 16;
    ParticleEffectDocument scaled;
    std::string error;
    ASSERT_TRUE(buildPixelParticlePreset("pixel_spark", sixteen, scaled, error)) << error;
    EXPECT_FLOAT_EQ(scaled.main.startSpeed.constantMax, 260.0f / 16.0f);
    EXPECT_FLOAT_EQ(scaled.forceOverLifetime.y.constant, -427.5f / 16.0f);
    EXPECT_FLOAT_EQ(scaled.collision.planes[0].point.y, -1.0f);
    EXPECT_FLOAT_EQ(scaled.main.startSize.constant, 1.0f);
    EXPECT_EQ(scaled.designPixelsPerUnit, 16.0f);
}

TEST(PixelPresets, SparkFliesAsPixelfx){
    const float vx0 = 140.0f, vy0 = 210.0f;
    PresetRun run(compiled(oneSpark(vx0, vy0, false)));
    // sim.js SPARK (y down there: here g is -950 x 0.45 on y)
    const double dt = 1.0 / 120.0, g = -950.0 * 0.45;
    run.runtime.step(run.input);   // the burst's tick: born
    ASSERT_EQ(run.runtime.count(), 1u);
    // from where and how fast the birth tick left it
    double x = run.ch(ChPosX, 0), y = run.ch(ChPosY, 0), vx = run.ch(ChVelX, 0), vy = run.ch(ChVelY, 0);
    EXPECT_NEAR(vx, vx0, 10.0);
    EXPECT_NEAR(vy, vy0, 10.0);
    // S20 moves a particle by the tick's mean velocity, pixelfx by its end velocity: the two paths
    // stay (v0 - v) dt / 2 apart by construction (spec Deviations), so that offset is taken out
    // before the 1 px budget
    const double startVx = vx, startVy = vy;
    double worst = 0.0;
    for (int tick = 0; tick < 60; tick++){
        run.runtime.step(run.input);
        vy += g * dt;
        vx -= vx * 2.2 * dt;
        vy -= vy * 2.2 * dt;
        x += vx * dt;
        y += vy * dt;
        const double ox = (startVx - vx) * dt * 0.5, oy = (startVy - vy) * dt * 0.5;
        worst = std::max(worst, std::hypot(run.ch(ChPosX, 0) - (x + ox), run.ch(ChPosY, 0) - (y + oy)));
    }
    EXPECT_LT(worst, 1.0) << "the spark left pixelfx's path by " << worst << " px";
}

TEST(PixelPresets, SparkBouncesAtPixelfxsShare){
    PresetRun run(compiled(oneSpark(60.0f, -50.0f, true)));
    run.runtime.step(run.input);
    float previousVy = -50.0f;
    bool bounced = false;
    for (int tick = 0; tick < 240 && !bounced; tick++){
        run.runtime.step(run.input);
        const float vy = run.ch(ChVelY, 0);
        if (vy > 0.0f && previousVy < 0.0f){
            // what the tick's forces made of the incoming speed before the floor reflected it
            const double dt = 1.0 / 120.0;
            const double incoming = (previousVy + (-950.0 * 0.45) * dt) * std::exp(-2.2 * dt);
            const double share = vy / -incoming;
            EXPECT_NEAR(share, 0.45, 0.45 * 0.02) << "the bounce's vertical share";
            bounced = true;
        }
        previousVy = vy;
    }
    EXPECT_TRUE(bounced);
}

// pixelfx's floor response of each preset that lands (sim.js GRIT 79; Bits 315 with the bounce
// of its debris): the first hit leaves at pixelfx's vertical share of the speed in (S20 keeps
// (1 - dampen) of the reflected velocity, so the presets' bounce is pixelfx's over it), and the
// pieces come to rest once a landing is slow
TEST(PixelPresets, GritAndDebrisBounceAsPixelfx){
    struct Case { const char* name; double share; float vy0; };
    for (const Case& c : {Case{"pixel_grit", 0.25, -300.0f}, Case{"pixel_debris_shard", 0.35, 0.0f}, Case{"pixel_debris_chunk", 0.30, 0.0f}}){
        ParticleEffectDocument d = preset(c.name);
        d.emission.bursts = {Burst()};
        d.emission.bursts[0].count = MinMaxCurve(1.0f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.main.startLifetime = MinMaxCurve(10.0f);
        d.shape.enabled = false;
        d.startVelocity.present = true;
        d.startVelocity.min = d.startVelocity.max = Vector3(0.0f, c.vy0, 0.0f);
        d.collision.lifetimeLoss = MinMaxCurve(0.0f);
        d.interpolate = false;
        const double dt = 1.0 / (double)d.tickRate;
        const double g = d.forceOverLifetime.y.constant;
        const double k = d.limitVelocity.present ? d.limitVelocity.drag.constant : 0.0;
        PresetRun run(compiled(d));
        run.runtime.step(run.input);
        ASSERT_EQ(run.runtime.count(), 1u) << c.name;
        float previousVy = run.ch(ChVelY, 0);
        bool bounced = false, settled = false;
        for (int tick = 0; tick < 6 * d.tickRate && !settled; tick++){
            run.runtime.step(run.input);
            const float vy = run.ch(ChVelY, 0);
            if (!bounced && vy > 0.0f && previousVy < 0.0f){
                const double incoming = (previousVy + g * dt) * std::exp(-k * dt);
                EXPECT_NEAR(vy / -incoming, c.share, c.share * 0.02) << c.name << ": the first bounce's vertical share";
                bounced = true;
            }
            settled = run.ch(ChSettled, 0) > 0.0f;
            previousVy = vy;
        }
        EXPECT_TRUE(bounced) << c.name << " bounces";
        EXPECT_TRUE(settled) << c.name << " comes to rest";
    }
}

TEST(PixelPresets, GritAndDebrisSettleUntilTheyDie){
    for (const std::string& name : {std::string("pixel_grit"), std::string("pixel_debris_shard")}){
        ParticleEffectDocument d = preset(name);
        d.interpolate = false;
        if (d.emission.bursts.empty()){
            Burst b;
            b.count = MinMaxCurve(20.0f);
            d.emission.bursts = {b};
        }
        PresetRun run(compiled(d), 4);
        std::map<uint32_t, std::pair<float, float>> rested;   // spawn id -> where it came to rest
        size_t moved = 0;
        for (int tick = 0; tick < 30 * 3; tick++){
            run.runtime.step(run.input);
            for (uint32_t i = 0; i < run.runtime.count(); i++){
                if (run.ch(ChSettled, i) <= 0.0f) continue;
                EXPECT_EQ(run.ch(ChVelX, i), 0.0f);
                EXPECT_EQ(run.ch(ChVelY, i), 0.0f);
                auto found = rested.find(run.id(i));
                if (found == rested.end()){
                    rested[run.id(i)] = {run.ch(ChPosX, i), run.ch(ChPosY, i)};
                }else if (found->second.first != run.ch(ChPosX, i) || found->second.second != run.ch(ChPosY, i)){
                    moved++;
                }
            }
        }
        EXPECT_GE(rested.size(), 8u) << name << ": particles at rest";
        EXPECT_EQ(moved, 0u) << name << ": a particle at rest moved";
    }
}

TEST(PixelPresets, FireLeavesSmokeAtAThirdOfItsDeaths){
    ParticleEffectDocument fire = preset("pixel_fire");
    fire.emission.rateOverTime = MinMaxCurve(300.0f);   // more deaths a second, the same share
    auto smoke = compiled(preset("pixel_smoke"));
    PresetRun run(compiled(fire), 9);
    run.runtime.bindSubEmitters([&](const SubEmitterEntry& entry) -> std::shared_ptr<const CompiledEffect> {
        return entry.effect == "preset:pixel_smoke" ? smoke : nullptr;
    });
    run.runtime.reset(9);
    run.runtime.play();
    size_t deaths = 0, smokes = 0;
    while (deaths < 10000){
        run.runtime.step(run.input);
        deaths += run.runtime.deathEvents().size();
        smokes += run.runtime.subEmitEvents().size();
        ASSERT_LT(run.runtime.deathEvents().size(), (size_t)kMaxDeathEvents) << "the frame's death events filled up";
    }
    const double share = (double)smokes / (double)deaths;
    EXPECT_NEAR(share, 0.35, 0.03) << smokes << " smokes of " << deaths << " deaths";
}
