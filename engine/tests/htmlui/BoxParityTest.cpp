// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Taffy's layout fixtures against Chromium (phase-3 spec 5.3): every fixtures/taffy/<suite>/<name>.html
// is loaded through the compat layer at 1024x768 and the border boxes of #test-root and of every
// element under it are compared with Chromium's (golden/browser/taffy/<suite>.boxes.json, written by
// tools/htmlui/render-reference.mjs --fixtures taffy --boxes-only) within 1 px.
//
// excluded.txt: fixtures that are not compared (skipped, with the reason). known-failures.txt: the
// fixtures the fork does not pass yet, with the package that owns each; they are expected to fail
// (reported as KNOWN) and the test fails when one passes, so the package that fixes a fixture removes
// its line. Every run writes <binary>/htmlui-test-output/taffy/results.txt ("<suite>/<name> pass|fail
// <first difference>"), which tools/htmlui/import-taffy-fixtures.py known-failures turns into the list.

#include "FixtureRunner.h"

#include "harness/ImageCompare.h"

#include <gtest/gtest.h>

#include <RmlUi/Core.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* const kSuites[] = {"grid", "flex", "block", "blockflex", "blockgrid", "gridflex", "leaf", "float"};
    constexpr int kTaffyWidth = 1024;
    constexpr int kTaffyHeight = 768;

    struct TaffyReferences {
        std::map<std::string, std::vector<TaffyBox>> fixtures;   // "<suite>/<name>"
        std::vector<std::string> errors;
    };

    const TaffyReferences& taffyReferences(){
        static const TaffyReferences references = []{
            TaffyReferences result;
            for (const char* suite : kSuites){
                std::vector<std::pair<std::string, std::vector<TaffyBox>>> fixtures;
                std::string error;
                if (!loadTaffySuite(browserReferenceDirectory() + "/taffy/" + suite + ".boxes.json", fixtures, &error)){
                    result.errors.push_back(error);
                    continue;
                }
                for (auto& fixture : fixtures)
                    result.fixtures[std::string(suite) + "/" + fixture.first] = std::move(fixture.second);
            }
            return result;
        }();
        return references;
    }

    const FixtureList& taffyExcluded(){
        static const FixtureList list = loadFixtureList(taffyFixturesDirectory() + "/excluded.txt");
        return list;
    }

    const FixtureList& taffyKnownFailures(){
        static const FixtureList list = loadFixtureList(taffyFixturesDirectory() + "/known-failures.txt");
        return list;
    }

    std::string resultsPath(){
        return getOutputDirectory() + "/taffy/results.txt";
    }

    std::string paramName(const std::string& id){
        std::string name;
        for (char c : id)
            name += std::isalnum((unsigned char)c) ? c : '_';
        return name;
    }

}

// ---- the harness itself (no GPU) ------------------------------------------------------------------

TEST(BoxParityHarness, ComparesWithinOnePixel){
    std::vector<TaffyBox> reference = {{"", "div", 10.0f, 20.0f, 400.0f, 300.0f}, {"0", "div", 0.0f, 0.0f, 100.0f, 50.0f},
                                       {"1", "div", 100.0f, 0.0f, 100.0f, 50.0f}};
    EXPECT_TRUE(compareTaffyBoxes(reference, reference).empty());

    // a 1 px offset is within the tolerance; 1.5 px is not, and is reported with its element
    std::vector<TaffyBox> engine = reference;
    engine[1].x += 1.0f;
    engine[2].w -= 1.0f;
    EXPECT_TRUE(compareTaffyBoxes(engine, reference).empty());
    engine[2].w -= 0.5f;
    const std::vector<TaffyDifference> differences = compareTaffyBoxes(engine, reference);
    ASSERT_EQ(differences.size(), 1u);
    EXPECT_EQ(differences[0].path, "1");
    EXPECT_EQ(differences[0].field, 'w');
    EXPECT_NE(differences[0].text().find("#test-root > 1 w: engine 98.500, Chromium 100.000"), std::string::npos) << differences[0].text();

    // structure: a missing element, another tag, an extra element
    std::vector<TaffyBox> other = {reference[0], {"0", "span", 0.0f, 0.0f, 100.0f, 50.0f}, {"2", "div", 0.0f, 0.0f, 1.0f, 1.0f}};
    const std::vector<TaffyDifference> structure = compareTaffyBoxes(other, reference);
    ASSERT_EQ(structure.size(), 3u);
    EXPECT_NE(structure[0].text().find("the engine has a <span>, Chromium a <div>"), std::string::npos) << structure[0].text();
    EXPECT_NE(structure[1].text().find("no such element"), std::string::npos) << structure[1].text();
    EXPECT_NE(structure[2].text().find("extra <div>"), std::string::npos) << structure[2].text();
}

