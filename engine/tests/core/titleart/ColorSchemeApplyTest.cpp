// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The colour values of the gold, rainbow, zebra and blue fixtures are Pachislop's
// (src/presets/colourways.ts, src/model/defaults.ts) by SafEight, used with the author's permission.

#include <gtest/gtest.h>

#include "TestPaths.h"
#include "support/TitleStandardTitle.h"
#include "titleart/ColorScheme.h"
#include "titleart/ColorSchemeApply.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;
namespace fs = std::filesystem;

namespace {

#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    constexpr bool HAS_3D = true;
#else
    constexpr bool HAS_3D = false;
#endif

    // ---- scheme fixtures ----

    Json grad(std::vector<std::pair<double, const char*>> stops) {
        return standardGradient(std::move(stops));
    }

    Json linear(const Json& gradient) {
        return standardLinear(gradient);
    }

    Json paint(Json fill, Json ramp, Json material, const char* shade) {
        Json p{{"fill", std::move(fill)}, {"ramp", std::move(ramp)}, {"material_3d", std::move(material)}, {"shade", shade}};
        return p;
    }

    Json darkPaint(const char* color) {
        return paint(standardSolid(color), nullptr, Json{{"color", color}, {"metallic", 1}, {"roughness", 0.4}, {"env_multiplier", 0.6}}, "#000000");
    }

    Json solidPaint(const char* color, const char* shade, Json material) {
        return paint(standardSolid(color), nullptr, std::move(material), shade);
    }

    Json goldRampGradient() {
        return grad({{0, "#3a0c00"}, {0.25, "#a83800"}, {0.5, "#f08c00"}, {0.75, "#ffd23a"}, {1, "#fff8d6"}});
    }

    Json goldMaterial() {
        return Json{{"preset", "gold"}, {"color", "#ffc23a"}, {"metallic", 1}, {"roughness", 0.06}, {"gradient_span", "line"}, {"gradient_bend", 0.6},
                    {"gradient", grad({{0, "#fff8d8"}, {0.3, "#f0a018"}, {0.51, "#1a0800"}, {0.72, "#c87810"}, {1, "#fff4c8"}})}};
    }

    Json goldRim() {
        return paint(linear(grad({{0, "#8a2c00"}, {0.17, "#ffe470"}, {0.33, "#c05a00"}, {0.5, "#ffbe30"}, {0.67, "#fff0a0"}, {0.83, "#f09a10"}, {1, "#ffe060"}})),
                     goldRampGradient(), goldMaterial(), "#4a2200");
    }

    Json whitePaint() {
        return solidPaint("#ffffff", "#5a6478", Json{{"preset", "white_gloss"}, {"color", "#ffffff"}, {"clearcoat", 1}, {"roughness", 0.15}});
    }

    Json schemeHeader(const char* id) {
        return Json{{"format", TITLE_COLORS_FORMAT}, {"version", TITLE_COLORS_VERSION}, {"id", id}, {"revision", 1},
                    {"name", {{"en", id}, {"ja", id}}}};
    }

    Json goldSchemeJson() {
        Json j = schemeHeader("gold");
        j["paints"] = Json{
            {"face", paint(linear(grad({{0, "#ffffff"}, {0.16, "#fff8b8"}, {0.38, "#ffd820"}, {0.6, "#ff9400"}, {0.8, "#b84000"}, {1, "#4a1000"}})),
                           goldRampGradient(), goldMaterial(), "#5a2a00")},
            {"dark", darkPaint("#0a0604")},
            {"accents", Json::array({goldRim(), paint(linear(redFill()), grad({{0, "#2a0000"}, {0.4, "#a10010"}, {0.75, "#ff3a3a"}, {1, "#fff0f0"}}),
                                                      Json{{"preset", "red_metal"}, {"color", "#c00010"}, {"metallic", 1}}, "#3a0000")})},
            {"base", darkPaint("#120600")},
            {"side_3d", Json{{"preset", "copper"}, {"color", "#c87533"}, {"metallic", 1}, {"roughness", 0.25}}}};
        j["face_glow"] = "#fff3b0";
        j["glow"] = "#ffb000";
        j["glow_gradient"] = grad({{0, "#ffffff"}, {0.3, "#ffd040"}, {1, "#ff5a00"}});
        j["shadow"] = "#3a1800";
        j["glint"] = "#fff4dc";
        j["light_ramp"] = Json::array({"#2a1000", "#8a3000", "#c85a00", "#ff9400", "#ffb000", "#ffd820", "#fff8b8"});
        j["orb"] = Json{{"ramp", grad({{0, "#fff4a0"}, {0.55, "#ffb000"}, {1, "#a83800"}})}};
        return j;
    }

    Json rainbowSpectrumGradient() {
        return grad({{0, "#ff2a2a"}, {0.17, "#ff9b00"}, {0.33, "#fff200"}, {0.5, "#2bff3a"}, {0.67, "#00c8ff"}, {0.83, "#3a3aff"}, {1, "#d22cff"}});
    }

    Json rainbowSchemeJson() {
        Json j = schemeHeader("rainbow");
        j["paints"] = Json{
            {"face", paint(linear(grad({{0, "#ff1838"}, {0.14, "#ff5a00"}, {0.28, "#ffc400"}, {0.42, "#20d838"}, {0.5, "#008a40"}, {0.58, "#0088e8"},
                                        {0.82, "#2a34f0"}, {1, "#9a10f0"}})),
                           nullptr, Json{{"preset", "rainbow"}, {"metallic", 1}}, "#3a0060")},
            {"dark", darkPaint("#1a0030")},
            {"accents", Json::array({whitePaint(), goldRim()})},
            {"base", darkPaint("#0a0014")},
            {"side_3d", goldMaterial()}};
        j["face_glow"] = "#3a0080";
        j["glow"] = "#fff4c8";
        j["glow_gradient"] = rainbowSpectrumGradient();
        j["shadow"] = "#2a0040";
        j["glint"] = "#fffaf0";
        j["light_ramp"] = Json::array({"#14002a", "#6a1aff", "#ff2ad0", "#ff7a1a", "#ffe040", "#b0fff4"});
        j["orb"] = Json{{"ramp", rainbowSpectrumGradient()}, {"radial", false}};
        j["spectrum"] = true;
        return j;
    }

    Json zebraMarks() {
        return Json{{"kind", "zebra"}, {"color", "#060608"}, {"scale", 12}, {"angle", -8}, {"seed", 3}, {"shade", 0.55}, {"relief", 0.25}};
    }

    Json zebraSchemeJson() {
        Json j = schemeHeader("zebra");
        Json face = paint(linear(grad({{0, "#ffffff"}, {0.4, "#eef1f6"}, {0.5, "#a9b0bd"}, {0.56, "#e2e6ed"}, {0.8, "#f7f9fc"}, {1, "#c9cfd9"}})),
                          grad({{0, "#14161c"}, {0.35, "#5d6474"}, {0.7, "#c9cfdb"}, {1, "#ffffff"}}),
                          Json{{"preset", "silver"}, {"color", "#eef1f6"}, {"metallic", 0.75}, {"pattern", zebraMarks()}}, "#2a3040");
        face["material_3d"]["pattern"]["relief"] = 0.4;
        face["pattern"] = zebraMarks();
        j["paints"] = Json{{"face", face}, {"dark", darkPaint("#0a0604")}, {"accents", Json::array({whitePaint(), goldRim()})}, {"base", darkPaint("#050608")}};
        j["glow"] = "#e8eeff";
        j["shadow"] = "#0c0e14";
        j["light_ramp"] = Json::array({"#0c0e12", "#3a3e48", "#7a8090", "#b8bfcc", "#e2e6ed", "#f4f6fa"});
        j["pattern_ground"] = "#f2f2f0";
        return j;
    }

