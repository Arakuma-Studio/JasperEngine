// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85 AC6 (R10, R28, D4): marker crossings on the unwrapped time line. The EventCrossing.* cases
// of the clip sampler are the oracle (a loop section from 0 gives a clip's loop); the loop
// section's passes fire each marker once per pass; a seek fires none. Red control: with the pass
// split disabled, a marker at loop_start fires twice when frames land on the boundary.

#include <gtest/gtest.h>

#include "titleanim/TitleAnimPlayer.h"
#include "titleanim/TitleAnimSampling.h"

#include <memory>

using namespace jasperengine;
using namespace jasperengine::titleanim;

namespace {

    using Names = std::vector<std::string>;

    // EventCrossing's clip: 1 s, markers at 0, 0.25, 0.5 and 1 (the end)
    CompiledTitleAnimation eventAnimation(AnimWrap wrap, double loopStart = 0.0){
        CompiledTitleAnimation a;
        a.name = "Events";
        a.duration = 1.0;
        a.wrap = wrap;
        a.loopStart = loopStart;
        for (float time : {0.0f, 0.25f, 0.5f, 1.0f}){
            CompiledMarker m;
            m.time = time;
            m.name = "at" + std::to_string((int)(time * 100));
            a.markers.push_back(m);
        }
        a.layoutPose();
        return a;
    }

    Names crossed(const CompiledTitleAnimation& a, double from, double to, bool start = false){
        std::vector<CrossedMarker> out;
        collectCrossedMarkers(a, from, to, start, out);
        Names names;
        for (const CrossedMarker& c : out) names.push_back(a.markers[c.index].name);
        return names;
    }

}

TEST(TitleAnimMarkersTest, CrossingCases){
    const CompiledTitleAnimation once = eventAnimation(AnimWrap::Once);
    EXPECT_EQ(crossed(once, 0.0, 0.3), (Names{"at25"}));
    EXPECT_EQ(crossed(once, 0.0, 0.3, true), (Names{"at0", "at25"}));
    EXPECT_EQ(crossed(once, 0.25, 0.5), (Names{"at50"}));
    EXPECT_EQ(crossed(once, 0.6, 5.0), (Names{"at100"}));
    EXPECT_EQ(crossed(once, 0.3, 0.3), (Names{}));
    EXPECT_EQ(crossed(once, 1.0, 0.2), (Names{"at50", "at25"}));
    EXPECT_EQ(crossed(once, 1.0, 0.2, true), (Names{"at100", "at50", "at25"}));
    EXPECT_EQ(crossed(once, 0.5, -1.0), (Names{"at25", "at0"}));
    EXPECT_EQ(crossed(once, 1.0, 1.5), (Names{}));

    const CompiledTitleAnimation loop = eventAnimation(AnimWrap::Loop);
    EXPECT_EQ(crossed(loop, 0.9, 1.3), (Names{"at100", "at0", "at25"}));
    EXPECT_EQ(crossed(loop, 0.6, 1.0), (Names{"at100", "at0"}));
    EXPECT_EQ(crossed(loop, 1.0, 1.2), (Names{}));
    EXPECT_EQ(crossed(loop, 0.0, 0.1, true), (Names{"at0"}));
    EXPECT_EQ(crossed(loop, 1.3, 0.9), (Names{"at25", "at0", "at100"}));
    EXPECT_EQ(crossed(loop, 1.0, 0.9, true), (Names{"at100"}));
    EXPECT_EQ(crossed(loop, 0.3, 3.6), (Names{"at50", "at100", "at0", "at25"}));
    EXPECT_EQ(crossed(loop, 3.6, 0.3), (Names{"at50", "at25", "at0", "at100"}));

    const CompiledTitleAnimation pingPong = eventAnimation(AnimWrap::PingPong);
    EXPECT_EQ(crossed(pingPong, 0.9, 1.6), (Names{"at100", "at50"}));
    EXPECT_EQ(crossed(pingPong, 1.6, 2.1), (Names{"at25", "at0"}));
    EXPECT_EQ(crossed(pingPong, 0.1, 5.0), (Names{"at25", "at50", "at100", "at0"}));

    // positions order markers on the time line
    std::vector<CrossedMarker> out;
    collectCrossedMarkers(loop, 0.9, 2.3, false, out);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_NEAR(out[0].position, 1.0, 1e-9);
    EXPECT_NEAR(out[1].position, 1.0, 1e-9);
    EXPECT_NEAR(out[2].position, 1.25, 1e-9);
    EXPECT_NEAR(out[3].position, 1.5, 1e-9);

    // a marker past an explicit duration never fires
    CompiledTitleAnimation cut = eventAnimation(AnimWrap::Loop);
    cut.duration = 0.4;
    cut.markers = {cut.markers[0]};
    cut.markers[0].time = 0.5f;
    EXPECT_EQ(crossed(cut, 0.0, 3.0, true), (Names{}));
}