TEST(BoxParityHarness, ReadsFixtureLists){
    const std::string path = getOutputDirectory() + "/taffy/selftest-list.txt";
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    {
        std::ofstream file(path);
        file << "# a comment\n\ngrid/a owner=p3-grid reason=grid-unsupported # grid is a fallback\nflex/b reason=flex-wrap\n"
                "flex/c owner=p3-layout\nflex/b reason=flex-wrap\nblock/d colour=red reason=x\n";
    }
    const FixtureList list = loadFixtureList(path);
    ASSERT_EQ(list.entries.size(), 5u);
    EXPECT_EQ(list.entries[0].id, "grid/a");
    EXPECT_EQ(list.entries[0].owner, "p3-grid");
    EXPECT_EQ(list.entries[0].reason, "grid-unsupported");
    EXPECT_EQ(list.entries[0].note, "grid is a fallback");
    EXPECT_EQ(list.entries[0].line, 3);
    EXPECT_NE(list.find("flex/b"), nullptr);
    EXPECT_EQ(list.find("flex/z"), nullptr);
    // flex/c has no reason, flex/b is listed twice, block/d has an unknown key
    ASSERT_EQ(list.errors.size(), 3u);
    EXPECT_NE(list.errors[0].find("flex/c has no reason"), std::string::npos) << list.errors[0];
    EXPECT_NE(list.errors[1].find("listed twice"), std::string::npos) << list.errors[1];
    EXPECT_NE(list.errors[2].find("'colour=red'"), std::string::npos) << list.errors[2];
    EXPECT_TRUE(loadFixtureList(path + ".missing").entries.empty());
}

// ---- the committed data ----------------------------------------------------------------------------

// Every fixture is excluded or has a reference, every reference has its fixture; the lists name
// existing fixtures, a fixture is either excluded or a known failure, and known failures name their
// owner (a phase-3 package) and reason class.
TEST(BoxParityFixtures, ListsAndReferencesAreConsistent){
    const std::vector<std::string> fixtures = listTaffyFixtures();
    ASSERT_GE(fixtures.size(), 1595u) << "the imported Taffy suites (tools/htmlui/import-taffy-fixtures.py import)";
    const TaffyReferences& references = taffyReferences();
    for (const std::string& error : references.errors)
        ADD_FAILURE() << error;
    const FixtureList& excluded = taffyExcluded();
    const FixtureList& known = taffyKnownFailures();
    for (const std::string& error : excluded.errors)
        ADD_FAILURE() << error;
    for (const std::string& error : known.errors)
        ADD_FAILURE() << error;

    size_t compared = 0;
    for (const std::string& fixture : fixtures){
        const bool isExcluded = excluded.find(fixture) != nullptr;
        const bool hasReference = references.fixtures.count(fixture) > 0;
        if (isExcluded){
            EXPECT_FALSE(hasReference) << fixture << " is excluded but has a reference";
            EXPECT_EQ(known.find(fixture), nullptr) << fixture << " is both excluded and a known failure";
        }else{
            EXPECT_TRUE(hasReference) << fixture << " has no reference: node tools/htmlui/render-reference.mjs --fixtures taffy/" << fixture
                                      << " --boxes-only";
            compared++;
        }
    }
    for (const auto& reference : references.fixtures)
        EXPECT_TRUE(std::binary_search(fixtures.begin(), fixtures.end(), reference.first)) << reference.first << " has a reference but no fixture";
    for (const ListedFixture& entry : excluded.entries)
        EXPECT_TRUE(std::binary_search(fixtures.begin(), fixtures.end(), entry.id)) << "excluded.txt:" << entry.line << ": no fixture " << entry.id;
    for (const ListedFixture& entry : known.entries){
        EXPECT_TRUE(std::binary_search(fixtures.begin(), fixtures.end(), entry.id)) << "known-failures.txt:" << entry.line << ": no fixture " << entry.id;
        EXPECT_TRUE(isPhase3Package(entry.owner)) << "known-failures.txt:" << entry.line << ": owner '" << entry.owner << "' is not a phase-3 package";
    }
    std::cout << "[ TAFFY    ] " << fixtures.size() << " fixtures, " << excluded.entries.size() << " excluded, " << compared << " compared, "
              << known.entries.size() << " known failures\n";
}

