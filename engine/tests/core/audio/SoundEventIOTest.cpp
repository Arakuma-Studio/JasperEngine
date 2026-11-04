// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .soundevent format and the pick logic (docs/specs/S37-sound-events.md R1, R5, R8; AC1, AC3):
// a round trip in key order with defaults left out, unknown keys kept, every validation error with
// its JSON path, a newer version refused; the generator against values from its Python twin; the
// bag, the weights and the sequence.

#include <gtest/gtest.h>

#include "audio/SoundEventIO.h"
#include "audio/SoundEventLibrary.h"

#include "json.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // the example of R1
    const char* exampleEvent = R"({
  "format": "jasperengine.soundevent",
  "version": 1,
  "mode": "shuffle",
  "clips": [
    {"clip": {"guid": "6f1c2a9e0b3d4c5e8f7a6b5c4d3e2f10", "path": "sounds/step_1.wav"},
     "weight": 2, "volume_db": -1.5, "pitch_semitones": 0, "delay_ms": 0},
    {"clip": {"guid": "7f1c2a9e0b3d4c5e8f7a6b5c4d3e2f11", "path": "sounds/step_2.wav"}}
  ],
  "avoid_repeat": true,
  "pitch_semitones": [-1, 1],
  "volume_db": [-3, 0],
  "delay_ms": [0, 20],
  "output": {"id": "5fx00001", "name": "SFX"},
  "max_instances": 4,
  "steal": "oldest",
  "cooldown_ms": 50,
  "priority": 128,
  "loop": false,
  "spatial": {"min_distance": 1, "max_distance": 30, "attenuation": "linear_distance", "rolloff": 1, "doppler": 1},
  "type": "footstep"
})";

    SoundEventDef parseOk(const std::string& text) {
        SoundEventDef def;
        std::string error;
        EXPECT_TRUE(SoundEventIO::parse(text, def, &error)) << error;
        return def;
    }

    std::string parseError(const std::string& text) {
        SoundEventDef def;
        std::string error;
        EXPECT_FALSE(SoundEventIO::parse(text, def, &error));
        return error;
    }

    SoundEventDef clips(int count, SoundEventMode mode = SoundEventMode::Random) {
        SoundEventDef def;
        def.mode = mode;
        for (int i = 0; i < count; i++) {
            SoundEventClip clip;
            clip.clip.path = "c" + std::to_string(i) + ".wav";
            def.clips.push_back(clip);
        }
        return def;
    }

    bool hasProblem(const SoundEventDef& def, const std::string& prefix) {
        for (const std::string& problem : SoundEventIO::validate(def)) {
            if (problem.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }

    std::vector<std::string> topKeys(const std::string& text) {
        std::vector<std::string> keys;
        const nlohmann::ordered_json root = nlohmann::ordered_json::parse(text);
        for (auto it = root.begin(); it != root.end(); ++it) keys.push_back(it.key());
        return keys;
    }

}

TEST(SoundEventIO, ExampleReads) {
    const SoundEventDef def = parseOk(exampleEvent);
    EXPECT_EQ(def.mode, SoundEventMode::Shuffle);
    ASSERT_EQ(def.clips.size(), 2u);
    EXPECT_EQ(def.clips[0].clip.path, "sounds/step_1.wav");
    EXPECT_EQ(def.clips[0].clip.guid.str(), "6f1c2a9e0b3d4c5e8f7a6b5c4d3e2f10");
    EXPECT_FLOAT_EQ(def.clips[0].weight, 2.0f);
    EXPECT_FLOAT_EQ(def.clips[0].volumeDb, -1.5f);
    EXPECT_FLOAT_EQ(def.pitchMin, -1.0f);
    EXPECT_FLOAT_EQ(def.pitchMax, 1.0f);
    EXPECT_FLOAT_EQ(def.volumeMin, -3.0f);
    EXPECT_FLOAT_EQ(def.delayMax, 20.0f);
    EXPECT_EQ(def.outputGroupId, "5fx00001");
    EXPECT_EQ(def.outputGroupName, "SFX");
    EXPECT_EQ(def.maxInstances, 4);
    EXPECT_EQ(def.steal, AudioStealPolicy::Oldest);
    EXPECT_FLOAT_EQ(def.cooldownMs, 50.0f);
    ASSERT_TRUE(def.spatial.has_value());
    EXPECT_FLOAT_EQ(def.spatial->maxDistance, 30.0f);
    EXPECT_EQ(def.spatial->attenuation, AudioSourceAttenuation::LINEAR_DISTANCE);
    EXPECT_EQ(def.type, "footstep");
    EXPECT_TRUE(SoundEventIO::validate(def).empty());
}

TEST(SoundEventIO, RoundTripInKeyOrderDefaultsOmitted) {
    const SoundEventDef def = parseOk(exampleEvent);
    const std::string written = SoundEventIO::write(def);
    // avoid_repeat, steal, priority and loop are at their defaults
    const std::vector<std::string> expected = {"format", "version", "mode", "clips", "pitch_semitones", "volume_db", "delay_ms",
                                               "output", "max_instances", "cooldown_ms", "spatial", "type"};
    EXPECT_EQ(topKeys(written), expected);
    EXPECT_EQ(parseOk(written), def);
    // writing again changes nothing
    EXPECT_EQ(SoundEventIO::write(parseOk(written)), written);
    // the clip's defaults are left out too, LF and two spaces
    const nlohmann::json root = nlohmann::json::parse(written);
    EXPECT_FALSE(root["clips"][0].contains("pitch_semitones"));
    EXPECT_FALSE(root["clips"][0].contains("delay_ms"));
    EXPECT_FALSE(root["clips"][1].contains("weight"));
    EXPECT_EQ(written.find('\r'), std::string::npos);
    EXPECT_NE(written.find("\n  \"version\": 1,\n"), std::string::npos);
    EXPECT_EQ(written.back(), '\n');
}

TEST(SoundEventIO, EveryKeyInOrder) {
    SoundEventDef def = clips(1);
    def.avoidRepeat = false;
    def.pitchMax = 1.0f;
    def.volumeMin = -1.0f;
    def.delayMax = 5.0f;
    def.outputGroupName = "SFX";
    def.maxInstances = 2;
    def.steal = AudioStealPolicy::Quietest;
    def.cooldownMs = 10.0f;
    def.priority = 10;
    def.loop = true;
    def.spatial = SoundEventSpatial();
    def.type = "impact";
    const std::vector<std::string> expected = {"format", "version", "mode", "clips", "avoid_repeat", "pitch_semitones", "volume_db", "delay_ms",
                                               "output", "max_instances", "steal", "cooldown_ms", "priority", "loop", "spatial", "type"};
    EXPECT_EQ(topKeys(SoundEventIO::write(def)), expected);
}

TEST(SoundEventIO, ShortestFloatsWritten) {
    SoundEventDef def = clips(1);
    def.clips[0].volumeDb = 0.1f;
    const std::string written = SoundEventIO::write(def);
    EXPECT_NE(written.find("\"volume_db\": 0.1\n"), std::string::npos) << written;
}

TEST(SoundEventIO, UnknownKeysKept) {
    const std::string written = SoundEventIO::write(parseOk(R"({
        "format": "jasperengine.soundevent", "version": 1, "mode": "sequence", "future": {"a": [1, 2]},
        "clips": [{"clip": {"path": "a.wav"}, "loops": 3}], "other": "x"})"));
    const nlohmann::json root = nlohmann::json::parse(written);
    EXPECT_EQ(root["future"]["a"][1], 2);
    EXPECT_EQ(root["other"], "x");
    EXPECT_EQ(root["clips"][0]["loops"], 3);
    const std::vector<std::string> keys = topKeys(written);
    EXPECT_EQ(keys[keys.size() - 2], "future");
    EXPECT_EQ(keys.back(), "other");
}

TEST(SoundEventIO, ParseErrorsNameTheirPath) {
    EXPECT_NE(parseError("{").find("not valid JSON"), std::string::npos);
    EXPECT_EQ(parseError("[]").rfind("$:", 0), 0u);
    EXPECT_EQ(parseError(R"({"format": "x"})").rfind("format:", 0), 0u);
    const std::string head = R"("format": "jasperengine.soundevent", "version": 1)";
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "loud", "clips": []})").rfind("mode:", 0), 0u);
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "random"})").rfind("clips:", 0), 0u);
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "random", "clips": [{"clip": {"path": "a"}}, {}]})").rfind("clips[1].clip:", 0), 0u);
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "random", "clips": [{"clip": {"path": "a", "guid": "zz"}}]})").rfind("clips[0].clip.guid:", 0), 0u);
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "random", "clips": [{"clip": {"path": "a"}, "weight": "x"}]})").rfind("clips[0].weight:", 0), 0u);
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "random", "clips": [], "pitch_semitones": [1]})").rfind("pitch_semitones:", 0), 0u);
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "random", "clips": [], "output": 1})").rfind("output:", 0), 0u);
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "random", "clips": [], "steal": "all"})").rfind("steal:", 0), 0u);
    EXPECT_EQ(parseError("{" + head + R"(, "mode": "random", "clips": [], "spatial": {"attenuation": "x"}})").rfind("spatial.attenuation:", 0), 0u);
}

