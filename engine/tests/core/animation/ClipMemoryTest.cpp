// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S77 R1-R5, R42: what a built clip keeps on the heap. Tracks hold no slack, identical key times
// share one range, all-linear tracks have no easings, and none of it changes what the clip samples
// or what it writes back. memoryBytes() is checked against an independent count, and the pool's
// "Animation.Clips" record follows every way an entry comes and goes. The two shapes are generated
// here with the track and key counts and the path lengths of the real clips S77 measured.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include "animation/AnimatorRig.h"
#include "animation/KeyframeSampling.h"
#include "memory/MemoryStats.h"
#include "pool/AnimationClipPoolTesting.h"

#include <cstring>
#include <set>

using namespace animatortest;

namespace {

    constexpr size_t kMeanPathLength = 97;

    // a unique bone path as long as the real rigs' (their mean is 97 characters)
    std::string bonePath(size_t bone){
        std::string path = "Armature/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/bone_" + std::to_string(bone);
        path.resize(kMeanPathLength, '_');
        return path;
    }

    std::vector<float> turn(float degrees){
        const float half = degrees * 3.14159265358979f / 360.0f;
        return {0.0f, std::sin(half), 0.0f, std::cos(half)};
    }

    // `count` keys of one spacing; a different spacing makes a different time array
    ClipTrackData keyedRotation(const std::string& path, size_t count, float spacing){
        ClipTrackData t;
        t.path = path;
        t.channel = ClipChannel::Rotation;
        for (size_t k = 0; k < count; k++) t.keys.push_back(key((float)k * spacing, turn((float)k * 3.0f)));
        return t;
    }

    // The reduced `walk` shape: 132 tracks, 911 keys in 23 groups of key times. 100 constant tracks
    // of one key; 10 groups of 18 keys with two tracks each; 12 tracks with times of their own
    ClipData walkShape(){
        ClipData data;
        size_t bone = 0;
        for (size_t i = 0; i < 100; i++){
            data.tracks.push_back(track(bonePath(bone++), ClipChannel::Position, {key(0, {0.0f, 1.0f, 0.0f})}));
        }
        size_t group = 0;
        for (size_t g = 0; g < 10; g++, group++){
            const float spacing = 0.05f + (float)group * 0.001f;
            data.tracks.push_back(keyedRotation(bonePath(bone++), 18, spacing));
            data.tracks.push_back(keyedRotation(bonePath(bone++), 18, spacing));
        }
        for (size_t g = 0; g < 12; g++, group++){
            data.tracks.push_back(keyedRotation(bonePath(bone++), g == 11 ? 44 : 37, 0.03f + (float)group * 0.0001f));
        }
        return data;
    }

    // The dense `pab_walk` shape: 67 bones, translation, rotation and scale, 41 keys each
    ClipData denseShape(){
        ClipData data;
        for (size_t bone = 0; bone < 67; bone++){
            const std::string path = bonePath(bone);
            ClipTrackData position = track(path, ClipChannel::Position, {});
            ClipTrackData scale = track(path, ClipChannel::Scale, {});
            for (size_t k = 0; k < 41; k++){
                const float time = (float)k / 30.0f;
                position.keys.push_back(key(time, {(float)k * 0.01f, 1.0f, 0.0f}));
                scale.keys.push_back(key(time, {1.0f, 1.0f, 1.0f}));
            }
            data.tracks.push_back(position);
            data.tracks.push_back(keyedRotation(path, 41, 1.0f / 30.0f));
            data.tracks.push_back(scale);
        }
        return data;
    }

    size_t keyCount(const AnimationClip& clip){
        size_t keys = 0;
        for (const AnimationClip::Track& t : clip.getTracks()) keys += t.keyCount;
        return keys;
    }

