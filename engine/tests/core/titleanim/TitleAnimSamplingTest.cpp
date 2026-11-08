// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85 (docs/specs/S85-title-animation.md) AC2-AC5: the title sampler against the clip sampler,
// colours, stepped keys, relative combination, drives, reseeds, the wrap with a loop section and
// the stagger orders. Always built (engine/core/titleanim has no module dependency).

#include <gtest/gtest.h>

#include "animation/AnimationClip.h"
#include "animation/KeyframeSampling.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimSampling.h"

#include <algorithm>
#include <random>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleanim;

namespace {

    ClipKey key(float time, std::vector<float> value, EaseType ease = EaseType::LINEAR){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        k.ease = ease;
        return k;
    }

    CompiledTitleTrack numericTrack(const std::vector<ClipKey>& keys, size_t width){
        CompiledTitleTrack t;
        compileNumericKeys(keys, width, t);
        t.rest.assign(width, 0.0f);
        return t;
    }

    TitleTrack titleTrack(const std::string& field, std::vector<std::pair<float, std::vector<float>>> keys, bool relative = false){
        TitleTrack t;
        t.field = field;
        t.relative = relative;
        for (auto& [time, value] : keys){
            TitleKey k;
            k.key.time = time;
            k.key.value = value;
            t.keys.push_back(k);
        }
        return t;
    }

    CompiledTitleAnimation compile(const TitleAnimation& a){
        CompiledTitleAnimation c;
        compileTitleOnly(a, c, nullptr);
        return c;
    }

}

TEST(TitleAnimSamplingTest, MatchesClipSampler){
    // 200 random tracks (eases, tangents, steps) sample bit-identically to the same keys in a clip
    std::mt19937 random(85);
    std::uniform_real_distribution<float> value(-50.0f, 50.0f);
    std::uniform_real_distribution<float> gap(0.01f, 0.5f);
    std::uniform_int_distribution<int> ease(0, (int)EaseType::STEP);
    for (int trackIndex = 0; trackIndex < 200; trackIndex++){
        const size_t width = 1 + (size_t)(trackIndex % 3);
        const size_t count = 2 + (size_t)(random() % 9);
        const bool tangents = trackIndex % 4 == 1;
        std::vector<ClipKey> keys;
        float time = (trackIndex % 5) * 0.1f;
        for (size_t k = 0; k < count; k++){
            std::vector<float> v(width);
            for (float& x : v) x = value(random);
            ClipKey ck = key(time, v, tangents ? EaseType::LINEAR : (EaseType)ease(random));
            if (tangents){
                ck.inTangent.resize(width);
                ck.outTangent.resize(width);
                for (size_t c = 0; c < width; c++){
                    ck.inTangent[c] = value(random);
                    ck.outTangent[c] = value(random);
                }
            }
            keys.push_back(ck);
            time += gap(random);
        }
        CompiledTitleTrack titleTrackCompiled = numericTrack(keys, width);

        ClipData data;
        ClipTrackData clipTrack;
        clipTrack.channel = ClipChannel::Morph;
        clipTrack.path = "";
        clipTrack.keys = keys;
        data.tracks = {clipTrack};
        data.length = time + 1.0f;
        std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
        ASSERT_TRUE(clip);
        ASSERT_EQ(clip->getTracks().size(), 1u);
        const AnimationClip::Track& track = clip->getTracks()[0];

        for (int s = 0; s <= 97; s++){
            const float t = -0.2f + (time + 0.4f) * (float)s / 97.0f;
            anim::TrackSample expected;
            ASSERT_TRUE(anim::sampleTrack(track, t, expected));
            double got[3];
            sampleTrackValue(titleTrackCompiled, t, 0.0, 1.0, got);
            for (size_t c = 0; c < width; c++)
                ASSERT_EQ((float)got[c], expected.weights[c]) << "track " << trackIndex << " at " << t << " component " << c;
        }
    }
}

TEST(TitleAnimSamplingTest, ColorsSrgbMidpoint){
    // "#000000" -> "#ffffff": the midpoint is 0.5 in sRGB-encoded channels (S80's gradient rule)
    TitleKey a, b;
    std::string error;
    ASSERT_TRUE(decodeKey(AnimJson::array({0, "#000000"}), a, error));
    ASSERT_TRUE(decodeKey(AnimJson::array({1, "#ffffff"}), b, error));
    ASSERT_EQ(a.key.value.size(), 3u);
    CompiledTitleTrack t = numericTrack({a.key, b.key}, 3);
    double v[3];
    sampleTrackValue(t, 0.5, 0.0, 1.0, v);
    for (double c : v) EXPECT_NEAR(c, 0.5, 1e-6);
    EXPECT_EQ(formatHexColor(std::vector<float>{0.5f, 0.5f, 0.5f}.data()), "#808080");
    // a colour key keeps its text
    EXPECT_EQ(encodeKey(b), AnimJson::array({1, "#ffffff"}));
}

