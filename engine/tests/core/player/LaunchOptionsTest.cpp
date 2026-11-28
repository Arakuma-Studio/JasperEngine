// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "player/InputScriptRun.h"
#include "player/PlayerLaunch.h"
#include "player/LaunchOptions.h"
#include "player/RunControl.h"
#include "PlatformConfig.h"
#include "TempDir.h"

#include <string>
#include <vector>

namespace LaunchOptionsTest_cpp {

using namespace jasperengine;
using namespace jasperengine::player;

namespace {

    std::variant<LaunchOptions, ParseError> launchOptionsRun(std::vector<std::string> args, ParseMode mode = ParseMode::Strict) {
        return parse(args, mode);
    }

    LaunchOptions ok(std::vector<std::string> args, ParseMode mode = ParseMode::Strict) {
        auto result = launchOptionsRun(std::move(args), mode);
        if (auto* error = std::get_if<ParseError>(&result)) {
            ADD_FAILURE() << error->message;
            return LaunchOptions();
        }
        return std::get<LaunchOptions>(result);
    }

    std::string refused(std::vector<std::string> args, ParseMode mode = ParseMode::Strict) {
        auto result = launchOptionsRun(std::move(args), mode);
        if (auto* error = std::get_if<ParseError>(&result)) return error->message;
        ADD_FAILURE() << "accepted";
        return "";
    }

}

TEST(LaunchOptions, Defaults) {
    const LaunchOptions options = ok({});
    EXPECT_FALSE(options.headless);
    EXPECT_FALSE(options.afterReady);
    EXPECT_FALSE(options.silentAudio);
    EXPECT_FALSE(options.failOnError);
    EXPECT_FALSE(options.verifyContent);
    EXPECT_EQ(options.quitAfter, 0u);
    EXPECT_EQ(options.screenshotFrame, 0u);
    EXPECT_EQ(options.fixedFps, 0.0);
    EXPECT_EQ(options.readyTimeout, 120.0);
    EXPECT_TRUE(options.screenshot.empty());
    EXPECT_TRUE(options.content.empty());
    EXPECT_EQ(options.width, 0);
    EXPECT_EQ(options.height, 0);
    EXPECT_TRUE(options.scene.empty());
    EXPECT_TRUE(options.scriptArgs.empty());
}

TEST(LaunchOptions, EveryFlag) {
    test::TempDir dir;
    const std::string shot = (dir.path() / "game.png").string();
    const LaunchOptions options = ok({"--headless", "--after-ready", "--ready-timeout", "30.5", "--quit-after", "120",
                                      "--fixed-fps=60", "--screenshot", shot, "--screenshot-frame", "100",
                                      "--resolution", "1280x720", "--scene", "Main", "--silent-audio",
                                      "--content", dir.str(), "--log-level", "debug", "--verify-content",
                                      "--fail-on-error", "--profile", "--profile-frames", "10"});
    EXPECT_TRUE(options.headless);
    EXPECT_TRUE(options.afterReady);
    EXPECT_DOUBLE_EQ(options.readyTimeout, 30.5);
    EXPECT_EQ(options.quitAfter, 120u);
    EXPECT_DOUBLE_EQ(options.fixedFps, 60.0);
    EXPECT_EQ(options.screenshot.string(), shot);
    EXPECT_EQ(options.screenshotFrame, 100u);
    EXPECT_EQ(options.width, 1280);
    EXPECT_EQ(options.height, 720);
    EXPECT_EQ(options.scene, "Main");
    EXPECT_TRUE(options.silentAudio);
    EXPECT_EQ(options.content.string(), dir.str());
    EXPECT_EQ(options.logLevel, "debug");
    EXPECT_TRUE(options.verifyContent);
    EXPECT_TRUE(options.failOnError);
}

TEST(LaunchOptions, DoubleDashGoesToTheScripts) {
    const LaunchOptions options = ok({"--quit-after", "5", "--", "--level", "3", "--headless"});
    EXPECT_EQ(options.quitAfter, 5u);
    EXPECT_FALSE(options.headless);
    EXPECT_EQ(options.scriptArgs, (std::vector<std::string>{"--level", "3", "--headless"}));
}

TEST(LaunchOptions, NumbersUseTheWholeString) {
    EXPECT_NE(refused({"--quit-after", "10x"}).find("--quit-after 600"), std::string::npos);
    EXPECT_NE(refused({"--quit-after", " 10"}).find("whole number"), std::string::npos);
    EXPECT_NE(refused({"--quit-after", "-3"}).find("at least 1"), std::string::npos);
    EXPECT_NE(refused({"--quit-after", "1.5"}).find("whole number"), std::string::npos);
    EXPECT_NE(refused({"--fixed-fps", "60fps"}).find("--fixed-fps 60"), std::string::npos);
    EXPECT_NE(refused({"--ready-timeout", "2m"}).find("--ready-timeout 120"), std::string::npos);
    EXPECT_NE(refused({"--screenshot-frame", "3.0", "--screenshot", "x.png"}).find("whole frame number"), std::string::npos);
}

TEST(LaunchOptions, RefusalsSayHowToFixThem) {
    // --quit-after >= 1
    EXPECT_NE(refused({"--quit-after", "0"}).find("at least 1"), std::string::npos);
    // --fixed-fps > 0 and finite
    EXPECT_NE(refused({"--fixed-fps", "0"}).find("above 0"), std::string::npos);
    EXPECT_NE(refused({"--fixed-fps", "-30"}).find("above 0"), std::string::npos);
    EXPECT_NE(refused({"--fixed-fps", "inf"}).find("finite"), std::string::npos);
    EXPECT_NE(refused({"--fixed-fps", "nan"}).find("finite"), std::string::npos);
    // --screenshot needs a writable directory
    const std::string message = refused({"--quit-after", "5", "--screenshot", "/nonexistent-dir-s30/x.png"});
    EXPECT_NE(message.find("does not exist"), std::string::npos) << message;
    // --screenshot-frame <= --quit-after
    test::TempDir dir;
    const std::string shot = (dir.path() / "a.png").string();
    EXPECT_NE(refused({"--quit-after", "10", "--screenshot", shot, "--screenshot-frame", "11"}).find("up to 10"), std::string::npos);
    EXPECT_NE(refused({"--screenshot-frame", "3"}).find("needs --screenshot"), std::string::npos);
    // an unknown -- flag
    const std::string unknown = refused({"--frobnicate"});
    EXPECT_NE(unknown.find("unknown option --frobnicate"), std::string::npos) << unknown;
    EXPECT_NE(unknown.find("--help"), std::string::npos);
    // a stray argument
    EXPECT_NE(refused({"level1"}).find("after \"--\""), std::string::npos);
    // values
    EXPECT_NE(refused({"--quit-after"}).find("needs a value"), std::string::npos);
    EXPECT_NE(refused({"--headless=1"}).find("takes no value"), std::string::npos);
    EXPECT_NE(refused({"--resolution", "1280"}).find("WIDTHxHEIGHT"), std::string::npos);
    EXPECT_NE(refused({"--resolution", "0x720"}).find("between 1"), std::string::npos);
    EXPECT_NE(refused({"--content", "/nonexistent-dir-s30"}).find("not a directory"), std::string::npos);
    EXPECT_NE(refused({"--scene", ""}).find("scene name"), std::string::npos);
    EXPECT_NE(refused({"--help"}).find("--quit-after N"), std::string::npos);
    // --command: a console line (and, in a player whose Console Commands keep typed commands
    // off, RunControl's refusal: exit 2)
    EXPECT_NE(refused({"--command", "  "}).find("needs a console line"), std::string::npos);
    EXPECT_NE(refused({"--command"}).find("needs a value"), std::string::npos);
    EXPECT_NE(refused({"--help"}).find("--command LINE"), std::string::npos);
    EXPECT_EQ(std::string(commandsOffRefusal()),
              "--command needs a development player or Project Settings > Debug > Console Commands: Always.");
}

// --command (dev-console-for-agents R16): repeatable, in order; the web's ?command= items too
TEST(LaunchOptions, CommandRepeatsInOrder) {
    const LaunchOptions options = ok({"--command", "give gold 10", "--headless", "--command=tp \"Big Tree\""});
    EXPECT_EQ(options.commands, (std::vector<std::string>{"give gold 10", "tp \"Big Tree\""}));
    EXPECT_TRUE(ok({}).commands.empty());
    const LaunchOptions web = ok(argsFromQuery("?command=help&command=give+gold+10"), ParseMode::Lenient);
    EXPECT_EQ(web.commands, (std::vector<std::string>{"help", "give gold 10"}));
}

TEST(LaunchOptions, LenientModeKeepsUnknownArguments) {
    const LaunchOptions options = ok({"--mygame-flag", "level2", "--quit-after", "3"}, ParseMode::Lenient);
    EXPECT_EQ(options.quitAfter, 3u);
    EXPECT_EQ(options.scriptArgs, (std::vector<std::string>{"--mygame-flag", "level2"}));
    // known flags still refuse bad values
    EXPECT_NE(refused({"--quit-after", "x"}, ParseMode::Lenient).find("whole number"), std::string::npos);
}

TEST(LaunchOptions, WebQueryItemsAreFlags) {
    const std::vector<std::string> args = argsFromQuery("?quit-after=30&headless&scene=Main%20Menu&jasperengine_ready_signal=1");
    EXPECT_EQ(args, (std::vector<std::string>{"--quit-after=30", "--headless", "--scene=Main Menu", "--jasperengine_ready_signal=1"}));
    const LaunchOptions options = ok(args, ParseMode::Lenient);
    EXPECT_EQ(options.quitAfter, 30u);
    EXPECT_TRUE(options.headless);
    EXPECT_EQ(options.scene, "Main Menu");
}

TEST(LaunchOptions, LaunchAppliesThemToThePlatform) {
    PlatformConfig saved = platformConfig();
    platformConfig() = PlatformConfig();
    platformConfig().windowMode = 2;
    const int code = launch({"--headless", "--resolution", "640x360", "--quit-after", "2"}, ParseMode::Strict);
    EXPECT_EQ(code, -1);
    EXPECT_TRUE(platformConfig().headless);
    EXPECT_FALSE(platformConfig().vsync);
    EXPECT_TRUE(platformConfig().silentAudio);
    EXPECT_EQ(platformConfig().windowMode, 0);
    EXPECT_EQ(platformConfig().width, 640);
    EXPECT_EQ(platformConfig().height, 360);
    EXPECT_TRUE(RunControl::isActive());
    EXPECT_EQ(launch({"--bogus"}, ParseMode::Strict), EXIT_BAD_ARGUMENTS);
    RunControl::reset();
    platformConfig() = saved;
}

// --device-tier (spec S56 R4): a tier name in either form, case-insensitive; anything else refused
TEST(LaunchOptions, DeviceTier) {
    EXPECT_EQ(ok({"--device-tier=low"}).deviceTier, "low");
    EXPECT_EQ(ok({"--device-tier", "Ultra"}).deviceTier, "ultra");
    EXPECT_TRUE(ok({}).deviceTier.empty());
    EXPECT_NE(refused({"--device-tier=extreme"}).find("low, medium, high or ultra"), std::string::npos);
    EXPECT_NE(refused({"--device-tier"}).find("needs a value"), std::string::npos);
}

// project-test-runner AC11: --run-tests needs the test runner; a release player (built without
// JASPERENGINE_TEST_RUNNER) refuses it with exit 2 and the fixed message, a development one
// takes it with its filter and results path
TEST(LaunchOptions, RunTestsNeedsTheTestRunner) {
    if (!testRunnerCompiled()) {
        EXPECT_EQ(refused({"--run-tests"}), NO_TEST_RUNNER);
        EXPECT_EQ(refused({"--test-filter", "smoke"}), NO_TEST_RUNNER);
        return;
    }
    test::TempDir dir;
    const std::string results = (dir.path() / "results.json").string();
    const LaunchOptions options = ok({"--run-tests", "--test-filter", "tag:smoke,shop", "--test-results", results, "--keep-test-data"});
    EXPECT_TRUE(options.runTests);
    EXPECT_TRUE(options.keepTestData);
    EXPECT_EQ(options.testFilter, "tag:smoke,shop");
    EXPECT_EQ(options.testResults.string(), results);
    EXPECT_TRUE(options.afterReady) << "a test run counts from the ready frame";
    EXPECT_NE(refused({"--test-filter", "smoke"}).find("go with --run-tests"), std::string::npos);
    EXPECT_NE(refused({"--run-tests=yes"}).find("takes no value"), std::string::npos);
    EXPECT_NE(refused({"--run-tests", "--test-results", (dir.path() / "missing" / "r.json").string()}).find("does not exist"), std::string::npos);
}

TEST(LaunchOptions, TestFlagsFromTheQuery) {
    const std::vector<std::string> args = argsFromQuery("?run-tests&test-filter=tag%3Asmoke");
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0], "--run-tests");
    EXPECT_EQ(args[1], "--test-filter=tag:smoke");
    if (!testRunnerCompiled()) return;
    const LaunchOptions options = ok(args);
    EXPECT_TRUE(options.runTests);
    EXPECT_EQ(options.testFilter, "tag:smoke");
}

