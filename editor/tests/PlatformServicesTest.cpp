// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Platform services (docs/superpowers/specs/2026-10-31-platform-services-design.md §9): the
// pure rules every platform shares (locale, orientation, frame pacing, URLs, OS strings), the
// frame limiter, Engine's lifecycle events (idempotent pause, focus order), Screen's
// orientation and sleep rules, the editor's device simulation and the project.yaml migration of
// android.orientation. The platforms' own code runs on devices (Android/Web compile checks).

#include <gtest/gtest.h>

#include "Engine.h"
#include "Screen.h"
#include "System.h"
#include "log/Logger.h"
#include "util/FrameLimiter.h"
#include "util/PlatformRules.h"

#include "Platform.h"
#include "Project.h"
#include "Stream.h"
#include "Generator.h"
#include "Workspace.h"
#include "window/DeviceProfiles.h"
#include "support/EditorTestSupport.h"

#include <filesystem>

#include <chrono>
#include <cstdio>
#include <thread>
#include <regex>
#include <string>
#include <vector>

namespace jasperengine::editor {
    // TEST-ONLY seam (Generator.h befriends it)
    struct GeneratorTestAccess {
        static std::string platformCMake(Generator& generator) {
            return generator.getPlatformCMakeConfig(true, WindowSettings(), "/project", "/project/assets", "/project/lua");
        }
    };
}

namespace jasperengine {
    // TEST-ONLY seam (System.h befriends it): detach / restore the instance
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // A System of a given screen that records what the engine asks of the platform
    class RecordingSystem : public System {
    public:
        int width = 1280;
        int height = 720;
        std::vector<std::string> locales = {"en-US"};
        std::string region;
        ScreenOrientation orientation = ScreenOrientation::Portrait;
        bool reportOrientation = false;
        std::vector<std::pair<ScreenOrientation, uint8_t>> policies;
        std::vector<bool> keepScreenOn;
        std::vector<FramePacing> pacings;
        std::vector<std::string> opened;

        int getScreenWidth() override { return width; }
        int getScreenHeight() override { return height; }
        std::vector<std::string> getPreferredLocales() override { return locales; }
        std::string getRegionOverride() override { return region; }
        void applyOrientationPolicy(ScreenOrientation requested, uint8_t mask) override { policies.push_back({requested, mask}); }
        ScreenOrientation getScreenOrientation() override {
            return reportOrientation ? orientation : System::getScreenOrientation();
        }
        void setKeepScreenOn(bool keepOn) override { keepScreenOn.push_back(keepOn); }
        void applyFramePacing(const FramePacing& pacing) override { pacings.push_back(pacing); }

    protected:
        bool platformOpenURL(const std::string& url) override {
            opened.push_back(url);
            return true;
        }
    };

    // Installs a RecordingSystem for one test and puts the previous System back
    class PlatformServicesTest : public ::testing::Test {
    protected:
        RecordingSystem system;
        System* previous = nullptr;

        void SetUp() override {
            previous = SystemTestAccess::swap(&system);
            Engine::resetApplicationState();
            Engine::clearAllSubscriptions(true);
        }

        void TearDown() override {
            Engine::clearAllSubscriptions(true);
            Engine::resetApplicationState();
            Engine::setVSyncCount(1);
            Screen::initialize(ScreenOrientation::AutoRotation, PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS, true);
            ::jasperengine::Logger::flush();   // nothing is delivered to this System after it goes
            SystemTestAccess::swap(previous);
        }
    };

    uint8_t bits(std::initializer_list<ScreenOrientation> orientations) {
        uint8_t mask = 0;
        for (ScreenOrientation o : orientations) mask |= PlatformRules::orientationBit(o);
        return mask;
    }

    constexpr ScreenOrientation P = ScreenOrientation::Portrait;
    constexpr ScreenOrientation PU = ScreenOrientation::PortraitUpsideDown;
    constexpr ScreenOrientation LL = ScreenOrientation::LandscapeLeft;
    constexpr ScreenOrientation LR = ScreenOrientation::LandscapeRight;
    constexpr ScreenOrientation AUTO = ScreenOrientation::AutoRotation;

}

// ---- 1. SystemLanguage ------------------------------------------------------------------------

TEST(LocaleRules, SystemLanguageOfTags) {
    EXPECT_EQ(LocaleRules::toSystemLanguage("es-MX"), SystemLanguage::Spanish);
    EXPECT_EQ(LocaleRules::toSystemLanguage("pt_BR.UTF-8"), SystemLanguage::Portuguese);
    EXPECT_EQ(LocaleRules::toSystemLanguage("zh-Hant-TW"), SystemLanguage::ChineseTraditional);
    EXPECT_EQ(LocaleRules::toSystemLanguage("zh-HK"), SystemLanguage::ChineseTraditional);
    EXPECT_EQ(LocaleRules::toSystemLanguage("zh_MO"), SystemLanguage::ChineseTraditional);
    EXPECT_EQ(LocaleRules::toSystemLanguage("zh-CN"), SystemLanguage::ChineseSimplified);
    EXPECT_EQ(LocaleRules::toSystemLanguage("zh-Hans"), SystemLanguage::ChineseSimplified);
    EXPECT_EQ(LocaleRules::toSystemLanguage("zh"), SystemLanguage::ChineseSimplified);
    // a script beats the region
    EXPECT_EQ(LocaleRules::toSystemLanguage("zh-Hans-HK"), SystemLanguage::ChineseSimplified);
    EXPECT_EQ(LocaleRules::toSystemLanguage("nb-NO"), SystemLanguage::Norwegian);
    EXPECT_EQ(LocaleRules::toSystemLanguage("nn"), SystemLanguage::Norwegian);
    EXPECT_EQ(LocaleRules::toSystemLanguage("sr-Latn"), SystemLanguage::SerboCroatian);
    EXPECT_EQ(LocaleRules::toSystemLanguage("hr"), SystemLanguage::SerboCroatian);
    EXPECT_EQ(LocaleRules::toSystemLanguage("bs-BA"), SystemLanguage::SerboCroatian);
    EXPECT_EQ(LocaleRules::toSystemLanguage("iw"), SystemLanguage::Hebrew);
    EXPECT_EQ(LocaleRules::toSystemLanguage("he-IL"), SystemLanguage::Hebrew);
    EXPECT_EQ(LocaleRules::toSystemLanguage("in"), SystemLanguage::Indonesian);
    EXPECT_EQ(LocaleRules::toSystemLanguage("hi-IN"), SystemLanguage::Hindi);
    EXPECT_EQ(LocaleRules::toSystemLanguage("C"), SystemLanguage::English);
    EXPECT_EQ(LocaleRules::toSystemLanguage("POSIX"), SystemLanguage::English);
    EXPECT_EQ(LocaleRules::toSystemLanguage("EN_us"), SystemLanguage::English);
    EXPECT_EQ(LocaleRules::toSystemLanguage(""), SystemLanguage::Unknown);
    EXPECT_EQ(LocaleRules::toSystemLanguage("xx"), SystemLanguage::Unknown);
    EXPECT_EQ(LocaleRules::toSystemLanguage("tlh"), SystemLanguage::Unknown);
}

