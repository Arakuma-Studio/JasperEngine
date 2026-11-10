// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Which language a title shows (docs/specs/S87-title-localisation-interchange.md R12-R14, D4,
// AC1, AC2): RFC 4647 lookup with the Chinese script rule, the candidate order, the global
// language of TitleArt.setLanguage and the sidecar's `languages` table.

#include <gtest/gtest.h>

#include "util/TitleArtLanguage.h"

#include <cctype>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    struct MatchRow {
        const char* candidate;
        std::vector<std::string> available;
        const char* expected;
    };

    const std::vector<MatchRow>& matchTable() {
        static const std::vector<MatchRow> rows = {
            {"zh-TW", {"en", "zh-Hans", "zh-Hant"}, "zh-Hant"},
            {"zh-HK", {"en", "zh-Hant-TW"}, "zh-Hant-TW"},
            {"zh-CN", {"en", "zh-Hant"}, ""},
            {"zh-CN", {"zh-Hant", "zh-Hans"}, "zh-Hans"},
            {"zh", {"zh-Hant", "zh-Hans"}, "zh-Hans"},
            {"zh-Hant-HK", {"zh-Hant-TW", "zh-Hant"}, "zh-Hant"},
            {"zh-MO", {"zh-Hans", "zh-Hant-HK"}, "zh-Hant-HK"},
            {"zh-SG", {"zh-Hant", "zh-Hans-SG"}, "zh-Hans-SG"},
            {"pt-PT", {"en", "pt-BR"}, "pt-BR"},
            {"pt-PT", {"pt-BR", "pt"}, "pt"},
            {"pt", {"pt-BR"}, "pt-BR"},
            {"en-GB", {"en", "de"}, "en"},
            {"en-GB", {"en-US", "en-GB"}, "en-GB"},
            {"en_gb", {"en-GB"}, "en-GB"},
            {"EN-us", {"en-US"}, "en-US"},
            {"sr-Latn", {"sr-Cyrl"}, ""},
            {"sr-Latn-RS", {"sr-Cyrl", "sr-Latn"}, "sr-Latn"},
            {"sr", {"sr-Latn"}, "sr-Latn"},
            {"sr-RS", {"sr-Cyrl", "sr-Latn"}, "sr-Cyrl"},
            {"iw", {"he"}, "he"},
            {"he-IL", {"iw"}, "iw"},
            {"in", {"id"}, "id"},
            {"ji", {"yi"}, "yi"},
            {"de-AT", {"de-DE", "de-CH"}, "de-DE"},
            {"de", {"de-CH"}, "de-CH"},
            {"fr-CA", {"fr", "fr-CA"}, "fr-CA"},
            {"es-419", {"es-ES", "es-419"}, "es-419"},
            {"es-MX", {"es-ES", "es-419"}, "es-ES"},
            {"ja-JP", {"ja"}, "ja"},
            {"ko", {"ja", "zh-Hans"}, ""},
            {"", {"en"}, ""},
            {"nl-BE", {"nl"}, "nl"},
        };
        return rows;
    }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // RFC 4647 basic lookup without the script rule or legacy codes: drop subtags from the end
    // until one equals an available tag (the red control of AC1)
    std::string prefixMatch(const std::string& candidate, const std::vector<std::string>& available) {
        std::string tag = lower(candidate);
        for (char& c : tag) if (c == '_') c = '-';
        while (!tag.empty()) {
            for (const std::string& a : available) {
                if (lower(a) == tag) return a;
            }
            const size_t dash = tag.rfind('-');
            if (dash == std::string::npos) break;
            tag = tag.substr(0, dash);
        }
        return "";
    }

    class TitleArtLanguageTest : public ::testing::Test {
    protected:
        void SetUp() override { TitleArtLanguage::resetForTesting(); }
        void TearDown() override { TitleArtLanguage::resetForTesting(); }
    };

}

TEST_F(TitleArtLanguageTest, MatchTable) {
    ASSERT_EQ(matchTable().size(), 32u);
    for (const MatchRow& row : matchTable()) {
        EXPECT_EQ(TitleArtLanguage::match(row.candidate, row.available), row.expected) << "candidate '" << row.candidate << "'";
    }
}

// AC1's red control: plain prefix matching gives Hong Kong users nothing and Portuguese users
// nothing; recorded: 17 of the 32 rows fail
TEST_F(TitleArtLanguageTest, PrefixMatcherFailsTheTable) {
    int failures = 0;
    for (const MatchRow& row : matchTable()) failures += prefixMatch(row.candidate, row.available) != row.expected ? 1 : 0;
    EXPECT_GE(failures, 6);
    EXPECT_EQ(failures, 17);
}

