// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85 AC3 (R13), AC15's player half (R24-R26), AC20 (R35): the player's base, overlays, ends,
// `next` chains, stop / reset / sample, fades and its companion uniforms; no allocation in steady
// state. The `<file>.anim.json` codec (AC10's TitleAnimDataIOTest).

#include <gtest/gtest.h>

#include "support/AllocationCounter.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimPlayer.h"

#include <cmath>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::titleanim;

namespace {

    TitleTrack track(const std::string& field, std::vector<std::pair<float, std::vector<float>>> keys, bool relative = false){
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

    CompiledTitleAnimation compiled(const TitleAnimation& a){
        CompiledTitleAnimation c;
        compileTitleOnly(a, c, nullptr);
        return c;
    }

    std::shared_ptr<TitleAnimLibrary> library(){
        auto lib = std::make_shared<TitleAnimLibrary>();
        TitleAnimation intro;
        intro.name = "Intro";
        intro.duration = 0.5f;
        intro.next = "Idle";
        intro.tracks.push_back(track("transform.scale", {{0.0f, {2.0f}}, {0.5f, {1.0f}}}, true));
        intro.tracks.push_back(track("opacity", {{0.0f, {0.0f}}, {0.25f, {1.0f}}}));
        TitleMarker impact;
        impact.time = 0.3f;
        impact.name = "onImpact";
        intro.markers.push_back(impact);
        lib->animations.push_back(compiled(intro));

        TitleAnimation idle;
        idle.name = "Idle";
        idle.duration = 1.0f;
        idle.wrap = AnimWrap::Loop;
        idle.tracks.push_back(track("live.sweep_position", {{0.0f, {0.0f}}, {1.0f, {1.0f}}}));
        TitleMarker beat;
        beat.time = 0.0f;
        beat.name = "onBeat";
        idle.markers.push_back(beat);
        lib->animations.push_back(compiled(idle));

        TitleAnimation shake;
        shake.name = "Shake";
        shake.duration = 0.4f;
        shake.overlay = true;
        shake.tracks.push_back(track("transform.position", {{0.0f, {0.0f, 0.0f}}, {0.2f, {10.0f, 0.0f}}, {0.4f, {0.0f, 0.0f}}}, true));
        lib->animations.push_back(compiled(shake));
        return lib;
    }

    TitleFrameGeometry frame(){
        TitleFrameGeometry g;
        g.frameX = 100;
        g.frameY = 50;
        g.frameWidth = 400;
        g.frameHeight = 200;
        g.originX = 200;
        g.originY = 100;
        g.inkX = 150;
        g.inkY = 80;
        g.inkWidth = 300;
        g.inkHeight = 140;
        return g;
    }

}

TEST(TitleAnimPlayerTest, CompanionUniforms){
    TitleFrameGeometry g = frame();
    TitlePoseSummary s;
    CompanionAnimUniforms u;
    companionUniforms(s, g, TrackPivot::Origin, nullptr, -1, -1, 0.0f, u);
    EXPECT_FLOAT_EQ(u.vertex0[0], 1.0f);
    EXPECT_FLOAT_EQ(u.vertex0[2], 0.0f);
    EXPECT_FLOAT_EQ(u.vertex1[1], 1.0f);
    EXPECT_FLOAT_EQ(u.affine0[3], 1.0f) << "opacity";
    EXPECT_FLOAT_EQ(u.color[3], 0.0f) << "no flash";

    // scale 2 about the frame centre: the centre stays, the corners go out
    s.scale = 2.0;
    s.transformed = true;
    companionUniforms(s, g, TrackPivot::Origin, nullptr, -1, -1, 0.0f, u);
    auto map = [&](double x, double y, double& ox, double& oy){
        ox = u.vertex0[0] * x + u.vertex0[1] * y + u.vertex0[2];
        oy = u.vertex1[0] * x + u.vertex1[1] * y + u.vertex1[2];
    };
    double x, y;
    map(0.5, 0.5, x, y);
    EXPECT_NEAR(x, 0.5, 1e-6);
    EXPECT_NEAR(y, 0.5, 1e-6);
    map(0.0, 0.0, x, y);
    EXPECT_NEAR(x, -0.5, 1e-6);
    EXPECT_NEAR(y, -0.5, 1e-6);

    // rotation 90 clockwise about a pivot 40 px right of the centre (+y up)
    s = TitlePoseSummary();
    s.rotation = 90.0;
    s.pivot[0] = 40.0;
    companionUniforms(s, g, TrackPivot::Origin, nullptr, -1, -1, 0.0f, u);
    // the pivot (240, 100 frame px) stays
    map(240.0 / 400.0, 0.5, x, y);
    EXPECT_NEAR(x, 0.6, 1e-6);
    EXPECT_NEAR(y, 0.5, 1e-6);
    // a point right of the pivot goes below it (clockwise on screen, y down)
    map(340.0 / 400.0, 0.5, x, y);
    EXPECT_NEAR(x * 400.0, 240.0, 1e-4);
    EXPECT_NEAR(y * 200.0, 200.0, 1e-4);

    // position +y up moves the frame up the screen; a pivot at the ink's bottom
    s = TitlePoseSummary();
    s.position[1] = 20.0;
    s.stretch[1] = 0.5;
    companionUniforms(s, g, TrackPivot::Bottom, nullptr, -1, -1, 0.0f, u);
    // the ink's bottom (canvas y 220 -> frame y 170) stays put by the stretch, then moves 20 px up
    map(0.5, 170.0 / 200.0, x, y);
    EXPECT_NEAR(y * 200.0, 150.0, 1e-4);

    // opacity and flash
    s = TitlePoseSummary();
    s.opacity = 0.25;
    s.flashAmount = 1.5;
    s.flashColor[0] = 1.0;
    s.flashColor[1] = 0.5;
    s.flashColor[2] = 0.0;
    companionUniforms(s, g, TrackPivot::Origin, nullptr, -1, -1, 0.0f, u);
    EXPECT_FLOAT_EQ(u.affine0[3], 0.25f);
    EXPECT_FLOAT_EQ(u.color[3], 1.5f);
    EXPECT_NEAR(u.color[1], 0.2140411f, 1e-5) << "flash colour in linear";

    // a flipbook frame larger than the still: the quad grows onto it, the rect picks the frame
    CompiledFlipbook fb;
    fb.frameX = 80;
    fb.frameY = 50;
    fb.frameWidth = 440;
    fb.frameHeight = 200;
    fb.pixelWidth = 440;
    fb.pixelHeight = 200;
    fb.pages.push_back({"p0.png", "", 1024, 1024});
    fb.frames.push_back({0, 0, 0});
    fb.frames.push_back({0, 444, 0});
    fb.blend = true;
    companionUniforms(TitlePoseSummary(), g, TrackPivot::Origin, &fb, 1, 0, 0.5f, u);
    map(0.0, 0.0, x, y);
    EXPECT_NEAR(x * 400.0, -20.0, 1e-4);
    map(1.0, 1.0, x, y);
    EXPECT_NEAR(x * 400.0, 420.0, 1e-4);
    EXPECT_FLOAT_EQ(u.frameRect[0], 444.0f / 1024.0f);
    EXPECT_FLOAT_EQ(u.frameRect[2], 440.0f / 1024.0f);
    EXPECT_FLOAT_EQ(u.frameRect2[0], 0.0f);
    EXPECT_FLOAT_EQ(u.affine1[3], 0.5f);
}

TEST(TitleAnimPlayerTest, PlayChainsNextInTheSameFrame){
    TitleAnimPlayer player;
    player.setLibrary(library());
    EXPECT_FALSE(player.play("Nope")) << "an unknown name changes nothing";
    EXPECT_FALSE(player.active());
    ASSERT_TRUE(player.play("Intro"));
    // the start pose shows before the first frame advances
    TitlePoseSummary s;
    player.summary(nullptr, s);
    EXPECT_DOUBLE_EQ(s.scale, 2.0);
    EXPECT_DOUBLE_EQ(s.opacity, 0.0);
    std::vector<FiredMarker> markers;
    std::vector<std::string> finished;
    player.update(0.1, markers, finished);
    EXPECT_DOUBLE_EQ(player.baseTime(), 0.0) << "the first update shows the start";
    for (int i = 0; i < 4; i++) player.update(0.1, markers, finished);
    ASSERT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].marker->name, "onImpact");
    EXPECT_TRUE(finished.empty());
    markers.clear();
    // 0.4 + 0.2: Intro ends at 0.5, Idle starts in the same frame and runs the other 0.1 s
    player.update(0.2, markers, finished);
    EXPECT_EQ(finished, (std::vector<std::string>{"Intro"}));
    EXPECT_EQ(player.baseName(), "Idle");
    EXPECT_TRUE(player.isPlaying("Idle"));
    EXPECT_NEAR(player.baseTime(), 0.1, 1e-9);
    ASSERT_EQ(markers.size(), 1u) << "Idle's marker at 0 fires as it starts";
    EXPECT_EQ(markers[0].marker->name, "onBeat");
}

