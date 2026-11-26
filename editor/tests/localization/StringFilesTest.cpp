// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CSV and XLIFF code String Tables and titles share (docs/specs/game-ui-localization.md D6,
// AC5): S87's CSV and XLIFF read through StringFiles, title rows reported as unknown keys by a
// table import, and ICU plurals surviving an XLIFF round trip.

#include "localization/StringFiles.h"
#include "localization/StringTableDocument.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine::editor;

TEST(StringFiles, ImportsS87CsvAndXliff) {
    // TitleStringsTest's shapes: a BOM, semicolons, a quoted newline, a Unity header
    StringsFile csv;
    std::string error;
    ASSERT_TRUE(parseStringsCsv("\xEF\xBB\xBFkey;context;en;German (de)\r\nui.menu.play;Main;Play;\"Spie\nlen\"\r\n", csv, error)) << error;
    ASSERT_EQ(csv.rows.size(), 1u);
    EXPECT_EQ(csv.rows[0].key, "ui.menu.play");
    EXPECT_EQ(csv.rows[0].source, "Play");
    EXPECT_EQ(csv.rows[0].targets.at("de"), "Spie\nlen");
    StringsFile xliff;
    ASSERT_TRUE(parseStringsXliff(
        "<xliff version=\"2.0\" srcLang=\"en\" trgLang=\"ja\"><file id=\"f\"><unit id=\"u\" name=\"ui.menu.play\">"
        "<segment state=\"initial\"><source>Play</source><target>プレイ</target></segment></unit></file></xliff>", xliff, error)) << error;
    ASSERT_EQ(xliff.rows.size(), 1u);
    EXPECT_EQ(xliff.rows[0].targets.at("ja"), "プレイ");
    EXPECT_EQ(xliff.rows[0].states.at("ja"), "needs_review");
}

TEST(StringFiles, RoutesTitleRowsAsUnknownKey) {
    // a row of another namespace (a title's string_namespace) is not this table's
    StringsExportDoc doc;
    doc.file = "assets/l10n/ui.stringtable";
    doc.sourceLanguage = "en";
    doc.languages = {"de"};
    doc.units.push_back({"ui.menu.play", "", "Play", 0, {}});
    const std::string csv = exportStringsCsv({doc}, {});
    StringsFile parsed;
    std::string error;
    ASSERT_TRUE(parseStringsCsv(csv + "logo.headline,,BIG WIN,GROSSER GEWINN\r\n", parsed, error)) << error;
    ASSERT_EQ(parsed.rows.size(), 2u);
    const std::string key = parsed.rows[1].key;
    EXPECT_NE(key.substr(0, key.find('.')), "ui") << "the title's row keeps its own namespace, so the import reports it";
}

TEST(StringFiles, PluralsSurviveXliffRoundTrip) {
    StringsExportDoc doc;
    doc.file = "assets/l10n/ui.stringtable";
    doc.sourceLanguage = "en";
    doc.languages = {"ru"};
    StringsExportUnit unit;
    unit.key = "ui.shop.coins";
    unit.source = "{count, plural, one {# coin} other {# coins}}";
    unit.maxChars = 20;
    unit.targets["ru"] = {"{count, plural, one {# монета} few {# монеты} many {# монет} other {# монеты}}", false};
    doc.units.push_back(unit);
    const auto files = exportStringsXliff({doc}, {});
    ASSERT_EQ(files.size(), 1u);
    EXPECT_NE(files[0].second.find("maxwidth=\"20\""), std::string::npos);
    StringsFile parsed;
    std::string error;
    ASSERT_TRUE(parseStringsXliff(files[0].second, parsed, error)) << error;
    ASSERT_EQ(parsed.rows.size(), 1u);
    EXPECT_EQ(parsed.rows[0].source, unit.source);
    EXPECT_EQ(parsed.rows[0].targets.at("ru"), unit.targets["ru"].text);
    EXPECT_EQ(parsed.rows[0].maxChars, 20);
}