TEST(LocaleRules, SystemLanguageValuesAreUnitys) {
    // UnityEngine.SystemLanguage
    const std::pair<SystemLanguage, int> unity[] = {
        {SystemLanguage::Afrikaans, 0}, {SystemLanguage::Arabic, 1}, {SystemLanguage::Basque, 2},
        {SystemLanguage::Belarusian, 3}, {SystemLanguage::Bulgarian, 4}, {SystemLanguage::Catalan, 5},
        {SystemLanguage::Chinese, 6}, {SystemLanguage::Czech, 7}, {SystemLanguage::Danish, 8}, {SystemLanguage::Dutch, 9},
        {SystemLanguage::English, 10}, {SystemLanguage::Estonian, 11}, {SystemLanguage::Faroese, 12},
        {SystemLanguage::Finnish, 13}, {SystemLanguage::French, 14}, {SystemLanguage::German, 15},
        {SystemLanguage::Greek, 16}, {SystemLanguage::Hebrew, 17}, {SystemLanguage::Hungarian, 18},
        {SystemLanguage::Icelandic, 19}, {SystemLanguage::Indonesian, 20}, {SystemLanguage::Italian, 21},
        {SystemLanguage::Japanese, 22}, {SystemLanguage::Korean, 23}, {SystemLanguage::Latvian, 24},
        {SystemLanguage::Lithuanian, 25}, {SystemLanguage::Norwegian, 26}, {SystemLanguage::Polish, 27},
        {SystemLanguage::Portuguese, 28}, {SystemLanguage::Romanian, 29}, {SystemLanguage::Russian, 30},
        {SystemLanguage::SerboCroatian, 31}, {SystemLanguage::Slovak, 32}, {SystemLanguage::Slovenian, 33},
        {SystemLanguage::Spanish, 34}, {SystemLanguage::Swedish, 35}, {SystemLanguage::Thai, 36},
        {SystemLanguage::Turkish, 37}, {SystemLanguage::Ukrainian, 38}, {SystemLanguage::Vietnamese, 39},
        {SystemLanguage::ChineseSimplified, 40}, {SystemLanguage::ChineseTraditional, 41},
        {SystemLanguage::Unknown, 42}, {SystemLanguage::Hindi, 43},
    };
    for (const auto& [language, value] : unity) {
        EXPECT_EQ((int)language, value) << LocaleRules::systemLanguageName(language);
    }
    EXPECT_STREQ(LocaleRules::systemLanguageName(SystemLanguage::Spanish), "Spanish");
    EXPECT_STREQ(LocaleRules::systemLanguageName((SystemLanguage)99), "Unknown");
    // the other enums keep Unity's values too
    EXPECT_EQ((int)ScreenOrientation::Portrait, 1);
    EXPECT_EQ((int)ScreenOrientation::AutoRotation, 5);
    EXPECT_EQ((int)DeviceOrientation::FaceDown, 6);
    EXPECT_EQ((int)DeviceType::Desktop, 3);
    EXPECT_EQ((int)NetworkReachability::ReachableViaLocalAreaNetwork, 2);
    EXPECT_EQ(SleepTimeout::NeverSleep, -1);
    EXPECT_EQ(SleepTimeout::SystemSetting, -2);
}

// ---- 2. normalize, codes, region ---------------------------------------------------------------

TEST(LocaleRules, NormalizeAndCodes) {
    EXPECT_EQ(LocaleRules::normalize("es_MX.UTF-8@euro"), "es-MX");
    EXPECT_EQ(LocaleRules::normalize("zh_hant_tw"), "zh-Hant-TW");
    EXPECT_EQ(LocaleRules::normalize("C"), "en-US");
    EXPECT_EQ(LocaleRules::normalize("C.UTF-8"), "en-US");
    EXPECT_EQ(LocaleRules::normalize(""), "");
    EXPECT_EQ(LocaleRules::normalize("es-419"), "es-419");
    EXPECT_EQ(LocaleRules::languageCode("PT-br"), "pt");
    EXPECT_EQ(LocaleRules::regionCode("pt-br"), "BR");
    EXPECT_EQ(LocaleRules::regionCode("zh-Hant-TW"), "TW");
    EXPECT_EQ(LocaleRules::regionCode("es-419"), "419");
    EXPECT_EQ(LocaleRules::regionCode("fr"), "");
    EXPECT_EQ(LocaleRules::scriptCode("sr-Latn-RS"), "Latn");
    EXPECT_EQ(LocaleRules::parseLanguageList("es_MX:es:en_US.UTF-8:es"), (std::vector<std::string>{"es-MX", "es", "en-US"}));
    EXPECT_EQ(LocaleRules::resolveRegion("us", "es-MX"), "US");
    EXPECT_EQ(LocaleRules::resolveRegion("", "es-MX"), "MX");
}

