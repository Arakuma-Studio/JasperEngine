// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The family's lint result shape and verdicts (docs/specs/S44-aseprite-import.md §6.3, S46 R25)

#include "pixelart/PixelProblem.h"
#include "pixelart/lint/LintTypes.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::lint;

TEST(LintTypesTest, ResultJsonShape) {
    LintResult r;
    r.id = "orphans";
    r.status = LintStatus::Warn;
    r.measured = 12;
    r.threshold = 1;
    r.thresholdSource = "default";
    for (int i = 0; i < 12; i++) r.addSpot({1, i, 2 * i});
    r.message = "12 stray pixels";
    const nlohmann::json j = toJson(r);
    EXPECT_EQ(j["id"], "orphans");
    EXPECT_EQ(j["status"], "WARN");
    EXPECT_EQ(j["count"], 12);
    ASSERT_EQ(j["at"].size(), LintResult::kMaxSpots);   // at most 8 spots, all counted
    EXPECT_EQ(j["at"][3], nlohmann::json::array({1, 3, 6}));
    EXPECT_EQ(j["threshold_source"], "default");
    EXPECT_FALSE(j.contains("fix"));   // empty text is left out
}

TEST(LintTypesTest, ReportCountsAndOverall) {
    LintReport report;
    EXPECT_EQ(report.overall(), LintStatus::Pass);
    LintResult pass;
    pass.id = "size";
    pass.status = LintStatus::Pass;
    report.add(pass);
    LintResult skip;
    skip.id = "colors";
    skip.message = "no rules.max_colors";
    report.add(skip);
    EXPECT_EQ(report.overall(), LintStatus::Pass);
    LintResult fail;
    fail.id = "partial_alpha";
    fail.status = LintStatus::Fail;
    report.add(fail);
    EXPECT_EQ(report.pass, 1);
    EXPECT_EQ(report.skip, 1);
    EXPECT_EQ(report.fail, 1);
    EXPECT_EQ(report.overall(), LintStatus::Fail);
    report.results[2].status = LintStatus::Warn;
    report.recount();
    EXPECT_EQ(report.fail, 0);
    EXPECT_EQ(report.warn, 1);
    const nlohmann::json j = toJson(report);
    EXPECT_EQ(j["status"], "WARN");
    EXPECT_EQ(j["results"].size(), 3u);
    LintStatus parsed;
    EXPECT_TRUE(parseLintStatus("SKIP", parsed));
    EXPECT_EQ(parsed, LintStatus::Skip);
    EXPECT_FALSE(parseLintStatus("skip", parsed));
}

TEST(LintTypesTest, Verdicts) {
    EXPECT_STREQ(verdictName(Verdict::NeedsPerson), "needs_person");
    Verdict v;
    EXPECT_TRUE(parseVerdict("fixable", v));
    EXPECT_EQ(v, Verdict::Fixable);
    EXPECT_EQ(worse(Verdict::Clean, Verdict::Fixable), Verdict::Fixable);
    EXPECT_EQ(worse(Verdict::NeedsPerson, Verdict::Fixable), Verdict::NeedsPerson);
    PixelProblem p{"unknown_layer", "layers.include", "the layer 'Arm' does not exist", "layers are: Body, Body/Hand"};
    EXPECT_FALSE(p.empty());
    EXPECT_TRUE(PixelProblem{}.empty());
}
