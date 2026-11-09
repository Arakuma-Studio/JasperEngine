// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85 AC14 (R21-R23, R61), AC18's op half (R50), AC10's storage half (R16, R20): the animation ops
// inside S80's batches, the Animation Presets applied to titles, Save as Preset, and the flipbook
// packing. No GPU: the frames here are made by hand.

#include <gtest/gtest.h>

#include "titleanim/TitleAnimDataIO.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/anim/TitleAnimCost.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimOps.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/anim/TitleAnimPresets.h"
#include "titleart/anim/TitleFlipbookBaker.h"

#include <algorithm>
#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;
using Json = tt::Json;

namespace {

    tt::TitleArtDoc title(const std::string& text = "WIN"){
        registerTitleAnimOps();
        registerTitleAnimPresetOp();
        tt::NewTitleArtOptions options;
        options.text = text;
        return tt::makeNewTitleArt(options);
    }

    TitleOpsResult applyOps(const tt::TitleArtDoc& doc, Json ops){
        return TitleArtOps::apply(doc, ops);
    }

    tt::TitleArtDoc applied(const tt::TitleArtDoc& doc, Json ops){
        const TitleOpsResult r = applyOps(doc, std::move(ops));
        EXPECT_TRUE(r.ok) << r.error();
        return r.doc;
    }

    Json op(const char* name, Json members = Json::object()){
        members["op"] = name;
        return members;
    }

    std::string textLayer(const tt::TitleArtDoc& doc){
        return mainTextLayerOf(doc);
    }

    texsynth::Image image(int w, int h, int x, int y, int bw, int bh, uint8_t value){
        texsynth::Image out = texsynth::Image::make(w, h, 4, 8);
        for (int j = y; j < y + bh; j++)
            for (int i = x; i < x + bw; i++){
                uint8_t* p = out.pixel(i, j);
                p[0] = value; p[1] = 10; p[2] = 20; p[3] = 255;
            }
        return out;
    }

}

TEST(TitleAnimOpsTest, EveryOpAndAllOrNothing){
    tt::TitleArtDoc doc = title();
    doc = applied(doc, Json::array({op("add_animation", {{"name", "Intro"}, {"role", "intro"}, {"duration", 0.5}})}));
    ta::TitleAnimationSet set = tt::readAnimationSet(doc);
    ASSERT_EQ(set.animations.size(), 1u);
    EXPECT_EQ(set.defaultName, "Intro") << "the first animation is the default";
    EXPECT_TRUE(tt::isTitleArtId(set.animations[0].id));

    doc = applied(doc, Json::array({
        op("set_track", {{"animation", "Intro"}, {"field", "transform.scale"}, {"relative", true}, {"keys", Json::array({Json::array({0, 2, "quad_in"}), Json::array({0.5, 1})})}}),
        op("set_key", {{"animation", "Intro"}, {"field", "opacity"}, {"time", 0.25}, {"value", 0.5}}),
        op("set_marker", {{"animation", "Intro"}, {"name", "onImpact"}, {"time", 0.25}}),
        op("set_animation", {{"animation", "Intro"}, {"values", {{"flipbook.fps", 12}, {"speed", 2}}}}),
        op("duplicate_animation", {{"animation", "Intro"}, {"name", "Other"}}),
        op("set_default_animation", {{"animation", "Other"}}),
        op("retime", {{"animation", "Intro"}, {"scale", 2}}),
        op("remove_keys", {{"animation", "Other"}, {"field", "opacity"}, {"times", Json::array({0.25})}}),
        op("remove_marker", {{"animation", "Other"}, {"name", "onImpact"}}),
    }));
    set = tt::readAnimationSet(doc);
    ASSERT_EQ(set.animations.size(), 2u);
    EXPECT_EQ(set.defaultName, "Other");
    const ta::TitleAnimation* intro = set.find("Intro");
    ASSERT_NE(intro, nullptr);
    EXPECT_FLOAT_EQ(intro->duration, 1.0f) << "retime scales the duration";
    EXPECT_FLOAT_EQ(intro->markers[0].time, 0.5f);
    EXPECT_FLOAT_EQ(intro->flipbook.fps, 12.0f);
    const ta::TitleAnimation* other = set.find("Other");
    EXPECT_EQ(other->tracks.size(), 1u) << "a track without keys goes";
    EXPECT_TRUE(other->markers.empty());

    // a rename follows the references; removing one clears them
    doc = applied(doc, Json::array({op("set_animation", {{"animation", "Other"}, {"values", {{"next", "Intro"}}}}),
                                    op("set_animation", {{"animation", "Intro"}, {"values", {{"name", "Start"}}}})}));
    set = tt::readAnimationSet(doc);
    EXPECT_EQ(set.find("Other")->next, "Start");
    doc = applied(doc, Json::array({op("remove_animation", {{"animation", "Start"}})}));
    set = tt::readAnimationSet(doc);
    EXPECT_EQ(set.find("Other")->next, "");

    // all or nothing: the second op fails, with its index, and nothing is applied
    const TitleOpsResult bad = applyOps(doc, Json::array({op("set_marker", {{"animation", "Other"}, {"name", "a"}, {"time", 0}}),
                                                       op("set_key", {{"animation", "Nope"}, {"field", "opacity"}, {"time", 0}})}));
    ASSERT_FALSE(bad.ok);
    EXPECT_EQ(bad.failedOp, 1);
    ASSERT_FALSE(bad.problems.empty());
    EXPECT_EQ(bad.problems[0].code, "unknown_animation");
    EXPECT_EQ(bad.problems[0].nearest, "");
    EXPECT_FALSE(bad.problems[0].validHere.empty());
    EXPECT_TRUE(tt::readAnimationSet(doc).find("Other")->markers.empty());
}

