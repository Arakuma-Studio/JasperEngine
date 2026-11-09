// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Screen transitions on the CPU (S48 R12-R14, AC12): the mirror of transition.frag covers
// nothing at coverage 0 and everything in ink at 1, its rims are exactly `rim` art pixels wide
// along the whole edge, and the controller's phases (cover, hold, reveal, onCovered once, a new
// transition taking over from the coverage) run on the unscaled clock it is given.

#include "pixelart/ScreenTransition.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    const TransitionKind kAllKinds[] = {TransitionKind::IRIS, TransitionKind::BLINDS, TransitionKind::SLASH, TransitionKind::PIXEL_DISSOLVE,
                                        TransitionKind::CRT_OFF, TransitionKind::BURN_AWAY, TransitionKind::FADE};

    int countCells(const TransitionImage& image, uint8_t cell){
        int n = 0;
        for (uint8_t c : image.cells) n += c == cell ? 1 : 0;
        return n;
    }

}

TEST(ScreenTransitionCpu, CoverageZeroIsEmptyAndOneIsInk){
    for (TransitionKind kind : kAllKinds){
        for (TransitionPhase phase : {TransitionPhase::COVER, TransitionPhase::REVEAL}){
            TransitionOptions options;
            options.ink = Vector4(0.2f, 0.4f, 0.6f, 1.0f);
            TransitionImage image;
            ScreenTransition::evaluate(kind, options, 0.0f, phase, 80, 45, image);
            EXPECT_EQ(countCells(image, TransitionImage::UNCOVERED), 80 * 45) << transitionKindName(kind);
            for (const Rgba8& c : image.colors) ASSERT_EQ(c.a, 0) << transitionKindName(kind);

            ScreenTransition::evaluate(kind, options, 1.0f, phase, 80, 45, image);
            EXPECT_EQ(countCells(image, TransitionImage::INK), 80 * 45) << transitionKindName(kind);
            const Rgba8 ink{51, 102, 153, 255};
            for (const Rgba8& c : image.colors) ASSERT_EQ(c, ink) << transitionKindName(kind);
        }
    }
}

TEST(ScreenTransitionCpu, CoverageGrowsWithProgress){
    for (TransitionKind kind : kAllKinds){
        TransitionOptions options;
        int last = -1;
        for (float c : {0.1f, 0.25f, 0.5f, 0.75f, 0.9f}){
            TransitionImage image;
            ScreenTransition::evaluate(kind, options, c, TransitionPhase::COVER, 96, 54, image);
            const int covered = 96 * 54 - countCells(image, TransitionImage::UNCOVERED) - countCells(image, TransitionImage::LIGHT);
            EXPECT_GE(covered, last) << transitionKindName(kind) << " at " << c;
            last = covered;
        }
        EXPECT_GT(last, 0) << transitionKindName(kind);
    }
}

// AC12: the rim is the same width (±0 art pixels) along the whole edge, at 3 progress values
// and two rim widths, for every kind with an edge
TEST(ScreenTransitionCpu, RimWidthIsConstant){
    struct Case { TransitionKind kind; IrisShape shape; bool vertical; };
    const Case cases[] = {
        {TransitionKind::IRIS, IrisShape::CIRCLE, false}, {TransitionKind::IRIS, IrisShape::DIAMOND, false},
        {TransitionKind::IRIS, IrisShape::SQUARE, false}, {TransitionKind::BLINDS, IrisShape::CIRCLE, false},
        {TransitionKind::BLINDS, IrisShape::CIRCLE, true}, {TransitionKind::SLASH, IrisShape::CIRCLE, false},
        {TransitionKind::BURN_AWAY, IrisShape::CIRCLE, false}, {TransitionKind::CRT_OFF, IrisShape::CIRCLE, false}};
    for (const Case& test : cases){
        for (int rim : {2, 3}){
            for (float c : {0.25f, 0.5f, 0.9f}){
                for (TransitionPhase phase : {TransitionPhase::COVER, TransitionPhase::REVEAL}){
                    TransitionOptions options;
                    options.rim = rim;
                    options.shape = test.shape;
                    options.vertical = test.vertical;
                    options.count = 6;
                    TransitionImage image;
                    ScreenTransition::evaluate(test.kind, options, c, phase, 160, 90, image);
                    const TransitionRimMeasure measure = ScreenTransition::measureRim(test.kind, image);
                    const std::string what = std::string(transitionKindName(test.kind)) + " " + irisShapeName(test.shape) +
                                             (test.vertical ? " vertical" : "") + " rim " + std::to_string(rim) + " at " + std::to_string(c);
                    if (test.kind == TransitionKind::CRT_OFF && c > 0.55f)
                        continue;   // the line and the dot have no uncovered side
                    // near the end the uncovered part may be thinner than the rim: no edge to measure
                    if (c < 0.75f){
                        ASSERT_GT(measure.crossings, 0) << what;
                    }
                    if (measure.crossings == 0)
                        continue;
                    EXPECT_EQ(measure.minWidth, rim) << what << " at (" << measure.atX << ", " << measure.atY << ")";
                    EXPECT_EQ(measure.maxWidth, rim) << what << " at (" << measure.atX << ", " << measure.atY << ")";
                }
            }
        }
    }
}

