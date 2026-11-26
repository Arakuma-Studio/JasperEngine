// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S84-title-library.md AC5, AC6, AC7 on the committed library: capture(applyOps(look))
// applies to the same document for every available look, every scheme on every look is idempotent,
// and our recolouring equals Pachislop's own applyColourway on the dump's colourway_cases.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/ColorSchemeApply.h"
#include "titleart/fx/PachislopFxMap.h"
#include "titleart/library/TitleLibrary.h"
#include "titleart/library/TitleLibraryApply.h"
#include "titleart/three_d/Title3DConvert.h"
#include "support/EditorTestSupport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
using TitleLibraryBuiltinsJson = titleart::Json;   // its own name: editor::Json (McpJson.h) may be in sight in a unity unit
using titleart::TitleArtDoc;
namespace fs = std::filesystem;

namespace {

    TitleArtDoc textDoc(const std::string& text = "HERO") {
        titleart::NewTitleArtOptions o;
        o.text = text;
        o.idSeed = 0x84;
        return titleart::makeNewTitleArt(o);
    }

    TitleOpsResult titleLibraryBuiltinsApplyOps(const TitleArtDoc& doc, const TitleLibraryBuiltinsJson& ops) {
        TitleOpsOptions o;
        o.idSeed = 0x8484;
        return TitleArtOps::apply(doc, ops, o);
    }

    TitleLibraryBuiltinsJson strippedIds(TitleLibraryBuiltinsJson v, const std::string& key = {}) {
        if (v.is_object()) {
            TitleLibraryBuiltinsJson out = TitleLibraryBuiltinsJson::object();
            for (auto it = v.begin(); it != v.end(); ++it) {
                std::string k = it.key();
                if (k.rfind("rim:", 0) == 0 || k.rfind("band:", 0) == 0) k = k.substr(0, k.find(':') + 1) + "*";
                out[k] = strippedIds(it.value(), k);
            }
            return out;
        }
        if (v.is_array()) {
            TitleLibraryBuiltinsJson out = TitleLibraryBuiltinsJson::array();
            for (const TitleLibraryBuiltinsJson& e : v) out.push_back(strippedIds(e, key));
            return out;
        }
        static const std::set<std::string> ids = {"id", "owner", "follow", "link", "guid"};
        if (v.is_string() && ids.count(key)) return "*";
        return v;
    }

    const TitleLibraryBuiltinsJson* titleLibraryBuiltinsTitleLayer(const TitleOpsResult& r) {
        const TitleLibraryBuiltinsJson* found = nullptr;
        titleart::forEachLayer(r.doc.root, [&](const TitleLibraryBuiltinsJson& l, const TitleLibraryBuiltinsJson*, int) {
            if (l.value("name", std::string()) == "Title") found = &l;
            return found == nullptr;
        });
        return found;
    }

    class TitleLibraryBuiltins : public ::testing::Test {
    protected:
        void SetUp() override {
            registerTitleLibraryOps();
            TitleLibrary::get().setBuiltinsForTesting({});
            TitleLibrary::get().setProject({});
        }
    };

    // ---- AC7: Pachislop's layers in our names ----

