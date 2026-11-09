// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85 AC20 (R35, R36) on the test machine: `TitleAnimBench.Sample64` is a 64-glyph title with 40
// tracks (8 staggered glyph tracks, 4 drives, 2 reseeds, 26 keyed layer tracks) and 8 markers,
// compiled by the module from the document; `.Scene30` is 30 Baked titles each playing a flipbook
// title-live track and an overlay, through the player and the companion's uniforms. Medians print
// for the record: the budgets are for server E's Release build, and this suite is a Debug build, so
// the assertions are ten times the budgets.

#include <gtest/gtest.h>

#include "support/AllocationCounter.h"
#include "titleanim/TitleAnimPlayer.h"
#include "titleart/TitleArtIO.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/anim/TitleAnimRealtime.h"
#include "titleart/text/TitleTextLayout.h"

#include "../render/TitleRenderTestSupport.h"
#include "../support/TitleTestAssets.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;
namespace ta = jasperengine::titleanim;

namespace {

    Json keyed(const char* layer, const char* field, Json a, Json b){
        return Json{{"layer", layer}, {"field", field}, {"keys", Json::array({Json::array({0, std::move(a)}), Json::array({1.0, std::move(b)})})}};
    }

    std::shared_ptr<const TitleArtDoc> benchDoc(){
        std::string text;
        for (int i = 0; i < 64; i++) text += "abcdefghijklmnop"[i % 16];
        Json layers = Json::array();
        layers.push_back(Json{{"id", "00000001"}, {"type", "text"}, {"name", "Text"}, {"text", text}, {"font", fontRef(SOURCE_SANS)}, {"size", 24}, {"align", "start"}});
        for (int i = 0; i < 9; i++){
            char id[9];
            std::snprintf(id, sizeof(id), "%08x", 0x10 + i);
            layers.push_back(shapeLayer(id, "rect", 40, solidFace("#ffffff")));
        }
        Json tracks = Json::array();
        const char* glyphFields[] = {"glyph.opacity", "glyph.offset", "glyph.rotation", "glyph.scale", "glyph.stretch", "glyph.tint", "glyph.flash.amount", "glyph.flash.color"};
        Json glyphA[] = {0.0, Json::array({0, 0}), 0.0, 1.0, Json::array({1, 1}), "#ffffff", 0.0, "#ffffff"};
        Json glyphB[] = {1.0, Json::array({5, 5}), 15.0, 1.5, Json::array({1, 2}), "#ff0000", 0.5, "#00ff00"};
        for (int i = 0; i < 8; i++){
            Json track = keyed("00000001", glyphFields[i], glyphA[i], glyphB[i]);
            track["stagger"] = {{"each", 0.01}, {"order", "forward"}};
            tracks.push_back(track);
        }
        // 26 keyed layer tracks: three per plate
        for (int i = 0; i < 9 && tracks.size() < 34; i++){
            char id[9];
            std::snprintf(id, sizeof(id), "%08x", 0x10 + i);
            tracks.push_back(keyed(id, "opacity", 0.0, 1.0));
            if (tracks.size() < 34) tracks.push_back(keyed(id, "transform.position", Json::array({0, 0}), Json::array({10, 0})));
            if (tracks.size() < 34) tracks.push_back(keyed(id, "transform.rotation", 0.0, 30.0));
        }
        auto drive = [](const char* field){ return Json{{"layer", ""}, {"field", field}, {"drive", {{"from", Json::array({0})}, {"to", Json::array({1})}, {"cycles", 1}}}}; };
        tracks.push_back(drive("transform.rotation"));
        tracks.push_back(drive("live.sweep_position"));
        tracks.push_back(drive("live.tint_amount"));
        tracks.push_back(drive("live.glow_pulse"));
        tracks.push_back(Json{{"layer", ""}, {"field", "seed"}, {"reseed", {{"rate", 8}}}});
        tracks.push_back(Json{{"layer", "00000001"}, {"field", "seed"}, {"reseed", {{"rate", 6}}}});
        Json markers = Json::array();
        for (int i = 0; i < 8; i++) markers.push_back(Json{{"name", "m" + std::to_string(i)}, {"time", 0.1 * i}});
        Json animation = {{"name", "Bench"}, {"duration", 1.0}, {"wrap", "loop"}, {"tracks", tracks}, {"markers", markers}};
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
        root["canvas"] = canvas(1024, 512);
        root["layers"] = layers;
        root["animation"] = {{"animations", Json::array({animation})}};
        TitleArtParseResult parsed = parseTitleArtJson(std::move(root));
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return std::make_shared<const TitleArtDoc>(std::move(parsed.doc));
    }

    double percentile(std::vector<double> us, double p){
        std::sort(us.begin(), us.end());
        return us[std::min(us.size() - 1, (size_t)(us.size() * p))];
    }

}