    // a scheme with dark accents and a light "dark" paint: a ring's role read from its colour
    // after one application is not the role that painted it
    Json monoSchemeJson() {
        Json j = schemeHeader("mono-dark");
        j["paints"] = Json{{"face", solidPaint("#111111", "#000000", Json{{"color", "#111111"}, {"roughness", 0.7}})},
                           {"dark", solidPaint("#5a4a7a", "#000000", Json{{"color", "#5a4a7a"}, {"roughness", 0.7}})},
                           {"accents", Json::array({solidPaint("#ffffff", "#000000", Json{{"color", "#ffffff"}, {"roughness", 0.7}}),
                                                    solidPaint("#111111", "#000000", Json{{"color", "#111111"}, {"roughness", 0.7}})})},
                           {"base", solidPaint("#000000", "#000000", Json{{"color", "#000000"}, {"roughness", 0.7}})}};
        j["light_ramp"] = Json::array({"#111111", "#444444", "#888888", "#cccccc"});
        return j;
    }

    // blue: a 3D bevel rim (silver) on bands in the face's own material
    Json blueSchemeJson() {
        Json j = schemeHeader("blue");
        const Json silverRim = paint(linear(grad({{0, "#f4f6fa"}, {0.28, "#9aa3b2"}, {0.5, "#283040"}, {0.72, "#c3cad6"}, {1, "#ffffff"}})),
                                     grad({{0, "#14161c"}, {0.35, "#5d6474"}, {0.7, "#c9cfdb"}, {1, "#ffffff"}}),
                                     Json{{"preset", "chrome"}, {"color", "#ffffff"}, {"metallic", 1}, {"roughness", 0.04}}, "#1a2234");
        j["paints"] = Json{{"face", paint(linear(grad({{0, "#e0f4ff"}, {0.45, "#1a7aff"}, {1, "#001a6a"}})),
                                          grad({{0, "#000a2a"}, {0.4, "#0a3cc9"}, {0.75, "#4fb4ff"}, {1, "#f0fbff"}}),
                                          Json{{"preset", "blue_metal"}, {"color", "#1a6aff"}, {"metallic", 1}}, "#001a5c")},
                           {"dark", darkPaint("#001a5c")},
                           {"accents", Json::array({whitePaint(), silverRim})},
                           {"base", darkPaint("#000a26")},
                           {"bevel_rim_3d", silverRim},
                           {"side_3d", Json{{"preset", "blue_metal"}, {"color", "#0a2a8a"}, {"metallic", 1}}}};
        j["glow"] = "#3aa8ff";
        j["shadow"] = "#001040";
        j["glint"] = "#dff0ff";
        j["light_ramp"] = Json::array({"#000c40", "#0030b0", "#1a6aff", "#3aa8ff", "#9ad4ff", "#dff0ff"});
        return j;
    }

    ColorScheme parsed(const Json& j) {
        ColorScheme s;
        std::vector<std::string> warnings;
        const bool ok = parseColorScheme(j, s, &warnings);
        EXPECT_TRUE(ok) << j.value("id", std::string());
        for (const std::string& w : warnings) ADD_FAILURE() << j.value("id", std::string()) << ": " << w;
        return s;
    }

    fs::path schemesDir() {
        return fs::path(jasperengine::test::testPaths().sourceDir) / ".." / ".." / ".." / "editor" / "resources" / "titleart" / "color-schemes";
    }

