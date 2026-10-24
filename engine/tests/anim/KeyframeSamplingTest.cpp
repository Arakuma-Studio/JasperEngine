// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// KeyframeSampling (spec 3.1, 3.3): locateKey reproduces ActionSystem's former keyframeUpdate
// exactly (compared with a copy of it), every ease family against its closed form, every
// channel of a clip against hand-computed values (linear, step, eased, Hermite, slerp and the
// zero-norm guard, morph, lattice, looping tracks, the ends), cursors never change results, and
// the entity tracks ActionSystem samples through the same functions (relative mapping included).

#include <gtest/gtest.h>

#include "Scene.h"
#include "action/keyframe/RotateTracks.h"
#include "action/keyframe/TranslateTracks.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/KeyframeSampling.h"
#include "component/Transform.h"
#include "subsystem/ActionSystem.h"

#include <cmath>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    // ActionSystem::keyframeUpdate as it was before the refactor (the reference)
    void referenceKeyframeUpdate(const std::vector<float>& times, const std::vector<EaseType>& easings, float currentTime,
                                 int& index, float& interpolation){
        interpolation = 0;
        index = 0;
        while (index < (int)(times.size() - 1) && times[index] < currentTime){
            index++;
        }
        float previousTime = 0;
        float nextTime = times[index];
        if (index > 0){
            previousTime = times[index - 1];
        }
        if (nextTime > previousTime){
            interpolation = (currentTime - previousTime) / (nextTime - previousTime);
        }
        if (interpolation > 1){
            interpolation = 1;
        }
        if (index > 0){
            size_t segment = (size_t)index - 1;
            if (segment < easings.size() && easings[segment] != EaseType::LINEAR){
                interpolation = Ease::getFunction(easings[segment])(interpolation);
            }
        }
    }

    ClipKey key(float time, std::vector<float> value, EaseType ease = EaseType::LINEAR){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        k.ease = ease;
        return k;
    }

    ClipTrackData track(ClipChannel channel, std::vector<ClipKey> keys, const std::string& path = ""){
        ClipTrackData t;
        t.path = path;
        t.channel = channel;
        t.keys = std::move(keys);
        return t;
    }

    anim::TrackSample sampleOne(const ClipTrackData& data, float time){
        ClipData clip;
        clip.tracks = {data};
        std::shared_ptr<const AnimationClip> built = AnimationClip::build(clip);
        anim::TrackSample sample;
        EXPECT_TRUE(anim::sampleTrack(built->getTracks()[0], time, sample));
        return sample;
    }

    void expectVector(const Vector3& actual, float x, float y, float z, float tolerance = 1e-5f){
        EXPECT_NEAR(actual.x, x, tolerance);
        EXPECT_NEAR(actual.y, y, tolerance);
        EXPECT_NEAR(actual.z, z, tolerance);
    }

    void expectRotation(const Quaternion& actual, const Quaternion& expected, float tolerance = 1e-5f){
        EXPECT_NEAR(std::fabs(actual.dot(expected)), 1.0f, tolerance)
            << actual.w << " " << actual.x << " " << actual.y << " " << actual.z;
    }

    void step(Scene& scene, double dt, int count = 1){
        for (int i = 0; i < count; i++){
            scene.getSystem<ActionSystem>()->update(dt);
        }
    }

}