    const AnimationClip::Track& trackOf(const AnimationClip& clip, const std::string& path, ClipChannel channel){
        const int index = clip.findTrack(path, channel);
        EXPECT_GE(index, 0) << path;
        return clip.getTracks()[(size_t)(index < 0 ? 0 : index)];
    }

    ClipTrackData propertyTrack(const std::string& path, const std::string& component, const std::string& name, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = ClipChannel::Property;
        t.component = component;
        t.property = name;
        t.keys = std::move(keys);
        return t;
    }

    // Every kind of track the builder treats differently: shared times, an eased track on the same
    // times, a stepped one, an ease on the last key only, one key, tangents, a loop, morph, lattice,
    // interpolated, discrete and unknown properties
    ClipData everyKindOfTrack(){
        ClipData data;
        data.frameRate = 24.0f;
        data.wrap = ClipWrapMode::Loop;
        const std::vector<float> a = {0.0f, 1.0f, 2.0f};
        const std::vector<float> b = {3.0f, 2.0f, 1.0f};
        data.tracks.push_back(track("Linear", ClipChannel::Position, {key(0, a), key(0.5f, b), key(1, a)}));
        data.tracks.push_back(track("Linear", ClipChannel::Rotation, {key(0, turn(0)), key(0.5f, turn(40)), key(1, turn(170))}));
        data.tracks.push_back(track("Linear", ClipChannel::Scale, {key(0, a), key(0.5f, b), key(1, b)}));
        data.tracks.push_back(track("Eased", ClipChannel::Position, {key(0, a, EaseType::QUAD_OUT), key(0.5f, b, EaseType::LINEAR), key(1, a)}));
        data.tracks.push_back(track("Stepped", ClipChannel::Position, {key(0, a, EaseType::STEP), key(0.5f, b, EaseType::STEP), key(1, a)}));
        data.tracks.push_back(track("LastOnly", ClipChannel::Position, {key(0, a), key(0.5f, b), key(1, a, EaseType::CUBIC_IN)}));
        data.tracks.push_back(track("OneKey", ClipChannel::Position, {key(0.25f, b)}));
        data.tracks.push_back(track("OneKeyEased", ClipChannel::Position, {key(0.25f, b, EaseType::BOUNCE_OUT)}));
        data.tracks.push_back(track("Other", ClipChannel::Position, {key(0, a), key(0.3f, b), key(0.9f, a)}));
        ClipKey in = key(0, a);
        in.inTangent = {0.0f, 0.0f, 0.0f};
        in.outTangent = {1.0f, 1.0f, 1.0f};
        ClipKey out = key(1, b);
        out.inTangent = {1.0f, 0.0f, 0.0f};
        out.outTangent = {0.0f, 0.0f, 0.0f};
        data.tracks.push_back(track("Cubic", ClipChannel::Position, {in, out}));
        ClipTrackData looping = track("Loop", ClipChannel::Position, {key(0, a), key(0.5f, b)});
        looping.loop = true;
        data.tracks.push_back(looping);
        ClipTrackData morph = track("Body", ClipChannel::Morph, {key(0, {0.0f, 1.0f}), key(1, {1.0f, 0.5f})});
        data.tracks.push_back(morph);
        ClipTrackData lattice = track("Cage", ClipChannel::Lattice, {key(0, {0, 0, 0, 1, 1, 1}), key(1, {2, 2, 2, 3, 3, 3})});
        lattice.points = {4, 7};
        data.tracks.push_back(lattice);
        data.tracks.push_back(propertyTrack("Lamp", "light", "intensity", {key(0, {1.0f}), key(1, {4.0f})}));
        data.tracks.push_back(propertyTrack("Box", "gameobject", "active", {key(0, {0.0f}), key(0.5f, {1.0f}), key(1, {0.0f})}));
        data.tracks.push_back(propertyTrack("Box", "gameobject", "unheard_of", {}));
        data.events = {event(0.25f, "step"), event(0.75f, "land")};
        data.events[1].args = {AnimationEventArg(std::string("a string longer than any small string buffer")), AnimationEventArg(2.0)};
        data.unknownKeys = {{"made_by", "\"a tool whose name is longer than a small string\""}};
        return data;
    }

