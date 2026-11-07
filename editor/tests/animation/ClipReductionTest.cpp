// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S77 R19-R24, D5 (AC9, AC10): every row of D5's table, on curves planted here. The reducer's
// output is measured with measureClipError, the error the tools report, never read off the reducer.

#include <gtest/gtest.h>

#include "animation/AnimationClipIO.h"
#include "animation/ClipOptimize.h"
#include "animation/ClipReduction.h"
#include "animation/ClipReport.h"
#include "pool/AnimationClipPool.h"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

using namespace jasperengine;

namespace {

    constexpr double kPi = 3.14159265358979323846;

    ClipKey key(float time, std::vector<float> value){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        return k;
    }

    ClipTrackData track(const std::string& path, ClipChannel channel, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = channel;
        t.keys = std::move(keys);
        return t;
    }

    std::vector<float> aroundY(double degrees, double scale = 1.0){
        const double half = degrees * kPi / 360.0;
        return {0.0f, (float)(std::sin(half) * scale), 0.0f, (float)(std::cos(half) * scale)};
    }

    anim::ModelNodeInfo node(const std::string& name, int parent, const std::string& path, Vector3 position){
        anim::ModelNodeInfo info;
        info.name = name;
        info.parent = parent;
        info.path = path;
        info.position = position;
        return info;
    }

    // Hips at 1 m, with two legs of 0.5 m bones ending in feet, and an arm of the same shape
    anim::ClipRig bodyRig(){
        anim::ClipRig rig;
        rig.assign("test rig", {
            node("Hips", -1, "Hips", Vector3(0, 1, 0)),
            node("LeftLeg", 0, "Hips/LeftLeg", Vector3(0.2f, -0.5f, 0)),
            node("LeftFoot", 1, "Hips/LeftLeg/LeftFoot", Vector3(0, -0.5f, 0)),
            node("Arm", 0, "Hips/Arm", Vector3(-0.2f, -0.5f, 0)),
            node("Hand", 3, "Hips/Arm/Hand", Vector3(0, -0.5f, 0)),
        });
        return rig;
    }

    // a smooth rotation with a little noise, on a 30 Hz grid
    ClipTrackData wavingRotation(const std::string& path, double seconds, double degrees, double frequency, double noiseDegrees = 0.0){
        std::vector<ClipKey> keys;
        const int count = (int)std::round(seconds * 30.0) + 1;
        for (int i = 0; i < count; i++){
            const double t = i / 30.0;
            const double angle = degrees * std::sin(t * frequency * 2.0 * kPi) + noiseDegrees * std::sin((double)i * 12.9898) * std::cos((double)i * 78.233);
            keys.push_back(key((float)t, aroundY(angle)));
        }
        return track(path, ClipChannel::Rotation, std::move(keys));
    }

    ClipData walkClip(){
        ClipData data;
        data.tracks.push_back(wavingRotation("Hips", 2.0, 8.0, 1.0, 0.05));
        data.tracks.push_back(wavingRotation("Hips/LeftLeg", 2.0, 25.0, 1.0, 0.05));
        data.tracks.push_back(wavingRotation("Hips/LeftLeg/LeftFoot", 2.0, 12.0, 2.0, 0.05));
        data.tracks.push_back(wavingRotation("Hips/Arm", 2.0, 25.0, 1.0, 0.05));
        data.tracks.push_back(wavingRotation("Hips/Arm/Hand", 2.0, 12.0, 2.0, 0.05));
        return data;
    }

    std::shared_ptr<const AnimationClip> throughFile(const ClipData& data){
        return AnimationClip::build(anim::parseClip(anim::writeClip(data)));
    }

    double measured(const ClipData& source, const ClipData& reduced, const anim::ClipRig& rig, double skin = 100.0){
        ClipData normalized = source;
        anim::normalize(normalized);
        const anim::ClipErrorReport report = anim::measureClipError(*AnimationClip::build(normalized), *throughFile(reduced), rig, skin);
        EXPECT_TRUE(report.measured);
        return report.maxErrorMm;
    }

    size_t keyCount(const ClipData& data){
        size_t keys = 0;
        for (const ClipTrackData& t : data.tracks) keys += t.keys.size();
        return keys;
    }

    const ClipTrackData* findTrack(const ClipData& data, const std::string& path, ClipChannel channel){
        const int index = data.findTrack(path, channel);
        return index < 0 ? nullptr : &data.tracks[(size_t)index];
    }

    anim::ClipReductionOptions tolerance(double mm){
        anim::ClipReductionOptions options;
        options.toleranceMm = mm;
        return options;
    }

    std::vector<double> flatLimits(const anim::ClipRig& rig, double mm){
        return std::vector<double>(rig.size(), mm);
    }