TEST(ScreenTransitionCpu, RimTakesRampStepsWithoutBlending){
    TransitionOptions options;
    options.rampColors = {{10, 0, 0, 255}, {20, 0, 0, 255}, {30, 0, 0, 255}, {40, 0, 0, 255}};
    options.rim = 4;
    options.glow = 0.0f;
    TransitionImage image;
    ScreenTransition::evaluate(TransitionKind::SLASH, options, 0.5f, TransitionPhase::COVER, 120, 60, image);
    for (size_t i = 0; i < image.cells.size(); i++){
        if (image.cells[i] != TransitionImage::RIM) continue;
        // depth 1 is the hot (last) step, depth 4 the first
        EXPECT_EQ(image.colors[i].r, 50 - 10 * image.rimDepth[i]);
    }
    EXPECT_GT(countCells(image, TransitionImage::RIM), 0);
}

TEST(ScreenTransitionController, PlayCoversHoldsAndReveals){
    ScreenTransition::reset();
    int covered = 0;
    TransitionOptions options;
    options.ease = TransitionEase::LINEAR;
    const TransitionHandle handle = ScreenTransition::play(TransitionKind::IRIS, 1.0f, options, [&]{ covered++; });
    EXPECT_TRUE(ScreenTransition::isActive());
    EXPECT_FALSE(handle.isDone());
    EXPECT_EQ(ScreenTransition::getCoverage(), 0.0f);
    EXPECT_FALSE(ScreenTransition::needsDraw());    // coverage 0 draws nothing yet

    ScreenTransition::update(0.225);                // half the cover (45 %)
    EXPECT_NEAR(ScreenTransition::getCoverage(), 0.5f, 1e-4f);
    EXPECT_TRUE(ScreenTransition::needsDraw());
    ScreenTransition::update(0.225);
    EXPECT_EQ(ScreenTransition::getCoverage(), 1.0f);
    EXPECT_EQ(ScreenTransition::getPhase(), TransitionPhase::HOLD);
    EXPECT_EQ(covered, 1);
    ScreenTransition::update(0.05);
    EXPECT_EQ(ScreenTransition::getPhase(), TransitionPhase::HOLD);
    ScreenTransition::update(0.05);
    EXPECT_EQ(ScreenTransition::getPhase(), TransitionPhase::REVEAL);
    ScreenTransition::update(0.225);
    EXPECT_NEAR(ScreenTransition::getCoverage(), 0.5f, 1e-4f);
    ScreenTransition::update(0.3);
    EXPECT_EQ(ScreenTransition::getCoverage(), 0.0f);
    EXPECT_TRUE(handle.isDone());
    EXPECT_FALSE(ScreenTransition::isActive());
    EXPECT_FALSE(ScreenTransition::needsDraw());
    EXPECT_EQ(covered, 1);
}

TEST(ScreenTransitionController, ANewTransitionTakesOverFromTheCoverage){
    ScreenTransition::reset();
    TransitionOptions options;
    const TransitionHandle first = ScreenTransition::play(TransitionKind::BLINDS, 2.0f, options);
    ScreenTransition::update(0.5);
    const float coverage = ScreenTransition::getCoverage();
    ASSERT_GT(coverage, 0.1f);
    ASSERT_LT(coverage, 0.95f);
    const TransitionHandle second = ScreenTransition::cover(TransitionKind::FADE, 1.0f, options);
    EXPECT_TRUE(first.isDone());
    EXPECT_NEAR(ScreenTransition::getCoverage(), coverage, 1e-4f) << "no pop";
    ScreenTransition::update(1e-4);
    EXPECT_NEAR(ScreenTransition::getCoverage(), coverage, 2e-3f);
    ScreenTransition::update(1.0);
    EXPECT_TRUE(second.isDone());
    EXPECT_TRUE(ScreenTransition::isCovered());     // cover() holds until reveal()
    ScreenTransition::update(5.0);
    EXPECT_TRUE(ScreenTransition::isCovered());

    const TransitionHandle third = ScreenTransition::reveal(TransitionKind::FADE, 0.5f, options);
    EXPECT_EQ(ScreenTransition::getCoverage(), 1.0f);
    ScreenTransition::update(0.5);
    EXPECT_TRUE(third.isDone());
    EXPECT_FALSE(ScreenTransition::isActive());

    // a reveal with nothing running starts covered
    ScreenTransition::reveal(TransitionKind::IRIS, 0.5f, options);
    EXPECT_EQ(ScreenTransition::getCoverage(), 1.0f);
    ScreenTransition::stop();
    EXPECT_FALSE(ScreenTransition::isActive());
    EXPECT_FALSE(ScreenTransition::needsDraw());
}

TEST(ScreenTransitionController, OnCoveredMayStartAnother){
    ScreenTransition::reset();
    TransitionOptions options;
    TransitionHandle inner;
    ScreenTransition::play(TransitionKind::IRIS, 0.2f, options, [&]{
        inner = ScreenTransition::reveal(TransitionKind::SLASH, 0.4f, TransitionOptions());
    });
    ScreenTransition::update(0.2);
    ASSERT_TRUE(inner.isValid());
    EXPECT_EQ(ScreenTransition::getKind(), TransitionKind::SLASH);
    EXPECT_EQ(ScreenTransition::getPhase(), TransitionPhase::REVEAL);
    ScreenTransition::update(0.5);
    EXPECT_TRUE(inner.isDone());
    ScreenTransition::reset();
}

TEST(ScreenTransitionNames, RoundTrip){
    for (TransitionKind kind : kAllKinds){
        TransitionKind parsed;
        ASSERT_TRUE(transitionKindFromName(transitionKindName(kind), parsed));
        EXPECT_EQ(parsed, kind);
    }
    TransitionKind kind;
    EXPECT_FALSE(transitionKindFromName("swirl", kind));
    EXPECT_EQ(applyTransitionEase(TransitionEase::IN_OUT, 0.5f), 0.5f);
    EXPECT_EQ(applyTransitionEase(TransitionEase::IN, 1.0f), 1.0f);
}