    void expectSameSample(const anim::TrackSample& a, const anim::TrackSample& b, const std::string& what){
        EXPECT_EQ(a.valid, b.valid) << what;
        EXPECT_EQ(a.channel, b.channel) << what;
        EXPECT_EQ(std::memcmp(&a.vector, &b.vector, sizeof(Vector3)), 0) << what;
        EXPECT_EQ(std::memcmp(&a.rotation, &b.rotation, sizeof(Quaternion)), 0) << what;
        EXPECT_EQ(a.weights, b.weights) << what;
        ASSERT_EQ(a.offsets.size(), b.offsets.size()) << what;
        if (!a.offsets.empty()){
            EXPECT_EQ(std::memcmp(a.offsets.data(), b.offsets.data(), a.offsets.size() * sizeof(Vector3)), 0) << what;
        }
    }

    // bit for bit, at every key time, between keys, before and past the ends
    void expectSameSampling(const AnimationClip& a, const AnimationClip& b){
        ASSERT_EQ(a.getTracks().size(), b.getTracks().size());
        std::set<float> times = {-1.0f, 0.0f, a.getLength() + 1.0f};
        for (const AnimationClip::Track& t : a.getTracks()){
            for (size_t k = 0; k < t.keyCount; k++){
                times.insert(t.times[k]);
                times.insert(t.times[k] + 0.013f);
                times.insert(std::nextafter(t.times[k], -2.0f));
            }
        }
        std::vector<anim::TrackSample> first, second;
        for (float time : times){
            anim::sampleClipTracks(a, time, first);
            anim::sampleClipTracks(b, time, second);
            ASSERT_EQ(first.size(), second.size());
            for (size_t i = 0; i < first.size(); i++){
                expectSameSample(first[i], second[i], a.getTracks()[i].path + " at " + std::to_string(time));
            }
        }
    }

    uint64_t recordedBytes(){
        for (const memory::StatsEntry& entry : memory::snapshot()){
            if (entry.name == "Animation.Clips") return entry.current;
        }
        ADD_FAILURE() << "no Animation.Clips record";
        return 0;
    }

    std::string clipFileText(float x){
        ClipData data = slide("Hips", x);
        data.tracks.push_back(track("Hips/Spine", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {x, x, x})}));
        return anim::writeClip(data);
    }

    class ClipMemoryTest : public ::testing::Test, public AnimatorFixture {};

}

TEST(ClipMemory, TheWalkShapeFitsItsBudget){
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(walkShape());
    ASSERT_EQ(clip->getTracks().size(), 132u);
    ASSERT_EQ(keyCount(*clip), 911u);
    EXPECT_LE(clip->memoryBytes(), 72000u);
}

TEST(ClipMemory, TheDenseShapeFitsItsBudget){
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(denseShape());
    ASSERT_EQ(clip->getTracks().size(), 201u);
    ASSERT_EQ(keyCount(*clip), 8241u);
    EXPECT_LE(clip->memoryBytes(), 190000u);
}

TEST(ClipMemory, TheTrackArrayHoldsNoSlack){
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(walkShape());
    EXPECT_EQ(clip->getTracks().capacity(), clip->getTracks().size());
}