TEST(TitleAnimSamplingTest, SteppedSlack){
    std::vector<TitleKey> keys(3);
    keys[0].key.time = 0.0f; keys[0].stepped = "\"A\"";
    keys[1].key.time = 1.0f; keys[1].stepped = "\"B\"";
    keys[2].key.time = 2.0f; keys[2].stepped = "\"C\"";
    CompiledTitleTrack t;
    compileSteppedKeys(keys, t);
    double v = -1.0;
    sampleTrackValue(t, 0.999, 0.0, 1.0, &v);
    EXPECT_EQ(v, 0.0);
    // KeyframeSampling's step slack: the next key shows from 1e-6 s before its time
    sampleTrackValue(t, 1.0 - 5e-7, 0.0, 1.0, &v);
    EXPECT_EQ(v, 1.0);
    sampleTrackValue(t, 1.5, 0.0, 1.0, &v);
    EXPECT_EQ(v, 1.0);
    sampleTrackValue(t, 7.0, 0.0, 1.0, &v);
    EXPECT_EQ(v, 2.0);
    EXPECT_EQ(t.stepValues[(size_t)v], "\"C\"");
}

TEST(TitleAnimSamplingTest, RelativeCombine){
    const float rest[2] = {10.0f, 4.0f};
    const double sample[2] = {2.0, 0.5};
    double out[2];
    combineWithRest(TrackCombine::Add, sample, rest, 2, out);
    EXPECT_DOUBLE_EQ(out[0], 12.0);
    EXPECT_DOUBLE_EQ(out[1], 4.5);
    combineWithRest(TrackCombine::Multiply, sample, rest, 2, out);
    EXPECT_DOUBLE_EQ(out[0], 20.0);
    EXPECT_DOUBLE_EQ(out[1], 2.0);
    combineWithRest(TrackCombine::Replace, sample, rest, 2, out);
    EXPECT_DOUBLE_EQ(out[0], 2.0);
    // a whole layer field track (the module's): a relative opacity multiplies, then clamps
    CompiledTitleTrack opacity = numericTrack({key(0, {2.0f})}, 1);
    opacity.combine = TrackCombine::Multiply;
    opacity.rest = {0.8f};
    opacity.min = 0.0;
    opacity.max = 1.0;
    CompiledTitleAnimation a;
    a.duration = 1.0;
    a.tracks = {opacity};
    a.layoutPose();
    TitleAnimPose pose;
    pose.size(a);
    sampleAnimation(a, 0.5, pose);
    EXPECT_DOUBLE_EQ(pose.values[0], 1.0) << "0.8 x 2 clamped to the field's range";
}

TEST(TitleAnimSamplingTest, TitleFieldsIdentityRest){
    // relative title tracks combine with identity: a summary with no tracks is the still
    TitlePoseSummary s;
    EXPECT_FALSE(s.any());
    EXPECT_EQ(s.scale, 1.0);
    EXPECT_EQ(s.opacity, 1.0);
    EXPECT_EQ(s.flashAmount, 0.0);
    TitleAnimation anim;
    anim.name = "Pulse";
    anim.duration = 1.0f;
    anim.tracks.push_back(titleTrack("transform.scale", {{0.0f, {2.0f}}}, true));
    anim.tracks.push_back(titleTrack("transform.position", {{0.0f, {5.0f, -3.0f}}}, true));
    anim.tracks.push_back(titleTrack("opacity", {{0.0f, {0.5f}}}, false));
    anim.tracks.push_back(titleTrack("flash.amount", {{0.0f, {0.25f}}}, true));
    CompiledTitleAnimation c = compile(anim);
    ASSERT_EQ(c.tracks.size(), 4u);
    TitleAnimPose pose;
    pose.size(c);
    sampleAnimation(c, 0.0, pose);
    accumulateTitlePose(c, pose, false, nullptr, s);
    EXPECT_DOUBLE_EQ(s.scale, 2.0);
    EXPECT_DOUBLE_EQ(s.position[0], 5.0);
    EXPECT_DOUBLE_EQ(s.position[1], -3.0);
    EXPECT_DOUBLE_EQ(s.opacity, 0.5);
    EXPECT_DOUBLE_EQ(s.flashAmount, 0.25);
    // an overlay on top: relative whatever its flag (R25)
    accumulateTitlePose(c, pose, true, nullptr, s);
    EXPECT_DOUBLE_EQ(s.scale, 4.0);
    EXPECT_DOUBLE_EQ(s.position[0], 10.0);
    EXPECT_DOUBLE_EQ(s.opacity, 0.25);
    // a live field's relative track combines with the instance's value
    TitleAnimation live;
    live.name = "Sweep";
    live.tracks.push_back(titleTrack("live.sweep_angle", {{0.0f, {10.0f}}}, true));
    CompiledTitleAnimation lc = compile(live);
    TitleAnimPose lp;
    lp.size(lc);
    sampleAnimation(lc, 0.0, lp);
    double rest[(size_t)TitleField::Count] = {};
    rest[(size_t)TitleField::SweepAngle] = 30.0;
    TitlePoseSummary ls;
    accumulateTitlePose(lc, lp, false, rest, ls);
    EXPECT_TRUE(ls.liveSet[(size_t)TitleField::SweepAngle]);
    EXPECT_DOUBLE_EQ(ls.live[(size_t)TitleField::SweepAngle], 40.0);
}