    std::string readText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // the fixtures and every committed scheme
    std::vector<ColorScheme> allSchemes() {
        std::vector<ColorScheme> out = {parsed(goldSchemeJson()), parsed(rainbowSchemeJson()), parsed(zebraSchemeJson()),
                                        parsed(monoSchemeJson()), parsed(blueSchemeJson())};
        std::error_code ec;
        if (fs::is_directory(schemesDir(), ec)) {
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(schemesDir(), ec)) {
                if (e.path().extension() == ".titlecolors") files.push_back(e.path());
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& f : files) out.push_back(parsed(Json::parse(readText(f))));
        }
        return out;
    }

    // ---- document fixtures ----

    Json layerOf(const TitleArtDoc& doc, size_t i) {
        return doc.root["layers"][i];
    }

    const Json& rimMaterial(const TitleArtDoc& doc, size_t layer, size_t rim) {
        return doc.root["layers"][layer]["style"]["rims"][rim]["material"];
    }

    Json textLayer(const char* id, Json rims, Json face = Json::object()) {
        Json style{{"face", std::move(face)}, {"rims", std::move(rims)}};
        return Json{{"id", id}, {"type", "text"}, {"text", "AB"}, {"style", std::move(style)}};
    }

    Json generatorLayer(const char* id, const char* type, Json params = Json::object()) {
        return Json{{"id", id}, {"type", "generator"}, {"generator", {{"type", type}, {"params", std::move(params)}}}};
    }

    // the layers the idempotence and normalisation checks run over: the classic title with light,
    // glow and shadow effects on itself and on a rim, per-character faces, a shape with a faded
    // rim and a relief-only pattern, an image and SVG artwork, a group, every generator and every
    // effect at its defaults, and (with the 3D module) a banded 3D title and an orb
    TitleArtDoc fixtureDocument() {
        std::vector<Json> layers;
        Json classic = standardTitleLayer("00000001");
        // the glows start off any scheme's colour, so recolouring them shows in the changed paths
        classic["effects"] = Json::array({standardEffect("00000201", "outer_glow", Json{{"color", "#00ff00"}}), standardEffect("00000202", "drop_shadow"),
                                          standardEffect("00000203", "inner_shadow"), standardEffect("00000204", "shatter"),
                                          standardEffect("00000205", "neon_tube")});
        classic["part_effects"] = Json{{"rim#1", Json::array({standardEffect("00000206", "outer_glow", Json{{"color", "#00ff00"}})})}};
        classic["chars"] = Json{{"0", {{"face", {{"fill", standardSolid("#ff0000")}}}}}, {"1", {{"rotation", 5}}}};
        layers.push_back(classic);

        Json faded = standardRim("00000301", 4, Json{{"fill", {{"kind", "linear"}, {"gradient", {{"stops", Json::array({
            Json{{"pos", 0}, {"color", "#ffffff"}, {"alpha", 1}}, Json{{"pos", 1}, {"color", "#ffffff"}, {"alpha", 0}}})}}}}}});
        Json shape = textLayer("00000002", Json::array({faded}), Json{{"fill", standardSolid("#40a0ff")},
                                                                       {"pattern", {{"kind", "asanoha"}, {"opacity", 0}, {"relief", 0.4}}}});
        shape["type"] = "shape";
        shape.erase("text");
        shape["shape"] = "star";
        layers.push_back(shape);

        Json image{{"id", "00000003"}, {"type", "image"}, {"style", {{"rims", Json::array({standardRim("00000302", 3, Json{{"fill", standardSolid("#ffffff")}})})}}}};
        layers.push_back(image);
        Json artwork{{"id", "00000004"}, {"type", "vector"}, {"mode", "artwork"},
                     {"style", {{"rims", Json::array({standardRim("00000303", 2, Json{{"fill", standardSolid("#000000")}})})}}}};
        layers.push_back(artwork);

        Json group{{"id", "00000005"}, {"type", "group"}, {"effects", Json::array({standardEffect("00000207", "outer_glow", Json{{"color", "#00ff00"}})})}};
        group["children"] = Json::array({textLayer("00000006", Json::array({standardRim("00000304", 2, Json{{"fill", standardSolid("#202020")}})})),
                                         generatorLayer("00000007", "particles")});
        layers.push_back(group);

        int n = 0;
        for (const GeneratorDef* g : TitleEffectRegistry::get().generators()) {
            char id[9];
            std::snprintf(id, sizeof(id), "%08x", 0x100 + n++);
            layers.push_back(generatorLayer(id, g->type));
        }
        // every effect at its defaults, over as many layers as the per-stack limit needs
        Json effects = Json::array();
        int e = 0;
        auto flush = [&]() {
            if (effects.empty()) return;
            char id[9];
            std::snprintf(id, sizeof(id), "%08x", 0x200 + e);
            // rims and effects without ids get fresh ones from normalize
            Json l = textLayer(id, Json::array({standardRim("", 2, Json{{"fill", standardSolid("#ffffff")}})}));
            l["effects"] = effects;
            layers.push_back(l);
            effects = Json::array();
        };
        for (const EffectDef* d : TitleEffectRegistry::get().effects()) {
            char id[9];
            std::snprintf(id, sizeof(id), "%08x", 0x300 + e++);
            effects.push_back(standardEffect(id, d->type));
            if (effects.size() == TITLE_MAX_EFFECTS) flush();
        }
        flush();

        if (HAS_3D) {
            const Json face = goldMaterial();
            Json s3{{"face", face},
                    {"bands", Json::array({Json{{"material", face}}, Json{{"material", {{"color", "#101010"}}}},
                                           Json{{"material", {{"color", "#ffffff"}, {"gradient_axis", "depth"}}}}})}};
            Json t3 = textLayer("00000008", Json::array());
            t3["dimension"] = "3d";
            t3["style_3d"] = s3;
            layers.push_back(t3);
            Json orb{{"id", "00000009"}, {"type", "shape"}, {"shape", "orb"}, {"dimension", "3d"},
                     {"style_3d", {{"mode", "orb"}, {"face", {{"preset", "candy"}, {"color", "#ff0000"}, {"roughness", 0.1}, {"clearcoat", 1}}},
                                   {"bands", Json::array({Json{{"material", {{"color", "#c0c0c0"}}}}})}}}};
            layers.push_back(orb);
        }
        TitleArtDoc doc = standardDocument(layers);
        // a second pass: a RowArray param's rows (magic_circle) come out of the first normalize in
        // definition order and sorted only by the second
        normalizeTitleArt(doc);
        return doc;
    }

    std::string canonical(const Json& value) {
        return writeTitleArtJson(value);
    }

    bool contains(const std::vector<std::string>& list, const std::string& item) {
        return std::find(list.begin(), list.end(), item) != list.end();
    }

    std::string joined(const std::vector<std::string>& list) {
        std::string out;
        for (const std::string& s : list) out += s + "\n";
        return out;
    }

    // the paths where two JSON values differ (at most `limit` of them)
    void differences(const Json& a, const Json& b, const std::string& path, std::vector<std::string>& out, size_t limit = 8) {
        if (out.size() >= limit) return;
        if (a.is_object() && b.is_object()) {
            std::vector<std::string> ka, kb;
            for (auto it = a.begin(); it != a.end(); ++it) ka.push_back(it.key());
            for (auto it = b.begin(); it != b.end(); ++it) kb.push_back(it.key());
            if (ka != kb) {
                out.push_back(path + ": members " + Json(ka).dump() + " vs " + Json(kb).dump());
                return;
            }
            for (const std::string& k : ka) differences(a[k], b[k], path + "." + k, out, limit);
            return;
        }
        if (a.is_array() && b.is_array() && a.size() == b.size()) {
            for (size_t i = 0; i < a.size(); i++) differences(a[i], b[i], path + "[" + std::to_string(i) + "]", out, limit);
            return;
        }
        if (a != b) out.push_back(path + ": " + a.dump() + " vs " + b.dump());
    }

    std::string differencesOf(const Json& a, const Json& b) {
        std::vector<std::string> out;
        differences(a, b, "", out);
        return joined(out);
    }

    class ColorSchemeTest : public ::testing::Test {};
    class ColorSchemeApplyTest : public ::testing::Test {};

}

// ---- the file ----

TEST_F(ColorSchemeTest, RoundTripKeepsUnknownMembers) {
    Json j = goldSchemeJson();
    j["future_member"] = Json{{"a", 1}};
    j["paints"]["face"]["future_paint_member"] = "kept";
    j["paints"]["future_role"] = Json{{"fill", standardSolid("#123456")}};
    j["orb"]["future_orb_member"] = 2;
    j["paints"]["dark"]["fill"]["future_fill_member"] = true;
    const ColorScheme s = parsed(j);
    const Json written = writeColorScheme(s);
    EXPECT_EQ(written["future_member"], j["future_member"]);
    EXPECT_EQ(written["paints"]["face"]["future_paint_member"], "kept");
    EXPECT_EQ(written["paints"]["future_role"], j["paints"]["future_role"]);
    EXPECT_EQ(written["orb"]["future_orb_member"], 2);
    EXPECT_EQ(written["paints"]["dark"]["fill"]["future_fill_member"], true);
    // write(parse(write(s))) == write(s)
    const ColorScheme again = parsed(written);
    EXPECT_EQ(writeColorSchemeText(again), writeColorSchemeText(s));
    // unknown members follow the known ones
    std::vector<std::string> order;
    for (auto it = written.begin(); it != written.end(); ++it) order.push_back(it.key());
    EXPECT_EQ(order.back(), "future_member");
}

