// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC13: the twelve old systems converted to effects reproduce the old ActionSystem's
// trajectories (fixtures/particles/legacy/<name>.csv, recorded before it was removed) within
// 1e-3 world units and 1/255 of colour (sRGB) at a 60 Hz tick.

#include "particles/LegacyParticleConverter.h"
#include "particles/LegacyParticleSystem.h"

#include "LegacyParticleFixtures.h"
#include "ParticleTestUtil.h"
#include "TestPaths.h"

#include "math/Matrix4.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace particletest;

namespace {

    struct Row {
        int frame;
        uint32_t slot;
        float values[20];   // age, px, py, pz, r, g, b, a, sx, sy, sz, qx, qy, qz, qw, rot, rx, ry, rw, rh
    };

    std::vector<Row> readCsv(const std::string& path){
        std::vector<Row> rows;
        std::ifstream in(path);
        std::string line;
        std::getline(in, line);   // the header
        while (std::getline(in, line)){
            Row r{};
            std::stringstream ss(line);
            std::string cell;
            std::getline(ss, cell, ',');
            r.frame = std::stoi(cell);
            std::getline(ss, cell, ',');
            r.slot = (uint32_t)std::stoul(cell);
            for (float& v : r.values){
                std::getline(ss, cell, ',');
                v = std::stof(cell);
            }
            rows.push_back(r);
        }
        return rows;
    }

    float srgb(float linear){ return std::pow(std::max(linear, 0.0f), 1.0f / 2.2f) * 255.0f; }

}

TEST(ParticleMigration, ConvertedSystemsFollowTheOldTrajectories){
    for (size_t f = 0; f < legacyFixtures().size(); f++){
        const LegacyFixture& fx = legacyFixtures()[f];
        SCOPED_TRACE(fx.name);
        LegacyParticleSystem legacy;
        configureLegacy(f, legacy);
        LegacyTarget target;
        target.kind = fx.mesh ? LegacyTarget::Kind::Mesh : LegacyTarget::Kind::Points;
        for (const Rect& r : fx.frames){
            SpriteRect s;
            s.x = r.getX(); s.y = r.getY(); s.width = r.getWidth(); s.height = r.getHeight();
            target.frames.push_back(s);
        }
        std::vector<std::string> notes;
        const ParticleEffectDocument doc = LegacyParticleConverter::convert(legacy, target, &notes);
        EXPECT_TRUE(notes.empty()) << notes.front();
        Sim sim(doc);
        // the target's transform
        const Matrix4 m = Matrix4::translateMatrix(fx.position) * fx.rotation.getRotationMatrix() * Matrix4::scaleMatrix(fx.scale);
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) sim.transform[c * 4 + r] = m[c][r];

        const std::vector<Row> rows = readCsv(jasperengine::test::testPaths().assetDir + "/particles/legacy/" + fx.name + ".csv");
        ASSERT_FALSE(rows.empty());
        size_t row = 0;
        float worstPosition = 0.0f, worstColor = 0.0f, worstSize = 0.0f, worstRotation = 0.0f;
        for (int frame = 0; frame < kLegacyFrames; frame++){
            sim.ticks(1);
            const ParticleChannels& ch = sim.runtime.particleChannels();
            // the frame's rows against the particles, matched by spawn order
            std::map<uint32_t, uint32_t> bySpawn;
            for (uint32_t i = 0; i < ch.count; i++) bySpawn[ch.getU(ChSpawnIndex)[i]] = i;
            size_t rowsThisFrame = 0;
            for (; row < rows.size() && rows[row].frame == frame; row++){
                const Row& r = rows[row];
                rowsThisFrame++;
                ASSERT_TRUE(bySpawn.count(r.slot)) << "frame " << frame << ": no particle " << r.slot;
                const uint32_t i = bySpawn[r.slot];
                const float pos[3] = {ch.get(ChPosX)[i], ch.get(ChPosY)[i], ch.get(ChPosZ)[i]};
                for (int c = 0; c < 3; c++) worstPosition = std::max(worstPosition, std::fabs(pos[c] - r.values[1 + c]));
                float color[4], size, rotation;
                sim.runtime.appearance(i, color, size, rotation);
                for (int c = 0; c < 3; c++) worstColor = std::max(worstColor, std::fabs(srgb(color[c]) - srgb(r.values[4 + c])));
                worstColor = std::max(worstColor, std::fabs(color[3] - r.values[7]) * 255.0f);
                if (fx.mesh){
                    const float s[3] = {ch.get(ChSizeX)[i], ch.get(ChSizeY)[i], ch.get(ChSizeZ)[i]};
                    for (int c = 0; c < 3; c++) worstSize = std::max(worstSize, std::fabs(s[c] - r.values[8 + c]));
                    // q and -q are one rotation
                    const float dot = ch.get(ChQuatX)[i] * r.values[11] + ch.get(ChQuatY)[i] * r.values[12] +
                                      ch.get(ChQuatZ)[i] * r.values[13] + ch.get(ChQuatW)[i] * r.values[14];
                    worstRotation = std::max(worstRotation, 1.0f - std::fabs(dot));
                }else{
                    worstSize = std::max(worstSize, std::fabs(size - r.values[8]));
                    const float d = std::remainder(rotation - r.values[15], 6.28318530718f);
                    worstRotation = std::max(worstRotation, std::fabs(d));
                    // the sprite frame: its rect among the target's
                    if (!target.frames.empty()){
                        const uint32_t frameIndex = (uint32_t)ch.get(ChFrame)[i];
                        ASSERT_LT(frameIndex, target.frames.size());
                        EXPECT_NEAR(target.frames[frameIndex].x, r.values[16], 1e-6f) << "frame " << frame;
                        EXPECT_NEAR(target.frames[frameIndex].y, r.values[17], 1e-6f) << "frame " << frame;
                    }
                }
            }
            EXPECT_EQ(rowsThisFrame, (size_t)ch.count) << "frame " << frame;
        }
        EXPECT_LE(worstPosition, 1e-3f);
        EXPECT_LE(worstColor, 1.0f);
        EXPECT_LE(worstSize, 1e-3f);
        EXPECT_LE(worstRotation, 1e-3f);
    }
}

