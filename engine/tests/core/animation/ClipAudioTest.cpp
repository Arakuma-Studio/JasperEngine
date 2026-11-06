// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Audio cues of animation clips (docs/specs/S40-timeline-audio.md AC1, AC2): the `.anim` "audio"
// array read, checked and written as version 3, clips without cues unchanged, and cues crossed by
// exactly the rules clip events cross by.

#include <gtest/gtest.h>

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipAudio.h"
#include "animation/KeyframeSampling.h"

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // R1's example
    const char* const kExample = R"({
  "format": "jasperengine.anim",
  "version": 3,
  "frame_rate": 30,
  "wrap": "loop",
  "tracks": [],
  "events": [],
  "audio": [
    {"time": 0.25, "sound": {"guid": "0123456789abcdef0123456789abcdef", "path": "sounds/step.soundevent"}, "path": "Body/LeftFoot",
     "spatial": true, "volume_db": -2, "pitch_semitones": 0, "group": "SFX", "stop": "none", "name": "Left step"},
    {"time": 0.0, "source": "Body/Engine", "stop": "state_exit"},
    {"time": 1.0, "snapshot": "Build", "seconds": 0.5}
  ]
}
)";

    bool hasWarning(const std::vector<ClipWarning>& warnings, const std::string& path, const std::string& text){
        for (const ClipWarning& warning : warnings){
            if (warning.path == path && warning.message.find(text) != std::string::npos) return true;
        }
        return false;
    }

    std::string clipWith(const std::string& audio, int version = 3){
        return std::string("{\"format\": \"jasperengine.anim\", \"version\": ") + std::to_string(version) +
               ", \"tracks\": [], \"events\": [], \"audio\": " + audio + "}";
    }

}

TEST(ClipAudio, RoundTripsTheSpecExampleAsVersionThree){
    anim::ClipParseResult parsed = anim::parseClip(kExample);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    EXPECT_TRUE(parsed.warnings.empty()) << parsed.warnings.front().path << ": " << parsed.warnings.front().message;
    const ClipData& data = *parsed.data;
    ASSERT_EQ(data.audio.size(), 3u);
    // sorted by time
    EXPECT_EQ(data.audio[0].kind, ClipCueKind::Source);
    EXPECT_EQ(data.audio[0].path, "Body/Engine");
    EXPECT_EQ(data.audio[0].stop, ClipCueStop::StateExit);
    EXPECT_EQ(data.audio[1].kind, ClipCueKind::Sound);
    EXPECT_EQ(data.audio[1].sound.path, "sounds/step.soundevent");
    EXPECT_EQ(data.audio[1].sound.guid.str(), "0123456789abcdef0123456789abcdef");
    EXPECT_TRUE(data.audio[1].spatial);
    EXPECT_EQ(data.audio[1].path, "Body/LeftFoot");
    EXPECT_FLOAT_EQ(data.audio[1].volumeDb, -2.0f);
    EXPECT_EQ(data.audio[1].group, "SFX");
    EXPECT_EQ(data.audio[1].name, "Left step");
    EXPECT_EQ(data.audio[2].kind, ClipCueKind::Snapshot);
    EXPECT_EQ(data.audio[2].snapshot, "Build");
    EXPECT_FLOAT_EQ(data.audio[2].seconds, 0.5f);
    EXPECT_FALSE(data.audio[2].unscaled.has_value());

    const std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("\"version\": 3,"), std::string::npos) << text;
    EXPECT_NE(text.find("\"audio\": ["), std::string::npos);
    EXPECT_LT(text.find("\"events\""), text.find("\"audio\"")) << "audio follows events";
    // canonical: a fixed point
    anim::ClipParseResult again = anim::parseClip(text);
    ASSERT_TRUE(again.ok());
    EXPECT_TRUE(again.warnings.empty());
    EXPECT_EQ(again.data->audio, data.audio);
    EXPECT_EQ(anim::writeClip(*again.data), text);
    // the runtime clip has them, and a cue's time extends the clip like an event's
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    ASSERT_EQ(clip->getAudioCues().size(), 3u);
    EXPECT_FLOAT_EQ(clip->getLength(), 1.0f);
    EXPECT_EQ(clip->toData().audio, data.audio);
}