TEST_F(ColorSchemeTest, SparseCanonicalOrder) {
    const ColorScheme s = parsed(goldSchemeJson());
    const Json written = writeColorScheme(s);
    std::vector<std::string> order;
    for (auto it = written.begin(); it != written.end(); ++it) order.push_back(it.key());
    const std::vector<std::string> expected = {"format", "version", "id", "revision", "name", "paints", "face_glow", "glow",
                                               "glow_gradient", "shadow", "glint", "light_ramp", "orb"};
    EXPECT_EQ(order, expected);
    const Json& dark = written["paints"]["dark"];
    EXPECT_FALSE(dark.contains("shade")) << "#000000 is the default shade";
    EXPECT_FALSE(dark.contains("ramp")) << "no ramp is the default";
    EXPECT_FALSE(dark["fill"].contains("kind")) << "solid is the default kind: " << canonical(dark);
    EXPECT_EQ(dark["fill"]["color"], "#0a0604");
    EXPECT_FALSE(written.contains("spectrum"));
    EXPECT_FALSE(written["orb"].contains("radial"));
    // a member at its default is omitted and reads back as the default
    ColorScheme defaults;
    defaults.id = "plain";
    const Json plain = writeColorScheme(defaults);
    EXPECT_EQ(canonical(plain["paints"]), "{}");
    ColorScheme read;
    ASSERT_TRUE(parseColorScheme(plain, read, nullptr));
    EXPECT_EQ(read.glow, "#ffffff");
    EXPECT_EQ(read.shadow, "#000000");
    EXPECT_EQ(read.face.fill["kind"], "solid");
    EXPECT_EQ(writeColorSchemeText(read), writeColorSchemeText(defaults));
    // colours are written lowercase
    Json upper = goldSchemeJson();
    upper["glow"] = "#FFB000";
    EXPECT_EQ(writeColorScheme(parsed(upper))["glow"], "#ffb000");
}

TEST_F(ColorSchemeTest, VersionNewerFails) {
    Json j = goldSchemeJson();
    j["version"] = TITLE_COLORS_VERSION + 1;
    ColorScheme s;
    s.id = "untouched";
    std::string code;
    EXPECT_FALSE(parseColorScheme(j, s, nullptr, &code));
    EXPECT_EQ(code, "version_newer");
    EXPECT_EQ(s.id, "untouched");
    j["version"] = 0;
    EXPECT_FALSE(parseColorScheme(j, s, nullptr, &code));
    EXPECT_EQ(code, "bad_version");
    j = goldSchemeJson();
    j["format"] = "jasperengine.titlelook";
    EXPECT_FALSE(parseColorScheme(j, s, nullptr, &code));
    EXPECT_EQ(code, "bad_format");
    EXPECT_FALSE(parseColorScheme(Json::array(), s, nullptr, &code));
    EXPECT_EQ(code, "not_object");
    EXPECT_EQ(titleColorsMigrations().latestVersion(), TITLE_COLORS_VERSION);
}

TEST_F(ColorSchemeTest, LenientRead) {
    Json j = goldSchemeJson();
    j["glow"] = 12;
    j["spectrum"] = "yes";
    j["paints"]["dark"]["fill"]["kind"] = 5;
    j["paints"]["accents"] = "none";
    ColorScheme s;
    std::vector<std::string> warnings;
    ASSERT_TRUE(parseColorScheme(j, s, &warnings));
    EXPECT_EQ(s.glow, "#ffffff");
    EXPECT_FALSE(s.spectrum);
    EXPECT_EQ(s.dark.fill["kind"], "solid");
    EXPECT_TRUE(s.accents.empty());
    auto warned = [&](const std::string& prefix) {
        return std::any_of(warnings.begin(), warnings.end(), [&](const std::string& w) { return w.rfind(prefix, 0) == 0; });
    };
    EXPECT_TRUE(warned("glow:")) << joined(warnings);
    EXPECT_TRUE(warned("spectrum:")) << joined(warnings);
    EXPECT_TRUE(warned("paints.dark.fill.kind:")) << joined(warnings);
    EXPECT_TRUE(warned("paints.accents:")) << joined(warnings);
}

TEST_F(ColorSchemeTest, CommittedFilesAreCanonical) {
    // every built-in scheme reads without a warning and is written back byte for byte
    // (tools/titleart/expand_scheme.py and the Pachislop port write what writeColorScheme writes)
    std::error_code ec;
    ASSERT_TRUE(fs::is_directory(schemesDir(), ec)) << schemesDir();
    size_t count = 0;
    for (const auto& e : fs::directory_iterator(schemesDir(), ec)) {
        if (e.path().extension() != ".titlecolors") continue;
        count++;
        const std::string text = readText(e.path());
        ColorScheme s;
        std::vector<std::string> warnings;
        ASSERT_TRUE(parseColorScheme(Json::parse(text), s, &warnings)) << e.path();
        EXPECT_TRUE(warnings.empty()) << e.path() << "\n" << joined(warnings);
        EXPECT_EQ(writeColorSchemeText(s), text) << e.path();
        EXPECT_EQ(s.id + ".titlecolors", e.path().filename().string());
        EXPECT_GE(s.lightRamp.size(), 4u) << e.path();
        EXPECT_LE(s.lightRamp.size(), 7u) << e.path();
    }
    EXPECT_GE(count, 22u) << "the 22 new schemes of S84 §6.9";
}

TEST_F(ColorSchemeTest, DocumentMemberIsWrittenOnlyWhenUsed) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    const SchemaMember* m = schema.member(*schema.kind("document"), "color_schemes");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->owner, "S84");
    EXPECT_EQ(m->hash, HashScope::Identity) << "the embedded data draws nothing";
    EXPECT_EQ(schema.reservedOwner("document", "color_schemes"), "");
    // a document saved before S84 keeps its bytes
    TitleArtDoc plain = makeNewTitleArt();
    const std::string before = writeTitleArt(plain);
    const TitleArtParseResult read = parseTitleArt(before);
    ASSERT_TRUE(read.ok);
    EXPECT_FALSE(read.doc.root.contains("color_schemes"));
    EXPECT_EQ(writeTitleArt(read.doc), before);
    // non-objects are dropped, keys sorted
    Json root = plain.root;
    root["color_schemes"] = Json{{"zebra", writeColorScheme(parsed(zebraSchemeJson()))}, {"bad", 3}, {"gold", writeColorScheme(parsed(goldSchemeJson()))}};
    const TitleArtParseResult embedded = parseTitleArtJson(root);
    ASSERT_TRUE(embedded.ok);
    const Json& map = embedded.doc.root["color_schemes"];
    std::vector<std::string> keys;
    for (auto it = map.begin(); it != map.end(); ++it) keys.push_back(it.key());
    EXPECT_EQ(keys, (std::vector<std::string>{"gold", "zebra"}));
    const std::string text = writeTitleArt(embedded.doc);
    EXPECT_EQ(writeTitleArt(parseTitleArt(text).doc), text) << "write(parse(write(d))) == write(d)";
}

// ---- roles (AC6) ----

TEST_F(ColorSchemeApplyTest, KeylineIsDark) {
    // a keyline at luma 0.1 takes the dark paint; a light ring takes an accent
    TitleArtDoc doc = standardDocument({textLayer("00000001", Json::array({standardRim("00000101", 1, Json{{"fill", standardSolid("#1a1a1a")}}),
                                                                           standardRim("00000102", 2, Json{{"fill", standardSolid("#e0e0e0")}})}))});
    const ColorScheme gold = parsed(goldSchemeJson());
    applyColorScheme(doc, gold, {});
    EXPECT_EQ(rimMaterial(doc, 0, 0)["fill"]["color"], "#0a0604");
    EXPECT_EQ(rimMaterial(doc, 0, 0)["fill"]["kind"], "solid");
    // one accent ring of two accents: it takes the last (the red rim)
    EXPECT_EQ(rimMaterial(doc, 0, 1)["fill"]["kind"], "linear");
    EXPECT_EQ(rimMaterial(doc, 0, 1)["fill"]["gradient"]["stops"][1]["color"], "#e00012");
}