TEST_F(PlatformServicesTest, RegionOverrideWinsOverTheLocalesRegion) {
    system.locales = {"es_MX.UTF-8", "en-US"};
    EXPECT_EQ(system.getLocale(), "es-MX");
    EXPECT_EQ(system.getLanguageCode(), "es");
    EXPECT_EQ(system.getRegionCode(), "MX");
    EXPECT_EQ(system.getSystemLanguage(), SystemLanguage::Spanish);
    // a Spanish speaker set to the United States
    system.region = "US";
    EXPECT_EQ(system.getRegionCode(), "US");
    system.locales = {};
    system.region = "";
    EXPECT_EQ(system.getLocale(), "en-US");
}

// ---- 3. orientation mapping --------------------------------------------------------------------

TEST(PlatformRules, AndroidRequestedOrientationOfEveryMask) {
    // fixed requests ignore the mask
    EXPECT_EQ(PlatformRules::androidRequestedOrientation(P, 0), PlatformRules::ANDROID_PORTRAIT);
    EXPECT_EQ(PlatformRules::androidRequestedOrientation(PU, 0), PlatformRules::ANDROID_REVERSE_PORTRAIT);
    EXPECT_EQ(PlatformRules::androidRequestedOrientation(LL, 0), PlatformRules::ANDROID_LANDSCAPE);
    EXPECT_EQ(PlatformRules::androidRequestedOrientation(LR, 0), PlatformRules::ANDROID_REVERSE_LANDSCAPE);

    const ScreenOrientation order[] = {P, PU, LL, LR};
    for (int combination = 0; combination < 16; combination++) {
        uint8_t mask = 0;
        for (int i = 0; i < 4; i++) {
            if (combination & (1 << i)) mask |= PlatformRules::orientationBit(order[i]);
        }
        bool unsupported = false;
        const int value = PlatformRules::androidRequestedOrientation(AUTO, mask, &unsupported);
        int expected = PlatformRules::ANDROID_FULL_USER;
        bool expectedUnsupported = false;
        if (mask == 0) expected = PlatformRules::ANDROID_USER;  // empty: the default set P, LL, LR
        else if (mask == bits({P})) expected = PlatformRules::ANDROID_PORTRAIT;
        else if (mask == bits({PU})) expected = PlatformRules::ANDROID_REVERSE_PORTRAIT;
        else if (mask == bits({LL})) expected = PlatformRules::ANDROID_LANDSCAPE;
        else if (mask == bits({LR})) expected = PlatformRules::ANDROID_REVERSE_LANDSCAPE;
        else if (mask == bits({P, PU})) expected = PlatformRules::ANDROID_USER_PORTRAIT;
        else if (mask == bits({LL, LR})) expected = PlatformRules::ANDROID_USER_LANDSCAPE;
        else if (mask == bits({P, LL, LR})) expected = PlatformRules::ANDROID_USER;
        else if (mask == bits({P, PU, LL, LR})) expected = PlatformRules::ANDROID_FULL_USER;
        else expectedUnsupported = true;
        EXPECT_EQ(value, expected) << "mask " << (int)mask;
        EXPECT_EQ(unsupported, expectedUnsupported) << "mask " << (int)mask;
    }
    EXPECT_STREQ(PlatformRules::androidManifestOrientation(PlatformRules::ANDROID_USER_LANDSCAPE), "userLandscape");
    EXPECT_STREQ(PlatformRules::androidManifestOrientation(PlatformRules::ANDROID_FULL_USER), "fullUser");
    EXPECT_STREQ(PlatformRules::androidManifestOrientation(PlatformRules::ANDROID_REVERSE_PORTRAIT), "reversePortrait");
}

TEST(PlatformRules, OrientationOfDisplayRotation) {
    // a phone: rotation 90 = turned counter-clockwise = Unity's LandscapeLeft
    EXPECT_EQ(PlatformRules::orientationFromRotation(0, true), P);
    EXPECT_EQ(PlatformRules::orientationFromRotation(90, true), LL);
    EXPECT_EQ(PlatformRules::orientationFromRotation(180, true), PU);
    EXPECT_EQ(PlatformRules::orientationFromRotation(270, true), LR);
    // a tablet whose rotation 0 is landscape
    EXPECT_EQ(PlatformRules::orientationFromRotation(0, false), LL);
    EXPECT_EQ(PlatformRules::orientationFromRotation(90, false), PU);
    EXPECT_EQ(PlatformRules::orientationFromRotation(180, false), LR);
    EXPECT_EQ(PlatformRules::orientationFromRotation(270, false), P);
}

TEST(PlatformRules, DeviceOrientationWithHysteresis) {
    using DO = DeviceOrientation;
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(-1, true, DO::Portrait), DO::Unknown);
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(5, true, DO::Unknown), DO::Portrait);
    // OrientationEventListener: 270 is the right side up, Unity's LandscapeLeft
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(270, true, DO::Unknown), DO::LandscapeLeft);
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(90, true, DO::Unknown), DO::LandscapeRight);
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(180, true, DO::Unknown), DO::PortraitUpsideDown);
    // 50 degrees stays portrait (within 45 + 10), 60 turns
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(50, true, DO::Portrait), DO::Portrait);
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(60, true, DO::Portrait), DO::LandscapeRight);
    // and back: 40 stays landscape right
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(40, true, DO::LandscapeRight), DO::LandscapeRight);
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(30, true, DO::LandscapeRight), DO::Portrait);
    // a tablet whose rotation 0 is landscape: held at 0 degrees it is in LandscapeLeft, a quarter
    // turn clockwise from there is portrait
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(0, false, DO::Unknown), DO::LandscapeLeft);
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(90, false, DO::Unknown), DO::Portrait);
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(180, false, DO::Unknown), DO::LandscapeRight);
    EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(270, false, DO::Unknown), DO::PortraitUpsideDown);
}