TEST(TitleAnimOpsTest, ErrorsListTheAlternatives){
    tt::TitleArtDoc doc = title();
    const std::string layer = textLayer(doc);
    doc = applied(doc, Json::array({op("add_animation", {{"name", "A"}})}));
    // not animatable: the layer's animatable fields
    TitleOpsResult r = applyOps(doc, Json::array({op("set_key", {{"animation", "A"}, {"layer", "id:" + layer}, {"field", "name"}, {"time", 0}, {"value", "x"}})}));
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.problems[0].code, "not_animatable");
    EXPECT_NE(std::find(r.problems[0].validHere.begin(), r.problems[0].validHere.end(), "opacity"), r.problems[0].validHere.end());
    // a whole value and its component
    doc = applied(doc, Json::array({op("set_key", {{"animation", "A"}, {"layer", "id:" + layer}, {"field", "tint"}, {"time", 0}, {"value", "#ff0000"}})}));
    r = applyOps(doc, Json::array({op("set_key", {{"animation", "A"}, {"layer", "id:" + layer}, {"field", "tint.r"}, {"time", 0}, {"value", 0.5}})}));
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.problems[0].code, "component_conflict");
    // relative on a stepped field; a reseed on what is not a seed; a merged layer's glyph colour
    r = applyOps(doc, Json::array({op("set_track", {{"animation", "A"}, {"layer", "id:" + layer}, {"field", "text"}, {"relative", true},
                                                 {"keys", Json::array({Json::array({0, "A"})})}})}));
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.problems[0].code, "relative_stepped");
    r = applyOps(doc, Json::array({op("set_track", {{"animation", "A"}, {"layer", "id:" + layer}, {"field", "opacity"}, {"reseed", {{"rate", 4}}}})}));
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.problems[0].code, "reseed_not_seed");
    tt::TitleArtDoc merged = applied(doc, Json::array({op("set", {{"layer", "id:" + layer}, {"values", {{"glyph_mode", "merged"}}}})}));
    r = applyOps(merged, Json::array({op("set_track", {{"animation", "A"}, {"layer", "id:" + layer}, {"field", "glyph.opacity"},
                                                    {"keys", Json::array({Json::array({0, 1})})}})}));
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.problems[0].code, "glyph_color_needs_cells");
    // shorthands are stored long form
    doc = applied(doc, Json::array({op("set_key", {{"animation", "A"}, {"layer", "id:" + layer}, {"field", "face.opacity"}, {"time", 0}, {"value", 0.5}})}));
    bool longForm = false;
    const ta::TitleAnimationSet stored = tt::readAnimationSet(doc);
    for (const ta::TitleTrack& t : stored.find("A")->tracks) longForm = longForm || t.field == "style.face.opacity";
    EXPECT_TRUE(longForm);
}