TEST_F(ColorSchemeApplyTest, OffsetRimIsBase) {
    TitleArtDoc doc = standardDocument({textLayer("00000001", Json::array({standardRim("00000101", 2, Json{{"fill", standardSolid("#ffffff")}}),
                                                                           standardRim("00000102", 2, Json{{"fill", standardSolid("#ffffff")}}, Json::array({0, -3}))}))});
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    EXPECT_EQ(rimMaterial(doc, 0, 1)["fill"]["color"], "#120600");
    EXPECT_EQ(layerOf(doc, 0)["style"]["rims"][1]["offset"], Json::array({0, -3})) << "the offset is structure";
}

TEST_F(ColorSchemeApplyTest, OutermostAccentTakesTheLast) {
    // three accent rings, two accents: rings 0 and 1 take accents[0], the outermost accents[1]
    Json rims = Json::array();
    for (const char* id : {"00000101", "00000102", "00000103"}) rims.push_back(standardRim(id, 2, Json{{"fill", standardSolid("#c0c0c0")}}));
    TitleArtDoc doc = standardDocument({textLayer("00000001", rims)});
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const std::string goldRimStop = "#ffe470", redStop = "#e00012";
    EXPECT_EQ(rimMaterial(doc, 0, 0)["fill"]["gradient"]["stops"][1]["color"], goldRimStop);
    EXPECT_EQ(rimMaterial(doc, 0, 1)["fill"]["gradient"]["stops"][1]["color"], goldRimStop);
    EXPECT_EQ(rimMaterial(doc, 0, 2)["fill"]["gradient"]["stops"][1]["color"], redStop);
    EXPECT_EQ(rimMaterial(doc, 0, 2)["bevel"]["shadow_color"], "#3a0000") << "the bevel shadow takes the paint's shade";
}

// ---- structure (AC6) ----

TEST_F(ColorSchemeApplyTest, StructureKept) {
    TitleArtDoc doc = classicFixtureDocument();
    Json& layer = doc.root["layers"][0];
    layer["style"]["rims"][1]["join"] = "round";
    layer["style"]["rims"][2]["plate"]["kind"] = "hull";
    layer["style"]["face"]["texture"] = Json{{"kind", "brushed"}, {"amount", 0.4}, {"scale", 6}, {"blend", "overlay"}, {"seed", 3}, {"target", "color"}, {"angle", 10}};
    layer["style"]["face"]["satin"]["enabled"] = true;
    normalizeTitleArt(doc);
    const Json before = doc.root["layers"][0];
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const Json& after = doc.root["layers"][0];
    ASSERT_EQ(after["style"]["rims"].size(), before["style"]["rims"].size());
    for (size_t i = 0; i < before["style"]["rims"].size(); i++) {
        Json a = before["style"]["rims"][i], b = after["style"]["rims"][i];
        const Json ma = a["material"], mb = b["material"];
        a.erase("material");
        b.erase("material");
        EXPECT_EQ(canonical(a), canonical(b)) << "rim " << i << ": width, join, plate, offset, fill_counters";
        for (const char* key : {"opacity", "reflection", "satin", "texture", "gloss", "refraction", "glass", "sheen"}) {
            EXPECT_EQ(canonical(ma[key]), canonical(mb[key])) << "rim " << i << " " << key;
        }
        Json bevelA = ma["bevel"], bevelB = mb["bevel"];
        bevelA.erase("shadow_color");
        bevelB.erase("shadow_color");
        EXPECT_EQ(canonical(bevelA), canonical(bevelB)) << "rim " << i << " bevel";
    }
    for (const char* key : {"texture", "satin", "gloss", "bevel"}) {
        Json a = before["style"]["face"][key], b = after["style"]["face"][key];
        a.erase("shadow_color");
        b.erase("shadow_color");
        EXPECT_EQ(canonical(a), canonical(b)) << "face " << key;
    }
    EXPECT_EQ(canonical(before["style"]["erode"]), canonical(after["style"]["erode"]));
    EXPECT_EQ(before["style"]["knockout"], after["style"]["knockout"]);
    // effects: the non-colour params are byte-identical
    ASSERT_EQ(after["effects"].size(), before["effects"].size());
    for (size_t i = 0; i < before["effects"].size(); i++) {
        Json a = before["effects"][i]["params"], b = after["effects"][i]["params"];
        a.erase("color");
        b.erase("color");
        a.erase("gradient");
        b.erase("gradient");
        EXPECT_EQ(canonical(a), canonical(b)) << before["effects"][i]["type"];
    }
    // source, text, transform and the rest of the layer are untouched
    for (const char* key : {"id", "type", "name", "text", "font", "size", "transform", "blend", "opacity", "masks", "warp", "distort", "chars", "dimension"}) {
        EXPECT_EQ(canonical(before[key]), canonical(after[key])) << key;
    }
}

TEST_F(ColorSchemeApplyTest, StructureKept3D) {
    if (!HAS_3D) GTEST_SKIP() << "built without the 3D module";
    TitleArtDoc doc = fixtureDocument();
    Json* layer = findLayer(doc.root, "00000008");
    ASSERT_NE(layer, nullptr);
    (*layer)["style_3d"]["bands"][1]["grow"] = 7;
    (*layer)["style_3d"]["bands"][1]["depth"] = 12;
    (*layer)["style_3d"]["face"]["surface"] = Json{{"kind", "hammered"}, {"amount", 0.3}, {"scale", 10}, {"seed", 4}};
    normalizeTitleArt(doc);
    const Json before = *findLayer(doc.root, "00000008");
    applyColorScheme(doc, parsed(blueSchemeJson()), {});
    const Json& after = *findLayer(doc.root, "00000008");
    const Json &s0 = before["style_3d"], &s1 = after["style_3d"];
    for (const char* key : {"mode", "depth", "bevel", "bevel_position", "skew", "inflate", "curve_segments"}) EXPECT_EQ(canonical(s0[key]), canonical(s1[key])) << key;
    ASSERT_EQ(s1["bands"].size(), s0["bands"].size());
    for (size_t i = 0; i < s0["bands"].size(); i++) {
        Json a = s0["bands"][i], b = s1["bands"][i];
        a.erase("material");
        b.erase("material");
        a.erase("side");
        b.erase("side");
        EXPECT_EQ(canonical(a), canonical(b)) << "band " << i;
    }
    EXPECT_EQ(canonical(s0["face"]["surface"]), canonical(s1["face"]["surface"])) << "a surface is structure";
    // the band in the face's own material takes the bevel rim (silver), the dark band the dark paint
    EXPECT_EQ(s1["bands"][0]["material"]["preset"], "chrome");
    EXPECT_EQ(s1["bands"][1]["material"]["color"], "#001a5c");
    // a front-to-back ramp becomes the new colour fading to black
    const Json& depth = s1["bands"][2]["material"];
    EXPECT_EQ(depth["gradient_axis"], "depth");
    EXPECT_EQ(depth["gradient"]["stops"].back()["color"], "#000000");
    EXPECT_EQ(s1["glint"]["tint"], "#dff0ff");
    EXPECT_EQ(s1["face"]["gradient_span"], "line") << "faces span lines";
}

