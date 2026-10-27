// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The merge patch the Inspector's multi-edit sends to the other selected effects
// (ParticleEffectEdit::diff): applied to the effect it came from it gives the edited effect, a
// value set back to its default becomes a null, and applied to another effect it changes only
// what was edited.

#include "util/ParticleEffectEdit.h"

#include "particles/ParticleEffectCodec.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;

namespace {

    nlohmann::ordered_json patchOf(const ParticleEffectDocument& before, const ParticleEffectDocument& after){
        return ParticleEffectEdit::diff(ParticleEffectCodec::toKeyedJson(before), ParticleEffectCodec::toKeyedJson(after));
    }

}

TEST(ParticleEffectEdit, DiffTurnsBeforeIntoAfter){
    ParticleEffectDocument before;
    before.duration = 3.0f;
    before.noise.present = true;
    ParticleEffectDocument after = before;
    after.emission.rateOverTime = MinMaxCurve::twoConstants(4.0f, 9.0f);
    after.noise.present = false;                 // a module removed: null
    after.duration = 5.0f;                       // back to the default: null
    after.trails.present = true;
    after.trails.lifetime = MinMaxCurve(0.25f);

    const nlohmann::ordered_json patch = patchOf(before, after);
    EXPECT_TRUE(patch["duration"].is_null()) << patch.dump();
    EXPECT_TRUE(patch["modules"]["noise"].is_null()) << patch.dump();

    ParticleEffectDocument patched = before;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(patched, patch, diagnostics)) << ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(ParticleEffectCodec::write(patched), ParticleEffectCodec::write(after));
}

TEST(ParticleEffectEdit, DiffChangesOnlyTheEditOfAnotherEffect){
    ParticleEffectDocument shown;
    ParticleEffectDocument edited = shown;
    edited.main.startSpeed = MinMaxCurve(12.0f);

    ParticleEffectDocument other;
    other.maxParticles = 77;
    other.colorOverLifetime.present = true;
    ParticleEffectDocument expected = other;
    expected.main.startSpeed = MinMaxCurve(12.0f);

    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(other, patchOf(shown, edited), diagnostics)) << ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(ParticleEffectCodec::write(other), ParticleEffectCodec::write(expected));
}

TEST(ParticleEffectEdit, NoEditNoPatch){
    ParticleEffectDocument effect;
    effect.renderer.blend = ParticleBlend::Additive;
    EXPECT_TRUE(patchOf(effect, effect).empty());
}

TEST(ParticleEffectEdit, ArraysAreReplacedWhole){
    ParticleEffectDocument before;
    Burst burst;
    burst.count = MinMaxCurve(5.0f);
    before.emission.bursts = {burst, burst};
    ParticleEffectDocument after = before;
    after.emission.bursts.pop_back();

    const nlohmann::ordered_json patch = patchOf(before, after);
    ASSERT_TRUE(patch["modules"]["emission"]["bursts"].is_array()) << patch.dump();
    EXPECT_EQ(patch["modules"]["emission"]["bursts"].size(), 1u);
}
