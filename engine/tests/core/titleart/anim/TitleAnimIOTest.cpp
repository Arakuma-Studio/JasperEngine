// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85 AC1 (R1-R3, R58, R59), AC5's layout half (R8, R9), AC7 (R11, R12, §6.2, §6.3), AC23's
// document half (R58): the `animation` member's codec inside the `.titleart`, its leniency,
// limits, orphans and unknown members; the stagger units from the text layout; the classes and
// the plan per Render Mode.

#include <gtest/gtest.h>

#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimSampling.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRefs.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/text/TitleTextLayout.h"

#include "../support/TitleTestAssets.h"

#include <random>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;
namespace ta = jasperengine::titleanim;

namespace {

    // §6.1's title: a text layer and a layer whose effects have a phase (Wave Warp) and a seed (Distress)
    TitleArtDoc exampleDocument(){
        Json text = {{"type", "text"}, {"id", "3fa0b2c1"}, {"name", "Title"}, {"text", "WIN"}};
        Json fx = {{"type", "shape"}, {"id", "9b0d4e11"}, {"name", "Plate"},
                   {"effects", Json::array({Json{{"type", "wave_warp"}, {"id", "00000101"}}, Json{{"type", "distress"}, {"id", "00000102"}}})}};
        return documentWith({fx, text});
    }

    const char* EXAMPLE = R"({
  "fps": 30,
  "default": "Intro",
  "animations": [
    {
      "id": "7c1e0a42", "name": "Intro", "role": "intro",
      "duration": 0.8, "wrap": "once", "loop_start": 0, "next": "Idle", "overlay": false, "speed": 1,
      "rerender": "live", "rerender_fps": 30,
      "flipbook": {"fps": 30, "scale": 1, "max_page": 2048, "frame_blend": false},
      "preset_source": {"id": "stamp_in", "params": {"from_scale": 2.6, "shockwave": false}},
      "tracks": [
        {"layer": "", "field": "transform.scale", "relative": true, "pivot": "origin", "mute": false,
         "from_preset": "stamp_in", "keys": [[0, 2.6, "quad_in"], [0.48, 0.94, "sine_out"], [0.8, 1]]},
        {"layer": "", "field": "live.sweep_position", "relative": false, "pivot": "origin", "mute": false,
         "keys": [[0.5, 0, "sine_in_out"], [0.8, 1]]},
        {"layer": "3fa0b2c1", "field": "glyph.offset", "relative": true, "pivot": "baseline", "mute": false,
         "stagger": {"each": 0.04, "order": "forward", "unit": "glyph", "seed": 1, "ease": "linear"},
         "keys": [[0, [0, 60], "back_out"], [0.3, [0, 0]]]},
        {"layer": "9b0d4e11", "field": "effects[0].params.phase", "relative": false, "pivot": "origin", "mute": false,
         "drive": {"from": 0, "to": 10, "cycles": 1, "rate": 0, "wrap": false, "ease": "linear"}},
        {"layer": "9b0d4e11", "field": "effects[1].params.seed", "relative": false, "pivot": "origin", "mute": false,
         "reseed": {"rate": 12, "offset": 0}}
      ],
      "markers": [
        {"time": 0.48, "name": "onImpact", "args": [], "sound": {"path": "sounds/stamp.wav", "guid": "00112233445566778899aabbccddeeff"},
         "volume_db": 0, "group": ""}
      ]
    }
  ]
})";

    TitleArtDoc withAnimation(TitleArtDoc doc, const std::string& member){
        doc.root["animation"] = Json::parse(member);
        TitleArtParseResult r = parseTitleArt(writeTitleArt(doc));
        EXPECT_TRUE(r.ok) << r.error.toString();
        return r.doc;
    }

}