// what the deprecated setters do: the old view of an effect, written back, keeps the effect
// (the same structure, so live particles stay; the eases come back as curves)
TEST(ParticleMigration, ExtractAndApplyRoundTrip){
    for (size_t f = 0; f < legacyFixtures().size(); f++){
        SCOPED_TRACE(legacyFixtures()[f].name);
        LegacyParticleSystem legacy;
        configureLegacy(f, legacy);
        const ParticleEffectDocument doc = LegacyParticleConverter::convert(legacy, LegacyTarget());
        ParticleEffectDocument again = doc;
        LegacyParticleConverter::apply(LegacyParticleConverter::extract(doc), again);
        const auto a = compileOrFail(doc), b = compileOrFail(again);
        EXPECT_TRUE(a->program.sameStructure(b->program));
        ASSERT_EQ(again.setOverAge.entries.size(), doc.setOverAge.entries.size());
        for (size_t e = 0; e < doc.setOverAge.entries.size(); e++){
            const SetOverAgeEntry& x = doc.setOverAge.entries[e];
            const SetOverAgeEntry& y = again.setOverAge.entries[e];
            EXPECT_EQ(x.property, y.property);
            EXPECT_EQ(x.fromTime, y.fromTime);
            EXPECT_EQ(x.toTime, y.toTime);
            EXPECT_EQ(x.from, y.from);
            EXPECT_EQ(x.to, y.to);
            for (int i = 0; i <= 50; i++)
                EXPECT_NEAR(x.curve.evaluate(i / 50.0f), y.curve.evaluate(i / 50.0f), 1e-3f);
        }
        // every other field as it was
        ParticleEffectDocument withoutEntries = again;
        withoutEntries.setOverAge.entries = doc.setOverAge.entries;
        EXPECT_EQ(ParticleEffectCodec::write(withoutEntries), ParticleEffectCodec::write(doc));
    }
}

TEST(ParticleMigration, CycleDurationIsTheOldOne){
    LegacyParticleSystem p;
    p.maxParticles = 30;
    p.rate = 20;
    EXPECT_FLOAT_EQ(LegacyParticleConverter::cycleDuration(p), 1.5f);
    p.bursts = {{2.0f, 1, 1}};
    EXPECT_FLOAT_EQ(LegacyParticleConverter::cycleDuration(p), 2.0f);
    p.rate = 0;
    p.bursts = {{0.0f, 1, 1}};
    p.lifeInitializer.maxLife = 3.0f;
    EXPECT_FLOAT_EQ(LegacyParticleConverter::cycleDuration(p), 3.0f);
}

// A system that does not loop emitted, in the old step, until each of its maxParticles slots had
// been used once (ActionSystem::findUnusedParticle never reuses one without loop): its bursts'
// particles take slots from the rate's. The upgrade-from-main fixture's pickup sparks (300, 80 a
// second, a burst of 24) stopped emitting after 3.45 s, not maxParticles / rate's 3.75 s
TEST(ParticleMigration, NonLoopingSystemsEmitUntilTheirSlotsAreUsed){
    LegacyParticleSystem p;
    p.maxParticles = 300;
    p.rate = 80;
    p.loop = false;
    p.bursts = {{0.0f, 24, 24}};
    EXPECT_FLOAT_EQ(LegacyParticleConverter::emissionDuration(p), 3.45f);
    ParticleEffectDocument d;
    LegacyParticleConverter::apply(p, d);
    EXPECT_FLOAT_EQ(d.duration, 3.45f);
    // a burst later than that: the burst's time
    p.bursts = {{5.0f, 24, 24}};
    EXPECT_FLOAT_EQ(LegacyParticleConverter::emissionDuration(p), 5.0f);
    // a looping system's cycle is the old one (its slots are reused)
    p.loop = true;
    p.bursts = {{0.0f, 24, 24}};
    EXPECT_FLOAT_EQ(LegacyParticleConverter::emissionDuration(p), 3.75f);
    // bursts only: the old cycle (the longest life)
    p.loop = false;
    p.rate = 0;
    p.lifeInitializer.maxLife = 0.8f;
    EXPECT_FLOAT_EQ(LegacyParticleConverter::emissionDuration(p), 0.8f);
}

TEST(ParticleMigration, EasesSampleToSixteenKeys){
    for (EaseType type : {EaseType::QUAD_IN, EaseType::CUBIC_IN_OUT, EaseType::SINE_IN, EaseType::QUAD_OUT}){
        Ease ease(type);
        const Curve c = LegacyParticleConverter::sampleEase(ease);
        EXPECT_EQ(c.keys.size(), 16u);
        for (int i = 0; i <= 100; i++){
            const float t = i / 100.0f;
            EXPECT_NEAR(c.evaluate(t), ease.call(t), 2e-4f) << (int)type << " at " << t;
        }
    }
    Ease step(EaseType::STEP);
    const Curve c = LegacyParticleConverter::sampleEase(step);
    EXPECT_FLOAT_EQ(c.evaluate(0.99f), 0.0f);
    EXPECT_FLOAT_EQ(c.evaluate(1.0f), 1.0f);
}
