// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The codec against Aseprite 1.3.18 itself (docs/specs/S44-aseprite-import.md AC2, AC3, R8): the
// s44-corpus files tools/pixelart/make_golden_corpus.py authored with it (group opacity and blend,
// layer x cel opacity, nested composed groups, tilemap flips, user data, indexed + background,
// tag directions x repeats, slice keys), their frames as Aseprite exported them (`--save-as`,
// groups flattened as its CLI does; `_composed_` as its renderer draws them, Image:drawSprite),
// and every fixture's `--data` JSON next to what `describe` says.

#include "AsepriteTestSupport.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::aseprite;
using namespace jasperengine::editor::pixelart::testing;
namespace fs = std::filesystem;

namespace {

    fs::path parityCorpus() { return asepriteFixtures() / "s44-corpus"; }

    nlohmann::json readJsonFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return nlohmann::json::parse(in, nullptr, false);
    }

    std::string twoDigits(int n) {
        return (n < 10 ? "0" : "") + std::to_string(n);
    }

    // the Aseprite --data JSON beside an .aseprite file: <name>.data.json, or <name>.json in the corpus
    fs::path dataJsonOf(const fs::path& file) {
        fs::path data = file;
        data.replace_extension(".data.json");
        if (fs::exists(data)) return data;
        data = file;
        data.replace_extension(".json");
        return fs::exists(data) ? data : fs::path();
    }

    nlohmann::json rectJson(const nlohmann::json& r) { return {r.value("x", 0), r.value("y", 0), r.value("w", 0), r.value("h", 0)}; }

}

// AC3 / R8: every frame of the corpus as Aseprite exports it (groups flattened, as its CLI does) and,
// for the composed-group files, as its renderer composes them
TEST(AsepriteParityTest, CorpusFramesEqualAsepritesExports) {
    ASSERT_TRUE(fs::is_directory(parityCorpus())) << "the corpus: tools/pixelart/make_golden_corpus.py corpus";
    int compared = 0, composed = 0;
    for (const auto& entry : fs::directory_iterator(parityCorpus())) {
        if (entry.path().extension() != ".aseprite") continue;
        const std::string stem = entry.path().stem().string();
        SCOPED_TRACE(stem);
        const Document doc = loadDocument(entry.path());
        for (int f = 0; f < doc.frameCount(); f++) {
            const fs::path flat = parityCorpus() / (stem + "_" + twoDigits(f + 1) + ".png");
            if (fs::exists(flat)) {
                RenderOptions options;
                options.groups = GroupMode::Flatten;
                EXPECT_TRUE(samePicture(renderFrame(doc, f, options), loadPng(flat))) << "frame " << f << " (flattened)";
                compared++;
            }
            const fs::path composedPng = parityCorpus() / (stem + "_composed_" + twoDigits(f + 1) + ".png");
            if (fs::exists(composedPng)) {
                RenderOptions options;
                options.groups = GroupMode::Compose;
                EXPECT_TRUE(samePicture(renderFrame(doc, f, options), loadPng(composedPng))) << "frame " << f << " (composed groups)";
                composed++;
            }
        }
    }
    EXPECT_GE(compared, 20);
    EXPECT_GE(composed, 4);
}

// R8: indexed sprites with a special transparent index (Aseprite's bg-index-3, layers "fg" and
// "bg") and with a background layer (the corpus's indexed_background, whose background pixels are
// opaque whatever their index): Aseprite's --save-as of each
TEST(AsepriteParityTest, IndexedTransparentIndexAndBackground) {
    const Document indexed = loadDocument(asepriteFixtures() / "aseprite-tests" / "bg-index-3.aseprite");
    EXPECT_TRUE(samePicture(renderFrame(indexed, 0), loadPng(asepriteFixtures() / "aseprite-tests" / "bg-index-3_01.png")));
    const Document background = loadDocument(parityCorpus() / "indexed_background.aseprite");
    ASSERT_TRUE(background.hasBackground());
    ASSERT_EQ(background.mode, ColorMode::Indexed);
    for (int f = 0; f < background.frameCount(); f++) {
        EXPECT_TRUE(samePicture(renderFrame(background, f), loadPng(parityCorpus() / ("indexed_background_" + twoDigits(f + 1) + ".png")))) << f;
    }
}