// --gpu-quirk (GPU quirks R21, AC18): name:on|off, repeatable or comma-separated, in order
TEST(LaunchOptions, GpuQuirkFlagParses) {
    const LaunchOptions options = ok({"--gpu-quirk=gl_no_warm_draws:off,gl_no_worker_context:on", "--gpu-quirk", "gl_no_compile_threads:off"});
    ASSERT_EQ(options.gpuQuirks.size(), 3u);
    EXPECT_EQ(options.gpuQuirks[0].quirk, GpuQuirk::GlNoWarmDraws);
    EXPECT_FALSE(options.gpuQuirks[0].on);
    EXPECT_EQ(options.gpuQuirks[1].quirk, GpuQuirk::GlNoWorkerContext);
    EXPECT_TRUE(options.gpuQuirks[1].on);
    EXPECT_EQ(options.gpuQuirks[2].quirk, GpuQuirk::GlNoCompileThreads);
    EXPECT_TRUE(ok({}).gpuQuirks.empty());
    // the web page's query takes the same form (?gpu-quirk=gl_no_warm_draws:off)
    EXPECT_EQ(ok(argsFromQuery("?gpu-quirk=gl_no_warm_draws:off")).gpuQuirks.size(), 1u);
}

// an unknown name is refused (exit 2), naming the known quirks
TEST(LaunchOptions, GpuQuirkRejectsUnknownNames) {
    EXPECT_EQ(refused({"--gpu-quirk=x:on"}).rfind("--gpu-quirk: unknown quirk \"x\" (known: gl_no_worker_context, gl_no_compile_threads, "
                                                  "gl_no_warm_draws", 0), 0u);
    EXPECT_NE(refused({"--gpu-quirk=gl_no_warm_draws:maybe"}).find(":on or :off"), std::string::npos);
    const PlatformConfig saved = platformConfig();
    EXPECT_EQ(launch({"--gpu-quirk=x:on"}, ParseMode::Strict), EXIT_BAD_ARGUMENTS);
    RunControl::reset();
    platformConfig() = saved;
}