TEST(KeyframeSampling, LocateKeyMatchesTheFormerKeyframeUpdate){
    std::mt19937 random(99);
    std::uniform_real_distribution<float> time(-1.0f, 6.0f);
    for (int round = 0; round < 2000; round++){
        size_t count = 1 + random() % 6;
        std::vector<float> times;
        for (size_t i = 0; i < count; i++) times.push_back(round % 7 == 0 ? time(random) : (float)i * 0.75f + 0.1f * (round % 3));
        if (round % 7 != 0 && round % 2 == 0) times[0] = 0.0f;
        std::vector<EaseType> easings;
        size_t easingCount = random() % (count + 1);
        for (size_t i = 0; i < easingCount; i++) easings.push_back((EaseType)(random() % (int)EaseType::CUSTOM));
        float at = time(random);

        int index = 0;
        float interpolation = 0;
        referenceKeyframeUpdate(times, easings, at, index, interpolation);
        anim::KeyLocation location = anim::locateKey(times.data(), times.size(), easings.data(), easings.size(), at);
        ASSERT_EQ((int)location.index, index) << "round " << round;
        ASSERT_EQ(location.interpolation, interpolation) << "round " << round << ": bit-identical";
        EXPECT_EQ(location.segmentDuration, index > 0 ? times[index] - times[index - 1] : 0.0f);

        // sorted times: a cursor anywhere gives the same answer
        bool sorted = std::is_sorted(times.begin(), times.end());
        if (sorted){
            anim::KeyCursor cursor;
            cursor.index = random() % (count + 2);
            anim::KeyLocation hinted = anim::locateKey(times.data(), times.size(), easings.data(), easings.size(), at, &cursor);
            EXPECT_EQ(hinted.index, location.index);
            EXPECT_EQ(hinted.interpolation, location.interpolation);
            EXPECT_EQ(cursor.index, location.index);
        }
    }
    EXPECT_EQ(anim::locateKey(nullptr, 0, nullptr, 0, 1.0f).index, 0u);
}

TEST(KeyframeSampling, EveryEaseFamilyAtASamplePoint){
    const double t = 0.3;
    const double pi = 3.14159265358979323846;
    auto bounceOut = [](double x){
        if (x < 1 / 2.75) return 7.5625 * x * x;
        if (x < 2 / 2.75){ x -= 1.5 / 2.75; return 7.5625 * x * x + 0.75; }
        if (x < 2.5 / 2.75){ x -= 2.25 / 2.75; return 7.5625 * x * x + 0.9375; }
        x -= 2.625 / 2.75;
        return 7.5625 * x * x + 0.984375;
    };
    const double s = 1.70158;
    struct Case { EaseType ease; double expected; };
    const Case cases[] = {
        {EaseType::LINEAR, t},
        {EaseType::STEP, 0.0},
        {EaseType::QUAD_IN, t * t},
        {EaseType::QUAD_OUT, 1 - (1 - t) * (1 - t)},
        {EaseType::QUAD_IN_OUT, 2 * t * t},
        {EaseType::CUBIC_IN, t * t * t},
        {EaseType::CUBIC_OUT, 1 - std::pow(1 - t, 3)},
        {EaseType::CUBIC_IN_OUT, 4 * std::pow(t, 3)},
        {EaseType::QUART_IN, std::pow(t, 4)},
        {EaseType::QUART_OUT, 1 - std::pow(1 - t, 4)},
        {EaseType::QUART_IN_OUT, 8 * std::pow(t, 4)},
        {EaseType::QUINT_IN, std::pow(t, 5)},
        {EaseType::QUINT_OUT, 1 - std::pow(1 - t, 5)},
        {EaseType::QUINT_IN_OUT, 16 * std::pow(t, 5)},
        {EaseType::SINE_IN, 1 - std::cos(t * pi / 2)},
        {EaseType::SINE_OUT, std::sin(t * pi / 2)},
        {EaseType::SINE_IN_OUT, 0.5 * (1 - std::cos(pi * t))},
        {EaseType::EXPO_IN, std::pow(2.0, 10 * (t - 1))},
        {EaseType::EXPO_OUT, 1 - std::pow(2.0, -10 * t)},
        {EaseType::EXPO_IN_OUT, 0.5 * std::pow(2.0, 20 * t - 10)},
        {EaseType::CIRC_IN, 1 - std::sqrt(1 - t * t)},
        {EaseType::CIRC_OUT, std::sqrt(1 - (t - 1) * (t - 1))},
        {EaseType::CIRC_IN_OUT, 0.5 * (1 - std::sqrt(1 - 4 * t * t))},
        {EaseType::ELASTIC_IN, -std::pow(2.0, 10 * (t - 1)) * std::sin((t - 1.1) * 5 * pi)},
        {EaseType::ELASTIC_OUT, std::pow(2.0, -10 * t) * std::sin((t - 0.1) * 5 * pi) + 1},
        {EaseType::ELASTIC_IN_OUT, -0.5 * std::pow(2.0, 10 * (2 * t - 1)) * std::sin((2 * t - 1.1) * 5 * pi)},
        {EaseType::BACK_IN, t * t * ((s + 1) * t - s)},
        {EaseType::BACK_OUT, (t - 1) * (t - 1) * ((s + 1) * (t - 1) + s) + 1},
        {EaseType::BACK_IN_OUT, 0.5 * (2 * t) * (2 * t) * ((s * 1.525 + 1) * 2 * t - s * 1.525)},
        {EaseType::BOUNCE_IN, 1 - bounceOut(1 - t)},
        {EaseType::BOUNCE_OUT, bounceOut(t)},
        {EaseType::BOUNCE_IN_OUT, (1 - bounceOut(1 - 2 * t)) * 0.5},
    };
    for (const Case& c : cases){
        EXPECT_NEAR(anim::applyEase(c.ease, (float)t), c.expected, 2e-5) << anim::easeName(c.ease);
        // and as the segment shaper of a key
        float times[] = {0, 1};
        EaseType easings[] = {c.ease, EaseType::LINEAR};
        EXPECT_NEAR(anim::locateKey(times, 2, easings, 2, (float)t).interpolation, c.expected, 2e-5) << anim::easeName(c.ease);
    }
    EXPECT_EQ(anim::applyEase(EaseType::STEP, 1.0f), 1.0f);
    EXPECT_EQ(anim::applyEase(EaseType::CUSTOM, 0.3f), 0.3f);
}