    // reduced at one per-track value, without the search: what the metric alone decides
    ClipData atRatio(const ClipData& source, const anim::ClipRig& rig, double limitMm, double skin = 100.0){
        ClipData normalized = source;
        anim::normalize(normalized);
        return anim::reduceClipAtRatio(normalized, *AnimationClip::build(normalized), rig, flatLimits(rig, limitMm), skin, 1.0);
    }

}

// ---- R20, R21: what the output is ----

TEST(ClipReduction, OutputKeysAreASubsetInOrderWithTheirValuesUnchanged){
    const anim::ClipRig rig = bodyRig();
    const ClipData source = walkClip();
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(1.0));
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_LT(keyCount(result.clip), keyCount(source)) << "the curves are smooth: some keys must go";
    ClipData normalized = source;
    anim::normalize(normalized);
    ASSERT_EQ(result.clip.tracks.size(), normalized.tracks.size());
    for (size_t t = 0; t < normalized.tracks.size(); t++){
        const std::vector<ClipKey>& all = normalized.tracks[t].keys;
        size_t at = 0;
        for (const ClipKey& kept : result.clip.tracks[t].keys){
            while (at < all.size() && all[at].time != kept.time) at++;
            ASSERT_LT(at, all.size()) << normalized.tracks[t].path << ": a key at " << kept.time << " is not a source key, or out of order";
            EXPECT_EQ(all[at].value, kept.value) << "a value changed";
            at++;
        }
    }
}

TEST(ClipReduction, TheFileFormHoldsTheSameBytesKeyForKey){
    // a key's text in the output is its text in the source: no rounding, no sign change
    const anim::ClipRig rig = bodyRig();
    const ClipData source = walkClip();
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(1.0));
    ASSERT_TRUE(result.ok) << result.error;
    const anim::ClipParseResult fromSource = anim::parseClip(anim::writeClip(source));
    const anim::ClipParseResult fromOutput = anim::parseClip(anim::writeClip(result.clip));
    ASSERT_TRUE(fromSource.ok());
    ASSERT_TRUE(fromOutput.ok());
    for (size_t t = 0; t < fromSource.data->tracks.size(); t++){
        const std::vector<ClipKey>& all = fromSource.data->tracks[t].keys;
        size_t at = 0;
        for (const ClipKey& kept : fromOutput.data->tracks[t].keys){
            while (at < all.size() && all[at].time != kept.time) at++;
            ASSERT_LT(at, all.size());
            EXPECT_EQ(all[at].value, kept.value);
        }
    }
}

TEST(ClipReduction, KeepsLengthAndEveryEventAndNeverRemovesATrack){
    const anim::ClipRig rig = bodyRig();
    ClipData source = walkClip();
    ClipEventData event;
    event.time = 0.5f;
    event.function = "onStep";
    event.args.push_back(AnimationEvent::numberArg(3.0));
    source.events.push_back(event);
    // S40's audio cues ride along like events, and one sits past every key: it holds the length
    ClipAudioCue cue;
    cue.time = 2.5f;
    cue.kind = ClipCueKind::Snapshot;
    cue.snapshot = "Quiet";
    cue.seconds = 0.5f;
    source.audio.push_back(cue);
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(5.0));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FLOAT_EQ(result.clip.effectiveLength(), source.effectiveLength());
    EXPECT_EQ(result.clip.tracks.size(), source.tracks.size());
    ASSERT_EQ(result.clip.events.size(), 1u);
    EXPECT_EQ(result.clip.events[0].function, "onStep");
    EXPECT_EQ(result.clip.events[0].args, event.args);
    ASSERT_EQ(result.clip.audio.size(), 1u);
    EXPECT_EQ(result.clip.audio[0], cue);
    EXPECT_FLOAT_EQ(result.clip.effectiveLength(), 2.5f);
    // the same event bytes in the file
    ClipData noKeys = source;
    noKeys.tracks.clear();
    ClipData outNoKeys = result.clip;
    outNoKeys.tracks.clear();
    EXPECT_EQ(anim::writeClip(noKeys), anim::writeClip(outNoKeys));
}

TEST(ClipReduction, AnImplicitLengthSurvivesWhenEveryTrackIsConstant){
    const anim::ClipRig rig = bodyRig();
    ClipData source;
    source.tracks.push_back(track("Hips", ClipChannel::Rotation, {key(0, aroundY(5)), key(1, aroundY(5)), key(2, aroundY(5))}));
    source.tracks.push_back(track("Hips/Arm", ClipChannel::Rotation, {key(0, aroundY(1)), key(2, aroundY(1))}));
    ASSERT_FLOAT_EQ(source.effectiveLength(), 2.0f);
    const ClipData reduced = atRatio(source, rig, 1.0);
    EXPECT_FLOAT_EQ(reduced.effectiveLength(), 2.0f) << "one track keeps the clip's last time";
    EXPECT_EQ(keyCount(reduced), 3u) << "one track 1 key, the other first and last";
}

