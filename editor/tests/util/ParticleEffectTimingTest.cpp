// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The analytic timing of an effect (docs/specs/S91-effect-authoring.md R19): a system's start
// delay, emission and lifetime; a looping one for ever; bursts in its first loop; sub-emitter
// children from their parents' events (Birth, Death, Collision bounded); a child file that does
// not read; and the JSON MCP gets (infinite times are null).

#include "util/ParticleEffectTiming.h"

#include <gtest/gtest.h>

#include <cmath>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;

namespace {

    ParticleEffectDocument oneShot(float duration, float delayLo, float delayHi, float lifeLo, float lifeHi) {
        ParticleEffectDocument d;
        d.looping = false;
        d.duration = duration;
        d.startDelay = MinMaxCurve::twoConstants(delayLo, delayHi);
        d.main.startLifetime = MinMaxCurve::twoConstants(lifeLo, lifeHi);
        return d;
    }

    struct Files {
        std::map<std::string, ParticleEffectDocument> documents;
        ParticleEffectTiming::Resolver resolver() {
            return [this](const std::string& path) -> const ParticleEffectDocument* {
                auto it = documents.find(path);
                return it == documents.end() ? nullptr : &it->second;
            };
        }
    };

}

TEST(ParticleEffectTiming, AOneShotSystemStartsEmitsAndEnds) {
    const ParticleEffectDocument d = oneShot(2.0f, 0.5f, 1.0f, 1.0f, 3.0f);
    const std::vector<ParticleTimingRow> rows = ParticleEffectTiming::compute({{"Spark", "fx/spark.particles", &d}});
    ASSERT_EQ(rows.size(), 1u);
    const ParticleTimingRow& row = rows[0];
    EXPECT_EQ(row.name, "Spark");
    EXPECT_EQ(row.depth, 0);
    EXPECT_FLOAT_EQ(row.start, 0.5f);
    EXPECT_FLOAT_EQ(row.startMax, 1.0f);
    // the latest start plus Duration, then the longest lifetime
    EXPECT_FLOAT_EQ(row.emitEnd, 3.0f);
    EXPECT_FLOAT_EQ(row.lifetimeMin, 1.0f);
    EXPECT_FLOAT_EQ(row.lifetimeMax, 3.0f);
    EXPECT_FLOAT_EQ(row.end, 6.0f);
    EXPECT_FALSE(row.looping);
    EXPECT_FLOAT_EQ(ParticleEffectTiming::span(rows), 6.0f);
}

TEST(ParticleEffectTiming, ALoopingSystemEmitsForEver) {
    ParticleEffectDocument d;
    d.looping = true;
    d.prewarm = true;
    const std::vector<ParticleTimingRow> rows = ParticleEffectTiming::compute({{"Smoke", "", &d}});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(std::isinf(rows[0].emitEnd));
    EXPECT_TRUE(std::isinf(rows[0].end));
    EXPECT_TRUE(rows[0].prewarm);
    // the span is finite: two lifetimes of a looping system
    EXPECT_FLOAT_EQ(ParticleEffectTiming::span(rows), 10.0f);
    const nlohmann::ordered_json json = ParticleEffectTiming::toJson(rows);
    EXPECT_TRUE(json["rows"][0]["emit_end"].is_null()) << json.dump();
    EXPECT_TRUE(json["rows"][0]["end"].is_null());
    EXPECT_EQ(json["rows"][0]["effect"], "");
    EXPECT_EQ(json["span"], 10.0);
}

TEST(ParticleEffectTiming, BurstsOnlyEndAtTheLastBurst) {
    ParticleEffectDocument d = oneShot(4.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst burst;
    burst.time = 0.5f;
    burst.cycles = 3;
    burst.interval = 0.25f;
    d.emission.bursts.push_back(burst);
    Burst late;
    late.time = 9.0f;   // after Duration: never fires
    d.emission.bursts.push_back(late);
    const std::vector<ParticleTimingRow> rows = ParticleEffectTiming::compute({{"Pop", "", &d}});
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].bursts.size(), 3u);
    EXPECT_FLOAT_EQ(rows[0].bursts[0], 0.5f);
    EXPECT_FLOAT_EQ(rows[0].bursts[2], 1.0f);
    EXPECT_FLOAT_EQ(rows[0].emitEnd, 1.0f);
    EXPECT_FLOAT_EQ(rows[0].end, 2.0f);
}