TEST(TitleAnimBench, Sample64){
    auto doc = benchDoc();
    TestAssetResolver assets;
    TitleTextLayout layout(&assets);
    std::vector<TitleProblem> problems;
    auto library = compileAnimations(*doc, &layout, &problems);
    ASSERT_EQ(library->animations.size(), 1u);
    const ta::CompiledTitleAnimation& compiled = library->animations[0];
    EXPECT_EQ(compiled.tracks.size(), 40u);
    int orphan = 0;
    for (const ta::CompiledTitleTrack& t : compiled.tracks) orphan += t.cls == ta::TrackClass::None ? 1 : 0;
    EXPECT_EQ(orphan, 0) << "every field of the bench resolved";

    ta::TitleAnimPlayer player;
    player.setLibrary(library);
    ASSERT_TRUE(player.play("Bench"));
    std::vector<ta::FiredMarker> markers;
    markers.reserve(64);
    std::vector<std::string> finished;
    finished.reserve(4);
    for (int i = 0; i < 60; i++){
        markers.clear();
        player.update(1.0 / 60.0, markers, finished);
    }
    std::vector<double> samples;
    samples.reserve(600);
    uint64_t allocations = 0;
    {
        testsupport::AllocationCountScope scope;
        for (int i = 0; i < 600; i++){
            const auto start = std::chrono::steady_clock::now();
            markers.clear();
            player.update(1.0 / 60.0, markers, finished);
            const double elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            samples.push_back(elapsed);
        }
        allocations = scope.count();
    }
    const double median = percentile(samples, 0.5);
    std::printf("[title anim] Sample64: 40 tracks, 64 glyphs: median %.2f us, p95 %.2f us, %llu allocations over 600 frames (R35: 25 us median, 0)\n", median,
                percentile(samples, 0.95), (unsigned long long)allocations);
    EXPECT_LT(median, 250.0);
    if (testsupport::countsAllocations()) EXPECT_EQ(allocations, 0u) << "sampling allocates nothing in steady state";

    // the pose the runtime builds from it each frame (re-render requests and glyph quads)
    std::vector<double> poses;
    for (int i = 0; i < 120; i++){
        markers.clear();
        player.update(1.0 / 60.0, markers, finished);
        const auto start = std::chrono::steady_clock::now();
        PlayerPose pose;
        playerPose(*doc, player, true, nullptr, pose);
        poses.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("[title anim] Sample64 pose: median %.2f us over 120 frames\n", percentile(poses, 0.5));
}

TEST(TitleAnimBench, Scene30){
    // 30 titles, each a flipbook-style title-live animation with an overlay on top (the system's
    // per-title work besides its ECS loop: the player and the companion's uniforms)
    auto library = std::make_shared<ta::TitleAnimLibrary>();
    {
        ta::TitleAnimation base;
        base.name = "Base";
        base.duration = 1.0f;
        base.wrap = ta::AnimWrap::Loop;
        for (const char* field : {"opacity", "transform.position", "transform.rotation", "flash.amount"}){
            ta::TitleTrack track;
            track.field = field;
            ta::TitleKey a, b;
            a.key.time = 0.0f;
            b.key.time = 1.0f;
            const bool vec2 = std::string(field) == "transform.position";
            a.key.value = vec2 ? std::vector<float>{0.0f, 0.0f} : std::vector<float>{0.0f};
            b.key.value = vec2 ? std::vector<float>{10.0f, 0.0f} : std::vector<float>{1.0f};
            track.keys = {a, b};
            base.tracks.push_back(track);
        }
        ta::CompiledTitleAnimation compiled;
        ta::compileTitleOnly(base, compiled, nullptr);
        library->animations.push_back(std::move(compiled));
        ta::TitleAnimation over = base;
        over.name = "Over";
        over.overlay = true;
        ta::CompiledTitleAnimation compiledOver;
        ta::compileTitleOnly(over, compiledOver, nullptr);
        library->animations.push_back(std::move(compiledOver));
    }
    std::vector<ta::TitleAnimPlayer> players(30);
    for (ta::TitleAnimPlayer& player : players){
        player.setLibrary(library);
        ASSERT_TRUE(player.play("Base"));
        ASSERT_TRUE(player.playOverlay("Over", ta::TitleAnimPlayOptions()));
    }
    ta::TitleFrameGeometry frame;
    frame.frameWidth = 400;
    frame.frameHeight = 200;
    std::vector<ta::FiredMarker> markers;
    markers.reserve(64);
    std::vector<std::string> finished;
    std::vector<double> samples;
    for (int f = 0; f < 600; f++){
        const auto start = std::chrono::steady_clock::now();
        for (ta::TitleAnimPlayer& player : players){
            markers.clear();
            player.update(1.0 / 60.0, markers, finished);
            ta::TitlePoseSummary s;
            ta::CompanionAnimUniforms u;
            player.summary(nullptr, s);
            ta::companionUniforms(s, frame, ta::TrackPivot::Origin, nullptr, -1, -1, 0.0f, u);
        }
        samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    const double p95 = percentile(samples, 0.95);
    std::printf("[title anim] Scene30: 30 titles, median %.1f us, p95 %.1f us a frame (R36: 500 us p95)\n", percentile(samples, 0.5), p95);
    EXPECT_LT(p95, 5000.0);
}