TEST(ClipReduction, ADeterministicResultAtAnyThreadCount){
    const anim::ClipRig rig = bodyRig();
    const ClipData source = walkClip();
    const std::string alone = anim::writeClip(anim::reduceClip(source, rig, tolerance(1.0)).clip);
    std::vector<std::string> texts(8);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < texts.size(); i++){
        threads.emplace_back([&, i](){ texts[i] = anim::writeClip(anim::reduceClip(source, rig, tolerance(1.0)).clip); });
    }
    for (std::thread& thread : threads) thread.join();
    for (const std::string& text : texts) EXPECT_EQ(text, alone);
}

// ---- D5 row 1: float32, 1 - c^2 on raw keys ----

TEST(ClipReduction, RotationErrorIsReadBelowTheNoiseOfAFloatKey){
    // 0.8 mm at 1 m, between keys off unit length by what a float keeps and what 6 digits keep
    const double degrees = 2.0 * std::asin(0.0008 / 2.0) * 180.0 / kPi;
    for (const double noise : {1.0 + 1.2e-7, 1.0 + 5e-6}){
        const std::vector<float> a = aroundY(0.0, noise);
        const std::vector<float> b = aroundY(degrees, noise);
        const double chord = anim::rotationChordMetres(quaternionFromXYZW(a.data()), quaternionFromXYZW(b.data()), 1.0);
        EXPECT_NEAR(chord * 1000.0, 0.8, 0.02) << "noise " << noise << ": 1 - c^2 on raw keys reads this as 0";
    }
}

TEST(ClipReduction, Float32NoiseFixtureKeepsAKeyTheRawMetricDrops){
    // Keys off unit by 1e-6 (a file's 6 digits do this, and the reader keeps them within 1e-5): the
    // middle key is 2 mm off its neighbours' chord at this reach, far over the 1 mm limit, and the
    // raw 1 - c^2 reads it as 0
    anim::ClipRig rig;
    rig.assign("test rig", {node("Root", -1, "Root", Vector3(0, 0, 0)), node("Tip", 0, "Root/Tip", Vector3(0, 0, 0.9f))});
    const double off = 1.0 + 3e-6;
    const double degrees = 2.0 * std::asin(0.002 / 2.0) * 180.0 / kPi;  // at the 1 m reach below
    ClipData source;
    source.tracks.push_back(track("Root", ClipChannel::Rotation, {key(0, aroundY(0, off)), key(1, aroundY(degrees, off)), key(2, aroundY(0, off))}));
    const ClipData reduced = atRatio(source, rig, 1.0);
    ASSERT_EQ(reduced.tracks.size(), 1u);
    EXPECT_EQ(reduced.tracks[0].keys.size(), 3u) << "the middle key is a real 2 mm move, and it went";
}

TEST(ClipReduction, TheBoundHoldsThroughTheNoiseToo){
    // the same keys through the whole search: not a pretty number, a true one
    anim::ClipRig rig;
    rig.assign("test rig", {node("Root", -1, "Root", Vector3(0, 0, 0)), node("Tip", 0, "Root/Tip", Vector3(0, 0, 0.9f))});
    const double off = 1.0 + 3e-6;
    const double degrees = 2.0 * std::asin(0.002 / 2.0) * 180.0 / kPi;
    ClipData source;
    std::vector<ClipKey> keys;
    // a long straight run (removable) with one real move in it
    for (int i = 0; i < 30; i++) keys.push_back(key((float)i / 10.0f, aroundY(i == 15 ? degrees : 0.0, off)));
    source.tracks.push_back(track("Root", ClipChannel::Rotation, keys));
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(1.0));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.keptAllKeys);
    EXPECT_LT(keyCount(result.clip), keys.size()) << "the straight run goes";
    EXPECT_LE(measured(source, result.clip, rig), 1.0);
}

// ---- D5 row 2: tolerance per track, errors stack ----

TEST(ClipReduction, TheBoundHoldsOnAPlantedCurveSetWhereTracksAloneWouldNot){
    // a long chain of equally noisy joints: per-track errors stack toward its end
    std::vector<anim::ModelNodeInfo> nodes;
    std::string path;
    ClipData source;
    for (int i = 0; i < 12; i++){
        path += (i ? "/" : "") + ("Link" + std::to_string(i));
        nodes.push_back(node("Link" + std::to_string(i), i - 1, path, Vector3(0, i ? 0.1f : 0.0f, 0)));
        source.tracks.push_back(wavingRotation(path, 3.0, 3.0 + i * 0.7, 0.5 + i * 0.17, 0.15));
    }
    anim::ClipRig rig;
    rig.assign("chain", nodes);

    bool stackedOver = false;
    for (const double mm : {0.25, 1.0, 2.0, 5.0}){
        // the per-track value alone, at the full tolerance: the stack this test is about
        const double alone = measured(source, atRatio(source, rig, mm), rig);
        if (alone > mm) stackedOver = true;
        const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(mm));
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_LE(measured(source, result.clip, rig), mm) << "tolerance " << mm;
    }
    EXPECT_TRUE(stackedOver) << "no tolerance overshot per track: this fixture would pass a reducer with no search";
}