// AC2: describe's frames, durations, tags, slices and layers are Aseprite's --data for every
// fixture with one (Aseprite re-sorts tags and reverses slices on load: compared by name)
TEST(AsepriteParityTest, DescribeEqualsAsepritesData) {
    int files = 0;
    for (const char* folder : {"asefile", "aseprite-tests", "s44-corpus"}) {
        for (const auto& entry : fs::directory_iterator(asepriteFixtures() / folder)) {
            if (entry.path().extension() != ".aseprite") continue;
            const fs::path dataPath = dataJsonOf(entry.path());
            if (dataPath.empty()) continue;
            SCOPED_TRACE(entry.path().filename().string());
            const nlohmann::json data = readJsonFile(dataPath);
            ASSERT_FALSE(data.is_discarded());
            const nlohmann::json ours = describe(loadDocument(entry.path()), Detail::Full);
            files++;

            // frames and their durations
            std::vector<int> durations;
            for (const nlohmann::json& f : data["frames"]) durations.push_back(f.value("duration", 0));
            EXPECT_EQ(ours["frame_durations_ms"].get<std::vector<int>>(), durations);

            // tags: name, from, to, direction, repeat (Aseprite writes repeat as a string, absent for 0)
            std::map<std::string, nlohmann::json> theirTags, ourTags;
            for (const nlohmann::json& t : data["meta"].value("frameTags", nlohmann::json::array())) {
                int repeat = 0;
                if (t.contains("repeat")) repeat = t["repeat"].is_string() ? std::stoi(t["repeat"].get<std::string>()) : t["repeat"].get<int>();
                theirTags[t["name"].get<std::string>()] = {t["from"], t["to"], t["direction"], repeat};
            }
            for (const nlohmann::json& t : ours["tags"]) {
                ourTags[t["name"].get<std::string>()] = {t["from"], t["to"], t["direction"], t.value("repeat", 0)};
            }
            EXPECT_EQ(ourTags, theirTags);

            // slices: every key's frame, bounds, centre and pivot
            std::map<std::string, nlohmann::json> theirSlices, ourSlices;
            for (const nlohmann::json& s : data["meta"].value("slices", nlohmann::json::array())) {
                nlohmann::json keys = nlohmann::json::array();
                for (const nlohmann::json& k : s["keys"]) {
                    nlohmann::json key = {{"frame", k["frame"]}, {"bounds", rectJson(k["bounds"])}};
                    if (k.contains("center")) key["center"] = rectJson(k["center"]);
                    if (k.contains("pivot")) key["pivot"] = {k["pivot"]["x"], k["pivot"]["y"]};
                    keys.push_back(key);
                }
                theirSlices[s["name"].get<std::string>()] = keys;
            }
            for (const nlohmann::json& s : ours["slices"]) {
                nlohmann::json keys = nlohmann::json::array();
                for (const nlohmann::json& k : s["keys"]) {
                    nlohmann::json key = {{"frame", k["frame"]}, {"bounds", k["bounds"]}};
                    if (k.contains("center")) key["center"] = k["center"];
                    if (k.contains("pivot")) key["pivot"] = k["pivot"];
                    keys.push_back(key);
                }
                ourSlices[s["name"].get<std::string>()] = keys;
            }
            EXPECT_EQ(ourSlices, theirSlices);

            // layers Aseprite lists (by name, in order): opacity and blend mode
            const nlohmann::json& theirLayers = data["meta"].value("layers", nlohmann::json::array());
            size_t from = 0;
            for (const nlohmann::json& l : theirLayers) {
                const std::string name = l["name"].get<std::string>();
                size_t at = from;
                while (at < ours["layers"].size() && ours["layers"][at]["name"] != name) at++;
                ASSERT_LT(at, ours["layers"].size()) << "Aseprite's layer '" << name << "' in order";
                if (l.contains("opacity")) {
                    EXPECT_EQ(ours["layers"][at]["opacity"], l["opacity"]) << name;
                }
                if (l.contains("blendMode")) {
                    // Aseprite names the four HSL modes hsl_hue... in --data
                    std::string theirs = l["blendMode"].get<std::string>();
                    if (theirs.rfind("hsl_", 0) == 0) theirs = theirs.substr(4);
                    EXPECT_EQ(ours["layers"][at]["blend"], theirs) << name;
                }
                from = at + 1;
            }
        }
    }
    EXPECT_GE(files, 50);
}
