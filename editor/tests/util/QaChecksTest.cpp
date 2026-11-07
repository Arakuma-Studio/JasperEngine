// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/QaChecks.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace jasperengine::editor;

TEST(QaChecks, RowHasExactlyTheSharedKeys){
    const qa::Json row = qa::row("pop", "content");
    std::vector<std::string> keys;
    for (auto it = row.begin(); it != row.end(); ++it) keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<std::string>{"at", "fix_hint", "id", "kind", "limited_by", "measured", "method", "round", "status",
                                              "threshold", "threshold_source"}));
    EXPECT_EQ(row["status"], "SKIP");
}

TEST(QaChecks, AtKeepsTenTimesRoundedToTheMillisecond){
    qa::Json row = qa::row("x", "content");
    for (int i = 0; i < 15; i++) qa::addAt(row, i * 0.12345);
    ASSERT_EQ(row["at"].size(), (size_t)qa::MAX_AT);
    EXPECT_DOUBLE_EQ(row["at"][1].get<double>(), 0.123);
}

TEST(QaChecks, VerdictIsTheWorstStatus){
    qa::Json rows = qa::Json::array();
    EXPECT_EQ(qa::verdict(rows), "SKIP");
    rows.push_back({{"status", "PASS"}});
    EXPECT_EQ(qa::verdict(rows), "PASS");
    rows.push_back({{"status", "WARN"}});
    EXPECT_EQ(qa::verdict(rows), "WARN");
    rows.push_back({{"status", "FAIL"}});
    EXPECT_EQ(qa::verdict(rows), "FAIL");
    EXPECT_TRUE(qa::reaches("WARN", "warn"));
    EXPECT_FALSE(qa::reaches("WARN", "fail"));
    EXPECT_TRUE(qa::reaches("FAIL", "fail"));
    EXPECT_FALSE(qa::reaches("FAIL", ""));
}

TEST(QaChecks, RoundsCountConsecutiveFailsAndSayWhenToStop){
    qa::FailRounds memory;
    for (int run = 1; run <= 3; run++){
        qa::Json rows = qa::Json::array({qa::row("a", "content"), qa::row("b", "content")});
        rows[0]["status"] = "FAIL";
        rows[1]["status"] = run == 2 ? "FAIL" : "PASS";
        const bool stop = qa::countRounds(rows, memory.previous("fx/a.particles"), 2);
        memory.record("fx/a.particles", rows);
        EXPECT_EQ(rows[0]["round"], run);
        EXPECT_EQ(rows[1]["round"], run == 2 ? 1 : 0);
        EXPECT_EQ(stop, run == 3);
    }
    // another scope has its own count
    EXPECT_TRUE(memory.previous("fx/b.particles").empty());
    EXPECT_EQ(memory.previous("fx/a.particles").at("a"), 3);
    EXPECT_EQ(memory.previous("fx/a.particles").count("b"), 0u);
}

TEST(QaChecks, AuditListsOnlyChangedThresholds){
    qa::ThresholdAudit audit;
    audit.add("particles.qa.pop_fraction", 0.3, 0.3);
    audit.add("particles.qa.fix_rounds", 4, 2);
    const qa::Json json = audit.json();
    EXPECT_EQ(json["values"].size(), 2u);
    ASSERT_EQ(json["changed"].size(), 1u);
    EXPECT_EQ(json["changed"][0]["key"], "particles.qa.fix_rounds");
    EXPECT_EQ(json["changed"][0]["default"], 2);
    EXPECT_TRUE(audit.changedAny());
}

TEST(QaChecks, GuidanceEndsWithTheClosingLine){
    EXPECT_EQ(qa::guidance(false, false, 2, "preview again", "Report these numbers."), "Report these numbers.");
    const std::string fail = qa::guidance(false, true, 2, "preview again", "Report these numbers.");
    EXPECT_NE(fail.find("preview again. Never loosen a threshold."), std::string::npos);
    const std::string stop = qa::guidance(true, true, 2, "preview again", "Report these numbers.");
    EXPECT_NE(stop.find("failed 3 times in a row"), std::string::npos);
    EXPECT_EQ(stop.substr(stop.size() - 21), "Report these numbers.");
}

TEST(QaChecks, RowLineSaysStatusMeasureLimitAndFix){
    qa::Json row = qa::row("capacity_hit", "content");
    EXPECT_EQ(qa::rowLine(row), "SKIP capacity_hit: not measured");
    row["status"] = "FAIL";
    row["measured"] = 200;
    row["limited_by"] = "Max Particles 400.";
    row["fix_hint"] = "Raise Max Particles.";
    EXPECT_EQ(qa::rowLine(row), "FAIL capacity_hit: 200. Max Particles 400. Raise Max Particles.");
}