TEST(ClipReduction, ASearchThatCannotPassEndsWithEveryKeyAndSaysSo){
    anim::ClipRig rig;
    rig.assign("test rig", {node("Root", -1, "Root", Vector3(0, 0, 0))});
    ClipData source;
    // 6 digits cannot hold this position to 0.01 mm: the file form alone is over the limit
    source.tracks.push_back(track("Root", ClipChannel::Position, {key(0, {1234.5678f, 0, 0}), key(1, {1234.5678f, 0, 0}), key(2, {1235.6789f, 0, 0})}));
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(0.01));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.keptAllKeys);
    EXPECT_EQ(keyCount(result.clip), 3u);
    EXPECT_EQ(result.searchSteps, 8) << "R22: at most 8 steps, and this one needs them all";
}

TEST(ClipReduction, AToleranceThatHoldsAtOnceTakesOneStep){
    const anim::ClipRig rig = bodyRig();
    ClipData source;
    source.tracks.push_back(track("Hips/Arm", ClipChannel::Position, {key(0, {0, 0, 0}), key(0.5f, {0.5f, 0, 0}), key(1, {1, 0, 0})}));
    EXPECT_EQ(anim::reduceClip(source, rig, tolerance(1.0)).searchSteps, 1);
}

// ---- D5 row 3: nlerp ----

TEST(ClipReduction, TheEngineSamplerJudgesTheKeys){
    // Three keys at a constant angular speed over 160 degrees: slerp gives the middle one exactly,
    // nlerp misses it by 5.5 degrees, which at the 100 mm skin is 9.6 mm
    anim::ClipRig rig;
    rig.assign("test rig", {node("Root", -1, "Root", Vector3(0, 0, 0))});
    ClipData source;
    source.tracks.push_back(track("Root", ClipChannel::Rotation, {key(0, aroundY(0)), key(0.25f, aroundY(40)), key(1, aroundY(160))}));
    const ClipData reduced = atRatio(source, rig, 1.0);
    EXPECT_EQ(reduced.tracks[0].keys.size(), 2u) << "the middle key lies on the sampler's own path";
}

TEST(ClipReduction, KeptEndsStayOnOneHemisphere){
    // a turn past 180 degrees: its ends' short way round is the other way, so a key stays whatever the tolerance
    anim::ClipRig rig;
    rig.assign("test rig", {node("Root", -1, "Root", Vector3(0, 0, 0))});
    ClipData source;
    std::vector<ClipKey> keys;
    for (int i = 0; i <= 10; i++) keys.push_back(key((float)i * 0.1f, aroundY(i * 24.0)));
    source.tracks.push_back(track("Root", ClipChannel::Rotation, keys));
    const ClipData reduced = atRatio(source, rig, 100.0);
    const std::vector<ClipKey>& kept = reduced.tracks[0].keys;
    ASSERT_GE(kept.size(), 3u);
    for (size_t k = 1; k < kept.size(); k++){
        const float dot = kept[k - 1].value[1] * kept[k].value[1] + kept[k - 1].value[3] * kept[k].value[3];
        EXPECT_GE(dot, 0.0f) << "the reader would flip key " << k << "'s sign: not byte-identical";
    }
}

// ---- D5 row 4: empty track means identity, trailing keys dropped ----

TEST(ClipReduction, AConstantTrackKeepsOneKeyAndAMovingOneItsEnds){
    const anim::ClipRig rig = bodyRig();
    ClipData source;
    source.tracks.push_back(track("Hips", ClipChannel::Rotation, {key(0, aroundY(30)), key(0.5f, aroundY(30)), key(1, aroundY(30))}));
    // straight line: the interior goes, the ends stay
    source.tracks.push_back(track("Hips/Arm", ClipChannel::Position, {key(0, {0, 0, 0}), key(0.5f, {0.5f, 0, 0}), key(1, {1, 0, 0})}));
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(1.0));
    ASSERT_TRUE(result.ok) << result.error;
    const ClipTrackData* constant = findTrack(result.clip, "Hips", ClipChannel::Rotation);
    const ClipTrackData* moving = findTrack(result.clip, "Hips/Arm", ClipChannel::Position);
    ASSERT_NE(constant, nullptr) << "no track means rest here, not this pose";
    ASSERT_NE(moving, nullptr);
    EXPECT_EQ(constant->keys.size(), 1u);
    ASSERT_EQ(moving->keys.size(), 2u);
    EXPECT_EQ(moving->keys.front().time, 0.0f);
    EXPECT_EQ(moving->keys.back().time, 1.0f);
}