TEST(KeyframeSampling, PositionLinearStepAndEased){
    ClipTrackData position = track(ClipChannel::Position, {
        key(0, {0, 0, 0}),
        key(1, {2, 4, -2}, EaseType::STEP),
        key(2, {4, 4, 4}, EaseType::QUAD_OUT),
        key(3, {0, 0, 0})});
    expectVector(sampleOne(position, 0.25f).vector, 0.5f, 1.0f, -0.5f);
    expectVector(sampleOne(position, 1.5f).vector, 2, 4, -2);            // step holds the start
    expectVector(sampleOne(position, 1.999f).vector, 2, 4, -2);
    // quad_out at 0.5: 0.75 of the way from (4,4,4) to 0
    expectVector(sampleOne(position, 2.5f).vector, 1, 1, 1);
    // the ends are exactly the first and last keys
    expectVector(sampleOne(position, -3.0f).vector, 0, 0, 0, 0.0f);
    expectVector(sampleOne(position, 0.0f).vector, 0, 0, 0, 0.0f);
    expectVector(sampleOne(position, 3.0f).vector, 0, 0, 0, 0.0f);
    expectVector(sampleOne(position, 99.0f).vector, 0, 0, 0, 0.0f);
    expectVector(sampleOne(track(ClipChannel::Scale, {key(0.5f, {3, 2, 1})}), 0.1f).vector, 3, 2, 1, 0.0f);
}

TEST(KeyframeSampling, VectorHermite){
    ClipKey a = key(0, {0, 0, 0});
    a.inTangent = {0, 0, 0};
    a.outTangent = {1, 0, 0};
    ClipKey b = key(2, {1, 0, 0});
    b.inTangent = {0, 0, 0};
    b.outTangent = {0, 0, 0};
    // t = 0.25 of a 2 s segment: h10 * td * 1 + h01 * 1 = 0.140625 * 2 + 0.15625
    expectVector(sampleOne(track(ClipChannel::Position, {a, b}), 0.5f).vector, 0.4375f, 0, 0);
}

