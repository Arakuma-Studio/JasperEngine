// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Goldens of <video> (docs/html-ui/phase-6-video.md §5.2): pages of fixtures/video/golden drawn by
// the FixtureRunner (4x MSAA, the engine's default) at fixed UI times and compared with
// golden/video/<name>.png, the same images on glcore and Vulkan (the harness's default
// tolerance). Every test also reads the frame-index pattern of the test card where its page puts
// it, so a golden cannot pin a wrong frame.

#include "VideoTestCommon.h"

#include "FixtureRunner.h"
#include "harness/GpuEnvironment.h"
#include "harness/ImageCompare.h"
#include "harness/TestSystem.h"

#include "HtmlUIClock.h"
#include "effects/EffectRuntime.h"
#include "media/UIVideoControls.h"
#include "media/UIVideoElement.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class VideoGoldenTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            TestSystem::get().clearLogs();
            htmlui::EffectClock::setFixedTime(1.0);
            runner = std::make_unique<FixtureRunner>(640, 400);
            if (!runner->valid())
                GTEST_SKIP() << "no HtmlUI or GPU";
        }

        void TearDown() override{
            runner.reset();
            htmlui::EffectClock::setFixedTime(std::nullopt);
            HtmlUIClock::reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
        }

        void load(const std::string& page){
            std::string error;
            ASSERT_TRUE(runner->load("fixtures/video/golden/" + page, &error)) << error;
            runner->frameAt(0.0);
            runner->frameAt(0.0);
        }

        UIVideoElement* video(const char* id){
            return rmlui_dynamic_cast<UIVideoElement*>(runner->document()->GetElementById(id));
        }

        void seek(const char* id, double seconds){
            UIVideoElement* v = video(id);
            ASSERT_NE(v, nullptr) << id;
            v->setCurrentTime(seconds);
        }

        // two frames (the uploads and the conversions of the seeks), then the capture
        compat::Image capture(){
            runner->frameAt(runner->time(), true);
            FixtureRunner::Capture capture;
            EXPECT_TRUE(runner->capture(capture, true));
            return capture.image;
        }

        // the frame index of a test card drawn unclipped at (x, y), at `scale`
        static int indexAt(const compat::Image& image, int x, int y, double scale = 1.0){
            return cardIndexOfRgba(image.rgba.data(), image.width * 4, x, y, scale);
        }

        void golden(const std::string& name, const compat::Image& image){
            const CompareResult result = compareWithGolden("video/" + name, image.rgba, image.width, image.height);
            EXPECT_TRUE(result.passed) << result.message;
        }

        std::unique_ptr<FixtureRunner> runner;
    };

}

TEST_F(VideoGoldenTest, FramesAtFixedTimes){
    load("depth.html");
    struct Shot { const char* name; int frame; } shots[] = {{"frames.t0", 0}, {"frames.t05", 15}, {"frames.t1", 30}};
    for (const Shot& shot : shots){
        for (const char* id : {"eight", "ten", "full"})
            seek(id, cardTime(shot.frame));
        const compat::Image image = capture();
        // 8-bit, 10-bit and 4:4:4 (0.5 s long: its last frame after that) at 8, 216 and 424 px
        EXPECT_EQ(indexAt(image, 8, 8, 0.625), shot.frame) << shot.name;
        EXPECT_EQ(indexAt(image, 216, 8, 0.625), shot.frame) << shot.name;
        EXPECT_EQ(indexAt(image, 424, 8, 0.625), std::min(shot.frame, 14)) << shot.name;
        golden(shot.name, image);
    }
}

TEST_F(VideoGoldenTest, ObjectFitAndPosition){
    load("fit.html");
    for (const char* id : {"fill", "contain", "cover", "none", "scale"})
        seek(id, cardTime(21));
    const compat::Image image = capture();
    // contain: 200x112.5 centred in 200x150 (x 216, y 8 + 18.75); scale 0.625
    EXPECT_EQ(indexAt(image, 216, 27, 0.625), 21);
    golden("fit", image);
}

TEST_F(VideoGoldenTest, RoundedContentAndOpacity){
    load("rounded.html");
    for (const char* id : {"radius", "circle", "tinted"})
        seek(id, cardTime(44));
    const compat::Image image = capture();
    // the bordered video: 240x135 at (16, 16), cover of 320x180 (exact scale 0.75)
    EXPECT_EQ(indexAt(image, 16, 16, 0.75), 44);
    golden("rounded", image);
}

TEST_F(VideoGoldenTest, FiltersAndEffects){
    load("effects.html");
    for (const char* id : {"gray", "fx", "under"})
        seek(id, cardTime(37));
    const compat::Image image = capture();
    golden("effects", image);
}

TEST_F(VideoGoldenTest, Poster){
    load("poster.html");
    const compat::Image image = capture();
    EXPECT_TRUE(video("poster")->isShowingPoster());
    EXPECT_TRUE(video("framed")->isShowingPoster());
    golden("poster", image);
    // once playing, the frame replaces the poster
    video("framed")->play();
    runner->frameAt(0.5);
    const compat::Image playing = capture();
    EXPECT_FALSE(video("framed")->isShowingPoster());
    golden("poster.playing", playing);
}

TEST_F(VideoGoldenTest, ControlsPausedHoveredFocused){
    load("controls.html");
    seek("v", cardTime(30));
    golden("controls.paused", capture());
    UIVideoElement* v = video("v");
    v->setLoop(true);
    v->play();
    runner->frameAt(0.3);
    runner->frameAt(0.6);
    // playing, the pointer away: the bar has faded out (0.25 s transition)
    EXPECT_FALSE(v->getControlsWidget()->isShown());
    golden("controls.playing", capture());
    // hovered while playing: shown
    runner->context()->ProcessMouseMove(200, 120, 0);
    runner->frameAt(0.6);
    v->pause();
    v->setCurrentTime(1.0);
    runner->frameAt(0.6);
    runner->frameAt(0.9);
    golden("controls.hovered", capture());
    // muted icon
    v->setMuted(true);
    runner->frameAt(1.0);
    golden("controls.muted", capture());
}