TEST(TitleAnimIOTest, RoundTrip){
    TitleArtDoc doc = withAnimation(exampleDocument(), EXAMPLE);
    const std::string once = writeTitleArt(doc);
    TitleArtParseResult again = parseTitleArt(once);
    ASSERT_TRUE(again.ok);
    EXPECT_TRUE(again.warnings.empty()) << again.warnings[0].toString();
    EXPECT_EQ(writeTitleArt(again.doc), once) << "write -> parse -> write is identical";
    const ta::TitleAnimationSet set = readAnimationSet(again.doc);
    ASSERT_EQ(set.animations.size(), 1u);
    const ta::TitleAnimation& a = set.animations[0];
    EXPECT_EQ(a.id, "7c1e0a42");
    EXPECT_EQ(a.next, "Idle");
    EXPECT_TRUE(a.hasPresetSource);
    ASSERT_EQ(a.tracks.size(), 5u);
    // tracks sort by (layer order: the title first, then Plate below Title, field)
    EXPECT_EQ(a.tracks[0].field, "live.sweep_position");
    EXPECT_EQ(a.tracks[1].field, "transform.scale");
    EXPECT_EQ(a.tracks[2].layer, "9b0d4e11");
    EXPECT_EQ(a.tracks[4].layer, "3fa0b2c1");
    EXPECT_TRUE(a.tracks[4].stagger.has_value());
    for (const ta::TitleTrack& t : a.tracks) EXPECT_FALSE(t.orphan) << t.field;
    ASSERT_EQ(a.markers.size(), 1u);
    EXPECT_EQ(a.markers[0].sound.path, "sounds/stamp.wav");
}

TEST(TitleAnimIOTest, UnknownMembersKept){
    TitleArtDoc doc = withAnimation(exampleDocument(), R"({"fps": 24, "future": {"x": 1}, "animations": [
        {"id": "00000201", "name": "A", "later": true, "tracks": [{"layer": "", "field": "opacity", "keys": [[0, 1]], "curve_id": 7}],
         "markers": [{"time": 0, "name": "go", "color": "red"}]}]})");
    const std::string text = writeTitleArt(doc);
    EXPECT_NE(text.find("\"future\""), std::string::npos);
    EXPECT_NE(text.find("\"later\""), std::string::npos);
    EXPECT_NE(text.find("\"curve_id\""), std::string::npos);
    EXPECT_NE(text.find("\"color\": \"red\""), std::string::npos);
    TitleArtParseResult again = parseTitleArt(text);
    EXPECT_EQ(writeTitleArt(again.doc), text);
}

TEST(TitleAnimIOTest, LenientDropsWithJsonPath){
    TitleArtDoc base = exampleDocument();
    base.root["animation"] = Json::parse(R"({"animations": [{"id": "00000301", "name": "A", "tracks": [
        {"layer": "", "field": "opacity", "keys": [[0, 1], ["x", 2], [0.5, 0]]},
        {"layer": "", "keys": [[0, 1]]}]}]})");
    TitleArtParseResult r = parseTitleArt(writeTitleArt(base));
    ASSERT_TRUE(r.ok);
    bool key = false, track = false;
    for (const TitleWarning& w : r.warnings){
        if (w.path == "animation.animations[0].tracks[0].keys[1]") key = true;
        if (w.path == "animation.animations[0].tracks[1].field") track = true;
    }
    EXPECT_TRUE(key);
    EXPECT_TRUE(track);
    const ta::TitleAnimationSet set = readAnimationSet(r.doc);
    ASSERT_EQ(set.animations[0].tracks.size(), 1u);
    EXPECT_EQ(set.animations[0].tracks[0].keys.size(), 2u);
}

TEST(TitleAnimIOTest, LimitsClamped){
    Json member = {{"fps", 0}, {"animations", Json::array()}};
    for (int i = 0; i < 40; i++){
        Json a = {{"name", "A" + std::to_string(i)}, {"duration", 500}, {"speed", 99}, {"loop_start", 600}};
        Json markers = Json::array();
        for (int m = 0; m < 70; m++) markers.push_back({{"time", m * 0.1}, {"name", "m" + std::to_string(m)}});
        a["markers"] = markers;
        member["animations"].push_back(a);
    }
    TitleArtDoc doc = exampleDocument();
    doc.root["animation"] = member;
    TitleArtParseResult r = parseTitleArt(writeTitleArt(doc));
    const ta::TitleAnimationSet set = readAnimationSet(r.doc);
    EXPECT_EQ(set.fps, 1);
    ASSERT_EQ(set.animations.size(), ta::TITLE_ANIM_MAX_ANIMATIONS);
    EXPECT_EQ(set.animations[0].duration, 60.0f);
    EXPECT_EQ(set.animations[0].speed, 10.0f);
    EXPECT_EQ(set.animations[0].loopStart, 0.0f) << "a loop start not before the duration";
    EXPECT_EQ(set.animations[0].markers.size(), ta::TITLE_ANIM_MAX_MARKERS);
    for (const ta::TitleAnimation& a : set.animations) EXPECT_TRUE(isTitleArtId(a.id)) << "every animation gets an id";
}