TEST(KeyframeSampling, RotationSlerpHermiteAndZeroNormGuard){
    // identity to 90 degrees about y: halfway is 45 degrees about y
    ClipTrackData rotation = track(ClipChannel::Rotation, {key(0, {0, 0, 0, 1}), key(1, {0, 0.70710678f, 0, 0.70710678f})});
    anim::TrackSample half = sampleOne(rotation, 0.5f);
    EXPECT_EQ(half.channel, ClipChannel::Rotation);
    expectRotation(half.rotation, Quaternion(0.92387953f, 0, 0.38268343f, 0));

    // Hermite with zero tangents between q and the same q: q
    ClipKey a = key(0, {0, 0.6f, 0, 0.8f});
    a.inTangent = {0, 0, 0, 0};
    a.outTangent = {0, 0, 0, 0};
    ClipKey b = a;
    b.time = 1;
    expectRotation(sampleOne(track(ClipChannel::Rotation, {a, b}), 0.3f).rotation, Quaternion(0.8f, 0, 0.6f, 0));

    // Hermite with tangents: normalized result of the cubic
    ClipKey c = key(0, {0, 0, 0, 1});
    c.inTangent = {0, 0, 0, 0};
    c.outTangent = {0, 1, 0, 0};
    ClipKey d = key(1, {0, 0.70710678f, 0, 0.70710678f});
    d.inTangent = {0, 0, 0, 0};
    d.outTangent = {0, 0, 0, 0};
    // t 0.5, td 1: h00 0.5, h10 0.125, h01 0.5, h11 -0.125 (in tangent 0)
    Quaternion cubic(0.5f + 0.5f * 0.70710678f, 0, 0.125f + 0.5f * 0.70710678f, 0);
    expectRotation(sampleOne(track(ClipChannel::Rotation, {c, d}), 0.5f).rotation, cubic.normalized());

    // the guard: the cubic of the AnimationClip runtime arrays directly (a clip's continuity
    // would never store -q after q), v_k = -v_k+1 with zero tangents is all zero at t 0.5
    Quaternion q(0.8f, 0, 0.6f, 0);
    Quaternion values[] = {q, -q};
    Quaternion zero(0, 0, 0, 0);
    Quaternion tangents[] = {zero, zero};
    anim::KeyLocation at;
    at.index = 1;
    at.interpolation = 0.5f;
    at.segmentDuration = 1.0f;
    Quaternion guarded = anim::sampleQuaternion(at, values, tangents, tangents);
    EXPECT_TRUE(std::isfinite(guarded.w));
    expectRotation(guarded, q);
    at.interpolation = 0.25f;
    Quaternion early = anim::sampleQuaternion(at, values, tangents, tangents);
    EXPECT_TRUE(std::isfinite(early.w));
}

