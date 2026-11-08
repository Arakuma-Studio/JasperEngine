// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The platform lifecycle the engine owns (platform-lifecycle spec D4, D9): the window's
// visibility, a view destroyed that was never loaded, and a loading-thread scope opened on the
// main thread before the view exists.

#include "Engine.h"
#include "LogCapture.h"
#include "log/Logger.h"
#include "support/JobsTestUtil.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

using namespace jasperengine;

namespace {

    const char* callbackTag = "core-tests-engine-lifecycle";

}

TEST(EngineVisibility, ChangeEventsOnceAndResets){
    Engine::resetApplicationState();
    ASSERT_TRUE(Engine::isVisible());

    std::vector<bool> events;
    Engine::onVisibilityChanged.add(callbackTag, [&events](bool visible) { events.push_back(visible); });

    Engine::systemVisibilityChanged(false);
    Engine::systemVisibilityChanged(false);
    EXPECT_FALSE(Engine::isVisible());
    EXPECT_EQ(events, std::vector<bool>({false}));

    Engine::systemVisibilityChanged(true);
    EXPECT_TRUE(Engine::isVisible());
    EXPECT_EQ(events, std::vector<bool>({false, true}));

    Logger::flush();
    const test::LogCapture log = test::LogCapture::sinceTestStart();
    EXPECT_TRUE(log.contains("Window hidden: updating at 60 Hz without drawing"));
    EXPECT_TRUE(log.contains("Window visible: drawing again"));

    // Play start and stop in the editor: visible again, without an event
    Engine::systemVisibilityChanged(false);
    Engine::resetApplicationState();
    EXPECT_TRUE(Engine::isVisible());
    EXPECT_EQ(events.size(), 3u);

    Engine::onVisibilityChanged.remove(callbackTag);
}

// A macOS launch that fails before systemViewLoaded quits through applicationWillTerminate, which
// calls systemViewDestroyed: nothing may run then (no onViewDestroyed, no sg_shutdown without
// sg_setup). In a child process: the destroy without the guard tears the render system down.
TEST(EngineLifecycle, ViewDestroyedWithoutViewIsIgnored){
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        if (Engine::isViewLoaded())
            Engine::systemViewDestroyed();
        int calls = 0;
        Engine::onViewDestroyed.add(callbackTag, [&calls]() { calls++; });
        Engine::systemViewDestroyed();
        std::exit(calls == 0 && !Engine::isViewLoaded() ? 0 : 1);
    }, ::testing::ExitedWithCode(0), "");
}

// A script's init runs on the main thread before the view loads, and a prebuilt player's script
// may call Engine.startAsyncThread there: the main thread is the one that runs the render queue,
// so the scope must not wait for it (the players hung there on every backend). In a child process:
// the view is not loaded there, and a hang is ended by the watchdog.
TEST(EngineLifecycle, LoadingThreadScopeOnTheMainThreadNeverWaits){
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        bool mainThreadAllAlong = true;
        testsupport::runWithWatchdog(30.0, "loading-thread scopes on the main thread", [&mainThreadAllAlong]() {
            // twice: a second commit before a frame ran the first waits too
            for (int scope = 0; scope < 2; scope++){
                Engine::startAsyncThread();
                mainThreadAllAlong = mainThreadAllAlong && !Engine::isAsyncThread();
                Engine::commitThreadQueue();
                Engine::endAsyncThread();
            }
        });
        std::_Exit(mainThreadAllAlong && !Engine::isViewLoaded() ? 0 : 1);
    }, ::testing::ExitedWithCode(0), "");
}