TEST(TitleAnimIOTest, OrphanTrackKept){
    TitleArtDoc doc = withAnimation(exampleDocument(), EXAMPLE);
    // the text layer goes: its glyph track stays, marked orphan, not played
    Json& layers = doc.layers();
    Json removed = layers[1];
    layers.erase(layers.begin() + 1);
    normalizeTitleArt(doc);
    const std::string text = writeTitleArt(doc);
    EXPECT_NE(text.find("3fa0b2c1"), std::string::npos) << "the track is kept in the file";
    ta::TitleAnimationSet set = readAnimationSet(doc);
    int orphans = 0;
    for (const ta::TitleTrack& t : set.animations[0].tracks) orphans += t.orphan ? 1 : 0;
    EXPECT_EQ(orphans, 1);
    std::vector<TitleProblem> problems;
    const ta::CompiledTitleAnimation c = compileAnimation(doc, set.animations[0], nullptr, &problems);
    bool reported = false;
    for (const TitleProblem& p : problems) reported = reported || p.code == "anim_orphan";
    EXPECT_TRUE(reported);
    const TitleAnimPlan plan = buildAnimPlan(doc, {c}, TitleArtRenderMode::BAKED);
    int notPlayed = 0;
    for (const TrackPlan& t : plan.animations[0].tracks) notPlayed += t.play == TrackPlay::NotPlayed ? 1 : 0;
    EXPECT_EQ(notPlayed, 1);
    // undoing the removal brings it back to life
    doc.layers().push_back(removed);
    normalizeTitleArt(doc);
    set = readAnimationSet(doc);
    for (const ta::TitleTrack& t : set.animations[0].tracks)
        EXPECT_FALSE(t.orphan) << t.layer << " " << t.field << ": " << resolveTrackField(doc, t.layer, t.field).error;
}

TEST(TitleAnimIOTest, NoAnimationByteIdentical){
    // a document from before S85 (no animation, no stretch, tint, flash or glyph_mode) re-saves unchanged
    TitleArtDoc doc = exampleDocument();
    const std::string text = writeTitleArt(doc);
    EXPECT_EQ(text.find("\"animation\""), std::string::npos);
    EXPECT_EQ(text.find("\"stretch\": [1, 1]"), std::string::npos);
    EXPECT_EQ(text.find("\"tint\": \"#ffffff\""), std::string::npos);
    EXPECT_EQ(text.find("\"flash\": {"), std::string::npos);
    EXPECT_EQ(text.find("\"glyph_mode\""), std::string::npos);
    TitleArtParseResult r = parseTitleArt(text);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(writeTitleArt(r.doc), text);
    // in memory the members are there at their defaults
    const Json& layer = r.doc.layers()[1];
    EXPECT_EQ(layer["tint"], "#ffffff");
    EXPECT_EQ(layer["flash"]["amount"], 0);
    EXPECT_EQ(layer["glyph_mode"], "cells");
    EXPECT_EQ(layer["transform"]["stretch"], Json::array({1, 1}));
    EXPECT_EQ(TITLE_ART_VERSION, 1) << "R58: no version bump";
    // a value other than the default is written
    Json& edited = r.doc.layers()[1];
    edited["tint"] = "#ff0000";
    EXPECT_NE(writeTitleArt(r.doc).find("\"tint\": \"#ff0000\""), std::string::npos);
}

TEST(TitleAnimIOTest, FuzzNoCrash){
    std::mt19937 random(8585);
    const std::string example = EXAMPLE;
    TitleArtDoc base = exampleDocument();
    for (int i = 0; i < 400; i++){
        std::string text = example;
        const int edits = 1 + (int)(random() % 6);
        for (int e = 0; e < edits; e++){
            const size_t at = random() % text.size();
            switch (random() % 3){
                case 0: text.erase(at, 1 + random() % 4); break;
                case 1: text.insert(at, 1, "{}[],:\"0x-"[random() % 10]); break;
                default: text[at] = (char)(32 + random() % 95); break;
            }
        }
        Json member = Json::parse(text, nullptr, false);
        if (member.is_discarded()) member = text;
        TitleArtDoc doc = base;
        doc.root["animation"] = member;
        TitleArtParseResult r = parseTitleArtJson(doc.root);
        if (!r.ok) continue;
        const ta::TitleAnimationSet set = readAnimationSet(r.doc);
        for (const ta::TitleAnimation& a : set.animations){
            const ta::CompiledTitleAnimation c = compileAnimation(r.doc, a, nullptr, nullptr);
            ta::TitleAnimPose pose;
            pose.size(c);
            for (double t : {0.0, 0.3, 1.7}) ta::sampleAnimation(c, ta::wrapAnimationTime(t, c), pose);
            std::vector<ta::CrossedMarker> markers;
            ta::collectCrossedMarkers(c, 0.0, 5.0, true, markers);
        }
        EXPECT_EQ(writeTitleArt(parseTitleArt(writeTitleArt(r.doc)).doc), writeTitleArt(r.doc));
    }
}

