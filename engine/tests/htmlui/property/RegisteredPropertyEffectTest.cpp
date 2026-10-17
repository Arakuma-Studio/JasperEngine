// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Registered custom properties driving effect parameters (phase-5 spec 3.4 item 5, 3.11): an
// effect argument `var(--x)` of a registered property follows its transitions and keyframes frame
// by frame (getEffects at fixed UI-clock times), as the guide's "Animating effects" example
// shows; an unregistered one changes at once. The UI clock is the fixture runner's fixed clock,
// moved by `advance` steps.

#include "effects/EffectTestCommon.h"

#include "htmlui/effects/EffectInstancers.h"

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    std::string page(const std::string& css, const std::string& attributes = std::string()){
        return "<!DOCTYPE html><html><head><style>html, body { margin: 0; width: 200px; height: 120px; }\n"
               "#fx { position: absolute; left: 10px; top: 10px; width: 160px; height: 80px; }\n" + css +
               "</style></head><body><div id=\"fx\"" + attributes + "></div></body></html>";
    }

    struct RegisteredPropertyEffectTest : ::testing::Test {
        std::unique_ptr<EffectScene> scene;

        void SetUp() override{
            TestSystem::get().clearLogs();
            scene = std::make_unique<EffectScene>(200, 120, 1);
            if (!scene->valid())
                GTEST_SKIP() << "no GPU";
        }

        void TearDown() override{
            scene.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u) << logText(D_LOG_WARN);
        }

        bool load(const std::string& css, const std::string& attributes = std::string()){
            std::string error;
            const bool ok = scene->loadHtml(page(css, attributes), &error);
            EXPECT_TRUE(ok) << error;
            return ok;
        }

        void step(FixtureStep::Kind kind, const std::string& name = std::string(), double seconds = 0.0){
            FixtureStep s;
            s.kind = kind;
            s.id = "fx";
            s.name = name;
            s.seconds = seconds;
            std::string error;
            EXPECT_TRUE(scene->fixture().step(s, &error)) << error;
        }

        void advance(double seconds){ step(FixtureStep::Kind::Advance, "", seconds); }

        std::string param(const std::string& name){
            const std::vector<EffectUse> uses = collectEffectUses(scene->element("fx"));
            if (uses.empty())
                return "<no effect>";
            for (const auto& p : uses[0].params){
                if (p.first == name)
                    return p.second;
            }
            return "<no param " + name + ">";
        }

        TestImage image(){
            TestImage captured;
            EXPECT_TRUE(scene->capture(captured));
            return captured;
        }
    };

    size_t differentPixels(const TestImage& a, const TestImage& b){
        if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size())
            return a.rgba.size() / 4;
        size_t count = 0;
        for (size_t i = 0; i + 3 < a.rgba.size(); i += 4){
            int largest = 0;
            for (size_t c = 0; c < 4; c++)
                largest = std::max(largest, std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])));
            if (largest > 24)
                count++;
        }
        return count;
    }

}

// The guide's example ("Animating effects"): keyframes on a registered <angle> turn the stripes.
TEST_F(RegisteredPropertyEffectTest, KeyframesDriveAnEffectParameter){
    ASSERT_TRUE(load("@property --spin { syntax: '<angle>'; inherits: false; initial-value: 0deg; }\n"
                     "@keyframes turn { to { --spin: 360deg; } }\n"
                     ".ring { background-image: effect(stripes, angle var(--spin), speed 0); animation: turn 4s linear infinite; border-radius: 50%; }\n",
                     " class=\"ring\""));
    advance(1.0);
    EXPECT_EQ(param("angle"), "90deg");
    const TestImage quarter = image();
    advance(0.5);
    EXPECT_EQ(param("angle"), "135deg");
    const TestImage eighth = image();
    advance(3.0);   // 4.5 s: the second iteration
    EXPECT_EQ(param("angle"), "45deg");
    // the stripes turn: the frames differ
    EXPECT_GT(differentPixels(quarter, eighth), 1000u);
}

TEST_F(RegisteredPropertyEffectTest, TransitionsDriveEffectParameters){
    ASSERT_TRUE(load("@property --rpe-a { syntax: '<angle>'; inherits: false; initial-value: 0deg; }\n"
                     "@property --rpe-d { syntax: '<number>'; inherits: false; initial-value: 4; }\n"
                     "#fx { background-image: effect(stripes, angle var(--rpe-a), density var(--rpe-d), speed 0);\n"
                     "      transition: --rpe-a 1s linear, --rpe-d 1s linear; }\n"
                     "#fx.on { --rpe-a: 90deg; --rpe-d: 8; }\n"));
    EXPECT_EQ(param("angle"), "0deg");
    EXPECT_EQ(param("density"), "4");
    step(FixtureStep::Kind::AddClass, "on");
    advance(0.5);
    EXPECT_EQ(param("angle"), "45deg");
    EXPECT_EQ(param("density"), "6");
    advance(0.6);
    EXPECT_EQ(param("angle"), "90deg");
    EXPECT_EQ(param("density"), "8");
}

TEST_F(RegisteredPropertyEffectTest, UnregisteredParametersChangeAtOnce){
    ASSERT_TRUE(load("#fx { --rpe-u: 0deg; background-image: effect(stripes, angle var(--rpe-u), speed 0); transition: --rpe-u 1s linear; }\n"
                     "#fx.on { --rpe-u: 90deg; }\n"));
    step(FixtureStep::Kind::AddClass, "on");
    advance(0.1);
    EXPECT_EQ(param("angle"), "90deg");
}