TEST(PlatformRules, DeviceAndInterfaceOrientationAgree) {
    // the display turns the content back as far as the device turned: rotation r counter-clockwise
    // is the listener's (360 - r) clockwise, and both name the same orientation, on phones and tablets
    for (bool naturalPortrait : {true, false}) {
        for (int rotation : {0, 90, 180, 270}) {
            const int degrees = (360 - rotation) % 360;
            EXPECT_EQ(PlatformRules::deviceOrientationFromDegrees(degrees, naturalPortrait, DeviceOrientation::Unknown),
                      PlatformRules::deviceOrientationOf(PlatformRules::orientationFromRotation(rotation, naturalPortrait)))
                << "rotation " << rotation << (naturalPortrait ? " phone" : " tablet");
        }
    }
}

TEST(PlatformRules, IosMasks) {
    // UIInterfaceOrientationMask: Portrait 2, PortraitUpsideDown 4, LandscapeRight 8, LandscapeLeft 16
    EXPECT_EQ(PlatformRules::iosOrientationMask(P), 2u);
    EXPECT_EQ(PlatformRules::iosOrientationMask(PU), 4u);
    EXPECT_EQ(PlatformRules::iosOrientationMask(LL), 8u);   // UIInterfaceOrientationMaskLandscapeRight
    EXPECT_EQ(PlatformRules::iosOrientationMask(LR), 16u);  // ...LandscapeLeft
    EXPECT_EQ(PlatformRules::iosSupportedMask(LL, bits({P})), 8u);
    EXPECT_EQ(PlatformRules::iosSupportedMask(AUTO, bits({LL, LR})), 24u);
    EXPECT_EQ(PlatformRules::orientationFromIos(3), LL);
    EXPECT_EQ(PlatformRules::orientationFromIos(4), LR);
    EXPECT_EQ(PlatformRules::iosInterfaceOrientation(LL), 3);
    EXPECT_EQ(PlatformRules::iosPlistOrientations(AUTO, bits({P, LL, LR})),
              (std::vector<std::string>{"UIInterfaceOrientationPortrait", "UIInterfaceOrientationLandscapeRight",
                                        "UIInterfaceOrientationLandscapeLeft"}));
    EXPECT_EQ(PlatformRules::iosPlistOrientations(LR, bits({P, LL, LR})),
              (std::vector<std::string>{"UIInterfaceOrientationLandscapeLeft"}));
}

TEST(PlatformRules, ResolveOrientationAndNames) {
    EXPECT_EQ(PlatformRules::resolveOrientation(P, LL, bits({P})), LL);
    EXPECT_EQ(PlatformRules::resolveOrientation(LR, AUTO, bits({LL, LR})), LR);
    EXPECT_EQ(PlatformRules::resolveOrientation(P, AUTO, bits({LR, LL})), LL);
    EXPECT_EQ(PlatformRules::resolveOrientation(P, AUTO, bits({PU, LR})), LR);
    EXPECT_EQ(PlatformRules::orientationFromSize(1920, 1080), LL);
    EXPECT_EQ(PlatformRules::orientationFromSize(1080, 1920), P);
    ScreenOrientation parsed;
    EXPECT_TRUE(PlatformRules::parseOrientationName("landscape_left", parsed));
    EXPECT_EQ(parsed, LL);
    EXPECT_TRUE(PlatformRules::parseOrientationName("Auto_Rotation", parsed));
    EXPECT_EQ(parsed, AUTO);
    EXPECT_FALSE(PlatformRules::parseOrientationName("sideways", parsed));
}

// ---- 4. frame pacing ---------------------------------------------------------------------------

TEST(PlatformRules, FramePacingTable) {
    auto pace = [](jasperengine::Platform platform, int target, int vsync, double refresh, int maxInterval = 4, bool canDisable = true) {
        return PlatformRules::computeFramePacing(platform, target, vsync, refresh, maxInterval, canDisable);
    };
    // Android: 60 by default, capped by the display, vSyncCount ignored
    EXPECT_EQ(pace(jasperengine::Platform::Android, -1, 1, 60).mobileFps, 60);
    EXPECT_EQ(pace(jasperengine::Platform::Android, 30, 0, 60).mobileFps, 30);
    EXPECT_EQ(pace(jasperengine::Platform::Android, 60, 2, 60).mobileFps, 60);
    EXPECT_EQ(pace(jasperengine::Platform::Android, 120, 1, 60).mobileFps, 60);
    EXPECT_EQ(pace(jasperengine::Platform::iOS, 120, 1, 120).mobileFps, 120);
    EXPECT_EQ(pace(jasperengine::Platform::iOS, -1, 1, 0).mobileFps, 60);  // unknown refresh = 60
    EXPECT_DOUBLE_EQ(pace(jasperengine::Platform::Android, 30, 0, 60).limiterFps, 0.0);
    // desktop with vsync: targetFrameRate ignored (Unity)
    FramePacing p = pace(jasperengine::Platform::Linux, 30, 1, 60);
    EXPECT_EQ(p.swapInterval, 1);
    EXPECT_DOUBLE_EQ(p.limiterFps, 0.0);
    // no vsync: the limiter at the target
    p = pace(jasperengine::Platform::Windows, 30, 0, 60);
    EXPECT_EQ(p.swapInterval, 0);
    EXPECT_DOUBLE_EQ(p.limiterFps, 30.0);
    p = pace(jasperengine::Platform::Linux, -1, 0, 60);
    EXPECT_EQ(p.swapInterval, 0);
    EXPECT_DOUBLE_EQ(p.limiterFps, 0.0);
    // GL / D3D11 present every 2nd vblank; Vulkan (interval 1 only) uses the limiter
    p = pace(jasperengine::Platform::Windows, -1, 2, 144);
    EXPECT_EQ(p.swapInterval, 2);
    EXPECT_DOUBLE_EQ(p.limiterFps, 0.0);
    p = pace(jasperengine::Platform::Linux, -1, 2, 60, 1);
    EXPECT_EQ(p.swapInterval, 1);
    EXPECT_DOUBLE_EQ(p.limiterFps, 30.0);
    // the macOS MTKView backend stays synchronized
    p = pace(jasperengine::Platform::MacOS, 30, 0, 60, 1, false);
    EXPECT_EQ(p.swapInterval, 1);
    EXPECT_DOUBLE_EQ(p.limiterFps, 30.0);
    // web: a limiter, vSyncCount ignored
    p = pace(jasperengine::Platform::Web, 30, 1, 60);
    EXPECT_DOUBLE_EQ(p.limiterFps, 30.0);
    EXPECT_DOUBLE_EQ(pace(jasperengine::Platform::Web, -1, 1, 60).limiterFps, 0.0);
}