TEST(TitleAnimIOTest, NewMembersIdentity){
    // S85's members at their defaults change neither the content hash nor the planner's
    // placement: the still renders as without them (the GPU half is TitleAnimGpu)
    TitleArtDoc a = exampleDocument();
    TitleArtDoc b = a;
    for (Json& layer : b.layers()) layer.erase("tint");
    EXPECT_EQ(writeTitleArt(a), writeTitleArt(b));
    const TitleTransform t = decodeTransform(a.layers()[0]["transform"]);
    EXPECT_EQ(t.stretch.x, 1.0);
    EXPECT_EQ(t.stretch.y, 1.0);
}

// ---- stagger units from the layout (AC5) ----

TEST(TitleAnimStaggerTest, WhitespaceSkipped){
    TestAssetResolver assets;
    TitleTextLayout layout(&assets);
    TitleArtDoc doc = documentWith({Json{{"type", "text"}, {"id", "00000401"}, {"text", "A B  C"}, {"font", fontRef(OSWALD)}}});
    const GlyphUnits u = glyphUnitsOf(doc.layers()[0], ta::StaggerUnit::Glyph, &layout);
    EXPECT_EQ(u.count, 3u);
    int seen = 0;
    for (int32_t x : u.clusterUnit) if (x >= 0) EXPECT_EQ(x, seen++);
}

TEST(TitleAnimStaggerTest, WordAndLineUnits){
    TestAssetResolver assets;
    TitleTextLayout layout(&assets);
    TitleArtDoc doc = documentWith({Json{{"type", "text"}, {"id", "00000402"}, {"text", "BIG WIN\nMEGA"}, {"font", fontRef(OSWALD)}}});
    const Json& layer = doc.layers()[0];
    EXPECT_EQ(glyphUnitsOf(layer, ta::StaggerUnit::Word, &layout).count, 3u);
    EXPECT_EQ(glyphUnitsOf(layer, ta::StaggerUnit::Line, &layout).count, 2u);
    EXPECT_EQ(glyphUnitsOf(layer, ta::StaggerUnit::Glyph, &layout).count, 10u);
    // without a layout the code points give the same units
    EXPECT_EQ(glyphUnitsOf(layer, ta::StaggerUnit::Word, nullptr).count, 3u);
    EXPECT_EQ(glyphUnitsOf(layer, ta::StaggerUnit::Line, nullptr).count, 2u);
}

TEST(TitleAnimStaggerTest, VerticalColumns){
    TestAssetResolver assets;
    TitleTextLayout layout(&assets);
    TitleArtDoc doc = documentWith({Json{{"type", "text"}, {"id", "00000403"}, {"text", "確定\n大当"}, {"font", fontRef(DELA_GOTHIC)}, {"vertical", true}}});
    const Json& layer = doc.layers()[0];
    // a line unit is a column in vertical text
    EXPECT_EQ(glyphUnitsOf(layer, ta::StaggerUnit::Line, &layout).count, 2u);
    const GlyphUnits g = glyphUnitsOf(layer, ta::StaggerUnit::Glyph, &layout);
    EXPECT_EQ(g.count, 4u);
    // forward is reading order: down the first column, then the next
    const LaidOutText& t = layout.layout(layer);
    for (size_t i = 0; i + 1 < t.clusters.size(); i++){
        if (g.clusterUnit[i] < 0 || g.clusterUnit[i + 1] < 0) continue;
        if (t.clusters[i].index < t.clusters[i + 1].index) EXPECT_LT(g.clusterUnit[i], g.clusterUnit[i + 1]);
    }
}