TEST(TitleAnimOpsTest, SetKeyDefaultsToTheSampledValue){
    tt::TitleArtDoc doc = title();
    doc = applied(doc, Json::array({op("add_animation", {{"name", "A"}, {"duration", 1}}),
                                    op("set_track", {{"animation", "A"}, {"field", "opacity"}, {"keys", Json::array({Json::array({0, 0}), Json::array({1, 1})})}}),
                                    op("set_key", {{"animation", "A"}, {"field", "opacity"}, {"time", 0.25}})}));
    const ta::TitleAnimationSet stored = tt::readAnimationSet(doc);
    const ta::TitleTrack& t = stored.find("A")->tracks[0];
    ASSERT_EQ(t.keys.size(), 3u);
    EXPECT_NEAR(t.keys[1].key.value[0], 0.25f, 1e-5f);
}

TEST(TitleAnimPresetsTest, TheLibrary){
    registerTitleAnimOps();
    const std::vector<TitleAnimPreset>& all = TitleAnimPresets::all();
    EXPECT_GE(all.size(), 28u);
    int groups[4] = {0, 0, 0, 0};
    for (const TitleAnimPreset& p : all){
        groups[0] += p.group == "in";
        groups[1] += p.group == "emphasis";
        groups[2] += p.group == "loop";
        groups[3] += p.group == "out";
    }
    EXPECT_EQ(groups[0], 15);
    EXPECT_EQ(groups[1], 4);
    EXPECT_EQ(groups[2], 6);
    EXPECT_EQ(groups[3], 3);
}

TEST(TitleAnimPresetsTest, EveryPresetAppliesToThreeTitles){
    // §6.4's class column: these are title live only at their defaults (D7: 19 of 28)
    const std::set<std::string> titleLive = {"stamp_in", "glow_up", "bounce_in", "flip_in", "zoom_punch", "slide_in", "pop_in", "fade_in", "neon_flicker",
                                             "shake", "flash", "heartbeat", "jelly", "idle_shimmer", "win_pulse", "float_loop", "breathe", "zoom_out", "fade_out"};
    std::vector<tt::TitleArtDoc> docs = {title("WIN"), title("BIG WIN\nMEGA")};
    // a title with rays, a plate and a group
    tt::TitleArtDoc rich = title("JACKPOT");
    rich = applied(rich, Json::array({op("add_layer", {{"type", "shape"}, {"name", "Plate"}})}));
    docs.push_back(rich);
    int live = 0;
    for (const TitleAnimPreset& p : TitleAnimPresets::all()){
        if (p.scope != "builtin") continue;
        for (size_t d = 0; d < docs.size(); d++){
            const TitleOpsResult r = applyOps(docs[d], Json::array({op("add_animation", {{"name", "P"}, {"preset", p.id}})}));
            ASSERT_TRUE(r.ok) << p.id << ": " << r.error();
            const ta::TitleAnimationSet set = tt::readAnimationSet(r.doc);
            const ta::TitleAnimation* a = set.find("P");
            ASSERT_NE(a, nullptr);
            EXPECT_FALSE(a->tracks.empty()) << p.id;
            EXPECT_TRUE(a->hasPresetSource);
            EXPECT_EQ(a->presetId, p.id);
            for (const ta::TitleTrack& t : a->tracks){
                EXPECT_EQ(t.fromPreset, p.id);
                EXPECT_FALSE(t.orphan) << p.id << " " << t.field;
            }
            for (const TitleOpProblem& n : r.problems) EXPECT_NE(n.code, "preset_part_unavailable") << p.id << ": " << n.message;
            const auto lib = tt::compileAnimations(r.doc, nullptr, nullptr);
            const ta::CompiledTitleAnimation* c = lib->find("P");
            ASSERT_NE(c, nullptr);
            bool allLive = true;
            for (const ta::CompiledTitleTrack& t : c->tracks) allLive = allLive && t.cls == ta::TrackClass::TitleLive;
            EXPECT_EQ(allLive, titleLive.count(p.id) > 0) << p.id;
            if (d == 0 && allLive) live++;
            // no track fails to resolve, and the loops are loops
            if (p.wrap == "loop") { EXPECT_EQ(c->wrap, ta::AnimWrap::Loop) << p.id; }
            if (p.overlay) { EXPECT_TRUE(c->overlay) << p.id; }
        }
    }
    EXPECT_EQ(live, 19) << "D7: 19 of the 28 built-in presets are title live";
}