TEST(SoundEventIO, NewerVersionRefused) {
    const std::string error = parseError(R"({"format": "jasperengine.soundevent", "version": 2, "mode": "random", "clips": []})");
    EXPECT_EQ(error.rfind("version:", 0), 0u);
    EXPECT_NE(error.find("(1)"), std::string::npos) << "names the version it reads: " << error;
}

TEST(SoundEventIO, ValidationNamesEveryPath) {
    EXPECT_TRUE(SoundEventIO::validate(clips(1)).empty());
    EXPECT_TRUE(hasProblem(clips(0), "clips:"));
    EXPECT_TRUE(hasProblem(clips(65), "clips:"));
    EXPECT_TRUE(SoundEventIO::validate(clips(64)).empty());

    auto clipProblem = [](auto set, const std::string& path) {
        SoundEventDef def = clips(2);
        set(def.clips[1]);
        return hasProblem(def, path);
    };
    EXPECT_TRUE(clipProblem([](SoundEventClip& c) { c.weight = -1; }, "clips[1].weight"));
    EXPECT_TRUE(clipProblem([](SoundEventClip& c) { c.weight = 1001; }, "clips[1].weight"));
    EXPECT_TRUE(clipProblem([](SoundEventClip& c) { c.volumeDb = -61; }, "clips[1].volume_db"));
    EXPECT_TRUE(clipProblem([](SoundEventClip& c) { c.volumeDb = 13; }, "clips[1].volume_db"));
    EXPECT_TRUE(clipProblem([](SoundEventClip& c) { c.pitchSemitones = 25; }, "clips[1].pitch_semitones"));
    EXPECT_TRUE(clipProblem([](SoundEventClip& c) { c.delayMs = 2001; }, "clips[1].delay_ms"));
    EXPECT_TRUE(clipProblem([](SoundEventClip& c) { c.clip = AssetRef(); }, "clips[1].clip"));

    SoundEventDef weightless = clips(2);
    weightless.clips[0].weight = 0;
    weightless.clips[1].weight = 0;
    EXPECT_TRUE(hasProblem(weightless, "clips:"));
    weightless.mode = SoundEventMode::Sequence;
    EXPECT_TRUE(SoundEventIO::validate(weightless).empty()) << "weights are random mode's";

    auto problem = [](auto set, const std::string& path) {
        SoundEventDef def = clips(1);
        set(def);
        return hasProblem(def, path);
    };
    EXPECT_TRUE(problem([](SoundEventDef& d) { d.pitchMin = -25; }, "pitch_semitones"));
    EXPECT_TRUE(problem([](SoundEventDef& d) { d.pitchMin = 2; d.pitchMax = 1; }, "pitch_semitones"));
    EXPECT_TRUE(problem([](SoundEventDef& d) { d.volumeMax = 13; }, "volume_db"));
    EXPECT_TRUE(problem([](SoundEventDef& d) { d.delayMin = -1; }, "delay_ms"));
    EXPECT_TRUE(problem([](SoundEventDef& d) { d.maxInstances = 256; }, "max_instances"));
    EXPECT_TRUE(problem([](SoundEventDef& d) { d.cooldownMs = 60001; }, "cooldown_ms"));
    EXPECT_TRUE(problem([](SoundEventDef& d) { d.priority = 257; }, "priority"));
    EXPECT_TRUE(problem([](SoundEventDef& d) { d.type = "bagpipe"; }, "type"));
    EXPECT_FALSE(problem([](SoundEventDef& d) { d.type = "footstep"; }, "type"));

    auto spatial = [](auto set, const std::string& path) {
        SoundEventDef def = clips(1);
        def.spatial = SoundEventSpatial();
        set(*def.spatial);
        return hasProblem(def, path);
    };
    EXPECT_TRUE(spatial([](SoundEventSpatial& s) { s.minDistance = 0; }, "spatial.min_distance"));
    EXPECT_TRUE(spatial([](SoundEventSpatial& s) { s.minDistance = 5; s.maxDistance = 4; }, "spatial.max_distance"));
    EXPECT_TRUE(spatial([](SoundEventSpatial& s) { s.rolloff = 101; }, "spatial.rolloff"));
    EXPECT_TRUE(spatial([](SoundEventSpatial& s) { s.doppler = 11; }, "spatial.doppler"));
}

