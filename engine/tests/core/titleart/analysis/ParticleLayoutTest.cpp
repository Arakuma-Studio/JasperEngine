// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Particles family's CPU layouts (analysis/ParticleLayout.h; docs/specs/
// S83-title-effects-generators.md §6.7.1, §6.7.2, R25, D6, AC5), without a GPU:
// - CountStability: `count` 400 -> 401 keeps the first 400 instance records bit-identical, on
//   every emitter (AC5's particles clause).
// - FlippedMaskCell: on the layer emitter a flipped mask cell changes only the records of the
//   particles whose tries land on it (R25).
// - Motion*: a record's state is a closed-form function of the time: time 1 is the record
//   (Pachislop's still), the same time gives the same state, a loop wraps, and no animation
//   param reaches the records (they stay valid for a live frame).
// - Determinism, seeds, the mobile tier's cap in the cost model, Pachislop's emitter lattice,
//   and Ink Splatter's drops.

#include <gtest/gtest.h>

#include "titleart/TitleEffectRegistry.h"
#include "titleart/analysis/ParticleLayout.h"

#include <cstring>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    Json defaultsOf(const char* type) {
        const GeneratorDef* def = findGenerator(type);
        return def ? defaultParams(def->params) : Json::object();
    }

    bool sameBits(float a, float b) {
        uint32_t x = 0, y = 0;
        std::memcpy(&x, &a, sizeof(x));
        std::memcpy(&y, &b, sizeof(y));
        return x == y;
    }

    bool sameRecord(const ParticleRecord& a, const ParticleRecord& b) {
        if (a.index != b.index || a.glint != b.glint) return false;
        const float fa[] = {a.x, a.y, a.mx, a.my, a.w, a.h, a.rot, a.color[0], a.color[1], a.color[2], a.color[3],
                            a.blur, a.aspect, a.phase, a.z, a.side, a.motionPhase};
        const float fb[] = {b.x, b.y, b.mx, b.my, b.w, b.h, b.rot, b.color[0], b.color[1], b.color[2], b.color[3],
                            b.blur, b.aspect, b.phase, b.z, b.side, b.motionPhase};
        for (size_t i = 0; i < sizeof(fa) / sizeof(fa[0]); i++) {
            if (!sameBits(fa[i], fb[i])) return false;
        }
        return true;
    }

    ParticleSpace canvasSpace() {
        ParticleSpace s;
        s.canvasWidth = 1920;
        s.canvasHeight = 1080;
        return s;
    }

    std::vector<ParticleRecord> layout(const Json& params, const EmitMap* map = nullptr, double seed = 1.0) {
        const ParamValues p(params);
        return layoutParticles(p, readParticleLook(p, seed), canvasSpace(), map);
    }

    // a Color grid of 16 canvas px cells holding an ink rectangle that fills its snapped box
    Grid inkGrid() {
        Grid g;
        g.w = 32;
        g.h = 16;
        g.cellComp = 16.0;
        g.cell = 16.0;
        g.x0 = -256.0;
        g.y0 = -128.0;
        g.alpha.assign((size_t)g.w * g.h, 0.0f);
        g.rgba.assign((size_t)g.w * g.h * 4, 0.0f);
        for (int y = 4; y < 12; y++) {
            for (int x = 4; x < 28; x++) {
                const size_t i = (size_t)y * g.w + x;
                g.alpha[i] = 1.0f;
                g.rgba[i * 4 + 0] = 0.8f;
                g.rgba[i * 4 + 1] = 0.3f;
                g.rgba[i * 4 + 2] = 0.1f;
                g.rgba[i * 4 + 3] = 1.0f;
            }
        }
        return g;
    }

    void expectPrefixKept(const std::vector<ParticleRecord>& at400, const std::vector<ParticleRecord>& at401, const char* what) {
        ASSERT_FALSE(at400.empty()) << what;
        ASSERT_GE(at401.size(), at400.size()) << what;
        for (size_t i = 0; i < at400.size(); i++) {
            EXPECT_LT(at400[i].index, 400u) << what;
            EXPECT_TRUE(sameRecord(at400[i], at401[i])) << what << ": record " << i << " (particle " << at400[i].index << ") changed";
        }
        for (size_t i = at400.size(); i < at401.size(); i++) EXPECT_EQ(at401[i].index, 400u) << what;
    }

}