TEST(ClipReduction, ALoopingTrackKeepsItsPeriod){
    const anim::ClipRig rig = bodyRig();
    ClipData source;
    // a track that repeats its 0.5 s inside a 2 s clip, and ends flat: its last key is its period
    ClipTrackData looping = track("Hips", ClipChannel::Position, {key(0, {0, 0, 0}), key(0.25f, {0, 0.1f, 0}), key(0.4f, {0, 0.1f, 0}), key(0.5f, {0, 0.1f, 0})});
    looping.loop = true;
    source.tracks.push_back(looping);
    source.tracks.push_back(track("Hips/Arm", ClipChannel::Rotation, {key(0, aroundY(0)), key(2, aroundY(40))}));
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(1.0));
    ASSERT_TRUE(result.ok) << result.error;
    const ClipTrackData* kept = findTrack(result.clip, "Hips", ClipChannel::Position);
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->keys.back().time, 0.5f) << "the trailing flat key goes, and the track repeats at another period";
    EXPECT_LE(measured(source, result.clip, rig), 1.0);
}

// ---- D5 row 5: lengths from animated T keys ----

TEST(ClipReduction, ABoneWithNoTranslationKeysReachesAsFarAsItsRest){
    // the hip has a 1.2 m leg by rest translations alone (glTF often has no T on bones). A 0.2
    // degree bump moves the foot 4 mm: a metric with no reach reads 0.35 mm at the 100 mm skin
    anim::ClipRig rig;
    rig.assign("test rig", {node("Hip", -1, "Hip", Vector3(0, 0, 0)), node("Leg", 0, "Hip/Leg", Vector3(0, 0, 1.2f))});
    ClipData source;
    source.tracks.push_back(track("Hip", ClipChannel::Rotation, {key(0, aroundY(0)), key(1, aroundY(0.2)), key(2, aroundY(0))}));
    const ClipData reduced = atRatio(source, rig, 1.0);
    EXPECT_EQ(reduced.tracks[0].keys.size(), 3u);
}

// ---- D5 row 6: distance free ----

TEST(ClipReduction, SkinDistanceHasAFloor){
    // a leaf's rotation moves nothing but its skin: at distance 0 anything goes
    anim::ClipRig rig;
    rig.assign("test rig", {node("Leaf", -1, "Leaf", Vector3(0, 0, 0))});
    ClipData source;
    source.tracks.push_back(track("Leaf", ClipChannel::Rotation, {key(0, aroundY(0)), key(1, aroundY(60)), key(2, aroundY(0))}));
    anim::ClipReductionOptions zero = tolerance(1.0);
    zero.skinDistanceMm = 0.0;
    const anim::ClipReductionResult atZero = anim::reduceClip(source, rig, zero);
    anim::ClipReductionOptions floor = tolerance(1.0);
    floor.skinDistanceMm = anim::kClipReductionSkinFloorMm;
    const anim::ClipReductionResult atFloor = anim::reduceClip(source, rig, floor);
    ASSERT_TRUE(atZero.ok && atFloor.ok);
    EXPECT_EQ(anim::writeClip(atZero.clip), anim::writeClip(atFloor.clip));
    EXPECT_EQ(keyCount(atZero.clip), 3u) << "60 degrees at 10 mm is 10 mm";
}

// ---- D5 row 7: metres assumed ----

TEST(ClipReduction, ATinyRigGetsATinyLimit){
    // 10 mm tall: 1 mm would be a tenth of its body
    anim::ClipRig rig;
    rig.assign("tiny", {node("Root", -1, "Root", Vector3(0, 0, 0)), node("Top", 0, "Root/Top", Vector3(0, 0.01f, 0))});
    ClipData source;
    source.tracks.push_back(wavingRotation("Root", 2.0, 20.0, 1.0, 0.5));
    anim::ClipReductionOptions options = tolerance(1.0);
    options.skinDistanceMm = 10.0;
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, options);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_NEAR(result.effectiveToleranceMm, 0.01, 1e-6);
    EXPECT_LE(measured(source, result.clip, rig, 10.0), 0.01 + 1e-6);

    const anim::ClipRig body = bodyRig();
    EXPECT_NEAR(anim::reduceClip(walkClip(), body, tolerance(0.5)).effectiveToleranceMm, 0.5, 1e-9) << "a body's 0.1 % is over the tolerance";
}

