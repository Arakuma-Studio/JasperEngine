// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    CheckDef check(const std::string& id, TitleCheckKind kind, TitleCheckStatus status, const std::string& group = {}) {
        CheckDef def;
        def.id = id;
        def.kind = kind;
        def.owner = "test";
        def.group = group;
        def.run = [id, status](TitleCheckContext& context, std::vector<TitleCheckResult>& out) {
            TitleCheckResult r;
            r.status = status;
            r.measured = (int)context.doc.layers().size();
            r.threshold = 1;
            r.message = id + " measured";
            r.method = "count";
            if (status != TitleCheckStatus::Pass) {
                r.at = {{0, 0, 64, 64}};
                r.fix = Json{{"ops", Json::array({Json{{"op", "set"}, {"values", {{"seed", 2}}}}})}};
            }
            out.push_back(r);
        };
        return def;
    }

    class TitleArtChecksTest : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(TitleArtChecks::registerCheck(check("test_ok", TitleCheckKind::Document, TitleCheckStatus::Pass, "test_group")));
            ASSERT_TRUE(TitleArtChecks::registerCheck(check("test_warn", TitleCheckKind::Document, TitleCheckStatus::Warn, "test_group")));
            ASSERT_TRUE(TitleArtChecks::registerCheck(check("test_pixels", TitleCheckKind::Pixels, TitleCheckStatus::Fail)));
        }
        void TearDown() override {
            for (const char* id : {"test_ok", "test_warn", "test_pixels"}) TitleArtChecks::unregisterCheck(id);
        }
        titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
    };

}

TEST_F(TitleArtChecksTest, IdsRegisterOnce) {
    EXPECT_FALSE(TitleArtChecks::registerCheck(check("test_ok", TitleCheckKind::Document, TitleCheckStatus::Pass)))
        << "no two specs register the same id";
    CheckDef nameless;
    EXPECT_FALSE(TitleArtChecks::registerCheck(nameless));
    ASSERT_TRUE(TitleArtChecks::find("test_warn"));
    EXPECT_EQ(TitleArtChecks::find("test_warn")->owner, "test");
}

TEST_F(TitleArtChecksTest, ChecksAreSelectedByIdOrGroup) {
    std::vector<const CheckDef*> selected;
    std::string error;
    std::vector<std::string> valid;
    ASSERT_TRUE(TitleArtChecks::select({"test_group"}, selected, error, valid));
    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected[0]->id, "test_ok");
    ASSERT_TRUE(TitleArtChecks::select({"test_pixels", "test_ok"}, selected, error, valid));
    EXPECT_EQ(selected.size(), 2u);
    EXPECT_FALSE(TitleArtChecks::select({"test_okk"}, selected, error, valid));
    EXPECT_NE(std::find(valid.begin(), valid.end(), "test_ok"), valid.end());
    EXPECT_NE(std::find(valid.begin(), valid.end(), "test_group"), valid.end());
}

TEST_F(TitleArtChecksTest, AReportTakesTheWorstAndAlwaysCarriesTheGuidance) {
    std::vector<const CheckDef*> selected;
    std::string error;
    std::vector<std::string> valid;
    ASSERT_TRUE(TitleArtChecks::select({"test_group"}, selected, error, valid));
    TitleCheckContext context{doc, "assets/logo.titleart"};
    TitleCheckReport report = TitleArtChecks::run(selected, context);
    EXPECT_EQ(report.verdict, TitleCheckStatus::Warn);
    const Json json = report.toJson();
    EXPECT_EQ(json["verdict"], "WARN");
    EXPECT_EQ(json["guidance"], TitleArtChecks::guidance());
    ASSERT_EQ(json["checks"].size(), 2u);
    EXPECT_EQ(json["checks"][1]["id"], "test_warn");
    EXPECT_EQ(json["checks"][1]["fix"]["ops"][0]["op"], "set");
    EXPECT_EQ(json["checks"][1]["at"][0]["crop"], Json::array({0, 0, 64, 64}));
    EXPECT_EQ(json["look"].size(), 1u);
    context.options.failOnWarn = true;
    EXPECT_EQ(TitleArtChecks::run(selected, context).verdict, TitleCheckStatus::Fail);
    // a passing report says the same
    ASSERT_TRUE(TitleArtChecks::select({"test_ok"}, selected, error, valid));
    const Json passed = TitleArtChecks::run(selected, context).toJson();
    EXPECT_EQ(passed["verdict"], "PASS");
    EXPECT_NE(passed["guidance"].get<std::string>().find("does not mean the title looks good"), std::string::npos);
}

TEST_F(TitleArtChecksTest, PixelChecksSkipWithoutARenderer) {
    std::vector<const CheckDef*> selected;
    std::string error;
    std::vector<std::string> valid;
    ASSERT_TRUE(TitleArtChecks::select({"test_pixels"}, selected, error, valid));
    TitleCheckContext headless{doc, "assets/logo.titleart"};
    const TitleCheckReport skipped = TitleArtChecks::run(selected, headless);
    ASSERT_EQ(skipped.checks.size(), 1u);
    EXPECT_EQ(skipped.checks[0].status, TitleCheckStatus::Skip);
    EXPECT_EQ(skipped.toJson()["verdict"], "PASS");
    TitleCheckContext gpu{doc, "assets/logo.titleart"};
    gpu.render = [](const titleart::RenderRequest&, TitleCheckImage&, std::string&) { return true; };
    EXPECT_EQ(TitleArtChecks::run(selected, gpu).verdict, TitleCheckStatus::Fail);
}