TEST(KeyframeSampling, MorphAndLatticeRows){
    ClipTrackData morph = track(ClipChannel::Morph, {key(0, {0, 0.5f}), key(1, {1, 0}, EaseType::LINEAR)});
    anim::TrackSample weights = sampleOne(morph, 0.25f);
    ASSERT_EQ(weights.weights.size(), 2u);
    EXPECT_NEAR(weights.weights[0], 0.25f, 1e-6f);
    EXPECT_NEAR(weights.weights[1], 0.375f, 1e-6f);
    EXPECT_TRUE(weights.offsets.empty());

    // morph Hermite: one weight 0 -> 1 over 1 s, out tangent 2, in tangent 0, t 0.5
    ClipKey m0 = key(0, {0});
    m0.inTangent = {0};
    m0.outTangent = {2};
    ClipKey m1 = key(1, {1});
    m1.inTangent = {0};
    m1.outTangent = {0};
    anim::TrackSample cubic = sampleOne(track(ClipChannel::Morph, {m0, m1}), 0.5f);
    EXPECT_NEAR(cubic.weights[0], 0.125f * 2 + 0.5f, 1e-6f);

    ClipTrackData lattice = track(ClipChannel::Lattice, {
        key(0, {0, 0, 0, 1, 1, 1}),
        key(2, {0, 2, 0, 3, 1, 1}, EaseType::STEP)});
    lattice.points = {13, 40};
    anim::TrackSample offsets = sampleOne(lattice, 1.0f);
    ASSERT_EQ(offsets.offsets.size(), 2u);
    expectVector(offsets.offsets[0], 0, 1, 0);
    expectVector(offsets.offsets[1], 2, 1, 1);
    EXPECT_TRUE(offsets.weights.empty());
    expectVector(sampleOne(lattice, 5.0f).offsets[1], 3, 1, 1, 0.0f);

    ClipData clip;
    clip.tracks = {lattice};
    std::shared_ptr<const AnimationClip> built = AnimationClip::build(clip);
    ASSERT_EQ(built->getTracks()[0].width, 2u);
    EXPECT_EQ(built->getTracks()[0].points[1], 40);
}

TEST(KeyframeSampling, LoopingTracksRepeatTheirOwnKeys){
    ClipTrackData looping = track(ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {10, 0, 0})});
    looping.loop = true;
    ClipData clip;
    clip.tracks = {looping};
    clip.length = 3.0f;
    std::shared_ptr<const AnimationClip> built = AnimationClip::build(clip);
    const AnimationClip::Track& t = built->getTracks()[0];
    EXPECT_FLOAT_EQ(anim::trackTime(t, 1.25f), 0.25f);
    EXPECT_FLOAT_EQ(anim::trackTime(t, 0.5f), 0.5f);
    anim::TrackSample sample;
    ASSERT_TRUE(anim::sampleTrack(t, 2.5f, sample));
    expectVector(sample.vector, 5, 0, 0);
    // without loop the last key holds
    looping.loop = false;
    expectVector(sampleOne(looping, 2.5f).vector, 10, 0, 0, 0.0f);
}

TEST(KeyframeSampling, PropertyAndEmptyTracksAreNotSampled){
    ClipData clip;
    ClipTrackData property;
    property.channel = ClipChannel::Property;
    property.component = "Light";
    property.property = "intensity";
    clip.tracks = {property, track(ClipChannel::Position, {}, "Empty")};
    std::shared_ptr<const AnimationClip> built = AnimationClip::build(clip);
    std::vector<anim::TrackSample> samples;
    anim::sampleClipTracks(*built, 0.5f, samples);
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_FALSE(samples[0].valid);
    EXPECT_FALSE(samples[1].valid);
}

TEST(KeyframeSampling, SamplingIsDeterministicWhateverTheCursor){
    std::mt19937 random(7);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    ClipData clip;
    for (int c = 0; c < 5; c++){
        ClipTrackData t;
        t.path = "N";
        t.channel = (ClipChannel)c;
        if (t.channel == ClipChannel::Lattice) t.points = {1, 2};
        size_t width = c == 1 ? 4 : c == 3 ? 3 : c == 4 ? 6 : 3;
        for (int k = 0; k < 12; k++){
            ClipKey key;
            key.time = k * 0.25f;
            key.ease = (EaseType)(random() % (int)EaseType::CUSTOM);
            for (size_t i = 0; i < width; i++) key.value.push_back(unit(random));
            t.keys.push_back(key);
        }
        clip.tracks.push_back(t);
    }
    std::shared_ptr<const AnimationClip> first = AnimationClip::build(clip);
    std::shared_ptr<const AnimationClip> second = AnimationClip::build(clip);
    std::vector<anim::KeyCursor> cursors(first->getTracks().size());
    for (int i = 0; i < 300; i++){
        float time = std::fabs(unit(random)) * 3.5f;
        for (size_t t = 0; t < first->getTracks().size(); t++){
            anim::TrackSample hinted;
            anim::TrackSample plain;
            anim::sampleTrack(first->getTracks()[t], time, hinted, &cursors[t]);
            anim::sampleTrack(second->getTracks()[t], time, plain);
            EXPECT_EQ(hinted.vector.x, plain.vector.x);
            EXPECT_EQ(hinted.vector.z, plain.vector.z);
            EXPECT_EQ(hinted.rotation.w, plain.rotation.w);
            EXPECT_EQ(hinted.rotation.y, plain.rotation.y);
            EXPECT_EQ(hinted.weights, plain.weights);
            ASSERT_EQ(hinted.offsets.size(), plain.offsets.size());
            for (size_t o = 0; o < hinted.offsets.size(); o++) EXPECT_EQ(hinted.offsets[o].y, plain.offsets[o].y);
        }
    }
}