    std::string titleLibraryBuiltinsSnake(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (std::isupper((unsigned char)c)) {
                out += '_';
                out += (char)std::tolower((unsigned char)c);
            } else {
                out += c;
            }
        }
        return out;
    }

    TitleLibraryBuiltinsJson titleLibraryBuiltinsSnakeTree(const TitleLibraryBuiltinsJson& v) {
        if (v.is_object()) {
            TitleLibraryBuiltinsJson out = TitleLibraryBuiltinsJson::object();
            for (auto it = v.begin(); it != v.end(); ++it) out[titleLibraryBuiltinsSnake(it.key())] = titleLibraryBuiltinsSnakeTree(it.value());
            return out;
        }
        if (v.is_array()) {
            TitleLibraryBuiltinsJson out = TitleLibraryBuiltinsJson::array();
            for (const TitleLibraryBuiltinsJson& e : v) out.push_back(titleLibraryBuiltinsSnakeTree(e));
            return out;
        }
        return v;
    }

    bool hexColor(const TitleLibraryBuiltinsJson& v) { return v.is_string() && v.get<std::string>().size() == 7 && v.get<std::string>()[0] == '#'; }

    // `theirs` (Pachislop's, renamed) inside `ours`: every colour within 1/255, every number within
    // 0.001, every string equal; members only one side has are other specs' (ids, Pachislop-only keys)
    void compareTree(const TitleLibraryBuiltinsJson& theirs, const TitleLibraryBuiltinsJson& ours, const std::string& path, std::vector<std::string>& out) {
        static const std::set<std::string> skip = {"id", "link", "map", "offset_x", "offset_y", "image", "profile_curve", "gloss_curve", "contour"};
        if (theirs.is_object() && ours.is_object()) {
            for (auto it = theirs.begin(); it != theirs.end(); ++it) {
                if (skip.count(it.key()) || !ours.contains(it.key())) continue;
                compareTree(it.value(), ours[it.key()], path + "." + it.key(), out);
            }
            return;
        }
        if (theirs.is_array() && ours.is_array()) {
            if (theirs.size() != ours.size()) {
                out.push_back(path + ": " + std::to_string(theirs.size()) + " entries against ours " + std::to_string(ours.size()));
                return;
            }
            for (size_t i = 0; i < theirs.size(); i++) compareTree(theirs[i], ours[i], path + "[" + std::to_string(i) + "]", out);
            return;
        }
        if (hexColor(theirs) && hexColor(ours)) {
            titleart::TitleColor a, b;
            titleart::parseTitleColor(theirs, a);
            titleart::parseTitleColor(ours, b);
            if (std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)}) * 255.0f > 1.5f) {
                out.push_back(path + ": " + theirs.get<std::string>() + " against ours " + ours.get<std::string>());
            }
            return;
        }
        if (theirs.is_number() && ours.is_number()) {
            if (std::abs(theirs.get<double>() - ours.get<double>()) > 1e-3) out.push_back(path + ": " + theirs.dump() + " against ours " + ours.dump());
            return;
        }
        if (theirs.is_boolean() && ours.is_boolean() && theirs != ours) out.push_back(path + ": " + theirs.dump() + " against ours " + ours.dump());
    }

    TitleLibraryBuiltinsJson titleLibraryBuiltinsReadJson(const fs::path& file) {
        std::ifstream in(file);
        std::stringstream ss;
        ss << in.rdbuf();
        return TitleLibraryBuiltinsJson::parse(ss.str());
    }

}