TEST_F(PlatformServicesTest, EngineFrameRateSettersFollowUnity) {
    Engine::setTargetFrameRate(0);
    EXPECT_EQ(Engine::getTargetFrameRate(), -1);
    Engine::setTargetFrameRate(5000);
    EXPECT_EQ(Engine::getTargetFrameRate(), 1000);
    Engine::setVSyncCount(9);
    EXPECT_EQ(Engine::getVSyncCount(), 4);
    Engine::setVSyncCount(0);
    Engine::setTargetFrameRate(30);
    ASSERT_FALSE(system.pacings.empty());
    // the base System presents at most every vblank: no vsync, the limiter at 30
    EXPECT_EQ(system.pacings.back().swapInterval, 0);
    EXPECT_DOUBLE_EQ(system.pacings.back().limiterFps, 30.0);
    EXPECT_EQ(Engine::getFramePacing(), system.pacings.back());
}

// ---- 5. frame limiter --------------------------------------------------------------------------

TEST(FrameLimiter, PacesFramesAtTheRate) {
    FrameLimiter limiter;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 21; frame++) {
        limiter.wait(50.0);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // 20 periods of 20 ms after the first frame; the upper bound is a wall-clock budget, which a
    // loaded host's sleeps can overrun (docs/testing.md)
    EXPECT_GT(seconds, 0.4 * 0.9);
    std::printf("[ bench ] 20 frames at 50 fps in %.3f s (budget %.3f)\n", seconds, 0.4 * 1.25);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LT(seconds, 0.4 * 1.25 * slack);
    }
}

TEST(FrameLimiter, IsDueSkipsFramesBetweenDeadlines) {
    FrameLimiter limiter;
    EXPECT_TRUE(limiter.isDue(10.0));   // the first frame
    EXPECT_FALSE(limiter.isDue(10.0));  // 100 ms not passed yet
    EXPECT_TRUE(limiter.isDue(0.0));    // no limit: always
}

// ---- 6. lifecycle --------------------------------------------------------------------------------

TEST_F(PlatformServicesTest, PauseAndResumeAreIdempotentAndFocusComesFirst) {
    std::vector<std::string> events;
    Engine::onPause.add("test_pause", [&events]() { events.push_back("pause"); });
    Engine::onResume.add("test_resume", [&events]() { events.push_back("resume"); });
    Engine::onFocus.add("test_focus", [&events](bool focused) { events.push_back(focused ? "focus" : "blur"); });

    EXPECT_TRUE(Engine::isFocused());
    Engine::systemPause();
    Engine::systemPause();
    EXPECT_TRUE(Engine::isPaused());
    EXPECT_FALSE(Engine::isFocused());
    // leaving: focus lost first
    EXPECT_EQ(events, (std::vector<std::string>{"blur", "pause"}));

    events.clear();
    // a platform that says "focused" before "resumed" still resumes first
    Engine::systemFocus(true);
    Engine::systemResume();
    Engine::systemFocus(true);
    EXPECT_EQ(events, (std::vector<std::string>{"resume", "focus"}));

    events.clear();
    // focus changes alone (a dialog over a running game) fire only on change
    Engine::systemFocus(false);
    Engine::systemFocus(false);
    Engine::systemFocus(true);
    EXPECT_EQ(events, (std::vector<std::string>{"blur", "focus"}));
    EXPECT_FALSE(Engine::isPaused());
}

TEST_F(PlatformServicesTest, OrientationEventOnlyOnChangeAfterTheFirst) {
    std::vector<int> events;
    Engine::onOrientationChanged.add("test_orientation", [&events](int orientation) { events.push_back(orientation); });
    system.reportOrientation = true;
    system.orientation = P;
    Engine::systemOrientationChanged();  // where the view starts
    Engine::systemOrientationChanged();
    system.orientation = LL;
    Engine::systemOrientationChanged();
    Engine::systemOrientationChanged();
    system.orientation = LR;
    Engine::systemOrientationChanged();
    EXPECT_EQ(events, (std::vector<int>{(int)LL, (int)LR}));
}

TEST_F(PlatformServicesTest, QuarterTurnWaitsForTheCanvasAndHalfTurnDoesNot) {
    std::vector<std::string> events;
    Engine::onViewChanged.add("test_canvas", [&events]() { events.push_back("canvas"); });
    Engine::onOrientationChanged.add("test_orientation", [&events](int o) { events.push_back("orientation " + std::to_string(o)); });
    system.reportOrientation = true;
    system.orientation = P;
    system.width = 720;
    system.height = 1280;
    Engine::systemViewChanged();  // the baseline
    events.clear();

    // Android / the web: the rotation is known before the surface has its new size
    system.orientation = LL;
    Engine::systemOrientationChanged();
    EXPECT_TRUE(events.empty());
    system.width = 1280;
    system.height = 720;
    Engine::systemViewChanged();
    EXPECT_EQ(events, (std::vector<std::string>{"canvas", "orientation 3"}));

    // a half turn keeps the size: at once
    events.clear();
    system.orientation = LR;
    Engine::systemOrientationChanged();
    EXPECT_EQ(events, (std::vector<std::string>{"orientation 4"}));

    // a view that never changes shape (a fixed canvas in a page): after the timeout
    events.clear();
    system.orientation = P;
    Engine::systemOrientationChanged();
    EXPECT_TRUE(events.empty());
    std::this_thread::sleep_for(std::chrono::duration<double>(Engine::ORIENTATION_SIZE_TIMEOUT + 0.1));
    Engine::systemOrientationChanged();
    EXPECT_EQ(events, (std::vector<std::string>{"orientation 1"}));
}

