// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S92 layer cost (docs/specs/S92-htmlui-render-cost.md R29, AC10): the passes per frame of the
// port's UI patterns (fixtures/s92/), cold (the first frame drawn), warm (another element changed)
// and idle (nothing changed), at 4 samples and at 1. Every row is printed ("[ s92 ]" lines); the
// budgets asserted are those of the packages that landed (S92-A: the idle column and the static
// row; the cold and warm columns of the layered rows are S92-B2's).

#include "FixtureRunner.h"
#include "TestRenderCommon.h"

#include "HtmlUIClock.h"

#include <RmlUi/Core/ElementDocument.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct PatternFrames {
        HtmlUIRenderStats cold, warm, idle;
    };

    // the element a warm frame changes (#tick, or the fixture's own)
    void changeText(FixtureRunner& runner, const char* id, int value){
        Rml::Element* element = runner.document() ? runner.document()->GetElementById(id) : nullptr;
        ASSERT_NE(element, nullptr) << "#" << id;
        element->SetInnerRML(std::to_string(value));
    }

    // cold: the first frame that draws; then a frame for what it loaded; warm: #changed changed; idle:
    // the second frame after it with nothing changed
    bool measurePattern(const std::string& name, int samples, const char* changed, PatternFrames& frames){
        FixtureRunner runner(1280, 720, samples);
        if (!runner.valid())
            return false;
        std::string error;
        if (!runner.load("fixtures/s92/" + name + ".html", &error)){
            ADD_FAILURE() << name << ": " << error;
            return false;
        }
        double clock = runner.time();
        bool cold = false;
        for (int i = 0; i < 60 && !cold; i++){
            clock += 1.0 / 60.0;
            runner.frameAt(clock, true);
            cold = runner.renderer()->getStats().passes > 0;
            if (cold)
                frames.cold = runner.renderer()->getStats();
        }
        if (!cold){
            ADD_FAILURE() << name << ": no frame was drawn";
            return false;
        }
        HtmlUI::getFontEngine().finishDistanceGlyphsForTesting();
        clock += 1.0 / 60.0;
        runner.frameAt(clock, true);
        changeText(runner, changed, 1);
        clock += 1.0 / 60.0;
        runner.frameAt(clock, true);
        frames.warm = runner.renderer()->getStats();
        for (int i = 0; i < 2; i++){
            clock += 1.0 / 60.0;
            runner.frameAt(clock, true);
        }
        frames.idle = runner.renderer()->getStats();
        return true;
    }

    void printPattern(const std::string& name, int samples, const PatternFrames& f){
        std::printf("[ s92 ] %-16s %dx cold: passes %3u layers %3u layerPixels %8u | warm: passes %3u layers %3u kept %u | "
                    "idle: passes %3u draws %3u kept %u compositePixels %7u\n",
                    name.c_str(), samples, f.cold.passes, f.cold.layersPushed, f.cold.layerPixels, f.warm.passes, f.warm.layersPushed,
                    f.warm.framesKept, f.idle.passes, f.idle.drawCalls, f.idle.framesKept, f.idle.compositePixels);
    }

}

TEST(RenderLayerCost, ArcadePatterns){
    JASPER_REQUIRE_GPU();
    struct Row {
        const char* name;
        const char* changed;
    };
    const Row rows[] = {{"static", "tick"},     {"rounded-gradient", "tick"}, {"btn", "tick"},     {"card", "tick"},
                        {"inset-clip", "tick"}, {"cards4", "timer"},          {"o2-frame", "tick"}};
    for (int samples : {4, 1}){
        for (const Row& row : rows){
            PatternFrames frames;
            if (!measurePattern(row.name, samples, row.changed, frames))
                continue;
            printPattern(row.name, samples, frames);
            // S92-A (R1, R29's idle column and static row): nothing changed, nothing drawn
            EXPECT_EQ(frames.idle.passes, 0u) << row.name << " at " << samples << "x";
            EXPECT_EQ(frames.idle.drawCalls, 0u) << row.name << " at " << samples << "x";
            EXPECT_EQ(frames.idle.framesKept, 1u) << row.name << " at " << samples << "x";
            if (std::string(row.name) == "static"){
                EXPECT_EQ(frames.cold.passes, 1u) << samples << "x";
                EXPECT_EQ(frames.warm.passes, 1u) << samples << "x";
            }
        }
    }
}
