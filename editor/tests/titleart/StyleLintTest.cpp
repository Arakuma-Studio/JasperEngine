// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S84 AC16 (R35, R45): per style lint rule, a bad twin fires with its measured value, a good twin
// does not, and the finding's fix ops (through TitleArtOps, as edit_title runs them) clear it;
// MatchesPachislop: our findings on the dump's lint_cases (Pachislop 1c32502's layers, by SafEight,
// used with the author's permission) equal lint.ts's, rule ids mapped.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/fx/PachislopFxMap.h"
#include "titleart/library/StyleLint.h"
#include "titleart/three_d/Title3DEditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
using titleart::Json;
using titleart::TitleArtDoc;
namespace fs = std::filesystem;

namespace {

    Json solid(const char* color) {
        return Json{{"fill", {{"kind", "solid"}, {"color", color}}}};
    }

    Json rim(const char* color, double width) {
        return Json{{"width", width}, {"material", solid(color)}};
    }

    // a 2D text layer "Title" at size 200
    Json text2D(Json face, Json rims) {
        return Json{{"id", "00000001"}, {"type", "text"}, {"name", "Title"}, {"text", "WIN"}, {"size", 200},
                    {"style", {{"face", std::move(face)}, {"rims", std::move(rims)}}}};
    }

    TitleArtDoc docWith(const Json& layer) {
        Json root = Json{{"format", titleart::TITLE_ART_FORMAT}, {"version", titleart::TITLE_ART_VERSION}, {"layers", Json::array({layer})}};
        titleart::TitleArtParseResult parsed = titleart::parseTitleArtJson(root);
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return parsed.doc;
    }

    std::vector<StyleLintFinding> findings(const TitleArtDoc& doc, const std::string& rule) {
        std::vector<StyleLintFinding> out;
        for (StyleLintFinding& f : lintDocument(doc)) {
            if (f.rule == rule) out.push_back(std::move(f));
        }
        return out;
    }

    // the document with the finding's fix applied as edit_title applies it
    TitleArtDoc fixedBy(const TitleArtDoc& doc, const StyleLintFinding& f) {
        registerTitle3DEditor();
        EXPECT_TRUE(f.fix.is_object() && f.fix.contains("ops")) << f.rule << " has no fix";
        if (!f.fix.is_object()) return doc;
        const TitleOpsResult r = TitleArtOps::apply(doc, f.fix["ops"], TitleOpsOptions{7});
        EXPECT_TRUE(r.ok) << f.rule << ": " << r.error() << "\n" << f.fix.dump();
        return r.ok ? r.doc : doc;
    }

    // CIE L* of "#rrggbb" (the test's own: L* depends on Y only)
    double lstar(const char* hex) {
        titleart::TitleColor c;
        titleart::parseTitleColor(Json(hex), c);
        auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        const double y = 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
        return 116.0 * (y > 216.0 / 24389.0 ? std::cbrt(y) : ((24389.0 / 27.0) * y + 16.0) / 116.0) - 16.0;
    }

    // what the bad twin, the good twin and the fixed bad twin report for `rule`
    struct Twins {
        std::vector<StyleLintFinding> bad, good, fixed;
    };
    Twins twins(const Json& badLayer, const Json& goodLayer, const std::string& rule) {
        Twins t;
        const TitleArtDoc bad = docWith(badLayer);
        t.bad = findings(bad, rule);
        t.good = findings(docWith(goodLayer), rule);
        if (!t.bad.empty() && !t.bad[0].fix.is_null()) t.fixed = findings(fixedBy(bad, t.bad[0]), rule);
        return t;
    }

}