TEST(ParticleLayout, CountStability) {
    struct Case {
        const char* what;
        Json overrides;
    };
    const std::vector<Case> cases = {
        {"ellipse burst (defaults)", Json::object()},
        {"rect stream", Json{{"emitter", "rect"}, {"distribution", "stream"}, {"shape", "ember"}}},
        {"ring scatter", Json{{"emitter", "ring"}, {"distribution", "scatter"}, {"shape", "star"}, {"tumble", 40}}},
        {"line burst", Json{{"emitter", "line"}, {"shape", "rain"}}},
        {"point scatter", Json{{"emitter", "point"}, {"distribution", "scatter"}}},
        {"full frame confetti", Json{{"emitter", "full"}, {"distribution", "scatter"}, {"shape", "confetti"}, {"color_mode", "palette"}, {"tumble", 85}}},
        {"glint mix", Json{{"glint_mix", 20}}},
    };
    for (const Case& c : cases) {
        Json params = defaultsOf("particles");
        ASSERT_FALSE(params.empty());
        for (auto it = c.overrides.begin(); it != c.overrides.end(); ++it) params[it.key()] = it.value();
        params["count"] = 400;
        const std::vector<ParticleRecord> at400 = layout(params);
        params["count"] = 401;
        const std::vector<ParticleRecord> at401 = layout(params);
        expectPrefixKept(at400, at401, c.what);
    }

    // the layer emitter: placement tries keyed by the particle, so the prefix holds there too
    const Grid g = inkGrid();
    EmitMap map;
    ASSERT_TRUE(buildEmitMap(g, false, 4.0, map));
    Json params = defaultsOf("particles");
    params["emitter"] = "layer";
    params["color_mode"] = "sample";
    params["count"] = 400;
    const std::vector<ParticleRecord> at400 = layout(params, &map);
    params["count"] = 401;
    expectPrefixKept(at400, layout(params, &map), "layer emitter");
}

TEST(ParticleLayout, FlippedMaskCellMovesOnlyItsParticles) {
    Json params = defaultsOf("particles");
    params["emitter"] = "layer";
    params["source"] = "fill";
    // the heading and radius colours: a particle's record then depends on its own samples only
    params["direction"] = "angle";
    params["color_mode"] = "radial";
    params["count"] = 5000;
    const ParamValues p(params);
    const ParticleLook look = readParticleLook(p, 1.0);

    const Grid before = inkGrid();
    Grid after = inkGrid();
    // an interior cell: the ink box and every other cell keep their mask
    const size_t flipped = (size_t)8 * after.w + 15;
    after.alpha[flipped] = 0.0f;
    for (int k = 0; k < 4; k++) after.rgba[flipped * 4 + k] = 0.0f;
    EmitMap mapBefore, mapAfter;
    ASSERT_TRUE(buildEmitMap(before, false, 4.0, mapBefore));
    ASSERT_TRUE(buildEmitMap(after, false, 4.0, mapAfter));
    for (int k = 0; k < 4; k++) ASSERT_EQ(mapBefore.box[k], mapAfter.box[k]);

    // the particles whose placement tries land differently: those that took the flipped cell
    std::set<uint32_t> touched;
    for (int i = 0; i < look.count; i++) {
        gen::Rng a = particleStream(look.seed, i, 0);
        gen::Rng b = particleStream(look.seed, i, 0);
        MapSample sa, sb;
        const bool okA = sampleEmitMap(mapBefore, a, sa);
        const bool okB = sampleEmitMap(mapAfter, b, sb);
        if (okA != okB || sa.cell != sb.cell || sa.x != sb.x || sa.y != sb.y) {
            touched.insert((uint32_t)i);
            EXPECT_EQ(sa.cell, (int)flipped) << "particle " << i << " changed its sample off the flipped cell";
        }
    }
    EXPECT_FALSE(touched.empty());
    EXPECT_LT(touched.size(), (size_t)look.count / 20);

    const std::vector<ParticleRecord> ra = layoutParticles(p, look, canvasSpace(), &mapBefore);
    const std::vector<ParticleRecord> rb = layoutParticles(p, look, canvasSpace(), &mapAfter);
    std::map<uint32_t, ParticleRecord> byIndex;
    for (const ParticleRecord& r : rb) byIndex[r.index] = r;
    size_t kept = 0;
    for (const ParticleRecord& r : ra) {
        if (touched.count(r.index)) continue;
        auto it = byIndex.find(r.index);
        ASSERT_NE(it, byIndex.end()) << "particle " << r.index << " vanished";
        EXPECT_TRUE(sameRecord(r, it->second)) << "particle " << r.index << " moved without touching the flipped cell";
        kept++;
    }
    EXPECT_EQ(kept + touched.size(), ra.size());
}