TEST(TitleAnimStaggerTest, RtlLogicalOrder){
    TestAssetResolver assets;
    TitleTextLayout layout(&assets);
    // Hebrew runs right to left: forward is the logical order, not the visual one
    TitleArtDoc doc = documentWith({Json{{"type", "text"}, {"id", "00000404"}, {"text", "\xD7\x90\xD7\x91\xD7\x92"}, {"font", fontRef(OSWALD)}}});
    const Json& layer = doc.layers()[0];
    const LaidOutText& t = layout.layout(layer);
    const GlyphUnits g = glyphUnitsOf(layer, ta::StaggerUnit::Glyph, &layout);
    ASSERT_EQ(g.count, 3u);
    // the unit of the first logical character (alef) is 0, wherever it sits on screen
    int alef = -1;
    double alefX = 0.0, lastX = 0.0;
    int last = -1;
    for (size_t i = 0; i < t.clusters.size(); i++){
        if (t.clusters[i].index == 0){ alef = g.clusterUnit[i]; alefX = t.clusters[i].pen.x; }
        if (t.clusters[i].index == 2){ last = g.clusterUnit[i]; lastX = t.clusters[i].pen.x; }
    }
    EXPECT_EQ(alef, 0);
    EXPECT_EQ(last, 2);
    // the red control's premise: ordering by screen position would put alef last
    EXPECT_GT(alefX, lastX) << "the run is right to left on screen";
}

// ---- classes and plans (AC7) ----

TEST(TitleAnimPlanTest, ClassTable){
    // §6.2: S80's members carry their FxAnim; S85's live members are live, structure is none
    const TitleArtSchema& schema = TitleArtSchema::get();
    const SchemaKind* transform = schema.kind("transform");
    for (const char* key : {"position", "rotation", "scale", "stretch", "flip_x", "flip_y"})
        EXPECT_EQ(schema.member(*transform, key)->anim, FxAnim::Live) << key;
    const SchemaKind* layer = schema.kind("layer");
    for (const char* key : {"opacity", "tint", "visible"}) EXPECT_EQ(schema.member(*layer, key)->anim, FxAnim::Live) << key;
    for (const char* key : {"id", "type", "name", "locked", "look"}) EXPECT_EQ(schema.member(*layer, key)->anim, FxAnim::None) << key;
    const SchemaKind* text = schema.kind("layer.text");
    EXPECT_EQ(schema.member(*text, "text")->anim, FxAnim::Relayout);
    EXPECT_EQ(schema.member(*text, "size")->anim, FxAnim::Relayout);
    EXPECT_EQ(schema.member(*text, "font")->anim, FxAnim::Step);

    TitleArtDoc doc = exampleDocument();
    auto cls = [&](const std::string& layerId, const std::string& field){
        ta::TitleTrack t;
        t.layer = layerId;
        t.field = field;
        ta::TitleKey k;
        k.key.value = {1.0f};
        t.keys.push_back(k);
        ta::TitleAnimation a;
        a.name = "A";
        a.tracks = {t};
        return compileAnimation(doc, a, nullptr, nullptr).tracks[0].cls;
    };
    EXPECT_EQ(cls("", "transform.scale"), ta::TrackClass::TitleLive);
    EXPECT_EQ(cls("3fa0b2c1", "glyph.opacity"), ta::TrackClass::Glyph);
    EXPECT_EQ(cls("3fa0b2c1", "text"), ta::TrackClass::GlyphAtlasLive);
    EXPECT_EQ(cls("3fa0b2c1", "opacity"), ta::TrackClass::LayerLive);
    EXPECT_EQ(cls("3fa0b2c1", "transform.rotation"), ta::TrackClass::LayerLive);
    EXPECT_EQ(cls("3fa0b2c1", "size"), ta::TrackClass::Rerender);
    EXPECT_EQ(cls("3fa0b2c1", "face.fill.color"), ta::TrackClass::Rerender);
    EXPECT_EQ(cls("3fa0b2c1", "name"), ta::TrackClass::None);
    // a member registered without the attribute is re-render (safe)
    SchemaMember extra;
    extra.key = "s85_test_member";
    extra.type = ValueType::Number;
    extra.defaultValue = 0;
    ASSERT_TRUE(TitleArtSchema::get().addMember("layer", extra));
    TitleArtDoc again = exampleDocument();
    doc = again;
    EXPECT_EQ(cls("3fa0b2c1", "s85_test_member"), ta::TrackClass::Rerender);
    TitleArtSchema::get().removeMember("layer", "s85_test_member");
}