TEST(TitleAnimSamplingTest, DriveCyclesSeamless){
    for (bool wrap : {false, true}){
        CompiledTitleTrack d;
        d.source = TrackSource::Drive;
        d.width = 1;
        d.driveFrom = {0.0f};
        d.driveTo = {360.0f};
        d.cycles = 2.0f;
        d.driveWrap = wrap;
        d.driveEase = EaseType::SINE_IN_OUT;
        const double spanStart = 0.4, span = 1.6;
        double atStart, atEnd, mid;
        sampleTrackValue(d, spanStart, spanStart, span, &atStart);
        sampleTrackValue(d, spanStart + span, spanStart, span, &atEnd);
        EXPECT_NEAR(atEnd, atStart, 1e-6) << "the span's end equals its start";
        sampleTrackValue(d, spanStart + span / 4.0, spanStart, span, &mid);
        // half a cycle in: wrap is half way (eased), the bounce at the top
        EXPECT_NEAR(mid, wrap ? 180.0 : 360.0, 1e-3);
        if (!wrap){
            // no jump anywhere: neighbouring samples stay close
            double prev;
            sampleTrackValue(d, spanStart, spanStart, span, &prev);
            for (int i = 1; i <= 400; i++){
                double v;
                sampleTrackValue(d, spanStart + span * i / 400.0, spanStart, span, &v);
                EXPECT_LT(std::fabs(v - prev), 15.0);
                prev = v;
            }
        }
    }
}

TEST(TitleAnimSamplingTest, DriveRateWraps){
    CompiledTitleTrack d;
    d.source = TrackSource::Drive;
    d.width = 1;
    d.driveFrom = {0.0f};
    d.driveTo = {1.0f};
    d.rate = 0.75f;
    double v;
    sampleTrackValue(d, 2.0, 0.0, 1.0, &v);
    EXPECT_NEAR(v, 1.5, 1e-9) << "without wrap the rate runs on";
    d.driveWrap = true;
    sampleTrackValue(d, 2.0, 0.0, 1.0, &v);
    EXPECT_NEAR(v, 0.5, 1e-9);
    sampleTrackValue(d, 4.0 / 3.0, 0.0, 1.0, &v);
    EXPECT_GE(v, 0.0);
    EXPECT_LT(v, 1.0);
}

TEST(TitleAnimSamplingTest, ReseedSteps){
    CompiledTitleTrack r;
    r.source = TrackSource::Reseed;
    r.width = 1;
    r.reseedRate = 12.0f;
    r.combine = TrackCombine::Add;
    r.rest = {7.0f};
    r.min = 0.0;
    r.max = 2147483647.0;
    CompiledTitleAnimation a;
    a.duration = 2.0;
    a.tracks = {r};
    a.layoutPose();
    TitleAnimPose pose;
    pose.size(a);
    for (int k = 0; k < 24; k++){
        const double at = (double)k / 12.0;
        sampleAnimation(a, at + 1e-6, pose);
        EXPECT_EQ(pose.values[0], 7.0 + k) << "the seed at k/rate";
        sampleAnimation(a, at + 1.0 / 12.0 - 1e-6, pose);
        EXPECT_EQ(pose.values[0], 7.0 + k) << "just before the next step";
    }
    // one time, one seed
    sampleAnimation(a, 0.5, pose);
    const double first = pose.values[0];
    sampleAnimation(a, 0.5, pose);
    EXPECT_EQ(pose.values[0], first);
}