TEST(TitleAnimMarkersTest, LoopSectionOncePerPass){
    // D = 2, loop_start 0.5: passes [0, 2], then [2, 3.5), [3.5, 5)...
    CompiledTitleAnimation a = eventAnimation(AnimWrap::Loop, 0.5);
    a.duration = 2.0;
    a.markers.clear();
    for (auto [time, name] : std::vector<std::pair<float, const char*>>{{0.5f, "onLoop"}, {1.5f, "onBeat"}}){
        CompiledMarker m;
        m.time = time;
        m.name = name;
        a.markers.push_back(m);
    }
    // frames of 0.25 s land exactly on every boundary
    std::vector<std::string> fired;
    double t = 0.0;
    bool first = true;
    while (t < 6.25){
        std::vector<CrossedMarker> out;
        collectCrossedMarkers(a, t, t + 0.25, first, out);
        for (const CrossedMarker& c : out) fired.push_back(a.markers[c.index].name);
        first = false;
        t += 0.25;
    }
    // first pass: onLoop at 0.5, onBeat at 1.5; each later pass (1.5 s): onLoop at its start, onBeat 1 s in
    EXPECT_EQ(fired, (Names{"onLoop", "onBeat", "onLoop", "onBeat", "onLoop", "onBeat", "onLoop", "onBeat"}));

    // ping-pong over [0.5, 2]: the turning points once each
    a.wrap = AnimWrap::PingPong;
    fired.clear();
    t = 0.0;
    first = true;
    while (t < 5.0){
        std::vector<CrossedMarker> out;
        collectCrossedMarkers(a, t, t + 0.25, first, out);
        for (const CrossedMarker& c : out) fired.push_back(a.markers[c.index].name);
        first = false;
        t += 0.25;
    }
    // 0.5 onLoop, 1.5 onBeat, back: 2.5 onBeat (local 1.5), 3.5 onLoop (local 0.5), forward: 4.5 onBeat
    EXPECT_EQ(fired, (Names{"onLoop", "onBeat", "onBeat", "onLoop", "onBeat"}));
}

TEST(TitleAnimMarkersTest, PreLoopMarkerFirstPassOnly){
    CompiledTitleAnimation a = eventAnimation(AnimWrap::Loop, 0.5);
    a.duration = 2.0;
    a.markers.clear();
    CompiledMarker punch;
    punch.time = 0.2f;
    punch.name = "onPunch";
    a.markers.push_back(punch);
    EXPECT_EQ(crossed(a, 0.0, 10.0, true), (Names{"onPunch"}));
    EXPECT_EQ(crossed(a, 2.0, 10.0), (Names{})) << "loop passes never reach a time before loop_start";
}

TEST(TitleAnimMarkersTest, SeekFiresNone){
    auto library = std::make_shared<TitleAnimLibrary>();
    library->animations.push_back(eventAnimation(AnimWrap::Once));
    TitleAnimPlayer player;
    player.setLibrary(library);
    ASSERT_TRUE(player.play("Events"));
    std::vector<FiredMarker> markers;
    std::vector<std::string> finished;
    player.update(1.0 / 60.0, markers, finished);
    ASSERT_EQ(markers.size(), 1u) << "the start fires at0";
    markers.clear();
    player.seek(0.75);
    player.update(0.0, markers, finished);
    EXPECT_TRUE(markers.empty()) << "a seek crosses nothing";
    player.update(0.1, markers, finished);
    ASSERT_EQ(markers.size(), 0u);
    player.update(0.2, markers, finished);
    ASSERT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].marker->name, "at100");
    EXPECT_EQ(finished, (std::vector<std::string>{"Events"}));
}

TEST(TitleAnimMarkersTest, RedControlWithoutPassSplit){
    // the pass split is what keeps a marker at loop_start to one firing per pass: without it the
    // frame that ends on the boundary and the next one that starts there both fire it
    CompiledTitleAnimation a = eventAnimation(AnimWrap::Loop, 0.5);
    a.duration = 2.0;
    a.markers.clear();
    CompiledMarker m;
    m.time = 0.5f;
    m.name = "onLoop";
    a.markers.push_back(m);
    auto count = [&](){
        int n = 0;
        for (double t = 0.0; t < 3.4; t += 0.25){
            std::vector<CrossedMarker> out;
            collectCrossedMarkers(a, t, t + 0.25, t == 0.0, out);
            n += (int)out.size();
        }
        return n;
    };
    // up to 3.5: the first pass, then the starts of the second and third passes (2 and 3.5)
    EXPECT_EQ(count(), 3);
    setMarkerPassSplitDisabledForTesting(true);
    const int naive = count();
    setMarkerPassSplitDisabledForTesting(false);
    EXPECT_GT(naive, 3) << "the red control must fail without the split";
}