TEST(TitleAnimPresetsTest, EditPresetParametersRegeneratesOnlyItsTracks){
    tt::TitleArtDoc doc = title();
    doc = applied(doc, Json::array({op("add_animation", {{"name", "A"}, {"preset", "stamp_in"}}),
                                    op("set_key", {{"animation", "A"}, {"field", "live.tint_amount"}, {"time", 0.1}, {"value", 0.5}})}));
    const size_t tracks = tt::readAnimationSet(doc).find("A")->tracks.size();
    doc = applied(doc, Json::array({op("apply_animation_preset", {{"animation", "A"}, {"preset", "stamp_in"}, {"params", {{"from_scale", 4}}}})}));
    const ta::TitleAnimationSet set = tt::readAnimationSet(doc);
    const ta::TitleAnimation* a = set.find("A");
    EXPECT_EQ(a->tracks.size(), tracks) << "regenerated, not added";
    bool userTrack = false, scale = false;
    for (const ta::TitleTrack& t : a->tracks){
        userTrack = userTrack || (t.field == "live.tint_amount" && t.fromPreset.empty());
        if (t.field == "transform.scale"){
            scale = true;
            EXPECT_FLOAT_EQ(t.keys[0].key.value[0], 4.0f);
        }
    }
    EXPECT_TRUE(userTrack) << "the user's own track stays";
    EXPECT_TRUE(scale);
    EXPECT_EQ(a->markers.size(), 1u) << "its marker is regenerated, not doubled";
    // unknown names answer with the alternatives
    const TitleOpsResult bad = applyOps(doc, Json::array({op("apply_animation_preset", {{"animation", "A"}, {"preset", "stamp_inn"}})}));
    ASSERT_FALSE(bad.ok);
    EXPECT_EQ(bad.problems[0].code, "unknown_preset");
    EXPECT_EQ(bad.problems[0].nearest, "stamp_in");
    // mode replace clears what else is on the targets; at offsets it
    tt::TitleArtDoc replaced = applied(doc, Json::array({op("apply_animation_preset", {{"animation", "A"}, {"preset", "fade_in"}, {"mode", "replace"}, {"at", 0.2}})}));
    const ta::TitleAnimationSet after = tt::readAnimationSet(replaced);
    bool only = true;
    for (const ta::TitleTrack& t : after.find("A")->tracks) only = only && t.field == "opacity";
    EXPECT_TRUE(only);
    EXPECT_NEAR(after.find("A")->tracks[0].keys[0].key.time, 0.2f, 1e-5f);
}

