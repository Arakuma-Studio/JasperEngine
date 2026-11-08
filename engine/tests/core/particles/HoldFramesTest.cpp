// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 AC4 (R5): Hold Frames at 12 Hz on a 60 Hz frame loop draws a state that changes only every
// five frames (the drawn records' hash per frame), and packs only then: four of every five packs
// are saved (>= 70 %). Without Hold Frames every frame packs and changes (the red control).

#include "ParticleTestUtil.h"

#include "memory/Arena.h"
#include "particles/ParticleRender.h"

#include <cstring>
#include <vector>

using namespace particletest;

namespace {

    ParticleEffectDocument heldEffect(float holdRate){
        ParticleEffectDocument d;
        d.tickRate = 60;
        d.main.startLifetime = MinMaxCurve(3.0f);
        d.main.startSpeed = MinMaxCurve(2.0f);
        d.emission.rateOverTime = MinMaxCurve(40.0f);
        d.pixelArt.present = true;
        d.pixelArt.holdFrameRate = holdRate;
        d.renderer.renderMode = RenderMode::Pixel;
        return d;
    }

    uint64_t hashOf(const PackResult& pack){
        uint64_t h = 1469598103934665603ull;
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(pack.instances);
        for (size_t i = 0; i < pack.count * sizeof(ParticleInstance); i++) h = (h ^ bytes[i]) * 1099511628211ull;
        return h ^ pack.count;
    }

    struct Run {
        std::vector<uint64_t> hashes;
        uint64_t packs = 0;
    };

    Run run(float holdRate){
        Sim sim(heldEffect(holdRate));
        HeldPack held;
        Run out;
        const uint64_t before = ParticleRenderPrep::packCount();
        for (int frame = 0; frame < 120; frame++){
            sim.runtime.advance(sim.input, 1.0 / 60.0);
            memory::Arena arena("HoldFramesTest");
            const PackResult pack = holdRate > 0.0f ? held.get(sim.runtime, RenderView(), arena, nullptr, holdRate)
                                                    : ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
            out.hashes.push_back(hashOf(pack));
        }
        out.packs = ParticleRenderPrep::packCount() - before;
        return out;
    }

}

TEST(HoldFrames, DrawnStateChangesOnlyAtSteps){
    const Run held = run(12.0f);
    // after the first frames (the first particles), the state holds for 5 frames at a time
    int changes = 0;
    for (size_t f = 1; f < held.hashes.size(); f++){
        if (held.hashes[f] != held.hashes[f - 1]){
            changes++;
            // a change lands where 12 Hz crosses a step: frames 5, 10, 15, ... (time f / 60 s)
            EXPECT_EQ(((f + 1) % 5), 0u) << "the drawn state changed at frame " << f;
        }
    }
    EXPECT_GE(changes, 20);
    // the packs: one per step (24 in two seconds), against one per frame without it
    const Run every = run(0.0f);
    EXPECT_EQ(every.packs, 120u);
    EXPECT_LE((double)held.packs, 0.3 * (double)every.packs) << held.packs << " packs with Hold Frames";
    int everyChanges = 0;
    for (size_t f = 1; f < every.hashes.size(); f++) everyChanges += every.hashes[f] != every.hashes[f - 1] ? 1 : 0;
    EXPECT_GT(everyChanges, 100) << "without Hold Frames the drawn state changes every frame";
}