TEST(TitleAnimPlayerTest, OverlaysStopAndReset){
    TitleAnimPlayer player;
    player.setLibrary(library());
    std::vector<FiredMarker> markers;
    std::vector<std::string> finished;
    ASSERT_TRUE(player.play("Idle"));
    player.update(0.0, markers, finished);
    player.update(0.5, markers, finished);
    ASSERT_TRUE(player.playOverlay("Shake"));
    player.update(0.0, markers, finished);
    player.update(0.2, markers, finished);
    TitlePoseSummary s;
    player.summary(nullptr, s);
    EXPECT_NEAR(s.position[0], 10.0, 1e-4) << "the shake over Idle";
    EXPECT_TRUE(s.liveSet[(size_t)TitleField::SweepPosition]) << "Idle's sweep goes on under it";
    EXPECT_NEAR(s.live[(size_t)TitleField::SweepPosition], 0.7, 1e-4);
    player.update(0.3, markers, finished);
    EXPECT_TRUE(player.overlays().empty()) << "a once overlay ends on its own";
    EXPECT_EQ(finished.back(), "Shake");
    // stop keeps the pose
    player.stop();
    EXPECT_FALSE(player.isPlaying());
    EXPECT_TRUE(player.active());
    const double held = player.baseTime();
    player.update(0.5, markers, finished);
    EXPECT_EQ(player.baseTime(), held);
    // reset: the still again
    player.reset();
    EXPECT_FALSE(player.active());
    player.summary(nullptr, s);
    EXPECT_FALSE(s.any());
    // sample shows a time and pauses
    ASSERT_TRUE(player.sample("Intro", 0.25));
    player.update(1.0, markers, finished);
    EXPECT_DOUBLE_EQ(player.baseTime(), 0.25);
    EXPECT_TRUE(player.isPaused());
}