TEST(TitleAnimPresetsTest, SaveAsPresetRoundTrip){
    tt::TitleArtDoc doc = title();
    const std::string layer = textLayer(doc);
    doc = applied(doc, Json::array({op("add_animation", {{"name", "A"}, {"role", "intro"}, {"duration", 2}}),
                                    op("set_track", {{"animation", "A"}, {"field", "opacity"}, {"keys", Json::array({Json::array({0, 0}), Json::array({2, 1})})}}),
                                    op("set_track", {{"animation", "A"}, {"layer", "id:" + layer}, {"field", "opacity"}, {"keys", Json::array({Json::array({0, 0.5}), Json::array({2, 1})})}}),
                                    op("set_marker", {{"animation", "A"}, {"name", "go"}, {"time", 1}})}));
    const ta::TitleAnimationSet set = tt::readAnimationSet(doc);
    Json preset;
    std::string error;
    ASSERT_TRUE(titleAnimationToPreset(doc, *set.find("A"), "my_fade", "My Fade", preset, error)) << error;
    // times are D-relative; an absolute layer track became relative against its rest (1)
    bool relativeLayer = false;
    for (const Json& t : preset["tracks"])
        if (t.value("target", std::string()) == "main_text"){
            relativeLayer = t.value("relative", false);
            EXPECT_NEAR(t["keys"][0][1].get<double>(), 0.5, 1e-5);
            EXPECT_EQ(t["keys"][1][0], "D");
        }
    EXPECT_TRUE(relativeLayer);
    TitleAnimPreset parsed;
    ASSERT_TRUE(TitleAnimPresets::parse(preset.dump(), parsed, error)) << error;
    EXPECT_EQ(parsed.group, "in");
    EXPECT_DOUBLE_EQ(parsed.defaultDuration, 2.0);
    // a track on another layer cannot be a preset
    tt::TitleArtDoc two = applied(doc, Json::array({op("add_layer", {{"type", "shape"}, {"name", "Plate"}})}));
    std::string plate;
    for (const Json& l : two.layers()) if (l.value("name", std::string()) == "Plate") plate = l.value("id", std::string());
    two = applied(two, Json::array({op("set_key", {{"animation", "A"}, {"layer", "id:" + plate}, {"field", "opacity"}, {"time", 0}, {"value", 1}})}));
    Json refused;
    EXPECT_FALSE(titleAnimationToPreset(two, *tt::readAnimationSet(two).find("A"), "x", "X", refused, error));
    EXPECT_NE(error.find("Plate"), std::string::npos) << error;
}

TEST(TitleAnimPresetsTest, NoDeniedTermsInBuiltinIds){
    for (const TitleAnimPreset& p : TitleAnimPresets::all()){
        for (const char* term : {"hokuto", "pachinko", "sammy", "sega", "konami", "nintendo", "disney"}){
            EXPECT_EQ(p.id.find(term), std::string::npos) << p.id;
            std::string name = p.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            EXPECT_EQ(name.find(term), std::string::npos) << p.name;
        }
    }
}

TEST(TitleFlipbookTest, FrameTimes){
    {
        // a float duration just over a whole number of frames adds no frame (0.2f x 10 = 2.00000003)
        ta::TitleAnimation a;
        a.name = "Tenth";
        a.duration = 0.2f;
        a.flipbook.fps = 10.0f;
        ta::CompiledTitleAnimation c;
        ta::compileTitleOnly(a, c, nullptr);
        EXPECT_EQ(titleFlipbookFrameTimes(c).size(), 3u);
    }
    ta::CompiledTitleAnimation a;
    a.duration = 0.5;
    a.flipbookSettings.fps = 10;
    std::vector<double> t = titleFlipbookFrameTimes(a);
    ASSERT_EQ(t.size(), 6u) << "once includes D";
    EXPECT_DOUBLE_EQ(t.back(), 0.5);
    a.wrap = ta::AnimWrap::Loop;
    t = titleFlipbookFrameTimes(a);
    ASSERT_EQ(t.size(), 5u) << "a loop excludes D";
    EXPECT_NEAR(t.back(), 0.4, 1e-9);
    a.duration = 60.0;
    a.flipbookSettings.fps = 60;
    EXPECT_EQ(titleFlipbookFrameTimes(a).size(), ta::TITLE_ANIM_MAX_FLIPBOOK_FRAMES);
}