TEST(StyleLintTest, ContrastFiresOnNeighbouringRingsOfOneLightnessAndItsSeparatorFixesIt) {
    // a yellow face inside a pale yellow rim: 4 L* apart
    const Twins t = twins(text2D(solid("#ffd040"), Json::array({rim("#ffe070", 4), rim("#000000", 4)})),
                          text2D(solid("#ffd040"), Json::array({rim("#1a1008", 4), rim("#ffffff", 4)})), "contrast");
    ASSERT_EQ(t.bad.size(), 1u);
    EXPECT_EQ(t.bad[0].element, "rim#0");
    EXPECT_NEAR(t.bad[0].measured, std::fabs(lstar("#ffe070") - lstar("#ffd040")), 0.05);
    EXPECT_EQ(t.bad[0].threshold, LINT_MIN_DELTA_L);
    EXPECT_EQ(t.bad[0].layer, "00000001");
    ASSERT_TRUE(t.bad[0].fix.is_object());
    const Json& op = t.bad[0].fix["ops"][0];
    EXPECT_EQ(op["op"], "add_rim");
    EXPECT_EQ(op["index"], 0);
    EXPECT_EQ(op["values"]["material.fill.color"], "#0a0604") << "a light pair takes a dark separator";
    EXPECT_EQ(op["values"]["width"], LINT_SEPARATOR_WIDTH);
    EXPECT_TRUE(t.good.empty());
    EXPECT_TRUE(t.fixed.empty()) << "the separator splits the pair";
}

TEST(StyleLintTest, ContrastExemptsTwoDarkRingsSpectraAndBandedMetals) {
    // two near-black rings are one keyline
    EXPECT_TRUE(findings(docWith(text2D(solid("#ffd040"), Json::array({rim("#101010", 3), rim("#000000", 3)}))), "contrast").empty());
    // a spectrum face next to a rim of its mean lightness
    Json rainbow = Json{{"fill", {{"kind", "linear"}, {"gradient", {{"stops", Json::array({Json{{"pos", 0}, {"color", "#ff0000"}}, Json{{"pos", 0.5}, {"color", "#00ff00"}},
                                                                                        Json{{"pos", 1}, {"color", "#0000ff"}}})}}}}}};
    EXPECT_TRUE(findings(docWith(text2D(rainbow, Json::array({rim("#909090", 6)}))), "contrast").empty());
    // two banded metals of one mean lightness separate through their banding
    auto banded = [](const char* mid) {
        return Json{{"fill", {{"kind", "linear"}, {"gradient", {{"stops", Json::array({Json{{"pos", 0}, {"color", "#ffffff"}}, Json{{"pos", 0.5}, {"color", mid}},
                                                                                     Json{{"pos", 1}, {"color", "#ffffff"}}})}}}}}};
    };
    Json metals = text2D(banded("#303030"), Json::array({Json{{"width", 6}, {"material", banded("#383838")}}}));
    EXPECT_TRUE(findings(docWith(metals), "contrast").empty());
    // the same two as flat paints of those mean lightnesses do merge
    EXPECT_EQ(findings(docWith(text2D(solid("#a0a0a0"), Json::array({rim("#a8a8a8", 6)}))), "contrast").size(), 1u);
}

TEST(StyleLintTest, ContrastOnA3DBandInsertsABand) {
    registerTitle3DEditor();
    Json layer = Json{{"id", "00000001"}, {"type", "text"}, {"name", "Title"}, {"text", "WIN"}, {"size", 200}, {"dimension", "3d"},
                      {"style_3d", {{"face", {{"color", "#ffd040"}}},
                                    {"bands", Json::array({Json{{"grow", 3}, {"material", {{"color", "#ffe070"}}}}, Json{{"grow", 3}, {"material", {{"color", "#000000"}}}}})}}}};
    const TitleArtDoc doc = docWith(layer);
    ASSERT_TRUE(doc.layers()[0]["style_3d"].is_object());
    const std::vector<StyleLintFinding> bad = findings(doc, "contrast");
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].element, "band#0");
    EXPECT_EQ(bad[0].fix["ops"][0]["op"], "add_band");
    EXPECT_TRUE(findings(fixedBy(doc, bad[0]), "contrast").empty());
}

TEST(StyleLintTest, HueCountFiresOnThreeHueFamiliesWithoutAFix) {
    const Twins t = twins(text2D(solid("#e01010"), Json::array({rim("#10c020", 4), rim("#2040e0", 4)})),
                          text2D(solid("#e01010"), Json::array({rim("#10c020", 4), rim("#000000", 4)})), "hue_count");
    ASSERT_EQ(t.bad.size(), 1u);
    EXPECT_EQ(t.bad[0].measured, 3.0);
    EXPECT_EQ(t.bad[0].threshold, 3.0);
    EXPECT_TRUE(t.bad[0].fix.is_null()) << "advice only: which ring to change is a choice";
    EXPECT_NE(t.bad[0].message.find("face / rim#0 / rim#1"), std::string::npos) << t.bad[0].message;
    EXPECT_TRUE(t.good.empty());
}

