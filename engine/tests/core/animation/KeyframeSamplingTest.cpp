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
#include "math/Interpolation.h"

#include <algorithm>
#include <cfloat>
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
                // (FX) the one change since: a stepped key shows its own value from 1e-6 s before it
                if (easings[segment] == EaseType::STEP && nextTime > previousTime && currentTime < nextTime &&
                        nextTime - currentTime <= 1e-6f + std::fabs(nextTime) * FLT_EPSILON){
                    interpolation = 1;
                }
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

    anim::TrackSample sampleOne(const ClipTrackData& data, float time, anim::TrackSample sample = {}){
        ClipData clip;
        clip.tracks = {data};
        std::shared_ptr<const AnimationClip> built = AnimationClip::build(clip);
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

    // every component bit for bit
    void expectSameSample(const anim::TrackSample& a, const anim::TrackSample& b){
        EXPECT_EQ(a.valid, b.valid);
        EXPECT_EQ(a.vector.x, b.vector.x);
        EXPECT_EQ(a.vector.y, b.vector.y);
        EXPECT_EQ(a.vector.z, b.vector.z);
        EXPECT_EQ(a.rotation.w, b.rotation.w);
        EXPECT_EQ(a.rotation.x, b.rotation.x);
        EXPECT_EQ(a.rotation.y, b.rotation.y);
        EXPECT_EQ(a.rotation.z, b.rotation.z);
        EXPECT_EQ(a.weights, b.weights);
        ASSERT_EQ(a.offsets.size(), b.offsets.size());
        for (size_t o = 0; o < a.offsets.size(); o++){
            EXPECT_EQ(a.offsets[o].x, b.offsets[o].x);
            EXPECT_EQ(a.offsets[o].y, b.offsets[o].y);
            EXPECT_EQ(a.offsets[o].z, b.offsets[o].z);
        }
    }

    // ---- the value samplers of ActionSystem before the refactor (the reference) ----

    Vector3 referenceVector(int index, float interpolation, const std::vector<float>& times, const std::vector<Vector3>& values,
                            const std::vector<Vector3>& inTangents, const std::vector<Vector3>& outTangents){
        Vector3 previous = values[(index > 0) ? index - 1 : 0];
        bool cubic = index > 0 && values.size() > 0 && inTangents.size() == values.size() && outTangents.size() == values.size();
        if (cubic){
            float td = times[index] - times[index-1];
            return Interpolation::cubicSpline(interpolation, td, previous, outTangents[index-1], values[index], inTangents[index]);
        }
        return previous + interpolation * (values[index] - previous);
    }

    Quaternion referenceRotation(int index, float interpolation, const std::vector<float>& times, const std::vector<Quaternion>& values,
                                 const std::vector<Quaternion>& inTangents, const std::vector<Quaternion>& outTangents){
        Quaternion previousRotation = values[(index > 0) ? index - 1 : 0];
        Quaternion value;
        bool cubic = index > 0 && values.size() > 0 && inTangents.size() == values.size() && outTangents.size() == values.size();
        if (cubic){
            float td = times[index] - times[index-1];
            value = Interpolation::cubicSpline(interpolation, td, previousRotation, outTangents[index-1], values[index], inTangents[index]);
            if (value.norm() > FLT_EPSILON){
                value.normalize();
            }else{
                value = (interpolation < 0.5f) ? previousRotation : values[index];
            }
        }else{
            // the tiers of S74 R12 written out, Quaternion::slerp beyond them
            const Quaternion& next = values[index];
            const float d = previousRotation.w * next.w + previousRotation.x * next.x + previousRotation.y * next.y + previousRotation.z * next.z;
            const float absDot = std::fabs(d);
            if (absDot >= 0.9659f && interpolation > 0.0f && interpolation < 1.0f){
                const float x = 1.0f - absDot;
                const float u = absDot >= 0.999f ? interpolation
                    : interpolation + (2.0f / 3.0f) * x * (1.0f + x / 6.0f) * interpolation * (interpolation - 0.5f) * (interpolation - 1.0f);
                const float from = 1.0f - u;
                const float to = (d < 0.0f ? -1.0f : 1.0f) * u;
                const float w = from * previousRotation.w + to * next.w;
                const float qx = from * previousRotation.x + to * next.x;
                const float qy = from * previousRotation.y + to * next.y;
                const float qz = from * previousRotation.z + to * next.z;
                const float inverse = 1.0f / std::sqrt(w * w + qx * qx + qy * qy + qz * qz);
                value = Quaternion(w * inverse, qx * inverse, qy * inverse, qz * inverse);
            }else{
                value = Quaternion::slerp(interpolation, previousRotation, next);
            }
        }
        return value;
    }

    float referenceRowValue(int index, float interpolation, float td, bool cubic, const std::vector<std::vector<float>>& values,
                            const std::vector<std::vector<float>>& inTangents, const std::vector<std::vector<float>>& outTangents, size_t i){
        const std::vector<float>& previous = values[(index > 0) ? index - 1 : 0];
        if (cubic){
            return Interpolation::cubicSpline(interpolation, td, previous[i], outTangents[index-1][i], values[index][i], inTangents[index][i]);
        }
        return previous[i] + interpolation * (values[index][i] - previous[i]);
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
    anim::TrackSample previous;
    previous.offsets = {Vector3(9, 9, 9)};
    anim::TrackSample weights = sampleOne(morph, 0.25f, previous);
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
    anim::TrackSample offsets = sampleOne(lattice, 1.0f, weights);
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
            anim::TrackSample stale;
            anim::KeyCursor far;
            far.index = random() % 20;
            anim::sampleTrack(first->getTracks()[t], time, hinted, &cursors[t]);
            anim::sampleTrack(second->getTracks()[t], time, plain);
            anim::sampleTrack(second->getTracks()[t], time, stale, &far);
            expectSameSample(hinted, plain);
            expectSameSample(stale, plain);
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

// The value samplers against copies of the ActionSystem expressions they replaced, bit for bit,
// with ActionSystem's tangent gating (vector, rotation incl. the zero-norm guard, morph, lattice)
TEST(KeyframeSampling, ValueSamplersMatchTheFormerActionSystemExpressions){
    std::mt19937 random(31);
    std::uniform_real_distribution<float> unit(-2.0f, 2.0f);
    std::uniform_real_distribution<float> fraction(0.0f, 1.0f);
    for (int round = 0; round < 3000; round++){
        size_t count = 1 + random() % 5;
        bool cubic = round % 3 == 0;
        std::vector<float> times;
        std::vector<Vector3> vectors, vin, vout;
        std::vector<Quaternion> rotations, rin, rout;
        std::vector<std::vector<float>> rows, win, wout;
        size_t width = 1 + random() % 8;
        for (size_t k = 0; k < count; k++){
            times.push_back((float)k * 0.5f + fraction(random) * 0.25f);
            vectors.push_back(Vector3(unit(random), unit(random), unit(random)));
            Quaternion q(unit(random), unit(random), unit(random), unit(random));
            // some segments end on the opposite quaternion: the zero-norm guard
            rotations.push_back(round % 7 == 1 && k > 0 ? -rotations[k - 1] : q.normalized());
            std::vector<float> row;
            for (size_t i = 0; i < width; i++) row.push_back(unit(random));
            rows.push_back(row);
            if (cubic){
                bool zero = round % 7 == 1;
                vin.push_back(Vector3(unit(random), unit(random), unit(random)));
                vout.push_back(Vector3(unit(random), unit(random), unit(random)));
                rin.push_back(zero ? Quaternion(0, 0, 0, 0) : Quaternion(unit(random), unit(random), unit(random), unit(random)));
                rout.push_back(zero ? Quaternion(0, 0, 0, 0) : Quaternion(unit(random), unit(random), unit(random), unit(random)));
                std::vector<float> a, b;
                for (size_t i = 0; i < width; i++){ a.push_back(unit(random)); b.push_back(unit(random)); }
                win.push_back(a);
                wout.push_back(b);
            }
        }
        int index = (int)(random() % count);
        float interpolation = round % 11 == 0 ? 0.5f : fraction(random);
        anim::KeyLocation at;
        at.index = (size_t)index;
        at.interpolation = interpolation;
        if (index > 0) at.segmentDuration = times[index] - times[index - 1];
        bool tangents = cubic && index > 0;

        Vector3 v = anim::sampleVector3(at, vectors.data(), tangents ? vin.data() : nullptr, tangents ? vout.data() : nullptr);
        Vector3 vr = referenceVector(index, interpolation, times, vectors, vin, vout);
        ASSERT_EQ(v.x, vr.x) << "round " << round;
        ASSERT_EQ(v.y, vr.y);
        ASSERT_EQ(v.z, vr.z);

        Quaternion q = anim::sampleQuaternion(at, rotations.data(), tangents ? rin.data() : nullptr, tangents ? rout.data() : nullptr);
        Quaternion qr = referenceRotation(index, interpolation, times, rotations, rin, rout);
        ASSERT_EQ(q.w, qr.w) << "round " << round;
        ASSERT_EQ(q.x, qr.x);
        ASSERT_EQ(q.y, qr.y);
        ASSERT_EQ(q.z, qr.z);

        const std::vector<float>& previous = rows[index > 0 ? index - 1 : 0];
        std::vector<float> morph(width);
        anim::sampleFloatRow(at, previous.data(), rows[index].data(), width,
                             tangents ? wout[index - 1].data() : nullptr, tangents ? win[index].data() : nullptr, morph.data());
        for (size_t i = 0; i < width; i++){
            ASSERT_EQ(morph[i], referenceRowValue(index, interpolation, at.segmentDuration, tangents, rows, win, wout, i)) << "round " << round;
        }

        // lattice: one point per call, as latticeTracksUpdate does
        Vector3 offset;
        const Vector3* outRow = tangents ? &vout[index - 1] : nullptr;
        const Vector3* inRow = tangents ? &vin[index] : nullptr;
        anim::sampleVector3Row(at, &vectors[index > 0 ? index - 1 : 0], &vectors[index], 1, outRow, inRow, &offset);
        ASSERT_EQ(offset.x, vr.x);
        ASSERT_EQ(offset.y, vr.y);
        ASSERT_EQ(offset.z, vr.z);
    }
}

namespace {

    struct TrueQuaternion{
        double w, x, y, z;
    };

    TrueQuaternion normalizedTrue(double w, double x, double y, double z){
        const double n = std::sqrt(w * w + x * x + y * y + z * z);
        return {w / n, x / n, y / n, z / n};
    }

    // slerp in double, on the shortest arc
    TrueQuaternion trueSlerp(double t, const Quaternion& a, const Quaternion& b){
        double d = (double)a.w * b.w + (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
        const double side = d < 0.0 ? -1.0 : 1.0;
        d = std::min(1.0, std::fabs(d));
        const double th = std::acos(d);
        const double s = std::sin(th);
        const double m1 = s > 1e-12 ? std::sin((1.0 - t) * th) / s : 1.0 - t;
        const double m2 = (s > 1e-12 ? std::sin(t * th) / s : t) * side;
        return normalizedTrue(m1 * a.w + m2 * b.w, m1 * a.x + m2 * b.x, m1 * a.y + m2 * b.y, m1 * a.z + m2 * b.z);
    }

    // the rotation between two quaternions as the arc at 1 m radius, in mm
    double rotationErrorMm(const Quaternion& q, const TrueQuaternion& truth){
        const TrueQuaternion n = normalizedTrue(q.w, q.x, q.y, q.z);
        const double d = std::fabs(n.w * truth.w + n.x * truth.x + n.y * truth.y + n.z * truth.z);
        return 2.0 * std::atan2(std::sqrt(std::max(0.0, 1.0 - d * d)), d) * 1000.0;
    }

    Quaternion sampledAt(float t, const Quaternion& a, const Quaternion& b){
        const Quaternion values[2] = {a, b};
        anim::KeyLocation at;
        at.index = 1;
        at.interpolation = t;
        return anim::sampleQuaternion(at, values, nullptr, nullptr);
    }

    bool sameBits(const Quaternion& a, const Quaternion& b){
        return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z;
    }

    // keys `degrees` apart about a random axis; b on the far side of the hypersphere when `flip`
    std::pair<Quaternion, Quaternion> keysApart(std::mt19937& random, double degrees, bool flip){
        std::normal_distribution<double> gauss;
        const TrueQuaternion a = normalizedTrue(gauss(random), gauss(random), gauss(random), gauss(random));
        const TrueQuaternion axis = normalizedTrue(0.0, gauss(random), gauss(random), gauss(random));
        const double half = 0.5 * degrees * 3.14159265358979323846 / 180.0;
        const double c = std::cos(half), s = std::sin(half);
        const TrueQuaternion b = normalizedTrue(
            a.w * c - s * (a.x * axis.x + a.y * axis.y + a.z * axis.z),
            a.x * c + s * (a.w * axis.x + a.y * axis.z - a.z * axis.y),
            a.y * c + s * (a.w * axis.y + a.z * axis.x - a.x * axis.z),
            a.z * c + s * (a.w * axis.z + a.x * axis.y - a.y * axis.x));
        const float side = flip ? -1.0f : 1.0f;
        return {Quaternion((float)a.w, (float)a.x, (float)a.y, (float)a.z),
                Quaternion(side * (float)b.w, side * (float)b.x, side * (float)b.y, side * (float)b.z)};
    }

}

// S74 R12, R13, AC5: the tiered slerp against true slerp: the bound per segment angle, the tier
// boundaries, the end keys, the shortest arc, and the exact path where the tiers do not apply
TEST(KeyframeSampling, TieredSlerpMatchesSlerp){
    std::mt19937 random(7);
    const double boundMm = 0.01;
    // both sides of |d| = 0.999 (5.126 degrees between keys) and |d| = 0.9659 (30.01 degrees)
    for (double degrees : {0.1, 0.5, 1.0, 2.0, 4.0, 5.1, 5.13, 5.2, 10.0, 15.0, 20.0, 25.0, 29.0, 30.0}){
        double worst = 0.0;
        for (int pair = 0; pair < 300; pair++){
            const std::pair<Quaternion, Quaternion> keys = keysApart(random, degrees, pair % 2 == 1);
            for (int i = 1; i < 40; i++){
                const float t = (float)i / 40.0f;
                const Quaternion q = sampledAt(t, keys.first, keys.second);
                worst = std::max(worst, rotationErrorMm(q, trueSlerp(t, keys.first, keys.second)));
                ASSERT_NEAR(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z, 1.0f, 1e-5f) << degrees << " degrees";
                // the shortest arc: never on the far side of the first key
                ASSERT_GE(q.dot(keys.first), 0.0f) << degrees << " degrees, t " << t;
            }
        }
        EXPECT_LE(worst, boundMm) << degrees << " degrees between keys";
    }

    // the fast tiers are taken: not slerp's bits inside them, slerp's bits beyond them
    for (double degrees : {4.0, 20.0}){
        int differing = 0;
        for (int pair = 0; pair < 50; pair++){
            const std::pair<Quaternion, Quaternion> keys = keysApart(random, degrees, false);
            differing += sameBits(sampledAt(0.3f, keys.first, keys.second), Quaternion::slerp(0.3f, keys.first, keys.second)) ? 0 : 1;
        }
        EXPECT_GT(differing, 25) << degrees << " degrees between keys";
    }
    for (double degrees : {31.0, 45.0, 60.0, 90.0, 120.0, 170.0}){
        for (int pair = 0; pair < 100; pair++){
            const std::pair<Quaternion, Quaternion> keys = keysApart(random, degrees, pair % 2 == 1);
            const float t = (float)(pair + 1) / 101.0f;
            ASSERT_TRUE(sameBits(sampledAt(t, keys.first, keys.second), Quaternion::slerp(t, keys.first, keys.second))) << degrees << " degrees";
        }
    }

    // the end keys, exactly: b on a's side of the hypersphere, as slerp gives it
    for (double degrees : {1.0, 4.0, 20.0, 30.0, 60.0}){
        const std::pair<Quaternion, Quaternion> keys = keysApart(random, degrees, false);
        EXPECT_TRUE(sameBits(sampledAt(0.0f, keys.first, keys.second), keys.first)) << degrees;
        EXPECT_TRUE(sameBits(sampledAt(1.0f, keys.first, keys.second), keys.second)) << degrees;
        const Quaternion opposite = -keys.second;
        EXPECT_TRUE(sameBits(sampledAt(0.0f, keys.first, opposite), keys.first)) << degrees;
        EXPECT_TRUE(sameBits(sampledAt(1.0f, keys.first, opposite), keys.second)) << degrees << " the shortest arc ends on a's side";
    }

    // an ease past the segment (Back, Elastic) extrapolates as slerp does; so do NaN and zero keys
    const std::pair<Quaternion, Quaternion> near = keysApart(random, 10.0, false);
    for (float t : {-0.2f, 1.3f}){
        EXPECT_TRUE(sameBits(sampledAt(t, near.first, near.second), Quaternion::slerp(t, near.first, near.second))) << t;
    }
    const Quaternion zero(0, 0, 0, 0);
    EXPECT_TRUE(sameBits(sampledAt(0.5f, near.first, zero), Quaternion::slerp(0.5f, near.first, zero)));
    const Quaternion poisoned(NAN, 0, 0, 1);
    EXPECT_TRUE(std::isnan(sampledAt(0.5f, poisoned, near.second).w));
}

TEST(KeyframeSampling, NaNTimesAndBinarySearch){
    float times[] = {0, 1, 2, 3};
    anim::KeyCursor cursor;
    cursor.index = 2;
    EXPECT_EQ(anim::locateKey(times, 4, nullptr, 0, NAN, &cursor).index, anim::locateKey(times, 4, nullptr, 0, NAN).index);
    EXPECT_EQ(cursor.index, 0u);

    // a long track: without a cursor (binary search) and with stale cursors anywhere, the
    // location is the linear scan's
    std::vector<ClipKey> keys;
    for (int k = 0; k < 500; k++) keys.push_back(key(k * 0.1f, {(float)k, 0, 0}, (EaseType)(k % (int)EaseType::CUSTOM)));
    ClipData clip;
    clip.tracks = {track(ClipChannel::Position, keys)};
    std::shared_ptr<const AnimationClip> built = AnimationClip::build(clip);
    const AnimationClip::Track& t = built->getTracks()[0];
    std::mt19937 random(5);
    std::uniform_real_distribution<float> time(-1.0f, 51.0f);
    for (int i = 0; i < 2000; i++){
        float at = time(random);
        anim::KeyLocation linear = anim::locateKey(t.times, t.keyCount, t.easings, t.keyCount, at);
        anim::KeyCursor stale;
        stale.index = random() % 600;
        anim::KeyLocation hinted = anim::locateKey(t.times, t.keyCount, t.easings, t.keyCount, at, &stale);
        ASSERT_EQ(hinted.index, linear.index) << at;
        ASSERT_EQ(hinted.interpolation, linear.interpolation);
        anim::TrackSample plain, withCursor;
        anim::KeyCursor walking;
        walking.index = random() % 600;
        anim::sampleTrack(t, at, plain);
        anim::sampleTrack(t, at, withCursor, &walking);
        expectSameSample(plain, withCursor);
    }
    // a NaN time samples the first key
    anim::TrackSample nan;
    ASSERT_TRUE(anim::sampleTrack(t, NAN, nan));
    EXPECT_EQ(nan.vector.x, 0.0f);
}

// Stepped keys (Unity's rule): at exactly a key's time both samplers show that key's own value,
// the entity tracks and the clip alike; between keys the value of the key the segment starts at
TEST(KeyframeSampling, SteppedKeysShowTheirOwnValueAtTheirTime){
    Scene scene;
    Entity target = scene.createEntity();
    scene.addComponent<Transform>(target, Transform());

    std::vector<float> times = {0.0f, 0.5f, 1.0f, 1.5f};
    std::vector<Vector3> values = {Vector3(1, 0, 0), Vector3(2, 0, 0), Vector3(3, 0, 0), Vector3(4, 0, 0)};
    TranslateTracks move(&scene, times, values);
    move.setEasings({EaseType::STEP, EaseType::STEP, EaseType::STEP});
    move.setTarget(target);
    move.start();

    ClipTrackData position = track(ClipChannel::Position, {
        key(0, {1, 0, 0}, EaseType::STEP), key(0.5f, {2, 0, 0}, EaseType::STEP),
        key(1, {3, 0, 0}, EaseType::STEP), key(1.5f, {4, 0, 0})});
    ClipData clip;
    clip.tracks = {position};
    std::shared_ptr<const AnimationClip> built = AnimationClip::build(clip);
    ASSERT_EQ(built->getTracks().size(), 1u);

    // 0.125 s updates land exactly on every key time (the first update starts the action at 0)
    const float expected[] = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4};
    for (int i = 0; i < 13; i++){
        step(scene, 0.125);
        const float elapsed = 0.125f * i;
        anim::TrackSample sample;
        ASSERT_TRUE(anim::sampleTrack(built->getTracks()[0], elapsed, sample));
        EXPECT_EQ(sample.vector.x, expected[i]) << "clip at t " << elapsed;
        EXPECT_EQ(scene.getComponent<Transform>(target).position.x, expected[i]) << "entity track at t " << elapsed;
        // a cursor coming from either side lands on the same value
        anim::KeyCursor cursor;
        cursor.index = (size_t)(i % 4);
        ASSERT_TRUE(anim::sampleTrack(built->getTracks()[0], elapsed, sample, &cursor));
        EXPECT_EQ(sample.vector.x, expected[i]) << "clip with a cursor at t " << elapsed;
    }

    // an .anim key time is the entity key's float rounded to 1e-6 s: at either form of the
    // time the key shows its own value (1/3 s stored as 0.333333)
    const EaseType step[] = {EaseType::STEP};
    const float entityTimes[] = {0.0f, 1.0f / 3.0f};
    const float clipTimes[] = {0.0f, 0.333333f};
    EXPECT_EQ(anim::locateKey(entityTimes, 2, step, 1, 0.333333f).interpolation, 1.0f);
    EXPECT_EQ(anim::locateKey(clipTimes, 2, step, 1, 1.0f / 3.0f).interpolation, 1.0f);
    EXPECT_EQ(anim::locateKey(entityTimes, 2, step, 1, 0.3333f).interpolation, 0.0f) << "earlier: the start key";
}
