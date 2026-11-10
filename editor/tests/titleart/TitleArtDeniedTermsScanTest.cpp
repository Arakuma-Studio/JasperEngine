// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The owner's rule (docs/specs/S84-title-library.md R38, Deviation 34): no franchise, brand,
// product, company or creator name of tools/titleart/denied_terms.txt anywhere in the Title Art
// paths of the repository, not only in the shipped library: its data, the Pachislop port's dump and
// map, code, shaders, docs and tests. The matcher is the library's own (DeniedTerms.h). Left out:
// the list itself, its matcher's header, and the test lines that feed the matcher a denied term on
// purpose, which carry the marker below. Red before Deviation 34 (the dump, the map's withheld ids
// and texts, and four comments named terms).

#include "titleart/library/DeniedTerms.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine::editor::titlelibrary;

// (a named namespace, tests included: a unity build merges the test sources)
namespace title_art_denied_terms_scan_test {

    // a test line that feeds the matcher a denied term on purpose ends with this comment
    constexpr const char* FIXTURE_MARKER = "denied-term fixture";

    const fs::path& repo() {
        static const fs::path root = JASPER_TEST_REPO_DIR;
        return root;
    }

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream s;
        s << in.rdbuf();
        out = s.str();
        return true;
    }

    // a binary file (images, fonts, meshes, KTX2) holds a NUL in its first 4 KiB
    bool binary(const std::string& bytes) {
        return bytes.substr(0, 4096).find('\0') != std::string::npos;
    }

    struct Scanned {
        std::string path;   // repository-relative
        bool test = false;  // a test path, where the fixture marker counts
    };

    // the Title Art paths (Deviation 34's list)
    std::vector<Scanned> titleArtFiles() {
        const fs::path& root = repo();
        std::vector<Scanned> out;
        auto walk = [&](const fs::path& dir, bool test, auto keep) {
            if (!fs::is_directory(root / dir)) return;
            for (const fs::directory_entry& e : fs::recursive_directory_iterator(root / dir)) {
                if (!e.is_regular_file()) continue;
                const std::string rel = fs::relative(e.path(), root).generic_string();
                if (keep(rel)) out.push_back({rel, test});
            }
        };
        auto all = [](const std::string&) { return true; };
        for (const char* dir : {"editor/resources/titleart", "editor/titleart", "engine/core/titleart", "engine/shaderlib/titleart",
                                "tools/titleart", "docs/titleart"}) {
            walk(dir, false, all);
        }
        for (const fs::directory_entry& e : fs::directory_iterator(root / "engine")) {
            const std::string name = e.path().filename().string();
            if (e.is_directory() && name.rfind("shaders", 0) == 0) walk(fs::path("engine") / name / "titleart", false, all);
        }
        walk("docs/mcp/guide", false, [](const std::string& rel) { return fs::path(rel).filename().string().rfind("title-art", 0) == 0; });
        walk("editor/tests/titleart", true, all);
        walk("engine/tests/core/titleart", true, all);
        walk("engine/tests/core/golden/titleart", true, all);
        walk("tools/mcp/tests", true, [](const std::string& rel) {
            const std::string name = fs::path(rel).filename().string();
            return name.rfind("test_title", 0) == 0 || rel.find("/golden/title") != std::string::npos;
        });
        static const char* const excluded[] = {"tools/titleart/denied_terms.txt", "editor/titleart/library/DeniedTerms.h"};
        out.erase(std::remove_if(out.begin(), out.end(), [](const Scanned& s) {
            return std::find(std::begin(excluded), std::end(excluded), s.path) != std::end(excluded);
        }), out.end());
        std::sort(out.begin(), out.end(), [](const Scanned& a, const Scanned& b) { return a.path < b.path; });
        return out;
    }

    // "path:line: term" for every line of `text` holding a denied term
    void scan(const Scanned& file, const std::string& text, std::vector<std::string>& hits) {
        std::string term;
        if (!containsDeniedTerm(text, &term)) return;
        std::istringstream lines(text);
        std::string line;
        int number = 0;
        while (std::getline(lines, line)) {
            number++;
            if (!containsDeniedTerm(line, &term)) continue;
            if (file.test && line.find(FIXTURE_MARKER) != std::string::npos) continue;
            hits.push_back(file.path + ":" + std::to_string(number) + ": " + term);
        }
    }

}

using namespace title_art_denied_terms_scan_test;

TEST(TitleArtDeniedTermsScan, NoTitleArtPathHoldsADeniedTerm) {
    std::string list;
    ASSERT_TRUE(readText(repo() / "tools/titleart/denied_terms.txt", list));
    setDeniedTermsText(list);
    ASSERT_GE(deniedTerms().size(), 19u) << "the committed list (§6.7.3 starts it with 19 terms)";

    const std::vector<Scanned> files = titleArtFiles();
    ASSERT_GT(files.size(), 500u) << "the Title Art paths were not found under " << repo();
    std::vector<std::string> hits;
    size_t scanned = 0;
    for (const Scanned& f : files) {
        std::string text;
        ASSERT_TRUE(readText(repo() / f.path, text)) << f.path;
        if (binary(text)) continue;
        scanned++;
        scan(f, text, hits);
    }
    EXPECT_GT(scanned, 500u);
    std::ostringstream listed;
    for (size_t i = 0; i < hits.size() && i < 40; i++) listed << "\n  " << hits[i];
    EXPECT_TRUE(hits.empty()) << hits.size() << " denied term(s) in the Title Art paths (S84 Deviation 34: hash a Pachislop source id or "
                              << "text, rephrase prose):" << listed.str();
}

// the scan's own red control: a denied term in a scanned file is found, a test line with the
// marker is not
TEST(TitleArtDeniedTermsScan, FindsATermAndHonoursTheFixtureMarker) {
    std::string list;
    ASSERT_TRUE(readText(repo() / "tools/titleart/denied_terms.txt", list));
    setDeniedTermsText(list);
    const std::string term = deniedTerms().front();
    std::vector<std::string> hits;
    scan({"engine/core/titleart/x.cpp", false}, "ok\n// the " + term + " look\n", hits);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0], "engine/core/titleart/x.cpp:2: " + term);
    hits.clear();
    scan({"editor/tests/titleart/x.cpp", true}, "EXPECT(\"" + term + "\");  // " + std::string(FIXTURE_MARKER) + "\n", hits);
    EXPECT_TRUE(hits.empty());
    scan({"editor/titleart/x.cpp", false}, "EXPECT(\"" + term + "\");  // " + std::string(FIXTURE_MARKER) + "\n", hits);
    EXPECT_EQ(hits.size(), 1u) << "the marker counts in tests only";
}