TEST(ClipMemory, IdenticalTimesShareOneRange){
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(everyKindOfTrack());
    const AnimationClip::Track& position = trackOf(*clip, "Linear", ClipChannel::Position);
    EXPECT_EQ(trackOf(*clip, "Linear", ClipChannel::Rotation).times, position.times);
    EXPECT_EQ(trackOf(*clip, "Linear", ClipChannel::Scale).times, position.times);
    EXPECT_EQ(trackOf(*clip, "Eased", ClipChannel::Position).times, position.times) << "times alone decide, not eases";
    EXPECT_NE(trackOf(*clip, "Other", ClipChannel::Position).times, position.times);
    EXPECT_NE(trackOf(*clip, "Loop", ClipChannel::Position).times, position.times) << "a shorter track with a common prefix keeps its own";

    // the shape's groups: 1 + 10 + 12 arrays of key times
    std::shared_ptr<const AnimationClip> walk = AnimationClip::build(walkShape());
    std::set<const float*> ranges;
    for (const AnimationClip::Track& t : walk->getTracks()) ranges.insert(t.times);
    EXPECT_EQ(ranges.size(), 23u);
}

TEST(ClipMemory, TimesOneStepApartDoNotShare){
    // the smallest difference normalize keeps (it rounds key times to 1e-6 s)
    const float apart = 0.500001f;
    ClipData data;
    data.tracks.push_back(track("A", ClipChannel::Position, {key(0, {0, 0, 0}), key(0.5f, {1, 1, 1}), key(1, {0, 0, 0})}));
    data.tracks.push_back(track("B", ClipChannel::Position, {key(0, {0, 0, 0}), key(apart, {1, 1, 1}), key(1, {0, 0, 0})}));
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    const AnimationClip::Track& a = trackOf(*clip, "A", ClipChannel::Position);
    const AnimationClip::Track& b = trackOf(*clip, "B", ClipChannel::Position);
    EXPECT_NE(a.times, b.times);
    EXPECT_EQ(a.times[1], 0.5f);
    EXPECT_EQ(b.times[1], apart);
    expectSameSampling(*clip, *AnimationClip::build(clip->toData()));
}

TEST(ClipMemory, AnAllLinearTrackHasNoEasings){
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(everyKindOfTrack());
    EXPECT_EQ(trackOf(*clip, "Linear", ClipChannel::Position).easings, nullptr);
    EXPECT_EQ(trackOf(*clip, "Other", ClipChannel::Position).easings, nullptr);
    EXPECT_EQ(trackOf(*clip, "OneKey", ClipChannel::Position).easings, nullptr);
    EXPECT_EQ(trackOf(*clip, "Cubic", ClipChannel::Position).easings, nullptr);
    EXPECT_EQ(trackOf(*clip, "LastOnly", ClipChannel::Position).easings, nullptr) << "the last key starts no segment";
    EXPECT_EQ(trackOf(*clip, "OneKeyEased", ClipChannel::Position).easings, nullptr) << "the last key starts no segment";

    const AnimationClip::Track& eased = trackOf(*clip, "Eased", ClipChannel::Position);
    ASSERT_NE(eased.easings, nullptr);
    EXPECT_EQ(eased.easings[0], EaseType::QUAD_OUT);
    EXPECT_EQ(eased.easings[1], EaseType::LINEAR);
    ASSERT_NE(trackOf(*clip, "Stepped", ClipChannel::Position).easings, nullptr);

    const ClipData normalized = clip->toData();
    const int discrete = normalized.findPropertyTrack("Box", "gameobject", "active");
    ASSERT_GE(discrete, 0);
    ASSERT_NE(clip->getTracks()[(size_t)discrete].easings, nullptr) << "a discrete property steps";
    EXPECT_EQ(clip->getTracks()[(size_t)discrete].easings[0], EaseType::STEP);
}

TEST(ClipMemory, EveryReaderOfANullEasingSamples){
    // the three ways keys are located: the clip sampler, sampleTrack with and without a hint
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(everyKindOfTrack());
    const AnimationClip::Track& linear = trackOf(*clip, "Linear", ClipChannel::Position);
    ASSERT_EQ(linear.easings, nullptr);
    anim::TrackSample sample;
    ASSERT_TRUE(anim::sampleTrack(linear, 0.25f, sample));
    EXPECT_FLOAT_EQ(sample.vector.x, 1.5f);
    anim::KeyCursor cursor;
    ASSERT_TRUE(anim::sampleTrack(linear, 0.75f, sample, &cursor));
    EXPECT_FLOAT_EQ(sample.vector.x, 1.5f);
    EXPECT_FLOAT_EQ(anim::locateKey(linear.times, linear.keyCount, linear.easings, linear.keyCount, 0.25f).interpolation, 0.5f);
}