TEST(ClipAudio, ClipsWithoutCuesKeepTheirVersionByteForByte){
    ClipData data;
    data.frameRate = 30.0f;
    ClipTrackData track;
    track.path = "Body";
    track.channel = ClipChannel::Position;
    track.keys.push_back({0.0f, {0.0f, 0.0f, 0.0f}});
    track.keys.push_back({1.0f, {1.0f, 0.0f, 0.0f}});
    data.tracks.push_back(track);
    const std::string version1 = anim::writeClip(data);
    EXPECT_NE(version1.find("\"version\": 1,"), std::string::npos);
    EXPECT_EQ(version1.find("\"audio\""), std::string::npos) << "no audio key without cues";
    anim::ClipParseResult parsed = anim::parseClip(version1);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(anim::writeClip(*parsed.data), version1);

    // a cue makes it version 3; taking it away gives the version 1 text back
    ClipAudioCue cue;
    cue.time = 0.5f;
    cue.kind = ClipCueKind::Snapshot;
    cue.snapshot = "Build";
    data.audio.push_back(cue);
    EXPECT_NE(anim::writeClip(data).find("\"version\": 3,"), std::string::npos);
    data.audio.clear();
    EXPECT_EQ(anim::writeClip(data), version1);
}

TEST(ClipAudio, DropsInvalidCuesWithAWarningEach){
    anim::ClipParseResult parsed = anim::parseClip(clipWith(R"([
        {"time": 0.1, "sound": "a.wav", "snapshot": "Build"},
        {"time": 0.2, "sound": {"path": ""}},
        {"time": 0.3, "source": "Body//Foot"},
        {"time": 0.4, "snapshot": ""},
        {"sound": "a.wav"},
        {"time": -1, "sound": "b.wav", "volume_db": 40, "pitch_semitones": -30, "stop": "later", "colour": 1},
        "not a cue"
    ])"));
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    const std::vector<ClipWarning>& w = parsed.warnings;
    EXPECT_TRUE(hasWarning(w, "audio[0]", "exactly one of")) << "two kinds in one cue";
    EXPECT_TRUE(hasWarning(w, "audio[1]", "needs a sound"));
    EXPECT_TRUE(hasWarning(w, "audio[2]", "is not a binding path"));
    EXPECT_TRUE(hasWarning(w, "audio[3]", "needs a snapshot name"));
    EXPECT_TRUE(hasWarning(w, "audio[4].time", "expected a time"));
    EXPECT_TRUE(hasWarning(w, "audio[5].time", "negative time"));
    EXPECT_TRUE(hasWarning(w, "audio[5].volume_db", "outside"));
    EXPECT_TRUE(hasWarning(w, "audio[5].pitch_semitones", "outside"));
    EXPECT_TRUE(hasWarning(w, "audio[5].stop", "expected none or state_exit"));
    EXPECT_TRUE(hasWarning(w, "audio[5].colour", "unknown key"));
    EXPECT_TRUE(hasWarning(w, "audio[6]", "expected {"));
    ASSERT_EQ(parsed.data->audio.size(), 1u) << "only the repairable cue stays";
    const ClipAudioCue& kept = parsed.data->audio[0];
    EXPECT_EQ(kept.time, 0.0f);
    EXPECT_EQ(kept.sound.path, "b.wav");
    EXPECT_FLOAT_EQ(kept.volumeDb, 12.0f);
    EXPECT_FLOAT_EQ(kept.pitchSemitones, -24.0f);
    EXPECT_EQ(kept.stop, ClipCueStop::None);
}

TEST(ClipAudio, AnOlderVersionWithCuesIsSavedAsVersionThree){
    anim::ClipParseResult parsed = anim::parseClip(clipWith(R"([{"time": 0.5, "snapshot": "Build"}])", 1));
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(hasWarning(parsed.warnings, "version", "saved as version 3"));
    EXPECT_NE(anim::writeClip(*parsed.data).find("\"version\": 3,"), std::string::npos);
}