// --perf-summary (R12): a file in a directory that exists; it asks for the ready signal
TEST(LaunchOptions, PerfSummary) {
    test::TempDir dir;
    const std::string path = (dir.path() / "perf.json").string();
    const LaunchOptions options = ok({"--perf-summary=" + path});
    EXPECT_EQ(options.perfSummary.string(), path);
    EXPECT_NE(refused({"--perf-summary=" + (dir.path() / "missing" / "perf.json").string()}).find("does not exist"), std::string::npos);
    RunControl::configure(options);
    EXPECT_TRUE(RunControl::wantsReadySignal());
    EXPECT_FALSE(RunControl::isActive()) << "no frame option: the run is not counted";
    RunControl::reset();
}

// R30: the web page fetches an --input-script given as a URL or a page path before the start
// scene; a content path is read from the game's content as on every other target
TEST(InputScriptRun, WebFetchesUrlsNotContentPaths) {
    using player::InputScriptRun::isFetchedUrl;
    EXPECT_TRUE(isFetchedUrl("/tests/menu.input.json"));
    EXPECT_TRUE(isFetchedUrl("./menu.input.json"));
    EXPECT_TRUE(isFetchedUrl("../scripts/menu.input.json"));
    EXPECT_TRUE(isFetchedUrl("https://example.com/menu.input.json"));
    EXPECT_TRUE(isFetchedUrl("http://127.0.0.1:8000/menu.input.json"));
    EXPECT_FALSE(isFetchedUrl("tests/Input/menu.input.json"));
    EXPECT_FALSE(isFetchedUrl("asset://demo/attract.input.json"));
    EXPECT_FALSE(isFetchedUrl("data://input/menu.input.json"));
}

}  // namespace LaunchOptionsTest_cpp