TEST_F(TitleArtLanguageTest, CanonicalAndValid) {
    EXPECT_EQ(TitleArtLanguage::canonicalize("zh_hant_tw"), "zh-Hant-TW");
    EXPECT_EQ(TitleArtLanguage::canonicalize("PT-br"), "pt-BR");
    EXPECT_EQ(TitleArtLanguage::canonicalize("iw-il"), "he-IL");
    EXPECT_EQ(TitleArtLanguage::canonicalize(""), "");
    for (const char* good : {"de", "pt-BR", "zh-Hant", "zh-Hant-TW", "es-419", "gsw", "sr-Latn-RS"}) {
        EXPECT_TRUE(TitleArtLanguage::isValid(good)) << good;
    }
    std::string error;
    for (const char* bad : {"", "xx_!", "d", "de--DE", "1de", "de-toolongsubtag"}) {
        EXPECT_FALSE(TitleArtLanguage::isValid(bad, &error)) << bad;
    }
    TitleArtLanguage::isValid("xx_!", &error);
    EXPECT_EQ(error, "'xx_!' is not a language tag (BCP 47, for example \"de\", \"pt-BR\", \"zh-Hant\")");
}

TEST_F(TitleArtLanguageTest, CandidateOrder) {
    const std::vector<std::string> available = {"en", "de", "ja", "fr"};
    const std::vector<std::string> device = {"it-IT", "ja-JP", "de-DE"};
    // the component wins
    EXPECT_EQ(TitleArtLanguage::resolve(TitleArtLanguage::candidates("fr", device, "en"), available), "fr");
    // then each preferred locale in order (it has no match, ja is next)
    EXPECT_EQ(TitleArtLanguage::resolve(TitleArtLanguage::candidates("", device, "en"), available), "ja");
    // the global language comes before the device
    ASSERT_TRUE(TitleArtLanguage::setGlobal("de"));
    EXPECT_EQ(TitleArtLanguage::resolve(TitleArtLanguage::candidates("", device, "en"), available), "de");
    EXPECT_EQ(TitleArtLanguage::resolve(TitleArtLanguage::candidates("fr", device, "en"), available), "fr") << "but not before the component";
    // "" returns to the device
    ASSERT_TRUE(TitleArtLanguage::setGlobal(""));
    EXPECT_EQ(TitleArtLanguage::resolve(TitleArtLanguage::candidates("", device, "en"), available), "ja");
    // the project default, then the source
    TitleArtLanguage::setProjectDefault("fr");
    EXPECT_EQ(TitleArtLanguage::resolve(TitleArtLanguage::candidates("", {"ko-KR"}, "en"), available), "fr");
    TitleArtLanguage::setProjectDefault("");
    EXPECT_EQ(TitleArtLanguage::resolve(TitleArtLanguage::candidates("", {"ko-KR"}, "en"), available), "en");
    // the component naming a language the title lacks resolves to its best match
    EXPECT_EQ(TitleArtLanguage::resolve(TitleArtLanguage::candidates("de-AT", {}, "en"), available), "de");
    const std::vector<std::string> expected = {"fr", "it-IT", "ja-JP", "en"};
    EXPECT_EQ(TitleArtLanguage::candidates("fr", {"it-IT", "", "ja-JP"}, "en"), expected) << "empty entries are left out";
}

TEST_F(TitleArtLanguageTest, GlobalLanguageEvents) {
    std::vector<std::string> heard;
    const int id = TitleArtLanguage::addListener([&](const std::string& code) { heard.push_back(code); });
    const uint64_t start = TitleArtLanguage::generation();
    ASSERT_TRUE(TitleArtLanguage::setGlobal("pt_br"));
    EXPECT_EQ(TitleArtLanguage::global(), "pt-BR");
    ASSERT_TRUE(TitleArtLanguage::setGlobal("pt-BR")) << "the same value again";
    std::string error;
    EXPECT_FALSE(TitleArtLanguage::setGlobal("xx_!", &error));
    EXPECT_EQ(TitleArtLanguage::global(), "pt-BR") << "an invalid tag changes nothing";
    EXPECT_FALSE(error.empty());
    ASSERT_TRUE(TitleArtLanguage::setGlobal(""));
    EXPECT_EQ(heard, std::vector<std::string>({"pt-BR", ""})) << "once per call that changes the value";
    EXPECT_EQ(TitleArtLanguage::generation(), start + 2);
    TitleArtLanguage::removeListener(id);
    TitleArtLanguage::setGlobal("ja");
    EXPECT_EQ(heard.size(), 2u);
}

TEST_F(TitleArtLanguageTest, SidecarTable) {
    const std::string text = R"({"format": "jasperengine.titleart.bakeinfo", "version": 1,
        "languages": {"source": "en", "default": "en",
                      "sets": {"de": "logo.titleart.de.bake.json", "ja": null, "pt-BR": "logo.titleart.pt-br.bake.json"}}})";
    TitleArtLanguageTable table;
    ASSERT_TRUE(TitleArtLanguage::parseTable(text, table));
    EXPECT_EQ(table.source, "en");
    EXPECT_EQ(table.available(), std::vector<std::string>({"en", "de", "ja", "pt-BR"})) << "the file's order";
    EXPECT_EQ(table.setFile("de"), "logo.titleart.de.bake.json");
    EXPECT_EQ(table.setFile("ja"), "") << "null: the source set";
    EXPECT_EQ(table.setFile("pt-br"), "logo.titleart.pt-br.bake.json");
    EXPECT_FALSE(TitleArtLanguage::parseTable(R"({"format": "jasperengine.titleart.bakeinfo", "version": 1})", table))
        << "a monolingual title's sidecar has no table";
    EXPECT_FALSE(table.present);
    EXPECT_FALSE(TitleArtLanguage::parseTable("not json", table));
}