TEST(StyleLintTest, ThinRimsFireAndScaleToEightPercent) {
    const Twins t = twins(text2D(solid("#ffd040"), Json::array({rim("#000000", 1.5), rim("#ffffff", 0.5)})),
                          text2D(solid("#ffd040"), Json::array({rim("#000000", 5), rim("#ffffff", 2)})), "rims_thin");
    ASSERT_EQ(t.bad.size(), 1u);
    EXPECT_DOUBLE_EQ(t.bad[0].measured, 2.0);
    EXPECT_EQ(t.bad[0].threshold, LINT_MIN_RIMS);
    EXPECT_EQ(t.bad[0].fix["ops"][0]["values"]["rim#0.width"], 6.0);
    EXPECT_EQ(t.bad[0].fix["ops"][0]["values"]["rim#1.width"], 2.0);
    EXPECT_TRUE(t.good.empty());
    EXPECT_TRUE(t.fixed.empty());
    // no rim at all is a choice (a plain face), not thin rims
    EXPECT_TRUE(findings(docWith(text2D(solid("#ffd040"), Json::array())), "rims_thin").empty());
}

TEST(StyleLintTest, ThickRimsFireAndScaleToTwelvePercent) {
    const Twins t = twins(text2D(solid("#ffd040"), Json::array({rim("#000000", 10), rim("#ffffff", 10)})),
                          text2D(solid("#ffd040"), Json::array({rim("#000000", 6), rim("#ffffff", 6)})), "rims_thick");
    ASSERT_EQ(t.bad.size(), 1u);
    EXPECT_DOUBLE_EQ(t.bad[0].measured, 20.0);
    EXPECT_EQ(t.bad[0].threshold, LINT_STEM);
    EXPECT_TRUE(t.good.empty());
    EXPECT_TRUE(t.fixed.empty());
}

TEST(StyleLintTest, ThickBandsOfA3DLayerScaleTheirGrowAndBevels) {
    Json layer = Json{{"id", "00000001"}, {"type", "text"}, {"name", "Title"}, {"text", "WIN"}, {"size", 200}, {"dimension", "3d"},
                      {"style_3d", {{"face", {{"color", "#ffd040"}}}, {"bevel", {{"width", 4}}},
                                    {"bands", Json::array({Json{{"grow", 8}, {"material", {{"color", "#000000"}}}}, Json{{"grow", 8}, {"material", {{"color", "#ffffff"}}}}})}}}};
    const TitleArtDoc doc = docWith(layer);
    const std::vector<StyleLintFinding> bad = findings(doc, "rims_thick");
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_DOUBLE_EQ(bad[0].measured, 20.0) << "the letter's outside bevel 4 + grows 8 + 8";
    EXPECT_EQ(bad[0].element, "bands");
    EXPECT_TRUE(findings(fixedBy(doc, bad[0]), "rims_thick").empty());
    // 3D titles read their edge from the extrusion: thin bands are fine
    layer["style_3d"]["bevel"]["width"] = 0;
    layer["style_3d"]["bands"] = Json::array({Json{{"grow", 1}, {"material", {{"color", "#000000"}}}}});
    EXPECT_TRUE(findings(docWith(layer), "rims_thin").empty());
}