// ---- D5 row 8: all keys linear ----

TEST(ClipReduction, TracksItCannotJudgeStayByteIdenticalAndAreCounted){
    const anim::ClipRig rig = bodyRig();
    ClipData source = walkClip();

    ClipKey in0 = key(0, {0, 0, 0}), in1 = key(1, {0, 0, 0}), in2 = key(2, {0, 0, 0});
    in0.inTangent = {0, 0, 0}; in0.outTangent = {0, 0, 0};
    in1.inTangent = {0, 0, 0}; in1.outTangent = {0, 0, 0};
    in2.inTangent = {0, 0, 0}; in2.outTangent = {0, 0, 0};
    ClipTrackData hermite = track("Hips/Arm", ClipChannel::Position, {in0, in1, in2});
    source.tracks.push_back(hermite);

    ClipKey eased0 = key(0, {0, 0, 0}), eased1 = key(1, {0, 0, 0});
    eased0.ease = EaseType::QUAD_OUT;
    source.tracks.push_back(track("Hips/LeftLeg", ClipChannel::Position, {eased0, eased1, key(2, {0, 0, 0})}));

    ClipKey step0 = key(0, {1, 1, 1}), step1 = key(1, {1, 1, 1});
    step0.ease = EaseType::STEP;
    step1.ease = EaseType::STEP;
    source.tracks.push_back(track("Hips/Arm", ClipChannel::Scale, {step0, step1, key(2, {1, 1, 1})}));

    source.tracks.push_back(track("Hips", ClipChannel::Morph, {key(0, {0.5f}), key(1, {0.5f}), key(2, {0.5f})}));

    ClipTrackData lattice = track("Hips", ClipChannel::Lattice, {key(0, {0, 0, 0}), key(1, {0, 0, 0}), key(2, {0, 0, 0})});
    lattice.points = {0};
    source.tracks.push_back(lattice);

    ClipTrackData property = track("Hips", ClipChannel::Property, {key(0, {1}), key(1, {1}), key(2, {1})});
    property.component = "Transform";
    property.property = "visible";
    source.tracks.push_back(property);

    ClipTrackData humanoid = track("", ClipChannel::Humanoid, {key(0, {0, 0, 0, 1}), key(1, {0, 0, 0, 1}), key(2, {0, 0, 0, 1})});
    humanoid.humanBone = 0;
    source.tracks.push_back(humanoid);

    source.tracks.push_back(track("NotOnTheRig", ClipChannel::Rotation, {key(0, aroundY(0)), key(1, aroundY(0)), key(2, aroundY(0))}));

    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(1.0));
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_FALSE(result.keptAllKeys);
    EXPECT_LT(keyCount(result.clip), keyCount(source));
    ClipData normalized = source;
    anim::normalize(normalized);
    const std::shared_ptr<const AnimationClip> built = AnimationClip::build(normalized);

    anim::ClipSkippedTracks skipped;
    size_t skippedSeen = 0;
    ASSERT_EQ(normalized.tracks.size(), result.clip.tracks.size());
    for (size_t t = 0; t < normalized.tracks.size(); t++){
        if (anim::clipTrackIsReducible(built->getTracks()[t], rig, &skipped)) continue;
        skippedSeen++;
        ClipData one, two;
        one.tracks = {normalized.tracks[t]};
        two.tracks = {result.clip.tracks[t]};
        EXPECT_EQ(anim::writeClip(one), anim::writeClip(two)) << normalized.tracks[t].path;
    }
    EXPECT_EQ(skipped.tangents, 1u);
    EXPECT_EQ(skipped.eases, 1u);
    EXPECT_EQ(skipped.step, 1u);
    EXPECT_EQ(skipped.morph, 1u);
    EXPECT_EQ(skipped.lattice, 1u);
    EXPECT_EQ(skipped.property, 1u);
    EXPECT_EQ(skipped.humanoid, 1u);
    EXPECT_EQ(skipped.unbound, 1u);
    EXPECT_EQ(skipped.total(), skippedSeen);
}

// ---- D5 row 9: NaN passes ----

TEST(ClipReduction, ANonFiniteKeyIsANamedError){
    const anim::ClipRig rig = bodyRig();
    ClipData source;
    source.tracks.push_back(track("Hips", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {std::numeric_limits<float>::quiet_NaN(), 0, 0}), key(2, {1, 0, 0})}));
    anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(1.0));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorKind, "non_finite_key");
    EXPECT_NE(result.error.find("Hips"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("key 1"), std::string::npos) << result.error;

    source.tracks[0].keys[1].value[0] = std::numeric_limits<float>::infinity();
    result = anim::reduceClip(source, rig, tolerance(1.0));
    EXPECT_EQ(result.errorKind, "non_finite_key");
}

