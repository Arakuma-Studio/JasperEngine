// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The parsers of untrusted interchange files (docs/specs/S87-title-localisation-interchange.md R47,
// AC31): truncated and mutated inputs never crash; each one parses or fails with a message. Run
// under SANITIZE=address,undefined it also proves there is no read past an input and no leak. The
// importers (create.from: Pachislop, PSD, SVG; the Photoshop style libraries) run through
// TitleArtOps::createFrom on the same mutations.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/interchange/TitleImport.h"
#include "titleart/localisation/TitleStrings.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // `count` variants of `seed`: truncations at every length step, then random byte flips,
    // insertions and deletions (a fixed seed: the same corpus every run)
    std::vector<std::string> mutations(const std::string& seed, size_t count) {
        std::vector<std::string> out;
        std::mt19937 random(0x5387u);
        const size_t step = std::max<size_t>(1, seed.size() / 40);
        for (size_t n = 0; n < seed.size() && out.size() < count / 4; n += step) out.push_back(seed.substr(0, n));
        while (out.size() < count) {
            std::string s = seed;
            const int edits = 1 + (int)(random() % 8);
            for (int e = 0; e < edits && !s.empty(); e++) {
                const size_t at = random() % s.size();
                switch (random() % 4) {
                    case 0: s[at] = (char)(random() & 255); break;
                    case 1: s.insert(s.begin() + (long)at, (char)(random() & 255)); break;
                    case 2: s.erase(s.begin() + (long)at); break;
                    default: s[at] = "\"<>,;\t\n\r&'"[random() % 11]; break;
                }
            }
            out.push_back(std::move(s));
        }
        return out;
    }

    const char* CSV = "\xEF\xBB\xBF" "file,key,context,max_chars,en,de,ja\r\n"
                      "assets/titles/logo.titleart,logo.headline,\"Main title, \"\"one\"\" line\",12,BIG WIN,GROSSER GEWINN,\xe5\xa4\xa7\r\n"
                      "assets/titles/logo.titleart,logo.sub,,0,\"two\nlines\",\"zwei\nZeilen\",\r\n";

    const char* XLIFF = R"(<?xml version="1.0" encoding="UTF-8"?>
<xliff version="1.2" xmlns="urn:oasis:names:tc:xliff:document:1.2">
  <file original="assets/titles/logo.titleart" source-language="en" target-language="de" datatype="plaintext">
    <body>
      <trans-unit id="logo.headline" resname="logo.headline" maxwidth="12" size-unit="char" xml:space="preserve">
        <source>BIG WIN</source><target state="translated">GROSSER &amp; GEWINN</target>
        <note>Main title</note>
      </trans-unit>
      <group><trans-unit id="logo.sub"><source>x</source><target state="needs-review-translation">y <g id="1">z</g></target></trans-unit></group>
    </body>
  </file>
</xliff>
)";

}

TEST(InterchangeFuzz, CsvNeverCrashes) {
    size_t parsed = 0, refused = 0;
    for (const std::string& input : mutations(CSV, 600)) {
        TitleStringsFile out;
        std::string error;
        if (parseTitleStringsCsv(input, out, error)) {
            parsed++;
        } else {
            refused++;
            EXPECT_FALSE(error.empty()) << "a refusal says why";
        }
    }
    EXPECT_EQ(parsed + refused, 600u);
    EXPECT_GT(refused, 0u) << "some mutations are not CSV a title reads";
}

TEST(InterchangeFuzz, XliffNeverCrashes) {
    size_t parsed = 0, refused = 0;
    for (const std::string& input : mutations(XLIFF, 600)) {
        TitleStringsFile out;
        std::string error;
        if (parseTitleStringsXliff(input, out, error)) {
            parsed++;
        } else {
            refused++;
            EXPECT_FALSE(error.empty());
        }
    }
    EXPECT_EQ(parsed + refused, 600u);
    EXPECT_GT(refused, 0u);
}

// every registered importer of create.from (S87's Pachislop, PSD, SVG), on mutations of a file
// of each kind: an import works or fails with a code and a sentence
TEST(InterchangeFuzz, ImportersNeverCrash) {
    registerTitleInterchange();
    const fs::path dir = fs::temp_directory_path() / "je_title_fuzz";
    fs::create_directories(dir);
    const std::vector<std::pair<std::string, std::string>> seeds = {
        {"logo.pslot.json", R"({"format": "pslot", "version": 11, "project": {"version": 11, "name": "x", "comp": {"width": 800, "height": 400},
            "layers": [{"id": "a", "kind": "title2d", "name": "Title", "source": {"type": "text", "text": "WIN"}, "transform": {"x": 0, "y": 0}}]},
            "fonts": [], "assets": []})"},
        {"badge.svg", R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100"><g id="a"><path d="M10 10 L90 10 L50 90 Z" fill="#f00"/></g><circle cx="50" cy="50" r="10"/></svg>)"},
        {"layers.psd", std::string("8BPS\0\1\0\0\0\0\0\0\0\4\0\0\0\x10\0\0\0\x10\0\x08\0\x03\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 46)},
    };
    size_t runs = 0;
    for (const auto& [name, seed] : seeds) {
        for (const std::string& input : mutations(seed, 300)) {
            const fs::path file = dir / name;
            std::ofstream(file, std::ios::binary) << input;
            TitleCreateRequest request;
            request.sourcePath = file.string();
            request.documentPath = (dir / "out.titleart").string();
            request.projectPath = dir.string();
            request.assetsPath = dir.string();
            TitleCreateResult result;
            std::string code, error;
            if (!TitleArtOps::createFrom(request, result, code, error)) {
                EXPECT_FALSE(code.empty()) << name;
                EXPECT_FALSE(error.empty()) << name;
            }
            runs++;
        }
    }
    EXPECT_EQ(runs, 900u);
    fs::remove_all(dir);
}