TEST(TitleAnimPlanTest, LiveSuffixFallback){
    // Wave Warp's phase is live (a kernel) alone; followed by Distress (not scale_split) it falls back
    TitleArtDoc doc = documentWith({Json{{"type", "shape"}, {"id", "00000501"}, {"effects", Json::array({Json{{"type", "wave_warp"}}})}}});
    EXPECT_EQ(liveParamBlocker(doc.layers()[0], "effects[0].params.phase"), "");
    TitleArtDoc suffixed = documentWith({Json{{"type", "shape"}, {"id", "00000502"},
                                              {"effects", Json::array({Json{{"type", "wave_warp"}}, Json{{"type", "distress"}}})}}});
    const std::string reason = liveParamBlocker(suffixed.layers()[0], "effects[0].params.phase");
    EXPECT_NE(reason.find("D10"), std::string::npos) << reason;
    ta::TitleTrack t;
    t.layer = "00000502";
    t.field = "effects[0].params.phase";
    t.drive = ta::TitleDrive{{0.0f}, {1.0f}, 1.0f, 0.0f, false, EaseType::LINEAR};
    ta::TitleAnimation a;
    a.name = "Wave";
    a.tracks = {t};
    const ta::CompiledTitleAnimation c = compileAnimation(suffixed, a, nullptr, nullptr);
    EXPECT_EQ(c.tracks[0].cls, ta::TrackClass::Rerender);
}

TEST(TitleAnimPlanTest, ThreeModes){
    // §6.3: Rays (a sunburst, its rotation driven) · Plate (no tracks) · Title (a glyph cascade in
    // Intro, a Rim 1 Width pulse in Win); Idle's shimmer is the title's sweep
    Json rays = {{"type", "generator"}, {"id", "00000601"}, {"name", "Rays"}, {"generator", {{"type", "sunburst"}}}};
    Json plate = {{"type", "shape"}, {"id", "00000602"}, {"name", "Plate"}};
    Json title = {{"type", "text"}, {"id", "00000603"}, {"name", "Title"}, {"text", "WIN"},
                  {"style", {{"rims", Json::array({Json{{"width", 4}}})}}}};
    TitleArtDoc doc = documentWith({rays, plate, title});
    doc.root["runtime"]["glyph_layer"] = "00000603";
    doc.root["animation"] = Json::parse(R"({"animations": [
      {"id": "00000611", "name": "Intro", "duration": 0.8, "tracks": [
        {"layer": "", "field": "transform.scale", "relative": true, "keys": [[0, 2], [0.8, 1]]},
        {"layer": "00000603", "field": "glyph.offset", "relative": true, "stagger": {"each": 0.05}, "keys": [[0, [0, 40]], [0.3, [0, 0]]]}]},
      {"id": "00000612", "name": "Idle", "duration": 2, "wrap": "loop", "tracks": [
        {"layer": "", "field": "live.sweep_position", "keys": [[0, 0], [2, 1]]},
        {"layer": "00000601", "field": "gen.rotation", "drive": {"from": -180, "to": 180, "cycles": 1, "wrap": true}}]},
      {"id": "00000613", "name": "Win", "duration": 1, "tracks": [
        {"layer": "00000603", "field": "rim#0.width", "keys": [[0, 4], [0.5, 8], [1, 4]]},
        {"layer": "", "field": "flash.amount", "keys": [[0, 0], [0.5, 0.5], [1, 0]]}]}]})");
    TitleArtParseResult parsed = parseTitleArtJson(doc.root);
    ASSERT_TRUE(parsed.ok);
    doc = parsed.doc;
    const auto lib = compileAnimations(doc, nullptr, nullptr);
    ASSERT_EQ(lib->animations.size(), 3u);
    auto how = [&](TitleArtRenderMode mode, const std::string& anim, const std::string& field){
        const TitleAnimPlan plan = buildAnimPlan(doc, lib->animations, mode);
        const AnimationPlan* ap = plan.find(anim);
        std::string seen;
        for (const TrackPlan& t : ap->tracks){
            if (t.field == field) return t.how;
            seen += " " + t.field + "=" + t.how + "(" + t.reason + ")";
        }
        return std::string("?") + seen;
    };
    using M = TitleArtRenderMode;
    // Intro
    EXPECT_EQ(how(M::BAKED, "Intro", "transform.scale"), "companion");
    EXPECT_EQ(how(M::BAKED, "Intro", "glyph.offset"), "flipbook");
    EXPECT_EQ(how(M::REALTIME, "Intro", "glyph.offset"), "glyph_cells");
    EXPECT_EQ(how(M::GLYPH_ATLAS, "Intro", "glyph.offset"), "glyph_cells");
    EXPECT_EQ(how(M::GLYPH_ATLAS, "Intro", "transform.scale"), "companion");
    // Idle
    EXPECT_EQ(how(M::BAKED, "Idle", "live.sweep_position"), "companion");
    EXPECT_EQ(how(M::BAKED, "Idle", "generator.params.rotation"), "flipbook");
    EXPECT_EQ(how(M::GLYPH_ATLAS, "Idle", "generator.params.rotation"), "plate_rerender");
    // Win
    EXPECT_EQ(how(M::BAKED, "Win", "style.rims[0].width"), "flipbook");
    EXPECT_EQ(how(M::REALTIME, "Win", "style.rims[0].width"), "rerender");
    EXPECT_EQ(how(M::GLYPH_ATLAS, "Win", "style.rims[0].width"), "none") << "not played: rerender_in_glyph_atlas";
    EXPECT_EQ(how(M::GLYPH_ATLAS, "Win", "flash.amount"), "companion");
    const TitleAnimPlan baked = buildAnimPlan(doc, lib->animations, M::BAKED);
    EXPECT_TRUE(baked.find("Intro")->flipbook);
    EXPECT_TRUE(baked.find("Win")->flipbook);
}