TEST(TitleAnimPlayerTest, FadeBlendsFromTheCurrentPose){
    TitleAnimPlayer player;
    player.setLibrary(library());
    std::vector<FiredMarker> markers;
    std::vector<std::string> finished;
    TitleAnimPlayOptions options;
    ASSERT_TRUE(player.play("Intro", options));
    player.update(0.0, markers, finished);
    options.fade = 0.2f;
    ASSERT_TRUE(player.play("Shake", options));
    player.update(0.0, markers, finished);
    player.update(0.1, markers, finished);
    TitlePoseSummary s;
    player.summary(nullptr, s);
    // half way: Intro's scale 2 toward Shake's identity 1
    EXPECT_NEAR(s.scale, 1.5, 1e-6);
    player.update(0.2, markers, finished);
    player.summary(nullptr, s);
    EXPECT_NEAR(s.scale, 1.0, 1e-6);
}

TEST(TitleAnimPlayerTest, DrivenTimeFiresForwardOnly){
    TitleAnimPlayer player;
    player.setLibrary(library());
    std::vector<FiredMarker> markers;
    ASSERT_TRUE(player.drive("Intro", 0.0, &markers));
    ASSERT_TRUE(player.drive("Intro", 0.4, &markers));
    ASSERT_EQ(markers.size(), 1u);
    markers.clear();
    ASSERT_TRUE(player.drive("Intro", 0.1, &markers));
    EXPECT_TRUE(markers.empty()) << "backward: none";
    std::vector<std::string> finished;
    player.update(1.0, markers, finished);
    EXPECT_DOUBLE_EQ(player.baseTime(), 0.1) << "a driven animation does not advance itself";
}

