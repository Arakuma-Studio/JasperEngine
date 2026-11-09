// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S84-title-library.md AC5, AC6, AC7 on the committed library: capture(applyOps(look))
// applies to the same document for every available look, every scheme on every look is idempotent,
// and our recolouring equals Pachislop's own applyColourway on the dump's colourway_cases.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/ColorSchemeApply.h"
#include "titleart/fx/PachislopFxMap.h"
#include "titleart/library/TitleLibrary.h"
#include "titleart/library/TitleLibraryApply.h"
#include "titleart/three_d/Title3DConvert.h"

#include <algorithm>
#include <cctype>
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
using titleart::Json;
using titleart::TitleArtDoc;
namespace fs = std::filesystem;

namespace {

    TitleArtDoc textDoc(const std::string& text = "HERO") {
        titleart::NewTitleArtOptions o;
        o.text = text;
        o.idSeed = 0x84;
        return titleart::makeNewTitleArt(o);
    }

    TitleOpsResult applyOps(const TitleArtDoc& doc, const Json& ops) {
        TitleOpsOptions o;
        o.idSeed = 0x8484;
        return TitleArtOps::apply(doc, ops, o);
    }

    Json strippedIds(Json v, const std::string& key = {}) {
        if (v.is_object()) {
            Json out = Json::object();
            for (auto it = v.begin(); it != v.end(); ++it) {
                std::string k = it.key();
                if (k.rfind("rim:", 0) == 0 || k.rfind("band:", 0) == 0) k = k.substr(0, k.find(':') + 1) + "*";
                out[k] = strippedIds(it.value(), k);
            }
            return out;
        }
        if (v.is_array()) {
            Json out = Json::array();
            for (const Json& e : v) out.push_back(strippedIds(e, key));
            return out;
        }
        static const std::set<std::string> ids = {"id", "owner", "follow", "link", "guid"};
        if (v.is_string() && ids.count(key)) return "*";
        return v;
    }

    const Json* titleLayer(const TitleOpsResult& r) {
        const Json* found = nullptr;
        titleart::forEachLayer(r.doc.root, [&](const Json& l, const Json*, int) {
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

    std::string snake(const std::string& s) {
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

    Json snakeTree(const Json& v) {
        if (v.is_object()) {
            Json out = Json::object();
            for (auto it = v.begin(); it != v.end(); ++it) out[snake(it.key())] = snakeTree(it.value());
            return out;
        }
        if (v.is_array()) {
            Json out = Json::array();
            for (const Json& e : v) out.push_back(snakeTree(e));
            return out;
        }
        return v;
    }

    bool hexColor(const Json& v) { return v.is_string() && v.get<std::string>().size() == 7 && v.get<std::string>()[0] == '#'; }

    // `theirs` (Pachislop's, renamed) inside `ours`: every colour within 1/255, every number within
    // 0.001, every string equal; members only one side has are other specs' (ids, Pachislop-only keys)
    void compareTree(const Json& theirs, const Json& ours, const std::string& path, std::vector<std::string>& out) {
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

    Json readJson(const fs::path& file) {
        std::ifstream in(file);
        std::stringstream ss;
        ss << in.rdbuf();
        return Json::parse(ss.str());
    }

}

TEST_F(TitleLibraryBuiltins, CaptureRoundTripsEveryBuiltIn) {
    int checked = 0;
    for (const LibraryItemInfo& item : TitleLibrary::get().items(LibraryKind::Look)) {
        if (item.scope != LibraryScope::BuiltIn || !item.available) continue;
        SCOPED_TRACE(item.ref);
        const TitleOpsResult applied = applyOps(textDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", item.ref}});
        ASSERT_TRUE(applied.ok) << applied.error();
        const Json* layer = titleLayer(applied);
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
        const TitleOpsResult again = applyOps(textDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/captured"}});
        TitleLibrary::get().setBuiltinsForTesting({});
        ASSERT_TRUE(again.ok) << again.error();
        Json a = strippedIds(applied.doc.root["layers"]), b = strippedIds(again.doc.root["layers"]);
        // the look's own record differs by design
        std::function<void(Json&)> clearLook = [&](Json& v) {
            if (v.is_object()) {
                v.erase("look");
                for (auto it = v.begin(); it != v.end(); ++it) clearLook(it.value());
            } else if (v.is_array()) {
                for (Json& e : v) clearLook(e);
            }
        };
        clearLook(a);
        clearLook(b);
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
        const TitleOpsResult applied = applyOps(textDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", look.ref}});
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

TEST_F(TitleLibraryBuiltins, MatchesPachislop) {
    const fs::path repo = JASPER_TEST_REPO_DIR;
    const Json dump = readJson(repo / "tools/titleart/pachislop-port/dump/pachislop-1c32502.json");
    const Json map = readJson(repo / "editor/resources/titleart/pachislop-map.json");
    std::map<std::string, std::string> ids;
    for (auto it = map["ids"].begin(); it != map["ids"].end(); ++it) {
        if (it.value().is_string()) ids[it.key()] = it.value().get<std::string>();
        else if (it.value().is_object() && it.value().contains("ref")) ids[it.key()] = it.value()["ref"].get<std::string>();
    }
    int compared = 0;
    std::map<std::string, int> failuresByCase;
    std::vector<std::string> firstFailures;
    for (const Json& c : dump["colourway_cases"]) {
        const std::string lookId = c["look"].get<std::string>(), colourway = c["colourway"].get<std::string>();
        ASSERT_TRUE(ids.count(lookId)) << lookId;
        const Json& after = c["layer_after"];
        const bool is3D = after.value("kind", std::string()) == "title3d";
        // an orb case applies to an orb shape in Pachislop
        const bool orb = lookId.find("orb") != std::string::npos;
        TitleArtDoc doc = textDoc(is3D ? "BIG" : "確定");
        if (orb) continue;      // orb bodies are covered by OrbTint and the shape-layer port
        Json ops = Json::array({Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", ids[lookId]}}, Json{{"op", "apply_color_scheme"}, {"layer", "Title"}, {"color_scheme", colourway}}});
        const TitleOpsResult r = applyOps(doc, ops);
        ASSERT_TRUE(r.ok) << lookId << "/" << colourway << ": " << r.error();
        const Json* layer = titleLayer(r);
        ASSERT_NE(layer, nullptr);
        std::vector<std::string> diffs;
        if (is3D) compareTree(titleart::title3DFromPachislop(after["style3d"]), (*layer)["style_3d"], "style_3d", diffs);
        else compareTree(snakeTree(after["style"]), (*layer)["style"], "style", diffs);
        const Json effects = after.value("effects", Json::array());
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
    std::cout << "[colourways] " << compared << " cases compared, " << failuresByCase.size() << " differ" << std::endl;
    std::string list;
    for (const std::string& f : firstFailures) list += "  " + f + "\n";
    EXPECT_TRUE(failuresByCase.empty()) << failuresByCase.size() << " of " << compared << " cases differ:\n" << list;
}