TEST_F(PlatformServicesTest, LowMemoryEventAndLifecycleClearing) {
    int lowMemory = 0;
    Engine::onLowMemory.add("test_low_memory", [&lowMemory]() { lowMemory++; });
    Engine::systemLowMemory();
    EXPECT_EQ(lowMemory, 1);
    // lifecycle events go with clearAllSubscriptions(true) and by tag, not with (false)
    Engine::clearAllSubscriptions(false);
    Engine::systemLowMemory();
    EXPECT_EQ(lowMemory, 2);
    Engine::removeSubscriptionsByTag("test_low");
    Engine::systemLowMemory();
    EXPECT_EQ(lowMemory, 2);
}

TEST_F(PlatformServicesTest, OpenUrlRefusesUnsafeUrls) {
    EXPECT_TRUE(system.openURL("https://play.google.com/store/apps/details?id=com.example"));
    EXPECT_TRUE(system.openURL("mailto:support@example.com"));
    EXPECT_TRUE(system.openURL("market://details?id=com.example"));
    EXPECT_FALSE(system.openURL("javascript:alert(1)"));
    EXPECT_FALSE(system.openURL("JavaScript:alert(1)"));
    EXPECT_FALSE(system.openURL("data:text/html,hi"));
    EXPECT_FALSE(system.openURL("file:///etc/passwd"));
    EXPECT_FALSE(system.openURL("vbscript:x"));
    EXPECT_FALSE(system.openURL("www.example.com"));
    EXPECT_FALSE(system.openURL(""));
    EXPECT_FALSE(system.openURL("https://example.com/\nx"));
    // a Windows drive path is no scheme: ShellExecute would run the program
    EXPECT_FALSE(system.openURL("C:\\Windows\\System32\\calc.exe"));
    EXPECT_FALSE(system.openURL("c:/Windows/System32/calc.exe"));
    // Windows protocol handlers that start programs, installers or searches of local files
    for (const char* url : {"ms-msdt:/id PCWDiagnostic", "search-ms:query=x&crumb=location:\\\\host\\share",
                            "search:query=x", "shell:startup", "ms-appinstaller:?source=https://x/app.msix",
                            "ms-officecmd:{}", "MS-MSDT:x", "ms-word:ofe|u|https://x/doc.docx"}) {
        EXPECT_FALSE(system.openURL(url)) << url;
    }
    EXPECT_FALSE(UrlRules::check("C:\\x.exe").empty());
    EXPECT_TRUE(UrlRules::check("itms-apps://apps.apple.com/app/id1").empty());
    EXPECT_TRUE(UrlRules::check("tel:+15551234").empty());
    EXPECT_EQ(system.opened, (std::vector<std::string>{"https://play.google.com/store/apps/details?id=com.example",
                                                       "mailto:support@example.com", "market://details?id=com.example"}));
}

// ---- 7. Screen ---------------------------------------------------------------------------------

TEST_F(PlatformServicesTest, ScreenAutorotateRules) {
    Screen::initialize(AUTO, bits({P, LL, LR}), true);
    ASSERT_FALSE(system.policies.empty());
    EXPECT_EQ(system.policies.back(), std::make_pair(AUTO, bits({P, LL, LR})));
    // Keep Screen On is a phone and tablet setting (Unity): a desktop starts at the system's
    EXPECT_FALSE(Screen::isMobileSleepPolicy());
    EXPECT_EQ(system.keepScreenOn.back(), false);
    EXPECT_EQ(Screen::getSleepTimeout(), SleepTimeout::SystemSetting);

    // applied right away while auto-rotating
    system.policies.clear();
    Screen::setAutorotate(P, false);
    ASSERT_EQ(system.policies.size(), 1u);
    EXPECT_EQ(system.policies.back().second, bits({LL, LR}));
    EXPECT_FALSE(Screen::getAutorotate(P));

    // the last one cannot be turned off
    Screen::setAutorotate(LL, false);
    Screen::setAutorotate(LR, false);
    EXPECT_TRUE(Screen::getAutorotate(LR));
    EXPECT_EQ(Screen::getAllowedMask(), bits({LR}));

    // a fixed request: autorotateTo* is stored, not applied (Unity)
    Screen::setOrientation(LL);
    EXPECT_EQ(system.policies.back(), std::make_pair(LL, bits({LR})));
    system.policies.clear();
    Screen::setAutorotate(P, true);
    EXPECT_TRUE(system.policies.empty());
    EXPECT_EQ(Screen::getRequestedOrientation(), LL);
    // back to auto-rotation: the stored set
    Screen::setOrientation(AUTO);
    EXPECT_EQ(system.policies.back(), std::make_pair(AUTO, bits({P, LR})));
    // outside the enum: ignored
    system.policies.clear();
    Screen::setOrientation((ScreenOrientation)9);
    EXPECT_TRUE(system.policies.empty());

    // a fixed default is always allowed
    Screen::initialize(LR, bits({P}), false);
    EXPECT_EQ(Screen::getAllowedMask(), bits({P, LR}));
    EXPECT_EQ(Screen::getSleepTimeout(), SleepTimeout::SystemSetting);
    EXPECT_EQ(system.keepScreenOn.back(), false);
    // a script keeps it on everywhere
    Screen::setSleepTimeout(SleepTimeout::NeverSleep);
    EXPECT_EQ(system.keepScreenOn.back(), true);
}

TEST_F(PlatformServicesTest, ScreenSleepTimeoutAndOrientationOfTheSize) {
    Screen::setSleepTimeout(30);
    EXPECT_EQ(Screen::getSleepTimeout(), SleepTimeout::SystemSetting);
    Screen::setSleepTimeout(SleepTimeout::NeverSleep);
    EXPECT_EQ(Screen::getSleepTimeout(), SleepTimeout::NeverSleep);
    EXPECT_EQ(system.keepScreenOn.back(), true);
    // the base System: from the size, never AutoRotation
    system.width = 720;
    system.height = 1280;
    EXPECT_EQ(Screen::getOrientation(), P);
    system.width = 1280;
    system.height = 720;
    EXPECT_EQ(Screen::getOrientation(), LL);
}

// ---- 8. OS strings -------------------------------------------------------------------------------