TEST(ParticleLayout, MotionTimeOneIsTheStill) {
    const std::vector<ParticleRecord> records = layout(defaultsOf("particles"));
    ASSERT_FALSE(records.empty());
    const ParticleMotion still{};   // time 1, no gravity, spin, twinkle or fade
    for (const ParticleRecord& r : records) {
        const ParticleState s = particleAt(r, still, true, r.glint);
        EXPECT_EQ(s.x, (double)r.x);
        EXPECT_EQ(s.y, (double)r.y);
        EXPECT_EQ(s.rot, (double)r.rot);
        EXPECT_EQ(s.colorScale, 1.0);
    }
}

TEST(ParticleLayout, MotionIsClosedForm) {
    const std::vector<ParticleRecord> records = layout(defaultsOf("particles"));
    ASSERT_FALSE(records.empty());
    ParticleMotion m;
    m.time = 0.37;
    m.gravity = 240.0;
    m.spin = 360.0;
    m.twinkle = 0.5;
    m.fadeIn = 0.2;
    m.fadeOut = 0.3;
    for (const ParticleRecord& r : records) {
        // the same time twice: the same state, whatever was evaluated in between
        const ParticleState a = particleAt(r, m, true, true);
        ParticleMotion other = m;
        other.time = 0.9;
        (void)particleAt(r, other, true, true);
        const ParticleState b = particleAt(r, m, true, true);
        EXPECT_EQ(a.x, b.x);
        EXPECT_EQ(a.y, b.y);
        EXPECT_EQ(a.rot, b.rot);
        EXPECT_EQ(a.colorScale, b.colorScale);
    }
    // time 0 without a loop: every particle where it was born, before gravity pulls
    ParticleMotion birth;
    birth.time = 0.0;
    birth.gravity = 500.0;
    for (const ParticleRecord& r : records) {
        const ParticleState s = particleAt(r, birth, false, false);
        EXPECT_NEAR(s.x, (double)r.x - r.mx, 1e-9);
        EXPECT_NEAR(s.y, (double)r.y - r.my, 1e-9);
    }
    // gravity pulls a particle down by its full amount at time 1
    ParticleMotion fallen;
    fallen.gravity = 100.0;
    EXPECT_NEAR(particleAt(records.front(), fallen, false, false).y, (double)records.front().y - 100.0, 1e-9);
}

TEST(ParticleLayout, MotionLoopWraps) {
    const std::vector<ParticleRecord> records = layout(defaultsOf("particles"));
    ASSERT_FALSE(records.empty());
    ParticleMotion m;
    m.loop = true;
    m.gravity = 120.0;
    m.spin = 90.0;
    m.fadeIn = 0.1;
    m.fadeOut = 0.1;
    bool phased = false;
    for (const ParticleRecord& r : records) {
        m.time = 0.0;
        const ParticleState start = particleAt(r, m, true, false);
        m.time = 1.0;
        const ParticleState end = particleAt(r, m, true, false);
        // a loop's end is its start: a continuous stream when time runs 0 -> 1 again
        EXPECT_NEAR(start.x, end.x, 1e-6);
        EXPECT_NEAR(start.y, end.y, 1e-6);
        EXPECT_NEAR(start.rot, end.rot, 1e-6);
        EXPECT_NEAR(start.colorScale, end.colorScale, 1e-6);
        if (r.motionPhase > 0.01f && r.motionPhase < 0.99f) phased = true;
    }
    // each particle on its own offset (not all born together)
    EXPECT_TRUE(phased);
}