TEST(ClipAudio, ANewerVersionIsRefusedAsAnOlderEngineRefusesVersionThree){
    // what the engines before S40 (ANIM_VERSION 2) answer a version 3 file: refused, never read
    // with its sounds dropped
    const std::string olderEngineAnswer = "unsupported animation clip version 3 (this engine reads versions up to 2)";
    EXPECT_EQ(anim::ANIM_VERSION, 3);
    anim::ClipParseResult refused = anim::parseClip(clipWith("[]", 4));
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.error.message, "unsupported animation clip version 4 (this engine reads versions up to 3)");
    // the same sentence, one version on
    std::string expected = olderEngineAnswer;
    expected.replace(expected.find("version 3"), 9, "version 4");
    expected.replace(expected.find("up to 2"), 7, "up to 3");
    EXPECT_EQ(refused.error.message, expected);
}

TEST(ClipAudio, CrossingMatchesEventsAtTheSameTimes){
    // events and cues at the same times: every crossing of the event table gives the same order
    ClipData data;
    data.length = 1.0f;
    for (float time : {0.0f, 0.25f, 0.5f, 1.0f}){
        ClipEventData event;
        event.time = time;
        event.function = "at" + std::to_string((int)(time * 100));
        data.events.push_back(event);
        ClipAudioCue cue;
        cue.time = time;
        cue.kind = ClipCueKind::Snapshot;
        cue.snapshot = event.function;
        data.audio.push_back(cue);
    }
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    ASSERT_EQ(clip->getAudioCues().size(), 4u);

    struct Crossing { ClipWrapMode wrap; float from; float to; bool start; };
    const Crossing table[] = {
        {ClipWrapMode::Once, 0.0f, 0.3f, false}, {ClipWrapMode::Once, 0.0f, 0.3f, true}, {ClipWrapMode::Once, 0.25f, 0.5f, false},
        {ClipWrapMode::Once, 0.6f, 5.0f, false}, {ClipWrapMode::Once, 0.3f, 0.3f, false}, {ClipWrapMode::Once, 1.0f, 0.2f, false},
        {ClipWrapMode::Once, 1.0f, 0.2f, true}, {ClipWrapMode::Once, 0.5f, -1.0f, false}, {ClipWrapMode::ClampForever, 1.0f, 1.5f, false},
        {ClipWrapMode::Loop, 0.9f, 1.3f, false}, {ClipWrapMode::Loop, 0.6f, 1.0f, false}, {ClipWrapMode::Loop, 1.0f, 1.2f, false},
        {ClipWrapMode::Loop, 0.0f, 0.1f, true}, {ClipWrapMode::Loop, 1.3f, 0.9f, false}, {ClipWrapMode::Loop, 1.0f, 0.9f, true},
        {ClipWrapMode::Loop, 0.3f, 3.6f, false}, {ClipWrapMode::Loop, 3.6f, 0.3f, false}, {ClipWrapMode::PingPong, 0.9f, 1.6f, false},
        {ClipWrapMode::PingPong, 1.6f, 2.1f, false}, {ClipWrapMode::PingPong, 0.1f, 5.0f, false}, {ClipWrapMode::PingPong, 0.0f, 0.2f, true},
    };
    for (const Crossing& crossing : table){
        std::vector<anim::CrossedEvent> events;
        std::vector<anim::CrossedEvent> cues;
        anim::collectCrossedEvents(*clip, crossing.wrap, crossing.from, crossing.to, crossing.start, events);
        anim::collectCrossedCues(*clip, crossing.wrap, crossing.from, crossing.to, crossing.start, cues);
        ASSERT_EQ(events.size(), cues.size()) << anim::wrapName(crossing.wrap) << " " << crossing.from << " -> " << crossing.to;
        for (size_t i = 0; i < events.size(); i++){
            EXPECT_EQ(clip->getEvents()[events[i].index].function, clip->getAudioCues()[cues[i].index].snapshot);
            EXPECT_EQ(events[i].position, cues[i].position);
        }
    }

    // a clip without cues crosses none
    ClipData plain;
    plain.length = 1.0f;
    std::shared_ptr<const AnimationClip> none = AnimationClip::build(plain);
    std::vector<anim::CrossedEvent> crossed;
    anim::collectCrossedCues(*none, ClipWrapMode::Loop, 0.0f, 3.0f, true, crossed);
    EXPECT_TRUE(crossed.empty());
}