TEST(StyleLintTest, HardShadowFiresAndSoftens) {
    auto withShadow = [](double opacity, double size, const char* color) {
        Json l = text2D(solid("#ffd040"), Json::array({rim("#000000", 6)}));
        l["effects"] = Json::array({Json{{"id", "00000021"}, {"type", "drop_shadow"}, {"params", {{"opacity", opacity}, {"size", size}, {"color", color}}}}});
        return l;
    };
    const Twins t = twins(withShadow(1.0, 2, "#000000"), withShadow(0.6, 14, "#000000"), "hard_shadow");
    ASSERT_EQ(t.bad.size(), 1u);
    EXPECT_DOUBLE_EQ(t.bad[0].measured, 1.0) << "2 px of a 200 px layer";
    EXPECT_EQ(t.bad[0].threshold, LINT_HARD_SHADOW_SIZE);
    EXPECT_EQ(t.bad[0].element, "fx:drop_shadow");
    EXPECT_EQ(t.bad[0].fix["ops"][0]["values"]["effects[0].params.size"], 14.0);
    EXPECT_TRUE(t.good.empty());
    EXPECT_TRUE(t.fixed.empty());
    // a tinted (light) shadow is a choice
    EXPECT_TRUE(findings(docWith(withShadow(1.0, 2, "#c08040")), "hard_shadow").empty());
}

TEST(StyleLintTest, ALayerWideSpectrumOnSeveralLinesSpansEachLine) {
    auto rainbow = [](const char* span, const char* text) {
        Json face = Json{{"fill", {{"kind", "linear"}, {"span", span},
                                   {"gradient", {{"stops", Json::array({Json{{"pos", 0}, {"color", "#ff0000"}}, Json{{"pos", 0.33}, {"color", "#ffff00"}},
                                                                        Json{{"pos", 0.66}, {"color", "#00ff00"}}, Json{{"pos", 1}, {"color", "#0000ff"}}})}}}}}};
        Json l = text2D(face, Json::array({rim("#000000", 6)}));
        l["text"] = text;
        return l;
    };
    const Twins t = twins(rainbow("layer", "BIG\nWIN"), rainbow("line", "BIG\nWIN"), "spectrum_span");
    ASSERT_EQ(t.bad.size(), 1u);
    EXPECT_EQ(t.bad[0].element, "face");
    EXPECT_GT(t.bad[0].measured, LINT_SPECTRUM_SPREAD);
    EXPECT_EQ(t.bad[0].fix["ops"][0]["values"]["face.fill.span"], "line");
    EXPECT_TRUE(t.good.empty());
    EXPECT_TRUE(t.fixed.empty());
    EXPECT_TRUE(findings(docWith(rainbow("layer", "BIG WIN\n")), "spectrum_span").empty()) << "one line (a trailing break is no line)";
}

TEST(StyleLintTest, HollowLettersWithoutRimsOrUnderAnOpaqueFace) {
    Json noRims = text2D(solid("#ffd040"), Json::array());
    noRims["style"]["knockout"] = true;
    Json withRim = text2D(Json{{"fill", {{"kind", "solid"}, {"color", "#ffd040"}}}, {"opacity", 0}}, Json::array({rim("#000000", 6)}));
    withRim["style"]["knockout"] = true;
    const Twins t = twins(noRims, withRim, "noop_knockout");
    ASSERT_EQ(t.bad.size(), 1u);
    EXPECT_EQ(t.bad[0].element, "rims");
    EXPECT_DOUBLE_EQ(t.bad[0].measured, 0.0);
    EXPECT_EQ(t.bad[0].fix["ops"][0]["op"], "add_rim");
    EXPECT_EQ(t.bad[0].fix["ops"][0]["values"]["width"], LINT_KNOCKOUT_RIM);
    EXPECT_TRUE(t.good.empty());
    EXPECT_TRUE(t.fixed.empty()) << "the rim is added and the face cleared in one fix";

    Json opaque = text2D(solid("#ffd040"), Json::array({rim("#000000", 6)}));
    opaque["style"]["knockout"] = true;
    const TitleArtDoc doc = docWith(opaque);
    const std::vector<StyleLintFinding> covered = findings(doc, "noop_knockout");
    ASSERT_EQ(covered.size(), 1u);
    EXPECT_EQ(covered[0].element, "face");
    EXPECT_DOUBLE_EQ(covered[0].measured, 1.0);
    EXPECT_TRUE(findings(fixedBy(doc, covered[0]), "noop_knockout").empty());
}