TEST(ClipMemory, RoundTripSamplesIdentically){
    for (const ClipData& source : {everyKindOfTrack(), walkShape(), denseShape()}){
        std::shared_ptr<const AnimationClip> clip = AnimationClip::build(source);
        std::shared_ptr<const AnimationClip> again = AnimationClip::build(clip->toData());
        expectSameSampling(*clip, *again);
        EXPECT_EQ(again->memoryBytes(), clip->memoryBytes());
    }
}

TEST(ClipMemory, RoundTripKeepsEveryValue){
    ClipData source = everyKindOfTrack();
    anim::normalize(source, nullptr);
    const ClipData back = AnimationClip::build(source)->toData();
    ASSERT_EQ(back.tracks.size(), source.tracks.size());
    for (size_t t = 0; t < source.tracks.size(); t++){
        const ClipTrackData& a = source.tracks[t];
        const ClipTrackData& b = back.tracks[t];
        // an unknown property keeps its text and is never built
        if (a.channel == ClipChannel::Property && !a.known) continue;
        ASSERT_EQ(a.keys.size(), b.keys.size()) << a.path;
        for (size_t k = 0; k < a.keys.size(); k++){
            EXPECT_EQ(std::memcmp(&a.keys[k].time, &b.keys[k].time, sizeof(float)), 0) << a.path << " key " << k;
            EXPECT_EQ(a.keys[k].ease, b.keys[k].ease) << a.path << " key " << k;
            EXPECT_EQ(a.keys[k].value, b.keys[k].value) << a.path << " key " << k;
            EXPECT_EQ(a.keys[k].inTangent, b.keys[k].inTangent) << a.path << " key " << k;
            EXPECT_EQ(a.keys[k].outTangent, b.keys[k].outTangent) << a.path << " key " << k;
        }
    }
}

TEST(ClipMemory, AnimFilesStayByteStable){
    ClipData source = everyKindOfTrack();
    anim::normalize(source, nullptr);
    const std::string text = anim::writeClip(source);
    anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(anim::writeClip(*parsed.data), text);
    // through the runtime form too (what a converter that starts from a loaded clip writes)
    EXPECT_EQ(anim::writeClip(AnimationClip::build(parsed)->toData()), text);
}

TEST(ClipMemory, MemoryBytesIsWhatTheClipOwns){
    // a count that does not go through the implementation: everything here holds exact capacities
    // (reserved, or copied into a new string)
    ClipData source;
    source.guid = "0123456789abcdef0123456789abcdef";
    source.previewModel = "assets/models/a_preview_model_with_a_long_path.glb";
    const std::string first = bonePath(0);
    const std::string second = bonePath(1);
    source.tracks.push_back(track(first, ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {1, 1, 1})}));
    source.tracks.push_back(track(first, ClipChannel::Rotation, {key(0, turn(0)), key(1, turn(10))}));
    source.tracks.push_back(track(second, ClipChannel::Position, {key(0, {0, 0, 0}, EaseType::QUAD_IN), key(2, {1, 1, 1})}));
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(source);

    auto heapOf = [](const std::string& text){ return text.capacity() + 1; };
    size_t expected = sizeof(AnimationClip);
    expected += 3 * sizeof(AnimationClip::Track);
    for (const AnimationClip::Track& t : clip->getTracks()) expected += heapOf(t.path);
    expected += heapOf(clip->getGuid()) + heapOf(clip->getPreviewModel());
    expected += 4 * sizeof(float);          // two arrays of times, the first shared
    expected += 2 * sizeof(EaseType);       // one eased track
    expected += 4 * sizeof(Vector3) + 2 * sizeof(Quaternion);
    EXPECT_EQ(clip->memoryBytes(), expected);

    ClipData more = source;
    more.events = {event(0.5f, "an_event_function_with_a_long_enough_name")};
    more.unknownKeys = {{"unknown_key_with_a_long_enough_name", "\"and a long enough value too\""}};
    std::shared_ptr<const AnimationClip> bigger = AnimationClip::build(more);
    EXPECT_GE(bigger->memoryBytes(), clip->memoryBytes() + sizeof(ClipEventData) + more.events[0].function.size() +
                                     more.unknownKeys[0].first.size() + more.unknownKeys[0].second.size());
}