// Entity tracks go through the same samplers: the ActionSystem result equals the clip sample of
// the same keys (absolute), and a relative track adds the clip sample to its start pose
TEST(KeyframeSampling, EntityTracksMatchTheClipSamplers){
    Scene scene;
    Entity target = scene.createEntity();
    Transform transform;
    transform.position = Vector3(1, 2, 3);
    scene.addComponent<Transform>(target, transform);

    std::vector<float> times = {0.0f, 1.0f, 2.0f};
    std::vector<Vector3> values = {Vector3(0, 0, 0), Vector3(4, 2, 0), Vector3(0, 6, 8)};
    TranslateTracks move(&scene, times, values);
    move.setEasings({EaseType::CUBIC_IN_OUT, EaseType::BOUNCE_OUT});
    move.setRelative(true);
    move.setTarget(target);
    move.start();

    Quaternion r0(0.0f, 0.0f, 0.0f);
    Quaternion r1(0.0f, 120.0f, 0.0f);
    RotateTracks spin(&scene, {0.0f, 2.0f}, {r0, r1});
    spin.setEasings({EaseType::SINE_IN});
    spin.setTarget(target);
    spin.start();

    ClipTrackData position = track(ClipChannel::Position, {
        key(0, {0, 0, 0}, EaseType::CUBIC_IN_OUT), key(1, {4, 2, 0}, EaseType::BOUNCE_OUT), key(2, {0, 6, 8})});
    ClipData clip;
    clip.tracks = {position};
    ClipTrackData rotation = track(ClipChannel::Rotation, {key(0, {0, 0, 0, 1}, EaseType::SINE_IN), key(2, {0, 0, 0, 1})}, "R");
    quaternionToXYZW(r1, rotation.keys[1].value.data());
    clip.tracks.push_back(rotation);
    std::shared_ptr<const AnimationClip> built = AnimationClip::build(clip);
    ASSERT_EQ(built->getTracks().size(), 2u);

    float elapsed = 0.0f;
    for (int i = 0; i < 7; i++){
        step(scene, 0.25);
        elapsed = 0.25f * (i + 1) - 0.25f;   // the first update starts the action at time 0
        anim::TrackSample sample;
        ASSERT_TRUE(anim::sampleTrack(built->getTracks()[0], elapsed, sample));
        Vector3 position = scene.getComponent<Transform>(target).position;
        EXPECT_NEAR(position.x, 1 + sample.vector.x, 1e-5f) << "t " << elapsed;
        EXPECT_NEAR(position.y, 2 + sample.vector.y, 1e-5f) << "t " << elapsed;
        EXPECT_NEAR(position.z, 3 + sample.vector.z, 1e-5f) << "t " << elapsed;
        anim::TrackSample turn;
        ASSERT_TRUE(anim::sampleTrack(built->getTracks()[1], elapsed, turn));
        expectRotation(scene.getComponent<Transform>(target).rotation, turn.rotation, 1e-5f);
    }
}
