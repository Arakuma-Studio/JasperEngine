// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The localisation budgets (docs/specs/game-ui-localization.md R24, R25, AC10), desktop column:
// a 40-byte plain message 0.4 us with no allocation (getView), a message with two arguments and a
// plural 2 us, a language switch over a 5,000-entry table re-translating 300 keys 6 ms, and the
// first lookup of a namespace (the cooked load of 5,000 entries) 2 ms. Printed as [bench] lines;
// enforced with perfSlack() (JASPER_TEST_PERF_SLACK) on the benchmark server.

#include <gtest/gtest.h>

#include "Engine.h"
#include "localization/Localization.h"
#include "localization/StringTableRuntime.h"
#include "util/TitleArtLanguage.h"

#include "TempDir.h"
#include "support/AllocationCounter.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace LocalizationBenchTest_cpp {

using namespace jasperengine;
using namespace jasperengine::l10n;

namespace {

    constexpr int kEntries = 5000;

    std::vector<StringTable::Entry> entries(const std::string& language) {
        std::vector<StringTable::Entry> out;
        for (int k = 0; k < kEntries; k++) {
            // 40 bytes of plain text, every tenth with two arguments and a plural
            std::string text = k % 10 == 0 ? "{name} has {n, plural, one {# coin} other {# coins}} (" + language + ")"
                                           : language + " message number " + std::to_string(100000 + k) + " here....";
            text.resize(k % 10 == 0 ? text.size() : 40, '.');
            out.push_back({"key." + std::to_string(k), text, false});
        }
        return out;
    }

    class LocalizationBench : public ::testing::Test {
    protected:
        void SetUp() override {
            TitleArtLanguage::resetForTesting();
            Localization::resetForTesting();
            Engine::setDevelopmentBuild(false);
            LocalizationManifest manifest;
            manifest.defaultLanguage = "en";
            manifest.rememberLanguage = false;
            manifest.languages = {"en", "de"};
            LocalizationNamespace ns{"ui", "en", {}};
            for (const char* language : {"en", "de"}) {
                std::string cooked;
                ASSERT_TRUE(StringTable::cook(language, entries(language), cooked, nullptr));
                ns.files[language] = dir.write(std::string("ui.") + language + ".strbin", cooked).generic_string();
            }
            manifest.namespaces.push_back(ns);
            Localization::setManifest(manifest);
        }

        void TearDown() override {
            Localization::resetForTesting();
            TitleArtLanguage::resetForTesting();
        }

        static double microsecondsSince(std::chrono::steady_clock::time_point start, int count) {
            return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / count;
        }

        test::TempDir dir;
    };

}

TEST_F(LocalizationBench, Lookup) {
    ASSERT_EQ(Localization::getView("ui.key.1").size(), 40u);     // loads the namespace
    constexpr int rounds = 200000;
    size_t sink = 0;
    // the calling thread's: the logger's drain thread may allocate meanwhile
    testsupport::ThreadAllocationCount allocations;
    auto start = std::chrono::steady_clock::now();
    for (int k = 0; k < rounds; k++) sink += Localization::getView(k % 2 ? "ui.key.1" : "ui.key.3").size();
    const double plain = microsecondsSince(start, rounds);
    const uint64_t plainAllocations = allocations.count();
    const LocalizationArgs args = {{"name", std::string("Ana")}, {"n", int64_t(3)}};
    start = std::chrono::steady_clock::now();
    for (int k = 0; k < rounds / 10; k++) sink += Localization::get("ui.key.10", args).size();
    const double formatted = microsecondsSince(start, rounds / 10);
    std::printf("[bench] Localization getView 40-byte plain %.3f us (budget 0.4), %llu allocations; get with 2 arguments and a plural %.3f us (budget 2)\n",
                plain, (unsigned long long)plainAllocations, formatted);
    EXPECT_GT(sink, 0u);
    if (testsupport::countsAllocations()) { EXPECT_EQ(plainAllocations, 0u) << "getView allocates nothing"; }
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(plain, 0.4 * testsupport::perfSlack());
        EXPECT_LE(formatted, 2.0 * testsupport::perfSlack());
    }
}

TEST_F(LocalizationBench, Switch) {
    // the first lookup of a namespace after a change: the cooked load of 5,000 entries
    ASSERT_TRUE(Localization::setLanguage("de"));
    auto start = std::chrono::steady_clock::now();
    ASSERT_FALSE(Localization::get("ui.key.1").empty());
    const double load = microsecondsSince(start, 1) / 1000.0;
    // a switch: re-resolve, the listeners, 300 keyed texts translated again (the HTML pass's lookups)
    std::vector<std::string> keys;
    for (int k = 0; k < 300; k++) keys.push_back("ui.key." + std::to_string(k * 7));
    int translated = 0;
    const int listener = Localization::addListener([&](const std::string&) {
        std::string out;
        for (const std::string& key : keys) {
            out.clear();
            translated += Localization::translate(key, {}, out) ? 1 : 0;
        }
    });
    constexpr int switches = 20;
    start = std::chrono::steady_clock::now();
    for (int k = 0; k < switches; k++) { ASSERT_TRUE(Localization::setLanguage(k % 2 ? "de" : "en")); }
    const double perSwitch = microsecondsSince(start, switches) / 1000.0;
    Localization::removeListener(listener);
    std::printf("[bench] Localization first lookup of a namespace (cooked, %d entries) %.3f ms (budget 2); switch with 300 keys %.3f ms (budget 6)\n",
                kEntries, load, perSwitch);
    EXPECT_EQ(translated, 300 * switches);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(load, 2.0 * testsupport::perfSlack());
        EXPECT_LE(perSwitch, 6.0 * testsupport::perfSlack());
    }
}

}  // namespace LocalizationBenchTest_cpp