// ---- R23, AC10: feet ----

TEST(ClipReduction, AFootGetsHalfTheToleranceAndAJointOverrideWins){
    const anim::ClipRig rig = bodyRig();
    ASSERT_TRUE(rig.feet[(size_t)rig.findBone("Hips/LeftLeg/LeftFoot")]);
    ASSERT_FALSE(rig.feet[(size_t)rig.findBone("Hips/Arm/Hand")]);

    std::vector<double> limits;
    std::string unknown;
    const double noCap = std::numeric_limits<double>::max();
    anim::ClipReductionOptions options = tolerance(2.0);
    ASSERT_TRUE(anim::clipJointLimits(rig, options, noCap, limits, &unknown));
    EXPECT_DOUBLE_EQ(limits[(size_t)rig.findBone("Hips/LeftLeg/LeftFoot")], 1.0);
    EXPECT_DOUBLE_EQ(limits[(size_t)rig.findBone("Hips/Arm/Hand")], 2.0);

    options.joints = {{"Hips/LeftLeg/LeftFoot", 3.0}};
    ASSERT_TRUE(anim::clipJointLimits(rig, options, noCap, limits, &unknown));
    EXPECT_DOUBLE_EQ(limits[(size_t)rig.findBone("Hips/LeftLeg/LeftFoot")], 3.0) << "an override wins over the foot rule";

    options.joints = {{"Hand*", 0.5}};
    ASSERT_TRUE(anim::clipJointLimits(rig, options, noCap, limits, &unknown)) << "a name with a trailing *";
    EXPECT_DOUBLE_EQ(limits[(size_t)rig.findBone("Hips/Arm/Hand")], 0.5);

    options.joints = {{"NoSuchBone", 0.5}};
    EXPECT_FALSE(anim::clipJointLimits(rig, options, noCap, limits, &unknown));
    EXPECT_EQ(unknown, "NoSuchBone");
    EXPECT_EQ(anim::reduceClip(walkClip(), rig, options).errorKind, "unknown_joint");
}

TEST(ClipReduction, APlantedFootIsMeasuredWithinItsOwnHalf){
    const anim::ClipRig rig = bodyRig();
    // Arm + Hand and LeftLeg + LeftFoot carry the same curves: only the name differs
    const ClipData source = walkClip();
    const int foot = rig.findBone("Hips/LeftLeg/LeftFoot");
    const double tol = 4.0;
    const anim::ClipReductionResult result = anim::reduceClip(source, rig, tolerance(tol));
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_FALSE(result.keptAllKeys);
    ClipData normalized = source;
    anim::normalize(normalized);
    const anim::ClipErrorReport report = anim::measureClipError(*AnimationClip::build(normalized), *throughFile(result.clip), rig, 100.0);
    EXPECT_LE(report.jointMaxMm[(size_t)foot], tol * 0.5) << "half the clip's tolerance, measured";
    EXPECT_LE(report.maxErrorMm, tol);
    const size_t footKeys = findTrack(result.clip, "Hips/LeftLeg/LeftFoot", ClipChannel::Rotation)->keys.size();
    const size_t handKeys = findTrack(result.clip, "Hips/Arm/Hand", ClipChannel::Rotation)->keys.size();
    EXPECT_GT(footKeys, handKeys) << "tightening shows in the keys the foot keeps";

    // the override gives it the hand's budget back
    anim::ClipReductionOptions same = tolerance(tol);
    same.joints = {{"Hips/LeftLeg/LeftFoot", tol}};
    const anim::ClipReductionResult overridden = anim::reduceClip(source, rig, same);
    ASSERT_TRUE(overridden.ok) << overridden.error;
    EXPECT_LT(findTrack(overridden.clip, "Hips/LeftLeg/LeftFoot", ClipChannel::Rotation)->keys.size(), footKeys);
}

TEST(ClipReduction, TighteningAJointTightensItsAncestorsOnly){
    // the up-chain min: a leg moves its foot, a sibling arm does not
    const anim::ClipRig rig = bodyRig();
    ClipData source = walkClip();
    anim::normalize(source);
    const std::shared_ptr<const AnimationClip> built = AnimationClip::build(source);
    std::vector<double> loose = flatLimits(rig, 4.0);
    std::vector<double> tight = loose;
    tight[(size_t)rig.findBone("Hips/LeftLeg/LeftFoot")] = 0.2;
    const ClipData a = anim::reduceClipAtRatio(source, *built, rig, loose, 100.0, 1.0);
    const ClipData b = anim::reduceClipAtRatio(source, *built, rig, tight, 100.0, 1.0);
    auto keysOf = [](const ClipData& data, const char* path){ return findTrack(data, path, ClipChannel::Rotation)->keys.size(); };
    EXPECT_GT(keysOf(b, "Hips/LeftLeg/LeftFoot"), keysOf(a, "Hips/LeftLeg/LeftFoot"));
    EXPECT_GT(keysOf(b, "Hips/LeftLeg"), keysOf(a, "Hips/LeftLeg")) << "its parent, up the chain";
    EXPECT_GT(keysOf(b, "Hips"), keysOf(a, "Hips")) << "and the root";
    EXPECT_EQ(keysOf(b, "Hips/Arm"), keysOf(a, "Hips/Arm")) << "a sibling chain is untouched";
    EXPECT_EQ(keysOf(b, "Hips/Arm/Hand"), keysOf(a, "Hips/Arm/Hand"));
}

