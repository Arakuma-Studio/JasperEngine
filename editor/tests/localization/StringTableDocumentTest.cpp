// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// String Table and translation files (docs/specs/game-ui-localization.md R1, R2, R39, AC4): the
// byte-identical round trip with unknown members kept, source edits marking translations for
// review, newer versions refused, and namespaces shared with Title Art refused.

#include "localization/StringTableDocument.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace StringTableDocumentTest_cpp {

using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace st = jasperengine::editor::l10n;

namespace {

    const char* kTable =
        "{\n"
        "  \"format\": \"jasperengine.stringtable\",\n"
        "  \"version\": 1,\n"
        "  \"guid\": \"6f0c0000000000000000000000000001\",\n"
        "  \"namespace\": \"ui\",\n"
        "  \"source_language\": \"en\",\n"
        "  \"entries\": [\n"
        "    {\n"
        "      \"key\": \"menu.play\",\n"
        "      \"text\": \"Play\",\n"
        "      \"context\": \"Main menu button\",\n"
        "      \"max_chars\": 12,\n"
        "      \"voice\": \"vo/play.ogg\"\n"
        "    },\n"
        "    {\n"
        "      \"key\": \"shop.coins\",\n"
        "      \"text\": \"{count, plural, one {# coin} other {# coins}}\"\n"
        "    }\n"
        "  ],\n"
        "  \"tool_data\": {\n"
        "    \"author\": \"someone\"\n"
        "  }\n"
        "}\n";

    const char* kTranslation =
        "{\n"
        "  \"format\": \"jasperengine.translation\",\n"
        "  \"version\": 1,\n"
        "  \"guid\": \"a91e0000000000000000000000000002\",\n"
        "  \"owner\": {\n"
        "    \"guid\": \"6f0c0000000000000000000000000001\",\n"
        "    \"path\": \"assets/l10n/ui.stringtable\"\n"
        "  },\n"
        "  \"language\": \"de\",\n"
        "  \"entries\": {\n"
        "    \"menu.play\": {\n"
        "      \"text\": \"Spielen\",\n"
        "      \"status\": \"translated\",\n"
        "      \"source_hash\": \"0\",\n"
        "      \"reviewer\": \"anna\"\n"
        "    }\n"
        "  },\n"
        "  \"vendor\": 7\n"
        "}\n";

}

TEST(StringTableDocument, RoundTripIsByteIdentical) {
    st::StringTableDoc table;
    ASSERT_TRUE(st::parseStringTable(kTable, table));
    EXPECT_EQ(table.ns, "ui");
    ASSERT_EQ(table.entries.size(), 2u);
    EXPECT_EQ(table.entries[0].maxChars, 12);
    EXPECT_EQ(st::saveStringTable(table), kTable);
    st::TranslationDoc translation;
    ASSERT_TRUE(st::parseTranslation(kTranslation, translation));
    EXPECT_EQ(translation.language, "de");
    EXPECT_EQ(translation.entries.at("menu.play").text, "Spielen");
    EXPECT_EQ(st::saveTranslation(translation), kTranslation);
}

TEST(StringTableDocument, SourceEditMarksTranslationsForReview) {
    jasperengine::test::TempDir dir;
    st::StringTableSet set;
    std::string error;
    ASSERT_TRUE(st::createTable(dir.path(), dir.path() / "assets" / "l10n" / "ui.stringtable", "", "en", set, error)) << error;
    EXPECT_EQ(set.table.ns, "ui");
    ASSERT_TRUE(st::applyOps(set, st::Json::parse(R"([
        {"op": "set", "key": "menu.play", "text": "Play"},
        {"op": "set", "key": "menu.quit", "text": "Quit"},
        {"op": "add_language", "language": "de"},
        {"op": "set", "key": "menu.play", "language": "de", "text": "Spielen"},
        {"op": "set", "key": "menu.quit", "language": "de", "text": "Beenden"}])"), error)) << error;
    EXPECT_EQ(set.translations.at("de").entries.at("menu.play").status, "translated");
    ASSERT_TRUE(st::applyOps(set, st::Json::parse(R"([{"op": "set", "key": "menu.play", "text": "Start"}])"), error)) << error;
    EXPECT_EQ(set.translations.at("de").entries.at("menu.play").status, "needs_review");
    EXPECT_EQ(set.translations.at("de").entries.at("menu.quit").status, "translated");
    EXPECT_EQ(set.stale("de"), 1u);
    ASSERT_TRUE(st::applyOps(set, st::Json::parse(R"([{"op": "mark_reviewed", "key": "menu.play", "language": "de"}])"), error)) << error;
    EXPECT_EQ(set.stale("de"), 0u);
    // one failing op refuses them all, naming it
    const st::StringTableSet before = set;
    EXPECT_FALSE(st::applyOps(set, st::Json::parse(R"([
        {"op": "set", "key": "menu.back", "text": "Back"},
        {"op": "set", "key": "menu.coins", "text": "{n, plural, one {x}}"}])"), error));
    EXPECT_NE(error.find("op 1 (set)"), std::string::npos) << error;
    EXPECT_NE(error.find("column"), std::string::npos) << error;
    EXPECT_EQ(set.table.entries.size(), before.table.entries.size());
    EXPECT_EQ(st::pluralSkeleton("ru"), "{count, plural, one {# } few {# } many {# } other {# }}");
}

TEST(StringTableDocument, NewerVersionIsRefusedWithTheVersionNeeded) {
    std::string text = kTable;
    text.replace(text.find("\"version\": 1"), 12, "\"version\": 2");
    st::StringTableDoc table;
    st::ParseError error;
    EXPECT_FALSE(st::parseStringTable(text, table, &error));
    EXPECT_EQ(error.code, "version_newer");
    EXPECT_NE(error.message.find("version 2"), std::string::npos) << error.message;
    EXPECT_NE(error.message.find("reads up to version 1"), std::string::npos) << error.message;
}

TEST(StringTableDocument, NamespaceClashWithTitleArtIsAnError) {
    jasperengine::test::TempDir dir;
    dir.write("assets/titles/logo.titleart", R"({"format": "jasperengine.titleart", "version": 1, "string_namespace": "logo"})");
    st::StringTableSet set;
    std::string error;
    EXPECT_FALSE(st::createTable(dir.path(), dir.path() / "assets" / "l10n" / "logo.stringtable", "", "en", set, error));
    EXPECT_NE(error.find("l10n-namespace-taken"), std::string::npos) << error;
    EXPECT_NE(error.find("assets/titles/logo.titleart"), std::string::npos) << error;
    EXPECT_TRUE(st::createTable(dir.path(), dir.path() / "assets" / "l10n" / "logo.stringtable", "logo_ui", "en", set, error)) << error;
}

}  // namespace StringTableDocumentTest_cpp