// ---- the fixtures ----------------------------------------------------------------------------------

class BoxParity : public ::testing::TestWithParam<std::string> {
public:
    static void SetUpTestSuite(){
        runner = new FixtureRunner(kTaffyWidth, kTaffyHeight);
        std::filesystem::create_directories(std::filesystem::path(resultsPath()).parent_path());
        results.open(resultsPath(), std::ios::trunc);
        counts.clear();
    }

    static void TearDownTestSuite(){
        delete runner;
        runner = nullptr;
        results.close();
        // per suite: passing, known failures, unexpected failures, known failures that pass
        for (const auto& suite : counts)
            std::cout << "[ TAFFY    ] " << suite.first << ": " << suite.second[0] << " pass, " << suite.second[1] << " known failures, "
                      << suite.second[2] << " unexpected failures, " << suite.second[3] << " known failures now passing\n";
    }

protected:
    static FixtureRunner* runner;
    static std::ofstream results;
    static std::map<std::string, std::array<int, 4>> counts;
};

FixtureRunner* BoxParity::runner = nullptr;
std::ofstream BoxParity::results;
std::map<std::string, std::array<int, 4>> BoxParity::counts;

TEST_P(BoxParity, MatchesChromium){
    const std::string fixture = GetParam();
    const std::string suite = fixture.substr(0, fixture.find('/'));
    if (const ListedFixture* excluded = taffyExcluded().find(fixture))
        GTEST_SKIP() << fixture << " is excluded: " << excluded->reason << (excluded->note.empty() ? "" : " (" + excluded->note + ")");
    const auto reference = taffyReferences().fixtures.find(fixture);
    ASSERT_NE(reference, taffyReferences().fixtures.end()) << fixture << " has no reference (BoxParityFixtures.ListsAndReferencesAreConsistent)";
    if (!runner->valid())
        GTEST_SKIP() << "no headless GPU context";

    std::vector<TaffyDifference> differences;
    std::string error;
    std::vector<TaffyBox> engine;
    if (!runner->load("fixtures/taffy/" + fixture + ".html", &error) || !runner->taffyBoxes(engine, &error)){
        TaffyDifference difference;
        difference.detail = error;
        differences.push_back(difference);
    }else{
        differences = compareTaffyBoxes(engine, reference->second);
    }

    const ListedFixture* known = taffyKnownFailures().find(fixture);
    results << fixture << (differences.empty() ? " pass" : " fail") << (differences.empty() ? "" : " " + differences[0].text()) << "\n";
    results.flush();
    std::array<int, 4>& count = counts[suite];
    if (differences.empty()){
        if (known){
            count[3]++;
            ADD_FAILURE() << fixture << " passes now: remove its line from fixtures/taffy/known-failures.txt (owner " << known->owner << ", reason "
                          << known->reason << ")";
        }else{
            count[0]++;
        }
        return;
    }
    std::string list;
    for (const TaffyDifference& difference : differences)
        list += "\n  " + difference.text();
    if (known){
        count[1]++;
        std::cout << "[ KNOWN    ] " << fixture << " (owner " << known->owner << ", " << known->reason << "): " << differences[0].text() << "\n";
        return;
    }
    count[2]++;
    ADD_FAILURE() << fixture << " differs from Chromium (" << differences.size() << " differences):" << list;
}

INSTANTIATE_TEST_SUITE_P(Taffy, BoxParity, ::testing::ValuesIn(listTaffyFixtures()),
                         [](const ::testing::TestParamInfo<std::string>& info){ return paramName(info.param); });