// ---- R24: the output ----

TEST(ClipReduction, TheOutputRecordsItsRecipeAndParsesWithoutAWarning){
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "je_clip_reduction_test";
    std::filesystem::create_directories(directory);
    const std::filesystem::path file = directory / "walk.anim";
    ClipData source = walkClip();
    {
        std::ofstream out(file, std::ios::binary);
        out << anim::writeClip(source);
    }
    const anim::ClipRig rig = bodyRig();
    anim::ClipOptimizeRequest request;
    request.reference = file.generic_string();
    request.displayName = "assets/anim/walk.anim";
    request.rig = &rig;
    const anim::ClipOptimizeResult result = anim::runClipOptimize(request);
    ASSERT_TRUE(result.ok) << result.errorKind << ": " << result.error;
    EXPECT_LT(result.after.keys, result.before.keys);
    EXPECT_LE(result.measured.maxErrorMm, 1.0);

    const anim::ClipParseResult parsed = anim::parseClip(anim::writeClip(result.reduced));
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(parsed.warnings.empty()) << parsed.warnings.front().message;
    const anim::ClipOptimizeRecord record = anim::readClipOptimizeRecord(*parsed.data);
    ASSERT_TRUE(record.present);
    EXPECT_EQ(record.toleranceMm, 1.0);
    EXPECT_EQ(record.skinDistanceMm, 100.0);
    EXPECT_EQ(record.source, "assets/anim/walk.anim");
    EXPECT_EQ(record.sourceSha1, anim::clipSourceSha1(file.generic_string()));
    EXPECT_FALSE(record.sourceSha1.empty());
    EXPECT_LE(record.maxErrorMm, 1.0);

    // nothing was written
    std::string after;
    {
        std::ifstream in(file, std::ios::binary);
        after.assign(std::istreambuf_iterator<char>(in), {});
    }
    EXPECT_EQ(after, anim::writeClip(source));
    std::filesystem::remove_all(directory);
}

TEST(ClipReduction, AfterAWriteTheErrorIsReadFromTheFile){
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "je_clip_reduction_verify";
    std::filesystem::create_directories(directory);
    const std::filesystem::path source = directory / "walk.anim";
    const std::filesystem::path output = directory / "walk.opt.anim";
    {
        std::ofstream out(source, std::ios::binary);
        out << anim::writeClip(walkClip());
    }
    const anim::ClipRig rig = bodyRig();
    anim::ClipOptimizeRequest request;
    request.reference = source.generic_string();
    request.rig = &rig;
    anim::ClipOptimizeResult result = anim::runClipOptimize(request);
    ASSERT_TRUE(result.ok);
    {
        std::ofstream out(output, std::ios::binary);
        out << anim::writeClip(result.reduced);
    }
    std::string error;
    ASSERT_TRUE(anim::verifyWrittenClip(output, rig, result, error)) << error;
    const double fromDisk = result.measured.maxErrorMm;
    EXPECT_LE(fromDisk, 1.0);

    // somebody changes the file: the number changes, it was not remembered
    ClipData changed = result.reduced;
    for (ClipKey& k : changed.tracks[0].keys) k.value[1] += 0.01f;
    {
        std::ofstream out(output, std::ios::binary);
        out << anim::writeClip(changed);
    }
    ASSERT_TRUE(anim::verifyWrittenClip(output, rig, result, error)) << error;
    EXPECT_GT(result.measured.maxErrorMm, fromDisk + 5.0);

    {
        std::ofstream out(output, std::ios::binary);
        out << "not a clip";
    }
    EXPECT_FALSE(anim::verifyWrittenClip(output, rig, result, error));
    EXPECT_FALSE(error.empty());
    std::filesystem::remove_all(directory);
}

TEST(ClipReduction, ClipExtensionKeyDoesNotWarn){
    EXPECT_TRUE(anim::parseClip("{\"format\": \"jasperengine.anim\", \"version\": 1, \"optimize\": {\"tolerance_mm\": 1}, \"tracks\": [], \"events\": []}").warnings.empty());
}