// The generator's values from tools/mcp/audio_refs.py's twin: the Python reference and the engine
// must agree on every bit, a capture's picks are compared across them
TEST(SoundEventRng, MatchesTheReference) {
    EXPECT_EQ(SoundEventRng::fnv1a64("abc"), 0xe71fa2190541574bull);
    EXPECT_EQ(SoundEventRng::splitmix64(0), 0xe220a8397b1dcdafull);
    SoundEventRng rng(1, "6f1c2a9e0b3d4c5e8f7a6b5c4d3e2f10", 0);
    EXPECT_EQ(rng.next(), 0xa35dc472f7618edaull);
    EXPECT_EQ(rng.next(), 0x0af010002d6ed2dfull);
    EXPECT_DOUBLE_EQ(SoundEventRng(1, "entity:7", 3).uniform(), 0.9441475294889021);
}

TEST(SoundEventPick, ShuffleEachClipOnceABagNoneBackToBack) {
    const SoundEventDef def = clips(4, SoundEventMode::Shuffle);
    for (uint64_t seed = 1; seed < 40; seed++) {
        const std::vector<SoundEventPick> picks = SoundEventLibrary::preview(def, "6f1c2a9e0b3d4c5e8f7a6b5c4d3e2f10", seed, 8);
        std::map<int, int> counts;
        int last = -1;
        for (const SoundEventPick& pick : picks) {
            ASSERT_EQ(pick.clips.size(), 1u);
            EXPECT_NE(pick.clips[0], last) << "seed " << seed;
            last = pick.clips[0];
            counts[last]++;
        }
        for (int clip = 0; clip < 4; clip++) EXPECT_EQ(counts[clip], 2) << "seed " << seed << " clip " << clip;
    }
}