TEST(ParticleLayout, AnimationParamsLeaveTheRecords) {
    Json params = defaultsOf("particles");
    params["glint_mix"] = 10;
    const std::vector<ParticleRecord> still = layout(params);
    params["time"] = 0.25;
    params["loop"] = true;
    params["gravity"] = 300;
    params["spin"] = 720;
    params["twinkle"] = 1;
    params["fade_in"] = 0.5;
    params["fade_out"] = 0.5;
    const std::vector<ParticleRecord> animated = layout(params);
    ASSERT_EQ(still.size(), animated.size());
    for (size_t i = 0; i < still.size(); i++) EXPECT_TRUE(sameRecord(still[i], animated[i])) << "record " << i;
}

TEST(ParticleLayout, DeterministicAndSeeded) {
    const Json params = defaultsOf("particles");
    const std::vector<ParticleRecord> a = layout(params);
    const std::vector<ParticleRecord> b = layout(params);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); i++) EXPECT_TRUE(sameRecord(a[i], b[i])) << "record " << i;
    const std::vector<ParticleRecord> reseeded = layout(params, nullptr, 2.0);
    ASSERT_FALSE(reseeded.empty());
    EXPECT_FALSE(sameRecord(a.front(), reseeded.front()));
}

TEST(ParticleLayout, MobileTierCapsTheCount) {
    const GeneratorDef* def = findGenerator("particles");
    ASSERT_NE(def, nullptr);
    ASSERT_NE(def->estimate, nullptr);
    Json params = defaultParams(def->params);
    params["count"] = 5000;
    const ParamValues p(params);
    const FxSize size{1920, 1080, 1.0f};
    EXPECT_EQ(def->estimate(p, size, FxTier::Desktop).instances, 5000);
    EXPECT_EQ(def->estimate(p, size, FxTier::Mobile).instances, PARTICLES_MOBILE_MAX);
}

TEST(ParticleLayout, EmitterLatticeIsPachislops) {
    const double box[4] = {-301.0, -97.5, 288.0, 120.0};
    const GridSpec s = particleGridSpec(box);
    // the box +- 8 canvas px snapped outward to 64
    EXPECT_EQ(s.x0, -320.0);
    EXPECT_EQ(s.y0, -128.0);
    EXPECT_EQ(s.cellComp, 3.0);
    EXPECT_EQ(s.w, 214);     // ceil(640 / 3)
    EXPECT_EQ(s.h, 86);      // ceil(256 / 3)
    EXPECT_EQ(s.maxTaps, 8);
    EXPECT_TRUE(s.ceilTaps);
    EXPECT_EQ(s.packing, GridPacking::Color);
}

TEST(ParticleLayout, InkSplatterDrops) {
    // an 8 px content grid with a block of ink
    Grid g;
    g.w = 64;
    g.h = 32;
    g.cellComp = 8.0;
    g.cell = 8.0;
    g.x0 = -256.0;
    g.y0 = -128.0;
    g.alpha.assign((size_t)g.w * g.h, 0.0f);
    for (int y = 12; y < 20; y++) {
        for (int x = 20; x < 44; x++) g.alpha[(size_t)y * g.w + x] = 1.0f;
    }
    Json params = defaultsOf("ink_splatter");
    ASSERT_FALSE(params.empty());
    const std::vector<InkBlot> a = layoutInkSplatter(g, ParamValues(params), 1.0);
    ASSERT_FALSE(a.empty());
    // growth animates live: the drops are the same at any growth
    params["growth"] = 0.3;
    const std::vector<InkBlot> grown = layoutInkSplatter(g, ParamValues(params), 1.0);
    ASSERT_EQ(a.size(), grown.size());
    for (size_t i = 0; i < a.size(); i++) {
        EXPECT_EQ(a[i].ax, grown[i].ax);
        EXPECT_EQ(a[i].by, grown[i].by);
        EXPECT_EQ(a[i].rb, grown[i].rb);
        EXPECT_EQ(a[i].seed, grown[i].seed);
        EXPECT_GE(a[i].rb, 0.6);
    }
    const std::vector<InkBlot> reseeded = layoutInkSplatter(g, ParamValues(params), 2.0);
    ASSERT_FALSE(reseeded.empty());
    EXPECT_NE(a.front().ax, reseeded.front().ax);
    params["amount"] = 0;
    EXPECT_TRUE(layoutInkSplatter(g, ParamValues(params), 1.0).empty());
}