TEST_F(ColorSchemeApplyTest, FaceSpansLinesCharStays) {
    Json face{{"fill", {{"kind", "linear"}, {"gradient", goldRampGradient()}, {"angle", 30}, {"scale", 1.5}, {"span", "layer"}}}};
    Json charFace = face;
    charFace["fill"]["span"] = "char";
    TitleArtDoc doc = standardDocument({textLayer("00000001", Json::array(), face), textLayer("00000002", Json::array(), charFace)});
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const Json& a = doc.root["layers"][0]["style"]["face"]["fill"];
    EXPECT_EQ(a["span"], "line");
    EXPECT_EQ(a["angle"], 30) << "the gradient geometry stays";
    EXPECT_EQ(a["scale"], 1.5);
    EXPECT_EQ(a["gradient"]["stops"][2]["color"], "#ffd820") << "the scheme's face ramp";
    EXPECT_EQ(doc.root["layers"][1]["style"]["face"]["fill"]["span"], "char");
    EXPECT_EQ(doc.root["layers"][0]["style"]["face"]["inner_glow"]["color"], "#fff3b0") << "face_glow";
    // the gradient-map finish is enabled on a bevelled element, at 0.7 when it was off
    EXPECT_FALSE(doc.root["layers"][0]["style"]["face"]["finish"]["enabled"]) << "no bevel, no finish: unmapped";
}

TEST_F(ColorSchemeApplyTest, FinishFollowsBevel) {
    Json face{{"fill", standardSolid("#808080")}, {"bevel", {{"enabled", true}}}};
    TitleArtDoc doc = standardDocument({textLayer("00000001", Json::array(), face)});
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const Json& finish = doc.root["layers"][0]["style"]["face"]["finish"];
    EXPECT_TRUE(finish["enabled"]);
    EXPECT_EQ(finish["amount"], 0.7);
    EXPECT_EQ(finish["gradient"]["stops"][2]["color"], "#f08c00");
    // a paint without a ramp switches the finish off and keeps the colour unmapped
    applyColorScheme(doc, parsed(monoSchemeJson()), {});
    EXPECT_FALSE(doc.root["layers"][0]["style"]["face"]["finish"]["enabled"]);
}

TEST_F(ColorSchemeApplyTest, AlphaProfileKept) {
    TitleArtDoc doc = fixtureDocument();
    const Json before = doc.root["layers"][1]["style"]["rims"][0]["material"]["fill"];
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const Json& after = doc.root["layers"][1]["style"]["rims"][0]["material"]["fill"];
    EXPECT_EQ(after["kind"], "linear");
    const Json& stops = after["gradient"]["stops"];
    ASSERT_GE(stops.size(), 2u);
    EXPECT_EQ(stops.front()["alpha"], 1);
    EXPECT_EQ(stops.back()["alpha"], 0);
    for (const Json& s : stops) EXPECT_NEAR(s["alpha"].get<double>(), 1.0 - s["pos"].get<double>(), 1e-12) << canonical(stops);
    EXPECT_NE(stops.front()["color"], before["gradient"]["stops"][0]["color"]) << "the colours change";
}

TEST_F(ColorSchemeApplyTest, PatternsReliefOnlyStays) {
    TitleArtDoc doc = fixtureDocument();
    const Json relief = doc.root["layers"][1]["style"]["face"]["pattern"];
    ASSERT_EQ(relief["kind"], "asanoha");
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    EXPECT_EQ(canonical(doc.root["layers"][1]["style"]["face"]["pattern"]), canonical(relief)) << "a relief-only pattern is structure";
    // marks replace it; a colour pattern without marks is cleared
    applyColorScheme(doc, parsed(zebraSchemeJson()), {});
    EXPECT_EQ(doc.root["layers"][1]["style"]["face"]["pattern"]["kind"], "zebra");
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    EXPECT_EQ(doc.root["layers"][1]["style"]["face"]["pattern"]["kind"], "none");
}

TEST_F(ColorSchemeApplyTest, CharFacesRecolouredAsTheFace) {
    TitleArtDoc doc = fixtureDocument();
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const Json& chars = doc.root["layers"][0]["chars"];
    EXPECT_EQ(chars["0"]["face"]["fill"]["gradient"]["stops"][2]["color"], "#ffd820");
    EXPECT_EQ(chars["1"]["rotation"], 5);
    EXPECT_TRUE(chars["1"]["face"].is_null()) << "no face of its own: none is added";
}

TEST_F(ColorSchemeApplyTest, SourceFacesKeepTheirPixels) {
    TitleArtDoc doc = fixtureDocument();
    const Json image = doc.root["layers"][2]["style"]["face"], artwork = doc.root["layers"][3]["style"]["face"];
    ASSERT_EQ(image["fill"]["kind"], "source");
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    EXPECT_EQ(canonical(doc.root["layers"][2]["style"]["face"]), canonical(image));
    EXPECT_EQ(canonical(doc.root["layers"][3]["style"]["face"]), canonical(artwork));
    EXPECT_NE(rimMaterial(doc, 2, 0)["fill"]["color"], "#ffffff") << "their rims are recoloured";
}

TEST_F(ColorSchemeApplyTest, GuideAndAdjustmentLayersSkipped) {
    Json guide = textLayer("00000001", Json::array(), Json{{"fill", standardSolid("#808080")}});
    guide["guide"] = true;
    Json adjustment{{"id", "00000002"}, {"type", "adjustment"}};
    TitleArtDoc doc = standardDocument({guide, adjustment});
    const std::string before = writeTitleArt(doc);
    EXPECT_TRUE(applyColorScheme(doc, parsed(goldSchemeJson()), {}).empty());
    EXPECT_EQ(writeTitleArt(doc), before);
}

// ---- effects and generators (AC6) ----

TEST_F(ColorSchemeApplyTest, GlowsAndShadowsTakeTheirColours) {
    TitleArtDoc doc = fixtureDocument();
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const Json& layer = doc.root["layers"][0];
    EXPECT_EQ(layer["effects"][0]["params"]["color"], "#ffb000") << "outer glow: glow";
    EXPECT_EQ(layer["effects"][0]["params"]["gradient"]["stops"][1]["color"], "#ffd040") << "outer glow: glow_gradient";
    EXPECT_EQ(layer["effects"][1]["params"]["color"], "#3a1800") << "drop shadow: shadow, never black";
    EXPECT_EQ(layer["effects"][2]["params"]["color"], "#3a1800") << "inner shadow";
    const Json& parts = layer["part_effects"];
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts.begin().value()[0]["params"]["color"], "#ffb000") << "a glow on a rim";
    EXPECT_EQ(doc.root["layers"][4]["effects"][0]["params"]["color"], "#ffb000") << "a group's glow";
}