TEST(SoundEventPick, WeightsConvergeWithin3Percent) {
    SoundEventDef def = clips(3);
    def.avoidRepeat = false;
    def.clips[0].weight = 1;
    def.clips[1].weight = 2;
    def.clips[2].weight = 5;
    const std::vector<SoundEventPick> picks = SoundEventLibrary::preview(def, "6f1c2a9e0b3d4c5e8f7a6b5c4d3e2f10", 7, 4000);
    std::map<int, int> counts;
    for (const SoundEventPick& pick : picks) counts[pick.clips[0]]++;
    EXPECT_NEAR(counts[0] / 4000.0, 1.0 / 8.0, 0.03);
    EXPECT_NEAR(counts[1] / 4000.0, 2.0 / 8.0, 0.03);
    EXPECT_NEAR(counts[2] / 4000.0, 5.0 / 8.0, 0.03);
}

TEST(SoundEventPick, RandomAvoidsRepeatByDefault) {
    const std::vector<SoundEventPick> picks = SoundEventLibrary::preview(clips(3), "k", 3, 200);
    for (size_t i = 1; i < picks.size(); i++) EXPECT_NE(picks[i].clips[0], picks[i - 1].clips[0]);
}

TEST(SoundEventPick, ZeroWeightNeverPicked) {
    SoundEventDef def = clips(3);
    def.clips[1].weight = 0;
    for (const SoundEventPick& pick : SoundEventLibrary::preview(def, "k", 3, 200)) EXPECT_NE(pick.clips[0], 1);
}

TEST(SoundEventPick, SequenceAndLayer) {
    const std::vector<SoundEventPick> sequence = SoundEventLibrary::preview(clips(3, SoundEventMode::Sequence), "k", 1, 7);
    for (int i = 0; i < 7; i++) EXPECT_EQ(sequence[i].clips[0], i % 3);
    const std::vector<SoundEventPick> layer = SoundEventLibrary::preview(clips(3, SoundEventMode::Layer), "k", 1, 2);
    EXPECT_EQ(layer[1].clips, (std::vector<int>{0, 1, 2}));
}

