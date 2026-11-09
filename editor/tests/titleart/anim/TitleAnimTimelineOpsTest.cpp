// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85 R42, R43, R45: the Animation panel's edits as op batches, and Record's rewrite

#include "titleanim/TitleAnimDataIO.h"
#include "action/Ease.h"
#include "titleart/anim/TitleAnimOps.h"
#include "titleart/anim/TitleAnimPresets.h"
#include "titleart/anim/TitleAnimTimelineOps.h"
#include "titleart/TitleArtOps.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/TitleArtIO.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;
using Json = tt::Json;

namespace {

    ta::TitleTrack track(const std::string& field, std::vector<std::pair<float, float>> keys, const std::string& layer = ""){
        ta::TitleTrack t;
        t.layer = layer;
        t.field = field;
        for (const auto& k : keys){
            ta::TitleKey key;
            key.key.time = k.first;
            key.key.value = {k.second};
            t.keys.push_back(key);
        }
        return t;
    }

    ta::TitleAnimation animationWith(std::vector<ta::TitleTrack> tracks){
        ta::TitleAnimation a;
        a.name = "A";
        a.tracks = std::move(tracks);
        return a;
    }

    double keyTime(const Json& op, size_t index){
        const Json& key = op.at("keys").at(index);
        return key.is_array() ? key.at(0).get<double>() : key.at("time").get<double>();
    }

}

TEST(TitleAnimTimelineOpsTest, SnapAndMove){
    EXPECT_DOUBLE_EQ(snapTitleKeyTime(0.51, 10), 0.5);
    EXPECT_DOUBLE_EQ(snapTitleKeyTime(0.51, 0), 0.51);
    const ta::TitleAnimation a = animationWith({track("opacity", {{0.0f, 0.0f}, {1.0f, 1.0f}}), track("scale", {{0.0f, 1.0f}, {1.0f, 2.0f}})});
    // moving one key snaps it; the other track is untouched
    const Json ops = titleMoveKeysOps(a, {{0, 1}}, 0.42, 30);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0]["field"], "opacity");
    EXPECT_NEAR(keyTime(ops[0], 1), 43.0 / 30.0, 1e-4) << "1.42 s is frame 42.6: the nearest frame";
    // a move onto another key's time replaces it (Unity's rule); the moved value wins
    const Json merged = titleMoveKeysOps(a, {{0, 0}}, 1.0, 30);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0]["keys"].size(), 1u) << merged[0].dump();
    // no movement, no op
    EXPECT_TRUE(titleMoveKeysOps(a, {{0, 1}}, 0.0, 30).empty());
    // before 0 clamps
    EXPECT_NEAR(keyTime(titleMoveKeysOps(a, {{0, 1}}, -5.0, 30)[0], 0), 0.0, 1e-4);
}

TEST(TitleAnimTimelineOpsTest, ScaleAboutPivotDeleteDuplicatePaste){
    const ta::TitleAnimation a = animationWith({track("opacity", {{0.0f, 0.0f}, {1.0f, 1.0f}, {2.0f, 0.5f}})});
    const Json scaled = titleScaleKeysOps(a, {{0, 1}, {0, 2}}, 0.0, 0.5, 0);
    ASSERT_EQ(scaled.size(), 1u);
    EXPECT_NEAR(keyTime(scaled[0], 1), 0.5, 1e-4);
    EXPECT_NEAR(keyTime(scaled[0], 2), 1.0, 1e-4);

    // deleting every key removes the track; deleting some rewrites it
    EXPECT_EQ(titleDeleteKeysOps(a, {{0, 0}, {0, 1}, {0, 2}})[0]["op"], "remove_track");
    const Json some = titleDeleteKeysOps(a, {{0, 1}});
    EXPECT_EQ(some[0]["op"], "set_track");
    EXPECT_EQ(some[0]["keys"].size(), 2u);

    const Json duplicated = titleDuplicateKeysOps(a, {{0, 2}}, 0.5);
    EXPECT_EQ(duplicated[0]["keys"].size(), 4u);
    EXPECT_NEAR(keyTime(duplicated[0], 3), 2.5, 1e-4);

    const std::vector<TitleKeyClipboard> clip = titleCopyKeys(a, {{0, 1}, {0, 2}});
    ASSERT_EQ(clip.size(), 1u);
    EXPECT_NEAR(clip[0].keys[0].key.time, 0.0f, 1e-5f);
    EXPECT_NEAR(clip[0].keys[1].key.time, 1.0f, 1e-5f);
    const Json pasted = titlePasteKeysOps(a, clip, 3.0);
    ASSERT_EQ(pasted.size(), 1u);
    EXPECT_EQ(pasted[0]["keys"].size(), 5u);
    EXPECT_NEAR(keyTime(pasted[0], 3), 3.0, 1e-4);
    EXPECT_NEAR(keyTime(pasted[0], 4), 4.0, 1e-4);
}