TEST_F(ColorSchemeApplyTest, WhiteCoresKept) {
    TitleArtDoc doc = standardDocument({generatorLayer("00000001", "light_streaks", Json{{"core_color", "#ffffff"}, {"edge_color", "#3d8bff"}}),
                                        generatorLayer("00000002", "light_streaks", Json{{"core_color", "#ffffff"}, {"edge_color", "#f0f0f0"}})});
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const Json& a = doc.root["layers"][0]["generator"]["params"];
    EXPECT_EQ(a["core_color"], "#ffffff") << "a white core stays";
    EXPECT_NE(a["edge_color"], "#3d8bff");
    // the edge is on the gold ramp by its luma: red over blue
    const std::string edge = a["edge_color"].get<std::string>();
    EXPECT_GT(std::stoi(edge.substr(1, 2), nullptr, 16), std::stoi(edge.substr(5, 2), nullptr, 16)) << edge;
    // light that is all white takes the scheme's glow
    const Json& b = doc.root["layers"][1]["generator"]["params"];
    EXPECT_NE(b["core_color"], "#ffffff") << canonical(b);
}

TEST_F(ColorSchemeApplyTest, ExceptionGeneratorsKeepTheirColours) {
    std::vector<Json> layers;
    int n = 0;
    for (const char* type : {"fire", "smoke", "ink_splatter", "lens_flare", "glints"}) {
        char id[9];
        std::snprintf(id, sizeof(id), "%08x", ++n);
        layers.push_back(generatorLayer(id, type));
    }
    TitleArtDoc doc = standardDocument(layers);
    const Json before = doc.root["layers"];
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    for (size_t i = 0; i < layers.size(); i++) {
        EXPECT_EQ(canonical(doc.root["layers"][i]["generator"]), canonical(before[i]["generator"])) << before[i]["generator"]["type"];
        EXPECT_EQ(doc.root["layers"][i]["look"]["color_scheme"], "gold");
    }
}

TEST_F(ColorSchemeApplyTest, PatternFillTakesTheMarks) {
    TitleArtDoc doc = standardDocument({generatorLayer("00000001", "pattern_fill", Json{{"kind", "checker"}})});
    applyColorScheme(doc, parsed(zebraSchemeJson()), {});
    const Json& p = doc.root["layers"][0]["generator"]["params"];
    EXPECT_EQ(p["kind"], "zebra");
    EXPECT_EQ(p["color"], "#060608");
    EXPECT_EQ(p["background"], "#f2f2f0");
}

TEST_F(ColorSchemeApplyTest, SpectrumOnRainbow) {
    TitleArtDoc doc = standardDocument({generatorLayer("00000001", "speed_lines", Json{{"color_mode", "solid"}}),
                                        generatorLayer("00000002", "particles", Json{{"color_mode", "solid"}}),
                                        generatorLayer("00000003", "aura"),
                                        textLayer("00000004", Json::array(), Json{{"fill", standardSolid("#808080")}})});
    applyColorScheme(doc, parsed(rainbowSchemeJson()), {});
    EXPECT_EQ(doc.root["layers"][0]["generator"]["params"]["color_mode"], "around") << "the SpectrumSwitch shows the spectrum";
    EXPECT_EQ(doc.root["layers"][1]["generator"]["params"]["color_mode"], "palette");
    // a gradient param with a white core keeps the core, then the spectrum
    const Json& lines = doc.root["layers"][0]["generator"]["params"]["gradient"]["stops"];
    EXPECT_EQ(lines[0]["color"], "#ffffff");
    EXPECT_EQ(lines[1]["color"], "#ff2a2a");
    EXPECT_NEAR(lines[1]["pos"].get<double>(), 0.15, 1e-12);
    EXPECT_EQ(lines.back()["color"], "#d22cff");
    // faces take the rainbow face
    EXPECT_EQ(doc.root["layers"][3]["style"]["face"]["fill"]["gradient"]["stops"][1]["color"], "#ff5a00");
}

TEST_F(ColorSchemeApplyTest, OrbTint) {
    if (!HAS_3D) GTEST_SKIP() << "built without the 3D module";
    TitleArtDoc doc = fixtureDocument();
    const Json before = *findLayer(doc.root, "00000009");
    applyColorScheme(doc, parsed(goldSchemeJson()), {});
    const Json& after = *findLayer(doc.root, "00000009");
    const Json &f0 = before["style_3d"]["face"], &f1 = after["style_3d"]["face"];
    EXPECT_EQ(f1["color"], "#ffb000") << "the body tint at 0.55";
    EXPECT_EQ(f1["gradient_kind"], "radial");
    EXPECT_EQ(f1["gradient"]["stops"][0]["color"], "#fff4a0");
    for (const char* key : {"preset", "roughness", "clearcoat", "metallic", "transmission"}) EXPECT_EQ(f0[key], f1[key]) << key << ": the material model stays";
    EXPECT_EQ(canonical(before["style_3d"]["bands"]), canonical(after["style_3d"]["bands"])) << "the bezel stays";
    // a spectrum sweeps diagonally
    applyColorScheme(doc, parsed(rainbowSchemeJson()), {});
    EXPECT_EQ((*findLayer(doc.root, "00000009"))["style_3d"]["face"]["gradient_kind"], "linear");
    EXPECT_EQ((*findLayer(doc.root, "00000009"))["style_3d"]["face"]["gradient_angle"], 60);
}

// ---- targets and paths ----

TEST_F(ColorSchemeApplyTest, ChangedPathsAndTargets) {
    TitleArtDoc doc = fixtureDocument();
    const std::vector<std::string> paths = applyColorScheme(doc, parsed(goldSchemeJson()), {"00000001"});
    EXPECT_TRUE(contains(paths, "00000001:style.rims[1].material.fill.kind")) << joined(paths);
    EXPECT_TRUE(contains(paths, "00000001:effects[0].params.color")) << joined(paths);
    EXPECT_TRUE(contains(paths, "00000001:part_effects.rim:00000102[0].params.color")) << joined(paths);
    EXPECT_TRUE(contains(paths, "00000001:chars.0.face.fill.kind")) << joined(paths);
    EXPECT_TRUE(contains(paths, "00000001:look.color_scheme")) << joined(paths);
    for (const std::string& p : paths) EXPECT_EQ(p.rfind("00000001:", 0), 0u) << p << ": a layer target changes that layer only";
    std::vector<std::string> unique = paths;
    std::sort(unique.begin(), unique.end());
    EXPECT_EQ(std::unique(unique.begin(), unique.end()), unique.end()) << "each path once";
    EXPECT_EQ(doc.root["layers"][1]["look"]["color_scheme"], "") << "other layers untouched";
    // a group target recolours itself and its members
    const std::vector<std::string> group = applyColorScheme(doc, parsed(goldSchemeJson()), {"00000005"});
    EXPECT_TRUE(contains(group, "00000005:effects[0].params.color")) << joined(group);
    EXPECT_TRUE(contains(group, "00000006:look.color_scheme")) << joined(group);
    EXPECT_TRUE(contains(group, "00000007:look.color_scheme")) << joined(group);
    // a target that names nothing changes nothing
    EXPECT_TRUE(applyColorScheme(doc, parsed(goldSchemeJson()), {"0badf00d"}).empty());
}