TEST_F(ClipMemoryTest, RigGroupsFollowTimesAndEasings){
    ClipData data;
    const std::vector<float> a = {0.0f, 1.0f, 2.0f};
    const std::vector<float> b = {3.0f, 2.0f, 1.0f};
    data.tracks.push_back(track("Hips", ClipChannel::Position, {key(0, a), key(1, b)}));
    data.tracks.push_back(track("Hips", ClipChannel::Scale, {key(0, a), key(1, b)}));
    // the same times, so the same pointer: its easings still make it another search
    data.tracks.push_back(track("Hips/Spine", ClipChannel::Position, {key(0, a, EaseType::QUAD_OUT), key(1, b)}));
    data.tracks.push_back(track("Hips/Spine/Head", ClipChannel::Position, {key(0, a, EaseType::STEP), key(1, b)}));
    data.tracks.push_back(track("Hips/Leg", ClipChannel::Position, {key(0, a), key(0.5f, b)}));
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    ASSERT_EQ(clip->getTracks()[1].times, clip->getTracks()[0].times);

    anim::AnimatorRig rig;
    anim::AnimatorRestMap rest;
    rig.build(&scene, root, {"Hips", "Hips/Spine", "Hips/Spine/Head", "Hips/Leg"}, {}, rest);
    anim::RigClipBinding binding;
    binding.bind(clip, rig);
    EXPECT_EQ(binding.groups.size(), 4u);
    EXPECT_EQ(binding.boundCount, 5u);
}

TEST_F(ClipMemoryTest, ThePoolReportsItsClips){
    const uint64_t before = AnimationClipPool::loadedBytes();
    ASSERT_EQ(before, 0u) << "the fixture cleared the pool";
    const std::string a = clip("a", slide("Hips", 1.0f));
    const std::string b = clip("b", walkShape());

    std::shared_ptr<const AnimationClip> first = AnimationClipPool::getOrLoad(a);
    ASSERT_TRUE(first);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), first->memoryBytes());
    EXPECT_EQ(recordedBytes(), first->memoryBytes()) << "visible in Jobs & Memory";

    std::shared_ptr<const AnimationClip> second = AnimationClipPool::getOrLoad(b);
    ASSERT_TRUE(second);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), first->memoryBytes() + second->memoryBytes());
    EXPECT_EQ(AnimationClipPool::getOrLoad(a), first);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), first->memoryBytes() + second->memoryBytes()) << "a hit adds nothing";

    AnimationClipPool::invalidate(a);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), second->memoryBytes());

    AnimationClipPool::releaseUnused(b);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), second->memoryBytes()) << "still referenced";
    second.reset();
    AnimationClipPool::releaseUnused(b);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), 0u);

    ASSERT_TRUE(AnimationClipPool::getOrLoad(a));
    ASSERT_TRUE(AnimationClipPool::getOrLoad(b));
    EXPECT_GT(AnimationClipPool::loadedBytes(), 0u);
    AnimationClipPool::clearUnused();
    EXPECT_EQ(AnimationClipPool::loadedBytes(), 0u);

    ASSERT_TRUE(AnimationClipPool::getOrLoad(a));
    AnimationClipPool::clear();
    EXPECT_EQ(AnimationClipPool::loadedBytes(), 0u);
}