TEST(TitleAnimTimelineOpsTest, ConvertEasesStaysWithinTolerance){
    ta::TitleAnimation a = animationWith({track("opacity", {{0.0f, 0.0f}, {1.0f, 10.0f}})});
    a.tracks[0].keys[0].key.ease = EaseType::SINE_IN_OUT;
    const Json ops = titleConvertEasesOps(a, 0);
    ASSERT_EQ(ops.size(), 1u);
    // R43: the converted keys reproduce the ease within 0.005 of the range
    const Json& keys = ops[0]["keys"];
    ASSERT_GE(keys.size(), 3u);
    for (const Json& key : keys){
        ta::TitleKey k;
        std::string error;
        ASSERT_TRUE(ta::decodeKey(key, k, error)) << error;
        const float t = k.key.time;
        const float expected = 10.0f * Ease::getFunction(EaseType::SINE_IN_OUT)(t);
        EXPECT_NEAR(k.key.value[0], expected, 10.0f * 0.005f) << "at " << t;
    }
    // a track of plain linear keys has nothing to convert
    EXPECT_TRUE(titleConvertEasesOps(animationWith({track("opacity", {{0.0f, 0.0f}, {1.0f, 1.0f}})}), 0).empty());
}

TEST(TitleAnimTimelineOpsTest, RecordKeysAtThePlayheadFromRest){
    registerTitleAnimOps();
    registerTitleAnimPresetOp();
    tt::TitleArtParseResult parsed = tt::parseTitleArtJson(Json{{"format", tt::TITLE_ART_FORMAT}, {"version", tt::TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"},
        {"canvas", {{"width", 512}, {"height", 256}}},
        {"layers", Json::array({Json{{"id", "00000001"}, {"type", "shape"}, {"name", "Plate"}, {"opacity", 0.5}}})}});
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    const tt::TitleArtDoc& doc = parsed.doc;
    ta::TitleAnimation a;
    a.name = "A";

    const Json ops = Json::array({Json{{"op", "set"}, {"layer", "00000001"}, {"values", {{"opacity", 0.25}, {"name", "Back"}}}}});
    const TitleRecordResult r = titleRecordRewrite(doc, a, 0.5, ops);
    // 3 ops: the key at 0 with the rest value, the key at the playhead, and the unkeyed rest
    ASSERT_EQ(r.ops.size(), 3u) << r.ops.dump();
    EXPECT_EQ(r.ops[0]["op"], "set_key");
    EXPECT_DOUBLE_EQ(r.ops[0]["time"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(r.ops[0]["value"].get<double>(), 0.5);
    EXPECT_EQ(r.ops[1]["op"], "set_key");
    EXPECT_DOUBLE_EQ(r.ops[1]["time"].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(r.ops[1]["value"].get<double>(), 0.25);
    EXPECT_EQ(r.ops[2]["op"], "set");
    EXPECT_EQ(r.ops[2]["values"].size(), 1u);
    ASSERT_EQ(r.notKeyed.size(), 1u);
    EXPECT_NE(r.stepName.find("Record Key"), std::string::npos);

    // with a track already, no zero key
    ta::TitleAnimation b = animationWith({track("opacity", {{0.0f, 1.0f}}, "00000001")});
    const TitleRecordResult second = titleRecordRewrite(doc, b, 0.5, Json::array({Json{{"op", "set"}, {"layer", "00000001"}, {"values", {{"opacity", 0.25}}}}}));
    EXPECT_EQ(second.ops.size(), 1u);
}