TEST(ParticleEffectTiming, AnEffectWithoutEmissionEndsAtItsStart) {
    ParticleEffectDocument d = oneShot(3.0f, 1.0f, 1.0f, 2.0f, 2.0f);
    d.emission.enabled = false;
    const std::vector<ParticleTimingRow> rows = ParticleEffectTiming::compute({{"Off", "", &d}});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FLOAT_EQ(rows[0].emitEnd, 1.0f);
    EXPECT_FLOAT_EQ(rows[0].end, 1.0f);
}

TEST(ParticleEffectTiming, ChildrenFollowTheirParentsEvents) {
    Files files;
    files.documents["fx/flash.particles"] = oneShot(1.0f, 0.0f, 0.0f, 0.5f, 0.5f);
    ParticleEffectDocument trail = oneShot(1.0f, 0.0f, 0.0f, 0.25f, 0.75f);
    trail.subEmittersModule.present = true;
    SubEmitterEntry grandchild;
    grandchild.trigger = SubEmitterTrigger::Collision;
    grandchild.effect = "fx/flash.particles";
    trail.subEmitters.push_back(grandchild);
    files.documents["fx/trail.particles"] = trail;

    ParticleEffectDocument parent = oneShot(2.0f, 1.0f, 1.0f, 1.0f, 2.0f);
    parent.subEmittersModule.present = true;
    SubEmitterEntry death;
    death.trigger = SubEmitterTrigger::Death;
    death.effect = "fx/flash.particles";
    SubEmitterEntry birth;
    birth.trigger = SubEmitterTrigger::Birth;
    birth.effect = "fx/trail.particles";
    SubEmitterEntry missing;
    missing.effect = "fx/gone.particles";
    parent.subEmitters = {death, birth, missing};

    const std::vector<ParticleTimingRow> rows = ParticleEffectTiming::compute({{"Rocket", "fx/rocket.particles", &parent}}, files.resolver());
    // the parent, its three entries, and the trail's child, depth first
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_FLOAT_EQ(rows[0].start, 1.0f);
    EXPECT_FLOAT_EQ(rows[0].emitEnd, 3.0f);
    EXPECT_FLOAT_EQ(rows[0].end, 5.0f);

    const ParticleTimingRow& flash = rows[1];
    EXPECT_EQ(flash.name, "flash");
    EXPECT_EQ(flash.depth, 1);
    EXPECT_EQ(flash.parent, 0);
    EXPECT_EQ(flash.subEmitter, 0);
    EXPECT_EQ(flash.trigger, "death");
    // the first death: the parent's first emission plus its shortest life; the last: the parent's end
    EXPECT_FLOAT_EQ(flash.start, 2.0f);
    EXPECT_FLOAT_EQ(flash.emitEnd, 5.0f);
    EXPECT_FLOAT_EQ(flash.end, 5.5f);
    EXPECT_FALSE(flash.eventDriven);

    const ParticleTimingRow& trailRow = rows[2];
    EXPECT_EQ(trailRow.trigger, "birth");
    EXPECT_EQ(trailRow.subEmitter, 1);
    EXPECT_FLOAT_EQ(trailRow.start, 1.0f);
    EXPECT_FLOAT_EQ(trailRow.emitEnd, 3.0f);
    EXPECT_FLOAT_EQ(trailRow.end, 3.75f);

    // the trail's own child, on collisions: a bound over the trail's life
    const ParticleTimingRow& spark = rows[3];
    EXPECT_EQ(spark.depth, 2);
    EXPECT_EQ(spark.parent, 2);
    EXPECT_EQ(spark.trigger, "collision");
    EXPECT_TRUE(spark.eventDriven);
    EXPECT_FLOAT_EQ(spark.start, 1.0f);
    EXPECT_FLOAT_EQ(spark.end, 4.25f);

    const ParticleTimingRow& gone = rows[4];
    EXPECT_TRUE(gone.missing);
    EXPECT_EQ(gone.subEmitter, 2);
    const nlohmann::ordered_json json = ParticleEffectTiming::toJson(rows);
    EXPECT_EQ(json["rows"][4]["missing"], true) << json.dump();
    EXPECT_EQ(json["rows"][1]["lifetime"][0], 0.5);
}

TEST(ParticleEffectTiming, ADisabledSubEmittersModuleHasNoChildren) {
    Files files;
    files.documents["fx/flash.particles"] = oneShot(1.0f, 0.0f, 0.0f, 0.5f, 0.5f);
    ParticleEffectDocument parent = oneShot(2.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    parent.subEmittersModule.present = true;
    parent.subEmittersModule.enabled = false;
    SubEmitterEntry entry;
    entry.effect = "fx/flash.particles";
    parent.subEmitters.push_back(entry);
    EXPECT_EQ(ParticleEffectTiming::compute({{"P", "", &parent}}, files.resolver()).size(), 1u);
}