TEST(OsRules, AndroidOperatingSystemMatchesTheGamesRegex) {
    const std::string os = OsRules::androidOperatingSystem("14", 34, "UP1A.231005.007", "10754064");
    EXPECT_EQ(os, "Android OS 14 / API-34 (UP1A.231005.007/10754064)");
    std::smatch match;
    ASSERT_TRUE(std::regex_search(os, match, std::regex("Android OS (\\d+)")));
    EXPECT_EQ(match[1].str(), "14");
    EXPECT_EQ(OsRules::windowsOperatingSystem(10, 0, 22631, true), "Windows 11 (10.0.22631) 64bit");
    EXPECT_EQ(OsRules::windowsOperatingSystem(10, 0, 19045, true), "Windows 10 (10.0.19045) 64bit");
    EXPECT_EQ(OsRules::parseBuildNumber("42"), 42);
    EXPECT_EQ(OsRules::parseBuildNumber("1.2"), 1);
    EXPECT_EQ(OsRules::parseBuildNumber("b7"), 7);
    EXPECT_EQ(OsRules::parseBuildNumber(""), 0);
    EXPECT_EQ(OsRules::parseBuildNumber("beta"), 0);
}

// ---- 9. project.yaml migration ---------------------------------------------------------------

TEST(PresentationSettings, AndroidOrientationMigrates) {
    struct Case { const char* android; ScreenOrientation orientation; uint8_t allowed; bool migrated; };
    const Case cases[] = {
        {"portrait", P, bits({P}), true},
        {"landscape", LL, bits({LL}), true},
        {"sensorPortrait", AUTO, bits({P, PU}), true},
        {"sensorLandscape", AUTO, bits({LL, LR}), true},
        {"fullSensor", AUTO, bits({P, PU, LL, LR}), true},
        {"unspecified", AUTO, PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS, false},
    };
    for (const Case& c : cases) {
        PresentationSettings presentation;
        EXPECT_EQ(Stream::migrateAndroidOrientation(c.android, presentation), c.migrated) << c.android;
        EXPECT_EQ(presentation.defaultOrientation, c.orientation) << c.android;
        EXPECT_EQ(presentation.allowedOrientations, c.allowed) << c.android;
    }
}

TEST(PresentationSettings, ProjectYamlRoundTripAndMigration) {
    Workspace workspace;
    {
        // an older project: the Android-only orientation and keepScreenOn
        Project project;
        YAML::Node node = YAML::Load("android:\n  orientation: sensorLandscape\n  keepScreenOn: false\n  minSdk: 24\n");
        Stream::decodeProject(&project, node, workspace);
        const PresentationSettings& presentation = project.getPresentationSettings();
        EXPECT_EQ(presentation.defaultOrientation, AUTO);
        EXPECT_EQ(presentation.allowedOrientations, bits({LL, LR}));
        // dropped: builds always kept the screen on
        EXPECT_TRUE(presentation.keepScreenOn);
        EXPECT_EQ(project.getAndroidProjectSettings().minSdk, 24u);

        const YAML::Node saved = Stream::encodeProject(&project);
        ASSERT_TRUE(saved["presentation"]);
        EXPECT_FALSE(saved["presentation"]["defaultOrientation"]);  // the default
        ASSERT_TRUE(saved["presentation"]["allowedOrientations"].IsSequence());
        EXPECT_EQ(saved["presentation"]["allowedOrientations"].size(), 2u);
        EXPECT_EQ(saved["presentation"]["allowedOrientations"][0].as<std::string>(), "landscape_left");
        EXPECT_FALSE(saved["android"]["orientation"]);
        EXPECT_FALSE(saved["android"]["keepScreenOn"]);
    }
    {
        // presentation wins over a stale android.orientation
        Project project;
        YAML::Node node = YAML::Load(
            "presentation:\n  defaultOrientation: landscape_right\n  keepScreenOn: false\n  targetFrameRate: 30\n"
            "android:\n  orientation: portrait\n");
        Stream::decodeProject(&project, node, workspace);
        const PresentationSettings& presentation = project.getPresentationSettings();
        EXPECT_EQ(presentation.defaultOrientation, LR);
        EXPECT_EQ(presentation.allowedOrientations, PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS);
        EXPECT_FALSE(presentation.keepScreenOn);
        EXPECT_EQ(presentation.targetFrameRate, 30);
        EXPECT_EQ(presentation.effectiveAllowedOrientations(), PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS);

        const YAML::Node saved = Stream::encodeProject(&project);
        EXPECT_EQ(saved["presentation"]["defaultOrientation"].as<std::string>(), "landscape_right");
        EXPECT_FALSE(saved["presentation"]["keepScreenOn"].as<bool>());
        EXPECT_EQ(saved["presentation"]["targetFrameRate"].as<int>(), 30);
        EXPECT_FALSE(saved["presentation"]["allowedOrientations"]);
    }
    {
        // all defaults: nothing written
        Project project;
        Stream::decodeProject(&project, YAML::Load("canvasWidth: 1280\ncanvasHeight: 720\n"), workspace);
        EXPECT_FALSE(Stream::encodeProject(&project)["presentation"]);
    }
}

// ---- editor device simulation -----------------------------------------------------------------