TEST(TitleAnimPlayerTest, ZeroAllocSteadyState){
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this binary cannot count allocations";
    TitleAnimPlayer player;
    player.setLibrary(library());
    std::vector<FiredMarker> markers;
    markers.reserve(64);
    std::vector<std::string> finished;
    finished.reserve(8);
    ASSERT_TRUE(player.play("Idle"));
    ASSERT_TRUE(player.playOverlay("Shake", {1.0f, 1, 0.0f, 0.0}));
    TitlePoseSummary s;
    CompanionAnimUniforms u;
    const TitleFrameGeometry g = frame();
    for (int i = 0; i < 60; i++){
        markers.clear();
        player.update(1.0 / 60.0, markers, finished);
    }
    testsupport::AllocationCountScope scope;
    for (int i = 0; i < 600; i++){
        markers.clear();
        player.update(1.0 / 60.0, markers, finished);
        player.summary(nullptr, s);
        companionUniforms(s, g, TrackPivot::Origin, nullptr, -1, -1, 0.0f, u);
    }
    EXPECT_EQ(scope.count(), 0u);
}

TEST(TitleAnimDataIOTest, RoundTrip){
    TitleAnimData data;
    data.sourceHash = "0123456789abcdef";
    data.bakeVersion = 3;
    data.defaultName = "Intro";
    TitleAnimDataEntry e;
    e.authored.id = "7c1e0a42";
    e.authored.name = "Intro";
    e.authored.role = AnimRole::Intro;
    e.authored.duration = 0.8f;
    e.authored.next = "Idle";
    TitleTrack scale = track("transform.scale", {{0.0f, {2.6f}}, {0.48f, {0.94f}}, {0.8f, {1.0f}}}, true);
    scale.keys[0].key.ease = EaseType::QUAD_IN;
    scale.keys[1].key.ease = EaseType::SINE_OUT;
    e.authored.tracks.push_back(scale);
    TitleTrack breathe;
    breathe.field = "opacity";
    breathe.drive = TitleDrive{{0.5f}, {1.0f}, 1.0f, 0.0f, false, EaseType::SINE_IN_OUT};
    e.authored.tracks.push_back(breathe);
    TitleMarker m;
    m.time = 0.48f;
    m.name = "onImpact";
    m.args = {AnimationEvent::numberArg(2), AnimationEvent::stringArg("big")};
    m.sound = AssetRef("sounds/stamp.wav");
    e.authored.markers.push_back(m);
    e.compiled.flipbook.fps = 30;
    e.compiled.flipbook.frameX = -760;
    e.compiled.flipbook.frameY = 360;
    e.compiled.flipbook.frameWidth = 1520;
    e.compiled.flipbook.frameHeight = 720;
    e.compiled.flipbook.pixelWidth = 1520;
    e.compiled.flipbook.pixelHeight = 720;
    e.compiled.flipbook.pages.push_back({"logo.titleart.7c1e0a42.flip0.png", "00112233445566778899aabbccddeeff", 2048, 2048});
    e.compiled.flipbook.frames = {{0, 0, 0}, {0, 0, 724}, {0, 0, 724}};
    e.compiled.bakedCost.frames = 3;
    e.compiled.bakedCost.unique = 2;
    e.compiled.bakedCost.pages = 1;
    e.compiled.bakedCost.bytesMobile = 1866240;
    data.animations.push_back(e);

    const std::string text = writeAnimData(data);
    TitleAnimData read;
    std::string error;
    ASSERT_TRUE(readAnimData(text, read, nullptr, &error)) << error;
    EXPECT_EQ(writeAnimData(read), text);
    ASSERT_EQ(read.animations.size(), 1u);
    const CompiledTitleAnimation& c = read.animations[0].compiled;
    EXPECT_EQ(c.tracks.size(), 2u);
    EXPECT_EQ(c.markers.size(), 1u);
    EXPECT_EQ(c.flipbook.frames.size(), 3u);
    EXPECT_EQ(c.bakedCost.bytesMobile, 1866240u);
    EXPECT_NEAR(c.duration, 0.8, 1e-6);
    EXPECT_EQ(read.find("Intro"), &read.animations[0]);
}