TEST(TitleAnimPlanTest, LayerSplitRules){
    // R14: an adjustment layer above an animated layer stops its split; the red control's flag
    // ignores it
    Json moving = {{"type", "shape"}, {"id", "00000701"}, {"name", "Moving"}};
    Json adjust = {{"type", "adjustment"}, {"id", "00000702"}, {"name", "Grade"}};
    TitleArtDoc doc = documentWith({moving, adjust});
    std::string code;
    EXPECT_NE(layerSplitBlocker(doc, "00000701", &code), "");
    EXPECT_EQ(code, "adjustment_above");
    TitleAnimPlan::testFlags().ignoreAdjustments = true;
    EXPECT_EQ(layerSplitBlocker(doc, "00000701"), "");
    TitleAnimPlan::testFlags().ignoreAdjustments = false;
    // a clipped layer above makes it a clip base; a layer mask naming it
    Json clipped = {{"type", "shape"}, {"id", "00000703"}, {"clip", true}};
    TitleArtDoc base = documentWith({moving, clipped});
    EXPECT_NE(layerSplitBlocker(base, "00000701", &code), "");
    EXPECT_EQ(code, "clip_base");
    Json masked = {{"type", "shape"}, {"id", "00000704"}, {"masks", Json::array({Json{{"type", "layer"}, {"layer", "00000701"}}})}};
    TitleArtDoc maskDoc = documentWith({moving, masked});
    EXPECT_NE(layerSplitBlocker(maskDoc, "00000701", &code), "");
    EXPECT_EQ(code, "mask_names_it");
    TitleArtDoc alone = documentWith({moving});
    EXPECT_EQ(layerSplitBlocker(alone, "00000701"), "");
}

// R62: a marker's sound is one of the document's references, so S04's relink and index see it
TEST(TitleAnimIOTest, RefWalkerVisitsMarkerSounds) {
    Json root = {{"animation", {{"animations", Json::array({
        {{"name", "A"}, {"markers", Json::array({{{"name", "hit"}, {"time", 0.1}, {"sound", {{"path", "sfx/hit.wav"}, {"guid", "aa"}}}},
                                                     {{"name", "quiet"}, {"time", 0.2}}})}}})}}}};
    std::vector<std::string> fields;
    visitTitleArtRefs(root, [&](Json& ref, const std::string& field, TitleAssetKind kind) {
        EXPECT_EQ(kind, TitleAssetKind::Sound);
        fields.push_back(field);
        ref = encodeAssetRef({"sfx/moved.wav", "aa"});
        return true;
    });
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0], "animation.animations[0].markers[0].sound");
    EXPECT_EQ(root["animation"]["animations"][0]["markers"][0]["sound"]["path"], "sfx/moved.wav");
}