TEST(TitleAnimSamplingTest, WrapLoopSection){
    // once clamps; the first pass always plays 0..D
    EXPECT_DOUBLE_EQ(wrapAnimationTime(0.5, 2.0, 0.5, AnimWrap::Once), 0.5);
    EXPECT_DOUBLE_EQ(wrapAnimationTime(3.0, 2.0, 0.5, AnimWrap::Once), 2.0);
    EXPECT_DOUBLE_EQ(wrapAnimationTime(-1.0, 2.0, 0.5, AnimWrap::Once), 0.0);
    EXPECT_DOUBLE_EQ(wrapAnimationTime(0.25, 2.0, 0.5, AnimWrap::Loop), 0.25);
    EXPECT_DOUBLE_EQ(wrapAnimationTime(2.0, 2.0, 0.5, AnimWrap::Loop), 2.0);
    // loop: loop_start + fmod(t - loop_start, D - loop_start)
    EXPECT_NEAR(wrapAnimationTime(2.25, 2.0, 0.5, AnimWrap::Loop), 0.75, 1e-12);
    EXPECT_NEAR(wrapAnimationTime(3.5, 2.0, 0.5, AnimWrap::Loop), 0.5, 1e-12);
    EXPECT_NEAR(wrapAnimationTime(3.75, 2.0, 0.5, AnimWrap::Loop), 0.75, 1e-12);
    // ping_pong over [loop_start, D]
    EXPECT_NEAR(wrapAnimationTime(2.25, 2.0, 0.5, AnimWrap::PingPong), 1.75, 1e-12);
    EXPECT_NEAR(wrapAnimationTime(3.5, 2.0, 0.5, AnimWrap::PingPong), 0.5, 1e-12);
    EXPECT_NEAR(wrapAnimationTime(4.0, 2.0, 0.5, AnimWrap::PingPong), 1.0, 1e-12);
    EXPECT_NEAR(wrapAnimationTime(5.0, 2.0, 0.5, AnimWrap::PingPong), 2.0, 1e-12);
    EXPECT_NEAR(wrapAnimationTime(5.25, 2.0, 0.5, AnimWrap::PingPong), 1.75, 1e-12);
    // never NaN
    EXPECT_EQ(wrapAnimationTime(NAN, 2.0, 0.5, AnimWrap::Loop), 0.0);
}

TEST(TitleAnimStaggerTest, Orders){
    std::vector<uint32_t> r;
    staggerRanks(StaggerOrder::Forward, 7, 1, 1, r);
    EXPECT_EQ(r, (std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6}));
    staggerRanks(StaggerOrder::Reverse, 8, 1, 1, r);
    EXPECT_EQ(r, (std::vector<uint32_t>{7, 6, 5, 4, 3, 2, 1, 0}));
    staggerRanks(StaggerOrder::CenterOut, 7, 1, 1, r);
    EXPECT_EQ(r, (std::vector<uint32_t>{3, 2, 1, 0, 1, 2, 3}));
    staggerRanks(StaggerOrder::CenterOut, 8, 1, 1, r);
    EXPECT_EQ(r, (std::vector<uint32_t>{3, 2, 1, 0, 0, 1, 2, 3}));
    staggerRanks(StaggerOrder::EdgesIn, 7, 1, 1, r);
    EXPECT_EQ(r, (std::vector<uint32_t>{0, 1, 2, 3, 2, 1, 0}));
    staggerRanks(StaggerOrder::EdgesIn, 8, 1, 1, r);
    EXPECT_EQ(r, (std::vector<uint32_t>{0, 1, 2, 3, 3, 2, 1, 0}));
    // random: a permutation, stable per seed, different for another seed
    for (size_t n : {7u, 8u}){
        std::vector<uint32_t> a, b, c;
        staggerRanks(StaggerOrder::Random, n, 42, 3, a);
        staggerRanks(StaggerOrder::Random, n, 42, 3, b);
        staggerRanks(StaggerOrder::Random, n, 42, 4, c);
        EXPECT_EQ(a, b);
        EXPECT_NE(a, c);
        std::vector<uint32_t> sorted = a;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < n; i++) EXPECT_EQ(sorted[i], i);
    }
}

TEST(TitleAnimStaggerTest, OverWithEase){
    TitleStagger s;
    s.over = 1.0f;
    s.ease = EaseType::QUAD_IN;
    std::vector<float> d;
    staggerDelays(s, 5, 1, d);
    ASSERT_EQ(d.size(), 5u);
    EXPECT_FLOAT_EQ(d[0], 0.0f);
    EXPECT_FLOAT_EQ(d[4], 1.0f);
    EXPECT_NEAR(d[2], 0.25f, 1e-6) << "over x ease(o / (n - 1))";
    staggerDelays(s, 1, 1, d);
    EXPECT_EQ(d, std::vector<float>{0.0f}) << "a single unit has delay 0";
    TitleStagger each;
    each.each = 0.04f;
    staggerDelays(each, 4, 1, d);
    EXPECT_NEAR(d[3], 0.12f, 1e-6);
}