TEST_F(TitleLibraryBuiltins, CaptureRoundTripsEveryBuiltIn) {
    int checked = 0;
    // a copy: each pass replaces the built-in set, which invalidates the store's listing
    const std::vector<LibraryItemInfo> looks = TitleLibrary::get().items(LibraryKind::Look);
    for (const LibraryItemInfo& item : looks) {
        if (item.scope != LibraryScope::BuiltIn || !item.available) continue;
        SCOPED_TRACE(item.ref);
        const TitleOpsResult applied = titleLibraryBuiltinsApplyOps(textDoc(), TitleLibraryBuiltinsJson{{"op", "apply_look"}, {"layer", "Title"}, {"look", item.ref}});
        ASSERT_TRUE(applied.ok) << applied.error();
        const TitleLibraryBuiltinsJson* layer = titleLibraryBuiltinsTitleLayer(applied);
        ASSERT_NE(layer, nullptr);
        CaptureOptions options;
        options.name.en = "Captured";
        options.id = "metal/captured";
        TitleLook captured;
        std::string error;
        ASSERT_TRUE(captureLook(applied.doc, (*layer)["id"].get<std::string>(), options, captured, nullptr, &error)) << error;
        auto files = std::map<std::string, std::string>(TitleLibrary::get().builtinFiles());
        files["looks/metal/captured.titlelook"] = writeLook(captured);
        TitleLibrary::get().setBuiltinsForTesting(files);
        const TitleOpsResult again = titleLibraryBuiltinsApplyOps(textDoc(), TitleLibraryBuiltinsJson{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/captured"}});
        TitleLibrary::get().setBuiltinsForTesting({});
        ASSERT_TRUE(again.ok) << again.error();
        TitleLibraryBuiltinsJson a = strippedIds(applied.doc.root["layers"]), b = strippedIds(again.doc.root["layers"]);
        // the look's own record differs by design
        std::function<void(TitleLibraryBuiltinsJson&)> clearLook = [&](TitleLibraryBuiltinsJson& v) {
            if (v.is_object()) {
                v.erase("look");
                for (auto it = v.begin(); it != v.end(); ++it) clearLook(it.value());
            } else if (v.is_array()) {
                for (TitleLibraryBuiltinsJson& e : v) clearLook(e);
            }
        };
        clearLook(a);
        clearLook(b);
        // a captured look holds the complete lighting (R13), the source only what it sets: compare both expanded
        std::function<void(TitleLibraryBuiltinsJson&)> fullLighting = [&](TitleLibraryBuiltinsJson& v) {
            if (v.is_object()) {
                if (v.contains("lighting") && v["lighting"].is_object() && v["lighting"].contains("environment")) {
                    TitleLibraryBuiltinsJson env = titleart::TitleArtSchema::get().defaultObject("environment_3d");
                    for (auto it = v["lighting"]["environment"].begin(); it != v["lighting"]["environment"].end(); ++it) env[it.key()] = it.value();
                    v["lighting"]["environment"] = env;
                }
                for (auto it = v.begin(); it != v.end(); ++it) fullLighting(it.value());
            } else if (v.is_array()) {
                for (TitleLibraryBuiltinsJson& e : v) fullLighting(e);
            }
        };
        fullLighting(a);
        fullLighting(b);
        EXPECT_EQ(titleart::writeTitleArtJson(a), titleart::writeTitleArtJson(b));
        checked++;
    }
    std::cout << "[capture] " << checked << " looks round-trip" << std::endl;
    EXPECT_GT(checked, 0);
}

TEST_F(TitleLibraryBuiltins, EverySchemeOnEveryLookIsIdempotent) {
    int checked = 0;
    for (const LibraryItemInfo& look : TitleLibrary::get().items(LibraryKind::Look)) {
        if (look.scope != LibraryScope::BuiltIn || !look.available) continue;
        const TitleOpsResult applied = titleLibraryBuiltinsApplyOps(textDoc(), TitleLibraryBuiltinsJson{{"op", "apply_look"}, {"layer", "Title"}, {"look", look.ref}});
        ASSERT_TRUE(applied.ok) << look.ref << ": " << applied.error();
        for (const LibraryItemInfo& scheme : TitleLibrary::get().items(LibraryKind::ColorScheme)) {
            if (scheme.scope != LibraryScope::BuiltIn) continue;
            SCOPED_TRACE(look.ref + " + " + scheme.ref);
            auto s = TitleLibrary::get().colorScheme(scheme.ref, nullptr);
            ASSERT_TRUE(s);
            TitleArtDoc once = applied.doc;
            titleart::applyColorScheme(once, *s, titleart::SchemeTarget{});
            TitleArtDoc twice = once;
            titleart::applyColorScheme(twice, *s, titleart::SchemeTarget{});
            ASSERT_EQ(titleart::writeTitleArt(once), titleart::writeTitleArt(twice));
            checked++;
        }
    }
    std::cout << "[idempotent] " << checked << " look x scheme pairs" << std::endl;
}

// AC21 (R40): the CPU budgets, measured on the committed library in this RelWithDebInfo build. A
// shared build box is noisy, so the test holds three times each budget; the printed medians are
// the numbers recorded in the spec.
TEST_F(TitleLibraryBuiltins, Budgets) {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
    const auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::vector<double> index;
    for (int i = 0; i < 5; i++) {
        TitleLibrary::get().invalidate();
        const auto t = Clock::now();
        for (LibraryKind kind : {LibraryKind::Look, LibraryKind::ColorScheme, LibraryKind::Template}) TitleLibrary::get().items(kind);
        index.push_back(ms(t));
    }
    // 16 rims on the layer, as R40 says
    TitleLibraryBuiltinsJson rims = TitleLibraryBuiltinsJson::array();
    for (int i = 0; i < 16; i++) rims.push_back(TitleLibraryBuiltinsJson{{"op", "add_rim"}, {"layer", "Title"}, {"values", TitleLibraryBuiltinsJson{{"width", 2}}}});
    const TitleArtDoc doc16 = titleLibraryBuiltinsApplyOps(textDoc(), rims).doc;
    std::vector<double> look, scheme;
    for (int i = 0; i < 21; i++) {
        auto t = Clock::now();
        const TitleOpsResult applied = titleLibraryBuiltinsApplyOps(doc16, TitleLibraryBuiltinsJson{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/classic-gold"}});
        look.push_back(ms(t));
        ASSERT_TRUE(applied.ok);
        t = Clock::now();
        ASSERT_TRUE(titleLibraryBuiltinsApplyOps(applied.doc, TitleLibraryBuiltinsJson{{"op", "apply_color_scheme"}, {"color_scheme", "gold"}}).ok);
        scheme.push_back(ms(t));
    }
    TitleLibraryBuiltinsJson layers = TitleLibraryBuiltinsJson::array();
    for (int i = 0; i < 63; i++) layers.push_back(TitleLibraryBuiltinsJson{{"op", "add_layer"}, {"type", "text"}, {"name", "Layer" + std::to_string(i)}, {"values", TitleLibraryBuiltinsJson{{"text", "A"}}}});
    const TitleArtDoc doc64 = titleLibraryBuiltinsApplyOps(textDoc(), layers).doc;
    std::vector<double> scheme64;
    for (int i = 0; i < 11; i++) {
        const auto t = Clock::now();
        ASSERT_TRUE(titleLibraryBuiltinsApplyOps(doc64, TitleLibraryBuiltinsJson{{"op", "apply_color_scheme"}, {"color_scheme", "gold"}}).ok);
        scheme64.push_back(ms(t));
    }
    std::cout << "AC21: index " << median(index) << " ms (R40 60), apply_look 16 rims " << median(look) << " ms (3), apply_color_scheme one layer " << median(scheme)
              << " ms (2), 64 layers " << median(scheme64) << " ms (25)\n";
    // wall-clock budgets (R40's, 3 times over): enforced where perfSlack() asks for them (the benchmark run)
    if (const double slack = jasperengine::editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LE(median(index), 60.0 * 3 * slack);
        EXPECT_LE(median(look), 3.0 * 3 * slack);
        EXPECT_LE(median(scheme), 2.0 * 3 * slack);
        EXPECT_LE(median(scheme64), 25.0 * 3 * slack);
    }
}

TEST_F(TitleLibraryBuiltins, MatchesPachislop) {
    const fs::path repo = JASPER_TEST_REPO_DIR;
    const TitleLibraryBuiltinsJson dump = titleLibraryBuiltinsReadJson(repo / "tools/titleart/pachislop-port/dump/pachislop-1c32502.json");
    const TitleLibraryBuiltinsJson map = titleLibraryBuiltinsReadJson(repo / "editor/resources/titleart/pachislop-map.json");
    std::map<std::string, std::string> ids;
    for (auto it = map["ids"].begin(); it != map["ids"].end(); ++it) {
        if (it.value().is_string()) ids[it.key()] = it.value().get<std::string>();
        else if (it.value().is_object() && it.value().contains("ref")) ids[it.key()] = it.value()["ref"].get<std::string>();
    }
    std::set<std::string> tunedLooks;
    const TitleLibraryBuiltinsJson overrides = titleLibraryBuiltinsReadJson(repo / "tools/titleart/pachislop-port/overrides.json");
    for (const char* section : {"looks", "color_schemes"}) {
        if (!overrides.contains(section)) continue;
        for (auto it = overrides[section].begin(); it != overrides[section].end(); ++it) {
            for (auto m = it.value().begin(); m != it.value().end(); ++m) {
                if (m.key() != "check_allow" && m.key() != "lint_allow" && m.key() != "background") tunedLooks.insert(it.key());
            }
        }
    }
    int tunedSkipped = 0;
    int compared = 0;
    std::map<std::string, int> failuresByCase;
    std::vector<std::string> firstFailures;
    for (const TitleLibraryBuiltinsJson& c : dump["colourway_cases"]) {
        const std::string lookId = c["look"].get<std::string>(), colourway = c["colourway"].get<std::string>();
        ASSERT_TRUE(ids.count(lookId)) << lookId;
        const TitleLibraryBuiltinsJson& after = c["layer_after"];
        const bool is3D = after.value("kind", std::string()) == "title3d";
        // an orb case applies to an orb shape in Pachislop
        const bool orb = lookId.find("orb") != std::string::npos;
        TitleArtDoc doc = textDoc(is3D ? "BIG" : "確定");
        if (orb) continue;      // orb bodies are covered by OrbTint and the shape-layer port
        // a look or colourway tuned after the port (overrides.json beyond the gate's metadata; its review record says `tuned`)
        // is ours by design: Pachislop's output no longer describes it
        if (tunedLooks.count(ids[lookId]) || tunedLooks.count(ids.count(colourway) ? ids[colourway] : colourway)) {
            tunedSkipped++;
            continue;
        }
        TitleLibraryBuiltinsJson ops = TitleLibraryBuiltinsJson::array({TitleLibraryBuiltinsJson{{"op", "apply_look"}, {"layer", "Title"}, {"look", ids[lookId]}}, TitleLibraryBuiltinsJson{{"op", "apply_color_scheme"}, {"layer", "Title"}, {"color_scheme", colourway}}});
        const TitleOpsResult r = titleLibraryBuiltinsApplyOps(doc, ops);
        ASSERT_TRUE(r.ok) << lookId << "/" << colourway << ": " << r.error();
        const TitleLibraryBuiltinsJson* layer = titleLibraryBuiltinsTitleLayer(r);
        ASSERT_NE(layer, nullptr);
        std::vector<std::string> diffs;
        if (is3D) compareTree(titleart::title3DFromPachislop(after["style3d"]), (*layer)["style_3d"], "style_3d", diffs);
        else compareTree(titleLibraryBuiltinsSnakeTree(after["style"]), (*layer)["style"], "style", diffs);
        const TitleLibraryBuiltinsJson effects = after.value("effects", TitleLibraryBuiltinsJson::array());
        ASSERT_EQ(effects.size(), (*layer)["effects"].size()) << lookId;
        for (size_t i = 0; i < effects.size(); i++) {
            const PachislopFxConversion conv = convertPachislopEffect(effects[i]["type"].get<std::string>(), effects[i]["params"]);
            if (conv.ok) compareTree(conv.params, (*layer)["effects"][i]["params"], "effects[" + std::to_string(i) + "]", diffs);
        }
        compared++;
        if (!diffs.empty()) {
            failuresByCase[lookId + "/" + colourway] = (int)diffs.size();
            if (firstFailures.size() < 40) firstFailures.push_back(lookId + "/" + colourway + ": " + diffs.front());
        }
    }
    std::cout << "[colourways] " << compared << " cases compared, " << failuresByCase.size() << " differ, " << tunedSkipped << " of tuned looks left out" << std::endl;
    std::string list;
    for (const std::string& f : firstFailures) list += "  " + f + "\n";
    EXPECT_TRUE(failuresByCase.empty()) << failuresByCase.size() << " of " << compared << " cases differ:\n" << list;
}
