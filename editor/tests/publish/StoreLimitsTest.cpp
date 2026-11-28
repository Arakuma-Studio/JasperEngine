// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The stores' limits a Build Profile's build is checked against (docs/specs/build-profiles.md R16, AC7)

#include <gtest/gtest.h>

#include "publish/StoreLimits.h"

#include <string>

namespace StoreLimitsTest_cpp {

using namespace jasperengine::editor::publish;

namespace {

    bool has(const std::vector<StoreFinding>& findings, const std::string& code) {
        for (const StoreFinding& finding : findings)
            if (finding.code == code) return true;
        return false;
    }

    StoreLimitInput valid(const std::string& store) {
        StoreLimitInput input;
        input.store = store;
        input.storeTitle = "My Game";
        input.label = "My Game";
        input.identifier = "com.studio.mygame";
        input.version = "1.2.0";
        return input;
    }

}

TEST(StoreLimits, GooglePlayAndAppStoreTitlesAre30CodePoints) {
    // the table itself (the one place the numbers live)
    size_t titleRows = 0;
    for (const StoreLimitRow& row : storeLimitRows()) {
        if (std::string(row.checked) != "store_title") continue;
        titleRows++;
        EXPECT_EQ(row.limit, 30u) << row.store;
        EXPECT_STREQ(row.code, "W-STORE-TITLE-LENGTH");
        EXPECT_NE(std::string(row.source), "");
    }
    EXPECT_EQ(titleRows, 2u);
    for (const std::string store : {"google-play", "app-store"}) {
        StoreLimitInput input = valid(store);
        input.storeTitle = std::string(30, 'a');
        EXPECT_TRUE(checkStoreLimits(input).empty()) << store;
        input.storeTitle = std::string(31, 'a');
        const std::vector<StoreFinding> findings = checkStoreLimits(input);
        ASSERT_TRUE(has(findings, "W-STORE-TITLE-LENGTH")) << store;
        EXPECT_TRUE(findings.front().error) << "publish prepare refuses it";
        EXPECT_NE(findings.front().message.find("31 characters"), std::string::npos) << findings.front().message;
    }
    // stores without a title limit check nothing beyond the identifiers
    for (const std::string store : {"steam", "itch", "web", "none", ""}) {
        StoreLimitInput input = valid(store);
        input.storeTitle = std::string(100, 'a');
        input.label = std::string(100, 'a');
        input.identifier = "anything at all";
        EXPECT_TRUE(checkStoreLimits(input).empty()) << store;
        EXPECT_TRUE(isStoreName(store)) << store;
    }
    EXPECT_FALSE(isStoreName("gog"));
}

TEST(StoreLimits, LabelTruncationWarnsAbove12) {
    StoreLimitInput input = valid("google-play");
    input.label = "Twelve Chars";
    EXPECT_TRUE(checkStoreLimits(input).empty());
    input.label = "Thirteen Char";
    std::vector<StoreFinding> findings = checkStoreLimits(input);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings.front().code, "W-LABEL-TRUNCATED");
    EXPECT_FALSE(findings.front().error) << "a warning, even at publish prepare";
    input = valid("app-store");
    input.label = "A Long Home Screen Name";
    EXPECT_TRUE(has(checkStoreLimits(input), "W-LABEL-TRUNCATED"));
    // the identifiers and the App Store's numeric version are errors
    input = valid("google-play");
    input.identifier = "com.studio.my-game";
    EXPECT_TRUE(has(checkStoreLimits(input), "E-STORE-IDENTIFIER"));
    input = valid("app-store");
    input.identifier = "com.studio.my-game";
    EXPECT_FALSE(has(checkStoreLimits(input), "E-STORE-IDENTIFIER"));
    input.version = "1.2.0-demo";
    EXPECT_TRUE(has(checkStoreLimits(input), "E-STORE-VERSION"));
}

TEST(StoreLimits, CountsCodePointsNotBytes) {
    EXPECT_EQ(codePoints("abc"), 3u);
    EXPECT_EQ(codePoints("\xC3\xA9t\xC3\xA9"), 3u);          // "été": 5 bytes
    EXPECT_EQ(codePoints("\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0"), 3u);   // three CJK characters, 9 bytes
    EXPECT_EQ(codePoints("\xF0\x9F\x8E\xAE"), 1u);          // an emoji, 4 bytes
    StoreLimitInput input = valid("google-play");
    // 30 two-byte characters: 60 bytes, within the limit
    input.storeTitle.clear();
    for (int i = 0; i < 30; i++) input.storeTitle += "\xC3\xA9";
    EXPECT_FALSE(has(checkStoreLimits(input), "W-STORE-TITLE-LENGTH"));
    input.storeTitle += "\xC3\xA9";
    EXPECT_TRUE(has(checkStoreLimits(input), "W-STORE-TITLE-LENGTH"));
    // 12 three-byte characters on the home screen: no warning
    input = valid("app-store");
    input.label.clear();
    for (int i = 0; i < 12; i++) input.label += "\xE3\x83\xBC";
    EXPECT_FALSE(has(checkStoreLimits(input), "W-LABEL-TRUNCATED"));
}

}  // namespace StoreLimitsTest_cpp
