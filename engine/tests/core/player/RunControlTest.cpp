// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "player/RunControl.h"
#include "Log.h"
#include "TempDir.h"

#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::player;

namespace {

    struct RunControlFixture : public ::testing::Test {
        void TearDown() override { RunControl::reset(); }
    };

    // a 2x2 frame: red, green / blue, white
    FrameReader fakeFrame(int* reads) {
        return [reads](std::vector<uint8_t>& rgba, int& width, int& height, std::string&) {
            (*reads)++;
            width = 2;
            height = 2;
            rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 0};
            return true;
        };
    }

}

TEST_F(RunControlFixture, InactiveWithoutFrameOptions) {
    LaunchOptions options;
    options.headless = true;
    RunControl::configure(options);
    EXPECT_FALSE(RunControl::isActive());
    EXPECT_FALSE(RunControl::wantsOffscreenFrame());
    EXPECT_EQ(RunControl::finish(), EXIT_OK);
}

TEST_F(RunControlFixture, QuitsAfterExactlyNFrames) {
    LaunchOptions options;
    options.quitAfter = 120;
    RunControl::configure(options);
    for (int frame = 1; frame <= 119; frame++) {
        RunControl::frameDrawn(false, {});
        ASSERT_FALSE(RunControl::quitRequested()) << frame;
    }
    RunControl::frameDrawn(false, {});
    EXPECT_TRUE(RunControl::quitRequested());
    EXPECT_EQ(RunControl::countedFrames(), 120u);
    EXPECT_EQ(RunControl::finish(), EXIT_OK);
}

TEST_F(RunControlFixture, AfterReadyCountsFromTheReadyFrame) {
    LaunchOptions options;
    options.afterReady = true;
    options.quitAfter = 3;
    RunControl::configure(options);
    EXPECT_TRUE(RunControl::wantsReadySignal());
    for (int i = 0; i < 50; i++) RunControl::frameDrawn(false, {});
    EXPECT_EQ(RunControl::countedFrames(), 0u);
    RunControl::frameDrawn(true, {});
    EXPECT_EQ(RunControl::countedFrames(), 1u);
    RunControl::frameDrawn(true, {});
    EXPECT_FALSE(RunControl::quitRequested());
    RunControl::frameDrawn(true, {});
    EXPECT_TRUE(RunControl::quitRequested());
}

TEST_F(RunControlFixture, ReadyTimeoutExitsSix) {
    LaunchOptions options;
    options.afterReady = true;
    options.readyTimeout = 0.001;
    RunControl::configure(options);
    RunControl::engineStarted();
    // (two clock reads a few hundred ns apart are under any timeout: the wait has to pass)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    RunControl::frameDrawn(false, {});
    EXPECT_TRUE(RunControl::quitRequested());
    EXPECT_EQ(RunControl::finish(), EXIT_NOT_READY);
}

TEST_F(RunControlFixture, ScreenshotAtItsFrameIsAnOpaquePng) {
    test::TempDir dir;
    LaunchOptions options;
    options.quitAfter = 5;
    options.screenshotFrame = 3;
    options.screenshot = dir.path() / "shot.png";
    RunControl::configure(options);
    int reads = 0;
    std::vector<bool> offscreen;
    for (int frame = 1; frame <= 5; frame++) {
        const bool wanted = RunControl::wantsOffscreenFrame();
        offscreen.push_back(wanted);
        RunControl::frameDrawn(false, wanted ? fakeFrame(&reads) : FrameReader());
    }
    EXPECT_EQ(offscreen, (std::vector<bool>{false, false, true, false, false}));
    EXPECT_EQ(reads, 1);
    const std::string png = dir.read("shot.png");
    ASSERT_GT(png.size(), 8u);
    EXPECT_EQ(png.substr(1, 3), "PNG");
    EXPECT_EQ(RunControl::finish(), EXIT_OK);
}

TEST_F(RunControlFixture, ScreenshotDefaultsToTheLastFrame) {
    test::TempDir dir;
    LaunchOptions options;
    options.quitAfter = 4;
    options.screenshot = dir.path() / "last.png";
    RunControl::configure(options);
    int reads = 0;
    for (int frame = 1; frame <= 4; frame++) {
        const bool wanted = RunControl::wantsOffscreenFrame();
        EXPECT_EQ(wanted, frame == 4) << frame;
        RunControl::frameDrawn(false, wanted ? fakeFrame(&reads) : FrameReader());
    }
    EXPECT_EQ(reads, 1);
    EXPECT_TRUE(dir.exists("last.png"));
}

TEST_F(RunControlFixture, FailedCaptureExitsFour) {
    test::TempDir dir;
    LaunchOptions options;
    options.quitAfter = 1;
    options.screenshot = dir.path() / "x.png";
    RunControl::configure(options);
    RunControl::frameDrawn(false, [](std::vector<uint8_t>&, int&, int&, std::string& error) {
        error = "no GPU";
        return false;
    });
    EXPECT_EQ(RunControl::finish(), EXIT_SCREENSHOT_FAILED);
}

TEST_F(RunControlFixture, ScreenshotNeverTakenExitsFour) {
    test::TempDir dir;
    LaunchOptions options;
    options.quitAfter = 10;
    options.screenshot = dir.path() / "x.png";
    RunControl::configure(options);
    RunControl::frameDrawn(false, {});
    EXPECT_EQ(RunControl::finish(), EXIT_SCREENSHOT_FAILED);
}

TEST_F(RunControlFixture, FailOnErrorExitsThree) {
    LaunchOptions options;
    options.failOnError = true;
    options.quitAfter = 2;
    RunControl::configure(options);
    RunControl::engineStarted();
    RunControl::frameDrawn(false, {});
    Log::error("a deliberate test error (RunControlFixture.FailOnErrorExitsThree)");
    RunControl::frameDrawn(false, {});
    EXPECT_EQ(RunControl::finish(), EXIT_ERROR_LOGGED);
}

TEST_F(RunControlFixture, FirstQuitCodeWins) {
    LaunchOptions options;
    options.quitAfter = 100;
    RunControl::configure(options);
    RunControl::requestQuit(7);
    RunControl::requestQuit(0);
    EXPECT_EQ(RunControl::finish(), 7);
}