TEST(TitleFlipbookTest, ConstantFrameSizeDedupeAndGutters){
    std::vector<texsynth::Image> renders = {image(100, 60, 10, 10, 20, 20, 200), image(100, 60, 10, 10, 20, 20, 200), image(100, 60, 40, 20, 10, 30, 100)};
    TitleFlipbookPacking p;
    std::string error;
    ASSERT_TRUE(packTitleFlipbook(renders, 1024, p, error)) << error;
    // the union of the alpha bounds (10..50, 10..50) with 1 px
    EXPECT_EQ(p.bounds.x, 9);
    EXPECT_EQ(p.bounds.y, 9);
    EXPECT_EQ(p.frameWidth, 42);
    EXPECT_EQ(p.frameHeight, 42);
    EXPECT_EQ(p.unique, 2) << "a held section stores one rect";
    ASSERT_EQ(p.frames.size(), 3u);
    EXPECT_EQ(p.frames[0].x, p.frames[1].x);
    EXPECT_NE(p.frames[0].x, p.frames[2].x);
    // 4-px aligned cells with 4-px gutters, page sides multiples of 4
    EXPECT_EQ(p.frames[2].x % 4, 0);
    EXPECT_GE(p.frames[2].x - (p.frames[0].x + p.frameWidth), 4);
    ASSERT_EQ(p.pages.size(), 1u);
    EXPECT_EQ(p.pages[0].width % 4, 0);
    EXPECT_EQ(p.pages[0].height % 4, 0);
    // the gutter stays empty
    EXPECT_EQ(p.pages[0].pixel(p.frameWidth + 1, 10)[3], 0);
    // deterministic
    TitleFlipbookPacking again;
    ASSERT_TRUE(packTitleFlipbook(renders, 1024, again, error));
    EXPECT_EQ(again.pages[0].data, p.pages[0].data);
    // frames over max_page are refused with the fix
    EXPECT_FALSE(packTitleFlipbook({image(2000, 10, 0, 0, 1990, 5, 9)}, 1024, again, error));
    EXPECT_NE(error.find("max_page"), std::string::npos);
    // more than a page holds spreads over pages
    std::vector<texsynth::Image> many;
    for (int i = 0; i < 40; i++) many.push_back(image(300, 300, 0, 0, 280, 280, (uint8_t)(i * 5)));
    ASSERT_TRUE(packTitleFlipbook(many, 1024, again, error));
    EXPECT_GT(again.pages.size(), 1u);
    for (const texsynth::Image& page : again.pages){
        EXPECT_LE(page.width, 1024);
        EXPECT_LE(page.height, 1024);
    }
}

TEST(TitleFlipbookTest, EstimateEqualsWritten){
    // R20: the estimate of all-unique frames equals what the packing's pages cook to
    std::vector<texsynth::Image> renders;
    for (int i = 0; i < 9; i++) renders.push_back(image(64, 64, 0, 0, 62, 62, (uint8_t)(i * 20 + 5)));
    TitleFlipbookPacking p;
    std::string error;
    ASSERT_TRUE(packTitleFlipbook(renders, 2048, p, error));
    TitleFlipbookBytes expected;
    for (const texsynth::Image& page : p.pages) expected.add(page.width, page.height);
    for (const char* f : TITLE_FLIPBOOK_FAMILIES) EXPECT_EQ(expected.of(f), p.bytes.of(f)) << f;
    EXPECT_LT(p.bytes.mobile, p.bytes.desktop) << "ASTC 6x6 is the smallest";
    ta::CompiledTitleAnimation a;
    a.name = "A";
    a.duration = 9.0 / 30.0;
    a.wrap = ta::AnimWrap::Loop;
    a.flipbookSettings.fps = 30;
    tt::AnimationPlan plan;
    plan.name = "A";
    plan.flipbook = true;
    TitleAnimCostInputs inputs;
    inputs.still.frame = {0, 0, p.frameWidth - 2, p.frameHeight - 2};
    const TitleAnimCost cost = estimateAnimCost(tt::makeNewTitleArt(), a, plan, inputs);
    EXPECT_EQ(cost.frames, 9);
    EXPECT_EQ(cost.pages, (int)p.pages.size());
    for (const char* f : TITLE_FLIPBOOK_FAMILIES) EXPECT_EQ(cost.bytes.of(f), p.bytes.of(f)) << f;
}