TEST(SoundEventPick, DrawsLieInTheirRanges) {
    SoundEventDef def = clips(2);
    def.pitchMin = -2;
    def.pitchMax = 3;
    def.volumeMin = -6;
    def.volumeMax = -1;
    def.delayMin = 5;
    def.delayMax = 9;
    bool variedPitch = false;
    double firstPitch = 0;
    int index = 0;
    for (const SoundEventPick& pick : SoundEventLibrary::preview(def, "k", 11, 200)) {
        EXPECT_GE(pick.pitchSemitones, -2);
        EXPECT_LT(pick.pitchSemitones, 3);
        EXPECT_GE(pick.volumeDb, -6);
        EXPECT_LT(pick.volumeDb, -1);
        EXPECT_GE(pick.delayMs, 5);
        EXPECT_LT(pick.delayMs, 9);
        if (index++ == 0) firstPitch = pick.pitchSemitones;
        variedPitch = variedPitch || pick.pitchSemitones != firstPitch;
    }
    EXPECT_TRUE(variedPitch);
}

TEST(SoundEventPick, SameSeedSamePicks) {
    const SoundEventDef def = clips(5);
    auto list = [&](uint64_t seed) {
        std::vector<int> out;
        for (const SoundEventPick& pick : SoundEventLibrary::preview(def, "k", seed, 32)) out.push_back(pick.clips[0]);
        return out;
    };
    EXPECT_EQ(list(9), list(9));
    EXPECT_NE(list(9), list(10));
}

TEST(SoundEventLibraryRuntime, ReloadWithANewClipListResetsItsBag) {
    SoundEventLibrary::clear();
    const std::string one = SoundEventIO::write(clips(4, SoundEventMode::Sequence));
    const SoundEventAsset* asset = SoundEventLibrary::load("a.soundevent", one);
    ASSERT_NE(asset, nullptr);
    const SoundEventClock clock;
    EXPECT_EQ(SoundEventLibrary::pick(*asset, clock).clips[0], 0);
    EXPECT_EQ(SoundEventLibrary::pick(*asset, clock).clips[0], 1);

    // another value, the same clips: the sequence goes on
    SoundEventDef louder = clips(4, SoundEventMode::Sequence);
    louder.volumeMax = 1.0f;
    asset = SoundEventLibrary::load("a.soundevent", SoundEventIO::write(louder));
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(SoundEventLibrary::pick(*asset, clock).clips[0], 2);

    // another clip list starts it over
    asset = SoundEventLibrary::load("a.soundevent", SoundEventIO::write(clips(3, SoundEventMode::Sequence)));
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(SoundEventLibrary::pick(*asset, clock).clips[0], 0);
    SoundEventLibrary::clear();
}

TEST(SoundEventLibraryRuntime, CooldownComparesSamplesOnTheBoundary) {
    SoundEventLibrary::clear();
    SoundEventDef def = clips(1);
    def.cooldownMs = 50.0f;
    const SoundEventAsset* asset = SoundEventLibrary::load("c.soundevent", SoundEventIO::write(def));
    ASSERT_NE(asset, nullptr);
    SoundEventClock clock;
    clock.samples = true;
    clock.rate = 48000;
    clock.cursor = 1000;
    EXPECT_TRUE(SoundEventLibrary::cooledDown(*asset, clock));
    SoundEventLibrary::pick(*asset, clock);
    clock.cursor = 1000 + 2399;
    EXPECT_FALSE(SoundEventLibrary::cooledDown(*asset, clock));
    clock.cursor = 1000 + 2400;
    EXPECT_TRUE(SoundEventLibrary::cooledDown(*asset, clock));
    SoundEventLibrary::clear();
}

TEST(SoundEventLibraryRuntime, InvalidTextIsRefused) {
    std::string error;
    EXPECT_EQ(SoundEventLibrary::load("x.soundevent", "{}", &error), nullptr);
    EXPECT_FALSE(error.empty());
    SoundEventDef def = clips(1);
    def.priority = 300;
    EXPECT_EQ(SoundEventLibrary::load("x.soundevent", SoundEventIO::write(def), &error), nullptr);
    EXPECT_EQ(error.rfind("priority:", 0), 0u);
    SoundEventLibrary::clear();
}