TEST_F(ClipMemoryTest, AnInvalidatedLoadIsNotCounted){
    const std::string a = clip("a", slide("Hips", 1.0f));
    AnimationClipPoolTestHooks hooks;
    hooks.beforeInsert = [&](const std::string&){ AnimationClipPool::invalidate(a); };
    setAnimationClipPoolTestHooks(hooks);
    std::shared_ptr<const AnimationClip> loaded = AnimationClipPool::getOrLoad(a);
    setAnimationClipPoolTestHooks({});
    ASSERT_TRUE(loaded);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), 0u) << "served, never cached";
}

TEST_F(ClipMemoryTest, AModelsSubClipsAreCountedAndDropped){
    AnimationClipPoolTestHooks hooks;
    hooks.listModelClips = [](const std::string&, std::vector<anim::ModelClipInfo>& out, std::string*){
        anim::ModelClipInfo walk;
        walk.name = "Walk";
        walk.index = 0;
        anim::ModelClipInfo run;
        run.name = "Run";
        run.index = 1;
        out = {walk, run};
        return true;
    };
    hooks.buildSubClip = [](const std::string&, ClipData& out, std::vector<ClipWarning>&, std::string*){
        out = walkShape();
        return true;
    };
    setAnimationClipPoolTestHooks(hooks);
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad("models/hero.glb#Walk");
    std::shared_ptr<const AnimationClip> run = AnimationClipPool::getOrLoad("models/hero.glb#Run");
    ASSERT_TRUE(walk);
    ASSERT_TRUE(run);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), walk->memoryBytes() + run->memoryBytes());
    AnimationClipPool::invalidateModel("models/hero.glb");
    EXPECT_EQ(AnimationClipPool::loadedBytes(), 0u);
    setAnimationClipPoolTestHooks({});
}

TEST_F(ClipMemoryTest, SharedTimesSurviveAHotReload){
    animtest::writeAsset(prefix + "reloaded.anim", clipFileText(1.0f));
    const std::string path = prefix + "reloaded.anim";
    animator(controllerFile("c", controller({}, {state("Play", path)})));
    step(0.0);
    step(0.5);
    ASSERT_TRUE(instance().bound);
    EXPECT_NEAR(position(hips).x, 0.5f, 1e-5f);

    // a player keeps the old clip through the invalidation, whose ranges stay valid until it lets go
    std::shared_ptr<const AnimationClip> old = AnimationClipPool::getOrLoad(path);
    ASSERT_TRUE(old);
    const AnimationClip::Track& oldTrack = old->getTracks()[0];
    ASSERT_EQ(oldTrack.times, old->getTracks()[1].times);
    const uint64_t oldBytes = old->memoryBytes();

    animtest::writeAsset(path, clipFileText(3.0f));
    AnimationClipPool::invalidate(path);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), 0u);
    anim::TrackSample sample;
    ASSERT_TRUE(anim::sampleTrack(oldTrack, 0.5f, sample));
    EXPECT_NEAR(sample.vector.x, 0.5f, 1e-5f);

    step(0.0);
    EXPECT_NEAR(position(hips).x, 1.5f, 1e-5f) << "the Animator rebound to the new clip";
    std::shared_ptr<const AnimationClip> fresh = AnimationClipPool::getOrLoad(path);
    ASSERT_TRUE(fresh);
    EXPECT_NE(fresh, old);
    EXPECT_EQ(fresh->getTracks()[0].times, fresh->getTracks()[1].times);
    EXPECT_NE(fresh->getTracks()[0].times, oldTrack.times);
    EXPECT_EQ(AnimationClipPool::loadedBytes(), fresh->memoryBytes());
    EXPECT_EQ(fresh->memoryBytes(), oldBytes) << "the same shape";
}