TEST(DeviceProfiles, SizesInsetsAndRotation) {
    const DeviceProfile* iphone = findDeviceProfile("iphone_15_pro");
    ASSERT_NE(iphone, nullptr);
    EXPECT_STREQ(iphone->model, "iPhone16,1");
    int width = 0, height = 0;
    profileSize(*iphone, LL, width, height);
    EXPECT_EQ(width, 2556);
    EXPECT_EQ(height, 1179);
    EXPECT_EQ(profileInsets(*iphone, P), EdgeInsets(0, 177, 0, 102));
    EXPECT_EQ(profileInsets(*iphone, LR), EdgeInsets(177, 0, 177, 63));
    // iPhones do not turn upside down
    EXPECT_FALSE(profileOrientationMask(*iphone) & PlatformRules::orientationBit(PU));
    EXPECT_EQ(nextProfileOrientation(*iphone, P, PlatformRules::ALL_ORIENTATIONS), LL);
    EXPECT_EQ(nextProfileOrientation(*iphone, LR, PlatformRules::ALL_ORIENTATIONS), P);
    EXPECT_EQ(nextProfileOrientation(*iphone, P, bits({P})), P);

    const DeviceProfile* pixel = findDeviceProfile("pixel_8");
    ASSERT_NE(pixel, nullptr);
    // the punch hole on the left in Unity's LandscapeLeft (turned counter-clockwise)
    EXPECT_EQ(profileInsets(*pixel, LL), EdgeInsets(128, 0, 0, 0));
    EXPECT_EQ(profileInsets(*pixel, LR), EdgeInsets(0, 0, 128, 0));
    EXPECT_EQ(profileInsets(*pixel, PU), EdgeInsets(0, 0, 0, 128));
    EXPECT_EQ(nextProfileOrientation(*pixel, LR, PlatformRules::ALL_ORIENTATIONS), PU);
    EXPECT_EQ(findDeviceProfile("nokia_3310"), nullptr);
    for (const DeviceProfile& profile : deviceProfiles()) {
        EXPECT_GT(profile.dpi, 100.0f) << profile.id;
        EXPECT_GT(profile.width, 0) << profile.id;
        EXPECT_LT(profile.width, profile.height) << profile.id;
    }
}

TEST(EditorPlatform, SimulatedDeviceTurnsForTheGamesRequests) {
    Project project;
    editor::Platform platform(&project);
    const DeviceProfile* iphone = findDeviceProfile("iphone_13");
    ASSERT_NE(iphone, nullptr);
    editor::Platform::setSimulatedDevice(iphone, P);
    editor::Platform::setSimulatedLanguage("es_MX");
    EXPECT_EQ(platform.getDeviceModel(), "iPhone14,5");
    EXPECT_EQ(platform.getDeviceType(), DeviceType::Handheld);
    EXPECT_FLOAT_EQ(platform.getScreenDpi(), 460.0f);
    EXPECT_EQ(platform.getLocale(), "es-MX");
    EXPECT_EQ(platform.getRegionCode(), "MX");
    EXPECT_EQ(platform.getSystemLanguage(), SystemLanguage::Spanish);
    EXPECT_TRUE(platform.isEditor());

    // a fixed landscape request turns the portrait device
    platform.applyOrientationPolicy(LL, PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS);
    EXPECT_EQ(editor::Platform::getSimulation().orientation, LL);
    EXPECT_EQ(platform.getDeviceOrientation(), DeviceOrientation::LandscapeLeft);
    // Screen.orientation follows the Game view's layout of it
    editor::Platform::setShownOrientation(LL);
    EXPECT_EQ(platform.getScreenOrientation(), LL);
    // auto-rotation that still allows it keeps it
    platform.applyOrientationPolicy(AUTO, bits({LL, LR}));
    EXPECT_EQ(editor::Platform::getSimulation().orientation, LL);
    platform.applyOrientationPolicy(AUTO, bits({LR}));
    EXPECT_EQ(editor::Platform::getSimulation().orientation, LR);
    // upside down: the iPhone cannot, it stays
    platform.applyOrientationPolicy(PU, bits({PU}));
    EXPECT_EQ(editor::Platform::getSimulation().orientation, LR);

    // Play stop: the toolbar's orientation again, no request
    editor::Platform::endPlaySimulation();
    EXPECT_EQ(editor::Platform::getSimulation().orientation, P);
    EXPECT_EQ(editor::Platform::getRequestedOrientation(), AUTO);

    // URLs and vibrations are recorded
    editor::Platform::resetPlayRecords();
    const System* previous = SystemTestAccess::swap(&platform);
    EXPECT_FALSE(platform.vibrate(400));
    EXPECT_EQ(editor::Platform::getVibrationCount(), 1);
    SystemTestAccess::swap(const_cast<System*>(previous));

    editor::Platform::setSimulatedDevice(nullptr, P);
    editor::Platform::setSimulatedLanguage("");
    EXPECT_EQ(platform.getDeviceType(), DeviceType::Desktop);
    EXPECT_EQ(platform.getDeviceOrientation(), DeviceOrientation::Unknown);
}

// ---- the generated project's platform sources ------------------------------------------------

TEST(GeneratedProject, PlatformSourcesListEveryBackendFile) {
    // a project built outside an export (Play with C++ scripts, `build_project` desktop) compiles
    // the desktop backends from this list: every source of platform/<os> must be in it (a missing
    // one, as HostServices*, fails at link time), but the entry points, D3D11 (not offered there)
    // and the HTTP and video backends (the engine library compiles them: JASPERENGINE_HTTP_BACKEND,
    // JASPERENGINE_VIDEO_BACKEND)
    Generator generator;
    const std::string cmake = GeneratorTestAccess::platformCMake(generator);
    const std::filesystem::path platform = std::filesystem::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "engine" / "platform";
    ASSERT_TRUE(std::filesystem::is_directory(platform)) << platform;
    int checked = 0;
    for (const char* os : {"linux", "win", "mac"}) {
        for (const auto& entry : std::filesystem::directory_iterator(platform / os)) {
            const std::string name = entry.path().filename().string();
            const std::string extension = entry.path().extension().string();
            if (extension != ".cpp" && extension != ".mm") continue;
            if ((std::string(os) != "mac" && name == "main.cpp") || name == "D3D11Context.cpp" ||
                name.rfind("HttpBackend", 0) == 0 || name.rfind("FFmpegBackend", 0) == 0 || name.rfind("MediaFoundationBackend", 0) == 0) continue;
            EXPECT_NE(cmake.find(std::string("${JASPERENGINE_PLATFORM_DIR}/") + os + "/" + name), std::string::npos)
                << os << "/" << name << " is not in the generated platform sources";
            checked++;
        }
    }
    EXPECT_GE(checked, 18);
    // what HostServicesWin links
    EXPECT_NE(cmake.find("ole32 uuid advapi32"), std::string::npos);
}