TEST_F(ColorSchemeApplyTest, OwnedGeneratorsFollowTheirLayer) {
    Json owned = generatorLayer("00000002", "speed_lines");
    owned["look"] = Json{{"owner", "00000001"}};
    Json other = generatorLayer("00000003", "speed_lines");
    other["follow"] = "00000001";
    Json fxGroup{{"id", "00000004"}, {"type", "group"}, {"look", {{"owner", "00000001"}}},
                 {"effects", Json::array({standardEffect("00000201", "outer_glow", Json{{"color", "#00ff00"}})})},
                 {"children", Json::array({textLayer("00000001", Json::array()), owned})}};
    TitleArtDoc doc = standardDocument({fxGroup, other});
    const std::vector<std::string> paths = applyColorScheme(doc, parsed(goldSchemeJson()), {"00000001"});
    EXPECT_TRUE(contains(paths, "00000002:look.color_scheme")) << joined(paths);
    EXPECT_TRUE(contains(paths, "00000004:effects[0].params.color")) << "the auto group's effects\n" << joined(paths);
    for (const std::string& p : paths) EXPECT_NE(p.rfind("00000003:", 0), 0u) << "a generator that only follows the layer is not its look's";
}

// ---- idempotence (R8, AC6) ----

TEST_F(ColorSchemeApplyTest, Idempotent) {
    const std::vector<ColorScheme> schemes = allSchemes();
    ASSERT_GE(schemes.size(), 5u);
    {
        // the control: the fixture itself is canonical
        TitleArtDoc doc = fixtureDocument();
        TitleArtDoc again = doc;
        normalizeTitleArt(again);
        ASSERT_TRUE(again.root == doc.root) << differencesOf(doc.root, again.root);
    }
    for (const ColorScheme& first : {schemes[0], schemes[3]}) {
        for (const ColorScheme& s : schemes) {
            TitleArtDoc doc = fixtureDocument();
            // from a document another scheme coloured, as from a fresh one
            if (first.id != s.id) applyColorScheme(doc, first, {});
            const std::vector<std::string> changed = applyColorScheme(doc, s, {});
            EXPECT_FALSE(changed.empty()) << s.id;
            const std::string once = writeTitleArt(doc);
            const Json onceRoot = doc.root;
            const std::vector<std::string> again = applyColorScheme(doc, s, {});
            EXPECT_TRUE(again.empty()) << first.id << " then " << s.id << " twice changed:\n" << joined(again);
            EXPECT_EQ(writeTitleArt(doc), once) << s.id;
            // what it writes is canonical: normalize changes nothing
            TitleArtDoc normalized = doc;
            normalizeTitleArt(normalized);
            EXPECT_TRUE(normalized.root == onceRoot) << s.id << ": the result is not normalized\n" << differencesOf(onceRoot, normalized.root);
        }
    }
}

// ---- embedded schemes (R12, AC23) ----

TEST_F(ColorSchemeApplyTest, EmbeddedSchemes) {
    const ColorScheme gold = parsed(goldSchemeJson()), rainbow = parsed(rainbowSchemeJson()), zebra = parsed(zebraSchemeJson());
    TitleArtDoc doc = fixtureDocument();
    doc.root["exposed"].push_back(Json{{"name", "color_scheme"}, {"type", "color_scheme"}, {"options", Json::array({"gold", "rainbow"})}});
    normalizeTitleArt(doc);
    const std::vector<std::string> paths = embedColorSchemes(doc, {gold, rainbow, zebra});
    EXPECT_EQ(paths, (std::vector<std::string>{"color_schemes.gold", "color_schemes.rainbow"}));
    ASSERT_TRUE(doc.root.contains("color_schemes"));
    EXPECT_EQ(doc.root["color_schemes"].size(), 2u) << "zebra is no option";
    EXPECT_EQ(canonical(doc.root["color_schemes"]["gold"]), canonical(writeColorScheme(gold)));
    // the member sits at its canonical place: a save and a read keep the document
    const std::string text = writeTitleArt(doc);
    const TitleArtParseResult read = parseTitleArt(text);
    ASSERT_TRUE(read.ok);
    EXPECT_TRUE(read.warnings.empty());
    EXPECT_EQ(writeTitleArt(read.doc), text);
    EXPECT_TRUE(read.doc.root == doc.root) << "embedding keeps the canonical member order";
    // a player switches from the embedded data alone: the same result as the library's scheme
    ColorScheme embedded;
    ASSERT_TRUE(embeddedColorScheme(doc, "rainbow", embedded));
    TitleArtDoc a = doc, b = doc;
    applyColorScheme(a, rainbow, {});
    applyColorScheme(b, embedded, {});
    EXPECT_EQ(writeTitleArt(a), writeTitleArt(b));
    EXPECT_FALSE(embeddedColorScheme(doc, "zebra", embedded));
    // applying an offered scheme refreshes its data; empty options offer every scheme
    doc.root["color_schemes"]["gold"]["glow"] = "#000001";
    const std::vector<std::string> applied = applyColorScheme(doc, gold, {});
    EXPECT_TRUE(contains(applied, "color_schemes.gold")) << joined(applied);
    EXPECT_EQ(canonical(doc.root["color_schemes"]["gold"]), canonical(writeColorScheme(gold)));
    doc.root["exposed"].back()["options"] = Json::array();
    EXPECT_EQ(embedColorSchemes(doc, {zebra}), std::vector<std::string>{"color_schemes.zebra"});
    // a document that exposes no scheme gets none
    TitleArtDoc plain = fixtureDocument();
    applyColorScheme(plain, gold, {});
    EXPECT_FALSE(plain.root.contains("color_schemes"));
}

// ---- time (R40: printed, not asserted) ----

TEST_F(ColorSchemeApplyTest, Time64Layers) {
    std::vector<Json> layers;
    for (int i = 0; i < 64; i++) {
        char id[9];
        std::snprintf(id, sizeof(id), "%08x", i + 1);
        Json l = standardTitleLayer(id);
        for (Json& rim : l["style"]["rims"]) rim["id"] = "";
        l["effects"] = Json::array({standardEffect("", "outer_glow"), standardEffect("", "drop_shadow")});
        layers.push_back(l);
    }
    const TitleArtDoc doc = standardDocument(layers);
    const ColorScheme gold = parsed(goldSchemeJson()), blue = parsed(blueSchemeJson());
    using Clock = std::chrono::steady_clock;
    double best = 1e9, bestAgain = 1e9;
    for (int run = 0; run < 3; run++) {
        TitleArtDoc copy = doc;
        const auto start = Clock::now();
        const std::vector<std::string> changed = applyColorScheme(copy, run % 2 ? blue : gold, {});
        const auto mid = Clock::now();
        applyColorScheme(copy, run % 2 ? blue : gold, {});
        const auto end = Clock::now();
        EXPECT_FALSE(changed.empty());
        best = std::min(best, std::chrono::duration<double, std::milli>(mid - start).count());
        bestAgain = std::min(bestAgain, std::chrono::duration<double, std::milli>(end - mid).count());
    }
    std::printf("[bench] applyColorScheme, 64 styled layers (4 rims, 2 effects each): %.3f ms (%.3f ms per layer); re-applied: %.3f ms\n",
                best, best / 64.0, bestAgain);
}