TEST(StyleLintTest, ErodeAtAmountOrEdgeZeroTakesTheStartingValues) {
    auto eroded = [](double amount, double edge) {
        Json l = text2D(solid("#ffd040"), Json::array({rim("#000000", 6)}));
        l["style"]["erode"] = Json{{"enabled", true}, {"amount", amount}, {"edge", edge}};
        return l;
    };
    const Twins t = twins(eroded(0, 6), eroded(0.4, 6), "noop_erode");
    ASSERT_EQ(t.bad.size(), 1u);
    EXPECT_DOUBLE_EQ(t.bad[0].measured, 0.0);
    EXPECT_EQ(t.bad[0].fix["ops"][0]["values"]["style.erode.amount"], LINT_ERODE_AMOUNT);
    EXPECT_FALSE(t.bad[0].fix["ops"][0]["values"].contains("style.erode.edge")) << "the edge was set: it stays";
    EXPECT_TRUE(t.good.empty());
    EXPECT_TRUE(t.fixed.empty());
    const TitleArtDoc edgeless = docWith(eroded(0.4, 0));
    const std::vector<StyleLintFinding> e = findings(edgeless, "noop_erode");
    ASSERT_EQ(e.size(), 1u);
    EXPECT_TRUE(findings(fixedBy(edgeless, e[0]), "noop_erode").empty());
}

TEST(StyleLintTest, GuideLayersAndWhatIsInsideAGuideGroupAreSkipped) {
    Json bad = text2D(solid("#ffd040"), Json::array({rim("#000000", 1)}));
    Json guide = bad;
    guide["guide"] = true;
    EXPECT_FALSE(findings(docWith(bad), "rims_thin").empty());
    EXPECT_TRUE(findings(docWith(guide), "rims_thin").empty());
    Json child = bad;
    child["id"] = "00000002";
    Json group = Json{{"id", "00000003"}, {"type", "group"}, {"name", "Guides"}, {"guide", true}, {"children", Json::array({child})}};
    EXPECT_TRUE(lintDocument(docWith(group)).empty());
}

// ---- parity with lint.ts ----

namespace {

    std::string snake(const std::string& camel) {
        std::string out;
        for (char c : camel) {
            if (std::isupper(static_cast<unsigned char>(c))) {
                out += '_';
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
        return out;
    }

    bool camelIdentifier(const std::string& s) {
        if (s.empty() || !std::islower(static_cast<unsigned char>(s[0]))) return false;
        bool upper = false;
        for (char c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c))) return false;
            upper = upper || std::isupper(static_cast<unsigned char>(c));
        }
        return upper;
    }

    // keys camelCase -> snake_case, and enum values with them ("shapeBurst" -> "shape_burst",
    // "whiteGloss" -> "white_gloss"); free text stays
    Json snakeTree(const Json& v, const std::string& key = {}) {
        if (v.is_object()) {
            Json out = Json::object();
            for (auto it = v.begin(); it != v.end(); ++it) out[snake(it.key())] = snakeTree(it.value(), it.key());
            return out;
        }
        if (v.is_array()) {
            Json out = Json::array();
            for (const Json& item : v) out.push_back(snakeTree(item, key));
            return out;
        }
        if (v.is_string() && key != "text" && key != "name" && camelIdentifier(v.get<std::string>())) return snake(v.get<std::string>());
        return v;
    }

    // A Pachislop layer as ours, limited to what the lint reads: the source (text, size), the 2D
    // style (rims' offsetX/offsetY become `offset`), the 3D style (bands' `shape` becomes `plate`),
    // the effects through S83's PachislopFxMap. Written here because the port's converter
    // (PachislopPort) was not on disk when this test was written.
    Json fromPachislop(const Json& p) {
        const Json& source = p["source"];
        const bool text = source.value("kind", std::string()) == "text";
        Json l = Json{{"id", p.value("id", std::string("00000001"))}, {"type", text ? "text" : "shape"}, {"name", p.value("name", std::string("Layer"))},
                      {"visible", p.value("visible", true)}, {"guide", p.value("guide", false)}, {"size", source.value("size", 200.0)}};
        if (text) l["text"] = source.value("text", std::string());
        else if (source.contains("shape")) l["shape"] = source["shape"];
        if (p.value("kind", std::string()) == "title3d") {
            l["dimension"] = "3d";
            Json s = snakeTree(p["style3d"]);
            for (Json& band : s["bands"]) {
                if (band.contains("shape")) {
                    band["plate"] = band["shape"];
                    band.erase("shape");
                }
            }
            l["style_3d"] = s;
        } else {
            l["dimension"] = "2d";
            Json s = snakeTree(p["style"]);
            for (Json& r : s["rims"]) {
                r["offset"] = Json::array({r.value("offset_x", 0.0), r.value("offset_y", 0.0)});
                r.erase("offset_x");
                r.erase("offset_y");
            }
            l["style"] = s;
        }
        Json effects = Json::array();
        for (const Json& fx : p.value("effects", Json::array())) {
            const PachislopFxConversion c = convertPachislopEffect(fx.value("type", std::string()), fx.value("params", Json::object()));
            if (!c.ok) continue;
            effects.push_back(Json{{"type", c.type}, {"enabled", fx.value("enabled", true)}, {"params", c.params}});
        }
        l["effects"] = effects;
        return l;
    }

    std::string ourRule(const std::string& id) {
        if (id.rfind("contrast-", 0) == 0) return "contrast";
        static const std::map<std::string, std::string> rules = {{"hues", "hue_count"},       {"thin", "rims_thin"},          {"thick", "rims_thick"},
                                                                 {"shadow", "hard_shadow"},   {"shadow3d", "hard_shadow"},    {"span", "spectrum_span"},
                                                                 {"knockout-rims", "noop_knockout"}, {"knockout-face", "noop_knockout"}, {"erode-zero", "noop_erode"}};
        auto it = rules.find(id);
        return it == rules.end() ? "?" + id : it->second;
    }

}

