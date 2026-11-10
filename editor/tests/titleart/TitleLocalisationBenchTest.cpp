// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The budgets of S87 (docs/specs/S87-title-localisation-interchange.md R48, R49, AC28) that run
// without a GPU, measured in Release-like builds (RelWithDebInfo, the unit-test preset):
// TitleArtLanguage::match <= 2 us, TitleArt.setLanguage with 100 listeners <= 0.3 ms, CSV of 1,000
// rows <= 50 ms each way, XLIFF of 1,000 units <= 80 ms, the PNG encoder at 4,096 x 2,048 8-bit
// <= 250 ms and 16-bit <= 600 ms on the job system's workers. Each prints what it measured.

#include <gtest/gtest.h>

#include "jobs/JobSystem.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/interchange/png/PngBandEncoder.h"
#include "titleart/localisation/TitleLanguageOps.h"
#include "titleart/localisation/TitleStrings.h"
#include "util/TitleArtLanguage.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

    double ms(Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    }

    // a title with `rows` text layers translated into de and ja (1,000 strings rows, R49)
    titleart::TitleArtDoc manyTexts(size_t rows) {
        Json root = Json{{"format", titleart::TITLE_ART_FORMAT}, {"version", 1}, {"source_language", "en"}, {"string_namespace", "bench"},
                         {"languages", Json::array({Json{{"code", "de"}}, Json{{"code", "ja"}}})}};
        Json layers = Json::array();
        for (size_t i = 0; i < rows; i++) {
            char id[9];
            std::snprintf(id, sizeof(id), "%08zx", i + 1);
            layers.push_back(Json{{"id", id}, {"type", "text"}, {"name", "Line " + std::to_string(i)}, {"text", "Line number " + std::to_string(i)},
                                  {"text_key", "line_" + std::to_string(i)},
                                  {"translations", Json{{"de", Json{{"text", "Zeile " + std::to_string(i)}}}, {"ja", Json{{"text", "\xe8\xa1\x8c " + std::to_string(i)}}}}}});
        }
        root["layers"] = layers;
        // the 256-layer document limit is the schema's: build the tree without normalize's cap
        titleart::TitleArtDoc doc;
        doc.root = root;
        return doc;
    }

}

TEST(TitleLocalisationBench, MatchAndGlobal) {
    const std::vector<std::string> available = {"en", "de", "ja", "zh-Hans", "zh-Hant", "pt-BR", "es-419", "fr", "it", "ko"};
    const auto start = Clock::now();
    size_t found = 0;
    for (int i = 0; i < 10000; i++) found += TitleArtLanguage::match(i % 2 ? "zh-HK" : "pt-PT", available).empty() ? 0 : 1;
    const double perMatchUs = ms(start) * 1000.0 / 10000.0;
    std::printf("TitleArtLanguage::match: %.3f us\n", perMatchUs);
    EXPECT_EQ(found, 10000u);
    EXPECT_LE(perMatchUs, 2.0);

    TitleArtLanguage::resetForTesting();
    std::vector<int> ids;
    size_t marked = 0;
    for (int i = 0; i < 100; i++) ids.push_back(TitleArtLanguage::addListener([&marked](const std::string&) { marked++; }));
    const auto set = Clock::now();
    ASSERT_TRUE(TitleArtLanguage::setGlobal("de"));
    const double setMs = ms(set);
    std::printf("TitleArt.setLanguage with 100 listeners: %.4f ms\n", setMs);
    EXPECT_EQ(marked, 100u);
    EXPECT_LE(setMs, 0.3);
    TitleArtLanguage::resetForTesting();
}

TEST(TitleLocalisationBench, StringsFiles) {
    registerTitleLanguageOps();
    registerTitleStringsOps();
    const titleart::TitleArtDoc doc = manyTexts(1000);
    const std::vector<TitleStringsDocument> docs = {{"assets/bench.titleart", &doc}};
    auto start = Clock::now();
    const std::string csv = exportTitleStringsCsv(docs, {});
    const double csvOut = ms(start);
    start = Clock::now();
    TitleStringsFile parsed;
    std::string error;
    ASSERT_TRUE(parseTitleStringsCsv(csv, parsed, error)) << error;
    const double csvIn = ms(start);
    EXPECT_EQ(parsed.rows.size(), 1000u);
    start = Clock::now();
    const auto xliff = exportTitleStringsXliff(docs, {"de"});
    const double xliffOut = ms(start);
    start = Clock::now();
    ASSERT_TRUE(parseTitleStringsXliff(xliff.front().second, parsed, error)) << error;
    const double xliffIn = ms(start);
    EXPECT_EQ(parsed.rows.size(), 1000u);
    std::printf("CSV 1,000 rows: export %.2f ms, parse %.2f ms; XLIFF 1,000 units: export %.2f ms, parse %.2f ms\n", csvOut, csvIn, xliffOut, xliffIn);
    EXPECT_LE(csvOut, 50.0);
    EXPECT_LE(csvIn, 50.0);
    EXPECT_LE(xliffOut, 80.0);
    EXPECT_LE(xliffIn, 80.0);
}

TEST(TitleLocalisationBench, PngEncoder) {
    for (int depth : {8, 16}) {
        texsynth::Image image = texsynth::Image::make(4096, 2048, 4, depth);
        // a title-like picture: half transparent, smooth ramps elsewhere
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                uint8_t* p = image.pixel(x, y);
                const bool ink = ((x / 300) + (y / 200)) % 2 == 0;
                for (int c = 0; c < 4; c++) {
                    const uint32_t v = ink ? (uint32_t)((x * (c + 1) + y) & 255) : 0;
                    if (depth == 16) reinterpret_cast<uint16_t*>(p)[c] = (uint16_t)(v * 257);
                    else p[c] = (uint8_t)v;
                }
            }
        }
        TitlePngOptions options;
        std::vector<uint8_t> png;
        std::string error;
        const auto start = Clock::now();
        ASSERT_TRUE(encodeTitlePng(image, options, png, error)) << error;
        const double took = ms(start);
        std::printf("PNG 4,096 x 2,048 %d-bit: %.1f ms on %d workers (%zu bytes)\n", depth, took, jobs::system().getWorkerCount(), png.size());
        EXPECT_LE(took, depth == 8 ? 250.0 : 600.0);
    }
}