TEST(TitleAnimDataIOTest, NewerRefused){
    TitleAnimData read;
    std::string error;
    EXPECT_FALSE(readAnimData(R"({"format": "jasperengine.titleanim", "version": 2, "animations": []})", read, nullptr, &error));
    EXPECT_NE(error.find("newer"), std::string::npos) << error;
    EXPECT_FALSE(readAnimData(R"({"format": "jasperengine.titleart", "version": 1})", read, nullptr, &error));
    EXPECT_FALSE(readAnimData("not json", read, nullptr, &error));
}

TEST(TitleAnimDataIOTest, LenientDropsWithJsonPath){
    TitleAnimationSet set;
    std::vector<TitleAnimWarning> warnings;
    AnimJson member = AnimJson::parse(R"({"fps": 500, "default": "Gone", "animations": [
        {"id": "00000001", "name": "Intro", "duration": 0.5, "tracks": [
            {"layer": "", "field": "opacity", "keys": [[0, 0], [0.5], [0.4, 1]]},
            {"layer": "", "field": "opacity", "keys": [[0, 0]], "drive": {"from": 0, "to": 1}}],
         "markers": [{"time": 0.1, "name": "end"}, {"time": 0.2, "name": "ok", "x": 1}]},
        {"name": "Intro", "wrap": "sideways", "speed": 0}]})");
    decodeAnimationSet(member, "animation", set, &warnings);
    auto has = [&](const std::string& path){
        for (const TitleAnimWarning& w : warnings) if (w.path == path) return true;
        return false;
    };
    EXPECT_TRUE(has("animation.fps"));
    EXPECT_TRUE(has("animation.default"));
    EXPECT_TRUE(has("animation.animations[0].tracks[0].keys[1]"));
    EXPECT_TRUE(has("animation.animations[0].tracks[1]")) << "keys and a drive: dropped";
    EXPECT_TRUE(has("animation.animations[0].markers[0].name")) << "'end' is a Lua keyword";
    EXPECT_TRUE(has("animation.animations[1].wrap"));
    EXPECT_TRUE(has("animation.animations[1].speed"));
    ASSERT_EQ(set.animations.size(), 2u);
    EXPECT_EQ(set.fps, 120);
    EXPECT_EQ(set.defaultName, "");
    EXPECT_EQ(set.animations[1].name, "Intro 2");
    ASSERT_EQ(set.animations[0].tracks.size(), 1u);
    EXPECT_EQ(set.animations[0].tracks[0].keys.size(), 2u);
    ASSERT_EQ(set.animations[0].markers.size(), 1u);
    // unknown members are kept and written back
    EXPECT_EQ(encodeMarker(set.animations[0].markers[0]).value("x", 0), 1);
}