TEST(StyleLintTest, MatchesPachislop) {
    const fs::path path = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "titleart" / "pachislop-port" / "dump" / "pachislop-1c32502.json";
    std::ifstream in(path);
    ASSERT_TRUE(in) << path;
    const Json dump = Json::parse(in);
    const Json& cases = dump["lint_cases"];
    ASSERT_EQ(cases.size(), 120u);
    size_t compared = 0, theirFindings = 0;
    for (const Json& c : cases) {
        const std::string name = c["case"].get<std::string>();
        const TitleArtDoc doc = docWith(fromPachislop(c["layer"]));
        const std::vector<StyleLintFinding> ours = lintDocument(doc);
        std::vector<std::string> ourIds, theirIds;
        for (const StyleLintFinding& f : ours) ourIds.push_back(f.rule);
        for (const Json& f : c["findings"]) theirIds.push_back(ourRule(f["id"].get<std::string>()));
        theirFindings += theirIds.size();
        std::sort(ourIds.begin(), ourIds.end());
        std::sort(theirIds.begin(), theirIds.end());
        std::ostringstream oursText;
        for (const StyleLintFinding& f : ours) oursText << "\n  " << f.rule << " " << f.element << " " << f.measured << ": " << f.message;
        EXPECT_EQ(ourIds, theirIds) << name << ": ours" << oursText.str();
        compared++;
        if (ourIds != theirIds) continue;
        // the measured values and elements lint.ts printed
        for (const Json& f : c["findings"]) {
            const std::string id = f["id"].get<std::string>();
            const Json& vars = f["message"].value("vars", Json::object());
            const std::string rule = ourRule(id);
            auto mine = std::find_if(ours.begin(), ours.end(), [&](const StyleLintFinding& o) {
                if (o.rule != rule) return false;
                if (rule != "contrast") return true;
                const Json& b = vars["b"];
                const std::string word = b.value("en", std::string()).rfind("Band", 0) == 0 ? "band#" : "rim#";
                return o.element == word + std::to_string(b["vars"]["n"].get<int>() - 1);
            });
            ASSERT_NE(mine, ours.end()) << name << ": no " << rule << " matching " << f.dump();
            if (rule == "contrast") {
                EXPECT_NEAR(mine->measured, std::stod(vars["dL"].get<std::string>()), 0.5 + 1e-9) << name;
            }
            if (rule == "rims_thin" || rule == "rims_thick") {
                EXPECT_NEAR(mine->measured, std::stod(vars["total"].get<std::string>()), 0.05 + 1e-9) << name;
            }
        }
    }
    EXPECT_EQ(compared, 120u);
    EXPECT_GE(theirFindings, 20u) << "the red controls and the library's own findings";
}
