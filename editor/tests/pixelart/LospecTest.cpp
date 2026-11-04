// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lospec by slug (docs/specs/S43-palettes.md R8): the slug forms and the parsing of recorded
// answers. No network here: the fetch itself is AC3's MCP test, against the recorded mirror.

#include "pixelart/Lospec.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

namespace lospec_test {

    namespace px = jasperengine::pixelart;
    using namespace jasperengine::editor::pixelart;

    std::string lospecFixture(const std::string& name) {
        std::ifstream file(std::filesystem::path(JASPER_TEST_FIXTURES_DIR) / "palettes" / name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    PaletteError lospecError(const std::variant<px::Palette, PaletteError>& result) {
        EXPECT_TRUE(std::holds_alternative<PaletteError>(result));
        return std::holds_alternative<PaletteError>(result) ? std::get<PaletteError>(result) : PaletteError();
    }

    TEST(Lospec, NormaliseSlug) {
        EXPECT_EQ(normaliseSlug("endesga-32"), std::optional<std::string>("endesga-32"));
        EXPECT_EQ(normaliseSlug("  Endesga-32 \n"), std::optional<std::string>("endesga-32"));
        EXPECT_EQ(normaliseSlug("https://lospec.com/palette-list/endesga-32"), std::optional<std::string>("endesga-32"));
        EXPECT_EQ(normaliseSlug("https://lospec.com/palette-list/endesga-32/"), std::optional<std::string>("endesga-32"));
        EXPECT_EQ(normaliseSlug("https://lospec.com/palette-list/endesga-32.json"), std::optional<std::string>("endesga-32"));
        EXPECT_EQ(normaliseSlug("http://www.lospec.com/palette-list/pico-8?ref=x"), std::optional<std::string>("pico-8"));
        EXPECT_EQ(normaliseSlug("lospec.com/palette-list/sweetie-16"), std::optional<std::string>("sweetie-16"));
        EXPECT_EQ(normaliseSlug("lospec-palette://endesga-32"), std::optional<std::string>("endesga-32"));
        EXPECT_EQ(normaliseSlug("LOSPEC-PALETTE://AAP-64/"), std::optional<std::string>("aap-64"));

        EXPECT_FALSE(normaliseSlug("").has_value());
        EXPECT_FALSE(normaliseSlug("endesga 32").has_value());
        EXPECT_FALSE(normaliseSlug("endesga_32").has_value());
        EXPECT_FALSE(normaliseSlug("../etc/passwd").has_value());
        EXPECT_FALSE(normaliseSlug("https://example.com/palette-list/endesga-32").has_value());
        EXPECT_FALSE(normaliseSlug("https://lospec.com/palettes/endesga-32").has_value());
        EXPECT_FALSE(normaliseSlug("ftp://lospec.com/palette-list/endesga-32").has_value());
        EXPECT_TRUE(normaliseSlug(std::string(64, 'a')).has_value());
        EXPECT_FALSE(normaliseSlug(std::string(65, 'a')).has_value());
    }

    TEST(Lospec, Urls) {
        EXPECT_EQ(lospecPageUrl("endesga-32"), "https://lospec.com/palette-list/endesga-32");
        EXPECT_EQ(lospecJsonUrl("endesga-32"), "https://lospec.com/palette-list/endesga-32.json");
    }

    TEST(Lospec, ParsesTheRecordedAnswer) {
        const auto result = parseLospecResponse("endesga-32", 200, lospecFixture("lospec-endesga-32.json"), "2026-11-04");
        ASSERT_TRUE(std::holds_alternative<px::Palette>(result)) << lospecError(result).message;
        const px::Palette& palette = std::get<px::Palette>(result);
        EXPECT_EQ(palette.name, "Endesga 32");
        EXPECT_EQ(palette.author, "ENDESGA");
        ASSERT_EQ(palette.count(), 32);
        EXPECT_EQ(px::toHex(palette.colors[0]), "#be4a2f");
        EXPECT_EQ(px::toHex(palette.colors[31]), "#c28569");
        EXPECT_EQ(palette.source.kind, "lospec");
        EXPECT_EQ(palette.source.id, "endesga-32");
        EXPECT_EQ(palette.source.url, "https://lospec.com/palette-list/endesga-32");
        EXPECT_EQ(palette.source.fetched, "2026-11-04");
        // it is a valid palette as it stands
        std::vector<px::Diagnostic> diagnostics;
        EXPECT_TRUE(px::PaletteCodec::validate(palette, diagnostics));
    }

    TEST(Lospec, UppercaseHexAndNoAuthor) {
        const auto result = parseLospecResponse("pico-8", 200, R"({"name":"PICO-8","author":"","colors":["000000","1D2B53","FF004D"]})", "2026-11-04");
        ASSERT_TRUE(std::holds_alternative<px::Palette>(result));
        const px::Palette& palette = std::get<px::Palette>(result);
        EXPECT_EQ(px::toHex(palette.colors[1]), "#1d2b53");
        EXPECT_EQ(palette.author, "");
        // no name: the slug
        const auto unnamed = parseLospecResponse("pico-8", 200, R"({"colors":["000000"]})", "2026-11-04");
        ASSERT_TRUE(std::holds_alternative<px::Palette>(unnamed));
        EXPECT_EQ(std::get<px::Palette>(unnamed).name, "pico-8");
    }

    TEST(Lospec, UnknownSlugIsNotFound) {
        // what Lospec answers for a slug it does not know: HTTP 404 and this body
        const std::string body = lospecFixture("lospec-not-found.json");
        const PaletteError notFound = lospecError(parseLospecResponse("no-such-palette-xyz", 404, body, "2026-11-04"));
        EXPECT_EQ(notFound.kind, "lospec_not_found");
        EXPECT_EQ(notFound.message, "No Lospec palette called 'no-such-palette-xyz'.");
        EXPECT_EQ(notFound.fix, "Copy the slug from the palette's address on lospec.com.");
        // the same body with a 200 is still not found
        EXPECT_EQ(lospecError(parseLospecResponse("no-such-palette-xyz", 200, body, "2026-11-04")).kind, "lospec_not_found");
        // a 404 without a body too (the mirror's missing file)
        EXPECT_EQ(lospecError(parseLospecResponse("no-such-palette-xyz", 404, "", "2026-11-04")).kind, "lospec_not_found");
    }

    TEST(Lospec, UnexpectedAnswersAreBadResponses) {
        const char* const bodies[] = {"<html><body>Lospec</body></html>", "{\"name\": \"x\", \"colors\": [", "[\"be4a2f\"]",
                                      "{\"name\": \"x\"}", "{\"name\": \"x\", \"colors\": []}", "{\"name\": \"x\", \"colors\": [\"nothex\"]}",
                                      "{\"name\": \"x\", \"colors\": [12]}"};
        for (const char* body : bodies) {
            const PaletteError error = lospecError(parseLospecResponse("endesga-32", 200, body, "2026-11-04"));
            EXPECT_EQ(error.kind, "bad_response") << body;
            EXPECT_NE(error.fix.find("https://lospec.com/palette-list/endesga-32"), std::string::npos) << error.fix;
            EXPECT_NE(error.fix.find(".hex"), std::string::npos) << error.fix;
        }
        EXPECT_EQ(lospecError(parseLospecResponse("endesga-32", 503, "Service Unavailable", "2026-11-04")).kind, "bad_response");
    }

    TEST(Lospec, TodayIsAnIsoDate) {
        const std::string today = todayIsoDate();
        ASSERT_EQ(today.size(), 10u);
        EXPECT_EQ(today[4], '-');
        EXPECT_EQ(today[7], '-');
        const int year = std::stoi(today.substr(0, 4)), month = std::stoi(today.substr(5, 2)), day = std::stoi(today.substr(8, 2));
        EXPECT_GE(year, 2026);
        EXPECT_TRUE(month >= 1 && month <= 12);
        EXPECT_TRUE(day >= 1 && day <= 31);
    }

}
